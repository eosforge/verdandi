package admission

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
	"github.com/eosforge/verdandi/supervisor/internal/membership"
)

// TestProtocolOwnership 锁定协议所有者和公开 RPC 路径, 避免两端同时生成错误包名而掩盖契约漂移.
func TestProtocolOwnership(t *testing.T) {
	if wire.File_orbit_proto.Package() != "proto.orbit.v1" || wire.File_astra_proto.Package() != "proto.astra.v1" || wire.File_comet_proto.Package() != "proto.comet.v1" {
		t.Fatal("unexpected protocol owner")
	}
	if (&wire.Member{}).ProtoReflect().Descriptor().FullName() != "proto.orbit.v1.Member" || wire.File_astra_proto.Messages().ByName("Member") != nil {
		t.Fatal("Member must be defined only by Orbit")
	}
	if wire.Admission_Register_FullMethodName != "/proto.orbit.v1.Admission/Register" || wire.StarTransport_OpenSession_FullMethodName != "/proto.astra.v1.StarTransport/OpenSession" {
		t.Fatal("unexpected v1 RPC path")
	}
	if SignatureDomain != "proto.orbit.v1.admission\x00" {
		t.Fatal("admission signing purpose changed")
	}
	if wire.File_comet_proto.Messages().Len() != 0 || wire.File_comet_proto.Services().Len() != 0 {
		t.Fatal("Comet interfaces require a separate SDK design")
	}
}

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
			// 共享历史向量的字段仍叫 cluster, 与生成协议的 Galaxy 字段名无关.
			Galaxy  string `json:"cluster"`
			Address string `json:"address"`
			SHA256  string `json:"sha256"`
		} `json:"principals"`
	}
	data, err := os.ReadFile(filepath.Join(fixture(""), "admission-v1.json"))
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
		if endpointPrincipal(entry.Username, entry.Galaxy, entry.Address) != entry.SHA256 {
			t.Fatal("endpoint principal encoding changed")
		}
	}
}
