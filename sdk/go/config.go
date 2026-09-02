package verdandi

import (
	"crypto/tls"
	"net"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

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
	// Delay 是驱动重新建立连接前的固定等待；零值使用 100 毫秒，允许范围为 10 毫秒至 30 秒且必须为整毫秒。
	Delay time.Duration
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
	// TLS 默认 nil 并使用明文传输；非 nil 时启用 TLS，最低允许 TLS 1.2，且禁止 InsecureSkipVerify。
	// SDK 会复制配置容器；调用方仍须保证其中引用的证书、私钥和回调在 Client 生命周期内不可变。
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
	// TLS 默认 nil 并使用明文传输；非 nil 时启用 TLS，最低允许 TLS 1.2、禁止 InsecureSkipVerify，且 ServerName 必须非空。
	// ServerName 是所有 Sentinel 和所有可能成为主节点的 Redis 证书共同包含的固定身份，不取信动态发现地址。
	// SDK 会复制配置容器；调用方仍须保证其中引用的证书、私钥和回调在 Client 生命周期内不可变。
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
	// Reconnect 控制驱动连接恢复等待；零值结构展开为固定 100 毫秒，范围见 ReconnectConfig。
	Reconnect ReconnectConfig
}

// Check 在不建立连接的情况下校验 Redis 拓扑、超时、连接池和重连配置。
// 零值默认项按运行时规则展开后检查，但不会写回接收者。
func (config Config) Check() error {
	_, err := config.normalize()
	return err
}

// normalize 校验配置的拓扑互斥、地址和取值范围，并返回不可变的运行时配置。
// 返回错误时不会建立网络连接；成功结果中的 timeout 始终为正值。
func (config Config) normalize() (runtimeConfig, error) {
	// 拓扑检查：Standalone 与 Sentinel 必须二选一，不能同时缺失或同时配置。
	if (config.Standalone == nil) == (config.Sentinel == nil) {
		return runtimeConfig{}, protocolError(CodeInvalid, "topology", 0)
	}
	// Standalone 检查：固定主节点模式必须提供合法 host:port；TLS 必须保持验证且最低使用 TLS 1.2。
	if config.Standalone != nil {
		if !validEndpoint(config.Standalone.Address) {
			return runtimeConfig{}, protocolError(CodeInvalid, "standalone.address", 0)
		}
		if err := checkTLS(config.Standalone.TLS); err != nil {
			return runtimeConfig{}, err
		}
	}
	// Sentinel 检查：至少提供一个合法端点和规范 MasterName；TLS 使用跨全部动态节点共享的固定证书身份。
	if config.Sentinel != nil {
		if len(config.Sentinel.Addresses) == 0 {
			return runtimeConfig{}, protocolError(CodeInvalid, "sentinel.addresses", 0)
		}
		if !canonicalText(config.Sentinel.MasterName) {
			return runtimeConfig{}, protocolError(CodeInvalid, "sentinel.master_name", 0)
		}
		for _, address := range config.Sentinel.Addresses {
			if !validEndpoint(address) {
				return runtimeConfig{}, protocolError(CodeInvalid, "sentinel.addresses", 0)
			}
		}
		if err := checkTLS(config.Sentinel.TLS); err != nil {
			return runtimeConfig{}, err
		}
		if config.Sentinel.TLS != nil && config.Sentinel.TLS.ServerName == "" {
			return runtimeConfig{}, protocolError(CodeInvalid, "tls.server_name", 0)
		}
	}

	// 复制拓扑容器并展开零值默认项，避免驱动持有调用方可变的地址切片或 TLS 配置指针。
	result := runtimeConfig{
		timeout:        config.Timeout,
		connectTimeout: config.ConnectTimeout,
		poolMin:        config.Pool.MinConnections,
		poolMax:        config.Pool.MaxConnections,
		poolIdle:       config.Pool.IdleTimeout,
		reconnectDelay: config.Reconnect.Delay,
	}
	if config.Standalone != nil {
		standalone := *config.Standalone
		standalone.TLS = cloneTLS(config.Standalone.TLS)
		result.standalone = &standalone
	} else {
		sentinel := *config.Sentinel
		sentinel.Addresses = append([]string(nil), config.Sentinel.Addresses...)
		sentinel.TLS = cloneTLS(config.Sentinel.TLS)
		result.sentinel = &sentinel
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
	// 驱动重连检查：默认固定等待 100 毫秒，必须是 10 毫秒至 30 秒内的整毫秒值。
	result.reconnectDelay, ok = validate.Duration(
		result.reconnectDelay,
		100*time.Millisecond,
		10*time.Millisecond,
		30*time.Second,
	)
	if !ok {
		return runtimeConfig{}, protocolError(CodeInvalid, "reconnect.delay", 0)
	}
	// 连接池整数默认值展开：最少使用 1 条、最多使用 4 条连接。
	if result.poolMin == 0 {
		result.poolMin = 1
	}
	if result.poolMax == 0 {
		result.poolMax = 4
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

	return result, nil
}

// validEndpoint 接受域名、IPv4 或方括号 IPv6 的 host:port，并要求端口处于 Redis 可连接范围。
func validEndpoint(address string) bool {
	if !canonicalText(address) {
		return false
	}
	host, portText, err := net.SplitHostPort(address)
	if err != nil || !validEndpointHost(host) {
		return false
	}
	port, err := strconv.ParseUint(portText, 10, 16)
	return err == nil && port != 0 && decimalPort(portText)
}

// decimalPort 固定跨语言端口词法，只接受至少一个 ASCII 十进制数字。
func decimalPort(value string) bool {
	if value == "" {
		return false
	}
	for index := range len(value) {
		if value[index] < '0' || value[index] > '9' {
			return false
		}
	}
	return true
}

// validEndpointHost 拒绝主机部分中的 ASCII 空白和控制字符；Unicode 主机名仍交由平台解析器按原样处理。
func validEndpointHost(host string) bool {
	if host == "" {
		return false
	}
	for index := range len(host) {
		if host[index] <= ' ' || host[index] == 0x7f {
			return false
		}
	}
	return true
}

// canonicalText 拒绝空值、首尾空白和 NUL，避免不同驱动对配置文本执行不同的隐式修整。
func canonicalText(value string) bool {
	return value != "" && utf8.ValidString(value) && strings.TrimSpace(value) == value && strings.IndexByte(value, 0) < 0
}

// checkTLS 统一原生 Go TLS 的最低安全语义，并校验可选固定证书身份。
func checkTLS(config *tls.Config) error {
	if config == nil {
		return nil
	}
	if config.InsecureSkipVerify {
		return protocolError(CodeInvalid, "tls.insecure_skip_verify", 0)
	}
	if config.MinVersion != 0 && config.MinVersion < tls.VersionTLS12 {
		return protocolError(CodeInvalid, "tls.min_version", 0)
	}
	if config.MaxVersion != 0 && config.MaxVersion < tls.VersionTLS12 {
		return protocolError(CodeInvalid, "tls.max_version", 0)
	}
	if config.MinVersion != 0 && config.MaxVersion != 0 && config.MinVersion > config.MaxVersion {
		return protocolError(CodeInvalid, "tls.min_version", 0)
	}
	if config.ServerName != "" && (!canonicalText(config.ServerName) || hasASCIIWhitespace(config.ServerName) || len(config.ServerName) > 253) {
		return protocolError(CodeInvalid, "tls.server_name", 0)
	}
	return nil
}

// hasASCIIWhitespace 判断文本是否含协议和各 TLS 驱动都不应隐式修整的 ASCII 空白。
func hasASCIIWhitespace(value string) bool {
	for index := range len(value) {
		switch value[index] {
		case ' ', '\t', '\n', '\r', '\v', '\f':
			return true
		}
	}
	return false
}

// cloneTLS 复制可变 TLS 容器和证书池；私钥对象及回调仍按 crypto/tls 的惯例由调用方保持不可变。
func cloneTLS(config *tls.Config) *tls.Config {
	if config == nil {
		return nil
	}
	result := config.Clone()
	if result.MinVersion == 0 {
		result.MinVersion = tls.VersionTLS12
	}
	if config.RootCAs != nil {
		result.RootCAs = config.RootCAs.Clone()
	}
	if config.ClientCAs != nil {
		result.ClientCAs = config.ClientCAs.Clone()
	}
	return result
}
