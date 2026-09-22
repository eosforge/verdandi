package admission

import (
	"context"
	"crypto/rand"
	"errors"
	randv2 "math/rand/v2"
	"sync"
	"time"

	"github.com/eosforge/verdandi/astra/internal/generated/astra"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// Node 独占一个启动幂等键和控制面 Channel, 登记/刷新通过 gate 防止重叠在途调用.
type Node struct {
	identity  *Identity
	directory *Directory
	channel   *grpc.ClientConn
	client    orbit.AdmissionClient
	request   *orbit.RegistrationRequest
	gate      chan struct{}
	mutex     sync.RWMutex
	member    *orbit.Member
	hello     *astra.Hello
}

// Open 只准备资源, 不发 RPC; super/advertise 必须为规范数值端点, 支持的 Go 角色为 Polaris/Astrolabe.
func Open(identity *Identity, super, galaxy, advertise, group string, role orbit.Role) (*Node, error) {
	if _, err := Endpoint(super); err != nil {
		return nil, err
	}
	if _, err := Endpoint(advertise); err != nil || identity == nil || !Name(galaxy) || !Name(group) || role != orbit.Role_ROLE_POLARIS && role != orbit.Role_ROLE_ASTROLABE {
		return nil, ErrIdentity
	}
	directory, err := NewDirectory(identity, galaxy, role, 16384)
	if err != nil {
		return nil, err
	}
	startup := make([]byte, 32)
	if _, err := rand.Read(startup); err != nil {
		return nil, ErrIdentity
	}
	channel, err := grpc.NewClient(super, grpc.WithTransportCredentials(credentials.NewTLS(identity.Client())), grpc.WithDisableRetry(), grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(8<<20), grpc.MaxCallSendMsgSize(4096)))
	if err != nil {
		return nil, err
	}
	return &Node{identity: identity, directory: directory, channel: channel, client: orbit.NewAdmissionClient(channel), gate: make(chan struct{}, 1), request: &orbit.RegistrationRequest{Galaxy: galaxy, Advertise: advertise, Role: role, Group: group, Username: identity.username, Password: identity.password, RequestId: startup}}, nil
}

// Close 取消所属 Channel 的在途调用; 调用方仍应先取消并等待自己启动的 Join/Refresh 循环.
func (node *Node) Close() error { return node.channel.Close() }

// Directory 返回共享的受控目录, 不交出内部可写 Member 或锁.
func (node *Node) Directory() *Directory { return node.directory }

// Hello 只在登记完成后返回独立凭证帧, 未就绪时为 nil.
func (node *Node) Hello() *astra.Hello {
	node.mutex.RLock()
	defer node.mutex.RUnlock()
	if node.hello == nil {
		return nil
	}
	return proto.Clone(node.hello).(*astra.Hello)
}

// Member 返回独立当前进程身份, 不随之后的目录变化偷偷替换当前进程.
func (node *Node) Member() *orbit.Member {
	node.mutex.RLock()
	defer node.mutex.RUnlock()
	if node.member == nil {
		return nil
	}
	return proto.Clone(node.member).(*orbit.Member)
}

// Context 添加原始准入 metadata; 调用者不得事先放入同名字段, 不携带账号密码.
func (node *Node) Context(ctx context.Context) (context.Context, error) {
	hello := node.Hello()
	if hello == nil {
		return nil, status.Error(codes.Unavailable, "Infrastructure admission pending")
	}
	values, _ := metadata.FromOutgoingContext(ctx)
	if len(values.Get("astra-admission-bin")) != 0 || len(values.Get("astra-signature-bin")) != 0 {
		return nil, ErrIdentity
	}
	return metadata.AppendToOutgoingContext(ctx, "astra-admission-bin", string(hello.Admission), "astra-signature-bin", string(hello.AdmissionSignature)), nil
}

// Register 发起一次有限时尝试; 超时重试必须复用本对象, 成功后不再重复密码登记.
func (node *Node) Register(ctx context.Context) error {
	select {
	case node.gate <- struct{}{}:
		defer func() { <-node.gate }()
	case <-ctx.Done():
		return ctx.Err()
	}
	if node.Member() != nil {
		return nil
	}
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	response, err := node.client.Register(ctx, node.request)
	if err != nil {
		return err
	}
	member, err := node.identity.Verify(response.Admission, response.Signature)
	if err != nil || member.Galaxy != node.request.Galaxy || member.Role != node.request.Role || member.Group != node.request.Group || member.Advertise != node.request.Advertise || member.Principal != node.identity.Principal(node.request.Galaxy, node.request.Advertise) {
		return ErrIdentity
	}
	if !slicesContains(response.Members, member) {
		return ErrIdentity
	}
	if err := node.directory.Install(response.Members); err != nil {
		return err
	}
	if !node.directory.Current(member) {
		return ErrIdentity
	}
	node.mutex.Lock()
	node.member = member
	node.hello = &astra.Hello{ProtocolMajor: 1, MaxFrameBytes: 8 << 20, Admission: response.Admission, AdmissionSignature: response.Signature}
	node.mutex.Unlock()
	return nil
}

// Refresh 读取一次完整目录, 与 Register 共用在途许可, 明确撤销身份时不自动生成新启动键.
func (node *Node) Refresh(ctx context.Context) error {
	select {
	case node.gate <- struct{}{}:
		defer func() { <-node.gate }()
	case <-ctx.Done():
		return ctx.Err()
	}
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	ctx, err := node.Context(ctx)
	if err != nil {
		return err
	}
	response, err := node.client.List(ctx, &orbit.DirectoryRequest{})
	if err != nil {
		return err
	}
	member := node.Member()
	if !slicesContains(response.Members, member) {
		return status.Error(codes.Unauthenticated, "Process admission replaced")
	}
	if err := node.directory.Install(response.Members); err != nil {
		return err
	}
	if !node.directory.Current(member) {
		return status.Error(codes.Unauthenticated, "Process admission replaced")
	}
	return nil
}

// Join 有界退避至登记完成或上下文结束, 永久拒绝直接返回, 绝不换启动键抢占身份.
func (node *Node) Join(ctx context.Context) error {
	delay := 500 * time.Millisecond
	for {
		err := node.Register(ctx)
		if err == nil || fatal(err) || ctx.Err() != nil {
			return err
		}
		if err := pause(ctx, delay); err != nil {
			return err
		}
		delay = min(delay*2, 30*time.Second)
	}
}

// Run 只做目录低频刷新, 每进程调用一次; changed 必须快速返回且不可重入 Register/Refresh.
// 临时失败保留原完整目录并退避, 永久身份拒绝返回交由宿主停止新管理操作.
func (node *Node) Run(ctx context.Context, changed func(error)) error {
	delay := 30 * time.Second
	for {
		if err := pause(ctx, delay); err != nil {
			return err
		}
		err := node.Refresh(ctx)
		if changed != nil {
			changed(err)
		}
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if fatal(err) {
			return err
		}
		if err == nil {
			delay = 30 * time.Second
		} else {
			delay = min(delay*2, 2*time.Minute)
		}
	}
}

// slicesContains 只比较已定义成员身份字段, 不把指针或未知协议字段当作身份.
func slicesContains(members []*orbit.Member, local *orbit.Member) bool {
	for _, member := range members {
		if same(member, local) {
			return true
		}
	}
	return false
}

// fatal 区分永久身份/参数错误与暂时不可达, 不把密码拒绝隐藏为无限自动重试.
func fatal(err error) bool {
	return errors.Is(err, ErrIdentity) || status.Code(err) == codes.Unauthenticated || status.Code(err) == codes.PermissionDenied || status.Code(err) == codes.InvalidArgument || status.Code(err) == codes.Aborted
}

// pause 是可取消的带抖动退避, math/rand 仅用于调度, 不用于凭证或启动 ID.
func pause(ctx context.Context, delay time.Duration) error {
	timer := time.NewTimer(delay + time.Duration(randv2.Int64N(int64(delay/5)+1)))
	defer timer.Stop()
	select {
	case <-timer.C:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}
