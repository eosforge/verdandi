use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::Duration;

use url::Url;

use super::model::{Auth, Config, Recovery, RegistrationPolicy, Selector, Tls};
use crate::catalog;
use crate::registration::{self, RegistrationLimits};
use crate::{Code, Config as RedisConfig, Error, PoolConfig, ReconnectConfig, Result, TlsConfig};

const MAXIMUM_JSON_BYTES: u64 = 1024 * 1024;

impl Config {
    /// 严格解析一份不超过 1 MiB 的 UTF-8 JSON，并立即验证所有启用领域。
    ///
    /// 未知字段、重复字段、尾随值、错误类型和不支持的版本都返回 Invalid。
    pub fn from_json(source: &[u8]) -> Result<Self> {
        if u64::try_from(source.len()).unwrap_or(u64::MAX) > MAXIMUM_JSON_BYTES {
            return Err(Error::field(Code::Capacity, "json"));
        }
        let config: Self = serde_json::from_slice(source).map_err(|error| Error::field_driver(Code::Invalid, "json", error))?;
        let tree: serde_json::Value = serde_json::from_slice(source).map_err(|error| Error::field_driver(Code::Invalid, "json", error))?;
        if contains_null(&tree) {
            return Err(Error::field(Code::Invalid, "json"));
        }
        config.check()?;
        Ok(config)
    }

    /// 从 path 读取并严格解析 JSON 配置；读取量硬限制为 1 MiB 加一个探测字节。
    pub fn load_json(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref();
        if path.as_os_str().is_empty() {
            return Err(Error::field(Code::Invalid, "path"));
        }
        let file = File::open(path).map_err(|error| Error::field_driver(Code::Unavailable, "path", error))?;
        let mut source = Vec::new();
        file.take(MAXIMUM_JSON_BYTES + 1)
            .read_to_end(&mut source)
            .map_err(|error| Error::field_driver(Code::Unavailable, "json", error))?;
        Self::from_json(&source)
    }

    /// 验证结构版本，并把 Redis 及所有已启用领域转换成语言原生精确配置。
    pub fn check(&self) -> Result<()> {
        if self.version != "v1" {
            return Err(Error::field(Code::Invalid, "version"));
        }
        self.redis_config()?;
        self.registration_config()?;
        self.catalog_config()?;
        Ok(())
    }

    /// 把共享 JSON Redis 设置转换为 Rust 的 URL、Duration 和连接池配置。
    pub fn redis_config(&self) -> Result<RedisConfig> {
        let source = &self.redis;
        check_addresses(&source.addresses)?;
        let database = unsigned(source.database, 0, 0, 255, "redis.database")?;
        let timeout = duration(source.timeout_ms, 2000, 10, 15_000, "redis.timeout_ms")?;
        let connect_timeout = duration(source.connect_timeout_ms, 5000, 20, 30_000, "redis.connect_timeout_ms")?;
        let pool_min = size(source.pool.min_connections, 1, 1, 1024, "redis.pool.min_connections")?;
        let pool_max = size(source.pool.max_connections, 4, 1, 1024, "redis.pool.max_connections")?;
        let pool_idle = duration(source.pool.idle_timeout_ms, 10_000, 1000, 3_600_000, "redis.pool.idle_timeout_ms")?;
        let reconnect_initial = duration(source.reconnect.initial_delay_ms, 100, 10, 5000, "redis.reconnect.initial_delay_ms")?;
        let reconnect_max = duration(source.reconnect.max_delay_ms, 5000, 100, 30_000, "redis.reconnect.max_delay_ms")?;
        let reconnect_multiplier = u32_value(source.reconnect.multiplier, 2, 1, 8, "redis.reconnect.multiplier")?;
        let reconnect_jitter = u8_value(source.reconnect.jitter_percent, 10, 0, 50, "redis.reconnect.jitter_percent")?;

        // 先用 URL 类型完成凭据和查询参数编码，再把稳定文本交给现有 Fred 适配层。
        let endpoint = match source.mode.as_str() {
            "standalone" => {
                if source.addresses.len() != 1 || !source.master_name.is_empty() || !empty_auth(&source.sentinel_auth) {
                    return Err(Error::field(Code::Invalid, "redis.topology"));
                }
                endpoint("redis", &source.addresses, &source.auth, None)?
            }
            "sentinel" => {
                if source.master_name.trim().is_empty() || source.master_name.trim() != source.master_name {
                    return Err(Error::field(Code::Invalid, "redis.master_name"));
                }
                endpoint(
                    "redis-sentinel",
                    &source.addresses,
                    &source.auth,
                    Some((&source.master_name, &source.sentinel_auth)),
                )?
            }
            _ => return Err(Error::field(Code::Invalid, "redis.mode")),
        };

        let native = RedisConfig {
            endpoint,
            database: u8::try_from(database).map_err(|_| Error::field(Code::Capacity, "redis.database"))?,
            tls: tls_config(&source.tls, &source.mode)?,
            timeout,
            connect_timeout,
            pool: PoolConfig {
                min_connections: pool_min,
                max_connections: pool_max,
                idle_timeout: pool_idle,
            },
            reconnect: ReconnectConfig {
                initial_delay: reconnect_initial,
                max_delay: reconnect_max,
                multiplier: reconnect_multiplier,
                jitter_percent: reconnect_jitter,
            },
        };
        native.check()?;
        Ok(native)
    }

    /// 把可选 JSON Registration/Selector 设置转换为 Rust 原生配置。
    pub fn registration_config(&self) -> Result<Option<registration::Config>> {
        let Some(source) = &self.registration else {
            return Ok(None);
        };
        let mut native = registration::Config::new(&source.zone);
        native.registration_buffer_capacity = size(source.buffer_capacity, 8, 1, 256, "registration.buffer_capacity")?;
        native.registration_error_buffer_capacity = size(source.error_buffer_capacity, 16, 1, 1024, "registration.error_buffer_capacity")?;
        native.minimum_renew_interval = duration(source.min_renew_interval_ms, 100, 10, 60_000, "registration.min_renew_interval_ms")?;
        native.renew_jitter_percent = u8_value(source.renew_jitter_percent, 10, 0, 50, "registration.renew_jitter_percent")?;
        native.policy_refresh_jitter_percent = u8_value(source.policy_refresh_jitter_percent, 10, 0, 50, "registration.policy_refresh_jitter_percent")?;
        native.policy = registration_policy(&source.policy)?;
        fill_selector(&mut native, &source.selector)?;
        native.check()?;
        Ok(Some(native))
    }

    /// 把可选 JSON Catalog 设置转换为 Rust 原生配置。
    pub fn catalog_config(&self) -> Result<Option<catalog::Config>> {
        let Some(source) = &self.catalog else {
            return Ok(None);
        };
        let mut native = catalog::Config::new(&source.zone);
        native.sync_timeout = duration(source.sync_timeout_ms, 30_000, 100, 3_600_000, "catalog.sync_timeout_ms")?;
        native.scan_page_size = size(source.scan_page_size, 256, 1, 4096, "catalog.scan_page_size")?;
        native.max_inflight_reads = size(source.max_inflight_reads, 32, 1, 256, "catalog.max_inflight_reads")?;
        native.event_buffer_capacity = size(source.event_buffer_capacity, 256, 1, 65_536, "catalog.event_buffer_capacity")?;
        native.error_buffer_capacity = size(source.error_buffer_capacity, 64, 1, 4096, "catalog.error_buffer_capacity")?;
        native.max_view_bytes = unsigned(source.max_view_bytes, 0, 0, 64 * 1024 * 1024 * 1024, "catalog.max_view_bytes")?;
        native.max_record_bytes = size(source.max_record_bytes, 512 * 1024, 1, 4 * 1024 * 1024, "catalog.max_record_bytes")?;
        fill_catalog_recovery(&mut native, &source.recovery)?;
        native.local_store_path = source.local_store_path.as_ref().map(PathBuf::from);
        native.check()?;
        Ok(Some(native))
    }
}

/// 校验 JSON TLS 对象并转换为延迟加载 PEM 文件的 Rust 原生配置。
fn tls_config(source: &Tls, mode: &str) -> Result<Option<TlsConfig>> {
    let system_roots = source.system_roots.unwrap_or(true);

    // 禁用 TLS 时只允许保留默认信任根选择，避免静默忽略证书或 SNI 配置。
    if !source.enabled {
        if !system_roots || !source.server_name.is_empty() || !source.ca_file.is_empty() || !source.cert_file.is_empty() || !source.key_file.is_empty() {
            return Err(Error::field(Code::Invalid, "redis.tls"));
        }
        return Ok(None);
    }

    // Fred 无法把固定服务名传播到 Sentinel 后续发现的新主节点，因此跨语言契约只在 Standalone 开放覆盖。
    if !source.server_name.is_empty()
        && (source.server_name.trim() != source.server_name
            || source.server_name.len() > 253
            || source.server_name.as_bytes().contains(&0)
            || mode != "standalone")
    {
        return Err(Error::field(Code::Invalid, "redis.tls.server_name"));
    }

    // 关闭系统根时必须提供私有根；客户端证书链和私钥只能同时缺失或同时存在。
    if !system_roots && source.ca_file.is_empty() {
        return Err(Error::field(Code::Invalid, "redis.tls.ca_file"));
    }
    if source.cert_file.is_empty() != source.key_file.is_empty() {
        return Err(Error::field(Code::Invalid, "redis.tls.cert_file"));
    }
    for (field, path) in [
        ("redis.tls.ca_file", source.ca_file.as_str()),
        ("redis.tls.cert_file", source.cert_file.as_str()),
        ("redis.tls.key_file", source.key_file.as_str()),
    ] {
        if !path.is_empty() && (path.len() > 4096 || path.as_bytes().contains(&0)) {
            return Err(Error::field(Code::Invalid, field));
        }
    }

    Ok(Some(TlsConfig {
        system_roots,
        server_name: (!source.server_name.is_empty()).then(|| source.server_name.clone()),
        ca_file: (!source.ca_file.is_empty()).then(|| PathBuf::from(&source.ca_file)),
        cert_file: (!source.cert_file.is_empty()).then(|| PathBuf::from(&source.cert_file)),
        key_file: (!source.key_file.is_empty()).then(|| PathBuf::from(&source.key_file)),
    }))
}

/// 递归拒绝 JSON null，使“省略采用默认值”不会与显式空值混为一谈。
fn contains_null(value: &serde_json::Value) -> bool {
    match value {
        serde_json::Value::Null => true,
        serde_json::Value::Array(values) => values.iter().any(contains_null),
        serde_json::Value::Object(values) => values.values().any(contains_null),
        _ => false,
    }
}

/// 把 Redis 地址和凭据安全编码为 Fred 已有的 Standalone/Sentinel URL 输入。
fn endpoint(scheme: &str, addresses: &[String], auth: &Auth, sentinel: Option<(&str, &Auth)>) -> Result<String> {
    let mut url = Url::parse(&format!("{scheme}://{}", addresses[0])).map_err(|error| Error::field_driver(Code::Invalid, "redis.addresses", error))?;
    if !auth.username.is_empty() {
        url.set_username(&auth.username)
            .map_err(|()| Error::field(Code::Invalid, "redis.auth.username"))?;
    }
    if !auth.password.is_empty() {
        url.set_password(Some(&auth.password))
            .map_err(|()| Error::field(Code::Invalid, "redis.auth.password"))?;
    }
    if let Some((master_name, sentinel_auth)) = sentinel {
        let mut query = url.query_pairs_mut();
        query.append_pair("sentinelServiceName", master_name);
        for address in &addresses[1..] {
            query.append_pair("node", address);
        }
        if !sentinel_auth.username.is_empty() {
            query.append_pair("sentinelUsername", &sentinel_auth.username);
        }
        if !sentinel_auth.password.is_empty() {
            query.append_pair("sentinelPassword", &sentinel_auth.password);
        }
    }
    Ok(url.into())
}

/// 统一验证 host:port、IPv6 方括号和端口范围，避免驱动间接受集合不同。
fn check_addresses(addresses: &[String]) -> Result<()> {
    if addresses.is_empty() {
        return Err(Error::field(Code::Invalid, "redis.addresses"));
    }
    for address in addresses {
        if address.is_empty() || address.trim() != address {
            return Err(Error::field(Code::Invalid, "redis.addresses"));
        }
        let (host, port) = if let Some(rest) = address.strip_prefix('[') {
            let Some((host, port)) = rest.split_once("]:") else {
                return Err(Error::field(Code::Invalid, "redis.addresses"));
            };
            (host, port)
        } else {
            let Some((host, port)) = address.rsplit_once(':') else {
                return Err(Error::field(Code::Invalid, "redis.addresses"));
            };
            if host.contains(':') {
                return Err(Error::field(Code::Invalid, "redis.addresses"));
            }
            (host, port)
        };
        if host.is_empty() || port.parse::<u16>().ok().is_none_or(|port| port == 0) {
            return Err(Error::field(Code::Invalid, "redis.addresses"));
        }
    }
    Ok(())
}

/// 判断 Sentinel 身份是否确实未配置。
fn empty_auth(auth: &Auth) -> bool {
    auth.username.is_empty() && auth.password.is_empty()
}

/// 转换 Registration 的 Redis Zone 初始化策略。
fn registration_policy(source: &RegistrationPolicy) -> Result<RegistrationLimits> {
    Ok(RegistrationLimits {
        attr_max_fields: size(source.attr_max_fields, 16, 1, 128, "registration.policy.attr_max_fields")?,
        data_max_fields: size(source.data_max_fields, 32, 1, 128, "registration.policy.data_max_fields")?,
        field_name_max_bytes: size(source.field_name_max_bytes, 64, 1, 64, "registration.policy.field_name_max_bytes")?,
        attr_value_max_bytes: size(source.attr_value_max_bytes, 128, 1, 16_384, "registration.policy.attr_value_max_bytes")?,
        data_value_max_bytes: size(source.data_value_max_bytes, 128, 1, 16_384, "registration.policy.data_value_max_bytes")?,
        record_max_bytes: size(source.record_max_bytes, 16_384, 1, 65_536, "registration.policy.record_max_bytes")?,
        configuration_refresh: duration(source.refresh_ms, 30_000, 1000, 86_400_000, "registration.policy.refresh_ms")?,
    })
}

/// 把嵌套 JSON Selector 设置写入 Rust 原生 Registration 配置。
fn fill_selector(native: &mut registration::Config, source: &Selector) -> Result<()> {
    native.selector_page_size = size(source.scan_page_size, 256, 1, 1024, "registration.selector.scan_page_size")?;
    native.selector_event_buffer = size(source.max_pending_entries, 4096, 1, 65_536, "registration.selector.max_pending_entries")?;
    native.selector_event_bytes = size(
        source.max_pending_bytes,
        64 * 1024 * 1024,
        1,
        1024 * 1024 * 1024,
        "registration.selector.max_pending_bytes",
    )?;
    native.selector_publish_interval = duration(source.view_publish_interval_ms, 10, 0, 1000, "registration.selector.view_publish_interval_ms")?;
    native.selector_sync_timeout = duration(source.sync_timeout_ms, 30_000, 100, 3_600_000, "registration.selector.sync_timeout_ms")?;
    native.selector_max_bytes = size(
        source.max_active_bytes,
        256 * 1024 * 1024,
        1,
        1024 * 1024 * 1024,
        "registration.selector.max_active_bytes",
    )?;
    native.selector_retained_bytes = Some(size(
        source.max_retained_bytes,
        64 * 1024 * 1024,
        0,
        1024 * 1024 * 1024,
        "registration.selector.max_retained_bytes",
    )?);
    native.clock_refresh = duration(
        source.clock_refresh_interval_ms,
        30_000,
        1000,
        3_600_000,
        "registration.selector.clock_refresh_interval_ms",
    )?;
    native.clock_uncertainty = duration(source.clock_uncertainty_ms, 1, 0, 1000, "registration.selector.clock_uncertainty_ms")?;
    native.selector_error_buffer_capacity = size(source.error_buffer_capacity, 16, 1, 1024, "registration.selector.error_buffer_capacity")?;
    native.selector_recovery_initial_delay = duration(
        source.recovery.initial_delay_ms,
        100,
        10,
        5000,
        "registration.selector.recovery.initial_delay_ms",
    )?;
    native.selector_recovery_max_delay = duration(source.recovery.max_delay_ms, 5000, 100, 30_000, "registration.selector.recovery.max_delay_ms")?;
    native.selector_recovery_multiplier = u8_value(source.recovery.multiplier, 2, 1, 8, "registration.selector.recovery.multiplier")?;
    native.selector_recovery_jitter_percent = u8_value(source.recovery.jitter_percent, 50, 0, 50, "registration.selector.recovery.jitter_percent")?;
    Ok(())
}

/// 转换 Catalog 权威恢复退避。
fn fill_catalog_recovery(native: &mut catalog::Config, source: &Recovery) -> Result<()> {
    native.recovery_initial_delay = duration(source.initial_delay_ms, 250, 10, 5000, "catalog.recovery.initial_delay_ms")?;
    native.recovery_max_delay = duration(source.max_delay_ms, 5000, 100, 30_000, "catalog.recovery.max_delay_ms")?;
    native.recovery_multiplier = u32_value(source.multiplier, 2, 1, 8, "catalog.recovery.multiplier")?;
    native.recovery_jitter_percent = u8_value(source.jitter_percent, 10, 0, 50, "catalog.recovery.jitter_percent")?;
    Ok(())
}

/// 校验一个可选 JSON 整数并返回无符号值；None 使用 fallback。
fn unsigned(value: Option<i64>, fallback: u64, minimum: u64, maximum: u64, field: &'static str) -> Result<u64> {
    let value = match value {
        Some(value) => u64::try_from(value).map_err(|_| Error::field(Code::Invalid, field))?,
        None => fallback,
    };
    if value < minimum || value > maximum {
        return Err(Error::field(Code::Invalid, field));
    }
    Ok(value)
}

/// 把可选 JSON 整数转换为平台 usize，并拒绝平台容量不足。
fn size(value: Option<i64>, fallback: u64, minimum: u64, maximum: u64, field: &'static str) -> Result<usize> {
    usize::try_from(unsigned(value, fallback, minimum, maximum, field)?).map_err(|_| Error::field(Code::Capacity, field))
}

/// 把可选整毫秒转换为 Duration。
fn duration(value: Option<i64>, fallback: u64, minimum: u64, maximum: u64, field: &'static str) -> Result<Duration> {
    Ok(Duration::from_millis(unsigned(value, fallback, minimum, maximum, field)?))
}

/// 把可选 JSON 整数转换为 u32。
fn u32_value(value: Option<i64>, fallback: u64, minimum: u64, maximum: u64, field: &'static str) -> Result<u32> {
    u32::try_from(unsigned(value, fallback, minimum, maximum, field)?).map_err(|_| Error::field(Code::Capacity, field))
}

/// 把可选 JSON 整数转换为 u8。
fn u8_value(value: Option<i64>, fallback: u64, minimum: u64, maximum: u64, field: &'static str) -> Result<u8> {
    u8::try_from(unsigned(value, fallback, minimum, maximum, field)?).map_err(|_| Error::field(Code::Capacity, field))
}
