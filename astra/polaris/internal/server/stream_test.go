//go:build linux && cgo

package server

import (
	"context"
	"crypto/ed25519"
	"crypto/x509"
	"encoding/pem"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/eosforge/verdandi/astra/internal/admission"
	"github.com/eosforge/verdandi/astra/internal/generated/astra"
	"github.com/eosforge/verdandi/astra/internal/generated/comet"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"github.com/eosforge/verdandi/astra/internal/generated/polaris"
	"github.com/eosforge/verdandi/astra/polaris/internal/storage"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// issuer 只在隔离测试端口签发公开测试材料, 不替代生产 Pulsar 持久事务测试.
type issuer struct {
	orbit.UnimplementedAdmissionServer
	identity *admission.Identity
	key      ed25519.PrivateKey
}

// sign 固定使用生产签名域, 测试不同传输与角色共享同一准入格式.
func (issuer *issuer) sign(member *orbit.Member) *astra.Hello {
	data, err := proto.Marshal(member)
	if err != nil {
		panic(err)
	}
	return &astra.Hello{ProtocolMajor: 1, MaxFrameBytes: 8 << 20, Admission: data, AdmissionSignature: ed25519.Sign(issuer.key, append([]byte("proto.orbit.v1.admission\x00"), data...))}
}

// Register 回应测试节点的真实部署绑定, 不让接收端跳过 TLS 或准入验签.
func (issuer *issuer) Register(_ context.Context, request *orbit.RegistrationRequest) (*orbit.RegistrationResponse, error) {
	member := &orbit.Member{Galaxy: request.Galaxy, Id: "polaris", Principal: issuer.identity.Principal(request.Galaxy, request.Advertise), Epoch: 1, Advertise: request.Advertise, Role: request.Role, Group: request.Group}
	hello := issuer.sign(member)
	return &orbit.RegistrationResponse{Members: []*orbit.Member{member}, Admission: hello.Admission, Signature: hello.AdmissionSignature}, nil
}

// system 拥有每例的真实 TLS/gRPC 连接和所有退出资源, 不共享端口或后台任务.
type system struct {
	store   *storage.Store
	issuer  *issuer
	node    *admission.Node
	streams *Streams
	channel *grpc.ClientConn
	star    *astra.Hello
	manager *astra.Hello
}

// environment 使用仓库公开身份和独立 SQLite 文件, 返回前完成实际准入但不修改系统设置.
func environment(t *testing.T, limits storage.Limits) *system {
	t.Helper()
	root := filepath.Join("..", "..", "..", "..", "cluster", "tests", "fixtures")
	identity, err := admission.Load(filepath.Join(root, "star-a"), "127.0.0.1:7443")
	if err != nil {
		t.Fatal(err)
	}
	encoded, err := os.ReadFile(filepath.Join(root, "supervisor", "admission.key"))
	if err != nil {
		t.Fatal(err)
	}
	block, _ := pem.Decode(encoded)
	if block == nil {
		t.Fatal("missing public fixture")
	}
	key, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		t.Fatal(err)
	}
	issuer := &issuer{identity: identity, key: key.(ed25519.PrivateKey)}
	listening, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	pulsar := grpc.NewServer(grpc.Creds(credentials.NewTLS(identity.Server())))
	orbit.RegisterAdmissionServer(pulsar, issuer)
	go func() { _ = pulsar.Serve(listening) }()
	t.Cleanup(func() { pulsar.Stop(); _ = listening.Close() })
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = listener.Close() })
	node, err := admission.Open(identity, listening.Addr().String(), "alpha", listener.Addr().String(), "default", orbit.Role_ROLE_POLARIS)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = node.Close() })
	ctx, cancel := context.WithTimeout(t.Context(), 5*time.Second)
	defer cancel()
	if err := node.Register(ctx); err != nil {
		t.Fatal(err)
	}
	store := fixture(t, limits)
	authority, err := NewAuthority(store, node.Directory(), 256<<20)
	if err != nil {
		t.Fatal(err)
	}
	streams := NewStreams(node, authority)
	rpc := grpc.NewServer(grpc.Creds(credentials.NewTLS(identity.Server())), grpc.MaxSendMsgSize(8<<20), grpc.MaxRecvMsgSize(8<<20))
	polaris.RegisterAuthorityServer(rpc, authority)
	polaris.RegisterAlmanacServer(rpc, streams)
	go func() { _ = rpc.Serve(listener) }()
	t.Cleanup(func() { rpc.Stop(); streams.Wait() })
	channel, err := grpc.NewClient(listener.Addr().String(), grpc.WithTransportCredentials(credentials.NewTLS(identity.Client())), grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(8<<20)))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = channel.Close() })
	member := &orbit.Member{Galaxy: "alpha", Id: "star", Principal: identity.Principal("alpha", "127.0.0.1:7447"), Epoch: 1, Advertise: "127.0.0.1:7447", Role: orbit.Role_ROLE_STAR, Group: "default"}
	star := issuer.sign(member)
	member.Id, member.Advertise, member.Principal, member.Role = "manager", "127.0.0.1:7448", identity.Principal("alpha", "127.0.0.1:7448"), orbit.Role_ROLE_ASTROLABE
	return &system{store: store, issuer: issuer, node: node, streams: streams, channel: channel, star: star, manager: issuer.sign(member)}
}

// connect 提交完整位置清单并读取对端 Hello, 后续帧留给具体场景验证安装边界.
func (system *system) connect(t *testing.T, inventory *polaris.Inventory) grpc.BidiStreamingClient[polaris.Packet, polaris.Packet] {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 10*time.Second)
	t.Cleanup(cancel)
	stream, err := polaris.NewAlmanacClient(system.channel).Open(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if err := stream.Send(&polaris.Packet{Body: &polaris.Packet_Hello{Hello: system.star}}); err != nil {
		t.Fatal(err)
	}
	if err := stream.Send(&polaris.Packet{Body: &polaris.Packet_Inventory{Inventory: inventory}}); err != nil {
		t.Fatal(err)
	}
	packet, err := stream.Recv()
	if err != nil || packet.GetHello() == nil {
		t.Fatal("missing signed Polaris Hello", err)
	}
	return stream
}

// TestStreamSnapshotSuffix 在首轮分片过程中推进并裁剪持久历史, 检查快照后缀仍连续而非重复全量.
func TestStreamSnapshotSuffix(t *testing.T) {
	t.Parallel()
	limits := storage.Default()
	limits.History = 1
	system := environment(t, limits)
	scope := storage.Scope{Sector: "routes", Spectrum: "main"}
	for version := storage.Version(1); version <= 40; version++ {
		if _, err := system.store.Commit(t.Context(), scope, storage.Change{Version: version, Key: fmt.Sprint(version), Value: make([]byte, 16384)}); err != nil {
			t.Fatal(err)
		}
	}
	stream := system.connect(t, &polaris.Inventory{Complete: true})
	var installed uint64
	var snapshot uint64
	count, changed, ready := 0, false, false
	for !ready || installed < 45 {
		packet, err := stream.Recv()
		if err != nil {
			t.Fatal(err)
		}
		switch body := packet.Body.(type) {
		case *polaris.Packet_Plan:
			if !body.Plan.Complete || len(body.Plan.Positions) != 1 || body.Plan.Positions[0].Version != 40 {
				t.Fatal("initial plan drifted")
			}
		case *polaris.Packet_Snapshot:
			page := body.Snapshot
			if page.Version == nil || page.GetVersion() != 40 {
				t.Fatal("snapshot version was not frozen")
			}
			snapshot = page.GetVersion()
			count += len(page.Entries)
			if !changed {
				for version := storage.Version(41); version <= 45; version++ {
					if _, err := system.store.Commit(t.Context(), scope, storage.Change{Version: version, Key: "new", Value: []byte{byte(version)}}); err != nil {
						t.Fatal(err)
					}
				}
				changed = true
			}
			if page.Complete {
				if count != 40 {
					t.Fatal("partial snapshot")
				}
				installed = snapshot
			}
		case *polaris.Packet_Updates:
			for _, patch := range body.Updates.Patches {
				if patch.Version != installed+1 {
					t.Fatal("suffix gap", installed, patch.Version)
				}
				installed = patch.Version
			}
		case *polaris.Packet_Ready:
			ready = true
		default:
			t.Fatal("unexpected frame")
		}
		if installed != 0 {
			if err := stream.Send(&polaris.Packet{Body: &polaris.Packet_Acknowledged{Acknowledged: &polaris.Position{Scope: &comet.Scope{Sector: scope.Sector, Spectrum: scope.Spectrum}, Version: installed}}}); err != nil {
				t.Fatal(err)
			}
		}
	}
}

// TestStreamUnknownEmptyScope 版本零同样必须属于持久权威范围, 不能由客户端凭空创建安装记录.
func TestStreamUnknownEmptyScope(t *testing.T) {
	t.Parallel()
	system := environment(t, storage.Default())
	stream := system.connect(t, &polaris.Inventory{Complete: true, Positions: []*polaris.Position{{Scope: &comet.Scope{Sector: "missing", Spectrum: "scope"}}}})
	_, err := stream.Recv()
	if status.Code(err) != codes.Aborted {
		t.Fatal("accepted unknown installed empty scope", err)
	}
}

// TestAuthorityRPC 验证身份隔离、真实持久确认、原请求确认及同版本冲突, 不按错误文本分类.
func TestAuthorityRPC(t *testing.T) {
	t.Parallel()
	system := environment(t, storage.Default())
	client := polaris.NewAuthorityClient(system.channel)
	ctx, cancel := context.WithTimeout(t.Context(), 5*time.Second)
	defer cancel()
	request := &polaris.CommitRequest{Scope: &comet.Scope{Sector: "s", Spectrum: "p"}, Version: 1, Change: &comet.AlmanacChange{Key: "k", Action: &comet.AlmanacChange_Value{Value: []byte{1}}}}
	if _, err := client.Commit(ctx, request); status.Code(err) != codes.Unauthenticated {
		t.Fatal("missing identity accepted", err)
	}
	star := metadata.AppendToOutgoingContext(ctx, "astra-admission-bin", string(system.star.Admission), "astra-signature-bin", string(system.star.AdmissionSignature))
	if _, err := client.Commit(star, request); status.Code(err) != codes.Unauthenticated {
		t.Fatal("Star could manage authority", err)
	}
	manager := metadata.AppendToOutgoingContext(ctx, "astra-admission-bin", string(system.manager.Admission), "astra-signature-bin", string(system.manager.AdmissionSignature))
	for range 2 {
		reply, err := client.Commit(manager, request)
		if err != nil || reply.Version != 1 {
			t.Fatal("durable original request not confirmed", err)
		}
	}
	request.Change.Action = &comet.AlmanacChange_Value{Value: []byte{2}}
	if _, err := client.Commit(manager, request); status.Code(err) != codes.Aborted {
		t.Fatal("conflicting request accepted", err)
	}
	loaded, err := system.store.Load(ctx, storage.Scope{Sector: "s", Spectrum: "p"})
	if err != nil || loaded.Version != 1 || loaded.Records[0].Value[0] != 1 {
		t.Fatal("failed request changed durable data", err)
	}
}

// TestWindow 累计确认只释放相应范围的前缀, 版本零有效, 已取消等待不滞留整个同步 worker.
func TestWindow(t *testing.T) {
	t.Parallel()
	a, b := storage.Scope{Sector: "s", Spectrum: "a"}, storage.Scope{Sector: "s", Spectrum: "b"}
	ctx, cancel := context.WithCancel(t.Context())
	transfer := &transfer{ctx: ctx, wake: make(chan struct{}, 1), sent: make(map[storage.Scope]storage.Version), input: make(chan incoming, 1)}
	transfer.charge(a, 0, 100)
	transfer.charge(a, 1, 200)
	transfer.charge(b, 2, 300)
	transfer.release(a, 0)
	if transfer.bytes != 500 || len(transfer.credits) != 2 {
		t.Fatal("zero boundary released wrong suffix")
	}
	transfer.release(a, 1)
	if transfer.bytes != 300 || len(transfer.credits) != 1 {
		t.Fatal("scope boundary released other scope")
	}
	transfer.charge(a, 2, 8<<20)
	cancel()
	if err := transfer.budget(1); err != context.Canceled {
		t.Fatal("cancelled window wait did not stop", err)
	}
	transfer.release(a, 2)
	transfer.release(b, 2)
	if transfer.bytes != 0 || len(transfer.credits) != 0 {
		t.Fatal("window retained released credits")
	}
}
