package catalog

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	"github.com/vmihailenco/msgpack/v5"
)

func TestSubscriberSynchronizationSlotDoesNotLoseRequests(t *testing.T) {
	t.Parallel()
	owner, cancel := context.WithCancel(context.Background())
	defer cancel()

	t.Run("request queued before idle check stays with current worker", func(t *testing.T) {
		subscriber := &Subscriber{syncRunning: true}
		subscriber.pending.scope = true
		batch, ok := subscriber.takeSyncBatch()
		if !ok || !batch.scope || !subscriber.syncRunning {
			t.Fatalf("queued batch was not retained: ok=%v batch=%#v running=%v", ok, batch, subscriber.syncRunning)
		}
	})

	t.Run("request queued after idle check starts replacement worker", func(t *testing.T) {
		subscriber := &Subscriber{syncRunning: true}
		if batch, ok := subscriber.takeSyncBatch(); ok || batch.scope || subscriber.syncRunning {
			t.Fatalf("empty worker did not retire: ok=%v batch=%#v running=%v", ok, batch, subscriber.syncRunning)
		}
		subscriber.syncMu.Lock()
		subscriber.pending.scope = true
		start := subscriber.startSyncLocked(owner)
		subscriber.syncMu.Unlock()
		if !start || !subscriber.syncRunning || subscriber.workers.Load() != 1 {
			t.Fatalf("replacement worker was not reserved: start=%v running=%v workers=%d", start, subscriber.syncRunning, subscriber.workers.Load())
		}
	})
}

func TestEmbeddedScriptsMatchGeneratedSources(t *testing.T) {
	t.Parallel()
	scripts := map[string]string{
		"read":    readLua,
		"replace": replaceLua,
		"patch":   patchLua,
		"delete":  deleteLua,
	}
	for name, embedded := range scripts {
		name, embedded := name, embedded
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			path := filepath.Join("..", "..", "..", "lua", "catalog", name+".lua")
			generated, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if string(generated) != embedded {
				t.Fatalf("embedded script differs from %s", path)
			}
		})
	}
}

func TestPathAndSubscriptionNormalization(t *testing.T) {
	t.Parallel()

	path, err := NewPath("routing", "public-v1")
	if err != nil {
		t.Fatal(err)
	}
	if path.Part() != "routing" || path.ID() != "public-v1" {
		t.Fatalf("unexpected path: %#v", path)
	}
	if _, err := NewPath("bad:part", "id"); !verdandi.IsCode(err, verdandi.CodeInvalid) {
		t.Fatalf("expected invalid part, got %v", err)
	}

	other, err := NewPath("limits", "edge")
	if err != nil {
		t.Fatal(err)
	}
	normalized, err := normalizeSubscription("Prod", Subscription{
		Parts: []string{"routing", "routing"},
		Paths: []Path{path, other, other},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(normalized.patterns) != 1 ||
		normalized.patterns[0] != "verdandi:catalog:Prod:routing:*" {
		t.Fatalf("unexpected patterns: %#v", normalized.patterns)
	}
	if len(normalized.channels) != 1 ||
		normalized.channels[0] != "verdandi:catalog:Prod:limits:edge" {
		t.Fatalf("unexpected channels: %#v", normalized.channels)
	}
	if !normalized.covers(path) || !normalized.covers(other) {
		t.Fatal("normalized subscription lost coverage")
	}
	if normalized.checkpointScope() != normalized.checkpointScope() {
		t.Fatal("checkpoint scope is not deterministic")
	}

	zone, err := normalizeSubscription("Prod", Subscription{Zone: true, Paths: []Path{path}})
	if err != nil {
		t.Fatal(err)
	}
	if len(zone.channels) != 0 || len(zone.patterns) != 1 ||
		zone.patterns[0] != "verdandi:catalog:Prod:*" {
		t.Fatalf("unexpected zone subscription: %#v", zone)
	}
}

func TestValueShapeValidation(t *testing.T) {
	t.Parallel()

	if _, _, err := validateValue(Array, verdandi.Fields{}, 128); err != nil {
		t.Fatalf("empty array must be valid: %v", err)
	}
	if _, _, err := validateValue(Map, verdandi.Fields{}, 128); err != nil {
		t.Fatalf("empty map must be valid: %v", err)
	}
	if _, _, err := validateValue(Value, verdandi.Fields{"value": nil}, 128); err != nil {
		t.Fatalf("zero-byte value must be valid: %v", err)
	}
	if _, _, err := validateValue(Array, verdandi.Fields{"0": nil, "2": nil}, 128); !verdandi.IsCode(err, verdandi.CodeContract) {
		t.Fatalf("expected array-hole contract error, got %v", err)
	}
	if _, _, err := validateValue(Array, verdandi.Fields{"00": nil}, 128); !verdandi.IsCode(err, verdandi.CodeContract) {
		t.Fatalf("expected non-canonical array index error, got %v", err)
	}
	if _, _, err := validateValue(Array, verdandi.Fields{"999999999999999999999": nil}, 128); !verdandi.IsCode(err, verdandi.CodeContract) {
		t.Fatalf("expected out-of-range array index error, got %v", err)
	}
	if _, _, err := validateValue(Map, verdandi.Fields{"@meta": nil}, 128); !verdandi.IsCode(err, verdandi.CodeInvalid) {
		t.Fatalf("expected reserved-name error, got %v", err)
	}
	if _, _, err := validateValue(Value, verdandi.Fields{"value": bytes.Repeat([]byte{'x'}, 9)}, 8); !verdandi.IsCode(err, verdandi.CodeCapacity) {
		t.Fatalf("expected capacity error, got %v", err)
	}
}

func TestSubscriberViewBudgetTracksReplacementAndDelete(t *testing.T) {
	t.Parallel()
	path, err := NewPath("routing", "budget")
	if err != nil {
		t.Fatal(err)
	}
	subscriber := &Subscriber{
		client: &Client{config: runtimeConfig{maxViewBytes: 3}},
	}
	entry := newEntry(path, StatusSynchronizing)
	base := entry.state.Load()
	installed, err := subscriber.installState(entry, base, &rawState{
		revision: 1, status: StatusPresent, kind: Map, encodedBytes: 3,
	})
	if err != nil || !installed || subscriber.viewBytes != 3 {
		t.Fatalf("initial install = %v, %v, bytes=%d", installed, err, subscriber.viewBytes)
	}
	base = entry.state.Load()
	installed, err = subscriber.installState(entry, base, &rawState{
		revision: 2, status: StatusPresent, kind: Map, encodedBytes: 4,
	})
	if installed || !verdandi.IsCode(err, verdandi.CodeCapacity) || subscriber.viewBytes != 3 {
		t.Fatalf("oversized install = %v, %v, bytes=%d", installed, err, subscriber.viewBytes)
	}
	installed, err = subscriber.installState(entry, base, deletedState(3))
	if err != nil || !installed || subscriber.viewBytes != 0 {
		t.Fatalf("delete install = %v, %v, bytes=%d", installed, err, subscriber.viewBytes)
	}
}

func TestNotificationDecoding(t *testing.T) {
	t.Parallel()

	path, err := NewPath("routing", "public")
	if err != nil {
		t.Fatal(err)
	}
	replacePayload, err := msgpack.Marshal([]any{
		"v1", "replace", path.member(), "12", "map", "4",
		[]any{"a", []byte("one")},
	})
	if err != nil {
		t.Fatal(err)
	}
	replace, err := decodeEvent(string(replacePayload), path, 128)
	if err != nil {
		t.Fatal(err)
	}
	if replace.kind != eventReplace || replace.revision != 12 ||
		!bytes.Equal(replace.fields["a"], []byte("one")) {
		t.Fatalf("unexpected replace: %#v", replace)
	}
	if cap(replace.fields["a"]) != len(replace.fields["a"]) {
		t.Fatal("decoded field capacity must not expose adjacent immutable storage")
	}
	arrayPayload, err := msgpack.Marshal([]any{
		"v1", "replace", path.member(), "13", "array", "4",
		[]any{"0", []byte("x"), "1", []byte("y")},
	})
	if err != nil {
		t.Fatal(err)
	}
	array, err := decodeEvent(string(arrayPayload), path, 128)
	if err != nil || array.valueKind != Array || !bytes.Equal(array.fields["1"], []byte("y")) {
		t.Fatalf("unexpected Array replace: event=%#v error=%v", array, err)
	}
	nonCanonicalArray, err := msgpack.Marshal([]any{
		"v1", "replace", path.member(), "14", "array", "4",
		[]any{"1", []byte("y"), "0", []byte("x")},
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := decodeEvent(string(nonCanonicalArray), path, 128); err == nil {
		t.Fatal("Array notification fields must use numeric contiguous order")
	}

	patchPayload, err := msgpack.Marshal([]any{
		"v1", "patch", path.member(), "12", "15", "map", "4",
		[]any{"a", []byte("two")},
	})
	if err != nil {
		t.Fatal(err)
	}
	patch, err := decodeEvent(string(patchPayload), path, 128)
	if err != nil {
		t.Fatal(err)
	}
	if patch.kind != eventPatch || patch.baseRevision != 12 || patch.revision != 15 {
		t.Fatalf("unexpected patch: %#v", patch)
	}

	deletePayload, err := msgpack.Marshal([]any{"v1", "delete", path.member(), "16"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := decodeEvent(string(append(deletePayload, 0)), path, 128); err == nil {
		t.Fatal("trailing notification bytes must be rejected")
	}

	if _, err := decodeEvent(string([]byte{0xdd, 0xff, 0xff, 0xff, 0xff}), path, 128); err == nil {
		t.Fatal("declared outer allocation bomb must be rejected")
	}
	prefix, err := msgpack.Marshal([]any{
		"v1", "replace", path.member(), "1", "map", "0",
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(prefix) == 0 || prefix[0] != 0x96 {
		t.Fatalf("unexpected MessagePack prefix: %x", prefix)
	}
	prefix[0] = 0x97
	impossibleFields := append(prefix, 0xdd, 0xff, 0xff, 0xff, 0xff)
	if _, err := decodeEvent(string(impossibleFields), path, 128); err == nil {
		t.Fatal("declared field allocation bomb must be rejected")
	}

	nonCanonicalMap, err := msgpack.Marshal([]any{
		"v1", "replace", path.member(), "17", "map", "3",
		[]any{"9", []byte{}, "10", []byte{}},
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := decodeEvent(string(nonCanonicalMap), path, 128); err == nil {
		t.Fatal("Map notification fields must use lexical order")
	}
}

func TestReadReplyReconstruction(t *testing.T) {
	t.Parallel()

	base := &rawState{
		revision:        8,
		replaceRevision: 5,
		status:          StatusSynchronizing,
		kind:            Map,
		encodedBytes:    4,
		fields: verdandi.Fields{
			"a": []byte("one"),
		},
	}
	reply := []any{
		"&result", "ok",
		"&status", "present",
		"&mode", "patch",
		"@revision", "9",
		"@replace_revision", "5",
		"@kind", "map",
		"@encoded_bytes", "4",
		"&fields", []any{"a", []byte("two")},
	}
	state, err := parseReadReply(reply, base, 128)
	if err != nil {
		t.Fatal(err)
	}
	if state.status != StatusPresent || state.revision != 9 ||
		!bytes.Equal(state.fields["a"], []byte("two")) {
		t.Fatalf("unexpected reconstructed state: %#v", state)
	}

	emptyReply := []any{
		"&result", "ok",
		"&status", "present",
		"&mode", "replace",
		"@revision", "10",
		"@replace_revision", "10",
		"@kind", "array",
		"@encoded_bytes", "0",
		"&fields", []any{},
	}
	empty, err := parseReadReply(emptyReply, nil, 128)
	if err != nil {
		t.Fatal(err)
	}
	if empty.kind != Array || len(empty.fields) != 0 {
		t.Fatalf("unexpected empty array: %#v", empty)
	}
}

func TestLocalStoreSeparatesSubscriptionScopes(t *testing.T) {
	t.Parallel()

	store, err := openLocalStore(filepath.Join(t.TempDir(), "catalog.db"), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := store.close(); err != nil {
			t.Error(err)
		}
	})
	path, err := NewPath("routing", "public")
	if err != nil {
		t.Fatal(err)
	}
	state := &rawState{
		revision:        4,
		replaceRevision: 4,
		status:          StatusPresent,
		kind:            Value,
		encodedBytes:    6,
		fields:          verdandi.Fields{"value": []byte("x")},
	}
	if err := store.saveEntry("Prod", "scope-a", path, state); err != nil {
		t.Fatal(err)
	}
	if err := store.saveCursor("Prod", "scope-a", 4); err != nil {
		t.Fatal(err)
	}
	if err := store.saveCursor("Prod", "scope-b", 9); err != nil {
		t.Fatal(err)
	}

	cursor, entries, err := store.load("Prod", "scope-a", 128)
	if err != nil {
		t.Fatal(err)
	}
	if cursor != 4 || entries[path].revision != 4 {
		t.Fatalf("unexpected scope-a checkpoint: cursor=%d entries=%#v", cursor, entries)
	}
	cursor, entries, err = store.load("Prod", "scope-b", 128)
	if err != nil {
		t.Fatal(err)
	}
	if cursor != 9 || len(entries) != 0 {
		t.Fatalf("scope-b leaked entries: cursor=%d entries=%#v", cursor, entries)
	}
}

func TestLocalStoreRejectsEntryAndCursorRegression(t *testing.T) {
	t.Parallel()
	store, err := openLocalStore(filepath.Join(t.TempDir(), "catalog.db"), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := store.close(); err != nil {
			t.Error(err)
		}
	})
	path, err := NewPath("routing", "monotonic")
	if err != nil {
		t.Fatal(err)
	}
	newer := &rawState{
		revision:        9,
		replaceRevision: 7,
		status:          StatusPresent,
		kind:            Map,
		encodedBytes:    5,
		fields:          verdandi.Fields{"x": []byte("data")},
	}
	older := *newer
	older.revision = 8
	older.fields = cloneFields(newer.fields)
	if err := store.saveEntry("Prod", "scope", path, newer); err != nil {
		t.Fatal(err)
	}
	if err := store.saveEntry("Prod", "scope", path, &older); err != nil {
		t.Fatal(err)
	}
	if err := store.saveCursor("Prod", "scope", 9); err != nil {
		t.Fatal(err)
	}
	if err := store.saveCursor("Prod", "scope", 8); err != nil {
		t.Fatal(err)
	}
	cursor, entries, err := store.load("Prod", "scope", 64)
	if err != nil {
		t.Fatal(err)
	}
	if cursor != 9 || entries[path] == nil || entries[path].revision != 9 {
		t.Fatalf("checkpoint regressed: cursor=%d entries=%#v", cursor, entries)
	}
}
