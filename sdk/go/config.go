package verdandi

import (
	"crypto/tls"
	"strings"
	"time"

	"github.com/eosforge/verdandi/sdk/go/internal/validate"
)

// PoolConfig 控制根 Redis Client 共享连接池的容量和空闲回收。
type PoolConfig struct {
	// MinConnections 是客户端主动保留的最少空闲连接数；零值使用 1，允许范围为 1 至 1024。
	MinConnections int
	// MaxConnections 是池内允许同时存在的连接上限；零值使用 4，允许范围为 1 至 1024，且不得小于 MinConnections。
	MaxConnections int
	// IdleTimeout 是超过最少连接数的空闲连接可保留的最长时间；零值使用 10 秒，允许范围为 1 秒至 1 小时且必须为整毫秒。
	IdleTimeout time.Duration
}

// ReconnectConfig 控制 Redis 连接建立失败后的驱动级退避。
// 它只影响连接恢复，不会重试已经发送或结果不明确的 Redis 命令。
type ReconnectConfig struct {
	// InitialDelay 是同一次连接建立过程首次失败后的基础等待；零值使用 100 毫秒，允许范围为 10 毫秒至 5 秒且必须为整毫秒。
	InitialDelay time.Duration
	// MaxDelay 是连续连接失败时的退避上限；零值使用 5 秒，允许范围为 100 毫秒至 30 秒且必须为整毫秒。
	MaxDelay time.Duration
	// Multiplier 是每次连续失败后的指数增长倍数；零值使用 2，允许范围为 1 至 8。
	Multiplier int
	// JitterPercent 是从计算延迟中随机扣除的百分比上限；nil 使用 10，显式值允许范围为 0 至 50，指向零值表示禁用。
	JitterPercent *int
}

// Standalone 配置一个地址固定的 Redis 主节点。
type Standalone struct {
	// Address 是 host:port 格式的 Redis 端点；无默认值，去除首尾空白后必须非空。
	Address string
	// Username 是可选的 Redis ACL 用户名；默认空字符串并使用 Redis 默认用户。
	Username string
	// Password 是可选的 Redis 密码；默认空字符串并且不发送密码。
	Password string
	// Database 是 Redis 逻辑数据库编号；零值就是数据库 0，允许范围为 0 至 255。
	Database int
	// TLS 默认 nil 并使用明文传输；非 nil 时启用 TLS，建立客户端时会复制配置，调用方之后的修改不会生效。
	TLS *tls.Config
}

// Sentinel 配置通过 Redis Sentinel 发现和切换的主节点。
type Sentinel struct {
	// Addresses 列出所有 Sentinel 的 host:port 端点；无默认值，至少需要一个非空地址且列表中不能包含空项。
	Addresses []string
	// MasterName 是 Sentinel 监控的主服务名称；无默认值，去除首尾空白后必须非空。
	MasterName string
	// Username 是 Redis 数据节点的 ACL 用户名；默认空字符串并使用 Redis 默认用户。
	Username string
	// Password 是 Redis 数据节点的密码；默认空字符串并且不发送密码。
	Password string
	// SentinelUsername 是 Sentinel 自身独立配置的 ACL 用户名；默认空字符串并使用 Sentinel 默认用户。
	SentinelUsername string
	// SentinelPassword 是 Sentinel 自身独立配置的密码；默认空字符串并且不向 Sentinel 发送密码。
	SentinelPassword string
	// Database 是 Redis 数据节点的逻辑数据库编号；零值就是数据库 0，允许范围为 0 至 255。
	Database int
	// TLS 默认 nil 并使用明文传输；非 nil 时启用到 Redis 数据节点的 TLS，建立客户端时会复制配置。
	TLS *tls.Config
}

// Config 描述一个根 Redis 连接。
// Zone、Registration 和 Catalog 等领域身份不属于根配置，由共享该连接的领域客户端分别提供。
type Config struct {
	// Standalone 默认 nil；选择固定主节点拓扑时必须非 nil，并与 Sentinel 保持严格二选一。
	Standalone *Standalone
	// Sentinel 默认 nil；选择 Sentinel 主节点发现时必须非 nil，并与 Standalone 保持严格二选一。
	Sentinel *Sentinel
	// Timeout 限制单条普通 Redis 操作；零值使用 2 秒，允许范围为 10 毫秒至 15 秒且必须为整毫秒。
	Timeout time.Duration
	// ConnectTimeout 限制单次 TCP 连接和 TLS 握手；零值使用 5 秒，允许范围为 20 毫秒至 30 秒且必须为整毫秒。
	ConnectTimeout time.Duration
	// Pool 控制共享 Redis 连接池；零值结构展开为最少 1 条、最多 4 条连接和 10 秒空闲回收时间，范围见 PoolConfig。
	Pool PoolConfig
	// Reconnect 控制连接级恢复退避；零值结构展开为 100 毫秒起步、5 秒封顶、倍数 2 和 10% 抖动，范围见 ReconnectConfig。
	Reconnect ReconnectConfig
}

// Check 在不建立连接的情况下校验 Redis 拓扑、超时、连接池和重连配置。
// 零值默认项按运行时规则展开后检查，但不会写回接收者。
func (config Config) Check() error {
	_, err := config.normalize()
	return err
}

// runtimeConfig 保存校验后的公开配置和供驱动构建直接读取的展开值。
type runtimeConfig struct {
	// Config 保留调用方传入并通过校验的公开配置；私有字段保存展开后的实际运行值。
	Config
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
	// reconnectInitial 是连接恢复首次退避；默认展开值为 100 毫秒，范围为 10 毫秒至 5 秒。
	reconnectInitial time.Duration
	// reconnectMax 是连接恢复退避上限；默认展开值为 5 秒，范围为 100 毫秒至 30 秒且不小于 reconnectInitial。
	reconnectMax time.Duration
	// reconnectFactor 是连接恢复指数倍数；默认展开值为 2，范围为 1 至 8。
	reconnectFactor int
	// reconnectJitter 是连接恢复随机扣减百分比；默认展开值为 10，范围为 0 至 50。
	reconnectJitter int
}

// normalize 校验配置的拓扑互斥、地址和取值范围，并返回不可变的运行时配置。
// 返回错误时不会建立网络连接；成功结果中的 timeout 始终为正值。
func (config Config) normalize() (runtimeConfig, error) {
	// 拓扑检查：Standalone 与 Sentinel 必须二选一，不能同时缺失或同时配置。
	if (config.Standalone == nil) == (config.Sentinel == nil) {
		return runtimeConfig{}, protocolError(CodeInvalid, "topology", 0)
	}
	// Standalone 检查：固定主节点模式必须提供非空 host:port 地址。
	if config.Standalone != nil && strings.TrimSpace(config.Standalone.Address) == "" {
		return runtimeConfig{}, protocolError(CodeInvalid, "standalone.address", 0)
	}
	// Sentinel 检查：至少提供一个非空端点和非空 MasterName，且端点列表中不能出现空项。
	if config.Sentinel != nil {
		if len(config.Sentinel.Addresses) == 0 {
			return runtimeConfig{}, protocolError(CodeInvalid, "sentinel.addresses", 0)
		}
		if strings.TrimSpace(config.Sentinel.MasterName) == "" {
			return runtimeConfig{}, protocolError(CodeInvalid, "sentinel.master_name", 0)
		}
		for _, address := range config.Sentinel.Addresses {
			if strings.TrimSpace(address) == "" {
				return runtimeConfig{}, protocolError(CodeInvalid, "sentinel.addresses", 0)
			}
		}
	}

	// 保留原始公开值，同时把零值默认值展开到私有字段，避免驱动和热路径重复判断。
	result := runtimeConfig{
		Config:           config,
		timeout:          config.Timeout,
		connectTimeout:   config.ConnectTimeout,
		poolMin:          config.Pool.MinConnections,
		poolMax:          config.Pool.MaxConnections,
		poolIdle:         config.Pool.IdleTimeout,
		reconnectInitial: config.Reconnect.InitialDelay,
		reconnectMax:     config.Reconnect.MaxDelay,
		reconnectFactor:  config.Reconnect.Multiplier,
	}
	var ok bool
	// 命令超时检查：默认 2 秒，必须是 10 毫秒至 15 秒内的整毫秒值。
	result.timeout, ok = validate.Duration(result.timeout, 2*time.Second, 10*time.Millisecond, 15*time.Second)
	if !ok {
		return runtimeConfig{}, protocolError(CodeInvalid, "timeout", 0)
	}
	// 建连超时检查：默认 5 秒，必须是 20 毫秒至 30 秒内的整毫秒值。
	result.connectTimeout, ok = validate.Duration(
		result.connectTimeout,
		5*time.Second,
		20*time.Millisecond,
		30*time.Second,
	)
	if !ok {
		return runtimeConfig{}, protocolError(CodeInvalid, "connect_timeout", 0)
	}
	// 空闲回收检查：默认 10 秒，必须是 1 秒至 1 小时内的整毫秒值。
	result.poolIdle, ok = validate.Duration(
		result.poolIdle,
		10*time.Second,
		time.Second,
		time.Hour,
	)
	if !ok {
		return runtimeConfig{}, protocolError(CodeInvalid, "pool.idle_timeout", 0)
	}
	// 重连延迟检查：首次延迟默认 100 毫秒、范围 10 毫秒至 5 秒。
	result.reconnectInitial, ok = validate.Duration(
		result.reconnectInitial,
		100*time.Millisecond,
		10*time.Millisecond,
		5*time.Second,
	)
	if !ok {
		return runtimeConfig{}, protocolError(CodeInvalid, "reconnect.initial_delay", 0)
	}
	// 重连上限检查：最大延迟默认 5 秒、范围 100 毫秒至 30 秒。
	result.reconnectMax, ok = validate.Duration(
		result.reconnectMax,
		5*time.Second,
		100*time.Millisecond,
		30*time.Second,
	)
	if !ok {
		return runtimeConfig{}, protocolError(CodeInvalid, "reconnect.max_delay", 0)
	}
	// 整数默认值展开：连接池最少使用 1 条、最多使用 4 条连接，重连指数倍数使用 2。
	if result.poolMin == 0 {
		result.poolMin = 1
	}
	if result.poolMax == 0 {
		result.poolMax = 4
	}
	if result.reconnectFactor == 0 {
		result.reconnectFactor = 2
	}
	// 重连抖动检查：nil 使用 10%，显式值允许 0% 至 50%。
	result.reconnectJitter, ok = validate.OptionalInt(
		config.Reconnect.JitterPercent,
		10,
		0,
		50,
	)
	if !ok {
		return runtimeConfig{}, protocolError(CodeInvalid, "reconnect.jitter_percent", 0)
	}

	// 数据库检查：Standalone 或 Sentinel 数据节点的数据库编号必须在 0 至 255 之间。
	database := 0
	if config.Standalone != nil {
		database = config.Standalone.Database
	} else {
		database = config.Sentinel.Database
	}
	if database < 0 || database > 255 {
		return runtimeConfig{}, protocolError(CodeInvalid, "database", 0)
	}

	// 连接池检查：分别定位最少/最多连接数越界；关系失败归因于不能被满足的最小值。
	if result.poolMin < 1 || result.poolMin > 1024 {
		return runtimeConfig{}, protocolError(CodeInvalid, "pool.min_connections", 0)
	}
	if result.poolMax < 1 || result.poolMax > 1024 {
		return runtimeConfig{}, protocolError(CodeInvalid, "pool.max_connections", 0)
	}
	if result.poolMin > result.poolMax {
		return runtimeConfig{}, protocolError(CodeInvalid, "pool.min_connections", 0)
	}

	// 重连关系检查：指数倍数为 1 至 8，首次延迟不得超过最大延迟。
	if result.reconnectFactor < 1 || result.reconnectFactor > 8 {
		return runtimeConfig{}, protocolError(CodeInvalid, "reconnect.multiplier", 0)
	}
	if result.reconnectInitial > result.reconnectMax {
		return runtimeConfig{}, protocolError(CodeInvalid, "reconnect.initial_delay", 0)
	}
	return result, nil
}
