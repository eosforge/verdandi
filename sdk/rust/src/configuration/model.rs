use serde::Deserialize;

/// 一份版本化 Verdandi JSON 配置。
#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    /// 配置结构版本；首版必须为 `v1`。
    pub version: String,
    /// 必需的共享 Redis 传输配置。
    pub redis: Redis,
    /// 可选 Registration/Selector 配置；缺失表示当前进程不创建该领域 Client。
    pub registration: Option<Registration>,
    /// 可选 Catalog 配置；缺失表示当前进程不创建 Catalog Client。
    pub catalog: Option<Catalog>,
}

/// Redis ACL 身份；本类型不实现 Debug，避免密码进入派生诊断输出。
#[derive(Clone, Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Auth {
    /// ACL 用户名；默认空字符串并使用 Redis 默认用户。
    pub username: String,
    /// ACL 密码；默认空字符串并且不发送密码。
    pub password: String,
}

/// Go/Rust 共用的 Redis TLS 信任、SNI 和客户端证书文件。
#[derive(Clone, Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Tls {
    /// 是否启用 TLS；默认 false。
    pub enabled: bool,
    /// 是否信任操作系统根证书；省略使用 true，false 时必须提供 `ca_file`。
    pub system_roots: Option<bool>,
    /// 覆盖证书校验和握手使用的服务名；默认空字符串并使用连接地址，仅 Standalone 允许配置，UTF-8 字节数上限为 253。
    pub server_name: String,
    /// 追加信任的 PEM CA bundle 路径；默认空字符串，每个文件读取上限为 1 MiB。
    pub ca_file: String,
    /// PEM 客户端证书链路径；默认空字符串，必须与 `key_file` 同时配置，每个文件读取上限为 1 MiB。
    pub cert_file: String,
    /// 未加密 PEM 客户端私钥路径；默认空字符串，必须与 `cert_file` 同时配置，每个文件读取上限为 1 MiB。
    pub key_file: String,
}

/// Standalone 或 Sentinel 传输及共享连接行为。
#[derive(Clone, Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Redis {
    /// 必须为 `standalone` 或 `sentinel`；Redis Cluster 不受支持。
    pub mode: String,
    /// Standalone 必须恰好一个 host:port；Sentinel 必须至少一个 Sentinel host:port。
    pub addresses: Vec<String>,
    /// Sentinel 服务名；Sentinel 必需，Standalone 必须为空。
    pub master_name: String,
    /// Redis 数据节点 ACL 身份。
    pub auth: Auth,
    /// Sentinel 自身的独立 ACL 身份；Standalone 必须为空。
    pub sentinel_auth: Auth,
    /// Redis 逻辑数据库；省略使用 0，允许 0 至 255。
    pub database: Option<i64>,
    /// 加密、系统/私有信任根、Standalone SNI 覆盖和可选双向 TLS；子字段均可省略并采用默认值。
    pub tls: Tls,
    /// 单条普通命令超时毫秒；省略使用 2000，允许 10 至 15000。
    pub timeout_ms: Option<i64>,
    /// 单次 TCP/TLS 建连超时毫秒；省略使用 5000，允许 20 至 30000。
    pub connect_timeout_ms: Option<i64>,
    /// 共享连接池设置；子字段省略时分别使用 1、4 和 10000 毫秒。
    pub pool: Pool,
    /// 连接恢复退避；不会重放已经发送的业务命令。
    pub reconnect: Reconnect,
}

/// 共享 Redis 连接池配置。
#[derive(Clone, Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Pool {
    /// 最少保留连接数；省略使用 1，允许 1 至 1024。
    pub min_connections: Option<i64>,
    /// 最多连接数；省略使用 4，允许 1 至 1024，且不小于最少连接数。
    pub max_connections: Option<i64>,
    /// 多余空闲连接回收毫秒；省略使用 10000，允许 1000 至 3600000。
    pub idle_timeout_ms: Option<i64>,
}

/// Redis 连接恢复退避配置。
#[derive(Clone, Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Reconnect {
    /// 首次恢复等待毫秒；省略使用 100，允许 10 至 5000。
    pub initial_delay_ms: Option<i64>,
    /// 恢复等待上限毫秒；省略使用 5000，允许 100 至 30000。
    pub max_delay_ms: Option<i64>,
    /// 指数增长倍数；省略使用 2，允许 1 至 8。
    pub multiplier: Option<i64>,
    /// 随机抖动百分比；省略使用 10，允许 0 至 50，零禁用。
    pub jitter_percent: Option<i64>,
}

/// 一个 Zone 的 Registration、Redis 策略默认值和 Selector 本地行为。
#[derive(Clone, Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Registration {
    /// 1 至 32 字节、仅含大小写 ASCII 字母的管理隔离标识。
    pub zone: String,
    /// 单 Registration 邮箱等待者容量；省略使用 8，允许 1 至 256。
    pub buffer_capacity: Option<i64>,
    /// Registration 异步诊断容量；省略使用 16，允许 1 至 1024。
    pub error_buffer_capacity: Option<i64>,
    /// 最短续期间隔毫秒；省略使用 100，允许 10 至 60000。
    pub min_renew_interval_ms: Option<i64>,
    /// 续期抖动百分比；省略使用 10，允许 0 至 50，零禁用。
    pub renew_jitter_percent: Option<i64>,
    /// Redis 策略刷新抖动百分比；省略使用 10，允许 0 至 50，零禁用。
    pub policy_refresh_jitter_percent: Option<i64>,
    /// 首次初始化 Redis Zone 配置时使用的默认限制。
    pub policy: RegistrationPolicy,
    /// 同一 Zone 的 Selector 本地同步、视图和恢复配置。
    pub selector: Selector,
}

/// Redis 中可由管理员后续修改的 Registration 限制默认值。
#[derive(Clone, Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct RegistrationPolicy {
    /// Attr 顶层字段上限；省略使用 16，允许 1 至 128。
    pub attr_max_fields: Option<i64>,
    /// Data 顶层字段上限；省略使用 32，允许 1 至 128。
    pub data_max_fields: Option<i64>,
    /// 字段名字节上限；省略使用 64，允许 1 至 64。
    pub field_name_max_bytes: Option<i64>,
    /// 单个 Attr 值字节上限；省略使用 128，允许 1 至 16384。
    pub attr_value_max_bytes: Option<i64>,
    /// 单个 Data 值字节上限；省略使用 128，允许 1 至 16384。
    pub data_value_max_bytes: Option<i64>,
    /// 完整 Registration 字节上限；省略使用 16384，允许 1 至 65536。
    pub record_max_bytes: Option<i64>,
    /// SDK 重新读取 Redis 策略的周期毫秒；省略使用 30000，允许 1000 至 86400000。
    pub refresh_ms: Option<i64>,
}

/// 一个 Selector 的同步、视图、RedisClock 和恢复行为。
#[derive(Clone, Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Selector {
    /// HSCAN 目标页大小；省略使用 256，允许 1 至 1024。
    pub scan_page_size: Option<i64>,
    /// 同步期间待处理 UUID 上限；省略使用 4096，允许 1 至 65536。
    pub max_pending_entries: Option<i64>,
    /// 同步期间待处理事件字节上限；省略使用 64 MiB，允许 1 字节至 1 GiB。
    pub max_pending_bytes: Option<i64>,
    /// 视图最短发布间隔毫秒；省略使用 10，允许 0 至 1000，零立即发布。
    pub view_publish_interval_ms: Option<i64>,
    /// 一代完整或定向同步总上限毫秒；省略使用 30000，允许 100 至 3600000。
    pub sync_timeout_ms: Option<i64>,
    /// 活动视图字节预算；省略使用 256 MiB，允许 1 字节至 1 GiB。
    pub max_active_bytes: Option<i64>,
    /// retained 视图字节预算；省略使用 64 MiB，允许 0 至 1 GiB，零禁用。
    pub max_retained_bytes: Option<i64>,
    /// RedisClock 校准周期毫秒；省略使用 30000，允许 1000 至 3600000。
    pub clock_refresh_interval_ms: Option<i64>,
    /// RedisClock 附加保守误差毫秒；省略使用 1，允许 0 至 1000。
    pub clock_uncertainty_ms: Option<i64>,
    /// Selector 异步诊断容量；省略使用 16，允许 1 至 1024。
    pub error_buffer_capacity: Option<i64>,
    /// Selector 权威恢复退避。
    pub recovery: Recovery,
}

/// Selector/Catalog 共用形状的恢复退避配置。
#[derive(Clone, Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Recovery {
    /// 首次恢复等待毫秒；省略值由所属领域定义，允许 10 至 5000。
    pub initial_delay_ms: Option<i64>,
    /// 恢复等待上限毫秒；省略使用 5000，允许 100 至 30000。
    pub max_delay_ms: Option<i64>,
    /// 指数增长倍数；省略使用 2，允许 1 至 8。
    pub multiplier: Option<i64>,
    /// 随机抖动百分比；省略值由所属领域定义，允许 0 至 50。
    pub jitter_percent: Option<i64>,
}

/// 一个 Zone 的 Catalog 同步、容量、恢复和可选检查点配置。
#[derive(Clone, Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Catalog {
    /// 1 至 32 字节、仅含大小写 ASCII 字母的管理隔离标识。
    pub zone: String,
    /// 初始同步或权威修复总上限毫秒；省略使用 30000，允许 100 至 3600000。
    pub sync_timeout_ms: Option<i64>,
    /// 索引扫描目标页大小；省略使用 256，允许 1 至 4096。
    pub scan_page_size: Option<i64>,
    /// 权威同步并发读取上限；省略使用 32，允许 1 至 256。
    pub max_inflight_reads: Option<i64>,
    /// 待修复 Path 容量；省略使用 256，允许 1 至 65536。
    pub event_buffer_capacity: Option<i64>,
    /// 异步诊断容量；省略使用 64，允许 1 至 4096。
    pub error_buffer_capacity: Option<i64>,
    /// 完整内存视图编码预算；省略或零不额外限制，最大 64 GiB。
    pub max_view_bytes: Option<i64>,
    /// 单条完整值上限；省略使用 512 KiB，允许 1 字节至 4 MiB。
    pub max_record_bytes: Option<i64>,
    /// Catalog 权威恢复退避；首次等待默认 250 毫秒，抖动默认 10%。
    pub recovery: Recovery,
    /// 可丢弃本地检查点路径；省略禁用，显式空字符串非法。
    pub local_store_path: Option<String>,
}
