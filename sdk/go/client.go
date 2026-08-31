package verdandi

import (
	"context"
	"errors"
	"fmt"
	mathrand "math/rand/v2"
	"sync"
	"sync/atomic"
	"time"

	redis "github.com/redis/go-redis/v9"
)

// Client 持有一个可共享的 Redis 传输实例。
// Registration 与 Catalog 客户端借用该传输，但分别管理自己的脚本、策略、工作协程和数据。
type Client struct {
	// config 是 Open 校验后的连接配置，生命周期内不可变。
	config runtimeConfig
	// redis 是唯一底层驱动实例，由 Close 统一释放。
	redis *redis.Client

	// hashDescriptors 缓存按 Go 结构体类型生成的不可变 Hash 字段描述。
	hashDescriptors sync.Map

	// done 只表示显式永久关闭，不表示临时断网或 Sentinel 切换。
	done chan struct{}
	// closed 为普通命令提供无锁的快速准入判断。
	closed atomic.Bool
	// closeOnce 保证关闭广播和驱动释放只执行一次。
	closeOnce sync.Once
	// closeErr 保存第一次驱动关闭结果，供所有重复 Close 调用共享。
	closeErr error
}

// Open 校验配置并建立一个根客户端。
// ctx 控制配置校验和首次 PING；config 只包含连接参数与普通命令超时。
// 此函数不会加载领域脚本、安装 Registration 策略、打开 Catalog 本地存储或启动后台协程。
func Open(ctx context.Context, config Config) (*Client, error) {
	runtime, err := config.normalize()
	if err != nil {
		return nil, err
	}
	if ctx == nil {
		return nil, protocolError(CodeInvalid, "context", 0)
	}
	if err := ctx.Err(); err != nil {
		return nil, wrapContext(err)
	}

	driver := newRedisClient(runtime)
	client := &Client{
		config: runtime,
		redis:  driver,
		done:   make(chan struct{}),
	}
	// 首次 PING 可能包含 TCP/TLS 建连，使用独立连接上限；普通命令随后使用 timeout。
	commandCtx, cancel := context.WithTimeout(ctx, runtime.connectTimeout)
	pingErr := driver.Ping(commandCtx).Err()
	cancel()
	if pingErr != nil {
		_ = driver.Close()
		return nil, wrapDriver(CodeUnavailable, pingErr)
	}
	return client, nil
}

// Close 永久关闭根客户端，广播传输关闭并释放 Redis 连接。
// Registration 与 Catalog 客户端仍负责取消并等待各自的工作协程；重复调用返回同一个关闭结果。
func (client *Client) Close() error {
	if client == nil {
		return nil
	}
	client.closeOnce.Do(func() {
		client.closed.Store(true)
		if client.done != nil {
			close(client.done)
		}
		if client.redis != nil {
			client.closeErr = client.redis.Close()
		}
	})
	return client.closeErr
}

// Redis 返回根客户端持有的 go-redis 指针。
// 返回值是借用引用：领域客户端可复用连接池，但调用方不得单独关闭或重新配置它；其生命周期由 Client.Close 管理。
// 原生命令受 Redis ACL 约束，但会绕过 Verdandi 的参数上限、错误映射和多 key 原子不变量；nil Client 返回 nil。
// Client 关闭后仍返回同一个指针，使已构建领域对象能收到 go-redis 的确定关闭错误；新领域对象必须同时检查 Done。
func (client *Client) Redis() *redis.Client {
	if client == nil {
		return nil
	}
	return client.redis
}

// Done 返回根客户端永久关闭时被关闭的只读通道。
// 通道不传递值且只关闭一次；临时断网、Redis 重连和 Sentinel 主节点切换不会关闭它；nil Client 返回 nil 通道。
func (client *Client) Done() <-chan struct{} {
	if client == nil {
		return nil
	}
	return client.done
}

// Timeout 返回每条普通 Redis 命令采用的规范化超时。
// 返回值在 Client 生命周期内固定，领域客户端可将零值配置继承为同一超时；nil Client 返回零时长。
func (client *Client) Timeout() time.Duration {
	if client == nil {
		return 0
	}
	return client.config.timeout
}

// newRedisClient 根据已校验的 config 构造具体的 *redis.Client。
// Standalone 与 Sentinel 都返回同一具体类型；TLS 和 Sentinel 地址会复制，避免调用方后续修改。
func newRedisClient(config runtimeConfig) *redis.Client {
	// go-redis 不重放业务命令；连接池在需要新连接时可进行固定次数的拨号重试。
	// 自定义退避只作用于建立连接，后续独立命令仍可再次触发连接恢复。
	backoff := func(attempt int) time.Duration {
		return reconnectDelay(
			attempt,
			config.reconnectInitial,
			config.reconnectMax,
			config.reconnectFactor,
			config.reconnectJitter,
		)
	}
	if config.Standalone != nil {
		standalone := config.Standalone
		var tlsConfig = standalone.TLS
		if tlsConfig != nil {
			tlsConfig = tlsConfig.Clone()
		}
		// 独立拓扑只需要固定数据节点地址，不启用任何服务发现任务。
		return redis.NewClient(&redis.Options{
			Addr:                  standalone.Address,
			Username:              standalone.Username,
			Password:              standalone.Password,
			DB:                    standalone.Database,
			TLSConfig:             tlsConfig,
			MaxRetries:            -1,
			MinRetryBackoff:       -1,
			MaxRetryBackoff:       -1,
			DialTimeout:           config.connectTimeout,
			DialerRetries:         5,
			DialerRetryTimeout:    config.reconnectInitial,
			DialerRetryBackoff:    backoff,
			ReadTimeout:           config.timeout,
			WriteTimeout:          config.timeout,
			PoolSize:              config.poolMax,
			PoolTimeout:           config.timeout,
			MinIdleConns:          config.poolMin,
			MaxIdleConns:          config.poolMax,
			MaxActiveConns:        config.poolMax,
			ConnMaxIdleTime:       config.poolIdle,
			ContextTimeoutEnabled: true,
		})
	}
	sentinel := config.Sentinel
	var tlsConfig = sentinel.TLS
	if tlsConfig != nil {
		tlsConfig = tlsConfig.Clone()
	}
	// FailoverClient 内部通过 Sentinel 解析当前主节点，并在连接错误后重新解析。
	return redis.NewFailoverClient(&redis.FailoverOptions{
		MasterName:            sentinel.MasterName,
		SentinelAddrs:         append([]string(nil), sentinel.Addresses...),
		Username:              sentinel.Username,
		Password:              sentinel.Password,
		SentinelUsername:      sentinel.SentinelUsername,
		SentinelPassword:      sentinel.SentinelPassword,
		DB:                    sentinel.Database,
		TLSConfig:             tlsConfig,
		MaxRetries:            -1,
		MinRetryBackoff:       -1,
		MaxRetryBackoff:       -1,
		DialTimeout:           config.connectTimeout,
		DialerRetries:         5,
		DialerRetryTimeout:    config.reconnectInitial,
		DialerRetryBackoff:    backoff,
		ReadTimeout:           config.timeout,
		WriteTimeout:          config.timeout,
		PoolSize:              config.poolMax,
		PoolTimeout:           config.timeout,
		MinIdleConns:          config.poolMin,
		MaxIdleConns:          config.poolMax,
		MaxActiveConns:        config.poolMax,
		ConnMaxIdleTime:       config.poolIdle,
		ContextTimeoutEnabled: true,
	})
}

// reconnectDelay 计算不超过 max 的指数退避，并从结果中扣除有界随机抖动。
// attempt 从零开始；饱和乘法避免极端连续失败发生整数溢出。
func reconnectDelay(attempt int, initial, maximum time.Duration, multiplier, jitterPercent int) time.Duration {
	delay := initial
	for range attempt {
		if delay >= maximum || multiplier <= 1 {
			break
		}
		if delay > maximum/time.Duration(multiplier) {
			delay = maximum
			break
		}
		delay *= time.Duration(multiplier)
	}
	if delay > maximum {
		delay = maximum
	}
	span := delay * time.Duration(jitterPercent) / 100
	if span <= 0 {
		return delay
	}
	return delay - time.Duration(mathrand.Int64N(int64(span)+1))
}

// wrapContext 把标准 Context 结束原因映射为稳定 Verdandi 错误类别。
// 未知 Context 错误保守地归类为 unavailable，同时保留原始错误链。
func wrapContext(err error) error {
	if errors.Is(err, context.DeadlineExceeded) {
		return wrapError(CodeDeadline, err)
	}
	if errors.Is(err, context.Canceled) {
		return wrapError(CodeClosed, err)
	}
	return wrapError(CodeUnavailable, err)
}

// wrapDriver 把 go-redis 和 Context 错误映射为稳定类别，并保留 cause。
// code 由调用方表示该操作在未知传输结果下应视为 unavailable 还是 ambiguous。
func wrapDriver(code Code, err error) error {
	if err == nil {
		return nil
	}
	if errors.Is(err, redis.ErrClosed) {
		return &Error{Code: CodeClosed, Cause: fmt.Errorf("redis operation: %w", err)}
	}
	if code == CodeAmbiguous {
		return &Error{Code: code, Cause: fmt.Errorf("redis operation: %w", err)}
	}
	if errors.Is(err, context.DeadlineExceeded) {
		code = CodeDeadline
	} else if errors.Is(err, context.Canceled) {
		code = CodeClosed
	}
	return &Error{Code: code, Cause: fmt.Errorf("redis operation: %w", err)}
}
