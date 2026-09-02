package verdandi

import (
	"context"
	"errors"
	"fmt"
	"time"

	redis "github.com/redis/go-redis/v9"
)

// runtimeConfig 保存完成复制和默认值展开、仅供驱动构建读取的私有配置。
type runtimeConfig struct {
	// standalone 与 sentinel 严格二选一，并且不再引用调用方可变的拓扑容器。
	standalone *Standalone
	sentinel   *Sentinel
	// timeout 是已展开的普通命令超时；默认展开值为 2 秒，范围为 10 毫秒至 15 秒。
	timeout time.Duration
	// connectTimeout 是已展开的连接及 TLS 握手超时；默认展开值为 5 秒，范围为 20 毫秒至 30 秒。
	connectTimeout time.Duration
	// poolMin 是连接池实际保留的最少连接数；默认展开值为 1，范围为 1 至 1024。
	poolMin int
	// poolMax 是连接池实际允许的最多连接数；默认展开值为 4，范围为 1 至 1024，且不小于 poolMin。
	poolMax int
	// poolIdle 是超过 poolMin 的空闲连接回收时间；默认展开值为 10 秒，范围为 1 秒至 1 小时。
	poolIdle time.Duration
	// reconnectDelay 是每次驱动重新建连前的固定等待；默认展开值为 100 毫秒，范围为 10 毫秒至 30 秒。
	reconnectDelay time.Duration
}

// newRedisClient 根据已校验的 config 构造具体的 *redis.Client。
// Standalone 与 Sentinel 都返回同一具体类型；TLS 和 Sentinel 地址会复制，避免调用方后续修改。
func newRedisClient(config runtimeConfig) *redis.Client {
	// go-redis 不重放业务命令；连接池只在建立物理连接时按统一固定间隔进行有限拨号重试。
	// DialerRetryBackoff 保持 nil 时，驱动直接使用 DialerRetryTimeout，省去每次拨号重试的函数间接调用。
	if config.standalone != nil {
		standalone := config.standalone
		// 独立拓扑只需要固定数据节点地址，不启用任何服务发现任务。
		return redis.NewClient(&redis.Options{
			Addr:                  standalone.Address,
			Username:              standalone.Username,
			Password:              standalone.Password,
			DB:                    standalone.Database,
			TLSConfig:             standalone.TLS,
			MaxRetries:            -1,
			MinRetryBackoff:       -1,
			MaxRetryBackoff:       -1,
			DialTimeout:           config.connectTimeout,
			DialerRetries:         5,
			DialerRetryTimeout:    config.reconnectDelay,
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
	sentinel := config.sentinel
	// FailoverClient 内部通过 Sentinel 解析当前主节点，并在连接错误后重新解析。
	return redis.NewFailoverClient(&redis.FailoverOptions{
		MasterName:            sentinel.MasterName,
		SentinelAddrs:         append([]string(nil), sentinel.Addresses...),
		Username:              sentinel.Username,
		Password:              sentinel.Password,
		SentinelUsername:      sentinel.SentinelUsername,
		SentinelPassword:      sentinel.SentinelPassword,
		DB:                    sentinel.Database,
		TLSConfig:             sentinel.TLS,
		MaxRetries:            -1,
		MinRetryBackoff:       -1,
		MaxRetryBackoff:       -1,
		DialTimeout:           config.connectTimeout,
		DialerRetries:         5,
		DialerRetryTimeout:    config.reconnectDelay,
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

// wrapRedisCommand 把确定性服务端拒绝统一归类为 protocol，否则使用调用方给出的传输结果 code。
func wrapRedisCommand(code Code, err error) error {
	if _, ok := errors.AsType[redis.Error](err); ok {
		return wrapDriver(CodeProtocol, err)
	}
	return wrapDriver(code, err)
}
