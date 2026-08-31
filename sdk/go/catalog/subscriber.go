package catalog

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"strings"
	"sync"
	"sync/atomic"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

type syncWaiter chan error

type syncBatch struct {
	scope     bool
	forceFull bool
	align     bool
	paths     map[Path]struct{}
	waiters   []syncWaiter
}

// Subscriber 为规范化订阅范围持有一个常驻 Pub/Sub 读取协程，并只在存在权威读取工作时临时启动一个同步协程。
// 两者共享内存 Entry，但通过原子状态与有界合并请求协调；稳定期只有读取协程，且不会为每个 Path 创建协程。
type Subscriber struct {
	client       *Client
	subscription normalizedSubscription
	scope        string

	cancel        context.CancelFunc
	releaseClient func()
	pubsub        *redis.PubSub

	entriesMu sync.RWMutex
	entries   map[Path]*Entry
	cursor    uint64
	viewMu    sync.Mutex
	viewBytes int64

	scopeStatus atomic.Uint32
	closed      atomic.Bool
	errors      chan error

	syncMu        sync.Mutex
	pending       syncBatch
	syncRunning   bool
	fenceMu       sync.Mutex
	fences        map[string]chan struct{}
	fencePrefix   string
	fenceID       atomic.Uint64
	channelPrefix string

	persistMu       sync.Mutex
	storeErrorShown atomic.Bool

	workers   atomic.Int32
	stopOnce  sync.Once
	closeDone chan struct{}
}

// Subscriber 先完成全部订阅确认，再进行权威初始读取，并只在请求范围完成对齐后返回。
// ctx 控制初始化；subscription 定义精确或前缀范围。成功对象生命周期与 ctx 脱离。
func (client *Client) Subscriber(
	ctx context.Context,
	subscription Subscription,
) (*Subscriber, error) {
	if ctx == nil || client == nil {
		return nil, newError(verdandi.CodeInvalid, "context", 0, nil)
	}
	if err := ctx.Err(); err != nil {
		return nil, wrapContext(err)
	}
	normalized, err := normalizeSubscription(client.config.Zone, subscription)
	if err != nil {
		return nil, err
	}
	// owner 由常驻读取协程和当前可选同步协程持有；Client 保存 cancel 而不保存长期 Context。
	owner, cancel := context.WithCancel(context.Background())
	releaseClient, err := client.track(cancel)
	if err != nil {
		cancel()
		return nil, err
	}
	subscriber := &Subscriber{
		client:        client,
		subscription:  normalized,
		scope:         normalized.checkpointScope(),
		cancel:        cancel,
		releaseClient: releaseClient,
		entries:       make(map[Path]*Entry),
		errors:        make(chan error, client.config.errorBuffer),
		fences:        make(map[string]chan struct{}),
		closeDone:     make(chan struct{}),
		channelPrefix: zonePrefix(client.config.Zone) + ":",
	}
	subscriber.scopeStatus.Store(uint32(StatusSynchronizing))
	var random [12]byte
	if _, err := rand.Read(random[:]); err != nil {
		subscriber.abortBeforeStart()
		return nil, newError(verdandi.CodeUnavailable, "subscriber", 0, err)
	}
	subscriber.fencePrefix = hex.EncodeToString(random[:])
	subscriber.restoreCheckpoint()
	for _, path := range normalized.exactPaths() {
		subscriber.getOrCreate(path, StatusSynchronizing)
	}

	initialCtx, initialCancel := subscriber.initialContext(ctx, owner)
	defer initialCancel()
	pubsub, err := subscriber.openPubSub(initialCtx)
	if err != nil {
		subscriber.abortBeforeStart()
		return nil, err
	}
	subscriber.pubsub = pubsub
	// 读取协程是唯一常驻任务；初始对齐通过普通请求路径创建一个完成后立即退出的同步协程。
	subscriber.workers.Store(1)
	waiter := make(syncWaiter, 1)
	subscriber.requestScope(owner, false, waiter)
	go subscriber.readLoop(owner)
	select {
	case err := <-waiter:
		if err == nil {
			return subscriber, nil
		}
		subscriber.stop()
		<-subscriber.closeDone
		return nil, err
	case <-initialCtx.Done():
		subscriber.stop()
		<-subscriber.closeDone
		return nil, wrapContext(initialCtx.Err())
	}
}

// Find 返回 covered path 的稳定本地 Entry 指针，不执行 Redis 或磁盘 I/O。
// path 无效、范围外或 Subscriber 已关闭时返回 nil；缺失值仍返回状态为 Absent 的 Entry。
func (subscriber *Subscriber) Find(path Path) *Entry {
	if subscriber == nil || subscriber.closed.Load() ||
		!subscriber.subscription.covers(path) {
		return nil
	}
	status := Status(subscriber.scopeStatus.Load())
	if status == StatusPresent {
		status = StatusAbsent
	}
	return subscriber.getOrCreate(path, status)
}

// Errors 返回尽力而为的有界异步诊断；全部 Subscriber 工作停止后通道关闭。
func (subscriber *Subscriber) Errors() <-chan error {
	if subscriber == nil {
		return nil
	}
	return subscriber.errors
}

// Close 是终止且幂等的操作：取消常驻读取协程和当前可选同步协程、关闭订阅并等待本地状态关闭。
// 它绝不删除 Redis Catalog 数据；ctx 只限制本次等待。
func (subscriber *Subscriber) Close(ctx context.Context) error {
	if subscriber == nil {
		return nil
	}
	if ctx == nil {
		return newError(verdandi.CodeInvalid, "context", 0, nil)
	}
	subscriber.stop()
	select {
	case <-subscriber.closeDone:
		return nil
	case <-ctx.Done():
		return wrapContext(ctx.Err())
	}
}

// initialContext 合并调用方 parent 与 Subscriber owner，并叠加一次同步总超时。
// 返回 CancelFunc 会注销 owner 回调并释放全部派生资源。
func (subscriber *Subscriber) initialContext(parent context.Context, owner context.Context) (context.Context, context.CancelFunc) {
	merged, cancelMerged := context.WithCancel(parent)
	stop := context.AfterFunc(owner, cancelMerged)
	ctx, cancelTimeout := subscriber.client.syncContext(merged)
	return ctx, func() {
		cancelTimeout()
		stop()
		cancelMerged()
	}
}

// openPubSub 创建专用 Pub/Sub 连接，提交所有频道/模式订阅并消费精确数量的确认帧。
// ctx 控制整个过程；失败会关闭连接，不返回半订阅对象。
func (subscriber *Subscriber) openPubSub(ctx context.Context) (*redis.PubSub, error) {
	pubsub := subscriber.client.redis.Subscribe(ctx)
	if len(subscriber.subscription.channels) != 0 {
		if err := pubsub.Subscribe(ctx, subscriber.subscription.channels...); err != nil {
			_ = pubsub.Close()
			return nil, wrapDriver(verdandi.CodeUnavailable, err)
		}
	}
	if len(subscriber.subscription.patterns) != 0 {
		if err := pubsub.PSubscribe(ctx, subscriber.subscription.patterns...); err != nil {
			_ = pubsub.Close()
			return nil, wrapDriver(verdandi.CodeUnavailable, err)
		}
	}
	expected := make(map[string]struct{},
		len(subscriber.subscription.channels)+len(subscriber.subscription.patterns))
	for _, channel := range subscriber.subscription.channels {
		expected["subscribe\x00"+channel] = struct{}{}
	}
	for _, pattern := range subscriber.subscription.patterns {
		expected["psubscribe\x00"+pattern] = struct{}{}
	}
	// 只有所有确认都消费后才允许权威读取，确保读取期间的变化已进入同一连接顺序。
	for len(expected) != 0 {
		frame, err := pubsub.Receive(ctx)
		if err != nil {
			_ = pubsub.Close()
			return nil, wrapDriver(verdandi.CodeUnavailable, err)
		}
		switch frame := frame.(type) {
		case *redis.Subscription:
			delete(expected, frame.Kind+"\x00"+frame.Channel)
		case *redis.Message:
			// 只有全部订阅确认都已消费，才能开始初始权威读取。
		case *redis.Pong:
		default:
			_ = pubsub.Close()
			return nil, newError(verdandi.CodeProtocol, "pubsub", 0, nil)
		}
	}
	return pubsub, nil
}

// restoreCheckpoint 从可选本地存储恢复 cursor 与覆盖范围内 Entry，并统一标记为 synchronizing。
// 检查点只是恢复加速，不是权威源；读取失败只报告一次并继续 Redis 全量同步。
func (subscriber *Subscriber) restoreCheckpoint() {
	if subscriber.client.store == nil {
		return
	}
	cursor, restored, err := subscriber.client.store.load(
		subscriber.client.config.Zone,
		subscriber.scope,
		subscriber.client.config.maxRecordBytes,
	)
	if err != nil {
		subscriber.reportStoreError(err)
		return
	}
	subscriber.cursor = cursor
	var viewBytes int64
	for path, state := range restored {
		if !subscriber.subscription.covers(path) {
			continue
		}
		entry := newEntry(path, StatusSynchronizing)
		next := state.withStatus(StatusSynchronizing)
		viewBytes += int64(next.encodedBytes)
		if subscriber.client.config.maxViewBytes != 0 && viewBytes > subscriber.client.config.maxViewBytes {
			// 检查点只是加速缓存；超出当前本地预算时整份放弃，由 Redis 权威同步重新报告容量。
			subscriber.cursor = 0
			clear(subscriber.entries)
			subscriber.report(newError(verdandi.CodeCapacity, "max_view_bytes", 0, nil))
			return
		}
		entry.state.Store(next)
		subscriber.entries[path] = entry
	}
	subscriber.viewBytes = viewBytes
}

// readLoop 持续消费专用 Pub/Sub 帧，并把变更应用到本地或请求权威修复。
// owner/根关闭时退出；可恢复读取错误会标记 unavailable、触发范围同步并短暂退避。
func (subscriber *Subscriber) readLoop(owner context.Context) {
	defer subscriber.finishWorker()
	defer func() { _ = subscriber.pubsub.Close() }()
	failures := 0
	for {
		frame, err := subscriber.pubsub.Receive(owner)
		if err != nil {
			if owner.Err() != nil {
				return
			}
			if transportClosed(subscriber.client.transportDone) {
				subscriber.client.startClose()
				return
			}
			subscriber.markScope(StatusUnavailable)
			subscriber.report(wrapDriver(verdandi.CodeUnavailable, err))
			subscriber.requestScope(owner, false, nil)
			if waitContext(owner, subscriber.client.config.recoveryDelay(failures)) != nil {
				return
			}
			failures++
			continue
		}
		failures = 0
		switch frame := frame.(type) {
		case *redis.Message:
			subscriber.handleMessage(owner, frame)
		case *redis.Pong:
			subscriber.deliverFence(frame.Payload)
		case *redis.Subscription:
			// 初始确认已在进入循环前消费；此后出现的确认意味着连接重建，必须重新修复视图。
			subscriber.markScope(StatusSynchronizing)
			subscriber.requestScope(owner, false, nil)
		default:
			subscriber.report(newError(verdandi.CodeProtocol, "pubsub", 0, nil))
			subscriber.requestScope(owner, false, nil)
		}
	}
}

// handleMessage 校验频道覆盖、解码事件，并把合法完整操作交给 applyEvent。
func (subscriber *Subscriber) handleMessage(owner context.Context, message *redis.Message) {
	path, ok := subscriber.pathFromChannel(message.Channel)
	if !ok || !subscriber.subscription.covers(path) {
		subscriber.report(newError(verdandi.CodeTarget, "channel", 0, nil))
		subscriber.requestScope(owner, false, nil)
		return
	}
	event, err := decodeEvent(
		message.Payload,
		path,
		subscriber.client.config.maxRecordBytes,
	)
	if err != nil {
		subscriber.report(err)
		subscriber.requestPath(owner, path, nil)
		return
	}
	subscriber.applyEvent(owner, event)
}

// pathFromChannel 从当前 Zone 频道前缀解析 Path；前缀不匹配或成员非法返回 false。
func (subscriber *Subscriber) pathFromChannel(channel string) (Path, bool) {
	if !strings.HasPrefix(channel, subscriber.channelPrefix) {
		return Path{}, false
	}
	return pathFromMember(channel[len(subscriber.channelPrefix):])
}

// applyEvent 用 revision CAS 把一条完整 Replace/Patch/Delete 事件应用到 Entry。
// Patch 基准不匹配或内容校验失败时不猜测状态，而是请求该 Path 权威读取。
func (subscriber *Subscriber) applyEvent(owner context.Context, event catalogEvent) {
	entry := subscriber.getOrCreate(event.path, StatusSynchronizing)
	for {
		current := entry.state.Load()
		if current != nil && event.revision <= current.revision {
			return
		}
		// 先构造完整不可变 next，再用指针 CAS 发布；失败则基于更新后的 current 重试。
		var next *rawState
		switch event.kind {
		case eventReplace:
			next = &rawState{
				revision:        event.revision,
				replaceRevision: event.revision,
				status:          StatusPresent,
				kind:            event.valueKind,
				encodedBytes:    event.encodedBytes,
				fields:          event.fields,
			}
		case eventDelete:
			next = deletedState(event.revision)
		case eventPatch:
			if !completePresent(current) || current.revision != event.baseRevision ||
				current.kind != event.valueKind {
				subscriber.requestPath(owner, event.path, nil)
				return
			}
			fields := cloneFields(current.fields)
			for name, value := range event.fields {
				fields[name] = value
			}
			_, encodedBytes, err := validateValue(
				current.kind,
				fields,
				subscriber.client.config.maxRecordBytes,
			)
			if err != nil || encodedBytes != event.encodedBytes {
				subscriber.report(newError(
					verdandi.CodeCorrupt, "notification", event.revision, err,
				))
				subscriber.requestPath(owner, event.path, nil)
				return
			}
			next = &rawState{
				revision:        event.revision,
				replaceRevision: current.replaceRevision,
				status:          StatusPresent,
				kind:            current.kind,
				encodedBytes:    encodedBytes,
				fields:          fields,
			}
		default:
			subscriber.requestPath(owner, event.path, nil)
			return
		}
		installed, err := subscriber.installState(entry, current, next)
		if err != nil {
			subscriber.report(err)
			subscriber.requestPath(owner, event.path, nil)
			return
		}
		if installed {
			subscriber.persistEntry(entry, next)
			return
		}
	}
}

// installState 在单一短锁内同时提交 Entry CAS 与完整值字节总量，避免并发事件突破本地视图预算。
// 返回 installed=false,nil 表示 base 已变化，调用方可按自己的 revision 规则重试或忽略。
func (subscriber *Subscriber) installState(entry *Entry, base, next *rawState) (bool, error) {
	subscriber.viewMu.Lock()
	defer subscriber.viewMu.Unlock()
	if entry.state.Load() != base {
		return false, nil
	}
	previousBytes := int64(0)
	if base != nil {
		previousBytes = int64(base.encodedBytes)
	}
	nextBytes := int64(0)
	if next != nil {
		nextBytes = int64(next.encodedBytes)
	}
	projected := subscriber.viewBytes - previousBytes + nextBytes
	if projected < 0 {
		return false, newError(verdandi.CodeCorrupt, "max_view_bytes", 0, nil)
	}
	if maximum := subscriber.client.config.maxViewBytes; maximum != 0 && projected > maximum {
		return false, newError(verdandi.CodeCapacity, "max_view_bytes", 0, nil)
	}
	if !entry.state.CompareAndSwap(base, next) {
		return false, nil
	}
	subscriber.viewBytes = projected
	return true, nil
}

// completePresent 判断 state 是否是一份可作为 Patch 基准的完整 Present 值。
func completePresent(state *rawState) bool {
	return state != nil && state.revision != 0 && state.replaceRevision != 0 &&
		state.kind != 0
}

// getOrCreate 返回 path 的稳定 Entry；首次创建时使用 status 初始化。
// 双重锁检查避免已存在 Entry 的写锁开销，并保证每个 Path 只有一个公开指针。
func (subscriber *Subscriber) getOrCreate(path Path, status Status) *Entry {
	subscriber.entriesMu.RLock()
	entry := subscriber.entries[path]
	subscriber.entriesMu.RUnlock()
	if entry != nil {
		return entry
	}
	subscriber.entriesMu.Lock()
	defer subscriber.entriesMu.Unlock()
	if entry = subscriber.entries[path]; entry == nil {
		entry = newEntry(path, status)
		subscriber.entries[path] = entry
	}
	return entry
}

// markScope 更新范围状态，并把当前所有非 Closed Entry 标记为相同状态。
func (subscriber *Subscriber) markScope(status Status) {
	subscriber.scopeStatus.Store(uint32(status))
	subscriber.entriesMu.RLock()
	entries := make([]*Entry, 0, len(subscriber.entries))
	for _, entry := range subscriber.entries {
		entries = append(entries, entry)
	}
	subscriber.entriesMu.RUnlock()
	for _, entry := range entries {
		subscriber.markEntry(entry, status)
	}
}

// markEntry 用 CAS 更新单 Entry 状态，同时保留 revision、kind 和字段值。
func (subscriber *Subscriber) markEntry(entry *Entry, status Status) {
	for {
		current := entry.state.Load()
		if current == nil || current.status == StatusClosed {
			return
		}
		if entry.state.CompareAndSwap(current, current.withStatus(status)) {
			return
		}
	}
}

// requestScope 合并一次范围同步请求；范围请求覆盖所有待处理 Path 请求，并可附带 waiter。
func (subscriber *Subscriber) requestScope(owner context.Context, forceFull bool, waiter syncWaiter) {
	subscriber.syncMu.Lock()
	subscriber.pending.scope = true
	subscriber.pending.forceFull = subscriber.pending.forceFull || forceFull
	subscriber.pending.align = true
	clear(subscriber.pending.paths)
	if waiter != nil {
		subscriber.pending.waiters = append(subscriber.pending.waiters, waiter)
	}
	start := subscriber.startSyncLocked(owner)
	subscriber.syncMu.Unlock()
	if start {
		go subscriber.syncWorker(owner)
	}
}

// requestPath 把一个 Path 标记为 synchronizing，并在没有范围请求时合并进定向同步集合。
func (subscriber *Subscriber) requestPath(owner context.Context, path Path, waiter syncWaiter) {
	entry := subscriber.getOrCreate(path, StatusSynchronizing)
	subscriber.markEntry(entry, StatusSynchronizing)
	subscriber.syncMu.Lock()
	if !subscriber.pending.scope {
		if subscriber.pending.paths == nil {
			subscriber.pending.paths = make(map[Path]struct{})
		}
		if _, exists := subscriber.pending.paths[path]; exists || len(subscriber.pending.paths) < subscriber.client.config.eventBuffer {
			subscriber.pending.paths[path] = struct{}{}
		} else {
			// 大量不同 Path 的修复请求退化为一份范围同步，保持内存有界且不丢失权威恢复能力。
			subscriber.pending.scope = true
			subscriber.pending.align = true
			clear(subscriber.pending.paths)
		}
	}
	if waiter != nil {
		subscriber.pending.waiters = append(subscriber.pending.waiters, waiter)
	}
	start := subscriber.startSyncLocked(owner)
	subscriber.syncMu.Unlock()
	if start {
		go subscriber.syncWorker(owner)
	}
}

// startSyncLocked 在持有 syncMu 时为非空 pending 取得唯一临时同步协程所有权。
// workers 必须在释放锁和启动协程前增加，使关闭等待不可能错过已经获准启动的任务。
func (subscriber *Subscriber) startSyncLocked(owner context.Context) bool {
	if subscriber.syncRunning || subscriber.closed.Load() || owner.Err() != nil {
		return false
	}
	subscriber.syncRunning = true
	subscriber.workers.Add(1)
	return true
}

// takeSyncBatch 在锁下取走当前全部待同步工作；空批次会原子释放临时协程槽。
// 请求要么在释放前进入本批，要么在释放后观察 syncRunning=false 并创建下一协程，因此不会漏唤醒。
func (subscriber *Subscriber) takeSyncBatch() (syncBatch, bool) {
	subscriber.syncMu.Lock()
	defer subscriber.syncMu.Unlock()
	if !subscriber.pending.scope && len(subscriber.pending.paths) == 0 {
		subscriber.syncRunning = false
		return syncBatch{}, false
	}
	batch := subscriber.pending
	subscriber.pending = syncBatch{}
	return batch, true
}

// stopSyncWorker 在 owner 取消且仍有 pending 时释放临时协程槽；终止收尾随后统一失败全部 waiter。
func (subscriber *Subscriber) stopSyncWorker() {
	subscriber.syncMu.Lock()
	subscriber.syncRunning = false
	subscriber.syncMu.Unlock()
}

// drainPending 在终止收尾中取走全部尚未完成的同步请求，不改变已经停止的临时任务状态。
func (subscriber *Subscriber) drainPending() syncBatch {
	subscriber.syncMu.Lock()
	defer subscriber.syncMu.Unlock()
	batch := subscriber.pending
	subscriber.pending = syncBatch{}
	return batch
}

// carryBatch 检查同步期间到达的新工作，把旧 waiter/alignment 责任并入新批次并恢复相应状态标记。
func (subscriber *Subscriber) carryBatch(batch syncBatch) bool {
	subscriber.syncMu.Lock()
	if !subscriber.pending.scope && len(subscriber.pending.paths) == 0 {
		subscriber.syncMu.Unlock()
		return false
	}
	subscriber.pending.align = subscriber.pending.align || batch.align
	subscriber.pending.waiters = append(subscriber.pending.waiters, batch.waiters...)
	pendingScope := subscriber.pending.scope
	pendingPaths := make([]Path, 0, len(subscriber.pending.paths))
	for path := range subscriber.pending.paths {
		pendingPaths = append(pendingPaths, path)
	}
	subscriber.syncMu.Unlock()
	if pendingScope {
		subscriber.markScope(StatusSynchronizing)
	} else {
		for _, path := range pendingPaths {
			subscriber.markEntry(
				subscriber.getOrCreate(path, StatusSynchronizing),
				StatusSynchronizing,
			)
		}
	}
	return true
}

// report 尽力写入有界错误通道；缓冲区满不会阻塞订阅或同步协程。
func (subscriber *Subscriber) report(err error) {
	if err == nil {
		return
	}
	select {
	case subscriber.errors <- err:
	default:
	}
}

// reportStoreError 对本地检查点错误只报告一次，避免持续磁盘故障淹没诊断通道。
func (subscriber *Subscriber) reportStoreError(err error) {
	if err != nil && subscriber.storeErrorShown.CompareAndSwap(false, true) {
		subscriber.report(newError(verdandi.CodeUnavailable, "local_store_path", 0, err))
	}
}

// persistEntry 在可选本地存储中保存 Entry；仅当它仍是当前原子 state 时才写入。
func (subscriber *Subscriber) persistEntry(entry *Entry, state *rawState) {
	if subscriber.client.store == nil {
		return
	}
	subscriber.persistMu.Lock()
	defer subscriber.persistMu.Unlock()
	if entry.state.Load() != state {
		return
	}
	err := subscriber.client.store.saveEntry(
		subscriber.client.config.Zone,
		subscriber.scope,
		entry.path,
		state,
	)
	subscriber.reportStoreError(err)
}

// persistCursor 串行保存当前范围 cursor；本地存储未启用时为空操作。
func (subscriber *Subscriber) persistCursor(revision uint64) {
	if subscriber.client.store == nil {
		return
	}
	subscriber.persistMu.Lock()
	defer subscriber.persistMu.Unlock()
	err := subscriber.client.store.saveCursor(
		subscriber.client.config.Zone,
		subscriber.scope,
		revision,
	)
	subscriber.reportStoreError(err)
}

// stop 只执行一次终止广播，并主动关闭 Pub/Sub 连接以解除阻塞 Receive。
func (subscriber *Subscriber) stop() {
	if subscriber == nil {
		return
	}
	subscriber.stopOnce.Do(func() {
		subscriber.closed.Store(true)
		subscriber.cancel()
		if subscriber.pubsub != nil {
			_ = subscriber.pubsub.Close()
		}
	})
}

// finishWorker 递减当前拥有型任务数；常驻读取或最后一个临时同步协程负责统一终止收尾。
// 该路径避免为了等待动态任务再常驻一个只执行 Wait 的协程。
func (subscriber *Subscriber) finishWorker() {
	if subscriber.workers.Add(-1) != 0 {
		return
	}
	subscriber.closed.Store(true)
	subscriber.markScope(StatusClosed)
	subscriber.failPending(newError(verdandi.CodeClosed, "", 0, nil))
	subscriber.releaseClient()
	close(subscriber.errors)
	close(subscriber.closeDone)
}

// failPending 用 err 非阻塞通知当前批次的全部同步 waiter。
func (subscriber *Subscriber) failPending(err error) {
	batch := subscriber.drainPending()
	for _, waiter := range batch.waiters {
		select {
		case waiter <- err:
		default:
		}
	}
}

// abortBeforeStart 清理尚未启动工作协程的 Subscriber，用于构造阶段失败路径。
func (subscriber *Subscriber) abortBeforeStart() {
	subscriber.closed.Store(true)
	subscriber.cancel()
	subscriber.releaseClient()
	close(subscriber.errors)
	close(subscriber.closeDone)
}

// deliverFence 查找并关闭 payload 对应的 PING waiter；未知或重复 PONG 被忽略。
func (subscriber *Subscriber) deliverFence(payload string) {
	subscriber.fenceMu.Lock()
	waiter := subscriber.fences[payload]
	if waiter != nil {
		delete(subscriber.fences, payload)
	}
	subscriber.fenceMu.Unlock()
	if waiter != nil {
		close(waiter)
	}
}

// pingFence 在专用订阅连接发送唯一 PING，并等待 readLoop 交付完全匹配的 PONG。
// ctx 结束时移除 waiter，防止迟到 PONG 泄漏状态。
func (subscriber *Subscriber) pingFence(ctx context.Context) error {
	payload := subscriber.fencePrefix + "-" +
		formatRevision(subscriber.fenceID.Add(1))
	waiter := make(chan struct{})
	subscriber.fenceMu.Lock()
	subscriber.fences[payload] = waiter
	subscriber.fenceMu.Unlock()
	remove := func() {
		subscriber.fenceMu.Lock()
		delete(subscriber.fences, payload)
		subscriber.fenceMu.Unlock()
	}
	if err := subscriber.pubsub.Ping(ctx, payload); err != nil {
		remove()
		return wrapDriver(verdandi.CodeUnavailable, err)
	}
	select {
	case <-waiter:
		return nil
	case <-ctx.Done():
		remove()
		return wrapContext(ctx.Err())
	}
}

// retryableSyncError 判断一次同步错误是否适合在没有显式 waiter 时自动重新排队。
func retryableSyncError(err error) bool {
	return verdandi.IsCode(err, verdandi.CodeUnavailable) ||
		verdandi.IsCode(err, verdandi.CodeDeadline) ||
		errors.Is(err, context.DeadlineExceeded)
}
