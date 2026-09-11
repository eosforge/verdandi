package catalog

import (
	"bytes"
	"testing"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

func TestReadPatchAddsFirstMapFieldWithoutChangingBase(t *testing.T) {
	t.Parallel()
	for _, fields := range []verdandi.Fields{nil, {}} {
		base := &rawState{revision: 1, replaceRevision: 1, status: StatusPresent, kind: Map, fields: fields}
		reply := []any{
			"&result", "ok", "&status", "present", "&mode", "patch",
			"@revision", "2", "@replace_revision", "1", "@kind", "map",
			"@encoded_bytes", "2", "&fields", []any{"a", []byte("x")},
		}
		state, err := parseReadReply(reply, base, 1024)
		if err != nil {
			t.Fatal(err)
		}
		if state.revision != 2 || !bytes.Equal(state.fields["a"], []byte("x")) || len(base.fields) != 0 {
			t.Fatalf("Patch must add an owned first field without changing the base: %#v", state)
		}
	}
}

func TestReadErrorValidatesPresentFieldsAndDomainStatuses(t *testing.T) {
	for _, pair := range [][2]any{{"@revision", nil}, {"@revision", "01"}, {"&field", []any{"x"}}, {"unexpected", "x"}} {
		_, err := parseReadReply([]any{"&result", "error", "&status", "stale", pair[0], pair[1]}, nil, 1024)
		if !verdandi.IsCode(err, verdandi.CodeCorrupt) {
			t.Errorf("%v: %v", pair, err)
		}
	}
	for _, status := range []string{"closed", "deadline", "ambiguous", "target", "missing", "immutable", "unknown"} {
		_, err := parseReadReply([]any{"&result", "error", "&status", status}, nil, 1024)
		if !verdandi.IsCode(err, verdandi.CodeProtocol) {
			t.Errorf("%q: %v", status, err)
		}
	}
}
