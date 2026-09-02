package registration

import (
	"context"
	"fmt"
	"testing"
	"time"

	"github.com/vmihailenco/msgpack/v5"
)

func BenchmarkDecodeRegistrationEvent(b *testing.B) {
	payload, err := msgpack.Marshal([]any{
		"&protocol", "v1", "&kind", "register",
		"@uuid", pendingTestUUID, "@revision", uint64(1),
		"@timestamp", uint64(1787466000000), "@ttl", uint64(30_000),
		"@version", uint64(1), ".role", []byte("worker"),
		"address", []byte("10.0.0.1:8080"), "load", []byte("0"),
	})
	if err != nil {
		b.Fatal(err)
	}
	limits := protocolZoneConfig()
	b.ReportAllocs()
	b.SetBytes(int64(len(payload)))
	for b.Loop() {
		if _, err := decodeRegistrationEvent(payload, limits); err != nil {
			b.Fatal(err)
		}
	}
}

func BenchmarkRegistrationConstruction(b *testing.B) {
	client := &Client{}
	options := RegistrationOptions{Type: "benchmark", TTL: 3 * time.Second, Version: 1}
	b.ReportAllocs()
	for b.Loop() {
		registration, err := client.Registration[fields, fields](options)
		if err != nil {
			b.Fatal(err)
		}
		if len(registration.UUID()) != 32 {
			b.Fatal("invalid Registration UUID")
		}
	}
}

func BenchmarkPendingCoalesceThirtyTwoUpdates(b *testing.B) {
	events := make([]registrationEvent, 32)
	for index := range events {
		events[index] = registrationEvent{
			kind: "update", uuid: pendingTestUUID,
			revision: uint64(index + 2), timestamp: uint64(1787466000000 + index),
			data: Fields{"load": []byte(fmt.Sprintf("%03d", index))},
		}
	}
	b.ReportAllocs()
	for b.Loop() {
		pending := newPendingChanges(4096, 64*1024*1024)
		for _, event := range events {
			if err := pending.add(event); err != nil {
				b.Fatal(err)
			}
		}
		if changes := pending.drain(); len(changes) != 1 || changes[0].event.revision != 33 {
			b.Fatal("coalescing did not retain one latest change")
		}
	}
}

func BenchmarkPendingDrainSingleUpdate(b *testing.B) {
	pending := newPendingChanges(4096, 64*1024*1024)
	event := registrationEvent{
		kind: "update", uuid: pendingTestUUID, revision: 2,
		timestamp: 1787466000000, data: Fields{"load": []byte("001")},
	}
	b.ReportAllocs()
	for b.Loop() {
		if err := pending.add(event); err != nil {
			b.Fatal(err)
		}
		changes := pending.drain()
		if len(changes) != 1 || changes[0].event.revision != event.revision {
			b.Fatal("drain did not retain the update")
		}
		event.revision++
	}
}

func BenchmarkValidateDefaultMaximumRecord(b *testing.B) {
	attr := make(Fields, 16)
	data := make(Fields, 32)
	for index := range 16 {
		attr[fmt.Sprintf("attr%02d", index)] = make([]byte, 128)
	}
	for index := range 32 {
		data[fmt.Sprintf("data%02d", index)] = make([]byte, 128)
	}
	limits := defaultZoneConfig()
	b.ReportAllocs()
	for b.Loop() {
		if err := validateRecord(pendingTestUUID, 1, 30_000, 1, attr, data, limits); err != nil {
			b.Fatal(err)
		}
	}
}

func BenchmarkPublishSelectorView500(b *testing.B) {
	state := selectorState{
		records:   make(map[string]*selectorRecord, 500),
		deadlines: newDeadlineQueue(500),
	}
	for index := range 500 {
		uuid := fmt.Sprintf("%032x", index+1)
		record := &selectorRecord{
			meta:     Meta{UUID: uuid, Revision: 1, Timestamp: 1787466000000, TTL: 30_000, Version: 1},
			attr:     Fields{"role": []byte("worker")},
			data:     Fields{"load": []byte("0")},
			deadline: 1787466030000,
		}
		state.records[uuid] = record
		state.deadlines.set(uuid, record.deadline)
		state.bytes += selectorRecordSize(record)
	}
	selector := &selectorCore{}
	selector.view.Store(&selectorView{records: make(map[string]*selectorRecord)})
	b.ReportAllocs()
	for b.Loop() {
		selector.publish(state, 1, true)
	}
	if snapshot, err := selector.Snapshot(); err != nil || len(snapshot.Records) != 500 {
		b.Fatalf("snapshot records = %d", len(snapshot.Records))
	}
}

func BenchmarkRedisClockUpperNow(b *testing.B) {
	clock := redisClock{anchor: time.Now(), upper: 1787466000000}
	b.ReportAllocs()
	for b.Loop() {
		if clock.upperNow() == 0 {
			b.Fatal("zero RedisClock value")
		}
	}
}

func BenchmarkApplyRegistrationUpdate(b *testing.B) {
	selector, state, clock := benchmarkSelectorState()
	event := registrationEvent{
		kind: "update", uuid: pendingTestUUID,
		revision: 2, timestamp: 1787466000001,
		data: Fields{"data00": make([]byte, 128)},
	}
	b.ReportAllocs()
	for b.Loop() {
		current := state.records[pendingTestUUID]
		event.revision = current.meta.Revision + 1
		event.timestamp = current.meta.Timestamp + 1
		changed, repair, err := selector.applyEvent(&state, event, protocolZoneConfig(), clock)
		if err != nil || !changed || repair {
			b.Fatalf("apply update: changed=%t repair=%t err=%v", changed, repair, err)
		}
	}
}

func BenchmarkApplyRegistrationRenew(b *testing.B) {
	selector, state, clock := benchmarkSelectorState()
	event := registrationEvent{kind: "renew", uuid: pendingTestUUID, revision: 1, timestamp: 1787466000001}
	b.ReportAllocs()
	for b.Loop() {
		current := state.records[pendingTestUUID]
		event.revision = current.meta.Revision
		event.timestamp = current.meta.Timestamp + 1
		changed, repair, err := selector.applyEvent(&state, event, protocolZoneConfig(), clock)
		if err != nil || changed || repair {
			b.Fatalf("apply renew: changed=%t repair=%t err=%v", changed, repair, err)
		}
	}
}

func BenchmarkApplyPendingWithoutRepair(b *testing.B) {
	selector, state, clock := benchmarkSelectorState()
	change := pendingChange{event: registrationEvent{
		kind: "renew", uuid: pendingTestUUID, revision: 1, timestamp: 1787466000000,
	}}
	b.ReportAllocs()
	for b.Loop() {
		changed, repair, err := selector.applyPending(&state, []pendingChange{change}, protocolZoneConfig(), clock)
		if err != nil || changed || len(repair) != 0 {
			b.Fatalf("apply pending: changed=%t repair=%d err=%v", changed, len(repair), err)
		}
	}
}

func BenchmarkTypedSelectorOne500(b *testing.B) {
	data := make([]apiData, 500)
	for index := range data {
		data[index].Power = int64(index)
	}
	selector := newAPISelector(b, data)
	ctx := context.Background()
	b.ReportAllocs()
	for b.Loop() {
		_, selected, err := selector.One(ctx, func(candidates Candidates[apiAttr, apiData]) (Candidate[apiAttr, apiData], bool, error) {
			index := 0
			for candidate := 1; candidate < len(candidates); candidate++ {
				if candidates[candidate].Data.Power < candidates[index].Data.Power {
					index = candidate
				}
			}
			if err := candidates.Mutate(index, func(data *apiData) error {
				data.Power++
				return nil
			}); err != nil {
				return Candidate[apiAttr, apiData]{}, false, err
			}
			return candidates[index], true, nil
		})
		if err != nil || !selected {
			b.Fatalf("One selected=%v error=%v", selected, err)
		}
	}
}

func BenchmarkTypedSelectorAnyEightOf500(b *testing.B) {
	data := make([]apiData, 500)
	for index := range data {
		data[index].Power = int64(index)
	}
	selector := newAPISelector(b, data)
	ctx := context.Background()
	selected := make([]Candidate[apiAttr, apiData], 8)
	b.ReportAllocs()
	for b.Loop() {
		result, err := selector.Any(ctx, func(candidates Candidates[apiAttr, apiData]) ([]Candidate[apiAttr, apiData], error) {
			copy(selected, candidates[:len(selected)])
			return selected, nil
		})
		if err != nil || len(result) != len(selected) {
			b.Fatalf("Any selected=%d error=%v", len(result), err)
		}
	}
}

func BenchmarkReferenceSelectorWithOne500(b *testing.B) {
	data := make([]apiData, 500)
	for index := range data {
		data[index].Power = int64(index)
	}
	selector := newAPISelector(b, data)
	reference := newAPIReferenceSelector(b, selector)
	ctx := context.Background()
	b.ReportAllocs()
	for b.Loop() {
		selected, err := reference.WithOne(ctx, func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
			best, ok := candidates.At(0)
			if !ok {
				return apiReferenceSelection{}, false, nil
			}
			power := best.Data().Power()
			for index := 1; index < candidates.Len(); index++ {
				candidate, _ := candidates.At(index)
				candidatePower := candidate.Data().Power()
				if candidatePower < power {
					best = candidate
					power = candidatePower
				}
			}
			selection := best.Select()
			if err := selection.Edit().SetPower(power + 1); err != nil {
				return apiReferenceSelection{}, false, err
			}
			return selection, true, nil
		})
		if err != nil || !selected {
			b.Fatalf("WithOne selected=%v error=%v", selected, err)
		}
	}
}

func BenchmarkReferenceSelectorWithAnyEightOf500(b *testing.B) {
	data := make([]apiData, 500)
	for index := range data {
		data[index].Power = int64(index)
	}
	selector := newAPISelector(b, data)
	reference := newAPIReferenceSelector(b, selector)
	ctx := context.Background()
	selected := make([]apiReferenceSelection, 8)
	b.ReportAllocs()
	for b.Loop() {
		count, err := reference.WithAny(ctx, func(candidates apiReferenceCandidates) ([]apiReferenceSelection, error) {
			for index := range selected {
				candidate, _ := candidates.At(index)
				selected[index] = candidate.Select()
			}
			return selected, nil
		})
		if err != nil || count != len(selected) {
			b.Fatalf("WithAny selected=%d error=%v", count, err)
		}
	}
}

func benchmarkSelectorState() (*selectorCore, selectorState, redisClock) {
	data := make(Fields, 32)
	for index := range 32 {
		data[fmt.Sprintf("data%02d", index)] = make([]byte, 128)
	}
	record := &selectorRecord{
		meta: Meta{
			UUID: pendingTestUUID, Revision: 1, Timestamp: 1787466000000,
			TTL: 30_000, Version: 1,
		},
		attr: Fields{"role": []byte("worker")}, data: data, deadline: 1787466030000,
	}
	state := newSelectorState(1)
	state.records[pendingTestUUID] = record
	state.deadlines.set(pendingTestUUID, record.deadline)
	state.bytes = selectorRecordSize(record)
	selector := &selectorCore{client: newTestRuntime(runtimeConfig{
		selectorMaxBytes: 256 * 1024 * 1024, selectorRetainedBytes: 64 * 1024 * 1024,
	}, protocolZoneConfig())}
	clock := redisClock{anchor: time.Now(), upper: 1787466000000}
	return selector, state, clock
}
