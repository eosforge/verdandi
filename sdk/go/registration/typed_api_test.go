package registration

import (
	"bytes"
	"context"
	"errors"
	"strconv"
	"testing"
	"time"
)

type apiAttr struct {
	Region []byte
}

func (value apiAttr) Encode() (Fields, error) {
	return Fields{"region": bytes.Clone(value.Region)}, nil
}

func (value *apiAttr) Decode(src Fields) error {
	value.Region = bytes.Clone(src["region"])
	return nil
}

type apiData struct {
	Power  int64
	Queued int64
}

func (value apiData) Encode() (Fields, error) {
	return Fields{
		"power":  []byte(strconv.FormatInt(value.Power, 10)),
		"queued": []byte(strconv.FormatInt(value.Queued, 10)),
	}, nil
}

func (value *apiData) Decode(src Fields) error {
	power, err := strconv.ParseInt(string(src["power"]), 10, 64)
	if err != nil {
		return err
	}
	value.Power = power
	queued, err := strconv.ParseInt(string(src["queued"]), 10, 64)
	if err != nil {
		return err
	}
	value.Queued = queued
	return nil
}

func TestClientRegistrationIsLocal(t *testing.T) {
	client := &Client{}
	registration, err := client.Registration[apiAttr, apiData](RegistrationOptions{
		Type:    "proxy",
		TTL:     3 * time.Second,
		Version: 1,
	})
	if err != nil {
		t.Fatalf("Client.Registration error = %v", err)
	}
	if len(registration.UUID()) != 32 || registration.Registered() || registration.Revision() != 0 || registration.Timestamp() != 0 {
		t.Fatalf("unexpected unpublished Registration: uuid=%q registered=%v revision=%d timestamp=%d",
			registration.UUID(), registration.Registered(), registration.Revision(), registration.Timestamp())
	}
	if err := registration.Unregister(context.Background()); err != nil {
		t.Fatalf("local Unregister error = %v", err)
	}
	if err := registration.Register(context.Background(), apiAttr{}, apiData{}); !IsCode(err, CodeClosed) {
		t.Fatalf("Register after Unregister error = %v", err)
	}
}

func TestRawFieldsImplementTypedBoundaryWithoutAliasing(t *testing.T) {
	source := Fields{"value": []byte("one")}
	encoded, err := encodeFieldValue(source, "data")
	if err != nil {
		t.Fatalf("encodeFieldValue error = %v", err)
	}
	source["value"][0] = 'x'
	if string(encoded["value"]) != "one" {
		t.Fatalf("encoded value aliased source: %q", encoded["value"])
	}
	decoded, err := decodeFieldValue[Fields, *Fields](encoded, "data")
	if err != nil {
		t.Fatalf("decodeFieldValue error = %v", err)
	}
	decoded["value"][0] = 'y'
	if string(encoded["value"]) != "one" {
		t.Fatalf("decoded value aliased encoded source: %q", encoded["value"])
	}
}

func TestSelectorOneCommitsAndReconcilesLocalMutation(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 4}, {Power: 1}})
	selected, ok, err := selector.One(context.Background(), func(candidates Candidates[apiAttr, apiData]) (Candidate[apiAttr, apiData], bool, error) {
		index := 0
		if candidates[1].Data.Power < candidates[0].Data.Power {
			index = 1
		}
		if err := candidates.Mutate(index, func(data *apiData) error {
			data.Power++
			return nil
		}); err != nil {
			return Candidate[apiAttr, apiData]{}, false, err
		}
		if candidates[index].Data.Power != 2 {
			t.Fatalf("mutation not visible inside callback: %#v", candidates[index].Data)
		}
		return candidates[index], true, nil
	})
	if err != nil || !ok || selected.Meta.UUID != "b" || selected.Data.Power != 2 {
		t.Fatalf("One = %#v, %v, %v", selected, ok, err)
	}
	for _, commit := range selector.transaction.commits[:cap(selector.transaction.commits)] {
		if commit.uuid != "" || commit.overlay.base != nil || commit.overlay.fields != nil {
			t.Fatalf("commit scratch retained published state: %#v", commit)
		}
	}

	observed, ok, err := selector.One(context.Background(), func(candidates Candidates[apiAttr, apiData]) (Candidate[apiAttr, apiData], bool, error) {
		return candidates[1], true, nil
	})
	if err != nil || !ok || observed.Data.Power != 2 {
		t.Fatalf("committed overlay = %#v, %v, %v", observed, ok, err)
	}

	view := selector.core.view.Load()
	renewed := *view
	renewed.records = cloneRecordMap(view.records)
	renewed.records["b"] = cloneSelectorRecord(view.records["b"])
	renewed.records["b"].meta.Timestamp++
	selector.core.view.Store(&renewed)
	preserved, ok, err := selector.Find(context.Background(), "b")
	if err != nil || !ok || preserved.Data.Power != 2 {
		t.Fatalf("renew did not preserve overlay: %#v, %v, %v", preserved, ok, err)
	}

	partiallyUpdated := renewed
	partiallyUpdated.records = cloneRecordMap(renewed.records)
	partiallyUpdated.records["b"] = cloneSelectorRecord(renewed.records["b"])
	partiallyUpdated.records["b"].meta.Revision++
	partiallyUpdated.records["b"].projectedData = apiData{Power: 1, Queued: 5}
	partiallyUpdated.records["b"].data = Fields{"power": []byte("1"), "queued": []byte("5")}
	selector.core.view.Store(&partiallyUpdated)
	merged, ok, err := selector.Find(context.Background(), "b")
	if err != nil || !ok || merged.Data.Power != 2 || merged.Data.Queued != 5 {
		t.Fatalf("remote unrelated field did not preserve overlay: %#v, %v, %v", merged, ok, err)
	}

	updated := partiallyUpdated
	updated.records = cloneRecordMap(partiallyUpdated.records)
	updated.records["b"] = cloneSelectorRecord(partiallyUpdated.records["b"])
	updated.records["b"].meta.Revision++
	updated.records["b"].projectedData = apiData{Power: 9, Queued: 5}
	updated.records["b"].data = Fields{"power": []byte("9"), "queued": []byte("5")}
	selector.core.view.Store(&updated)
	reconciled, ok, err := selector.Find(context.Background(), "b")
	if err != nil || !ok || reconciled.Data.Power != 9 {
		t.Fatalf("remote content did not replace overlay: %#v, %v, %v", reconciled, ok, err)
	}
}

func TestSelectorDetachedDecodeFailureDoesNotCommitMutation(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}})
	selector.decodeOwnedData = func(Fields) (apiData, error) {
		return apiData{}, errors.New("decode selected data")
	}
	_, _, err := selector.One(context.Background(), func(candidates Candidates[apiAttr, apiData]) (Candidate[apiAttr, apiData], bool, error) {
		if mutateErr := candidates.Mutate(0, func(data *apiData) error {
			data.Power = 2
			return nil
		}); mutateErr != nil {
			return Candidate[apiAttr, apiData]{}, false, mutateErr
		}
		return candidates[0], true, nil
	})
	if err == nil {
		t.Fatal("One succeeded after detached decoder failure")
	}
	if len(selector.overlays) != 0 {
		t.Fatalf("detached decoder failure committed overlay: %#v", selector.overlays)
	}
}

func TestSelectorTransactionReuseClearsShrunkViewTail(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}, {Power: 2}})
	if _, _, err := selector.begin(); err != nil {
		t.Fatal(err)
	}
	view := selector.core.view.Load()
	reduced := *view
	reduced.orderedRecords = view.orderedRecords[:1]
	reduced.records = map[string]*selectorRecord{view.orderedRecords[0].meta.UUID: view.orderedRecords[0]}
	selector.core.view.Store(&reduced)
	if _, _, err := selector.begin(); err != nil {
		t.Fatal(err)
	}
	entries := selector.transaction.entries[:cap(selector.transaction.entries)]
	if entries[1].record != nil || entries[1].dataFields != nil {
		t.Fatalf("transaction entry tail retained removed record: %#v", entries[1])
	}
	candidates := selector.candidates[:cap(selector.candidates)]
	if candidates[1].transaction != nil || candidates[1].Attr != nil || candidates[1].Data != nil {
		t.Fatalf("candidate tail retained removed record: %#v", candidates[1])
	}
}

func TestSelectorSelectionRollbackAndCandidateValidation(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}, {Power: 2}})
	callbackErr := errors.New("policy")
	_, _, err := selector.One(context.Background(), func(candidates Candidates[apiAttr, apiData]) (Candidate[apiAttr, apiData], bool, error) {
		if mutateErr := candidates.Mutate(0, func(data *apiData) error {
			data.Power = 99
			return nil
		}); mutateErr != nil {
			return Candidate[apiAttr, apiData]{}, false, mutateErr
		}
		return candidates[0], true, callbackErr
	})
	if !errors.Is(err, callbackErr) {
		t.Fatalf("callback error = %v", err)
	}
	record, ok, err := selector.Find(context.Background(), "a")
	if err != nil || !ok || record.Data.Power != 1 {
		t.Fatalf("failed selection committed mutation: %#v, %v, %v", record, ok, err)
	}

	_, err = selector.Any(context.Background(), func(candidates Candidates[apiAttr, apiData]) ([]Candidate[apiAttr, apiData], error) {
		return []Candidate[apiAttr, apiData]{candidates[0], candidates[0]}, nil
	})
	if !IsCode(err, CodeContract) {
		t.Fatalf("duplicate Any error = %v", err)
	}
	selector.transaction.token = ^uint64(0)
	selector.transaction.selected = []uint64{1, 1}
	selected, err := selector.Any(context.Background(), func(candidates Candidates[apiAttr, apiData]) ([]Candidate[apiAttr, apiData], error) {
		return []Candidate[apiAttr, apiData]{candidates[0], candidates[1]}, nil
	})
	if err != nil || len(selected) != 2 {
		t.Fatalf("Any after token wrap = %#v, %v", selected, err)
	}

	foreign := Candidate[apiAttr, apiData]{Meta: Meta{UUID: "a"}}
	_, _, err = selector.One(context.Background(), func(Candidates[apiAttr, apiData]) (Candidate[apiAttr, apiData], bool, error) {
		return foreign, true, nil
	})
	if !IsCode(err, CodeContract) {
		t.Fatalf("foreign One error = %v", err)
	}

	_, _, err = selector.One(context.Background(), func(candidates Candidates[apiAttr, apiData]) (Candidate[apiAttr, apiData], bool, error) {
		candidates[0].Data.Power++
		return candidates[0], true, nil
	})
	if !IsCode(err, CodeContract) {
		t.Fatalf("direct borrowed mutation error = %v", err)
	}
}

func TestSelectorAnyCommitsStagedMutationsAndReturnsDetachedValues(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}, {Power: 2}})
	selected, err := selector.Any(context.Background(), func(candidates Candidates[apiAttr, apiData]) ([]Candidate[apiAttr, apiData], error) {
		if err := candidates.Mutate(0, func(data *apiData) error {
			data.Power++
			return nil
		}); err != nil {
			return nil, err
		}
		if err := candidates.Mutate(1, func(data *apiData) error {
			data.Queued = 3
			return nil
		}); err != nil {
			return nil, err
		}
		return []Candidate[apiAttr, apiData]{candidates[0], candidates[1]}, nil
	})
	if err != nil || len(selected) != 2 || selected[0].Data.Power != 2 || selected[1].Data.Queued != 3 {
		t.Fatalf("Any = %#v, %v", selected, err)
	}
	selected[0].Data.Power = 99
	first, ok, err := selector.Find(context.Background(), "a")
	if err != nil || !ok || first.Data.Power != 2 {
		t.Fatalf("detached Any result changed overlay: %#v, %v, %v", first, ok, err)
	}
}

func TestSelectorCommitRollsBackAllOverlaysWhenLaterDecodeFails(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}, {Power: 2}})
	transaction, _, err := selector.begin()
	if err != nil {
		t.Fatal(err)
	}
	transaction.entries[0].staged = true
	transaction.entries[0].dataFields = Fields{"power": []byte("3"), "queued": []byte("0")}
	transaction.entries[1].staged = true
	transaction.entries[1].dataFields = Fields{"power": []byte("4"), "queued": []byte("0")}
	original := selector.decodeData
	calls := 0
	selector.decodeData = func(source Fields) (apiData, error) {
		calls++
		if calls == 2 {
			return apiData{}, errors.New("decode")
		}
		return original(source)
	}
	if err := transaction.commit(); err == nil {
		t.Fatal("commit succeeded after decoder failure")
	}
	if len(selector.overlays) != 0 {
		t.Fatalf("commit published partial overlays: %#v", selector.overlays)
	}
}

func TestSelectorCallbackCancellationRollsBackAfterReturn(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}})
	ctx, cancel := context.WithCancel(context.Background())
	_, _, err := selector.One(ctx, func(candidates Candidates[apiAttr, apiData]) (Candidate[apiAttr, apiData], bool, error) {
		if mutateErr := candidates.Mutate(0, func(data *apiData) error {
			data.Power = 8
			return nil
		}); mutateErr != nil {
			return Candidate[apiAttr, apiData]{}, false, mutateErr
		}
		cancel()
		return candidates[0], true, nil
	})
	if !IsCode(err, CodeClosed) {
		t.Fatalf("cancellation error = %v", err)
	}
	record, ok, err := selector.Find(context.Background(), "a")
	if err != nil || !ok || record.Data.Power != 1 {
		t.Fatalf("cancellation committed mutation: %#v, %v, %v", record, ok, err)
	}
}

func TestSelectorHalfSynchronizedTypedViewIsUnavailable(t *testing.T) {
	t.Parallel()
	selector := newAPISelector(t, []apiData{{Power: 1}})
	view := selector.core.view.Load()
	half := *view
	half.synchronized = false
	selector.core.view.Store(&half)
	ctx := context.Background()

	if _, err := selector.Snapshot(ctx); !IsCode(err, CodeUnavailable) {
		t.Fatalf("Snapshot error = %v, want unavailable", err)
	}
	if _, _, err := selector.Find(ctx, "a"); !IsCode(err, CodeUnavailable) {
		t.Fatalf("Find error = %v, want unavailable", err)
	}
	if _, _, err := selector.FindRetained(ctx, "a"); !IsCode(err, CodeUnavailable) {
		t.Fatalf("FindRetained error = %v, want unavailable", err)
	}
	if _, _, err := selector.One(ctx, func(candidates Candidates[apiAttr, apiData]) (Candidate[apiAttr, apiData], bool, error) {
		return Candidate[apiAttr, apiData]{}, false, nil
	}); !IsCode(err, CodeUnavailable) {
		t.Fatalf("One error = %v, want unavailable", err)
	}
	if _, err := selector.Any(ctx, func(candidates Candidates[apiAttr, apiData]) ([]Candidate[apiAttr, apiData], error) {
		return nil, nil
	}); !IsCode(err, CodeUnavailable) {
		t.Fatalf("Any error = %v, want unavailable", err)
	}
}

func newAPISelector(t interface{ Helper() }, data []apiData) *Selector[apiAttr, apiData] {
	t.Helper()
	limits := defaultZoneConfig()
	client := newTestRuntime(runtimeConfig{selectorMaxBytes: 1 << 20}, limits)
	core := &selectorCore{client: client}
	view := emptySelectorView(1, true)
	for index, value := range data {
		uuid := string(rune('a' + index))
		record := &selectorRecord{
			meta: Meta{UUID: uuid, Revision: 1, Timestamp: 100, TTL: 1_000, Version: 1},
			attr: Fields{"region": []byte("east")},
			data: Fields{
				"power":  []byte(strconv.FormatInt(value.Power, 10)),
				"queued": []byte(strconv.FormatInt(value.Queued, 10)),
			},
			projectedAttr: apiAttr{Region: []byte("east")},
			projectedData: value,
		}
		view.orderedRecords = append(view.orderedRecords, record)
		view.records[uuid] = record
	}
	core.view.Store(view)
	selector := &Selector[apiAttr, apiData]{
		core:      core,
		operation: make(chan struct{}, 1),
		overlays:  make(map[string]localOverlay[apiData]),
	}
	selector.operation <- struct{}{}
	selector.transaction.selector = selector
	selector.encodeAttr = encodeSelectorAttr
	selector.encodeData = encodeSelectorData
	selector.decodeAttr = decodeSelectorAttr[apiAttr, *apiAttr]
	selector.decodeData = decodeSelectorData[apiData, *apiData]
	selector.decodeOwnedAttr = decodeOwnedSelectorAttr[apiAttr, *apiAttr]
	selector.decodeOwnedData = decodeOwnedSelectorData[apiData, *apiData]
	return selector
}

func cloneRecordMap(source map[string]*selectorRecord) map[string]*selectorRecord {
	result := make(map[string]*selectorRecord, len(source))
	for uuid, record := range source {
		result[uuid] = record
	}
	return result
}
