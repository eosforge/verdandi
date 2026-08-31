package catalog

import (
	"context"
	"sync"
	"time"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	"github.com/LaconisIves/verdandi/sdk/go/internal/lifecycle"
	redis "github.com/redis/go-redis/v9"
)

// Client 管理一个 Zone 的 Catalog 脚本、可选本地检查点和子操作。
// 它借用根 Client 的 Redis 传输，但独立取消并等待自己的工作。
type Client struct {
	config        runtimeConfig
	redis         *redis.Client
	transportDone <-chan struct{}
	done          chan struct{}
	scripts       scripts
	store         *localStore

	gate lifecycle.Gate

	closeOnce sync.Once
	closeDone chan struct{}
	closeErr  error
}

// Open 把 Catalog 领域附着到现有 transport，加载脚本并按 config 可选打开可丢弃本地检查点。
// ctx 只控制初始化；成功对象生命周期与 ctx 脱离。transport 关闭或任一步失败时不会留下工作协程。
func Open(ctx context.Context, transport *verdandi.Client, config Config) (*Client, error) {
	if ctx == nil {
		return nil, newError(verdandi.CodeInvalid, "context", 0, nil)
	}
	if err := ctx.Err(); err != nil {
		return nil, wrapContext(err)
	}
	if transport == nil {
		return nil, newError(verdandi.CodeClosed, "client", 0, nil)
	}
	driver := transport.Redis()
	done := transport.Done()
	if driver == nil || done == nil || transportClosed(done) {
		return nil, newError(verdandi.CodeClosed, "client", 0, nil)
	}
	runtime, err := config.normalize(transport.Timeout())
	if err != nil {
		return nil, err
	}
	client := &Client{
		config:        runtime,
		redis:         driver,
		transportDone: done,
		done:          make(chan struct{}),
		scripts:       newScripts(),
		closeDone:     make(chan struct{}),
	}
	// 脚本加载与普通命令使用相同超时；失败时不发布半初始化 Client。
	commandCtx, cancel := client.commandContext(ctx)
	loadErr := client.scripts.load(commandCtx, driver)
	cancel()
	if loadErr != nil {
		return nil, wrapDriver(verdandi.CodeUnavailable, loadErr)
	}
	if runtime.localStorePath != "" {
		store, openErr := openLocalStore(runtime.localStorePath, runtime.timeout)
		if openErr != nil {
			return nil, newError(verdandi.CodeUnavailable, "local_store_path", 0, openErr)
		}
		client.store = store
	}
	if transportClosed(done) {
		client.startClose()
		<-client.closeDone
		return nil, newError(verdandi.CodeClosed, "client", 0, nil)
	}
	return client, nil
}

// transportClosed 对根关闭通道执行无阻塞检查；nil 通道视为尚未关闭。
func transportClosed(done <-chan struct{}) bool {
	select {
	case <-done:
		return true
	default:
		return false
	}
}

// Close 取消并等待 Catalog 操作，随后关闭本地检查点，但不关闭共享 Redis 传输。
// ctx 只限制本次等待；关闭不可逆，重复调用共享同一个最终结果。
func (client *Client) Close(ctx context.Context) error {
	if client == nil {
		return nil
	}
	if ctx == nil {
		return newError(verdandi.CodeInvalid, "context", 0, nil)
	}
	client.startClose()
	select {
	case <-client.closeDone:
		return client.closeErr
	case <-ctx.Done():
		return wrapContext(ctx.Err())
	}
}

// startClose 关闭领域广播、取消所有已登记 Context，并异步等待工作结束和本地存储关闭。
func (client *Client) startClose() {
	client.gate.Start(func() { close(client.done) })
	client.closeOnce.Do(func() {
		go func() {
			client.gate.Wait()
			var storeErr error
			if client.store != nil {
				storeErr = client.store.close()
			}
			client.closeErr = storeErr
			close(client.closeDone)
		}()
	})
}

// closed 判断领域或根传输是否永久关闭，并在首次观察到根关闭时启动领域关闭。
func (client *Client) closed() bool {
	if client == nil {
		return true
	}
	if client.gate.Closing() {
		return true
	}
	if transportClosed(client.transportDone) {
		client.startClose()
		return true
	}
	return false
}

// commandContext 在 parent 上叠加普通 Catalog Redis 命令超时。
func (client *Client) commandContext(parent context.Context) (context.Context, context.CancelFunc) {
	return context.WithTimeout(parent, client.config.timeout)
}

// syncContext 在 parent 上叠加一次 Catalog 权威同步的总超时。
func (client *Client) syncContext(parent context.Context) (context.Context, context.CancelFunc) {
	return context.WithTimeout(parent, client.config.syncTimeout)
}

// admit 派生一条可由 Client.Close 取消的调用 Context，并把工作计入关闭等待。
// parent 控制调用；返回释放函数幂等且必须执行。关闭栅栏保证 Wait 开始后不会再 Add。
func (client *Client) admit(parent context.Context) (context.Context, func(), error) {
	if client == nil || parent == nil {
		return nil, nil, newError(verdandi.CodeInvalid, "context", 0, nil)
	}
	if err := parent.Err(); err != nil {
		return nil, nil, wrapContext(err)
	}
	if client.closed() {
		return nil, nil, newError(verdandi.CodeClosed, "", 0, nil)
	}
	ctx, cancel := context.WithCancel(parent)
	if transportClosed(client.transportDone) {
		cancel()
		client.startClose()
		return nil, nil, newError(verdandi.CodeClosed, "", 0, nil)
	}
	release, ok := client.gate.Track(cancel)
	if !ok {
		return nil, nil, newError(verdandi.CodeClosed, "", 0, nil)
	}
	return ctx, release, nil
}

// track 登记一个已由长期工作拥有的 cancel，并返回幂等释放函数。
// 它不创建或存储 Context，主要供 Subscriber 工作树使用。
func (client *Client) track(cancel context.CancelFunc) (func(), error) {
	if client == nil || cancel == nil || client.closed() {
		return nil, newError(verdandi.CodeClosed, "", 0, nil)
	}
	if transportClosed(client.transportDone) {
		client.startClose()
		return nil, newError(verdandi.CodeClosed, "", 0, nil)
	}
	release, ok := client.gate.Track(cancel)
	if !ok {
		return nil, newError(verdandi.CodeClosed, "", 0, nil)
	}
	return release, nil
}

// waitContext 等待 duration 或 ctx 结束，并把取消原因转换为稳定错误。
func waitContext(ctx context.Context, duration time.Duration) error {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-timer.C:
		return nil
	case <-ctx.Done():
		return wrapContext(ctx.Err())
	}
}
