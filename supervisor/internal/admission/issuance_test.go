// 验证单次登记的幂等绑定, 旧请求隔离和服务端签发身份.
package admission

import (
	"context"
	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
	"github.com/eosforge/verdandi/supervisor/internal/membership"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
	"testing"
)

func TestCommittedRequestCannotChangeRegistrationBinding(t *testing.T) {
	server, _, _, _ := testServer(t)
	original := request(1)
	original.Username, original.Password = "stars", fixturePassword
	first, err := server.Register(context.Background(), original)
	if err != nil {
		t.Fatal(err)
	}
	for name, mutate := range map[string]func(*wire.RegistrationRequest){
		"endpoint":        func(r *wire.RegistrationRequest) { r.Advertise = "127.0.0.1:12999" },
		"group":           func(r *wire.RegistrationRequest) { r.Group = "another" },
		"role":            func(r *wire.RegistrationRequest) { r.Role = wire.Role_ROLE_PLANET },
		"account":         func(r *wire.RegistrationRequest) { r.Username = "planets"; r.Role = wire.Role_ROLE_PLANET },
		"galaxy":         func(r *wire.RegistrationRequest) { r.Galaxy = "another" },
		"missing-request": func(r *wire.RegistrationRequest) { r.RequestId = nil },
	} {
		t.Run(name, func(t *testing.T) {
			r := proto.Clone(original).(*wire.RegistrationRequest)
			mutate(r)
			if _, err := server.Register(context.Background(), r); err == nil {
				t.Fatal("changed binding accepted")
			}
		})
	}
	again, err := server.Register(context.Background(), original)
	if err != nil || !proto.Equal(first, again) {
		t.Fatal("rejected request mutated original", err)
	}
	var member wire.Member
	if err := proto.Unmarshal(first.Admission, &member); err != nil || !membership.ID(member.Id) {
		t.Fatal("issuer did not provide identity", err)
	}
	if string(original.RequestId) == member.Id {
		t.Fatal("startup key became public identity")
	}
}

func TestNewRequestsSerializeAndSupersededRequestNeverReturns(t *testing.T) {
	server, _, _, _ := testServer(t)
	first := request(1)
	first.Username, first.Password = "stars", fixturePassword
	next := proto.Clone(first).(*wire.RegistrationRequest)
	next.RequestId = request(2).RequestId
	a, err := server.Register(context.Background(), first)
	if err != nil {
		t.Fatal(err)
	}
	b, err := server.Register(context.Background(), next)
	if err != nil {
		t.Fatal(err)
	}
	if a.Members[0].Id == b.Members[0].Id || b.Members[0].Epoch != a.Members[0].Epoch+1 {
		t.Fatal("replacement did not advance")
	}
	for range 3 {
		if _, err := server.Register(context.Background(), first); status.Code(err) != codes.Aborted {
			t.Fatal("old startup reclaimed slot", err)
		}
	}
}
