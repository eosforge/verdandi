package admission

import (
	"context"
	"crypto/ed25519"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
	"github.com/eosforge/verdandi/supervisor/internal/membership"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

const fixturePassword = "verdandi-public-test-only"

func fixture(role string) string {
	return filepath.Join("..", "..", "..", "peer", "tests", "fixtures", role)
}
func testServer(t *testing.T) (*Server, string, context.CancelFunc, <-chan error) {
	t.Helper()
	authority, err := Load(fixture("supervisor"))
	if err != nil {
		t.Fatal(err)
	}
	store, err := membership.Open(filepath.Join(t.TempDir(), "members.db"), 8)
	if err != nil {
		t.Fatal(err)
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	server := &Server{Cluster: "alpha", Authority: authority, Store: store, MaximumConnections: 4, Logger: slog.New(slog.NewTextHandler(io.Discard, nil))}
	result, done := make(chan error, 1), make(chan struct{})
	go func() { result <- server.Serve(ctx, listener); close(done) }()
	t.Cleanup(func() {
		cancel()
		select {
		case <-done:
		case <-time.After(3 * time.Second):
			t.Error("server did not stop")
		}
		_ = store.Close()
	})
	return server, listener.Addr().String(), cancel, result
}
func connect(address, role string) (*grpc.ClientConn, error) {
	name := "ca.pem"
	if role == "rogue" {
		name = "cert.pem"
	}
	ca, err := os.ReadFile(filepath.Join(fixture(role), name))
	if err != nil {
		return nil, err
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(ca) {
		return nil, errors.New("invalid fixture CA")
	}
	// 不提供客户端证书, 覆盖账号模式不再依赖 mTLS 的真实传输.
	return grpc.NewClient(address, grpc.WithTransportCredentials(credentials.NewTLS(&tls.Config{RootCAs: roots, MinVersion: tls.VersionTLS13, MaxVersion: tls.VersionTLS13})))
}
func request(index int) *wire.RegistrationRequest {
	return &wire.RegistrationRequest{Role: wire.NodeRole_NODE_ROLE_STAR, Group: "default", ClusterId: "alpha",
		PeerId: fmt.Sprintf("%08x000040008000000000000000", index), Advertise: fmt.Sprintf("127.0.0.1:%d", 12000+index)}
}
func exchange(address, role string, r *wire.RegistrationRequest) (proto.Message, error) {
	connection, err := connect(address, role)
	if err != nil {
		return nil, err
	}
	defer connection.Close()
	r = proto.Clone(r).(*wire.RegistrationRequest)
	if r.Username == "" {
		r.Username = "stars"
		if strings.HasPrefix(role, "planet") {
			r.Username = "planets"
		}
	}
	if r.Password == "" {
		r.Password = fixturePassword
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	reply, err := wire.NewAdmissionClient(connection).Register(ctx, r)
	if err != nil {
		return nil, err
	}
	return reply, nil
}
func TestSameAccountNodesReceiveIndependentVerifiableCredentials(t *testing.T) {
	_, address, _, _ := testServer(t)
	first, err := exchange(address, "peer-a", request(1))
	if err != nil {
		t.Fatal(err)
	}
	response := first.(*wire.RegistrationResponse)
	var local wire.RegistrationResponse_Member
	if err := proto.Unmarshal(response.Admission, &local); err != nil {
		t.Fatal(err)
	}
	public, err := os.ReadFile(filepath.Join(fixture("peer-a"), "admission.pub"))
	if err != nil {
		t.Fatal(err)
	}
	if !ed25519.Verify(public, append([]byte(SignatureDomain), response.Admission...), response.Signature) {
		t.Fatal("invalid signature")
	}
	if local.Epoch != 1 || len(response.Members) != 1 || !proto.Equal(&local, response.Members[0]) {
		t.Fatal("credential and list disagree")
	}
	second, err := exchange(address, "peer-a", request(2))
	if err != nil {
		t.Fatal(err)
	}
	next := second.(*wire.RegistrationResponse)
	if len(next.Members) != 2 || next.Members[0].Principal == next.Members[1].Principal {
		t.Fatal("same account replaced another endpoint")
	}
	if strings.Contains(string(next.Admission), fixturePassword) {
		t.Fatal("password leaked in credential")
	}
	replay, err := exchange(address, "peer-a", request(1))
	if err != nil {
		t.Fatal(err)
	}
	if len(replay.(*wire.RegistrationResponse).Members) != 2 || string(replay.(*wire.RegistrationResponse).Admission) != string(response.Admission) {
		t.Fatal("retry changed credential")
	}
}
func TestLoginAndRoleFailClosed(t *testing.T) {
	_, address, _, _ := testServer(t)
	for _, test := range []struct {
		name   string
		change func(*wire.RegistrationRequest)
		code   codes.Code
	}{
		{"password", func(r *wire.RegistrationRequest) { r.Password = "wrong" }, codes.Unauthenticated},
		{"unknown-account", func(r *wire.RegistrationRequest) { r.Username = "unknown" }, codes.Unauthenticated},
		{"cluster", func(r *wire.RegistrationRequest) { r.ClusterId = "beta" }, codes.InvalidArgument},
		{"uuid", func(r *wire.RegistrationRequest) { r.PeerId = "bad" }, codes.InvalidArgument},
		{"wildcard", func(r *wire.RegistrationRequest) { r.Advertise = "0.0.0.0:7443" }, codes.InvalidArgument},
		{"role", func(r *wire.RegistrationRequest) { r.Role = wire.NodeRole_NODE_ROLE_PLANET }, codes.PermissionDenied},
	} {
		t.Run(test.name, func(t *testing.T) {
			r := request(1)
			test.change(r)
			_, err := exchange(address, "peer-a", r)
			if status.Code(err) != test.code {
				t.Fatalf("got %v", err)
			}
		})
	}
}
func TestChallengeRequiresLoginAndTracksEndpointEpoch(t *testing.T) {
	_, address, _, _ := testServer(t)
	connection, err := connect(address, "peer-a")
	if err != nil {
		t.Fatal(err)
	}
	defer connection.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	client := wire.NewAdmissionClient(connection)
	login := &wire.LoginRequest{Username: "stars", Password: "wrong", ClusterId: "alpha", Advertise: request(1).Advertise}
	if _, err := client.Challenge(ctx, login); status.Code(err) != codes.Unauthenticated {
		t.Fatal("unauthenticated baseline", err)
	}
	login.Password = fixturePassword
	baseline, err := client.Challenge(ctx, login)
	if err != nil || baseline.ExpectedEpoch != 0 {
		t.Fatal("initial baseline", err)
	}
	r := request(1)
	r.Username, r.Password = login.Username, login.Password
	if _, err := client.Register(ctx, r); err != nil {
		t.Fatal(err)
	}
	baseline, err = client.Challenge(ctx, login)
	if err != nil || baseline.ExpectedEpoch != 1 {
		t.Fatal("committed baseline", err)
	}
}
func TestOldRequestCannotReplaceRestartOnSameEndpoint(t *testing.T) {
	_, address, _, _ := testServer(t)
	first := request(1)
	if _, err := exchange(address, "peer-a", first); err != nil {
		t.Fatal(err)
	}
	// 丢弃已经成功提交的响应, 使用原始请求重试必须幂等.
	if _, err := exchange(address, "peer-a", first); err != nil {
		t.Fatal(err)
	}
	next := request(2)
	next.Advertise = first.Advertise
	next.ExpectedEpoch = 1
	reply, err := exchange(address, "peer-a", next)
	if err != nil {
		t.Fatal(err)
	}
	members := reply.(*wire.RegistrationResponse).Members
	if len(members) != 1 || members[0].Epoch != 2 {
		t.Fatal("restart did not fence old instance")
	}
	if _, err := exchange(address, "peer-a", first); status.Code(err) != codes.Aborted {
		t.Fatal("stale request accepted", err)
	}
}
func TestConcurrentSameEndpointCASHasOneWinner(t *testing.T) {
	_, address, _, _ := testServer(t)
	var workers sync.WaitGroup
	results := make(chan codes.Code, 2)
	for index := 1; index <= 2; index++ {
		workers.Go(func() {
			r := request(index)
			r.Advertise = request(1).Advertise
			_, err := exchange(address, "peer-a", r)
			results <- status.Code(err)
		})
	}
	workers.Wait()
	close(results)
	counts := make(map[codes.Code]int)
	for result := range results {
		counts[result]++
	}
	if counts[codes.OK] != 1 || counts[codes.Aborted] != 1 {
		t.Fatal(counts)
	}
}
func TestCancellationReleasesSlowConnectionsAndListener(t *testing.T) {
	_, address, cancel, result := testServer(t)
	for range 5 {
		connection, err := net.DialTimeout("tcp", address, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = connection.Close() })
	}
	cancel()
	select {
	case err := <-result:
		if err != nil && !errors.Is(err, net.ErrClosed) {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("slow connection blocked shutdown")
	}
	rebound, err := net.Listen("tcp", address)
	if err != nil {
		t.Fatal(err)
	}
	_ = rebound.Close()
}
func TestOversizedRequestAndUntrustedServerFail(t *testing.T) {
	_, address, _, _ := testServer(t)
	r := request(1)
	r.Group = strings.Repeat("x", 8192)
	if _, err := exchange(address, "peer-a", r); status.Code(err) != codes.ResourceExhausted {
		t.Fatal("oversize accepted", err)
	}
	if _, err := exchange(address, "rogue", request(1)); err == nil {
		t.Fatal("untrusted server accepted")
	}
}
