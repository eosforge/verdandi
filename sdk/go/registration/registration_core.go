package registration

import (
	"bytes"
	"context"
	cryptorand "crypto/rand"
	"encoding/hex"
	"errors"
	mathrand "math/rand/v2"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

// registrationConfig 是首次发布一条进程 Registration 所需的完整内部配置。
// Attr、TTL 和 Data 字段名集合在该 UUID 生命周期内不可变。
type registrationConfig struct {
	// Type 标识 Client Zone 内的 Registry。
	Type string
	// TTL 是整毫秒精度的 Redis 租约时长。
	TTL time.Duration
	// RenewInterval 控制自动续期，零值在进入此层前会解析为 TTL/3。
	RenewInterval time.Duration
	// Version 是应用定义的正整数版本。
	Version uint64
	// Attr 是完整且不可变的顶层属性字段。
	Attr fields
	// Data 是值可变但字段名集合固定的完整顶层数据。
	Data fields
}

// registrationUpdateFields 表示一次非空 Version 和/或 Data 内容变化。
// Data 只包含变化字段，名称必须已经存在于该 Registration 的固定 Data 结构中。
type registrationUpdateFields struct {
	// Version 非 nil 时修改 @version；nil 表示保持现值。
	Version *uint64
	// Data 只包含变化后的编码字段值；省略字段表示不变。
	Data fields
}

// registrationBatch 是 worker 一次从合并邮箱取走的拥有型工作。
// Data 直接保存最后写入的字段值；Update/Renew 等待者受 slots 容量限制。
type registrationBatch struct {
	version    uint64
	hasVersion bool
	data       fields
	updates    []chan error
	renews     []chan error
}

// registrationState 只由该 Registration 的唯一工作协程读写。
// atomic Revision/Timestamp 只是对外投影，不是此状态的并发所有者。
type registrationState struct {
	revision  uint64
	timestamp uint64
	ttl       uint64
	version   uint64
	attr      fields
	data      fields
	uncertain bool
	healthy   bool
}

// registrationCore 持有一条已发布 Registration、一个单格 Fields 合并邮箱和唯一同步协程。
// 除 Client 的 Redis 传输与只读配置外，它不与其他 Registration 共享状态。
type registrationCore struct {
	client               *clientRuntime
	typeName             string
	uuid                 string
	ttl                  time.Duration
	renew                time.Duration
	dataShape            fields
	wake                 chan struct{}
	slots                chan struct{}
	closing              chan struct{}
	release              func()
	releaseConfiguration func()

	errors chan error
	done   chan struct{}

	bufferMu          sync.Mutex
	closed            bool
	pendingVersion    uint64
	pendingVersionSet bool
	pendingData       fields
	pendingUpdates    []chan error
	pendingRenews     []chan error

	closeOnce sync.Once
	doneOnce  sync.Once
	finalErr  error

	revision  atomic.Uint64
	timestamp atomic.Uint64
}

// registerWithUUID 校验完整初始状态、接纳一个工作协程并同步完成首次 Register。
// ctx 只控制首次发布；config 和 uuid 在调用前已由类型化外层构造，但仍在此执行协议校验。
func (client *clientRuntime) registerWithUUID(ctx context.Context, config registrationConfig, uuid string) (*registrationCore, error) {
	if client == nil {
		return nil, protocolError(codeClosed, "", 0)
	}
	if ctx == nil {
		return nil, protocolError(codeInvalid, "context", 0)
	}
	if !validType(config.Type) {
		return nil, protocolError(codeInvalid, "type", 0)
	}
	ttlMilliseconds, err := durationMilliseconds(config.TTL)
	if err != nil {
		return nil, err
	}
	renew := config.RenewInterval
	if renew == 0 {
		renew = config.TTL / 3
	}
	if renew < client.config.minimumRenewInterval || renew > config.TTL/3 {
		return nil, protocolError(codeInvalid, "renew_interval", 0)
	}
	if err := client.RefreshConfiguration(ctx); err != nil {
		return nil, err
	}
	limits := client.limits()
	attr := cloneFields(config.Attr)
	data := cloneFields(config.Data)
	if err := validateRecord(uuid, 1, ttlMilliseconds, config.Version, attr, data, limits); err != nil {
		return nil, err
	}

	// owner 的 Context 只由新工作协程持有；Client 仅保存 cancel，以满足关闭时的单向取消。
	owner, cancel := context.WithCancel(context.Background())
	releaseClient, err := client.admit(cancel)
	if err != nil {
		cancel()
		return nil, err
	}
	release := func() {
		cancel()
		releaseClient()
	}

	registration := &registrationCore{
		client:    client,
		typeName:  config.Type,
		uuid:      uuid,
		ttl:       config.TTL,
		renew:     renew,
		dataShape: fieldStructure(data),
		wake:      make(chan struct{}, 1),
		slots:     make(chan struct{}, client.config.registrationBuffer),
		closing:   make(chan struct{}),
		release:   release,
		errors:    make(chan error, client.config.registrationErrorBuffer),
		done:      make(chan struct{}),
	}
	state := registrationState{
		revision: 1,
		ttl:      ttlMilliseconds,
		version:  config.Version,
		attr:     attr,
		data:     data,
	}
	ready := make(chan error, 1)
	go registration.run(owner, state, ctx, ready)
	if err := <-ready; err != nil {
		return nil, err
	}
	return registration, nil
}

// Revision 返回最新期望内容版本；不确定写入也会推进该值以保持重放顺序。
func (registration *registrationCore) Revision() uint64 {
	if registration == nil {
		return 0
	}
	return registration.revision.Load()
}

// Timestamp 返回最近一次 Redis 确认的 Unix 毫秒时间戳。
func (registration *registrationCore) Timestamp() uint64 {
	if registration == nil {
		return 0
	}
	return registration.timestamp.Load()
}

// Errors 返回该 UUID 自动续期和恢复的有界异步诊断通道。
func (registration *registrationCore) Errors() <-chan error {
	if registration == nil {
		return nil
	}
	return registration.errors
}

// updateOwned 把一个字段字节已归 SDK 所有的 Update 合并进单格 Fields 邮箱。
// 类型化外层使用此路径避免二次深拷贝；ctx 可在准入前取消，准入后返回 worker 对本批的确认结果。
func (registration *registrationCore) updateOwned(ctx context.Context, update registrationUpdateFields) error {
	if registration == nil {
		return protocolError(codeClosed, "", 0)
	}
	if ctx == nil {
		return protocolError(codeInvalid, "context", 0)
	}
	if err := registration.validateBufferedUpdate(update); err != nil {
		return err
	}
	result := make(chan error, 1)
	if err := registration.acquireSlot(ctx); err != nil {
		return err
	}
	defer registration.releaseSlot()

	registration.bufferMu.Lock()
	if registration.closed {
		registration.bufferMu.Unlock()
		return protocolError(codeClosed, "", 0)
	}
	if update.Version != nil {
		registration.pendingVersion = *update.Version
		registration.pendingVersionSet = true
	}
	if len(update.Data) != 0 {
		if registration.pendingData == nil {
			registration.pendingData = make(fields, len(update.Data))
		}
		for name, value := range update.Data {
			registration.pendingData[name] = value
		}
	}
	registration.pendingUpdates = append(registration.pendingUpdates, result)
	registration.bufferMu.Unlock()
	registration.signal()
	return registration.waitResult(result)
}

// Renew 立即请求刷新固定租约而不改变内容 revision；自动续期在此调用之外继续运行。
func (registration *registrationCore) Renew(ctx context.Context) error {
	if registration == nil {
		return protocolError(codeClosed, "", 0)
	}
	if ctx == nil {
		return protocolError(codeInvalid, "context", 0)
	}
	result := make(chan error, 1)
	if err := registration.acquireSlot(ctx); err != nil {
		return err
	}
	defer registration.releaseSlot()
	registration.bufferMu.Lock()
	if registration.closed {
		registration.bufferMu.Unlock()
		return protocolError(codeClosed, "", 0)
	}
	registration.pendingRenews = append(registration.pendingRenews, result)
	registration.bufferMu.Unlock()
	registration.signal()
	return registration.waitResult(result)
}

// Close 停止新请求、排空已接纳请求，并等待终止清理完成。
// ctx 只约束本次等待；一旦 closing 关闭，该 Registration 不会恢复。
func (registration *registrationCore) Close(ctx context.Context) error {
	if registration == nil {
		return nil
	}
	if ctx == nil {
		return protocolError(codeInvalid, "context", 0)
	}
	registration.closeOnce.Do(func() {
		registration.bufferMu.Lock()
		registration.closed = true
		close(registration.closing)
		registration.bufferMu.Unlock()
	})
	select {
	case <-registration.done:
		return registration.finalErr
	case <-ctx.Done():
		return wrapContext(ctx.Err())
	}
}

// validateBufferedUpdate 在修改邮箱前完成每个调用可独立判断的 Version、字段结构和值容量校验。
// 完整记录大小与 revision 上限仍由 worker 在最终合并状态上验证。
func (registration *registrationCore) validateBufferedUpdate(update registrationUpdateFields) error {
	if update.Version == nil && len(update.Data) == 0 {
		return protocolError(codeContract, "update", 0)
	}
	if update.Version != nil && (*update.Version == 0 || *update.Version > maxSafeInteger) {
		return protocolError(codeInvalid, "@version", 0)
	}
	limits := registration.client.limits()
	for name, value := range update.Data {
		if _, exists := registration.dataShape[name]; !exists {
			return protocolError(codeContract, name, 0)
		}
		if err := validateApplicationField(name, value, limits.fieldNameMaxBytes, limits.dataValueMaxBytes); err != nil {
			return err
		}
	}
	return nil
}

// acquireSlot 限制同一 Registration 同时等待的公开操作数量；它不保存 Fields 或请求对象。
func (registration *registrationCore) acquireSlot(ctx context.Context) error {
	select {
	case registration.slots <- struct{}{}:
		return nil
	case <-registration.closing:
		return protocolError(codeClosed, "", 0)
	case <-registration.client.done:
		return protocolError(codeClosed, "", 0)
	case <-registration.client.transportDone:
		return protocolError(codeClosed, "", 0)
	case <-ctx.Done():
		return wrapContext(ctx.Err())
	}
}

// releaseSlot 归还一次成功 acquireSlot；只能成对调用。
func (registration *registrationCore) releaseSlot() {
	<-registration.slots
}

// signal 以容量一通知唤醒 worker；邮箱本身保存完整待处理状态，因此重复通知可安全合并。
func (registration *registrationCore) signal() {
	select {
	case registration.wake <- struct{}{}:
	default:
	}
}

// waitResult 等待已准入操作的 worker 结果；调用方 Context 不再掩盖已经可能执行的 Redis 写。
func (registration *registrationCore) waitResult(result <-chan error) error {
	select {
	case err := <-result:
		return err
	default:
	}
	select {
	case err := <-result:
		return err
	case <-registration.done:
		// worker 总是在完成已处理等待者后才关闭 done；二次检查避免 Close 竞争掩盖真实写入结果。
		select {
		case err := <-result:
			return err
		default:
			return protocolError(codeClosed, "", 0)
		}
	}
}

// takeBatch 原子接管当前邮箱内容并原地清空共享引用，后续调用可立即形成下一批。
func (registration *registrationCore) takeBatch() (registrationBatch, bool) {
	registration.bufferMu.Lock()
	defer registration.bufferMu.Unlock()
	if len(registration.pendingUpdates) == 0 && len(registration.pendingRenews) == 0 {
		return registrationBatch{}, false
	}
	batch := registrationBatch{
		version:    registration.pendingVersion,
		hasVersion: registration.pendingVersionSet,
		data:       registration.pendingData,
		updates:    registration.pendingUpdates,
		renews:     registration.pendingRenews,
	}
	registration.pendingVersion = 0
	registration.pendingVersionSet = false
	registration.pendingData = nil
	registration.pendingUpdates = nil
	registration.pendingRenews = nil
	return batch, true
}

// run 是一条 Registration 唯一的长期工作协程。
// owner 由 Client 关闭时取消；registerContext 只控制首次发布；ready 精确通知构造方初始化结果。
func (registration *registrationCore) run(owner context.Context, state registrationState, registerContext context.Context, ready chan<- error) {
	defer func() {
		if registration.releaseConfiguration != nil {
			registration.releaseConfiguration()
		}
		registration.release()
		registration.complete()
	}()
	// 首次 Register 同时服从调用方和对象所有者取消；成功后生命周期只由 owner/closing 控制。
	operation, cancel := mergeContexts(registerContext, owner)
	result, err := registration.client.callRegistration(operation, registrationScriptRegister, registration.typeName, registration.uuid,
		registerArguments(registration.uuid, state.revision, state.ttl, state.version, state.attr, state.data))
	cancel()
	if err == nil {
		err = validateRegistrationSuccess(result, state.revision)
	}
	if err != nil {
		registration.client.bestEffortUnregister(registration.typeName, registration.uuid)
		registration.finalErr = err
		ready <- err
		return
	}
	state.timestamp = result.timestamp
	state.healthy = true
	registration.revision.Store(state.revision)
	registration.timestamp.Store(result.timestamp)
	registration.releaseConfiguration = registration.client.acquireConfigurationRefresh()
	ready <- nil

	// Fields 邮箱和自动续期由同一协程串行处理，因此 TTL 刷新与 Data revision 永不并发乱序。
	timer := time.NewTimer(jitteredInterval(registration.renew, registration.client.config.renewJitterPercent))
	defer timer.Stop()
	for {
		select {
		case <-owner.Done():
			registration.shutdown(&state)
			return
		case <-registration.client.transportDone:
			registration.client.startClose()
			registration.shutdown(&state)
			return
		case <-registration.closing:
			registration.drainAndClose(owner, &state, timer)
			return
		case <-registration.wake:
			if batch, exists := registration.takeBatch(); exists {
				registration.handleBatch(owner, &state, batch, timer)
			}
		case <-timer.C:
			select {
			case <-owner.Done():
				registration.shutdown(&state)
				return
			case <-registration.client.transportDone:
				registration.client.startClose()
				registration.shutdown(&state)
				return
			case <-registration.closing:
				registration.drainAndClose(owner, &state, timer)
				return
			case <-registration.wake:
				if batch, exists := registration.takeBatch(); exists && registration.handleBatch(owner, &state, batch, timer) {
					continue
				}
			default:
			}
			registration.automaticRenew(owner, &state, timer)
		}
	}
}

// automaticRenew 执行一次定时续期、报告异步错误并无条件安排下一次尝试。
func (registration *registrationCore) automaticRenew(owner context.Context, state *registrationState, timer *time.Timer) {
	ctx, cancel := registration.client.commandContext(owner)
	err := registration.renewState(ctx, state)
	cancel()
	registration.report(err)
	resetTimer(timer, jitteredInterval(registration.renew, registration.client.config.renewJitterPercent))
}

// handleBatch 处理一次已从邮箱接管的合并工作，并完成本批所有等待者。
// 返回值表示 Update 或 Renew 已成功刷新 TTL，供计时器到期竞争路径避免立刻重复续期。
func (registration *registrationCore) handleBatch(
	owner context.Context,
	state *registrationState,
	batch registrationBatch,
	timer *time.Timer,
) bool {
	wrote := false
	if len(batch.updates) != 0 {
		var version *uint64
		if batch.hasVersion {
			version = &batch.version
		}
		previousRevision := state.revision
		ctx, cancel := registration.client.commandContext(owner)
		err := registration.updateState(ctx, state, registrationUpdateFields{Version: version, Data: batch.data})
		cancel()
		wrote = err == nil && state.revision != previousRevision
		for _, result := range batch.updates {
			result <- err
		}
	}
	if len(batch.renews) != 0 {
		renewErr := error(nil)
		// 有效 Update 已在同一 Lua 中刷新 TTL；no-op 或失败时仍独立执行显式 Renew。
		if !wrote {
			ctx, cancel := registration.client.commandContext(owner)
			renewErr = registration.renewState(ctx, state)
			cancel()
		}
		for _, result := range batch.renews {
			result <- renewErr
		}
		wrote = wrote || renewErr == nil
	}
	if wrote {
		resetTimer(timer, jitteredInterval(registration.renew, registration.client.config.renewJitterPercent))
	}
	return wrote
}

// drainAndClose 在显式关闭时处理邮箱中所有已接纳工作，然后执行终止清理。
// 关闭栅栏已阻止新请求；takeBatch 返回空时邮箱已经稳定排空。
func (registration *registrationCore) drainAndClose(owner context.Context, state *registrationState, timer *time.Timer) {
	for {
		batch, exists := registration.takeBatch()
		if !exists {
			registration.finish(state)
			return
		}
		registration.handleBatch(owner, state, batch, timer)
	}
}

// shutdown 处理 Client/owner 非正常关闭：停止准入、尽力清理当前状态，并让邮箱等待者返回 closed。
func (registration *registrationCore) shutdown(state *registrationState) {
	registration.bufferMu.Lock()
	registration.closed = true
	registration.bufferMu.Unlock()
	err := protocolError(codeClosed, "", 0)
	if batch, exists := registration.takeBatch(); exists {
		for _, result := range batch.updates {
			result <- err
		}
		for _, result := range batch.renews {
			result <- err
		}
	}
	registration.finish(state)
}

// complete 只关闭一次该 Registration 的诊断和完成通道。
func (registration *registrationCore) complete() {
	registration.doneOnce.Do(func() {
		close(registration.errors)
		close(registration.done)
	})
}

// updateState 校验并写入一条 Version/Data 变化。
// ctx 控制 Redis 调用；state 是唯一协程拥有的可变状态，失败时按确定/不确定结果分别处理。
func (registration *registrationCore) updateState(ctx context.Context, state *registrationState, update registrationUpdateFields) error {
	if update.Version == nil && len(update.Data) == 0 {
		return protocolError(codeContract, "update", 0)
	}
	version := state.version
	if update.Version != nil {
		version = *update.Version
		if version == 0 || version > maxSafeInteger {
			return protocolError(codeInvalid, "@version", 0)
		}
	}
	var changed fields
	for name, value := range update.Data {
		current, exists := state.data[name]
		if !exists {
			return protocolError(codeContract, name, 0)
		}
		if !bytes.Equal(current, value) {
			if changed == nil {
				changed = make(fields, len(update.Data))
			}
			changed[name] = value
		}
	}
	versionChanged := version != state.version
	if !versionChanged && len(changed) == 0 {
		return nil
	}
	if state.revision >= maxSafeInteger {
		return protocolError(codeCapacity, "@revision", state.revision)
	}
	// Update 已拥有输入值；只复制 map，使未变化的不可变字段字节可在相邻期望版本间共享。
	nextData := state.data
	if len(changed) != 0 {
		nextData = cloneFieldMap(state.data)
		for name, value := range changed {
			nextData[name] = value
		}
	}
	nextRevision := state.revision + 1
	limits := registration.client.limits()
	if err := validateRecord(registration.uuid, nextRevision, state.ttl, version, state.attr, nextData, limits); err != nil {
		return err
	}

	// 不确定状态或后端明确落后/缺失时使用完整 Register 恢复；正常路径只发送变化字段。
	var result registrationReply
	var err error
	if state.uncertain {
		result, err = registration.client.callRegistration(ctx, registrationScriptRegister, registration.typeName, registration.uuid,
			registerArguments(registration.uuid, nextRevision, state.ttl, version, state.attr, nextData))
	} else {
		result, err = registration.client.callRegistration(ctx, registrationScriptUpdate, registration.typeName, registration.uuid,
			updateArguments(registration.uuid, nextRevision, versionChanged, version, changed))
		// 异步副本提升后可能落后于已确认 revision；missing/transition 都用完整 Register 恢复。
		if isCode(err, codeMissing) || isCode(err, codeTransition) {
			result, err = registration.client.callRegistration(ctx, registrationScriptRegister, registration.typeName, registration.uuid,
				registerArguments(registration.uuid, nextRevision, state.ttl, version, state.attr, nextData))
		}
	}
	if err == nil {
		err = validateRegistrationSuccess(result, nextRevision)
	}
	if err != nil {
		if uncertainRegistrationOutcome(err) {
			state.revision = nextRevision
			state.version = version
			state.data = nextData
			state.uncertain = true
			state.healthy = false
			registration.revision.Store(nextRevision)
		}
		return err
	}
	state.revision = nextRevision
	state.timestamp = result.timestamp
	state.version = version
	state.data = nextData
	state.uncertain = false
	state.healthy = true
	registration.revision.Store(nextRevision)
	registration.timestamp.Store(result.timestamp)
	return nil
}

// renewState 刷新租约但保持内容 revision。
// 状态不确定、Redis 记录缺失或主节点落后时，以同 UUID/revision 的完整 Register 自愈。
func (registration *registrationCore) renewState(ctx context.Context, state *registrationState) error {
	var result registrationReply
	var err error
	if state.uncertain {
		result, err = registration.client.callRegistration(ctx, registrationScriptRegister, registration.typeName, registration.uuid,
			registerArguments(registration.uuid, state.revision, state.ttl, state.version, state.attr, state.data))
	} else {
		result, err = registration.client.callRegistration(ctx, registrationScriptRenew, registration.typeName, registration.uuid,
			renewArguments(registration.uuid, state.revision))
		if isCode(err, codeMissing) || isCode(err, codeTransition) {
			result, err = registration.client.callRegistration(ctx, registrationScriptRegister, registration.typeName, registration.uuid,
				registerArguments(registration.uuid, state.revision, state.ttl, state.version, state.attr, state.data))
		}
	}
	if err == nil {
		err = validateRegistrationSuccess(result, state.revision)
	}
	if err != nil {
		if uncertainRegistrationOutcome(err) {
			state.uncertain = true
			state.healthy = false
		}
		return err
	}
	state.timestamp = result.timestamp
	state.uncertain = false
	state.healthy = true
	registration.timestamp.Store(result.timestamp)
	return nil
}

// finish 在工作协程退出前执行一次最佳努力终止清理。
// 只有状态健康且结果确定时才发送 Unregister，避免旧请求删除无法确认所有权的状态。
func (registration *registrationCore) finish(state *registrationState) {
	registration.bufferMu.Lock()
	registration.closed = true
	registration.bufferMu.Unlock()
	if !state.healthy || state.uncertain {
		return
	}
	cleanup, cancel := context.WithTimeout(context.Background(), registration.client.config.timeout)
	defer cancel()
	registration.finalErr = registration.unregisterState(cleanup, state)
}

// unregisterState 用精确 UUID 删除当前 Registration；已缺失视为成功。
func (registration *registrationCore) unregisterState(ctx context.Context, state *registrationState) error {
	if !state.healthy || state.uncertain {
		return nil
	}
	_, err := registration.client.callRegistration(ctx, registrationScriptUnregister, registration.typeName, registration.uuid,
		unregisterArguments(registration.uuid))
	if isCode(err, codeMissing) {
		return nil
	}
	return err
}

// report 把非 nil、非正常取消错误同时投递到 Registration 和 Client 有界诊断通道。
func (registration *registrationCore) report(err error) {
	if err == nil || errors.Is(err, context.Canceled) {
		return
	}
	select {
	case registration.errors <- err:
	default:
	}
	registration.client.report(err)
}

// validateRegistrationSuccess 校验成功回复必须匹配期望 revision 且带正 timestamp。
func validateRegistrationSuccess(result registrationReply, revision uint64) error {
	if result.revision != revision || result.timestamp == 0 {
		return protocolError(codeCorrupt, "reply", result.revision)
	}
	return nil
}

// uncertainRegistrationOutcome 判断错误是否表示写入可能已经生效，需要推进本地期望状态后对账。
func uncertainRegistrationOutcome(err error) bool {
	return isCode(err, codeAmbiguous) || isCode(err, codeCorrupt)
}

// jitteredInterval 在 interval 的正负 percent 范围生成均匀抖动，避免大量进程同时续期或刷新。
func jitteredInterval(interval time.Duration, percent int) time.Duration {
	span := interval * time.Duration(percent) / 100
	if span <= 0 {
		return interval
	}
	return interval - span + time.Duration(mathrand.Int64N(int64(span*2)+1))
}

// newRegistrationUUID 使用密码学随机数生成恰好 32 个小写十六进制字符的新进程 UUID。
func newRegistrationUUID() (string, error) {
	var value [16]byte
	if _, err := cryptorand.Read(value[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(value[:]), nil
}

// registerArguments 按 Register Lua v1 固定位置 ABI 构造完整参数。
// Attr 使用点前缀，Data 不加前缀；动态字段按名称排序以稳定字节与测试行为。
func registerArguments(uuid string, revision uint64, ttl uint64, version uint64, attr fields, data fields) []any {
	arguments := make([]any, 0, 4+(len(attr)+len(data))*2)
	arguments = append(arguments,
		uuid,
		strconv.FormatUint(revision, 10),
		strconv.FormatUint(ttl, 10),
		strconv.FormatUint(version, 10),
	)
	for _, name := range sortedFieldNames(attr) {
		arguments = append(arguments, "."+name, attr[name])
	}
	for _, name := range sortedFieldNames(data) {
		arguments = append(arguments, name, data[name])
	}
	return arguments
}

// updateArguments 按 Update Lua v1 固定位置 ABI 构造局部变化参数。
// versionChanged 为 false 时第三个槽位为空字符串；Data 只携带变化字段。
func updateArguments(uuid string, revision uint64, versionChanged bool, version uint64, data fields) []any {
	arguments := make([]any, 0, 3+len(data)*2)
	encodedVersion := ""
	if versionChanged {
		encodedVersion = strconv.FormatUint(version, 10)
	}
	arguments = append(arguments,
		uuid,
		strconv.FormatUint(revision, 10),
		encodedVersion,
	)
	for _, name := range sortedFieldNames(data) {
		arguments = append(arguments, name, data[name])
	}
	return arguments
}

// renewArguments 按 Renew Lua v1 固定位置 ABI 返回 UUID 和当前内容 revision。
func renewArguments(uuid string, revision uint64) []any {
	return []any{
		uuid,
		strconv.FormatUint(revision, 10),
	}
}

// unregisterArguments 按 Unregister Lua v1 固定位置 ABI 返回唯一 UUID 参数。
func unregisterArguments(uuid string) []any {
	return []any{uuid}
}

// bestEffortUnregister 在首次 Register 失败后用独立短超时尽力删除可能残留的同 UUID 状态。
// 结果有意忽略，因为原始初始化错误仍是调用方需要处理的主结果。
func (client *clientRuntime) bestEffortUnregister(typeName string, uuid string) {
	ctx, cancel := context.WithTimeout(context.Background(), client.config.timeout)
	defer cancel()
	_, _ = client.callRegistration(ctx, registrationScriptUnregister, typeName, uuid, unregisterArguments(uuid))
}

// mergeContexts 合并调用方 parent 与对象 owner 的取消信号，不把任一 Context 存入结构体。
// 返回 CancelFunc 会注销 owner 回调并释放派生 Context，调用者必须执行。
func mergeContexts(parent context.Context, owner context.Context) (context.Context, context.CancelFunc) {
	ctx, cancel := context.WithCancel(parent)
	stop := context.AfterFunc(owner, cancel)
	return ctx, func() {
		stop()
		cancel()
	}
}
