package registration

import (
	"bytes"
	"slices"
	"testing"
	"time"
)

func TestSelectorNaturalExpiryRetainsForOneAdditionalTTL(t *testing.T) {
	t.Parallel()
	selector := retainedTestSelector(1 << 20)
	state := newSelectorState(1)
	record := retainedTestRecord("11111111111111111111111111111111", 1_000, 100)
	selector.setRecord(&state, record)

	if changed := selector.expire(&state, 1_100); changed != 1 {
		t.Fatalf("expire() changed = %d, want 1", changed)
	}
	if len(state.records) != 0 || len(state.retained) != 1 {
		t.Fatalf("expiry state active=%d retained=%d", len(state.records), len(state.retained))
	}
	retained := state.retained[record.meta.UUID]
	if retained.until != 1_200 {
		t.Fatalf("retained until = %d, want 1200", retained.until)
	}

	selector.publish(state, 1, true)
	if _, found, err := selector.Find(record.meta.UUID); err != nil || found {
		t.Fatal("retained record remained selectable")
	}
	detached, found, err := selector.FindRetained(record.meta.UUID)
	if err != nil || !found || detached.RetainedUntil != 1_200 {
		t.Fatalf("FindRetained() = %#v, %v, %v", detached, found, err)
	}
	detached.Record.Data["load"][0] = '9'
	if bytes.Equal(selector.view.Load().retained[record.meta.UUID].record.data["load"], detached.Record.Data["load"]) {
		t.Fatal("FindRetained returned aliased Data")
	}

	if changed := selector.expire(&state, 1_199); changed != 0 {
		t.Fatalf("early retained expiry changed = %d", changed)
	}
	if changed := selector.expire(&state, 1_200); changed != 1 || len(state.retained) != 0 {
		t.Fatalf("final retained expiry changed=%d retained=%d", changed, len(state.retained))
	}
}

func TestSelectorHalfSynchronizedRawViewIsUnavailable(t *testing.T) {
	t.Parallel()
	selector := retainedTestSelector(1 << 20)
	selector.view.Store(emptySelectorView(7, false))
	if _, err := selector.Snapshot(); !IsCode(err, CodeUnavailable) {
		t.Fatalf("Snapshot error = %v, want unavailable", err)
	}
	if _, _, err := selector.Find("missing"); !IsCode(err, CodeUnavailable) {
		t.Fatalf("Find error = %v, want unavailable", err)
	}
	if _, _, err := selector.FindRetained("missing"); !IsCode(err, CodeUnavailable) {
		t.Fatalf("FindRetained error = %v, want unavailable", err)
	}
}

func TestSelectorPublishedSnapshotKeepsUUIDOrder(t *testing.T) {
	t.Parallel()
	selector := retainedTestSelector(1 << 20)
	state := newSelectorState(5)
	for _, uuid := range []string{
		"33333333333333333333333333333333",
		"11111111111111111111111111111111",
		"22222222222222222222222222222222",
	} {
		selector.setRecord(&state, retainedTestRecord(uuid, 1_000, 100))
	}
	for _, uuid := range []string{
		"55555555555555555555555555555555",
		"44444444444444444444444444444444",
	} {
		record := retainedTestRecord(uuid, 1_000, 100)
		state.retained[uuid] = retainedSelectorRecord{record: record, until: 1_200}
	}
	selector.publish(state, 1, true)
	snapshot, err := selector.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	active := make([]string, len(snapshot.Records))
	for index := range snapshot.Records {
		active[index] = snapshot.Records[index].Meta.UUID
	}
	retained := make([]string, len(snapshot.Retained))
	for index := range snapshot.Retained {
		retained[index] = snapshot.Retained[index].Record.Meta.UUID
	}
	if !slices.IsSorted(active) || !slices.IsSorted(retained) {
		t.Fatalf("snapshot order active=%v retained=%v", active, retained)
	}
}

func TestSelectorRenewReactivatesRetainedContent(t *testing.T) {
	t.Parallel()
	selector := retainedTestSelector(1 << 20)
	state := newSelectorState(1)
	record := retainedTestRecord("22222222222222222222222222222222", 1_000, 100)
	selector.setRecord(&state, record)
	selector.expire(&state, 1_100)

	changed, repair, err := selector.applyEvent(&state, registrationEvent{
		kind:      "renew",
		uuid:      record.meta.UUID,
		revision:  1,
		timestamp: 1_150,
	}, protocolZoneConfig(), redisClock{anchor: time.Now(), upper: 1_100})
	if err != nil || !changed || repair {
		t.Fatalf("renew = changed %v repair %v err %v", changed, repair, err)
	}
	if state.records[record.meta.UUID] == nil || len(state.retained) != 0 {
		t.Fatalf("renew did not reactivate record: %#v", state)
	}
	if state.records[record.meta.UUID].deadline != 1_250 {
		t.Fatalf("reactivated deadline = %d, want 1250", state.records[record.meta.UUID].deadline)
	}
}

func TestSelectorUpdatePreservesPublishedVersion(t *testing.T) {
	t.Parallel()
	selector := retainedTestSelector(1 << 20)
	state := newSelectorState(1)
	record := retainedTestRecord("77777777777777777777777777777777", 1_000, 100)
	selector.setRecord(&state, record)
	selector.publish(state, 1, true)
	previous := selector.view.Load()

	changed, repair, err := selector.applyEvent(&state, registrationEvent{
		kind: "update", uuid: record.meta.UUID, revision: 2, timestamp: 1_001,
		data: Fields{"load": []byte("1")},
	}, protocolZoneConfig(), redisClock{anchor: time.Now(), upper: 1_000})
	if err != nil || !changed || repair {
		t.Fatalf("update = changed %v repair %v err %v", changed, repair, err)
	}
	if got := string(previous.records[record.meta.UUID].data["load"]); got != "0" {
		t.Fatalf("published value changed in place: %q", got)
	}
	if got := string(state.records[record.meta.UUID].data["load"]); got != "1" {
		t.Fatalf("next value = %q, want 1", got)
	}
}

func TestSelectorUpdateMaintainsCachedRecordSizeAndCapacity(t *testing.T) {
	t.Parallel()
	selector := retainedTestSelector(1 << 20)
	state := newSelectorState(1)
	record := retainedTestRecord("88888888888888888888888888888888", 1_000, 100)
	record.meta.Revision = 9
	record.meta.Version = 9
	selector.setRecord(&state, record)

	changed, repair, err := selector.applyEvent(&state, registrationEvent{
		kind: "update", uuid: record.meta.UUID, revision: 10, timestamp: 1_001,
		version: 10, hasVersion: true, data: Fields{"load": []byte("wide")},
	}, protocolZoneConfig(), redisClock{anchor: time.Now(), upper: 1_000})
	if err != nil || !changed || repair {
		t.Fatalf("update = changed %v repair %v err %v", changed, repair, err)
	}
	next := state.records[record.meta.UUID]
	want := registrationSize(next.meta.UUID, next.meta.Revision, next.meta.TTL, next.meta.Version, next.attr, next.data)
	if next.size != want || state.bytes != want {
		t.Fatalf("cached size=%d state bytes=%d want=%d", next.size, state.bytes, want)
	}

	limits := protocolZoneConfig()
	large := retainedTestRecord("99999999999999999999999999999999", 2_000, 100)
	large.data = Fields{
		"a": make([]byte, protocolFieldValueBytes),
		"b": make([]byte, protocolFieldValueBytes),
		"c": make([]byte, protocolFieldValueBytes),
		"d": []byte("small"),
	}
	if err := validateRecord(large.meta.UUID, large.meta.Revision, large.meta.TTL, large.meta.Version, large.attr, large.data, limits); err != nil {
		t.Fatalf("initial large record: %v", err)
	}
	largeState := newSelectorState(1)
	selector.setRecord(&largeState, large)
	_, _, err = selector.applyEvent(&largeState, registrationEvent{
		kind: "update", uuid: large.meta.UUID, revision: 2, timestamp: 2_001,
		data: Fields{"d": make([]byte, protocolFieldValueBytes)},
	}, limits, redisClock{anchor: time.Now(), upper: 2_000})
	if !IsCode(err, CodeCapacity) || largeState.records[large.meta.UUID].meta.Revision != 1 {
		t.Fatalf("oversized patch err=%v state=%#v", err, largeState.records[large.meta.UUID])
	}
}

func TestSelectorExplicitUnregisterPurgesRetainedContent(t *testing.T) {
	t.Parallel()
	selector := retainedTestSelector(1 << 20)
	state := newSelectorState(1)
	record := retainedTestRecord("33333333333333333333333333333333", 1_000, 100)
	selector.setRecord(&state, record)
	selector.expire(&state, 1_100)

	changed, repair, err := selector.applyEvent(&state, registrationEvent{
		kind: "unregister",
		uuid: record.meta.UUID,
	}, protocolZoneConfig(), redisClock{anchor: time.Now(), upper: 1_100})
	if err != nil || !changed || repair {
		t.Fatalf("unregister = changed %v repair %v err %v", changed, repair, err)
	}
	if len(state.records) != 0 || len(state.retained) != 0 || state.retainedBytes != 0 {
		t.Fatalf("unregister left state: %#v", state)
	}
}

func TestSelectorRetainedBudgetEvictsEarliestDeadline(t *testing.T) {
	t.Parallel()
	first := retainedTestRecord("44444444444444444444444444444444", 1_000, 100)
	second := retainedTestRecord("55555555555555555555555555555555", 1_100, 100)
	selector := retainedTestSelector(selectorRecordSize(first))
	state := newSelectorState(2)

	selector.setRetained(&state, second, 1_300, 1_000)
	selector.setRetained(&state, first, 1_200, 1_000)
	if _, exists := state.retained[first.meta.UUID]; exists {
		t.Fatal("earliest retained deadline was not evicted")
	}
	if _, exists := state.retained[second.meta.UUID]; !exists {
		t.Fatal("later retained deadline was unexpectedly evicted")
	}
	if state.retainedBytes > selector.client.config.selectorRetainedBytes {
		t.Fatalf("retained bytes = %d, limit %d", state.retainedBytes, selector.client.config.selectorRetainedBytes)
	}
}

func TestSelectorRetainedBudgetCanBeDisabled(t *testing.T) {
	t.Parallel()
	selector := retainedTestSelector(0)
	state := newSelectorState(1)
	record := retainedTestRecord("66666666666666666666666666666666", 1_000, 100)
	selector.setRecord(&state, record)
	selector.expire(&state, 1_100)
	if len(state.records) != 0 || len(state.retained) != 0 {
		t.Fatalf("disabled retention kept state: %#v", state)
	}
}

func retainedTestSelector(limit int) *selectorCore {
	return &selectorCore{client: newTestRuntime(runtimeConfig{
		selectorMaxBytes:      1 << 20,
		selectorRetainedBytes: limit,
	}, protocolZoneConfig())}
}

func retainedTestRecord(uuid string, timestamp uint64, ttl uint64) *selectorRecord {
	return &selectorRecord{
		meta: Meta{
			UUID:      uuid,
			Revision:  1,
			Timestamp: timestamp,
			TTL:       ttl,
			Version:   1,
		},
		attr:     Fields{"role": []byte("worker")},
		data:     Fields{"load": []byte("0")},
		deadline: timestamp + ttl,
	}
}
