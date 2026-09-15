package admission

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
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

// 公开夹具保持原始密码及哈希, 产品更名不重签 TLS 证书或改写历史测试身份.
const fixturePassword = "verdandi-public-test-only"

func fixture(role string) string {
	return filepath.Join("..", "..", "..", "cluster", "tests", "fixtures", role)
}
func testServer(t *testing.T) (*Server, string, context.CancelFunc, <-chan error) {
	t.Helper()
	authority, err := Load(fixture("supervisor"))
	if err != nil {
		t.Fatal(err)
	}
	store, err := membership.Open(filepath.Join(t.TempDir(), "members.db"), 8, membership.DefaultMaximumStarts)
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
	// race 插桩使固定 KDF 明显变慢, 本夹具使用十五秒; 真实进程回归仍调用固定五秒的 Serve.
	go func() { result <- server.serve(ctx, listener, 15*time.Second); close(done) }()
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
	requestID := make([]byte, 32)
	rand.Read(requestID)
	return &wire.RegistrationRequest{RequestId: requestID, Role: wire.Role_ROLE_STAR, Group: "default", ClusterId: "alpha",
		Advertise: fmt.Sprintf("127.0.0.1:%d", 12000+index)}
}
func exchange(address, role string, r *wire.RegistrationRequest) (proto.Message, error) {
	connection, err := connect(address, role)
	if err != nil {
		return nil, err
	}
	defer connection.Close()
	if r.Username == "" {
		r.Username = "stars"
		if strings.HasPrefix(role, "planet") {
			r.Username = "planets"
		}
	}
	if r.Password == "" {
		r.Password = fixturePassword
	}
	// 单 RPC 保留 race 插桩的测试预算, 不改变生产握手期限.
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	client := wire.NewAdmissionClient(connection)
	reply, err := client.Register(ctx, r)
	if err != nil {
		return nil, err
	}
	return reply, nil
}
func TestSameAccountNodesReceiveIndependentVerifiableCredentials(t *testing.T) {
	_, address, _, _ := testServer(t)
	initial := request(1)
	first, err := exchange(address, "star-a", initial)
	if err != nil {
		t.Fatal(err)
	}
	response := first.(*wire.RegistrationResponse)
	var local wire.Member
	if err := proto.Unmarshal(response.Admission, &local); err != nil {
		t.Fatal(err)
	}
	public, err := os.ReadFile(filepath.Join(fixture("star-a"), "admission.pub"))
	if err != nil {
		t.Fatal(err)
	}
	if !ed25519.Verify(public, append([]byte(SignatureDomain), response.Admission...), response.Signature) {
		t.Fatal("invalid signature")
	}
	if local.Epoch != 1 || len(response.Members) != 1 || !proto.Equal(&local, response.Members[0]) {
		t.Fatal("credential and list disagree")
	}
	second, err := exchange(address, "star-a", request(2))
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
	replay, err := exchange(address, "star-a", initial)
	if err != nil {
		t.Fatal(err)
	}
	if len(replay.(*wire.RegistrationResponse).Members) != 2 || string(replay.(*wire.RegistrationResponse).Admission) != string(response.Admission) {
		t.Fatal("retry changed credential")
	}
}

// 错误码语义直接调用 handler, 不将 race 插桩下固定 KDF 的速度当作认证语义.
// 真实 TLS 拒绝和服务端截止另由网络/进程场景验证, 生产 5 秒期限保持不变.
func TestLoginAndRoleFailClosed(t *testing.T) {
	server, _, _, _ := testServer(t)
	for _, test := range []struct {
		name   string
		change func(*wire.RegistrationRequest)
		code   codes.Code
	}{
		{"password", func(r *wire.RegistrationRequest) { r.Password = "wrong" }, codes.Unauthenticated},
		{"unknown-account", func(r *wire.RegistrationRequest) { r.Username = "unknown" }, codes.Unauthenticated},
		{"cluster", func(r *wire.RegistrationRequest) { r.ClusterId = "beta" }, codes.InvalidArgument},
		{"request-id", func(r *wire.RegistrationRequest) { r.RequestId = []byte("bad") }, codes.InvalidArgument},
		{"wildcard", func(r *wire.RegistrationRequest) { r.Advertise = "0.0.0.0:7443" }, codes.InvalidArgument},
		{"role", func(r *wire.RegistrationRequest) { r.Role = wire.Role_ROLE_PLANET }, codes.PermissionDenied},
	} {
		t.Run(test.name, func(t *testing.T) {
			r := request(1)
			r.Username, r.Password = "stars", fixturePassword
			test.change(r)
			_, err := server.Register(context.Background(), r)
			if status.Code(err) != test.code {
				t.Fatalf("got %v", err)
			}
		})
	}
}
func TestOldRequestCannotReplaceRestartOnSameEndpoint(t *testing.T) {
	_, address, _, _ := testServer(t)
	first := request(1)
	if _, err := exchange(address, "star-a", first); err != nil {
		t.Fatal(err)
	}
	// 丢弃已经成功提交的响应, 使用原始请求重试必须幂等.
	if _, err := exchange(address, "star-a", first); err != nil {
		t.Fatal(err)
	}
	next := request(2)
	next.Advertise = first.Advertise
	reply, err := exchange(address, "star-a", next)
	if err != nil {
		t.Fatal(err)
	}
	members := reply.(*wire.RegistrationResponse).Members
	if len(members) != 1 || members[0].Epoch != 2 {
		t.Fatal("restart did not fence old instance")
	}
	if _, err := exchange(address, "star-a", first); status.Code(err) != codes.Aborted {
		t.Fatal("stale request accepted", err)
	}
}
func TestConcurrentSameStartupHasOneIdentity(t *testing.T) {
	_, address, _, _ := testServer(t)
	original := request(1)
	var workers sync.WaitGroup
	results := make(chan *wire.RegistrationResponse, 2)
	failures := make(chan error, 2)
	for range 2 {
		r := proto.Clone(original).(*wire.RegistrationRequest)
		workers.Go(func() {
			reply, err := exchange(address, "star-a", r)
			failures <- err
			if err == nil {
				results <- reply.(*wire.RegistrationResponse)
			}
		})
	}
	workers.Wait()
	close(failures)
	close(results)
	for err := range failures {
		if err != nil {
			t.Fatal(err)
		}
	}
	var first *wire.RegistrationResponse
	for reply := range results {
		if first == nil {
			first = reply
		} else if !proto.Equal(first, reply) {
			t.Fatal("concurrent retry created two identities")
		}
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
	if _, err := exchange(address, "star-a", r); status.Code(err) != codes.ResourceExhausted {
		t.Fatal("oversize accepted", err)
	}
	if _, err := exchange(address, "rogue", request(1)); err == nil {
		t.Fatal("untrusted server accepted")
	}
}

// 真实 RPC 中错误密码可能先命中服务端期限; 两者均必须拒绝且不产生任何成员.
func TestInvalidPasswordOverTLSNeverRegisters(t *testing.T) {
	server, address, _, _ := testServer(t)
	original := request(1)
	original.Password = "wrong"
	reply, err := exchange(address, "star-a", original)
	if reply != nil || (status.Code(err) != codes.Unauthenticated && status.Code(err) != codes.DeadlineExceeded) {
		t.Fatal("invalid password was not rejected", err)
	}
	// 随后的第一次合法登记仍必须是 epoch 1, 证明被拒绝请求没有消耗成员代次.
	original.Username, original.Password = "stars", fixturePassword
	result, err := server.Register(context.Background(), original)
	if err != nil || len(result.Members) != 1 || result.Members[0].Epoch != 1 {
		t.Fatal("rejected login modified membership", err)
	}
}
