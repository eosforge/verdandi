package registration

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestEmbeddedRegistrationScriptsMatchCanonicalSources(t *testing.T) {
	t.Parallel()
	scripts := []struct {
		kind     string
		embedded string
	}{
		{kind: "register", embedded: registrationRegisterLua},
		{kind: "update", embedded: registrationUpdateLua},
		{kind: "renew", embedded: registrationRenewLua},
		{kind: "unregister", embedded: registrationUnregisterLua},
	}
	for _, script := range scripts {
		t.Run(script.kind, func(t *testing.T) {
			t.Parallel()
			canonical, err := os.ReadFile(filepath.Join("..", "..", "..", "lua", "registration", script.kind+".lua"))
			if err != nil {
				t.Fatal(err)
			}
			if string(canonical) != script.embedded {
				t.Fatalf("embedded Registration %s script differs from canonical repository source", script.kind)
			}
		})
	}
}

func TestRegistrationArgumentsCanonicalOrder(t *testing.T) {
	t.Parallel()
	actual := registerArguments(
		"0123456789abcdef0123456789abcdef",
		7,
		15000,
		3,
		Fields{"z": []byte("last"), "a": []byte("first")},
		Fields{"load": []byte("1"), "address": []byte("127.0.0.1")},
	)
	expected := []any{
		"0123456789abcdef0123456789abcdef",
		"7",
		"15000",
		"3",
		".a", []byte("first"),
		".z", []byte("last"),
		"address", []byte("127.0.0.1"),
		"load", []byte("1"),
	}
	if !reflect.DeepEqual(actual, expected) {
		t.Fatalf("registerArguments() = %#v, want %#v", actual, expected)
	}

	update := updateArguments(
		"0123456789abcdef0123456789abcdef",
		8,
		false,
		3,
		Fields{"load": []byte("2")},
	)
	expectedUpdate := []any{
		"0123456789abcdef0123456789abcdef", "8", "", "load", []byte("2"),
	}
	if !reflect.DeepEqual(update, expectedUpdate) {
		t.Fatalf("updateArguments() = %#v, want %#v", update, expectedUpdate)
	}
	if version := updateArguments("0123456789abcdef0123456789abcdef", 9, true, 4, nil); version[2] != "4" {
		t.Fatalf("version update slot = %#v, want 4", version[2])
	}
	if renew := renewArguments("0123456789abcdef0123456789abcdef", 9); !reflect.DeepEqual(renew, []any{"0123456789abcdef0123456789abcdef", "9"}) {
		t.Fatalf("renewArguments() = %#v", renew)
	}
	if unregister := unregisterArguments("0123456789abcdef0123456789abcdef"); !reflect.DeepEqual(unregister, []any{"0123456789abcdef0123456789abcdef"}) {
		t.Fatalf("unregisterArguments() = %#v", unregister)
	}
}

func TestParseRegistrationReply(t *testing.T) {
	t.Parallel()
	reply, err := parseRegistrationReply([]any{"&result", "ok", "@revision", int64(4), "@timestamp", int64(9)})
	if err != nil || reply.revision != 4 || reply.timestamp != 9 {
		t.Fatalf("parseRegistrationReply() = %#v, %v", reply, err)
	}
	_, err = parseRegistrationReply([]any{
		"&result", "error", "&status", "stale", "&field", "@revision", "@revision", int64(7),
	})
	if !IsCode(err, CodeStale) {
		t.Fatalf("parseRegistrationReply() error = %v", err)
	}
}

func TestRegistrationSuccessValidationAndUncertainOutcomes(t *testing.T) {
	t.Parallel()
	if err := validateRegistrationSuccess(registrationReply{revision: 4, timestamp: 9}, 4); err != nil {
		t.Fatalf("valid Registration reply error = %v", err)
	}
	for _, reply := range []registrationReply{
		{revision: 3, timestamp: 9},
		{revision: 4, timestamp: 0},
	} {
		if err := validateRegistrationSuccess(reply, 4); !IsCode(err, CodeCorrupt) {
			t.Fatalf("validateRegistrationSuccess(%#v) error = %v", reply, err)
		}
	}
	for _, code := range []Code{CodeAmbiguous, CodeCorrupt} {
		if !uncertainRegistrationOutcome(protocolError(code, "reply", 0)) {
			t.Fatalf("%s outcome was not treated as uncertain", code)
		}
	}
	if uncertainRegistrationOutcome(protocolError(CodeContract, "update", 0)) {
		t.Fatal("contract outcome was treated as uncertain")
	}
}

func TestFieldsEqualDistinguishesMissingEmptyValues(t *testing.T) {
	t.Parallel()
	if fieldsEqual(Fields{"left": {}}, Fields{"right": {}}) {
		t.Fatal("different field names with empty values compared equal")
	}
	if !fieldsEqual(Fields{"same": {}}, Fields{"same": {}}) {
		t.Fatal("identical empty field values compared unequal")
	}
}
