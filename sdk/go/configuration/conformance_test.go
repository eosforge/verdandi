package configuration

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

func TestConfigurationConformanceCorpus(t *testing.T) {
	t.Parallel()
	content, err := os.ReadFile(filepath.Join("..", "..", "..", "testkit", "conformance", "v1", "configuration.json"))
	if err != nil {
		t.Fatal(err)
	}
	var corpus struct {
		Cases []struct {
			Name     string          `json:"name"`
			Valid    bool            `json:"valid"`
			Field    string          `json:"field"`
			Code     verdandi.Code   `json:"code"`
			Document json.RawMessage `json:"document"`
		} `json:"cases"`
		RawCases []struct {
			Name      string        `json:"name"`
			Valid     bool          `json:"valid"`
			Field     string        `json:"field"`
			Code      verdandi.Code `json:"code"`
			Source    string        `json:"source"`
			SourceHex string        `json:"source_hex"`
		} `json:"raw_cases"`
	}
	if err := json.Unmarshal(content, &corpus); err != nil {
		t.Fatal(err)
	}
	for _, test := range corpus.Cases {
		test := test
		t.Run(test.Name, func(t *testing.T) {
			t.Parallel()
			assertConformance(t, test.Valid, test.Code, test.Field, test.Document)
		})
	}
	for _, test := range corpus.RawCases {
		test := test
		t.Run(test.Name, func(t *testing.T) {
			t.Parallel()
			source := []byte(test.Source)
			if test.SourceHex != "" {
				var err error
				source, err = hex.DecodeString(test.SourceHex)
				if err != nil {
					t.Fatal(err)
				}
			}
			assertConformance(t, test.Valid, test.Code, test.Field, source)
		})
	}
}

func assertConformance(t *testing.T, valid bool, code verdandi.Code, field string, source []byte) {
	t.Helper()
	_, err := ParseJSON(source)
	if valid {
		if err != nil {
			t.Fatal(err)
		}
		return
	}
	var actual *verdandi.Error
	if !errors.As(err, &actual) || actual.Code != code || actual.Field != field {
		t.Fatalf("error = %#v, want %q field %q", err, code, field)
	}
}
