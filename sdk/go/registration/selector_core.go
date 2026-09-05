package registration

import (
	"cmp"
	"context"
	cryptorand "crypto/rand"
	"encoding/hex"
	"errors"
	mathrand "math/rand/v2"
	"net"
	"slices"
	"sync"
	"sync/atomic"
	"time"

	redis "github.com/redis/go-redis/v9"
)

// selectorConfig 标识一个需要同步到本地的 Registry。
type selectorConfig struct {
	// Type 标识 Client Zone 内的 Registry。
	Type string
}

// Meta 是 Selector 调用方可见、由协议管理的 Registration 元数据。
type Meta struct {
	// UUID 是本次服务进程启动的精确身份。
	UUID string
	// Revision 是该 Registration 独立维护的内容版本。
	Revision uint64
	// Timestamp 是最近发布的 Redis Unix 毫秒时间戳。
	Timestamp uint64
	// TTL 是不可变租约时长，单位毫秒。
	TTL uint64
	// Version 是应用定义的正整数版本。
	Version uint64
}

type selectorRecord struct {
	meta          Meta
	attr          fields
	data          fields
	deadline      uint64
	size          int
	projectedAttr any
	projectedData any
}

type retainedSelectorRecord struct {
	record *selectorRecord
	until  uint64
}

type selectorView struct {
	generation      uint64
	synchronized    bool
	records         map[string]*selectorRecord
	orderedRecords  []*selectorRecord
	retained        map[string]retainedSelectorRecord
	orderedRetained []retainedSelectorRecord
}

type selectorState struct {
	records           map[string]*selectorRecord
	deadlines         deadlineQueue
	bytes             int
	retained          map[string]retainedSelectorRecord
	retainedDeadlines deadlineQueue
	retainedBytes     int
}

type subscriptionGeneration struct {
	pubsub    *redis.PubSub
	pendingMu sync.Mutex
	pending   pendingChanges
	pongs     chan string
	failed    chan error
}

type selectorSyncResult struct {
	state selectorState
	clock redisClock
	full  bool
	err   error
}

// selectorCore 持有一个带订阅确认、分页扫描和 PING 栅栏的同步生命周期。
// 它是可变 Registry 状态的唯一所有者，并只向调用方发布不可变本地视图。
type selectorCore struct {
	client      *clientRuntime
	typeName    string
	projectAttr func(fields) (any, error)
	projectData func(fields) (any, error)
	cancel      context.CancelFunc
	release     func()

	errors chan error
	done   chan struct{}
	view   atomic.Pointer[selectorView]

	closeOnce sync.Once
	finalErr  error
}

// selectRegistry 校验 config，接纳一个 Selector 工作协程，并等待首次完整同步完成。
// ctx 只控制构造等待；projectAttr/projectData 在发布视图前把原始字段投影成强类型缓存。
func (client *clientRuntime) selectRegistry(
	ctx context.Context,
	config selectorConfig,
	projectAttr func(fields) (any, error),
	projectData func(fields) (any, error),
) (*selectorCore, error) {
	if client == nil {
		return nil, protocolError(codeClosed, "", 0)
	}
	if ctx == nil {
		return nil, protocolError(codeInvalid, "context", 0)
	}
	if !validType(config.Type) {
		return nil, protocolError(codeInvalid, "type", 0)
	}
	// owner 只传给该 Selector 的工作树；Client 保存 cancel 而不保存 Context。
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
	selector := &selectorCore{
		client:      client,
		typeName:    config.Type,
		projectAttr: projectAttr,
		projectData: projectData,
		cancel:      cancel,
		release:     release,
		errors:      make(chan error, client.config.selectorErrorBuffer),
		done:        make(chan struct{}),
	}
	selector.view.Store(emptySelectorView(0, false))
	ready := make(chan error, 1)
	go selector.run(owner, ready)
	select {
	case err := <-ready:
		if err != nil {
			cancel()
			<-selector.done
			return nil, err
		}
		return selector, nil
	case <-ctx.Done():
		cancel()
		<-selector.done
		return nil, wrapContext(ctx.Err())
	}
}

// Errors 返回同步、订阅和修复的有界异步诊断通道；Selector 关闭后通道关闭。
func (selector *selectorCore) Errors() <-chan error {
	if selector == nil {
		return nil
	}
	return selector.errors
}

// Close 取消同步、关闭专用 Pub/Sub 连接并等待所有拥有的协程退出。
// ctx 只限制本次等待；取消一旦开始不会恢复该 Selector。
func (selector *selectorCore) Close(ctx context.Context) error {
	if selector == nil {
		return nil
	}
	if ctx == nil {
		return protocolError(codeInvalid, "context", 0)
	}
	selector.closeOnce.Do(selector.cancel)
	select {
	case <-selector.done:
		return selector.finalErr
	case <-ctx.Done():
		return wrapContext(ctx.Err())
	}
}

// run 是 Selector 唯一的长期协程，依次建立连接代、运行同步/监听并在失败后退避重试。
// owner 由 Selector.Close 或 Client.Close 取消；ready 只汇报首次同步是否可用。
func (selector *selectorCore) run(owner context.Context, ready chan<- error) {
	defer func() {
		selector.view.Store(emptySelectorView(0, false))
		close(selector.errors)
		selector.release()
		close(selector.done)
	}()
	first := true
	failures := uint(0)
	generationNumber := uint64(0)
	state := newSelectorState(0)
	var clock redisClock
	clockValid := false
	// 每次重连创建独立 generation；旧代在发布不可用视图后完全关闭，不跨代复用 Pub/Sub 状态。
	for owner.Err() == nil {
		openContext, cancel := context.WithTimeout(owner, selector.client.config.selectorSyncTimeout)
		generation, err := selector.openGeneration(openContext)
		cancel()
		if err == nil {
			previousGeneration := generationNumber
			state, clock, clockValid, err = selector.listenGeneration(
				owner,
				generation,
				state,
				clock,
				clockValid,
				&generationNumber,
				&first,
				ready,
			)
			if generationNumber != previousGeneration {
				failures = 0
			}
			generation.close()
		}
		if owner.Err() != nil {
			return
		}
		if transportClosed(selector.client.transportDone) {
			selector.client.startClose()
			return
		}
		if clockValid {
			selector.expire(&state, clock.upperNow())
		}
		selector.publish(state, generationNumber, false)
		selector.report(err)
		if first && errors.Is(err, context.DeadlineExceeded) {
			ready <- wrapError(codeDeadline, err)
			first = false
			return
		}
		if !waitContext(owner, selectorRetryDelay(
			failures,
			selector.client.config.selectorRecoveryInitial,
			selector.client.config.selectorRecoveryMax,
			selector.client.config.selectorRecoveryFactor,
			selector.client.config.selectorRecoveryJitter,
		)) {
			return
		}
		failures++
	}
	if first {
		ready <- protocolError(codeClosed, "", 0)
	}
}

// listenGeneration 在当前连接代内由唯一长期 Selector 协程执行。
// 它接收 Pub/Sub、独占可变状态，并且最多启动一个临时的完整或定向同步协程。
func (selector *selectorCore) listenGeneration(
	owner context.Context,
	generation *subscriptionGeneration,
	state selectorState,
	clock redisClock,
	clockValid bool,
	generationNumber *uint64,
	first *bool,
	ready chan<- error,
) (selectorState, redisClock, bool, error) {
	var syncResult chan selectorSyncResult
	var syncCancel context.CancelFunc
	synchronizing := false
	synchronized := false
	clockRefreshAt := time.Time{}
	publishAt := time.Time{}
	advanceGeneration := false

	// 临时同步结果通过容量一通道回传；取消后仍等待结果，保证 generation 关闭前没有遗留协程。
	startSynchronization := func(previous selectorState, repair map[string]struct{}) {
		ctx, cancel := context.WithTimeout(owner, selector.client.config.selectorSyncTimeout)
		result := make(chan selectorSyncResult, 1)
		syncResult = result
		syncCancel = cancel
		synchronizing = true
		go func() {
			if len(repair) == 0 {
				next, nextClock, err := selector.synchronize(ctx, generation, previous)
				result <- selectorSyncResult{state: next, clock: nextClock, full: true, err: err}
				return
			}
			next, err := selector.repairSnapshot(ctx, generation, previous, repair, clock)
			result <- selectorSyncResult{state: next, clock: clock, err: err}
		}()
	}
	stopSynchronization := func() {
		if syncCancel != nil {
			syncCancel()
			syncCancel = nil
		}
		synchronizing = false
		syncResult = nil
	}
	defer func() {
		if synchronizing {
			syncCancel()
			<-syncResult
		}
	}()

	// 视图发布可按配置合并；零间隔保持每次变化立即发布的确定语义。
	markDirty := func() {
		if selector.client.config.selectorPublishInterval == 0 {
			selector.publish(state, *generationNumber, true)
			return
		}
		if publishAt.IsZero() {
			publishAt = time.Now().Add(selector.client.config.selectorPublishInterval)
		}
	}
	startSynchronization(state, nil)

	for {
		if synchronizing {
			select {
			case result := <-syncResult:
				stopSynchronization()
				if result.err != nil {
					generation.fail(result.err)
					return state, clock, clockValid, result.err
				}
				state = result.state
				clock = result.clock
				clockValid = true
				advanceGeneration = advanceGeneration || result.full
				_, repair, err := selector.applyPending(&state, generation.drain(), protocolZoneConfig(), clock)
				if err != nil {
					generation.fail(err)
					return state, clock, clockValid, err
				}
				if len(repair) != 0 {
					startSynchronization(state, repair)
					continue
				}
				selector.expire(&state, clock.upperNow())
				if advanceGeneration {
					*generationNumber = *generationNumber + 1
					advanceGeneration = false
				}
				synchronized = true
				clockRefreshAt = time.Now().Add(selector.client.config.clockRefresh)
				publishAt = time.Time{}
				selector.publish(state, *generationNumber, true)
				if *first {
					ready <- nil
					*first = false
				}
			default:
			}
		}

		now := time.Now()
		if synchronized && !clockRefreshAt.IsZero() && !now.Before(clockRefreshAt) {
			next, err := selector.client.calibrateClock(owner)
			if err != nil {
				generation.fail(err)
				return state, clock, clockValid, err
			}
			clock = next
			clockRefreshAt = now.Add(selector.client.config.clockRefresh)
			if selector.expire(&state, clock.upperNow()) > 0 {
				markDirty()
			}
		}
		if synchronized {
			if deadline, ok := selector.nextDeadline(&state); ok && deadline <= clock.upperNow() {
				if selector.expire(&state, clock.upperNow()) > 0 {
					markDirty()
				}
			}
			if !publishAt.IsZero() && !now.Before(publishAt) {
				publishAt = time.Time{}
				selector.publish(state, *generationNumber, true)
			}
		}

		timeout := selectorReceiveTimeout(synchronizing, synchronized, &state, clock, clockRefreshAt, publishAt, selector)
		// go-redis 的读取 timeout 不覆盖连接断开后的同步自动重连；额外 Context 截止保证
		// 一次接收连同驱动内部恢复都不会阻塞 Selector 发布 unavailable。
		receiveContext, receiveCancel := context.WithTimeout(owner, timeout)
		raw, err := generation.pubsub.ReceiveTimeout(receiveContext, timeout)
		receiveCancel()
		if err != nil {
			if owner.Err() != nil {
				return state, clock, clockValid, owner.Err()
			}
			if transportClosed(selector.client.transportDone) {
				selector.client.startClose()
				return state, clock, clockValid, context.Canceled
			}
			if isReceiveTimeout(err) {
				continue
			}
			err = wrapDriver(codeUnavailable, err)
			generation.fail(err)
			return state, clock, clockValid, err
		}
		switch value := raw.(type) {
		case *redis.Message:
			event, decodeErr := decodeRegistrationEvent([]byte(value.Payload), protocolZoneConfig())
			if decodeErr != nil {
				generation.fail(decodeErr)
				return state, clock, clockValid, decodeErr
			}
			if addErr := generation.add(event); addErr != nil {
				generation.fail(addErr)
				return state, clock, clockValid, addErr
			}
			if !synchronized || synchronizing {
				continue
			}
			changed, repair, applyErr := selector.applyPending(&state, generation.drain(), protocolZoneConfig(), clock)
			if applyErr != nil {
				generation.fail(applyErr)
				return state, clock, clockValid, applyErr
			}
			if len(repair) != 0 {
				synchronized = false
				publishAt = time.Time{}
				selector.markUnavailable(*generationNumber)
				startSynchronization(state, repair)
				continue
			}
			if changed {
				markDirty()
			}
		case *redis.Pong:
			if !synchronizing {
				err := protocolError(codeCorrupt, "pong", 0)
				generation.fail(err)
				return state, clock, clockValid, err
			}
			select {
			case generation.pongs <- value.Payload:
			default:
				err := protocolError(codeCorrupt, "pong", 0)
				generation.fail(err)
				return state, clock, clockValid, err
			}
		case *redis.Subscription:
			err := protocolError(codeUnavailable, "subscription_generation", 0)
			generation.fail(err)
			return state, clock, clockValid, err
		default:
			err := protocolError(codeCorrupt, "subscription", 0)
			generation.fail(err)
			return state, clock, clockValid, err
		}
	}
}

// selectorReceiveTimeout 计算下一次 Pub/Sub ReceiveTimeout 的最短安全等待。
// 它综合同步轮询、时钟校准、视图发布和最近租约截止时间，并保证至少一毫秒。
func selectorReceiveTimeout(
	synchronizing bool,
	synchronized bool,
	state *selectorState,
	clock redisClock,
	clockRefreshAt time.Time,
	publishAt time.Time,
	selector *selectorCore,
) time.Duration {
	timeout := time.Second
	if synchronizing {
		timeout = 10 * time.Millisecond
	}
	now := time.Now()
	// 每个本地截止点只可缩短等待，不能延迟更早的协议工作。
	shorten := func(deadline time.Time) {
		if deadline.IsZero() {
			return
		}
		delay := deadline.Sub(now)
		if delay < 0 {
			delay = 0
		}
		if delay < timeout {
			timeout = delay
		}
	}
	if synchronized {
		shorten(clockRefreshAt)
		shorten(publishAt)
		if deadline, ok := selector.nextDeadline(state); ok {
			nowMilliseconds := clock.upperNow()
			delay := time.Duration(0)
			if deadline > nowMilliseconds {
				delta := deadline - nowMilliseconds
				if delta > uint64((1<<63-1)/int64(time.Millisecond)) {
					delta = uint64((1<<63 - 1) / int64(time.Millisecond))
				}
				delay = time.Duration(delta) * time.Millisecond
			}
			if delay < timeout {
				timeout = delay
			}
		}
	}
	if timeout <= 0 {
		return time.Millisecond
	}
	return timeout
}

// isReceiveTimeout 判断 err 是否为网络层超时；这种超时用于驱动本地定时工作，不代表连接代失败。
func isReceiveTimeout(err error) bool {
	timeout, ok := errors.AsType[net.Error](err)
	return ok && timeout.Timeout()
}

// openGeneration 在专用 Pub/Sub 连接上订阅 Registry，并等待精确 subscribe 确认。
// ctx 控制建连和确认；失败会关闭连接，不返回半初始化 generation。
func (selector *selectorCore) openGeneration(ctx context.Context) (*subscriptionGeneration, error) {
	channel := registryKey(selector.client.config.Zone, selector.typeName)
	pubsub := selector.client.redis.Subscribe(ctx, channel)
	acknowledgement, err := pubsub.Receive(ctx)
	if err != nil {
		_ = pubsub.Close()
		return nil, wrapDriver(codeUnavailable, err)
	}
	subscription, ok := acknowledgement.(*redis.Subscription)
	if !ok || subscription.Kind != "subscribe" || subscription.Channel != channel {
		_ = pubsub.Close()
		return nil, protocolError(codeCorrupt, "subscribe", 0)
	}
	generation := &subscriptionGeneration{
		pubsub: pubsub,
		pending: newPendingChanges(
			selector.client.config.selectorEventBuffer,
			selector.client.config.selectorEventBytes,
		),
		pongs:  make(chan string, 1),
		failed: make(chan error, 1),
	}
	return generation, nil
}

// add 在 generation 锁下把一个已解码事件合并到每 UUID 有界缓冲区。
func (generation *subscriptionGeneration) add(event registrationEvent) error {
	generation.pendingMu.Lock()
	defer generation.pendingMu.Unlock()
	return generation.pending.add(event)
}

// drain 在 generation 锁下取走所有待处理变化，并清空缓冲区。
func (generation *subscriptionGeneration) drain() []pendingChange {
	generation.pendingMu.Lock()
	defer generation.pendingMu.Unlock()
	return generation.pending.drain()
}

// fail 尽力记录当前 generation 的首个失败，供同步协程的 PONG 等待立即退出。
func (generation *subscriptionGeneration) fail(err error) {
	select {
	case generation.failed <- err:
	default:
	}
}

// waitPong 等待与 nonce 精确匹配的订阅连接 PONG。
// generation 失败或 ctx 结束会提前返回；错误 PONG 被视为协议损坏。
func (generation *subscriptionGeneration) waitPong(ctx context.Context, nonce string) error {
	select {
	case pong := <-generation.pongs:
		if pong != nonce {
			return protocolError(codeCorrupt, "pong", 0)
		}
		return nil
	case err := <-generation.failed:
		return err
	case <-ctx.Done():
		return wrapContext(ctx.Err())
	}
}

// close 关闭 generation 的专用 Pub/Sub 连接；nil 接收者安全且重复关闭由驱动吸收。
func (generation *subscriptionGeneration) close() {
	if generation == nil {
		return
	}
	_ = generation.pubsub.Close()
}

// synchronize 执行一次完整 subscribe-before-scan-PING 恢复。
// previous 只作为过期保留候选来源；返回的新 state 在完成栅栏和所有定向修复前不会发布。
func (selector *selectorCore) synchronize(ctx context.Context, generation *subscriptionGeneration, previous selectorState) (selectorState, redisClock, error) {
	clock, err := selector.client.calibrateClock(ctx)
	if err != nil {
		return selectorState{}, redisClock{}, err
	}
	limits := protocolZoneConfig()
	state := selector.recoveryState(previous, clock.upperNow())
	registry := registryKey(selector.client.config.Zone, selector.typeName)
	// Registry HSCAN 只读取 UUID/revision 提示；每页再批量读取对应 Registration 记录。
	var cursor uint64
	for {
		commandCtx, cancel := selector.client.commandContext(ctx)
		page, next, err := selector.client.redis.HScan(commandCtx, registry, cursor, "", int64(selector.client.config.selectorPageSize)).Result()
		cancel()
		if err != nil {
			return selectorState{}, redisClock{}, wrapDriver(codeUnavailable, err)
		}
		if len(page)%2 != 0 {
			return selectorState{}, redisClock{}, protocolError(codeCorrupt, "registry", 0)
		}
		hints := make(map[string]uint64, len(page)/2)
		for index := 0; index < len(page); index += 2 {
			uuid := page[index]
			revision, parseErr := parseCanonicalUint(page[index+1])
			if !validUUID(uuid) || parseErr != nil {
				return selectorState{}, redisClock{}, protocolError(codeCorrupt, "registry", 0)
			}
			hints[uuid] = revision
		}
		if err := selector.fetchRecords(ctx, &state, hints, limits, clock); err != nil {
			return selectorState{}, redisClock{}, err
		}
		cursor = next
		if cursor == 0 {
			break
		}
	}
	repair, err := selector.fence(ctx, generation, &state, limits, clock)
	if err != nil {
		return selectorState{}, redisClock{}, err
	}
	for attempts := 0; len(repair) > 0 && attempts < 3; attempts++ {
		repair, err = selector.repair(ctx, generation, &state, repair, limits, clock)
		if err != nil {
			return selectorState{}, redisClock{}, err
		}
	}
	if len(repair) != 0 {
		return selectorState{}, redisClock{}, protocolError(codeTransition, "selector_repair", 0)
	}
	selector.expire(&state, clock.upperNow())
	return state, clock, nil
}

// repairSnapshot 从 previous 克隆一个临时状态，并对指定 UUID 集合最多执行三轮定向修复。
// 仍无法收敛时返回 transition，调用方会放弃当前连接代并重新完整同步。
func (selector *selectorCore) repairSnapshot(
	ctx context.Context,
	generation *subscriptionGeneration,
	previous selectorState,
	repair map[string]struct{},
	clock redisClock,
) (selectorState, error) {
	state := cloneSelectorState(previous)
	limits := protocolZoneConfig()
	var err error
	for attempts := 0; len(repair) > 0 && attempts < 3; attempts++ {
		repair, err = selector.repair(ctx, generation, &state, repair, limits, clock)
		if err != nil {
			return selectorState{}, err
		}
	}
	if len(repair) != 0 {
		return selectorState{}, protocolError(codeTransition, "selector_repair", 0)
	}
	selector.expire(&state, clock.upperNow())
	return state, nil
}

// fetchRecords 根据 Registry revision 提示批量读取 Registration。
// 同 revision 的缓存只 HMGET revision/timestamp；新版本或缺失缓存才 HGETALL，所有结果事务性安装到 state。
func (selector *selectorCore) fetchRecords(ctx context.Context, state *selectorState, hints map[string]uint64, limits zoneConfig, clock redisClock) error {
	if len(hints) == 0 {
		return nil
	}
	uuidList := make([]string, 0, len(hints))
	for uuid := range hints {
		uuidList = append(uuidList, uuid)
	}
	slices.Sort(uuidList)
	headers := make(map[string]*redis.SliceCmd, len(uuidList))
	full := make(map[string]*redis.MapStringStringCmd, len(uuidList))
	// 第一批根据缓存 revision 选择两字段头部读取或完整 HGETALL，所有命令共用一次 Pipeline 往返。
	commandCtx, cancel := selector.client.commandContext(ctx)
	_, err := selector.client.redis.Pipelined(commandCtx, func(pipe redis.Pipeliner) error {
		for _, uuid := range uuidList {
			cached, _ := stateRecord(state, uuid)
			key := registrationKey(selector.client.config.Zone, selector.typeName, uuid)
			if cached != nil && cached.meta.Revision == hints[uuid] {
				headers[uuid] = pipe.HMGet(commandCtx, key, "@revision", "@timestamp")
			} else {
				full[uuid] = pipe.HGetAll(commandCtx, key)
			}
		}
		return nil
	})
	cancel()
	if err != nil {
		return wrapDriver(codeUnavailable, err)
	}
	fallback := make([]string, 0)
	for _, uuid := range uuidList {
		if command := full[uuid]; command != nil {
			values, resultErr := command.Result()
			if resultErr != nil {
				return wrapDriver(codeUnavailable, resultErr)
			}
			if err := selector.installFetchedRecord(state, uuid, hints[uuid], values, limits, clock); err != nil {
				return err
			}
			continue
		}
		values, resultErr := headers[uuid].Result()
		if resultErr != nil {
			return wrapDriver(codeUnavailable, resultErr)
		}
		if len(values) != 2 {
			return protocolError(codeCorrupt, "registration_header", 0)
		}
		if values[0] == nil || values[1] == nil {
			selector.retainUUID(state, uuid, clock.upperNow())
			continue
		}
		revisionText, revisionOK := values[0].(string)
		timestampText, timestampOK := values[1].(string)
		if !revisionOK || !timestampOK {
			return protocolError(codeCorrupt, "registration_header", 0)
		}
		revision, revisionErr := parseCanonicalUint(revisionText)
		timestamp, timestampErr := parseCanonicalUint(timestampText)
		if revisionErr != nil || timestampErr != nil {
			return protocolError(codeCorrupt, "registration_header", 0)
		}
		if revision < hints[uuid] {
			return protocolError(codeTransition, "@revision", revision)
		}
		cached, _ := stateRecord(state, uuid)
		if cached == nil || revision != cached.meta.Revision {
			fallback = append(fallback, uuid)
			continue
		}
		if timestamp > maxHashFieldExpireAtMilliseconds-cached.meta.TTL {
			return protocolError(codeCorrupt, "@timestamp", 0)
		}
		next := cloneSelectorRecord(cached)
		next.meta.Timestamp = timestamp
		next.deadline = timestamp + next.meta.TTL
		if next.deadline <= clock.upperNow() {
			selector.retainRecord(state, next, clock.upperNow())
		} else {
			if err := selector.setRecord(state, next); err != nil {
				return err
			}
		}
	}
	if len(fallback) != 0 {
		// 头部显示 revision 已变化时，对这些 UUID 再做一批完整读取；不会为同 revision 记录支付 HGETALL 成本。
		commands := make([]*redis.MapStringStringCmd, len(fallback))
		commandCtx, cancel := selector.client.commandContext(ctx)
		_, err := selector.client.redis.Pipelined(commandCtx, func(pipe redis.Pipeliner) error {
			for index, uuid := range fallback {
				commands[index] = pipe.HGetAll(commandCtx, registrationKey(selector.client.config.Zone, selector.typeName, uuid))
			}
			return nil
		})
		cancel()
		if err != nil {
			return wrapDriver(codeUnavailable, err)
		}
		for index, uuid := range fallback {
			values, resultErr := commands[index].Result()
			if resultErr != nil {
				return wrapDriver(codeUnavailable, resultErr)
			}
			if err := selector.installFetchedRecord(state, uuid, hints[uuid], values, limits, clock); err != nil {
				return err
			}
		}
	}
	selector.expire(state, clock.upperNow())
	return nil
}

// installFetchedRecord 校验一条权威读取，并根据租约时间安装为活动或 retained 记录。
// hint 是 Registry 中观察到的 revision；不一致表示并发变化，需要后续 PING 栅栏修复。
func (selector *selectorCore) installFetchedRecord(
	state *selectorState,
	uuid string,
	hint uint64,
	values map[string]string,
	limits zoneConfig,
	clock redisClock,
) error {
	if len(values) == 0 {
		selector.retainUUID(state, uuid, clock.upperNow())
		return nil
	}
	record, err := parseStoredRecord(uuid, values, limits)
	if err != nil {
		return err
	}
	if record.meta.Revision < hint {
		return protocolError(codeTransition, "@revision", record.meta.Revision)
	}
	if record.deadline <= clock.upperNow() {
		if err := selector.projectRecord(record); err != nil {
			return err
		}
		selector.retainRecord(state, record, clock.upperNow())
	} else {
		if err := selector.setRecord(state, record); err != nil {
			return err
		}
	}
	return nil
}

// fence 在同一已订阅连接发送唯一 PING，等待 PONG 后应用此前缓冲的全部事件。
// 返回仍需权威回读的 UUID 集合；PONG 证明扫描前后的消息顺序边界。
func (selector *selectorCore) fence(
	ctx context.Context,
	generation *subscriptionGeneration,
	state *selectorState,
	limits zoneConfig,
	clock redisClock,
) (map[string]struct{}, error) {
	nonce, err := newNonce()
	if err != nil {
		return nil, wrapError(codeUnavailable, err)
	}
	commandCtx, cancel := selector.client.commandContext(ctx)
	err = generation.pubsub.Ping(commandCtx, nonce)
	cancel()
	if err != nil {
		return nil, wrapDriver(codeUnavailable, err)
	}
	if err := generation.waitPong(ctx, nonce); err != nil {
		return nil, err
	}
	_, repair, err := selector.applyPending(state, generation.drain(), limits, clock)
	return repair, err
}

// repair 对 UUID 集合执行一次权威批量读取，再通过新 PING/PONG 收拢并发事件。
func (selector *selectorCore) repair(
	ctx context.Context,
	generation *subscriptionGeneration,
	state *selectorState,
	repair map[string]struct{},
	limits zoneConfig,
	clock redisClock,
) (map[string]struct{}, error) {
	hints := make(map[string]uint64, len(repair))
	for uuid := range repair {
		hints[uuid] = 1
	}
	if err := selector.fetchRecords(ctx, state, hints, limits, clock); err != nil && !isCode(err, codeTransition) {
		return nil, err
	}
	return selector.fence(ctx, generation, state, limits, clock)
}

// applyPending 按 UUID 顺序把合并变化应用到临时 state。
// 返回视图是否变化、需要定向修复的 UUID 和首个不可恢复错误。
func (selector *selectorCore) applyPending(
	state *selectorState,
	changes []pendingChange,
	limits zoneConfig,
	clock redisClock,
) (bool, map[string]struct{}, error) {
	changed := false
	var repair map[string]struct{}
	for _, change := range changes {
		if change.repair {
			if repair == nil {
				repair = make(map[string]struct{}, len(changes))
			}
			repair[change.event.uuid] = struct{}{}
			continue
		}
		applied, needsRepair, err := selector.applyPendingChange(state, change, limits, clock)
		if err != nil {
			return false, nil, err
		}
		changed = changed || applied
		if needsRepair {
			if repair == nil {
				repair = make(map[string]struct{}, len(changes))
			}
			repair[change.event.uuid] = struct{}{}
		}
	}
	return changed, repair, nil
}

// applyPendingChange 处理一条合并变化；Update 额外检查 baseRevision，其他生命周期交给 applyEvent。
func (selector *selectorCore) applyPendingChange(state *selectorState, change pendingChange, limits zoneConfig, clock redisClock) (bool, bool, error) {
	if change.event.kind != "update" {
		return selector.applyEvent(state, change.event, limits, clock)
	}
	current, _ := stateRecord(state, change.event.uuid)
	if current == nil {
		return false, true, nil
	}
	if change.event.revision <= current.meta.Revision {
		return false, false, nil
	}
	if current.meta.Revision < change.baseRevision {
		return false, true, nil
	}

	return selector.applyUpdate(state, current, change.event, limits, clock)
}

// applyUpdate 把一条连续 Update Patch 应用到 current 的克隆。
// 返回 changed、needsRepair 和错误；字段结构、容量和租约均在发布前重新校验。
func (selector *selectorCore) applyUpdate(
	state *selectorState,
	current *selectorRecord,
	event registrationEvent,
	limits zoneConfig,
	clock redisClock,
) (bool, bool, error) {
	next := cloneSelectorRecord(current)
	next.meta.Revision = event.revision
	next.meta.Timestamp = event.timestamp
	if event.hasVersion {
		next.meta.Version = event.version
	}
	// 解码已校验 Patch 名称和值；固定字段结构与缓存字节差使完整记录约束可在 Patch 时间维护。
	next.size += decimalDigits(next.meta.Revision) - decimalDigits(current.meta.Revision)
	next.size += decimalDigits(next.meta.Version) - decimalDigits(current.meta.Version)
	if len(event.data) != 0 {
		next.projectedData = nil
	}
	for name, value := range event.data {
		previous, exists := next.data[name]
		if !exists {
			return false, true, nil
		}
		next.size += len(value) - len(previous)
		next.data[name] = value
	}
	if next.size > limits.recordMaxBytes {
		return false, false, protocolError(codeCapacity, "registration", 0)
	}
	if event.timestamp > maxHashFieldExpireAtMilliseconds-next.meta.TTL {
		return false, false, protocolError(codeCorrupt, "@timestamp", 0)
	}
	next.deadline = event.timestamp + next.meta.TTL
	if next.deadline <= clock.upperNow() {
		if err := selector.projectRecord(next); err != nil {
			return false, false, err
		}
		return selector.retainRecord(state, next, clock.upperNow()), false, nil
	}
	if err := selector.setRecord(state, next); err != nil {
		return false, false, err
	}
	return true, false, nil
}

// applyEvent 对 register/update/renew/unregister 执行统一的版本、时间戳和租约状态转换。
// 第二个 bool 表示无法凭事件安全决定，需要权威回读，而不是立即丢弃本地状态。
func (selector *selectorCore) applyEvent(state *selectorState, event registrationEvent, limits zoneConfig, clock redisClock) (bool, bool, error) {
	current, active := stateRecord(state, event.uuid)
	switch event.kind {
	case "unregister":
		if current == nil {
			return false, false, nil
		}
		selector.removeRecord(state, event.uuid)
		return true, false, nil
	case "register":
		if current != nil && event.revision < current.meta.Revision {
			return false, false, nil
		}
		next := &selectorRecord{
			meta: Meta{UUID: event.uuid, Revision: event.revision, Timestamp: event.timestamp, TTL: event.ttl, Version: event.version},
			attr: event.attr,
			data: event.data,
			size: registrationSize(event.uuid, event.revision, event.ttl, event.version, event.attr, event.data),
		}
		if event.timestamp > maxHashFieldExpireAtMilliseconds-event.ttl {
			return false, false, protocolError(codeCorrupt, "@timestamp", 0)
		}
		next.deadline = event.timestamp + event.ttl
		if current != nil && event.revision == current.meta.Revision {
			if current.meta.Version != next.meta.Version || current.meta.TTL != next.meta.TTL ||
				!fieldsEqual(current.attr, next.attr) || !fieldsEqual(current.data, next.data) {
				return false, true, nil
			}
			if current.meta.Timestamp > next.meta.Timestamp {
				next.meta.Timestamp = current.meta.Timestamp
				next.deadline = current.deadline
			}
		}
		if next.deadline <= clock.upperNow() {
			if err := selector.projectRecord(next); err != nil {
				return false, false, err
			}
			return selector.retainRecord(state, next, clock.upperNow()), false, nil
		}
		if err := selector.setRecord(state, next); err != nil {
			return false, false, err
		}
		return !active || current == nil || current.meta.Revision != next.meta.Revision ||
			current.meta.Version != next.meta.Version || !fieldsEqual(current.attr, next.attr) || !fieldsEqual(current.data, next.data), false, nil
	case "update":
		if current == nil {
			return false, true, nil
		}
		if event.revision <= current.meta.Revision {
			return false, false, nil
		}
		if event.revision != current.meta.Revision+1 {
			return false, true, nil
		}
		return selector.applyUpdate(state, current, event, limits, clock)
	case "renew":
		if current == nil {
			return false, true, nil
		}
		if event.revision < current.meta.Revision {
			return false, false, nil
		}
		if event.revision > current.meta.Revision {
			return false, true, nil
		}
		if event.timestamp <= current.meta.Timestamp {
			return false, false, nil
		}
		next := cloneSelectorRecord(current)
		next.meta.Timestamp = event.timestamp
		if event.timestamp > maxHashFieldExpireAtMilliseconds-next.meta.TTL {
			return false, false, protocolError(codeCorrupt, "@timestamp", 0)
		}
		next.deadline = event.timestamp + next.meta.TTL
		if next.deadline <= clock.upperNow() {
			return selector.retainRecord(state, next, clock.upperNow()), false, nil
		}
		if err := selector.setRecord(state, next); err != nil {
			return false, false, err
		}
		return !active, false, nil
	default:
		return false, false, protocolError(codeInvalid, "&kind", 0)
	}
}

// setRecord 把 record 安装为活动候选，并维护活动字节、租约堆和 retained 去重。
// 超过 SelectorMaxBytes 或强类型投影失败时不发布记录。
func (selector *selectorCore) setRecord(state *selectorState, record *selectorRecord) error {
	if record.size == 0 {
		record.size = registrationSize(record.meta.UUID, record.meta.Revision, record.meta.TTL, record.meta.Version, record.attr, record.data)
	}
	previousSize := 0
	if previous := state.records[record.meta.UUID]; previous != nil {
		previousSize = selectorRecordSize(previous)
	}
	nextBytes := state.bytes - previousSize + selectorRecordSize(record)
	if nextBytes > selector.client.config.selectorMaxBytes {
		return protocolError(codeCapacity, "selector_view", 0)
	}
	if err := selector.projectRecord(record); err != nil {
		return err
	}
	selector.removeRetained(state, record.meta.UUID)
	state.records[record.meta.UUID] = record
	state.deadlines.set(record.meta.UUID, record.deadline)
	state.bytes = nextBytes
	return nil
}

// projectRecord 惰性构造 Attr/Data 强类型投影，并缓存到不可变 record。
// 已投影字段不会重复解码；任一投影失败会阻止该记录进入可见视图。
func (selector *selectorCore) projectRecord(record *selectorRecord) error {
	if selector.projectAttr != nil && record.projectedAttr == nil {
		projected, err := selector.projectAttr(record.attr)
		if err != nil {
			return err
		}
		record.projectedAttr = projected
	}
	if selector.projectData != nil && record.projectedData == nil {
		projected, err := selector.projectData(record.data)
		if err != nil {
			return err
		}
		record.projectedData = projected
	}
	return nil
}

// removeRecord 只用于明确 Unregister 的终止删除。
// 自然过期或权威读取暂时缺失必须使用 retainRecord，给服务恢复留出一个额外 TTL。
func (selector *selectorCore) removeRecord(state *selectorState, uuid string) {
	if previous := state.records[uuid]; previous != nil {
		state.bytes -= selectorRecordSize(previous)
	}
	delete(state.records, uuid)
	state.deadlines.remove(uuid)
	selector.removeRetained(state, uuid)
}

// removeRetained 删除 uuid 的保留记录和截止堆项，并报告是否存在。
func (selector *selectorCore) removeRetained(state *selectorState, uuid string) bool {
	previous, exists := state.retained[uuid]
	if exists {
		state.retainedBytes -= selectorRecordSize(previous.record)
		delete(state.retained, uuid)
	}
	state.retainedDeadlines.remove(uuid)
	return exists
}

// retainRecord 把活动/已有记录移动到不可选择视图，默认保留到原 deadline 后再延长一个 TTL。
func (selector *selectorCore) retainRecord(state *selectorState, record *selectorRecord, now uint64) bool {
	if record == nil {
		return false
	}
	until := record.deadline + record.meta.TTL
	if until < record.deadline {
		until = ^uint64(0)
	}
	return selector.setRetained(state, record, until, now)
}

// retainUUID 查找 uuid 当前活动记录并按自然过期规则转入 retained。
func (selector *selectorCore) retainUUID(state *selectorState, uuid string, now uint64) bool {
	return selector.retainRecord(state, state.records[uuid], now)
}

// setRetained 安装一条 retained 记录，并按最早截止时间驱逐直到满足字节上限。
// limit 为零、until 已到或溢出处理后不可保留时，会直接移除本地记录。
func (selector *selectorCore) setRetained(state *selectorState, record *selectorRecord, until uint64, now uint64) bool {
	uuid := record.meta.UUID
	wasActive := state.records[uuid] != nil
	if active := state.records[uuid]; active != nil {
		state.bytes -= selectorRecordSize(active)
		delete(state.records, uuid)
		state.deadlines.remove(uuid)
	}
	wasRetained := selector.removeRetained(state, uuid)
	limit := selector.client.config.selectorRetainedBytes
	if limit == 0 || until <= now {
		return wasActive || wasRetained
	}
	if state.retained == nil {
		state.retained = make(map[string]retainedSelectorRecord)
	}
	state.retained[uuid] = retainedSelectorRecord{record: record, until: until}
	state.retainedDeadlines.set(uuid, until)
	state.retainedBytes += selectorRecordSize(record)
	for state.retainedBytes > limit {
		evicted, ok := state.retainedDeadlines.pop()
		if !ok {
			break
		}
		if previous, exists := state.retained[evicted]; exists {
			state.retainedBytes -= selectorRecordSize(previous.record)
			delete(state.retained, evicted)
		}
	}
	return true
}

// expire 把已过期活动记录转入 retained，并删除第二个 TTL 也已结束的 retained 记录。
// 返回可见状态变化次数，调用方据此决定是否发布新视图。
func (selector *selectorCore) expire(state *selectorState, now uint64) int {
	changed := 0
	for {
		uuid, ok := state.deadlines.expire(now)
		if !ok {
			break
		}
		if record := state.records[uuid]; record != nil {
			if selector.retainRecord(state, record, now) {
				changed++
			}
		}
	}
	for {
		uuid, ok := state.retainedDeadlines.expire(now)
		if !ok {
			break
		}
		if previous, exists := state.retained[uuid]; exists {
			state.retainedBytes -= selectorRecordSize(previous.record)
			delete(state.retained, uuid)
			changed++
		}
	}
	return changed
}

// publish 从唯一可变 state 构造一份新的不可变 selectorView 并原子发布。
// generation 标识完成同步的连接代；synchronized 为 false 时选择 API 必须返回 unavailable。
func (selector *selectorCore) publish(state selectorState, generation uint64, synchronized bool) {
	// 直接排序不可变记录指针；额外 UUID 切片会让每次候选访问和原始 Snapshot 多一次 Hash 查找。
	ordered := make([]*selectorRecord, 0, len(state.records))
	records := make(map[string]*selectorRecord, len(state.records))
	for uuid, record := range state.records {
		ordered = append(ordered, record)
		records[uuid] = record
	}
	slices.SortFunc(ordered, func(left, right *selectorRecord) int {
		return cmp.Compare(left.meta.UUID, right.meta.UUID)
	})
	var retainedOrdered []retainedSelectorRecord
	var retained map[string]retainedSelectorRecord
	if len(state.retained) != 0 {
		retainedOrdered = make([]retainedSelectorRecord, 0, len(state.retained))
		retained = make(map[string]retainedSelectorRecord, len(state.retained))
		for uuid, record := range state.retained {
			retainedOrdered = append(retainedOrdered, record)
			retained[uuid] = record
		}
		slices.SortFunc(retainedOrdered, func(left, right retainedSelectorRecord) int {
			return cmp.Compare(left.record.meta.UUID, right.record.meta.UUID)
		})
	}
	selector.view.Store(&selectorView{
		generation:      generation,
		synchronized:    synchronized,
		records:         records,
		orderedRecords:  ordered,
		retained:        retained,
		orderedRetained: retainedOrdered,
	})
}

// markUnavailable 复用当前不可变记录，只原子发布 synchronized=false 的浅拷贝视图。
func (selector *selectorCore) markUnavailable(generation uint64) {
	view := selector.view.Load()
	if view == nil {
		selector.view.Store(emptySelectorView(generation, false))
		return
	}
	next := *view
	next.generation = generation
	next.synchronized = false
	selector.view.Store(&next)
}

// report 忽略正常取消，并尽力投递到 Selector 与 Client 的有界诊断通道。
func (selector *selectorCore) report(err error) {
	if err == nil || errors.Is(err, context.Canceled) {
		return
	}
	select {
	case selector.errors <- err:
	default:
	}
	selector.client.report(err)
}

// cloneSelectorRecord 复制记录和 Data map；Attr 与字段值保持内部不可变共享。
func cloneSelectorRecord(record *selectorRecord) *selectorRecord {
	next := *record
	next.data = cloneFieldMap(record.data)
	next.size = selectorRecordSize(record)
	return &next
}

// stateRecord 优先返回活动记录，否则返回 retained 记录；第二个返回值表示是否活动可选择。
func stateRecord(state *selectorState, uuid string) (*selectorRecord, bool) {
	if record := state.records[uuid]; record != nil {
		return record, true
	}
	if retained, exists := state.retained[uuid]; exists {
		return retained.record, false
	}
	return nil, false
}

// selectorRecordSize 返回缓存大小，旧记录没有缓存时按完整协议字段重新计算。
func selectorRecordSize(record *selectorRecord) int {
	if record.size != 0 {
		return record.size
	}
	return registrationSize(record.meta.UUID, record.meta.Revision, record.meta.TTL, record.meta.Version, record.attr, record.data)
}

// newSelectorState 创建预分配 capacity 的空活动/保留状态及其截止队列。
func newSelectorState(capacity int) selectorState {
	return selectorState{
		records:           make(map[string]*selectorRecord, capacity),
		deadlines:         newDeadlineQueue(capacity),
		retained:          make(map[string]retainedSelectorRecord, capacity),
		retainedDeadlines: newDeadlineQueue(capacity),
	}
}

// cloneSelectorState 复制 map 和堆所有权，但共享内部不可变 selectorRecord 指针。
func cloneSelectorState(previous selectorState) selectorState {
	state := newSelectorState(len(previous.records) + len(previous.retained))
	state.bytes = previous.bytes
	state.retainedBytes = previous.retainedBytes
	for uuid, record := range previous.records {
		state.records[uuid] = record
		state.deadlines.set(uuid, record.deadline)
	}
	for uuid, retained := range previous.retained {
		state.retained[uuid] = retained
		state.retainedDeadlines.set(uuid, retained.until)
	}
	return state
}

// recoveryState 构造重连恢复状态：旧 retained 保持原截止，旧活动记录先转为 retained。
// 在新权威扫描证明仍有效前，任何旧记录都不会继续可选择。
func (selector *selectorCore) recoveryState(previous selectorState, now uint64) selectorState {
	state := newSelectorState(len(previous.records) + len(previous.retained))
	for _, retained := range previous.retained {
		selector.setRetained(&state, retained.record, retained.until, now)
	}
	for _, record := range previous.records {
		selector.retainRecord(&state, record, now)
	}
	return state
}

// nextDeadline 返回活动租约与 retained 截止中更早的一项。
func (selector *selectorCore) nextDeadline(state *selectorState) (uint64, bool) {
	active, hasActive := state.deadlines.next()
	retained, hasRetained := state.retainedDeadlines.next()
	if !hasActive {
		return retained, hasRetained
	}
	if !hasRetained || active <= retained {
		return active, true
	}
	return retained, true
}

// emptySelectorView 创建没有记录的不可变视图，并设置指定连接代和同步状态。
func emptySelectorView(generation uint64, synchronized bool) *selectorView {
	return &selectorView{
		generation:   generation,
		synchronized: synchronized,
		records:      make(map[string]*selectorRecord),
		retained:     make(map[string]retainedSelectorRecord),
	}
}

// registryKey 构造 Zone/Type Registry membership Hash 与 Pub/Sub 频道名。
func registryKey(zone string, typeName string) string {
	return "verdandi:registry:" + zone + ":" + typeName
}

// registrationKey 构造 Zone/Type/UUID 对应的单 Registration Hash 键。
func registrationKey(zone string, typeName string, uuid string) string {
	return "verdandi:registration:" + zone + ":" + typeName + ":" + uuid
}

// newNonce 使用密码学随机数生成 32 字符小写十六进制 PING 栅栏标识。
func newNonce() (string, error) {
	var value [16]byte
	if _, err := cryptorand.Read(value[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(value[:]), nil
}

// selectorRetryDelay 计算配置化指数退避，并在 delay-jitter 至 delay 范围加入均匀抖动。
func selectorRetryDelay(failures uint, initial time.Duration, maximum time.Duration, multiplier int, jitterPercent int) time.Duration {
	delay := initial
	for index := uint(0); index < failures && delay < maximum; index++ {
		if delay > maximum/time.Duration(multiplier) {
			delay = maximum
			break
		}
		delay *= time.Duration(multiplier)
	}
	span := delay * time.Duration(jitterPercent) / 100
	if span == 0 {
		return delay
	}
	return delay - span + time.Duration(mathrand.Int64N(int64(span)+1))
}

// waitContext 等待 delay 或 ctx 结束；true 表示计时完成，false 表示被取消。
func waitContext(ctx context.Context, delay time.Duration) bool {
	timer := time.NewTimer(delay)
	defer timer.Stop()
	select {
	case <-timer.C:
		return true
	case <-ctx.Done():
		return false
	}
}

// stopTimer 安全停止 timer，并在必要时排空已触发但未消费的值。
func stopTimer(timer *time.Timer) {
	if timer == nil || timer.Stop() {
		return
	}
	select {
	case <-timer.C:
	default:
	}
}

// resetTimer 在复用 timer 前安全停止和排空，再安排新的 delay。
func resetTimer(timer *time.Timer, delay time.Duration) {
	stopTimer(timer)
	timer.Reset(delay)
}
