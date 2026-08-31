// Package configuration 定义 Go/Rust 共用的 JSON 配置结构。
//
// JSON 层只使用字符串、布尔值、十进制整数、数组和嵌套对象；加载后再转换成
// Verdandi 各领域使用的 time.Duration、TLS、驱动拓扑和本地路径等精确类型。
package configuration

// Config 是一份版本化 Verdandi JSON 配置。
type Config struct {
	// Version 是配置结构版本；首版必须为 "v1"。
	Version string `json:"version"`
	// Redis 是必需的共享 Redis 传输配置。
	Redis Redis `json:"redis"`
	// Registration 是可选的 Registration/Selector 配置；nil 表示当前进程不创建该领域 Client。
	Registration *Registration `json:"registration,omitempty"`
	// Catalog 是可选的 Catalog 配置；nil 表示当前进程不创建 Catalog Client。
	Catalog *Catalog `json:"catalog,omitempty"`
}

// Auth 保存一个 Redis ACL 身份；空字段表示不发送对应凭据。
type Auth struct {
	// Username 是 ACL 用户名；默认空字符串并使用 Redis 默认用户。
	Username string `json:"username"`
	// Password 是 ACL 密码；默认空字符串并且不发送密码。
	Password string `json:"password"`
}

// TLS 描述 Go/Rust 共用的 Redis TLS 信任、SNI 和客户端证书文件。
type TLS struct {
	// Enabled 控制是否启用 TLS；默认 false。
	Enabled bool `json:"enabled"`
	// SystemRoots 控制是否信任操作系统根证书；省略使用 true，false 时必须提供 CAFile。
	SystemRoots *bool `json:"system_roots,omitempty"`
	// ServerName 覆盖证书校验和握手使用的服务名；默认空字符串并使用连接地址，仅 Standalone 允许配置，UTF-8 字节数上限为 253。
	ServerName string `json:"server_name"`
	// CAFile 是追加信任的 PEM CA bundle 路径；默认空字符串，每个文件读取上限为 1 MiB。
	CAFile string `json:"ca_file"`
	// CertFile 是 PEM 客户端证书链路径；默认空字符串，必须与 KeyFile 同时配置，每个文件读取上限为 1 MiB。
	CertFile string `json:"cert_file"`
	// KeyFile 是未加密 PEM 客户端私钥路径；默认空字符串，必须与 CertFile 同时配置，每个文件读取上限为 1 MiB。
	KeyFile string `json:"key_file"`
}

// Redis 描述 Standalone 或 Sentinel 传输及共享连接行为。
type Redis struct {
	// Mode 必须为 "standalone" 或 "sentinel"；Redis Cluster 不受支持。
	Mode string `json:"mode"`
	// Addresses 在 Standalone 模式必须恰好包含一个 host:port，在 Sentinel 模式必须至少包含一个 Sentinel host:port。
	Addresses []string `json:"addresses"`
	// MasterName 是 Sentinel 监控的服务名；Sentinel 模式必需，Standalone 模式必须为空。
	MasterName string `json:"master_name"`
	// Auth 是 Redis 数据节点使用的 ACL 身份。
	Auth Auth `json:"auth"`
	// SentinelAuth 是 Sentinel 自身使用的独立 ACL 身份；Standalone 模式必须为空。
	SentinelAuth Auth `json:"sentinel_auth"`
	// Database 是逻辑数据库编号；省略使用 0，允许范围为 0 至 255。
	Database *int64 `json:"database,omitempty"`
	// TLS 控制加密、系统/私有信任根、Standalone SNI 覆盖和可选双向 TLS；子字段均可省略并采用默认值。
	TLS TLS `json:"tls"`
	// TimeoutMS 是单条普通 Redis 命令总等待上限；省略使用 2000，允许范围为 10 至 15000。
	TimeoutMS *int64 `json:"timeout_ms,omitempty"`
	// ConnectTimeoutMS 是单次 TCP/TLS 建连上限；省略使用 5000，允许范围为 20 至 30000。
	ConnectTimeoutMS *int64 `json:"connect_timeout_ms,omitempty"`
	// Pool 控制共享连接池；省略字段分别使用 1、4 和 10000 毫秒。
	Pool Pool `json:"pool"`
	// Reconnect 控制连接恢复退避，不会重放已发送的业务命令。
	Reconnect Reconnect `json:"reconnect"`
}

// Pool 描述共享 Redis 连接池。
type Pool struct {
	// MinConnections 是最少保留连接数；省略使用 1，允许范围为 1 至 1024。
	MinConnections *int64 `json:"min_connections,omitempty"`
	// MaxConnections 是最多连接数；省略使用 4，允许范围为 1 至 1024，且不得小于 MinConnections。
	MaxConnections *int64 `json:"max_connections,omitempty"`
	// IdleTimeoutMS 是多余空闲连接的回收时间；省略使用 10000，允许范围为 1000 至 3600000。
	IdleTimeoutMS *int64 `json:"idle_timeout_ms,omitempty"`
}

// Reconnect 描述 Redis 连接恢复退避。
type Reconnect struct {
	// InitialDelayMS 是首次恢复等待；省略使用 100，允许范围为 10 至 5000。
	InitialDelayMS *int64 `json:"initial_delay_ms,omitempty"`
	// MaxDelayMS 是恢复等待上限；省略使用 5000，允许范围为 100 至 30000。
	MaxDelayMS *int64 `json:"max_delay_ms,omitempty"`
	// Multiplier 是指数增长倍数；省略使用 2，允许范围为 1 至 8。
	Multiplier *int64 `json:"multiplier,omitempty"`
	// JitterPercent 是随机抖动百分比；省略使用 10，允许范围为 0 至 50，零表示禁用。
	JitterPercent *int64 `json:"jitter_percent,omitempty"`
}

// Registration 描述一个 Zone 的 Registration、Redis 策略默认值和 Selector 本地行为。
type Registration struct {
	// Zone 是 1 至 32 字节、仅含大小写 ASCII 字母的管理隔离标识。
	Zone string `json:"zone"`
	// BufferCapacity 是单个 Registration 邮箱允许的结果等待者数；省略使用 8，允许范围为 1 至 256。
	BufferCapacity *int64 `json:"buffer_capacity,omitempty"`
	// ErrorBufferCapacity 是 Registration 异步诊断容量；省略使用 16，允许范围为 1 至 1024。
	ErrorBufferCapacity *int64 `json:"error_buffer_capacity,omitempty"`
	// MinRenewIntervalMS 是最短续期间隔；省略使用 100，允许范围为 10 至 60000。
	MinRenewIntervalMS *int64 `json:"min_renew_interval_ms,omitempty"`
	// RenewJitterPercent 是续期抖动；省略使用 10，允许范围为 0 至 50，零表示禁用。
	RenewJitterPercent *int64 `json:"renew_jitter_percent,omitempty"`
	// PolicyRefreshJitterPercent 是 Redis 策略刷新抖动；省略使用 10，允许范围为 0 至 50，零表示禁用。
	PolicyRefreshJitterPercent *int64 `json:"policy_refresh_jitter_percent,omitempty"`
	// Policy 是首次初始化 Redis Zone 配置时使用的默认限制。
	Policy RegistrationPolicy `json:"policy"`
	// Selector 是同一 Zone 的 Selector 本地同步、视图和恢复配置。
	Selector Selector `json:"selector"`
}

// RegistrationPolicy 描述 Redis 中可由管理员后续修改的 Registration 限制默认值。
type RegistrationPolicy struct {
	// AttrMaxFields 是 Attr 顶层字段上限；省略使用 16，允许范围为 1 至 128。
	AttrMaxFields *int64 `json:"attr_max_fields,omitempty"`
	// DataMaxFields 是 Data 顶层字段上限；省略使用 32，允许范围为 1 至 128。
	DataMaxFields *int64 `json:"data_max_fields,omitempty"`
	// FieldNameMaxBytes 是字段名字节上限；省略使用 64，允许范围为 1 至 64。
	FieldNameMaxBytes *int64 `json:"field_name_max_bytes,omitempty"`
	// AttrValueMaxBytes 是单个 Attr 值字节上限；省略使用 128，允许范围为 1 至 16384。
	AttrValueMaxBytes *int64 `json:"attr_value_max_bytes,omitempty"`
	// DataValueMaxBytes 是单个 Data 值字节上限；省略使用 128，允许范围为 1 至 16384。
	DataValueMaxBytes *int64 `json:"data_value_max_bytes,omitempty"`
	// RecordMaxBytes 是完整 Registration 字节上限；省略使用 16384，允许范围为 1 至 65536。
	RecordMaxBytes *int64 `json:"record_max_bytes,omitempty"`
	// RefreshMS 是 SDK 重新读取 Redis 策略的周期；省略使用 30000，允许范围为 1000 至 86400000。
	RefreshMS *int64 `json:"refresh_ms,omitempty"`
}

// Selector 描述一个 Selector 的同步、视图、RedisClock 和恢复行为。
type Selector struct {
	// ScanPageSize 是 HSCAN 目标页大小；省略使用 256，允许范围为 1 至 1024。
	ScanPageSize *int64 `json:"scan_page_size,omitempty"`
	// MaxPendingEntries 是同步期间待处理 UUID 上限；省略使用 4096，允许范围为 1 至 65536。
	MaxPendingEntries *int64 `json:"max_pending_entries,omitempty"`
	// MaxPendingBytes 是同步期间待处理事件字节上限；省略使用 64 MiB，允许范围为 1 字节至 1 GiB。
	MaxPendingBytes *int64 `json:"max_pending_bytes,omitempty"`
	// ViewPublishIntervalMS 是视图最短发布间隔；省略使用 10，允许范围为 0 至 1000，零表示立即发布。
	ViewPublishIntervalMS *int64 `json:"view_publish_interval_ms,omitempty"`
	// SyncTimeoutMS 是一代完整或定向同步总上限；省略使用 30000，允许范围为 100 至 3600000。
	SyncTimeoutMS *int64 `json:"sync_timeout_ms,omitempty"`
	// MaxActiveBytes 是活动视图字节预算；省略使用 256 MiB，允许范围为 1 字节至 1 GiB。
	MaxActiveBytes *int64 `json:"max_active_bytes,omitempty"`
	// MaxRetainedBytes 是 retained 视图字节预算；省略使用 64 MiB，允许范围为 0 至 1 GiB，零表示禁用。
	MaxRetainedBytes *int64 `json:"max_retained_bytes,omitempty"`
	// ClockRefreshIntervalMS 是 RedisClock 校准周期；省略使用 30000，允许范围为 1000 至 3600000。
	ClockRefreshIntervalMS *int64 `json:"clock_refresh_interval_ms,omitempty"`
	// ClockUncertaintyMS 是 RedisClock 额外保守误差；省略使用 1，允许范围为 0 至 1000。
	ClockUncertaintyMS *int64 `json:"clock_uncertainty_ms,omitempty"`
	// ErrorBufferCapacity 是 Selector 异步诊断容量；省略使用 16，允许范围为 1 至 1024。
	ErrorBufferCapacity *int64 `json:"error_buffer_capacity,omitempty"`
	// Recovery 描述 Selector 权威恢复退避。
	Recovery Recovery `json:"recovery"`
}

// Recovery 是 Selector/Catalog 共用形状的恢复退避配置。
type Recovery struct {
	// InitialDelayMS 是首次恢复等待；由所属领域定义默认值，允许范围均为 10 至 5000。
	InitialDelayMS *int64 `json:"initial_delay_ms,omitempty"`
	// MaxDelayMS 是恢复等待上限；省略使用 5000，允许范围为 100 至 30000。
	MaxDelayMS *int64 `json:"max_delay_ms,omitempty"`
	// Multiplier 是指数增长倍数；省略使用 2，允许范围为 1 至 8。
	Multiplier *int64 `json:"multiplier,omitempty"`
	// JitterPercent 是随机抖动百分比；由所属领域定义默认值，允许范围为 0 至 50。
	JitterPercent *int64 `json:"jitter_percent,omitempty"`
}

// Catalog 描述一个 Zone 的同步、容量、恢复和可选本地检查点配置。
type Catalog struct {
	// Zone 是 1 至 32 字节、仅含大小写 ASCII 字母的管理隔离标识。
	Zone string `json:"zone"`
	// SyncTimeoutMS 是初始同步或权威修复总上限；省略使用 30000，允许范围为 100 至 3600000。
	SyncTimeoutMS *int64 `json:"sync_timeout_ms,omitempty"`
	// ScanPageSize 是索引扫描目标页大小；省略使用 256，允许范围为 1 至 4096。
	ScanPageSize *int64 `json:"scan_page_size,omitempty"`
	// MaxInflightReads 是权威同步并发读取上限；省略使用 32，允许范围为 1 至 256。
	MaxInflightReads *int64 `json:"max_inflight_reads,omitempty"`
	// EventBufferCapacity 是待修复 Path 容量；省略使用 256，允许范围为 1 至 65536。
	EventBufferCapacity *int64 `json:"event_buffer_capacity,omitempty"`
	// ErrorBufferCapacity 是异步诊断容量；省略使用 64，允许范围为 1 至 4096。
	ErrorBufferCapacity *int64 `json:"error_buffer_capacity,omitempty"`
	// MaxViewBytes 是完整内存视图编码预算；省略或零表示不额外限制，最大 64 GiB。
	MaxViewBytes *int64 `json:"max_view_bytes,omitempty"`
	// MaxRecordBytes 是单条完整值上限；省略使用 512 KiB，允许范围为 1 字节至 4 MiB。
	MaxRecordBytes *int64 `json:"max_record_bytes,omitempty"`
	// Recovery 描述 Catalog 权威恢复退避；首次等待省略使用 250 毫秒，抖动省略使用 10%。
	Recovery Recovery `json:"recovery"`
	// LocalStorePath 是可丢弃本地检查点路径；省略禁用，显式空字符串非法。
	LocalStorePath *string `json:"local_store_path,omitempty"`
}
