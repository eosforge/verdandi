package configuration

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
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
			Document json.RawMessage `json:"document"`
		} `json:"cases"`
	}
	if err := json.Unmarshal(content, &corpus); err != nil {
		t.Fatal(err)
	}
	for _, test := range corpus.Cases {
		test := test
		t.Run(test.Name, func(t *testing.T) {
			t.Parallel()
			_, err := ParseJSON(test.Document)
			if test.Valid {
				if err != nil {
					t.Fatal(err)
				}
				return
			}
			var actual *verdandi.Error
			if !errors.As(err, &actual) || actual.Field != test.Field {
				t.Fatalf("error = %#v, want field %q", err, test.Field)
			}
		})
	}
}
