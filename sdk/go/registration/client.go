package registration

import (
	"context"
	"errors"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	"github.com/eosforge/verdandi/sdk/go/internal/lifecycle"
	redis "github.com/redis/go-redis/v9"
)

// configurationRefreshWorker 表示按需共享的 Zone 配置刷新协程。
// users 只在 configurationWorkerMu 下访问；cancel 与 done 分别负责停止和等待该协程。
type configurationRefreshWorker struct {
	users  int
	cancel context.CancelFunc
	done   chan struct{}
}

// Client 管理 Registration 策略、脚本、同步参数、工作协程和诊断。
// 它借用根 verdandi.Client 的 Redis 连接，但关闭时只等待自己拥有的工作。
type Client struct {
	// runtime 是领域内部共享状态；公开包装保持轻量且不可复制内部锁。
	runtime *clientRuntime
}

// clientRuntime 是一个 Registration 领域客户端的唯一生命周期所有者。
// Context 不存入结构体；长期协程各自在入口创建并持有 Context，此处只保存取消函数和关闭广播。
type clientRuntime struct {
	config        runtimeConfig
	redis         *redis.Client
	transportDone <-chan struct{}
	done          chan struct{}

	zoneConfig atomic.Pointer[zoneConfig]
	configMu   sync.Mutex

	configurationWorkerMu sync.Mutex
	configurationWorker   *configurationRefreshWorker

	errors chan error

	gate lifecycle.Gate

	closeOnce sync.Once
	closeDone chan struct{}
	closeErr  error
}

// Open 把一个 Registration 领域客户端连接到现有 transport。
// ctx 只控制初始化；config 提供 Zone 和本地同步参数。函数会验证 Redis 8、补齐缺失的 Zone 默认策略并加载 Lua。
// 成功后对象生命周期与 ctx 脱离；transport 已关闭或初始化失败时不启动任何长期协程。
func Open(ctx context.Context, transport *verdandi.Client, config Config) (*Client, error) {
	if ctx == nil {
		return nil, protocolError(codeInvalid, "context", 0)
	}
	if err := ctx.Err(); err != nil {
		return nil, wrapContext(err)
	}
	if transport == nil {
		return nil, protocolError(codeClosed, "client", 0)
	}
	// 同时取得驱动和关闭广播，避免在根关闭竞态中只观察到其中一项。
	driver := transport.Redis()
	done := transport.Done()
	if driver == nil || done == nil || transportClosed(done) {
		return nil, protocolError(codeClosed, "client", 0)
	}
	runtimeConfig, err := config.normalize(transport.Timeout())
	if err != nil {
		return nil, err
	}
	// 领域客户端不创建关闭监听协程；其现有拥有者循环直接观察 transportDone。
	runtime := &clientRuntime{
		config:        runtimeConfig,
		redis:         driver,
		transportDone: done,
		done:          make(chan struct{}),
		errors:        make(chan error, runtimeConfig.registrationErrorBuffer),
		closeDone:     make(chan struct{}),
	}
	if err := runtime.bootstrap(ctx); err != nil {
		return nil, err
	}
	if transportClosed(done) {
		runtime.startClose()
		<-runtime.closeDone
		return nil, protocolError(codeClosed, "client", 0)
	}
	client := &Client{runtime: runtime}
	return client, nil
}

// transportClosed 对只读关闭通道执行无阻塞快照检查。
// nil 通道视为未关闭，便于零值测试桩，但生产 Open 会拒绝 nil。
func transportClosed(done <-chan struct{}) bool {
	select {
	case <-done:
		return true
	default:
		return false
	}
}

// Errors 返回 Registration 与 Selector 的有界异步诊断通道。
// 缓冲区满时新诊断会被丢弃；Client 完成关闭后通道也会关闭。
func (client *Client) Errors() <-chan error {
	if client == nil || client.runtime == nil {
		return nil
	}
	return client.runtime.errors
}

// RegistrationLimits 返回当前最后一份完整有效的 Zone 策略快照。
// 返回结构按值复制；客户端无效或尚无配置时返回零值。
func (client *Client) RegistrationLimits() RegistrationLimits {
	if client == nil || client.runtime == nil {
		return RegistrationLimits{}
	}
	config := client.runtime.zoneConfig.Load()
	if config == nil {
		return RegistrationLimits{}
	}
	return RegistrationLimits{
		AttrMaxFields:        config.attrMaxFields,
		DataMaxFields:        config.dataMaxFields,
		FieldNameMaxBytes:    config.fieldNameMaxBytes,
		AttrValueMaxBytes:    config.attrValueMaxBytes,
		DataValueMaxBytes:    config.dataValueMaxBytes,
		RecordMaxBytes:       config.recordMaxBytes,
		ConfigurationRefresh: config.configurationRefresh,
	}
}

// RefreshConfiguration 使用 ctx 立即读取并原子替换一份完整 Zone 策略。
// 读取或校验失败时保留上一份有效快照，不发布部分配置。
func (client *Client) RefreshConfiguration(ctx context.Context) error {
	if client == nil || client.runtime == nil {
		return protocolError(codeClosed, "client", 0)
	}
	return client.runtime.RefreshConfiguration(ctx)
}

// Close 取消并等待 Registration 领域拥有的全部工作，但不关闭共享 Redis 传输。
// ctx 只限制本次等待；关闭过程一旦开始不会回滚，重复调用共享同一个最终结果。
func (client *Client) Close(ctx context.Context) error {
	if client == nil || client.runtime == nil {
		return nil
	}
	return client.runtime.close(ctx)
}

// runtimeFor 校验公开包装及其运行时状态，并返回可接纳新工作的内部对象。
func runtimeFor(client *Client) (*clientRuntime, error) {
	if client == nil || client.runtime == nil {
		return nil, protocolError(codeClosed, "client", 0)
	}
	if client.runtime.closed() {
		return nil, protocolError(codeClosed, "client", 0)
	}
	return client.runtime, nil
}

// closed 判断领域或根传输是否永久关闭。
// 首次观察到根关闭时会启动领域自身的取消与等待流程。
func (client *clientRuntime) closed() bool {
	if client == nil || client.gate.Closing() {
		return true
	}
	if transportClosed(client.transportDone) {
		client.startClose()
		return true
	}
	return false
}

// bootstrap 按固定顺序完成 Redis 版本、Zone 策略和 Lua 脚本初始化。
// ctx 会被叠加单命令超时；任一步失败都不会发布半初始化 Client。
func (client *clientRuntime) bootstrap(ctx context.Context) error {
	commandCtx, cancel := client.commandContext(ctx)
	defer cancel()
	if err := client.requireRedis8(commandCtx); err != nil {
		return err
	}
	config, err := client.readZoneConfig(commandCtx, true)
	if err != nil {
		return err
	}
	client.zoneConfig.Store(&config)
	if err := protocolScripts.load(commandCtx, client.redis); err != nil {
		return wrapDriver(codeUnavailable, err)
	}
	return nil
}

// requireRedis8 使用独占连接执行 HELLO 3，并要求服务端主版本至少为 8。
// Redis 8 是 Registry Hash-field TTL 命令的协议前提；connection 在所有路径归还连接池。
func (client *clientRuntime) requireRedis8(ctx context.Context) error {
	connection := client.redis.Conn()
	defer connection.Close()
	hello, err := connection.Hello(ctx, 3, "", "", "").Result()
	if err != nil {
		return wrapDriver(codeUnavailable, err)
	}
	version, ok := hello["version"].(string)
	if !ok {
		return protocolError(codeCorrupt, "redis_version", 0)
	}
	majorText, _, _ := strings.Cut(version, ".")
	major, err := strconv.Atoi(majorText)
	if err != nil || major < 8 {
		return protocolError(codeProtocol, "redis_version", 0)
	}
	return nil
}

// readZoneConfig 用一次 HMGET 读取完整 Zone 策略。
// installDefaults 为 true 时用 HSETNX 逐项补齐缺失默认值后重读；已有管理值永不被覆盖。
func (client *clientRuntime) readZoneConfig(ctx context.Context, installDefaults bool) (zoneConfig, error) {
	key := configKey(client.config.Zone)
	values, err := client.redis.HMGet(ctx, key, zoneConfigFields[:]...).Result()
	if err != nil {
		return zoneConfig{}, wrapDriver(codeUnavailable, err)
	}
	// 只有完整快照才能进入解析；初始化阶段可补齐缺项，刷新阶段则把缺项视为损坏。
	missing := false
	for _, value := range values {
		if value == nil {
			missing = true
			break
		}
	}
	if missing && installDefaults {
		defaults := client.config.zoneDefaults.values()
		for index, value := range values {
			if value != nil {
				continue
			}
			if err := client.redis.HSetNX(ctx, key, zoneConfigFields[index], defaults[index]).Err(); err != nil {
				return zoneConfig{}, wrapDriver(codeUnavailable, err)
			}
		}
		values, err = client.redis.HMGet(ctx, key, zoneConfigFields[:]...).Result()
		if err != nil {
			return zoneConfig{}, wrapDriver(codeUnavailable, err)
		}
	}
	return parseZoneConfig(values)
}

// limits 返回当前不可变 Zone 策略的值副本；尚未安装时返回零值。
func (client *clientRuntime) limits() zoneConfig {
	config := client.zoneConfig.Load()
	if config == nil {
		return zoneConfig{}
	}
	return *config
}

// RefreshConfiguration 串行读取并发布一份新策略。
// ctx 控制 Redis 读取；configMu 防止显式刷新和后台刷新交错发布旧结果。
func (client *clientRuntime) RefreshConfiguration(ctx context.Context) error {
	if client == nil || client.closed() {
		return protocolError(codeClosed, "", 0)
	}
	if ctx == nil {
		return protocolError(codeInvalid, "context", 0)
	}
	client.configMu.Lock()
	defer client.configMu.Unlock()
	config, err := client.readZoneConfig(ctx, false)
	if err != nil {
		return err
	}
	client.zoneConfig.Store(&config)
	return nil
}

// admit 在关闭栅栏内登记一个长期工作，并可保存其 cancel 供 Client.Close 调用。
// 返回的释放函数幂等，调用者必须在工作结束时执行；关闭开始后不再允许 WaitGroup.Add。
func (client *clientRuntime) admit(cancel context.CancelFunc) (func(), error) {
	if client == nil || client.closed() {
		return nil, protocolError(codeClosed, "", 0)
	}
	if transportClosed(client.transportDone) {
		client.startClose()
		return nil, protocolError(codeClosed, "", 0)
	}
	release, ok := client.gate.Track(cancel)
	if !ok {
		return nil, protocolError(codeClosed, "", 0)
	}
	return release, nil
}

// close 启动不可逆关闭并等待 closeDone，ctx 仅约束当前调用的等待时间。
func (client *clientRuntime) close(ctx context.Context) error {
	if ctx == nil {
		return protocolError(codeInvalid, "context", 0)
	}
	client.startClose()
	select {
	case <-client.closeDone:
		return client.closeErr
	case <-ctx.Done():
		return wrapContext(ctx.Err())
	}
}

// startClose 关闭领域广播、取消已登记工作，并异步等待其全部退出。
func (client *clientRuntime) startClose() {
	client.gate.Start(func() { close(client.done) })
	client.closeOnce.Do(func() {
		go func() {
			client.gate.Wait()
			close(client.errors)
			close(client.closeDone)
		}()
	})
}

// acquireConfigurationRefresh 为一个 Registration/Selector 引用共享刷新协程。
// 首个用户惰性启动协程；返回的释放函数幂等，最后一个用户释放时同步等待协程退出。
func (client *clientRuntime) acquireConfigurationRefresh() func() {
	client.configurationWorkerMu.Lock()
	worker := client.configurationWorker
	if worker == nil {
		release, ok := client.gate.Track(nil)
		if !ok {
			client.configurationWorkerMu.Unlock()
			return func() {}
		}
		owner, cancel := context.WithCancel(context.Background())
		worker = &configurationRefreshWorker{cancel: cancel, done: make(chan struct{})}
		client.configurationWorker = worker
		go client.refreshLoop(owner, worker.done, release)
	}
	worker.users++
	client.configurationWorkerMu.Unlock()

	var once sync.Once
	return func() {
		once.Do(func() { client.releaseConfigurationRefresh(worker) })
	}
}

// releaseConfigurationRefresh 减少 worker 的引用计数，并在最后一个用户离开时取消和等待。
// worker 必须来自 acquireConfigurationRefresh；重复释放由外层 sync.Once 吸收。
func (client *clientRuntime) releaseConfigurationRefresh(worker *configurationRefreshWorker) {
	client.configurationWorkerMu.Lock()
	if worker.users == 0 {
		client.configurationWorkerMu.Unlock()
		return
	}
	worker.users--
	last := worker.users == 0
	if last {
		if client.configurationWorker == worker {
			client.configurationWorker = nil
		}
		worker.cancel()
	}
	client.configurationWorkerMu.Unlock()
	if last {
		<-worker.done
	}
}

// refreshLoop 按 Redis 配置的抖动间隔刷新 Zone 策略。
// ctx、领域 done 或根 transportDone 任一结束都会退出；done 在退出时关闭供最后用户等待。
func (client *clientRuntime) refreshLoop(ctx context.Context, done chan<- struct{}, release func()) {
	defer close(done)
	defer release()
	timer := time.NewTimer(jitteredInterval(client.configurationRefresh(), client.config.policyRefreshJitter))
	defer timer.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-client.done:
			return
		case <-client.transportDone:
			client.startClose()
			return
		case <-timer.C:
			// 每轮重新读取当前刷新周期，使管理员更新在下一次成功刷新后生效。
			commandContext, cancel := client.commandContext(ctx)
			err := client.RefreshConfiguration(commandContext)
			cancel()
			client.report(err)
			timer.Reset(jitteredInterval(client.configurationRefresh(), client.config.policyRefreshJitter))
		}
	}
}

// configurationRefresh 返回当前策略中的刷新周期；无策略时使用协议默认值。
func (client *clientRuntime) configurationRefresh() time.Duration {
	config := client.zoneConfig.Load()
	if config == nil {
		return client.config.zoneDefaults.configurationRefresh
	}
	return config.configurationRefresh
}

// report 尽力投递一个异步诊断；nil、正常取消和缓冲区满都不会阻塞工作协程。
func (client *clientRuntime) report(err error) {
	if err == nil || errors.Is(err, context.Canceled) {
		return
	}
	select {
	case client.errors <- err:
	default:
	}
}

// commandContext 在 parent 上叠加领域配置的单命令超时。
// 返回的 CancelFunc 必须由调用者在所有路径调用。
func (client *clientRuntime) commandContext(parent context.Context) (context.Context, context.CancelFunc) {
	return context.WithTimeout(parent, client.config.timeout)
}
