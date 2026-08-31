package registration

import (
	"bytes"
	"testing"
	"time"
)

const pendingTestUUID = "0123456789abcdef0123456789abcdef"

func TestPendingChangesCoalesceOneLogicalChangePerUUID(t *testing.T) {
	t.Parallel()
	pending := newPendingChanges(8, 1<<20)
	register := registrationEvent{
		kind:       "register",
		uuid:       pendingTestUUID,
		revision:   1,
		timestamp:  10,
		ttl:        100,
		version:    1,
		hasVersion: true,
		attr:       Fields{"role": []byte("edge")},
		data:       Fields{"load": []byte("1"), "state": []byte("ready")},
	}
	updates := []registrationEvent{
		{kind: "update", uuid: pendingTestUUID, revision: 2, timestamp: 20, data: Fields{"load": []byte("2")}},
		{kind: "update", uuid: pendingTestUUID, revision: 3, timestamp: 30, version: 2, hasVersion: true, data: Fields{"state": []byte("busy")}},
		{kind: "renew", uuid: pendingTestUUID, revision: 3, timestamp: 40},
	}
	if err := pending.add(register); err != nil {
		t.Fatal(err)
	}
	for _, event := range updates {
		if err := pending.add(event); err != nil {
			t.Fatal(err)
		}
	}
	if len(pending.entries) != 1 {
		t.Fatalf("pending entries = %d, want 1", len(pending.entries))
	}
	changes := pending.drain()
	if len(changes) != 1 {
		t.Fatalf("drain length = %d, want 1", len(changes))
	}
	change := changes[0]
	if change.repair || change.event.kind != "register" || change.event.revision != 3 ||
		change.event.timestamp != 40 || change.event.version != 2 ||
		!bytes.Equal(change.event.data["load"], []byte("2")) ||
		!bytes.Equal(change.event.data["state"], []byte("busy")) {
		t.Fatalf("unexpected coalesced register: %#v", change)
	}
	if len(pending.entries) != 0 || pending.bytes != 0 {
		t.Fatalf("drain did not reset pending state: entries=%d bytes=%d", len(pending.entries), pending.bytes)
	}
}

func TestPendingChangesMergeContiguousUpdatesAndDetectGaps(t *testing.T) {
	t.Parallel()
	pending := newPendingChanges(8, 1<<20)
	for _, event := range []registrationEvent{
		{kind: "update", uuid: pendingTestUUID, revision: 5, timestamp: 50, data: Fields{"a": []byte("first")}},
		{kind: "update", uuid: pendingTestUUID, revision: 6, timestamp: 60, data: Fields{"b": []byte("second")}},
		{kind: "update", uuid: pendingTestUUID, revision: 7, timestamp: 70, data: Fields{"a": []byte("last")}},
	} {
		if err := pending.add(event); err != nil {
			t.Fatal(err)
		}
	}
	change := pending.drain()[0]
	if change.repair || change.baseRevision != 4 || change.latestRevision != 7 ||
		!bytes.Equal(change.event.data["a"], []byte("last")) ||
		!bytes.Equal(change.event.data["b"], []byte("second")) {
		t.Fatalf("unexpected contiguous update: %#v", change)
	}

	if err := pending.add(registrationEvent{kind: "update", uuid: pendingTestUUID, revision: 8, timestamp: 80, data: Fields{"a": []byte("8")}}); err != nil {
		t.Fatal(err)
	}
	if err := pending.add(registrationEvent{kind: "update", uuid: pendingTestUUID, revision: 10, timestamp: 100, data: Fields{"a": []byte("10")}}); err != nil {
		t.Fatal(err)
	}
	if err := pending.add(registrationEvent{
		kind: "register", uuid: pendingTestUUID, revision: 9, timestamp: 90, ttl: 100, version: 1, hasVersion: true, data: Fields{"a": []byte("9")},
	}); err != nil {
		t.Fatal(err)
	}
	if change = pending.entries[pendingTestUUID]; !change.repair || change.latestRevision != 10 {
		t.Fatalf("lower full state incorrectly cleared repair: %#v", change)
	}
	if err := pending.add(registrationEvent{
		kind: "register", uuid: pendingTestUUID, revision: 10, timestamp: 110, ttl: 100, version: 1, hasVersion: true, data: Fields{"a": []byte("10")},
	}); err != nil {
		t.Fatal(err)
	}
	if change = pending.drain()[0]; change.repair || change.event.kind != "register" || change.event.revision != 10 {
		t.Fatalf("authoritative register did not clear repair: %#v", change)
	}
}

func TestPendingChangesEnforceTransactionalEntryAndByteBounds(t *testing.T) {
	t.Parallel()
	one := "0123456789abcdef0123456789abcdef"
	two := "fedcba9876543210fedcba9876543210"
	pending := newPendingChanges(1, 1<<20)
	if err := pending.add(registrationEvent{kind: "renew", uuid: one, revision: 1, timestamp: 1}); err != nil {
		t.Fatal(err)
	}
	if err := pending.add(registrationEvent{kind: "renew", uuid: two, revision: 1, timestamp: 1}); !IsCode(err, CodeCapacity) {
		t.Fatalf("entry overflow error = %v, want capacity", err)
	}

	original := pending.entries[one]
	pending.maxBytes = pending.bytes + 1
	err := pending.add(registrationEvent{kind: "update", uuid: one, revision: 2, timestamp: 2, data: Fields{"payload": bytes.Repeat([]byte{'x'}, 128)}})
	if !IsCode(err, CodeCapacity) {
		t.Fatalf("byte overflow error = %v, want capacity", err)
	}
	retained := pending.entries[one]
	if retained.event.kind != original.event.kind || retained.latestRevision != original.latestRevision || len(retained.event.data) != 0 {
		t.Fatalf("failed add mutated retained state: %#v", retained)
	}
}

func TestPendingChangesTrackBytesAcrossInPlaceMerges(t *testing.T) {
	t.Parallel()
	pending := newPendingChanges(8, 1<<20)
	events := []registrationEvent{
		{kind: "update", uuid: pendingTestUUID, revision: 2, timestamp: 20, data: Fields{"a": []byte("1")}},
		{kind: "update", uuid: pendingTestUUID, revision: 3, timestamp: 30, data: Fields{"a": bytes.Repeat([]byte{'x'}, 128)}},
		{kind: "update", uuid: pendingTestUUID, revision: 4, timestamp: 40, data: Fields{"b": []byte("2")}},
		{kind: "update", uuid: pendingTestUUID, revision: 5, timestamp: 50, data: Fields{"a": []byte("3")}},
		{kind: "renew", uuid: pendingTestUUID, revision: 5, timestamp: 60},
	}
	for index, event := range events {
		if err := pending.add(event); err != nil {
			t.Fatalf("add %d: %v", index, err)
		}
		accounted := 0
		for _, change := range pending.entries {
			accounted += pendingChangeSize(change)
		}
		if pending.bytes != accounted {
			t.Fatalf("after add %d bytes = %d, want %d", index, pending.bytes, accounted)
		}
	}
}

func TestPendingChangesBoundLargeSingleRegistrationBurst(t *testing.T) {
	t.Parallel()
	pending := newPendingChanges(1, 512)
	for revision := uint64(2); revision <= 10_001; revision++ {
		event := registrationEvent{
			kind: "update", uuid: pendingTestUUID, revision: revision,
			timestamp: revision, data: Fields{"load": []byte{byte(revision)}},
		}
		if err := pending.add(event); err != nil {
			t.Fatalf("revision %d: %v", revision, err)
		}
		if len(pending.entries) != 1 || pending.bytes > 512 {
			t.Fatalf("revision %d expanded pending state: entries=%d bytes=%d", revision, len(pending.entries), pending.bytes)
		}
	}
	change := pending.drain()[0]
	if change.baseRevision != 1 || change.latestRevision != 10_001 || change.event.revision != 10_001 {
		t.Fatalf("unexpected burst result: %#v", change)
	}
}

func TestPendingChangesTreatUnregisterAsTerminal(t *testing.T) {
	t.Parallel()
	pending := newPendingChanges(2, 1<<20)
	if err := pending.add(registrationEvent{kind: "update", uuid: pendingTestUUID, revision: 2, timestamp: 2, data: Fields{"load": []byte("2")}}); err != nil {
		t.Fatal(err)
	}
	if err := pending.add(registrationEvent{kind: "unregister", uuid: pendingTestUUID}); err != nil {
		t.Fatal(err)
	}
	if change := pending.entries[pendingTestUUID]; change.event.kind != "unregister" || change.repair {
		t.Fatalf("unregister did not replace pending state: %#v", change)
	}
	if err := pending.add(registrationEvent{kind: "renew", uuid: pendingTestUUID, revision: 2, timestamp: 3}); !IsCode(err, CodeTransition) {
		t.Fatalf("post-unregister event error = %v, want transition", err)
	}
}

func TestSelectorAppliesCoalescedUpdateAndMaintainsCapacity(t *testing.T) {
	t.Parallel()
	selector := &selectorCore{client: newTestRuntime(runtimeConfig{selectorMaxBytes: 1 << 20}, protocolZoneConfig())}
	base := &selectorRecord{
		meta:     Meta{UUID: pendingTestUUID, Revision: 4, Timestamp: 40, TTL: 1000, Version: 1},
		attr:     Fields{"role": []byte("edge")},
		data:     Fields{"a": []byte("old"), "b": []byte("old")},
		deadline: 1040,
	}
	state := selectorState{records: make(map[string]*selectorRecord), deadlines: newDeadlineQueue(0)}
	selector.setRecord(&state, base)
	change := pendingChange{
		event: registrationEvent{
			kind: "update", uuid: pendingTestUUID, revision: 6, timestamp: 60,
			version: 2, hasVersion: true, data: Fields{"a": []byte("new"), "b": []byte("new")},
		},
		baseRevision: 4, latestRevision: 6,
	}
	clock := redisClock{anchor: time.Now(), upper: 1}
	changed, repair, err := selector.applyPendingChange(&state, change, protocolZoneConfig(), clock)
	if err != nil || !changed || repair {
		t.Fatalf("applyPendingChange() = changed %v repair %v err %v", changed, repair, err)
	}
	if record := state.records[pendingTestUUID]; record.meta.Revision != 6 || record.meta.Version != 2 || !bytes.Equal(record.data["a"], []byte("new")) {
		t.Fatalf("unexpected updated record: %#v", record)
	}
	wantBytes := selectorRecordSize(state.records[pendingTestUUID])
	if state.bytes != wantBytes {
		t.Fatalf("state bytes = %d, want %d", state.bytes, wantBytes)
	}
	selector.removeRecord(&state, pendingTestUUID)
	if state.bytes != 0 || len(state.records) != 0 {
		t.Fatalf("remove left bytes=%d records=%d", state.bytes, len(state.records))
	}
}
