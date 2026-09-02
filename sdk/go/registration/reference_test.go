package registration

import (
	"bytes"
	"context"
	"errors"
	"strconv"
	"sync"
	"testing"
)

type apiAttrRef struct {
	region ReferenceSlice[[]byte, byte]
}

func (view apiAttrRef) Region() []byte {
	return view.region.Clone()
}

type apiDataRef struct {
	power  int64
	queued int64
}

func (view apiDataRef) Power() int64 {
	return view.power
}

func (view apiDataRef) Queued() int64 {
	return view.queued
}

type apiDataEditor struct {
	editor ReferenceEditor[apiAttr, apiData]
}

func (editor apiDataEditor) SetPower(power int64) error {
	return editor.editor.Apply(func(data *apiData) { data.Power = power })
}

func (editor apiDataEditor) SetQueued(queued int64) error {
	return editor.editor.Apply(func(data *apiData) { data.Queued = queued })
}

type apiReferenceSelector = ReferenceSelector[apiAttr, apiData, apiAttrRef, apiDataRef, apiDataEditor]
type apiReferenceCandidates = ReferenceCandidates[apiAttr, apiData, apiAttrRef, apiDataRef, apiDataEditor]
type apiReferenceCandidate = ReferenceCandidate[apiAttr, apiData, apiAttrRef, apiDataRef, apiDataEditor]
type apiReferenceSelection = ReferenceSelection[apiAttr, apiData, apiAttrRef, apiDataRef, apiDataEditor]

type byteData struct {
	Payload []byte
}

func (value byteData) Encode() (Fields, error) {
	return Fields{"payload": bytes.Clone(value.Payload)}, nil
}

func (value *byteData) Decode(source Fields) error {
	value.Payload = bytes.Clone(source["payload"])
	return nil
}

type byteDataRef struct {
	payload ReferenceSlice[[]byte, byte]
}

func (view byteDataRef) Payload() []byte {
	return view.payload.Clone()
}

type byteDataEditor struct {
	editor ReferenceEditor[apiAttr, byteData]
}

func (editor byteDataEditor) SetPayload(payload []byte) error {
	return editor.editor.Apply(func(data *byteData) { data.Payload = bytes.Clone(payload) })
}

type byteReferenceCandidates = ReferenceCandidates[apiAttr, byteData, apiAttrRef, byteDataRef, byteDataEditor]
type byteReferenceSelection = ReferenceSelection[apiAttr, byteData, apiAttrRef, byteDataRef, byteDataEditor]

func TestReferenceSelectorOneCommitsOnlySelectedEdit(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}, {Power: 2}})
	reference := newAPIReferenceSelector(t, selector)

	selected, err := reference.WithOne(context.Background(), func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		first, ok := candidates.At(0)
		if !ok {
			t.Fatal("first candidate missing")
		}
		second, ok := candidates.At(1)
		if !ok {
			t.Fatal("second candidate missing")
		}
		if err := first.Select().Edit().SetPower(99); err != nil {
			return apiReferenceSelection{}, false, err
		}
		selection := second.Select()
		if err := selection.Edit().SetPower(3); err != nil {
			return apiReferenceSelection{}, false, err
		}
		if second.Data().Power() != 3 {
			t.Fatalf("staged power = %d", second.Data().Power())
		}
		return selection, true, nil
	})
	if err != nil || !selected {
		t.Fatalf("WithOne selected=%t error=%v", selected, err)
	}
	first, ok, err := selector.Find(context.Background(), "a")
	if err != nil || !ok || first.Data.Power != 1 {
		t.Fatalf("unselected edit committed: %#v, %t, %v", first, ok, err)
	}
	second, ok, err := selector.Find(context.Background(), "b")
	if err != nil || !ok || second.Data.Power != 3 {
		t.Fatalf("selected edit not committed: %#v, %t, %v", second, ok, err)
	}
	if cap(selector.candidates) != 0 {
		t.Fatalf("reference path allocated legacy candidates: len=%d cap=%d", len(selector.candidates), cap(selector.candidates))
	}
}

func TestReferenceSelectorRollbackAndLeaseInvalidation(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}})
	reference := newAPIReferenceSelector(t, selector)
	callbackErr := errors.New("policy")
	var retainedCandidates apiReferenceCandidates
	var retainedCandidate apiReferenceCandidate
	var retainedEditor apiDataEditor

	selected, err := reference.WithOne(context.Background(), func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		candidate, ok := candidates.At(0)
		if !ok {
			t.Fatal("candidate missing")
		}
		selection := candidate.Select()
		retainedCandidates = candidates
		retainedCandidate = candidate
		retainedEditor = selection.Edit()
		if err := retainedEditor.SetPower(7); err != nil {
			return apiReferenceSelection{}, false, err
		}
		return selection, true, callbackErr
	})
	if selected || !errors.Is(err, callbackErr) {
		t.Fatalf("WithOne selected=%t error=%v", selected, err)
	}
	if retainedCandidates.Len() != 1 {
		t.Fatalf("retained candidate count = %d", retainedCandidates.Len())
	}
	if retainedCandidate.Valid() {
		t.Fatal("borrowed reference remained valid after callback")
	}
	if err := retainedEditor.SetPower(8); !IsCode(err, CodeContract) {
		t.Fatalf("retained editor error = %v", err)
	}
	assertAPIPower(t, selector, "a", 1)

	selected, err = reference.WithOne(context.Background(), func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		candidate, _ := candidates.At(0)
		if err := candidate.Select().Edit().SetPower(9); err != nil {
			return apiReferenceSelection{}, false, err
		}
		return apiReferenceSelection{}, false, nil
	})
	if err != nil || selected {
		t.Fatalf("empty WithOne selected=%t error=%v", selected, err)
	}
	assertAPIPower(t, selector, "a", 1)

	ctx, cancel := context.WithCancel(context.Background())
	selected, err = reference.WithOne(ctx, func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		candidate, _ := candidates.At(0)
		selection := candidate.Select()
		if err := selection.Edit().SetPower(10); err != nil {
			return apiReferenceSelection{}, false, err
		}
		cancel()
		return selection, true, nil
	})
	if selected || !IsCode(err, CodeClosed) {
		t.Fatalf("cancelled WithOne selected=%t error=%v", selected, err)
	}
	assertAPIPower(t, selector, "a", 1)
}

func TestReferenceSelectorAnyValidatesAtomically(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}, {Power: 2}, {Power: 3}})
	reference := newAPIReferenceSelector(t, selector)
	selected := make([]apiReferenceSelection, 0, 2)

	count, err := reference.WithAny(context.Background(), func(candidates apiReferenceCandidates) ([]apiReferenceSelection, error) {
		selected = selected[:0]
		for index := range 2 {
			candidate, ok := candidates.At(index)
			if !ok {
				t.Fatalf("candidate %d missing", index)
			}
			selection := candidate.Select()
			if err := selection.Edit().SetQueued(int64(index + 4)); err != nil {
				return nil, err
			}
			selected = append(selected, selection)
		}
		third, _ := candidates.At(2)
		if err := third.Select().Edit().SetPower(99); err != nil {
			return nil, err
		}
		return selected, nil
	})
	if err != nil || count != 2 {
		t.Fatalf("WithAny count=%d error=%v", count, err)
	}
	assertAPIData(t, selector, "a", apiData{Power: 1, Queued: 4})
	assertAPIData(t, selector, "b", apiData{Power: 2, Queued: 5})
	assertAPIData(t, selector, "c", apiData{Power: 3})

	duplicate := newAPISelector(t, []apiData{{Power: 1}})
	duplicateReference := newAPIReferenceSelector(t, duplicate)
	count, err = duplicateReference.WithAny(context.Background(), func(candidates apiReferenceCandidates) ([]apiReferenceSelection, error) {
		candidate, _ := candidates.At(0)
		selection := candidate.Select()
		if err := selection.Edit().SetPower(8); err != nil {
			return nil, err
		}
		return []apiReferenceSelection{selection, selection}, nil
	})
	if count != 0 || !IsCode(err, CodeContract) {
		t.Fatalf("duplicate WithAny count=%d error=%v", count, err)
	}
	assertAPIPower(t, duplicate, "a", 1)

	atomicSelector := newAPISelector(t, []apiData{{Power: 1}, {Power: 2}})
	atomicReference := newAPIReferenceSelector(t, atomicSelector)
	originalEncode := atomicSelector.encodeData
	calls := 0
	atomicSelector.encodeData = func(value apiData) (fields, error) {
		calls++
		if calls == 2 {
			return nil, errors.New("encode second selection")
		}
		return originalEncode(value)
	}
	count, err = atomicReference.WithAny(context.Background(), func(candidates apiReferenceCandidates) ([]apiReferenceSelection, error) {
		result := make([]apiReferenceSelection, 0, 2)
		for index := range 2 {
			candidate, _ := candidates.At(index)
			selection := candidate.Select()
			if err := selection.Edit().SetPower(int64(index + 7)); err != nil {
				return nil, err
			}
			result = append(result, selection)
		}
		return result, nil
	})
	if count != 0 || err == nil {
		t.Fatalf("atomic WithAny count=%d error=%v", count, err)
	}
	if len(atomicSelector.overlays) != 0 {
		t.Fatalf("failed atomic selection published overlays: %#v", atomicSelector.overlays)
	}
}

func TestReferenceSelectorRejectsForeignAndStructuralChanges(t *testing.T) {
	first := newAPISelector(t, []apiData{{Power: 1}})
	second := newAPISelector(t, []apiData{{Power: 2}})
	firstReference := newAPIReferenceSelector(t, first)
	secondReference := newAPIReferenceSelector(t, second)
	var foreign apiReferenceSelection

	_, err := secondReference.WithOne(context.Background(), func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		candidate, _ := candidates.At(0)
		foreign = candidate.Select()
		return apiReferenceSelection{}, false, nil
	})
	if err != nil {
		t.Fatal(err)
	}
	selected, err := firstReference.WithOne(context.Background(), func(apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		return foreign, true, nil
	})
	if selected || !IsCode(err, CodeContract) {
		t.Fatalf("foreign WithOne selected=%t error=%v", selected, err)
	}

	first.encodeData = func(value apiData) (fields, error) {
		return fields{"power": []byte(strconv.FormatInt(value.Power, 10))}, nil
	}
	selected, err = firstReference.WithOne(context.Background(), func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		candidate, _ := candidates.At(0)
		selection := candidate.Select()
		if err := selection.Edit().SetPower(3); err != nil {
			return apiReferenceSelection{}, false, err
		}
		return selection, true, nil
	})
	if selected || !IsCode(err, CodeContract) {
		t.Fatalf("structural WithOne selected=%t error=%v", selected, err)
	}
	if len(first.overlays) != 0 {
		t.Fatalf("structural failure published overlay: %#v", first.overlays)
	}
}

func TestReferenceSelectorClonesMutableDataAndSetterInput(t *testing.T) {
	selector := newByteSelector(t, []byte("old"))
	reference, err := NewReferenceSelector(selector, ReferenceSchema[apiAttr, byteData, apiAttrRef, byteDataRef, byteDataEditor]{
		Attr: func(value *apiAttr) apiAttrRef {
			return apiAttrRef{region: NewReferenceSlice(value.Region)}
		},
		Data: func(value *byteData) byteDataRef {
			return byteDataRef{payload: NewReferenceSlice(value.Payload)}
		},
		Edit: func(editor ReferenceEditor[apiAttr, byteData]) byteDataEditor {
			return byteDataEditor{editor: editor}
		},
		CloneData: func(value byteData) byteData {
			value.Payload = bytes.Clone(value.Payload)
			return value
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	input := []byte("next")
	selected, err := reference.WithOne(context.Background(), func(candidates byteReferenceCandidates) (byteReferenceSelection, bool, error) {
		candidate, ok := candidates.At(0)
		if !ok {
			t.Fatal("candidate missing")
		}
		borrowedCopy := candidate.Data().Payload()
		borrowedCopy[0] = 'x'
		selection := candidate.Select()
		if err := selection.Edit().SetPayload(input); err != nil {
			return byteReferenceSelection{}, false, err
		}
		input[0] = 'x'
		if got := string(candidate.Data().Payload()); got != "next" {
			t.Fatalf("staged payload = %q", got)
		}
		return selection, true, nil
	})
	if err != nil || !selected {
		t.Fatalf("WithOne selected=%t error=%v", selected, err)
	}
	candidate, ok, err := selector.Find(context.Background(), "a")
	if err != nil || !ok || string(candidate.Data.Payload) != "next" {
		t.Fatalf("Find = %#v, %t, %v", candidate, ok, err)
	}
	record := selector.core.view.Load().records["a"]
	if got := string(record.projectedData.(byteData).Payload); got != "old" {
		t.Fatalf("immutable projected data changed to %q", got)
	}
}

func TestReferenceSelectorSerializesConcurrentPrediction(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 0}})
	reference := newAPIReferenceSelector(t, selector)
	const workers = 8
	const iterations = 100
	start := make(chan struct{})
	errorsFound := make(chan error, workers)
	var wait sync.WaitGroup
	wait.Add(workers)
	for range workers {
		go func() {
			defer wait.Done()
			<-start
			for range iterations {
				selected, err := reference.WithOne(context.Background(), func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
					candidate, ok := candidates.At(0)
					if !ok {
						return apiReferenceSelection{}, false, errors.New("candidate missing")
					}
					selection := candidate.Select()
					if err := selection.Edit().SetPower(candidate.Data().Power() + 1); err != nil {
						return apiReferenceSelection{}, false, err
					}
					return selection, true, nil
				})
				if err != nil || !selected {
					if err == nil {
						err = errors.New("no candidate selected")
					}
					errorsFound <- err
					return
				}
			}
		}()
	}
	close(start)
	wait.Wait()
	close(errorsFound)
	for err := range errorsFound {
		t.Fatal(err)
	}
	assertAPIPower(t, selector, "a", workers*iterations)
}

func TestReferenceSelectorPanicReleasesLeaseAndTransactionGate(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}})
	reference := newAPIReferenceSelector(t, selector)
	var retained apiDataEditor
	func() {
		defer func() {
			if recovered := recover(); recovered != "policy panic" {
				t.Fatalf("recovered = %#v", recovered)
			}
		}()
		_, _ = reference.WithOne(context.Background(), func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
			candidate, _ := candidates.At(0)
			selection := candidate.Select()
			retained = selection.Edit()
			if err := retained.SetPower(7); err != nil {
				t.Fatal(err)
			}
			panic("policy panic")
		})
	}()
	if err := retained.SetPower(8); !IsCode(err, CodeContract) {
		t.Fatalf("retained editor error = %v", err)
	}
	assertAPIPower(t, selector, "a", 1)
	selected, err := reference.WithOne(context.Background(), func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		candidate, _ := candidates.At(0)
		return candidate.Select(), true, nil
	})
	if err != nil || !selected {
		t.Fatalf("WithOne after panic selected=%t error=%v", selected, err)
	}
}

func TestNewReferenceSelectorValidatesSchema(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}})
	if _, err := NewReferenceSelector(selector, ReferenceSchema[apiAttr, apiData, apiAttrRef, apiDataRef, apiDataEditor]{}); !IsCode(err, CodeInvalid) {
		t.Fatalf("empty schema error = %v", err)
	}
	var nilSelector *Selector[apiAttr, apiData]
	if _, err := NewReferenceSelector(nilSelector, apiReferenceSchema()); !IsCode(err, CodeInvalid) {
		t.Fatalf("nil selector error = %v", err)
	}
}

func TestReferenceSelectorAvailabilityEmptySelectionAndTokenWrap(t *testing.T) {
	selector := newAPISelector(t, []apiData{{Power: 1}})
	reference := newAPIReferenceSelector(t, selector)
	view := selector.core.view.Load()
	half := *view
	half.synchronized = false
	selector.core.view.Store(&half)
	called := false
	selected, err := reference.WithOne(context.Background(), func(apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		called = true
		return apiReferenceSelection{}, false, nil
	})
	if selected || called || !IsCode(err, CodeUnavailable) {
		t.Fatalf("half-synchronized WithOne selected=%t called=%t error=%v", selected, called, err)
	}

	selector.core.view.Store(view)
	selector.transaction.token = ^uint64(0)
	selected, err = reference.WithOne(context.Background(), func(candidates apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		candidate, ok := candidates.At(0)
		if !ok || !candidate.Valid() || candidate.Meta().UUID != "a" || string(candidate.Attr().Region()) != "east" {
			t.Fatalf("candidate after token wrap = %#v, %t", candidate, ok)
		}
		return candidate.Select(), true, nil
	})
	if err != nil || !selected || selector.transaction.token != 1 {
		t.Fatalf("token-wrap WithOne selected=%t token=%d error=%v", selected, selector.transaction.token, err)
	}

	count, err := reference.WithAny(context.Background(), func(apiReferenceCandidates) ([]apiReferenceSelection, error) {
		return nil, nil
	})
	if err != nil || count != 0 {
		t.Fatalf("empty WithAny count=%d error=%v", count, err)
	}
	if _, err := reference.WithOne(nil, func(apiReferenceCandidates) (apiReferenceSelection, bool, error) {
		return apiReferenceSelection{}, false, nil
	}); !IsCode(err, CodeInvalid) {
		t.Fatalf("nil-context WithOne error = %v", err)
	}
	if _, err := reference.WithOne(context.Background(), nil); !IsCode(err, CodeInvalid) {
		t.Fatalf("nil-callback WithOne error = %v", err)
	}
	selector.closed.Store(true)
	if _, err := reference.WithAny(context.Background(), func(apiReferenceCandidates) ([]apiReferenceSelection, error) {
		return nil, nil
	}); !IsCode(err, CodeClosed) {
		t.Fatalf("closed WithAny error = %v", err)
	}
}

func newAPIReferenceSelector(t interface {
	Helper()
	Fatalf(string, ...any)
}, selector *Selector[apiAttr, apiData]) *apiReferenceSelector {
	t.Helper()
	reference, err := NewReferenceSelector(selector, apiReferenceSchema())
	if err != nil {
		t.Fatalf("NewReferenceSelector error = %v", err)
		return nil
	}
	return reference
}

func apiReferenceSchema() ReferenceSchema[apiAttr, apiData, apiAttrRef, apiDataRef, apiDataEditor] {
	return ReferenceSchema[apiAttr, apiData, apiAttrRef, apiDataRef, apiDataEditor]{
		Attr: func(value *apiAttr) apiAttrRef {
			return apiAttrRef{region: NewReferenceSlice(value.Region)}
		},
		Data: func(value *apiData) apiDataRef {
			return apiDataRef{power: value.Power, queued: value.Queued}
		},
		Edit: func(editor ReferenceEditor[apiAttr, apiData]) apiDataEditor {
			return apiDataEditor{editor: editor}
		},
		CloneData: func(value apiData) apiData { return value },
	}
}

func newByteSelector(t interface{ Helper() }, payload []byte) *Selector[apiAttr, byteData] {
	t.Helper()
	limits := defaultZoneConfig()
	client := newTestRuntime(runtimeConfig{selectorMaxBytes: 1 << 20}, limits)
	core := &selectorCore{client: client}
	view := emptySelectorView(1, true)
	record := &selectorRecord{
		meta:          Meta{UUID: "a", Revision: 1, Timestamp: 100, TTL: 1_000, Version: 1},
		attr:          Fields{"region": []byte("east")},
		data:          Fields{"payload": bytes.Clone(payload)},
		projectedAttr: apiAttr{Region: []byte("east")},
		projectedData: byteData{Payload: bytes.Clone(payload)},
	}
	view.orderedRecords = append(view.orderedRecords, record)
	view.records["a"] = record
	core.view.Store(view)
	selector := &Selector[apiAttr, byteData]{
		core:      core,
		operation: make(chan struct{}, 1),
		overlays:  make(map[string]localOverlay[byteData]),
	}
	selector.operation <- struct{}{}
	selector.transaction.selector = selector
	selector.encodeAttr = encodeSelectorAttr
	selector.encodeData = encodeSelectorData
	selector.decodeAttr = decodeSelectorAttr[apiAttr, *apiAttr]
	selector.decodeData = decodeSelectorData[byteData, *byteData]
	selector.decodeOwnedAttr = decodeOwnedSelectorAttr[apiAttr, *apiAttr]
	selector.decodeOwnedData = decodeOwnedSelectorData[byteData, *byteData]
	return selector
}

func assertAPIPower(t *testing.T, selector *Selector[apiAttr, apiData], uuid string, power int64) {
	t.Helper()
	candidate, ok, err := selector.Find(context.Background(), uuid)
	if err != nil || !ok || candidate.Data.Power != power {
		t.Fatalf("Find(%q) = %#v, %t, %v; power want %d", uuid, candidate, ok, err, power)
	}
}

func assertAPIData(t *testing.T, selector *Selector[apiAttr, apiData], uuid string, want apiData) {
	t.Helper()
	candidate, ok, err := selector.Find(context.Background(), uuid)
	if err != nil || !ok || *candidate.Data != want {
		t.Fatalf("Find(%q) = %#v, %t, %v; data want %#v", uuid, candidate, ok, err, want)
	}
}
