package admission

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/eosforge/verdandi/supervisor/internal/membership"
)

func TestSharedCppGoAdmissionVectors(t *testing.T) {
	type validation struct {
		Value string `json:"value"`
		Valid bool   `json:"valid"`
	}
	var vectors struct {
		Names      []validation `json:"names"`
		Addresses  []validation `json:"addresses"`
		IDs        []validation `json:"ids"`
		Principals []struct {
			Username string `json:"username"`
			Cluster  string `json:"cluster"`
			Address  string `json:"address"`
			SHA256   string `json:"sha256"`
		} `json:"principals"`
	}
	data, err := os.ReadFile(filepath.Join(fixture(""), "admission-v5.json"))
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(data, &vectors); err != nil {
		t.Fatal(err)
	}
	for _, group := range []struct {
		cases []validation
		check func(string) bool
	}{{vectors.Names, membership.Name}, {vectors.Addresses, membership.Address}, {vectors.IDs, membership.ID}} {
		if len(group.cases) == 0 {
			t.Fatal("missing shared vectors")
		}
		for _, entry := range group.cases {
			if group.check(entry.Value) != entry.Valid {
				t.Fatalf("unexpected validation for %q", entry.Value)
			}
		}
	}
	for _, entry := range vectors.Principals {
		if endpointPrincipal(entry.Username, entry.Cluster, entry.Address) != entry.SHA256 {
			t.Fatal("endpoint principal encoding changed")
		}
	}
}
