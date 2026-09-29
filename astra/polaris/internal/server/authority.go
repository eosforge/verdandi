// Package server 将 Polaris 持久核心暴露给已准入管理面和 Star, 不接受 Comet 会话.
package server

import (
	"context"
	"errors"
	"io"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/eosforge/astra/internal/admission"
	"github.com/eosforge/astra/internal/generated/comet"
	"github.com/eosforge/astra/internal/generated/orbit"
	"github.com/eosforge/astra/internal/generated/polaris"
	"github.com/eosforge/astra/polaris/internal/storage"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// Authority 只提供管理面读写; 租约、Comet Session 和对等数据不进入持久库.
type Authority struct {
	polaris.UnimplementedAuthorityServer
	store     *storage.Store       // 权威持久库, 提交/读取/目录的唯一真相源.
	directory *admission.Directory // 准入目录, 校验 Astrolabe 身份与实例新鲜度.
	cache     *cache               // 快照缓存, 共享准备预算, 避免重复构造大范围快照.
	slots     chan struct{}        // 管理并发许可 (16), 满时直接拒绝, 不排队.
}

// Validate 在发布部署身份和开放监听前检查内部登录底稿, 普通业务值仍保持不透明.
// ctx 为启动上下文; 库不可读或凭据条目非法时返回错误, 阻止带病启动.
func Validate(ctx context.Context, store *storage.Store) error {
	if store == nil {
		return storage.ErrInput
	}
	// 只检查固定内部凭据范围, 其他范围内容不作解析.
	scope := storage.Scope{Sector: "__auth", Spectrum: "comet"}
	snapshot, err := store.Load(ctx, scope)
	if err != nil {
		return err
	}
	// 逐条按凭据契约校验, 任何非法条目都视为库损坏.
	for _, record := range snapshot.Records {
		if !credential(scope, storage.Change{Key: record.Key, Value: record.Value}) {
			return storage.ErrCorrupt
		}
	}
	return nil
}

// NewAuthority 的 snapshotBytes 是整个服务共享准备预算, 必须可容纳一个合法最大范围.
// store/directory 为依赖; snapshotBytes 为共享预算 (不小于缓存下限且不超过 1 GiB).
func NewAuthority(store *storage.Store, directory *admission.Directory, snapshotBytes int64) (*Authority, error) {
	if store == nil || directory == nil {
		return nil, storage.ErrInput
	}
	cache := newCache(store, store.Limits(), snapshotBytes)
	if snapshotBytes < cache.reserve || snapshotBytes > 1<<30 {
		return nil, storage.ErrInput
	}
	return &Authority{store: store, directory: directory, cache: cache, slots: make(chan struct{}, 16)}, nil
}

// begin 验证 Astrolabe 身份和有限截止, 立即拒绝过载, 不为等待资源另建无界队列.
// ctx 为请求上下文; 返回已授权成员, 截止非法/过载/身份不符时返回对应 gRPC 错误.
func (authority *Authority) begin(ctx context.Context) (*orbit.Member, error) {
	// 截止必须存在且在 30 秒内, 防止无界管理请求占住许可.
	deadline, ok := ctx.Deadline()
	if !ok || time.Until(deadline) <= 0 || time.Until(deadline) > 30*time.Second {
		return nil, status.Error(codes.InvalidArgument, "Management deadline must be finite and at most 30 seconds")
	}
	// 先占许可再验身份, 满时立即拒绝; 身份失败要归还许可.
	select {
	case authority.slots <- struct{}{}:
	default:
		return nil, status.Error(codes.ResourceExhausted, "Management capacity exceeded")
	}
	member, err := authority.directory.Authorize(ctx, orbit.Role_ROLE_ASTROLABE)
	if err != nil {
		<-authority.slots
		return nil, err
	}
	return member, nil
}

// Commit 原子提交一个单键或唯一键批次, 返回仅证明 Polaris 的持久事务已经成功提交.
// request 为一帧可容纳的完整提交; 超出单帧的批次使用 Batch, 不限制整批总量.
func (authority *Authority) Commit(ctx context.Context, request *polaris.CommitRequest) (*polaris.Position, error) {
	// 先占许可, 退出时归还, 任何提前返回都不泄漏许可.
	member, err := authority.begin(ctx)
	if err != nil {
		return nil, err
	}
	defer func() { <-authority.slots }()
	return authority.commit(ctx, member, request)
}

// Batch 暂存同一范围/版本的运输页, 收到显式完整页和正常 EOF 后才允许一次提交.
// 不把已接收的前缀写入 records/history; 中断、跨页重复或不一致均由整批校验拒绝.
func (authority *Authority) Batch(stream grpc.ClientStreamingServer[polaris.BatchRequest, polaris.Position]) error {
	ctx := stream.Context()
	member, err := authority.begin(ctx)
	if err != nil {
		return err
	}
	defer func() { <-authority.slots }()
	var request *polaris.CommitRequest
	complete := false
	for {
		page, err := stream.Recv()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return err
		}
		if !authority.directory.Current(member) {
			return status.Error(codes.Unauthenticated, "Management admission replaced")
		}
		if complete || page == nil || page.Scope == nil || page.Version == 0 || len(page.Changes) == 0 {
			return status.Error(codes.InvalidArgument, "Invalid batch page")
		}
		if request == nil {
			request = &polaris.CommitRequest{Scope: page.Scope, Version: page.Version}
		} else if !proto.Equal(request.Scope, page.Scope) || request.Version != page.Version {
			return status.Error(codes.InvalidArgument, "Batch position changed")
		}
		request.Changes = append(request.Changes, page.Changes...)
		complete = page.Complete
	}
	if !complete {
		return status.Error(codes.InvalidArgument, "Incomplete batch")
	}
	result, err := authority.commit(ctx, member, request)
	if err != nil {
		return err
	}
	return stream.SendAndClose(result)
}

// commit 共用单帧和多帧提交校验, 调用方已占管理许可; 完整检查后只进入一次持久事务.
func (authority *Authority) commit(ctx context.Context, member *orbit.Member, request *polaris.CommitRequest) (*polaris.Position, error) {
	if request == nil || request.Scope == nil || (request.Change == nil) == (len(request.Changes) == 0) {
		return nil, status.Error(codes.InvalidArgument, "Invalid authority commit")
	}
	scope := storage.Scope{Sector: string(request.Scope.Sector), Spectrum: string(request.Scope.Spectrum)}
	if len(request.Changes) != 0 {
		if strings.HasPrefix(scope.Sector, "__") {
			return nil, status.Error(codes.InvalidArgument, "Invalid batch scope")
		}
		changes := make([]storage.Change, 0, len(request.Changes))
		for _, item := range request.Changes {
			if item == nil {
				return nil, status.Error(codes.InvalidArgument, "Missing batch entry")
			}
			entry := storage.Change{Key: item.Key}
			switch action := item.Action.(type) {
			case *comet.AlmanacChange_Value:
				if action == nil {
					return nil, status.Error(codes.InvalidArgument, "Missing value action")
				}
				entry.Value = action.Value
			case *comet.AlmanacChange_Erase:
				if action == nil || action.Erase == nil {
					return nil, status.Error(codes.InvalidArgument, "Missing erase action")
				}
				entry.Erase = true
			default:
				return nil, status.Error(codes.InvalidArgument, "Missing batch action")
			}
			changes = append(changes, entry)
		}
		if !authority.directory.Current(member) {
			return nil, status.Error(codes.Unauthenticated, "Management admission replaced")
		}
		version, err := authority.store.CommitBatch(ctx, scope, storage.Version(request.Version), changes)
		if err != nil {
			return nil, failure(err)
		}
		return &polaris.Position{Scope: request.Scope, Version: uint64(version)}, nil
	}
	change := storage.Change{Version: storage.Version(request.Version), Key: request.Change.Key}
	// 动作二选一: 设值携带载荷, 删除必须是显式擦除.
	switch action := request.Change.Action.(type) {
	case *comet.AlmanacChange_Value:
		change.Value = action.Value
	case *comet.AlmanacChange_Erase:
		if action.Erase == nil {
			return nil, status.Error(codes.InvalidArgument, "Missing erase action")
		}
		change.Erase = true
	default:
		return nil, status.Error(codes.InvalidArgument, "Missing authority action")
	}
	// 内部凭据范围走凭据契约, 防止非法密钥材料入库.
	if !credential(scope, change) {
		return nil, status.Error(codes.InvalidArgument, "Invalid internal credential")
	}
	// 提交前复核授权实例仍新鲜, 防止准入更替后用旧身份写入.
	if !authority.directory.Current(member) {
		return nil, status.Error(codes.Unauthenticated, "Management admission replaced")
	}
	version, err := authority.store.Commit(ctx, scope, change)
	if err != nil {
		return nil, failure(err)
	}
	return &polaris.Position{Scope: request.Scope, Version: uint64(version)}, nil
}

// credential 只解析固定内部凭据 Scope, 普通业务 Buffer 保持不透明; Delete 同样校验 APIKEY.
// scope/change 为待入库变更; 非内部范围直接放行, 内部范围按凭据契约校验.
func credential(scope storage.Scope, change storage.Change) bool {
	if scope.Sector != "__auth" || scope.Spectrum != "comet" {
		return true
	}
	// 键必须为合法短文本, 防止非法键污染内部范围.
	if len(change.Key) == 0 || len(change.Key) > 128 || !utf8.ValidString(change.Key) || strings.ContainsRune(change.Key, 0) {
		return false
	}
	if change.Erase {
		// 删除同样校验键, 通过即放行.
		return true
	}
	// 设值必须是合法凭据消息且密钥长度有界, 超大密钥拒绝.
	var value orbit.Credential
	return len(change.Value) <= 8192 && proto.Unmarshal(change.Value, &value) == nil && len(value.Secret) > 0 && len(value.Secret) <= 4096
}

// List 返回同一次 SQL 读取的完整有界范围元数据, 不复制所有内容或声称所有 Star 已安装.
// 返回完整目录, 单条响应超 8 MiB 即拒绝, 不截断返回部分目录.
func (authority *Authority) List(ctx context.Context, _ *comet.Empty) (*polaris.Inventory, error) {
	if _, err := authority.begin(ctx); err != nil {
		return nil, err
	}
	defer func() { <-authority.slots }()
	positions, err := authority.store.List(ctx)
	if err != nil {
		return nil, failure(err)
	}
	// 逐项转换为协议位置, 完整标志表示本次为全量目录.
	result := &polaris.Inventory{Complete: true}
	for _, position := range positions {
		result.Positions = append(result.Positions, &polaris.Position{Scope: &comet.Scope{Sector: []byte(position.Sector), Spectrum: []byte(position.Spectrum)}, Version: uint64(position.Version)})
	}
	if proto.Size(result) > 8<<20 {
		return nil, status.Error(codes.ResourceExhausted, "Scope inventory exceeds message capacity")
	}
	return result, nil
}

// Load 在 SQL 事务结束后分页发送冻结根, 从完整结束页取得版本, 网络不占数据库连接.
// request 为请求范围; stream 为服务端推送流; 授权实例中途更替即中断发送.
func (authority *Authority) Load(request *comet.Scope, stream grpc.ServerStreamingServer[polaris.Snapshot]) error {
	member, err := authority.begin(stream.Context())
	if err != nil {
		return err
	}
	defer func() { <-authority.slots }()
	if request == nil {
		return status.Error(codes.InvalidArgument, "Missing scope")
	}
	scope := storage.Scope{Sector: string(request.Sector), Spectrum: string(request.Spectrum)}
	// 当前请求单独捕获最低版本, 不复用已落后于这次读取的旧缓存快照.
	version, err := authority.store.Version(stream.Context(), scope)
	if err != nil {
		return failure(err)
	}
	// 从缓存钉住该版本快照, 发送期间不被回收, 结束即释放.
	pin, err := authority.cache.acquire(stream.Context(), scope, version)
	if err != nil {
		return failure(err)
	}
	defer pin.release()
	return pages(pin.Snapshot, 8<<20, func(page *polaris.Snapshot) error {
		// 每页发送前复核授权实例, 更替即中断, 不向旧身份继续推送.
		if !authority.directory.Current(member) {
			return status.Error(codes.Unauthenticated, "Management admission replaced")
		}
		return stream.Send(page)
	})
}

// pages 单遍打包, 初值 256 KiB 是工程分页目标而非记录上限, 单个大值允许独占一页.
// snapshot 为冻结快照; maximum 为接收方容量; send 逐页发送, 失败即中止.
func pages(snapshot *storage.Snapshot, maximum int, send func(*polaris.Snapshot) error) error {
	// 容量必须有界 (1 KiB..8 MiB), 防止无意义分页或超大单页.
	if maximum < 1024 || maximum > 8<<20 {
		return status.Error(codes.InvalidArgument, "Invalid Almanac receive capacity")
	}
	scope := &comet.Scope{Sector: []byte(snapshot.Sector), Spectrum: []byte(snapshot.Spectrum)}
	page := &polaris.Snapshot{Scope: scope, Version: proto.Uint64(uint64(snapshot.Version))}
	size := proto.Size(scope) + 32
	// 逐条打包: 页满即发送并开新页, 单条超限直接拒绝整批.
	for _, record := range snapshot.Records {
		entry := &comet.AlmanacChange{Key: record.Key, Action: &comet.AlmanacChange_Value{Value: record.Value}}
		cost := proto.Size(entry) + 6
		if len(page.Entries) != 0 && size+cost > min(maximum, 256<<10) {
			if err := send(page); err != nil {
				return err
			}
			page = &polaris.Snapshot{Scope: scope, Version: proto.Uint64(uint64(snapshot.Version))}
			size = proto.Size(scope) + 32
		}
		if proto.Size(scope)+32+cost > maximum {
			return status.Error(codes.ResourceExhausted, "Almanac record exceeds receiver capacity")
		}
		page.Entries = append(page.Entries, entry)
		size += cost
	}
	// 尾页标记完整后发送, 空快照也发送一个完整空页.
	page.Complete = true
	return send(page)
}

// failure 只发送固定分类, 不泄露 SQLite 原文、账号、载荷或宿主路径.
// err 为内部错误; 返回对应的 gRPC 状态错误.
func failure(err error) error {
	// 按内部错误到 gRPC 码的固定映射, 未知错误一律不可用, 不透传原文.
	switch {
	case errors.Is(err, storage.ErrInput):
		return status.Error(codes.InvalidArgument, "Invalid authority input")
	case errors.Is(err, storage.ErrVersion):
		return status.Error(codes.Aborted, "Authority version conflict")
	case errors.Is(err, storage.ErrHistory):
		return status.Error(codes.OutOfRange, "Continuous history unavailable")
	case errors.Is(err, storage.ErrUncertain):
		return status.Error(codes.Unknown, "Authority commit cannot be confirmed")
	case errors.Is(err, storage.ErrCapacity):
		return status.Error(codes.ResourceExhausted, "Authority capacity exceeded")
	case errors.Is(err, context.Canceled):
		return status.Error(codes.Canceled, "Authority request cancelled")
	case errors.Is(err, context.DeadlineExceeded):
		return status.Error(codes.DeadlineExceeded, "Authority request deadline exceeded")
	default:
		return status.Error(codes.Unavailable, "Authority storage unavailable")
	}
}
