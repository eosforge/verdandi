package configuration

import "testing"

func TestJSONMemberNamesRejectCaseAndUnicodeFolding(t *testing.T) {
	for _, source := range []string{
		`{"VERSION":"v1"}`, `{"version":"v1","Redis":{}}`,
		`{"redis":{"ADDRESSes":[]}}`, `{"redis":{"addreſſes":[]}}`,
		`{"catalog":{"checKpoint":{}}}`, `{"redis":{"auth":{"UserName":"x"}}}`,
	} {
		if err := rejectDuplicateFields([]byte(source)); err == nil {
			t.Errorf("noncanonical member accepted: %s", source)
		}
	}
	for _, source := range []string{
		`{"version":"v1","redis":{"addresses":["localhost:6379"]}}`,
		`{"redis":{"auth":{"username":"中文 K ſ A"}}}`, `{"sync_timeout_ms":100}`,
	} {
		if err := rejectDuplicateFields([]byte(source)); err != nil {
			t.Errorf("canonical members rejected: %s: %v", source, err)
		}
	}
}
