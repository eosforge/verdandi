package catalog

import (
	"testing"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

func TestPatchCapacityUsesFinalStateIndependentOfFieldOrder(t *testing.T) {
	fields := verdandi.Fields{"a": []byte("xxxxx"), "b": []byte("y")}
	for _, shape := range [][3]string{{"map", "a", "b"}, {"array", "0", "1"}} {
		kind, first, second := shape[0], shape[1], shape[2]
		updates := verdandi.Fields{first: []byte("xxxxx"), second: []byte("y")}
		for _, names := range [][]string{{first, second}, {second, first}} {
			old := map[string]string{first: "x", second: "yyyyy"}
			reply := []any{"1", kind, "8", old[names[0]], old[names[1]]}
			actual, err := projectPatchReply(reply, 1, names, updates, 8)
			if err != nil || actual != 8 {
				t.Errorf("%s order %v: legal size swap rejected: %d, %v", kind, names, actual, err)
			}
		}
	}
	if _, err := projectPatchReply([]any{"1", "map", "8", "x", "yyyyy"}, 1, []string{"a", "b"},
		verdandi.Fields{"a": []byte("xxxxxx"), "b": []byte("y")}, 8); !verdandi.IsCode(err, verdandi.CodeCapacity) {
		t.Fatalf("true final overflow must remain capacity: %v", err)
	}
	if _, err := projectPatchReply([]any{"1", "map", "2", "xxxx", "y"}, 1, []string{"a", "b"}, fields, 8); !verdandi.IsCode(err, verdandi.CodeCorrupt) {
		t.Fatalf("inconsistent stored byte count must be corrupt: %v", err)
	}
}

func TestPatchDistinguishesMissingAndCorruptHeaders(t *testing.T) {
	for _, test := range []struct {
		values []any
		code   verdandi.Code
	}{
		{[]any{nil, nil, nil, nil}, verdandi.CodeStale},
		{[]any{nil, nil, nil, "orphan"}, verdandi.CodeCorrupt},
		{[]any{"1", nil, nil, nil}, verdandi.CodeCorrupt},
		{[]any{nil}, verdandi.CodeCorrupt},
		{[]any{"1", "unknown", "1", ""}, verdandi.CodeCorrupt},
		{[]any{"1", "value", "1", ""}, verdandi.CodeTransition},
	} {
		_, err := projectPatchReply(test.values, 1, []string{"x"}, verdandi.Fields{"x": {}}, 8)
		if !verdandi.IsCode(err, test.code) {
			t.Errorf("header %v: got %v, want %v", test.values, err, test.code)
		}
	}
}
