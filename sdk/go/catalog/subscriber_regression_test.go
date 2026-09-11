package catalog

import (
	"context"
	"testing"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

func TestSubscriberPatchAddsFirstMapField(t *testing.T) {
	for _, fields := range []verdandi.Fields{nil, {}} {
		subscriber := &Subscriber{
			client:  &Client{config: runtimeConfig{maxRecordBytes: 1024}},
			entries: make(map[Path]*Entry), errors: make(chan error, 1),
		}
		path := Path{part: "part", id: "first"}
		entry := subscriber.getOrCreate(path, StatusSynchronizing)
		base := &rawState{revision: 1, replaceRevision: 1, status: StatusPresent, kind: Map, fields: fields}
		entry.state.Store(base)
		subscriber.applyEvent(context.Background(), catalogEvent{
			path: path, kind: eventPatch, valueKind: Map, revision: 2, baseRevision: 1,
			encodedBytes: 2, fields: verdandi.Fields{"a": []byte("x")},
		})
		current := entry.state.Load()
		if current.revision != 2 || string(current.fields["a"]) != "x" || len(base.fields) != 0 {
			t.Fatalf("first Map Patch must publish without mutating the base: %#v", current)
		}
	}
}

func TestFindInsertionAfterFinalCloseScanRemainsClosed(t *testing.T) {
	subscriber := &Subscriber{entries: make(map[Path]*Entry)}
	// Find 已取到旧 scope 状态，但在创建 Entry 前被最终关闭扫描赶过。
	initial := StatusAbsent
	subscriber.markScope(StatusClosed)
	path := Path{part: "part", id: "late"}
	entry := subscriber.getOrCreate(path, initial)
	if entry.state.Load().status != StatusClosed {
		t.Fatal("late entry escaped the final close scan")
	}
	if subscriber.getOrCreate(path, StatusSynchronizing) != entry {
		t.Fatal("entry identity changed")
	}
}
