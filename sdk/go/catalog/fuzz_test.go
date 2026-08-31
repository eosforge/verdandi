package catalog

import (
	"testing"

	"github.com/vmihailenco/msgpack/v5"
)

func FuzzDecodeCatalogEvent(fuzzer *testing.F) {
	path, err := NewPath("routing", "fuzz")
	if err != nil {
		fuzzer.Fatal(err)
	}
	seeds := []any{
		[]any{
			"v1", "replace", path.member(), "1", "map", "4",
			[]any{"a", []byte("one")},
		},
		[]any{
			"v1", "patch", path.member(), "1", "2", "map", "4",
			[]any{"a", []byte("two")},
		},
		[]any{"v1", "delete", path.member(), "3"},
	}
	for _, seed := range seeds {
		encoded, encodeErr := msgpack.Marshal(seed)
		if encodeErr != nil {
			fuzzer.Fatal(encodeErr)
		}
		fuzzer.Add(encoded)
	}
	for _, seed := range [][]byte{
		nil,
		{0xdd, 0xff, 0xff, 0xff, 0xff},
		{0x94, 0xa2, 'v', '1'},
	} {
		fuzzer.Add(seed)
	}

	fuzzer.Fuzz(func(t *testing.T, payload []byte) {
		event, decodeErr := decodeEvent(string(payload), path, 128)
		if decodeErr != nil {
			return
		}
		if event.path != path || event.revision == 0 || event.revision > maximumRevision {
			t.Fatalf("invalid accepted event identity: %#v", event)
		}
		switch event.kind {
		case eventReplace:
			_, encodedBytes, validateErr := validateValue(event.valueKind, event.fields, 128)
			if validateErr != nil || encodedBytes != event.encodedBytes {
				t.Fatalf("invalid accepted Replace: event=%#v error=%v", event, validateErr)
			}
		case eventPatch:
			if event.baseRevision == 0 || event.baseRevision >= event.revision ||
				event.valueKind == Value {
				t.Fatalf("invalid accepted Patch header: %#v", event)
			}
			if _, validateErr := validatePatchFields(event.fields, 128); validateErr != nil {
				t.Fatalf("invalid accepted Patch: event=%#v error=%v", event, validateErr)
			}
		case eventDelete:
			if event.baseRevision != 0 || event.valueKind != 0 || event.encodedBytes != 0 ||
				len(event.fields) != 0 {
				t.Fatalf("invalid accepted Delete: %#v", event)
			}
		default:
			t.Fatalf("invalid accepted event kind: %#v", event)
		}
	})
}
