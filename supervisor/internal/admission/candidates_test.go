package admission

import (
	"fmt"
	"github.com/eosforge/verdandi/supervisor/internal/membership"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"testing"

	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
	"google.golang.org/protobuf/proto"
)

func TestPlanetResponseIsSeparateSignedAndDoesNotJoinStarList(t *testing.T) {
	server, address, _, _ := testServer(t)
	for index, identity := range []string{"star-a", "star-b"} {
		r := request(index + 1)
		r.Group = []string{"local", "remote"}[index]
		if reply, err := exchange(address, identity, r); err != nil {
			t.Fatal(err)
		} else if _, ok := reply.(*wire.RegistrationResponse); !ok {
			t.Fatalf("Star registration failed: %v", reply)
		}
	}
	r := request(10)
	r.Role, r.Group = wire.Role_ROLE_PLANET, "local"
	reply, err := exchange(address, "planet-a", r)
	response, ok := reply.(*wire.RegistrationResponse)
	if err != nil || !ok || len(response.Members) != 2 || response.Members[0].Group != "local" {
		t.Fatalf("invalid Planet response: %v %v", reply, err)
	}
	var local wire.RegistrationResponse_Member
	if err := proto.Unmarshal(response.Admission, &local); err != nil || local.Role != r.Role || local.Group != r.Group || !membership.ID(local.Id) {
		t.Fatal("Planet credential lost role, group or process binding")
	}
	_, signature, err := server.Authority.Sign(&local)
	if err != nil || string(signature) != string(response.Signature) {
		t.Fatal("invalid Planet credential signature")
	}
	// 同一进程刷新名单保持 epoch 与准入正文, 不重新占据部署名额.
	r.CandidateRound++
	refreshed, err := exchange(address, "planet-a", r)
	next, ok := refreshed.(*wire.RegistrationResponse)
	if err != nil || !ok || string(next.Admission) != string(response.Admission) {
		t.Fatal("candidate refresh changed admission")
	}
	star, err := exchange(address, "star-c", request(3))
	list, ok := star.(*wire.RegistrationResponse)
	if err != nil || !ok || len(list.Members) != 3 {
		t.Fatalf("Planet polluted Star list: %v %v", star, err)
	}
	for _, member := range list.Members {
		if member.Role != wire.Role_ROLE_STAR {
			t.Fatal("non-Star in full mesh list")
		}
	}
}

func TestAccountsCannotClaimTheOtherRole(t *testing.T) {
	_, address, _, _ := testServer(t)
	for _, test := range []struct {
		identity string
		role     wire.Role
	}{
		{"planet-a", wire.Role_ROLE_STAR}, {"star-a", wire.Role_ROLE_PLANET},
		{"star-a", wire.Role_ROLE_UNSPECIFIED}, {"star-a", wire.Role(999)},
	} {
		r := request(1)
		r.Role = test.role
		reply, err := exchange(address, test.identity, r)
		if status.Code(err) != codes.PermissionDenied && status.Code(err) != codes.InvalidArgument {
			t.Fatalf("role escalation: %v %v", reply, err)
		}
	}
}

func TestCandidateRoundsAreBoundedPrioritizedAndEventuallyCoverEveryStar(t *testing.T) {
	stars := make([]*wire.RegistrationResponse_Member, 0, 31)
	for index := range 31 {
		group := "local"
		if index >= 19 {
			group = "remote"
		}
		stars = append(stars, &wire.RegistrationResponse_Member{Id: fmt.Sprintf("%032x", index), Group: group, Role: wire.Role_ROLE_STAR})
	}
	seen := make(map[string]bool)
	for round := range uint32(31) {
		batch := candidates(stars, "local", "process", round)
		if len(batch) != 8 {
			t.Fatal("candidate response not bounded to eight")
		}
		unique := make(map[string]bool)
		localCount := 0
		for index, member := range batch {
			if unique[member.Id] {
				t.Fatal("duplicate candidate")
			}
			unique[member.Id] = true
			seen[member.Id] = true
			if member.Group == "local" {
				localCount++
			}
			if index > 0 && ((batch[index-1].Group == "remote" && member.Group == "local") || (batch[index-1].Group == member.Group && batch[index-1].Id >= member.Id)) {
				t.Fatal("noncanonical candidate order")
			}
		}
		if localCount != 4 {
			t.Fatal("missing local or cross-group reserve")
		}
	}
	if len(seen) != len(stars) {
		t.Fatal("candidate rounds permanently omit Stars")
	}
	if len(candidates(nil, "local", "process", 0)) != 0 {
		t.Fatal("fabricated empty-cluster candidate")
	}
}
