package registration

import (
	"context"
	"sync/atomic"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

// RegistrationOptions 定义一条尚未发布的本地 Registration。
// Client.Registration 只校验这些参数并生成 UUID，不执行 Redis I/O 或启动协程。
type RegistrationOptions struct {
	// Type 标识 Client Zone 内的 Registry，必须符合 1 至 64 字节标识规则。
	Type string
	// TTL 是 Redis 租约时长，必须为正数且能精确表示为毫秒。
	TTL time.Duration
	// RenewInterval 控制自动续期；零值使用 TTL/3，显式值必须在 100ms 至 TTL/3 之间。
	RenewInterval time.Duration
	// Version 是应用定义的正整数版本，可供路由或兼容性策略使用；不得超过 maxSafeInteger。
	Version uint64
}

// Registration 表示一个应用拥有的强类型注册生命周期。
// A/D 负责字段编码，SDK 负责单格 Fields 合并、Redis 串行同步、续期和恢复。
type Registration[A, D verdandi.Encoder] struct {
	client  *Client
	options RegistrationOptions
	uuid    string

	operation chan struct{}
	core      atomic.Pointer[registrationCore]
	dataShape fields
	terminal  atomic.Bool

	registered atomic.Bool
}

// Registration 创建新的进程 UUID，但不发布、不续期，也不占用 Client 工作槽位。
// options 固定 Type、TTL、续期间隔和初始 Version；A/D 仅作为编译期编码契约。
// 应用准备完毕后才调用 Register，因此构造成功不代表服务已进入 Registry。
func (client *Client) Registration[
	A verdandi.Encoder,
	D verdandi.Encoder,
](options RegistrationOptions) (*Registration[A, D], error) {
	if client == nil {
		return nil, protocolError(codeClosed, "", 0)
	}
	minimumRenewInterval := 100 * time.Millisecond
	if client.runtime != nil {
		runtime, err := runtimeFor(client)
		if err != nil {
			return nil, err
		}
		minimumRenewInterval = runtime.config.minimumRenewInterval
	}
	if !validType(options.Type) {
		return nil, protocolError(codeInvalid, "type", 0)
	}
	if options.Version == 0 || options.Version > maxSafeInteger {
		return nil, protocolError(codeInvalid, "@version", 0)
	}
	if _, err := durationMilliseconds(options.TTL); err != nil {
		return nil, err
	}
	renew := options.RenewInterval
	if renew == 0 {
		renew = options.TTL / 3
	}
	if renew < minimumRenewInterval || renew > options.TTL/3 {
		return nil, protocolError(codeInvalid, "renew_interval", 0)
	}
	// UUID 在本地构造阶段生成，并在该句柄整个生命周期内保持不变。
	uuid, err := newRegistrationUUID()
	if err != nil {
		return nil, wrapError(codeUnavailable, err)
	}
	registration := &Registration[A, D]{
		client:    client,
		options:   options,
		uuid:      uuid,
		operation: make(chan struct{}, 1),
	}
	registration.operation <- struct{}{}
	return registration, nil
}

// UUID 返回 Client.Registration 生成的进程身份。
// 它在 Register 前即可读取，并一直保持到句柄终止；进程重启必须构造新句柄和新 UUID。
func (registration *Registration[A, D]) UUID() string {
	if registration == nil {
		return ""
	}
	return registration.uuid
}

// Registered 报告该句柄是否已经成功完成首次 Register 且尚未进入终止状态。
func (registration *Registration[A, D]) Registered() bool {
	return registration != nil && registration.registered.Load()
}

// Revision 返回 SDK 当前希望 Redis 持有的内容版本；Register 前返回零。
// 不确定写入仍会推进期望版本，以便后续完整 Register 恢复同一状态。
func (registration *Registration[A, D]) Revision() uint64 {
	if registration == nil {
		return 0
	}
	core := registration.core.Load()
	if core == nil {
		return 0
	}
	return core.Revision()
}

// Timestamp 返回最近一次 Redis 确认的 Unix 毫秒时间戳；Register 前或尚无确认时返回零。
func (registration *Registration[A, D]) Timestamp() uint64 {
	if registration == nil {
		return 0
	}
	core := registration.core.Load()
	if core == nil {
		return 0
	}
	return core.Timestamp()
}

// Errors 返回 Register 后自动续期和恢复的有界异步诊断通道。
// Register 前返回 nil；底层 Registration 结束后通道关闭。
func (registration *Registration[A, D]) Errors() <-chan error {
	if registration == nil {
		return nil
	}
	core := registration.core.Load()
	if core == nil {
		return nil
	}
	return core.Errors()
}

// Register 在应用准备就绪后发布完整不可变 attr 和可变 data，并启动该 UUID 的唯一同步协程。
// ctx 控制首次发布；确定未生效的失败可重试，同一句柄成功后再次调用返回 contract。
// 成功后 Attr、TTL 和 Data 字段名集合固定，字段值由后续 Update 修改。
func (registration *Registration[A, D]) Register(ctx context.Context, attr A, data D) error {
	if err := registration.acquire(ctx); err != nil {
		return err
	}
	defer registration.release()
	if registration.terminal.Load() {
		return protocolError(codeClosed, "", 0)
	}
	if core := registration.core.Load(); core != nil {
		return protocolError(codeContract, "register", core.Revision())
	}
	// 在接纳工作协程和 Redis I/O 前完成应用编码，确保契约错误没有外部副作用。
	encodedAttr, err := encodeFieldValue(attr, "attr")
	if err != nil {
		return err
	}
	encodedData, err := encodeFieldValue(data, "data")
	if err != nil {
		return err
	}
	runtime, err := runtimeFor(registration.client)
	if err != nil {
		return err
	}
	core, err := runtime.registerWithUUID(ctx, registrationConfig{
		Type:          registration.options.Type,
		TTL:           registration.options.TTL,
		RenewInterval: registration.options.RenewInterval,
		Version:       registration.options.Version,
		Attr:          encodedAttr,
		Data:          encodedData,
	}, registration.uuid)
	if err != nil {
		return err
	}
	// 先保存固定 Data 结构，再原子发布 core/registered，之后 Update 才能通过 activeCore。
	registration.dataShape = fieldStructure(encodedData)
	registration.core.Store(core)
	registration.registered.Store(true)
	return nil
}

// Update 编码一个完整期望 Data，并与最近期望状态比较。
// 并发调用先合并进该 UUID 的单格 Fields 邮箱，worker 只把最终变化写入 Redis；无变化时不推进 revision，也不刷新 TTL。
func (registration *Registration[A, D]) Update(ctx context.Context, data D) error {
	return registration.updateContent(ctx, nil, data)
}

// SetVersion 只修改应用 Version，并在值变化时推进一次内容 revision 和刷新 TTL。
// version 必须为正且不超过 maxSafeInteger。
func (registration *Registration[A, D]) SetVersion(ctx context.Context, version uint64) error {
	core, err := registration.activeCore()
	if err != nil {
		return err
	}
	if version == 0 || version > maxSafeInteger {
		return protocolError(codeInvalid, "@version", 0)
	}
	return core.updateOwned(ctx, registrationUpdateFields{Version: &version})
}

// UpdateContent 用一次原子写同时修改 version 和完整期望 data 中的变化字段。
// 无论变化字段数量多少，成功操作只推进一个内容 revision。
func (registration *Registration[A, D]) UpdateContent(ctx context.Context, version uint64, data D) error {
	return registration.updateContent(ctx, &version, data)
}

// Renew 显式刷新租约时间戳与 TTL，不修改内容 revision。
// 它与 Update 共用该 Registration 的 worker；同批有效 Update 已刷新 TTL 时不重复写，否则独立执行 Renew。
func (registration *Registration[A, D]) Renew(ctx context.Context) error {
	core, err := registration.activeCore()
	if err != nil {
		return err
	}
	return core.Renew(ctx)
}

// Unregister 是终止操作。
// Register 前调用只关闭本地句柄；Register 后会停止新写、排空已准入邮箱、删除可确认的 Redis 状态并等待协程退出。
// ctx 只限制本次等待，终止状态不会因超时回滚。
func (registration *Registration[A, D]) Unregister(ctx context.Context) error {
	if registration == nil {
		return nil
	}
	if err := registration.acquire(ctx); err != nil {
		return err
	}
	defer registration.release()
	if registration.terminal.Load() {
		if core := registration.core.Load(); core != nil {
			return core.Close(ctx)
		}
		return nil
	}
	registration.terminal.Store(true)
	registration.registered.Store(false)
	core := registration.core.Load()
	if core == nil {
		return nil
	}
	return core.Close(ctx)
}

// Close 是 Unregister 的资源管理惯例别名，语义和返回结果完全相同。
func (registration *Registration[A, D]) Close(ctx context.Context) error {
	return registration.Unregister(ctx)
}

// updateContent 编码完整 Data、校验固定字段结构，并提交 Version/Data 的组合期望状态。
// version 为 nil 表示保持现值；data 始终表示完整结构而不是局部 Patch。
func (registration *Registration[A, D]) updateContent(ctx context.Context, version *uint64, data D) error {
	core, err := registration.activeCore()
	if err != nil {
		return err
	}
	encoded, err := encodeFieldValue(data, "data")
	if err != nil {
		return err
	}
	if !sameFieldStructure(registration.dataShape, encoded) {
		return protocolError(codeContract, "data", core.Revision())
	}
	if version != nil && (*version == 0 || *version > maxSafeInteger) {
		return protocolError(codeInvalid, "@version", 0)
	}
	return core.updateOwned(ctx, registrationUpdateFields{Version: version, Data: encoded})
}

// activeCore 返回已成功 Register 且未终止的内部对象。
func (registration *Registration[A, D]) activeCore() (*registrationCore, error) {
	if registration == nil || registration.terminal.Load() {
		return nil, protocolError(codeClosed, "", 0)
	}
	core := registration.core.Load()
	if core == nil {
		return nil, protocolError(codeContract, "register", 0)
	}
	return core, nil
}

// acquire 使用容量为一的通道串行化公开生命周期操作，同时允许 ctx 在等待锁时取消。
// 成功后调用方必须 defer release。
func (registration *Registration[A, D]) acquire(ctx context.Context) error {
	if registration == nil {
		return protocolError(codeClosed, "", 0)
	}
	if ctx == nil {
		return protocolError(codeInvalid, "context", 0)
	}
	select {
	case <-registration.operation:
		return nil
	case <-ctx.Done():
		return wrapContext(ctx.Err())
	}
}

// release 归还公开生命周期操作令牌；仅能与一次成功 acquire 配对。
func (registration *Registration[A, D]) release() {
	registration.operation <- struct{}{}
}

// fieldStructure 只保留字段名集合，用于后续完整 Data 的固定结构校验。
func fieldStructure(values fields) fields {
	if len(values) == 0 {
		return nil
	}
	structure := make(fields, len(values))
	for name := range values {
		structure[name] = nil
	}
	return structure
}
