// Package bridge 连接 Pulsar 目录和唯一 Polaris, 不依赖 Comet Go 或复制权威持久状态.
package bridge

import (
	"context"
	"errors"
	"io"
	"strconv"
	"sync"
	"time"

	"github.com/eosforge/verdandi/astra/internal/admission"
	"github.com/eosforge/verdandi/astra/internal/generated/comet"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"github.com/eosforge/verdandi/astra/internal/generated/polaris"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/status"
)

// Backend 的目录观察和单个连接按短锁交换, 不在锁内发 RPC、等网络或关闭 Channel.
type Backend struct {
	node     *admission.Node
	identity *admission.Identity
	mutex    sync.RWMutex
	target   *target
	observed time.Time
	stale    bool
	closed   bool
	nodes    []node            // 仅目录刷新时创建, 发布后不可变, HTTP 读取不再克隆完整 Member 消息.
	members  map[string]string // 同一目录的端点 -> 实例 ID, 采样完成时 O(1) 拒绝旧实例结果, 不逐项扫描整个目录.
	metrics  map[string]metric // 仅明确部署的端点保存最近一次观测, 不累计时序或业务标签.
}

type target struct {
	member  *orbit.Member
	channel *grpc.ClientConn
	client  polaris.AuthorityClient
}

// New 不注册节点、不连接未知服务; 首次目录确认后 Update 才选定唯一 Polaris.
func New(node *admission.Node, identity *admission.Identity) *Backend {
	return &Backend{node: node, identity: identity, stale: true}
}

// Update 只由节点目录刷新路径调用, 暂时失败保留已有目标但标为陈旧; 多个权威绝不自行任选.
func (backend *Backend) Update(observation error) {
	if observation != nil {
		backend.mutex.Lock()
		backend.stale = true
		backend.mutex.Unlock()
		return
	}
	members := backend.node.Directory().Members()
	var member *orbit.Member
	multiple := false
	nodes := make([]node, 0, len(members))
	index := make(map[string]string, len(members))
	for _, candidate := range members {
		nodes = append(nodes, describe(candidate))
		index[candidate.Advertise] = candidate.Id
		if candidate.Role == orbit.Role_ROLE_POLARIS {
			multiple = multiple || member != nil
			member = candidate
		}
	}
	if multiple {
		member = nil
	} // 新观察仍发布, 多个权威不任选一个作为提交目标.
	backend.mutex.Lock()
	backend.observed, backend.stale, backend.nodes = time.Now(), false, nodes
	backend.members = index // 与目录在同一短锁内发布, 抓取仍在锁外, 不缓存一次请求开始时的旧身份.
	unchanged := backend.target != nil && member != nil && backend.target.member.Id == member.Id
	closed := backend.closed
	backend.mutex.Unlock()
	if unchanged || closed {
		return
	}
	if member == nil {
		backend.replace(nil)
		return
	}
	channel, err := grpc.NewClient("passthrough:///"+member.Advertise, grpc.WithTransportCredentials(credentials.NewTLS(backend.identity.Client())), grpc.WithDisableRetry(), grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(8<<20), grpc.MaxCallSendMsgSize(2<<20)))
	if err != nil {
		backend.replace(nil)
		return
	}
	backend.replace(&target{member: member, channel: channel, client: polaris.NewAuthorityClient(channel)})
}

// replace 的旧 Channel 在锁外关闭, 旧写入中断返回不确定, 不改投新实例重发.
func (backend *Backend) replace(next *target) {
	backend.mutex.Lock()
	if backend.closed {
		backend.mutex.Unlock()
		if next != nil {
			_ = next.channel.Close()
		}
		return
	}
	old := backend.target
	backend.target = next
	backend.mutex.Unlock()
	if old != nil {
		_ = old.channel.Close()
	}
}

// Close 幂等取消所属 RPC, 调用者先结束后台 Update 再关闭 Node.
func (backend *Backend) Close() {
	backend.mutex.Lock()
	backend.closed = true
	old := backend.target
	backend.target = nil
	backend.mutex.Unlock()
	if old != nil {
		_ = old.channel.Close()
	}
}

// begin 捕获当前目标和已准入 metadata, 不在每次浏览器操作中重新向 Pulsar 登录/查询.
func (backend *Backend) begin(ctx context.Context) (context.Context, polaris.AuthorityClient, error) {
	backend.mutex.RLock()
	current, closed := backend.target, backend.closed
	backend.mutex.RUnlock()
	if closed || current == nil || !backend.node.Directory().Current(current.member) {
		return nil, nil, status.Error(codes.Unavailable, "Authority target unavailable")
	}
	ctx, err := backend.node.Context(ctx)
	if err != nil {
		return nil, nil, err
	}
	return ctx, current.client, nil
}

// Commit 恰好执行一次内部 RPC, 不建立队列、改写期望版本或等待所有 Star 同步.
func (backend *Backend) Commit(ctx context.Context, request *polaris.CommitRequest) (*polaris.Position, error) {
	ctx, client, err := backend.begin(ctx)
	if err != nil {
		return nil, err
	}
	return client.Commit(ctx, request)
}

// List 只读取权威范围元信息, 不从 Star 的业务缓存推导管理内容.
func (backend *Backend) List(ctx context.Context) (*polaris.Inventory, error) {
	ctx, client, err := backend.begin(ctx)
	if err != nil {
		return nil, err
	}
	return client.List(ctx, &comet.Empty{})
}

// Load 使用派生 Context, HTTP 断流或校验失败立即取消自己的 gRPC 流; 不影响其他管理请求.
func (backend *Backend) Load(ctx context.Context, scope *comet.Scope, receive func(*polaris.Snapshot) error) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	ctx, client, err := backend.begin(ctx)
	if err != nil {
		return err
	}
	stream, err := client.Load(ctx, scope)
	if err != nil {
		return err
	}
	for {
		page, err := stream.Recv()
		if errors.Is(err, io.EOF) {
			return nil
		}
		if err != nil {
			return err
		}
		if err := receive(page); err != nil {
			return err
		}
	}
}

// node 是对外固定投影, 不包含 principal、签发正文、签名或任何账号材料.
type node struct {
	ID       string `json:"id"`
	Role     string `json:"role"`
	Galaxy   string `json:"galaxy"`
	Group    string `json:"group"`
	Endpoint string `json:"endpoint"`
	Epoch    string `json:"epoch"`
}

// describe 只在可信目录变更时提取展示字段, 浏览器 epoch 使用字符串防止丢失 uint64 精度.
func describe(member *orbit.Member) node {
	return node{member.Id, member.Role.String(), member.Galaxy, member.Group, member.Advertise, strconv.FormatUint(member.Epoch, 10)}
}

// Nodes 捕获不可变脱敏投影, observed/stale 不等同于节点在线状态, 不在请求热路径克隆全部 Protobuf.
func (backend *Backend) Nodes() any {
	backend.mutex.RLock()
	nodes, observed, stale := backend.nodes, backend.observed, backend.stale || backend.closed
	backend.mutex.RUnlock()
	if nodes == nil {
		nodes = []node{}
	}
	return struct {
		Nodes    []node    `json:"nodes"`
		Observed time.Time `json:"observed"`
		Stale    bool      `json:"stale"`
	}{nodes, observed.UTC(), stale || observed.IsZero()}
}
