// Package server 将 Polaris 持久核心暴露给已准入管理面和 Star, 不接受 Comet 会话.
package server

import (
	"context"
	"errors"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/eosforge/verdandi/astra/internal/admission"
	"github.com/eosforge/verdandi/astra/internal/generated/comet"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"github.com/eosforge/verdandi/astra/internal/generated/polaris"
	"github.com/eosforge/verdandi/astra/polaris/internal/storage"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// Authority 只提供管理面读写; 租约、Comet Session 和对等数据不进入持久库.
type Authority struct {
	polaris.UnimplementedAuthorityServer
	store     *storage.Store
	directory *admission.Directory
	cache     *cache
	slots     chan struct{}
}

// Validate 在发布部署身份和开放监听前检查内部登录底稿, 普通业务值仍保持不透明.
func Validate(ctx context.Context, store *storage.Store) error {
	if store == nil {
		return storage.ErrInput
	}
	scope := storage.Scope{Sector: "__auth", Spectrum: "comet"}
	snapshot, err := store.Load(ctx, scope)
	if err != nil {
		return err
	}
	for _, record := range snapshot.Records {
		if !credential(scope, storage.Change{Key: record.Key, Value: record.Value}) {
			return storage.ErrCorrupt
		}
	}
	return nil
}

// NewAuthority 的 snapshotBytes 是整个服务共享准备预算, 必须可容纳一个合法最大范围.
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
func (authority *Authority) begin(ctx context.Context) (*orbit.Member, error) {
	deadline, ok := ctx.Deadline()
	if !ok || time.Until(deadline) <= 0 || time.Until(deadline) > 30*time.Second {
		return nil, status.Error(codes.InvalidArgument, "Management deadline must be finite and at most 30 seconds")
	}
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

// Commit 单次请求只修改一个 Key, 返回仅证明 Polaris 的持久事务已经成功提交.
func (authority *Authority) Commit(ctx context.Context, request *polaris.CommitRequest) (*polaris.Position, error) {
	member, err := authority.begin(ctx)
	if err != nil {
		return nil, err
	}
	defer func() { <-authority.slots }()
	if request == nil || request.Scope == nil || request.Change == nil || proto.Size(request) > 2<<20 {
		return nil, status.Error(codes.InvalidArgument, "Invalid authority commit")
	}
	scope := storage.Scope{Sector: request.Scope.Sector, Spectrum: request.Scope.Spectrum}
	change := storage.Change{Version: storage.Version(request.Version), Key: request.Change.Key}
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
	if !credential(scope, change) {
		return nil, status.Error(codes.InvalidArgument, "Invalid internal credential")
	}
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
func credential(scope storage.Scope, change storage.Change) bool {
	if scope.Sector != "__auth" || scope.Spectrum != "comet" {
		return true
	}
	if len(change.Key) == 0 || len(change.Key) > 128 || !utf8.ValidString(change.Key) || strings.ContainsRune(change.Key, 0) {
		return false
	}
	if change.Erase {
		return true
	}
	var value orbit.Credential
	return len(change.Value) <= 8192 && proto.Unmarshal(change.Value, &value) == nil && len(value.Secret) > 0 && len(value.Secret) <= 4096
}

// List 返回同一次 SQL 读取的完整有界范围元数据, 不复制所有内容或声称所有 Star 已安装.
func (authority *Authority) List(ctx context.Context, _ *comet.Empty) (*polaris.Inventory, error) {
	if _, err := authority.begin(ctx); err != nil {
		return nil, err
	}
	defer func() { <-authority.slots }()
	positions, err := authority.store.List(ctx)
	if err != nil {
		return nil, failure(err)
	}
	result := &polaris.Inventory{Complete: true}
	for _, position := range positions {
		result.Positions = append(result.Positions, &polaris.Position{Scope: &comet.Scope{Sector: position.Sector, Spectrum: position.Spectrum}, Version: uint64(position.Version)})
	}
	if proto.Size(result) > 8<<20 {
		return nil, status.Error(codes.ResourceExhausted, "Scope inventory exceeds message capacity")
	}
	return result, nil
}

// Load 在 SQL 事务结束后分页发送冻结根, 从完整结束页取得版本, 网络不占数据库连接.
func (authority *Authority) Load(request *comet.Scope, stream grpc.ServerStreamingServer[polaris.Snapshot]) error {
	member, err := authority.begin(stream.Context())
	if err != nil {
		return err
	}
	defer func() { <-authority.slots }()
	if request == nil {
		return status.Error(codes.InvalidArgument, "Missing scope")
	}
	scope := storage.Scope{Sector: request.Sector, Spectrum: request.Spectrum}
	// 当前请求单独捕获最低版本, 不复用已落后于这次读取的旧缓存快照.
	version, err := authority.store.Version(stream.Context(), scope)
	if err != nil {
		return failure(err)
	}
	pin, err := authority.cache.acquire(stream.Context(), scope, version)
	if err != nil {
		return failure(err)
	}
	defer pin.release()
	return pages(pin.Snapshot, 8<<20, func(page *polaris.Snapshot) error {
		if !authority.directory.Current(member) {
			return status.Error(codes.Unauthenticated, "Management admission replaced")
		}
		return stream.Send(page)
	})
}

// pages 单遍打包, 初值 256 KiB 是工程分页目标而非记录上限, 单个大值允许独占一页.
func pages(snapshot *storage.Snapshot, maximum int, send func(*polaris.Snapshot) error) error {
	if maximum < 1024 || maximum > 8<<20 {
		return status.Error(codes.InvalidArgument, "Invalid Almanac receive capacity")
	}
	scope := &comet.Scope{Sector: snapshot.Sector, Spectrum: snapshot.Spectrum}
	page := &polaris.Snapshot{Scope: scope, Version: proto.Uint64(uint64(snapshot.Version))}
	size := proto.Size(scope) + 32
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
	page.Complete = true
	return send(page)
}

// failure 只发送固定分类, 不泄露 SQLite 原文、账号、载荷或宿主路径.
func failure(err error) error {
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
