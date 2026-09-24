// Package bridge 连接 Pulsar 目录和唯一 Polaris, 不依赖 Comet Go 或复制权威持久状态.
package bridge

import (
	"bytes"
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
	node     *admission.Node     // 所属准入节点, 提供目录与会话上下文, 构造后不变.
	identity *admission.Identity // 本进程身份材料, 用于拨号时的 TLS 凭据, 构造后不变.
	mutex    sync.RWMutex        // 保护下述目标与目录快照, 只做短临界区交换.
	target   *target             // 当前唯一 Polaris 提交目标, 空表示暂无可用权威.
	observed time.Time           // 最近一次目录观察的本地时间, 供管理面判断新鲜度.
	stale    bool                // 目标是否陈旧 (观察失败或目录变化后置位).
	closed   bool                // 关闭后不再建连或替换目标, 单向置位.
	nodes    []node              // 仅目录刷新时创建, 发布后不可变, HTTP 读取不再克隆完整 Member 消息.
	members  map[string]string   // 同一目录的端点 -> 实例 ID, 采样完成时 O(1) 拒绝旧实例结果, 不逐项扫描整个目录.
	metrics  map[string]metric   // 仅明确部署的端点保存最近一次观测, 不累计时序或业务标签.
}

// target 是单个 Polaris 的连接三元组, 替换时整体交换, 不复用旧 Channel.
type target struct {
	member  *orbit.Member           // 目录选定的权威成员描述, 用于实例一致性校验.
	channel *grpc.ClientConn        // 到该成员的 gRPC 连接, 替换/关闭时在锁外释放.
	client  polaris.AuthorityClient // 基于上述连接的权威 RPC 客户端, 浏览器操作共用.
}

// New 不注册节点、不连接未知服务; 首次目录确认后 Update 才选定唯一 Polaris.
func New(node *admission.Node, identity *admission.Identity) *Backend {
	return &Backend{node: node, identity: identity, stale: true}
}

// Update 只由节点目录刷新路径调用, 暂时失败保留已有目标但标为陈旧; 多个权威绝不自行任选.
// observation 为最近一次目录同步的结果, 非空表示同步失败, 此时只标陈旧不替换目标.
func (backend *Backend) Update(observation error) {
	if observation != nil {
		// 同步失败不丢弃已有目标, 仅标陈旧让管理面可见, 下次成功观察再恢复.
		backend.mutex.Lock()
		backend.stale = true
		backend.mutex.Unlock()
		return
	}
	// 扫描目录成员: 收集对外展示投影与端点索引, 同时挑出 Polaris 角色并检测多权威.
	members := backend.node.Directory().Members()
	var member *orbit.Member
	multiple := false
	nodes := make([]node, 0, len(members))
	index := make(map[string]string, len(members))
	for _, candidate := range members {
		nodes = append(nodes, describe(candidate))
		index[candidate.Advertise] = string(candidate.Id)
		if candidate.Role == orbit.Role_ROLE_POLARIS {
			multiple = multiple || member != nil
			member = candidate
		}
	}
	if multiple {
		member = nil
	} // 新观察仍发布, 多个权威不任选一个作为提交目标.
	// 在同一短锁内发布目录快照, 目标是否变化的判断也在锁内完成, 拨号留在锁外.
	backend.mutex.Lock()
	backend.observed, backend.stale, backend.nodes = time.Now(), false, nodes
	backend.members = index // 与目录在同一短锁内发布, 抓取仍在锁外, 不缓存一次请求开始时的旧身份.
	unchanged := backend.target != nil && member != nil && bytes.Equal(backend.target.member.Id, member.Id)
	closed := backend.closed
	backend.mutex.Unlock()
	if unchanged || closed {
		// 目标未变或已关闭, 无须重建连接.
		return
	}
	if member == nil {
		// 无可用权威 (缺失或多权威), 清空目标让请求明确失败, 不保留过期连接.
		backend.replace(nil)
		return
	}
	// 拨号参数固定: 禁用重试 (写语义由调用方判定), 收发消息上限防止超大页拖垮管理面.
	channel, err := grpc.NewClient("passthrough:///"+member.Advertise, grpc.WithTransportCredentials(credentials.NewTLS(backend.identity.Client())), grpc.WithDisableRetry(), grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(8<<20), grpc.MaxCallSendMsgSize(2<<20)))
	if err != nil {
		backend.replace(nil)
		return
	}
	backend.replace(&target{member: member, channel: channel, client: polaris.NewAuthorityClient(channel)})
}

// replace 的旧 Channel 在锁外关闭, 旧写入中断返回不确定, 不改投新实例重发.
// next 为新的提交目标, 空表示清空 (无可用权威或关闭中).
func (backend *Backend) replace(next *target) {
	backend.mutex.Lock()
	if backend.closed {
		// 关闭中不接纳新目标, 刚建好的连接也直接释放, 不留半发布状态.
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
		// 旧连接在锁外关闭, 关闭可能阻塞, 不能占着目标锁.
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
// ctx 为调用方传入的请求上下文; 返回派生上下文、可用客户端, 目标不可用时直接返回错误.
func (backend *Backend) begin(ctx context.Context) (context.Context, polaris.AuthorityClient, error) {
	// 短读锁捕获目标指针与关闭标志, 后续的实例校验与上下文派生都在锁外.
	backend.mutex.RLock()
	current, closed := backend.target, backend.closed
	backend.mutex.RUnlock()
	if closed || current == nil || !backend.node.Directory().Current(current.member) {
		// 关闭、无目标或目录已切换实例, 拒绝请求, 不向过期权威发送.
		return nil, nil, status.Error(codes.Unavailable, "Authority target unavailable")
	}
	ctx, err := backend.node.Context(ctx)
	if err != nil {
		return nil, nil, err
	}
	return ctx, current.client, nil
}

// Commit 恰好执行一次内部 RPC, 不建立队列、改写期望版本或等待所有 Star 同步.
// request 为提交内容; 返回权威确认的位置, 失败由调用方按位置语义判定重试.
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
// scope 为要加载快照的范围; receive 逐页消费快照, 返回错误即中止流.
func (backend *Backend) Load(ctx context.Context, scope *comet.Scope, receive func(*polaris.Snapshot) error) error {
	// 派生可取消上下文, 任何提前返回都取消流, 不泄漏服务端推送.
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
	// 逐页接收直至流结束; 接收错误与消费错误都直接返回, 由调用方区分重试.
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
	ID       string `json:"id"`       // 成员实例 ID, 与目录中的身份一致.
	Role     string `json:"role"`     // 成员角色文本 (如 Polaris), 由枚举转换而来.
	Galaxy   string `json:"galaxy"`   // 所属集群标识, 用于区分多集群部署.
	Group    string `json:"group"`    // 所属分组, 展示用, 不参与路由.
	Endpoint string `json:"endpoint"` // 对外播报地址, 浏览器用它识别节点.
	Epoch    string `json:"epoch"`    // 成员任期, 字符串形式避免 JSON 丢失 uint64 精度.
}

// describe 只在可信目录变更时提取展示字段, 浏览器 epoch 使用字符串防止丢失 uint64 精度.
func describe(member *orbit.Member) node {
	return node{string(member.Id), member.Role.String(), member.Galaxy, member.Group, member.Advertise, strconv.FormatUint(member.Epoch, 10)}
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
