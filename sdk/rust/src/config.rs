use std::fs::File;
use std::io::{BufReader, Read};
use std::path::{Path, PathBuf};
use std::time::Duration;

use fred::rustls::{ClientConfig as RustlsClientConfig, RootCertStore};
use fred::types::config::{Config as FredConfig, ReconnectPolicy, ServerConfig, TlsConfig as FredTlsConfig, TlsConnector as FredTlsConnector};

use crate::error::{Code, Error, Result};

const MAXIMUM_TLS_FILE_BYTES: u64 = 1024 * 1024;

/// Redis TLS 的语言原生信任、SNI 和客户端证书文件配置。
#[derive(Clone)]
pub struct TlsConfig {
    /// 是否把操作系统根证书加入信任集合；默认 true，false 时必须提供 `ca_file`。
    pub system_roots: bool,
    /// 可选的固定证书校验及握手服务名；仅 Standalone 拓扑支持，UTF-8 字节数上限为 253。
    pub server_name: Option<String>,
    /// 可选的 PEM CA bundle 路径；证书追加到所选根集合，单个文件读取上限为 1 MiB。
    pub ca_file: Option<PathBuf>,
    /// 可选的 PEM 客户端证书链路径；必须与 `key_file` 成对，单个文件读取上限为 1 MiB。
    pub cert_file: Option<PathBuf>,
    /// 可选的未加密 PEM 客户端私钥路径；必须与 `cert_file` 成对，单个文件读取上限为 1 MiB。
    pub key_file: Option<PathBuf>,
}

impl Default for TlsConfig {
    /// 返回只使用操作系统根、连接地址服务名且不发送客户端证书的 TLS 配置。
    fn default() -> Self {
        Self {
            system_roots: true,
            server_name: None,
            ca_file: None,
            cert_file: None,
            key_file: None,
        }
    }
}

impl TlsConfig {
    /// 在不读取证书文件的情况下校验信任源、服务名和客户端证书文件关系。
    fn validate(&self) -> Result<()> {
        // 关闭系统根时必须提供私有 CA，确保 TLS 不会得到一个意外的空信任集合。
        if !self.system_roots && self.ca_file.is_none() {
            return Err(Error::field(Code::Invalid, "tls.ca_file"));
        }

        // 固定服务名不能带首尾空白、NUL 或超过 DNS 名的 253 字节上限。
        if let Some(server_name) = &self.server_name {
            if server_name.is_empty() || server_name.trim() != server_name || server_name.len() > 253 || server_name.as_bytes().contains(&0) {
                return Err(Error::field(Code::Invalid, "tls.server_name"));
            }
        }

        // 客户端证书链和私钥只能同时缺失或同时存在，所有存在的路径必须非空。
        if self.cert_file.is_some() != self.key_file.is_some() {
            return Err(Error::field(Code::Invalid, "tls.cert_file"));
        }
        for (field, path) in [
            ("tls.ca_file", self.ca_file.as_deref()),
            ("tls.cert_file", self.cert_file.as_deref()),
            ("tls.key_file", self.key_file.as_deref()),
        ] {
            if path.is_some_and(|path| path.as_os_str().is_empty()) {
                return Err(Error::field(Code::Invalid, field));
            }
        }
        Ok(())
    }

    /// 有界读取根证书和可选客户端证书，构造 Fred 使用的 rustls 连接器。
    fn fred_config(&self) -> Result<FredTlsConfig> {
        let mut roots = RootCertStore::empty();

        // 系统根由 Fred 已选定的 rustls-native-certs 实现加载；任何平台加载错误都显式返回。
        if self.system_roots {
            let native = fred::rustls_native_certs::load_native_certs();
            if let Some(error) = native.errors.into_iter().next() {
                return Err(Error::field_driver(Code::Unavailable, "tls.system_roots", error));
            }
            for certificate in native.certs {
                roots
                    .add(certificate)
                    .map_err(|error| Error::field_driver(Code::Invalid, "tls.system_roots", error))?;
            }
        }

        // 一个 CA bundle 可以包含多张 PEM 证书；空 bundle 或任何无效根证书都拒绝。
        if let Some(path) = &self.ca_file {
            let content = read_tls_file(path, "tls.ca_file")?;
            let mut reader = BufReader::new(content.as_slice());
            let certificates = rustls_pemfile::certs(&mut reader)
                .collect::<std::result::Result<Vec<_>, _>>()
                .map_err(|error| Error::field_driver(Code::Invalid, "tls.ca_file", error))?;
            if certificates.is_empty() {
                return Err(Error::field(Code::Invalid, "tls.ca_file"));
            }
            for certificate in certificates {
                roots
                    .add(certificate)
                    .map_err(|error| Error::field_driver(Code::Invalid, "tls.ca_file", error))?;
            }
        }

        // rustls 在构造阶段验证客户端证书链、私钥编码以及二者是否匹配。
        let builder = RustlsClientConfig::builder().with_root_certificates(roots);
        let native = if let (Some(cert_path), Some(key_path)) = (&self.cert_file, &self.key_file) {
            let certificate_content = read_tls_file(cert_path, "tls.cert_file")?;
            let mut certificate_reader = BufReader::new(certificate_content.as_slice());
            let certificates = rustls_pemfile::certs(&mut certificate_reader)
                .collect::<std::result::Result<Vec<_>, _>>()
                .map_err(|error| Error::field_driver(Code::Invalid, "tls.cert_file", error))?;
            if certificates.is_empty() {
                return Err(Error::field(Code::Invalid, "tls.cert_file"));
            }
            let key_content = read_tls_file(key_path, "tls.key_file")?;
            let mut key_reader = BufReader::new(key_content.as_slice());
            let key = rustls_pemfile::private_key(&mut key_reader)
                .map_err(|error| Error::field_driver(Code::Invalid, "tls.key_file", error))?
                .ok_or_else(|| Error::field(Code::Invalid, "tls.key_file"))?;
            builder
                .with_client_auth_cert(certificates, key)
                .map_err(|error| Error::field_driver(Code::Invalid, "tls.cert_file", error))?
        } else {
            builder.with_no_client_auth()
        };
        Ok(FredTlsConfig::from(FredTlsConnector::from(native)))
    }
}

/// 根 Redis 连接池的容量与空闲回收配置。
#[derive(Clone)]
pub struct PoolConfig {
    /// 客户端启动时建立并始终保留的最少连接数；默认 1，允许范围为 1 至 1024。
    pub min_connections: usize,
    /// 并发命令压力下允许扩展到的最多连接数；默认 4，允许范围为 1 至 1024，且不得小于 `min_connections`。
    pub max_connections: usize,
    /// 超出最少数量的连接在没有命令后允许保留的时长；默认 10 秒，允许范围为 1 秒至 1 小时且必须为整毫秒。
    pub idle_timeout: Duration,
}

impl Default for PoolConfig {
    /// 返回最少 1 条、最多 4 条连接和 10 秒空闲回收时间的稳定默认值。
    fn default() -> Self {
        Self {
            min_connections: 1,
            max_connections: 4,
            idle_timeout: Duration::from_secs(10),
        }
    }
}

/// Redis 连接中断后的驱动级恢复退避配置。
///
/// 该策略只恢复连接，不会自动重放已经发送或结果不明确的业务命令。
#[derive(Clone)]
pub struct ReconnectConfig {
    /// 首次连续连接失败后的基础等待；默认 100 毫秒，允许范围为 10 毫秒至 5 秒且必须为整毫秒。
    pub initial_delay: Duration,
    /// 连续失败时的退避上限；默认 5 秒，允许范围为 100 毫秒至 30 秒且必须为整毫秒。
    pub max_delay: Duration,
    /// 每轮连续失败后的指数增长倍数；默认 2，允许范围为 1 至 8。
    pub multiplier: u32,
    /// 随机延迟上限百分比；默认 10，允许范围为 0 至 50，零明确禁用抖动。
    pub jitter_percent: u8,
}

impl Default for ReconnectConfig {
    /// 返回 100 毫秒起步、5 秒封顶、倍数 2 和 10% 抖动的稳定重连默认值。
    fn default() -> Self {
        Self {
            initial_delay: Duration::from_millis(100),
            max_delay: Duration::from_secs(5),
            multiplier: 2,
            jitter_percent: 10,
        }
    }
}

/// 供独立 Verdandi 领域客户端共享的 Redis 连接配置。
#[derive(Clone)]
pub struct Config {
    /// Redis 或 Redis Sentinel URL；无默认值，去除首尾空白后必须非空；凭据不得写入 Debug 或诊断输出。
    pub endpoint: String,
    /// Redis 逻辑数据库编号；默认 0，`u8` 类型将范围固定为 0 至 255，并覆盖 URL 路径中的数据库值。
    pub database: u8,
    /// 可选的语言原生 TLS 配置；默认 None 并沿用 endpoint scheme，Some 时启用或覆盖 Fred TLS 连接器。
    pub tls: Option<TlsConfig>,
    /// 一条普通 Redis 操作允许等待的最长时间；默认 2 秒，允许范围为 10 毫秒至 15 秒且必须为整毫秒。
    pub timeout: Duration,
    /// 一次 TCP 连接和 TLS 握手允许等待的最长时间；默认 5 秒，允许范围为 20 毫秒至 30 秒且必须为整毫秒。
    pub connect_timeout: Duration,
    /// 共享连接池容量和空闲回收策略；默认最少 1 条、最多 4 条连接和 10 秒空闲回收时间，范围见 `PoolConfig`。
    pub pool: PoolConfig,
    /// 连接级恢复退避策略；默认 100 毫秒起步、5 秒封顶、倍数 2 和 10% 抖动，范围见 `ReconnectConfig`。
    pub reconnect: ReconnectConfig,
}

impl Config {
    /// 用 `endpoint` 创建全部字段都采用共享默认值的传输配置。
    ///
    /// 该构造不解析 URL 或建立连接；`Client::open` 会统一校验所有公开字段。
    pub fn new(endpoint: impl Into<String>) -> Self {
        Self {
            endpoint: endpoint.into(),
            database: 0,
            tls: None,
            timeout: Duration::from_secs(2),
            connect_timeout: Duration::from_secs(5),
            pool: PoolConfig::default(),
            reconnect: ReconnectConfig::default(),
        }
    }

    /// 在不建立连接的情况下校验 Redis 端点、超时、连接池和重连配置。
    pub fn check(&self) -> Result<()> {
        self.validate()
    }

    /// 校验端点、数据库、超时、连接池以及重连退避的全部单值和关系约束。
    pub(crate) fn validate(&self) -> Result<()> {
        // 端点检查：Standalone 或 Sentinel URL 去除首尾空白后仍必须非空。
        if self.endpoint.trim().is_empty() {
            return Err(Error::field(Code::Invalid, "endpoint"));
        }

        // 自定义 TLS 只做结构校验；证书文件在建立 Redis Client 前才有界读取。
        if let Some(tls) = &self.tls {
            tls.validate()?;
        }

        // 命令超时检查：必须是 10 毫秒至 15 秒内的整毫秒值。
        validate_duration(self.timeout, Duration::from_millis(10), Duration::from_secs(15), "timeout")?;

        // 建连超时检查：必须是 20 毫秒至 30 秒内的整毫秒值。
        validate_duration(self.connect_timeout, Duration::from_millis(20), Duration::from_secs(30), "connect_timeout")?;

        // 空闲回收检查：必须是 1 秒至 1 小时内的整毫秒值。
        validate_duration(self.pool.idle_timeout, Duration::from_secs(1), Duration::from_secs(3600), "pool.idle_timeout")?;

        // 首次重连延迟检查：必须是 10 毫秒至 5 秒内的整毫秒值。
        validate_duration(
            self.reconnect.initial_delay,
            Duration::from_millis(10),
            Duration::from_secs(5),
            "reconnect.initial_delay",
        )?;

        // 最大重连延迟检查：必须是 100 毫秒至 30 秒内的整毫秒值。
        validate_duration(
            self.reconnect.max_delay,
            Duration::from_millis(100),
            Duration::from_secs(30),
            "reconnect.max_delay",
        )?;

        // 连接池关系检查：分别定位最少/最多连接数越界；关系失败归因于不能被满足的最小值。
        if !(1..=1024).contains(&self.pool.min_connections) {
            return Err(Error::field(Code::Invalid, "pool.min_connections"));
        }
        if !(1..=1024).contains(&self.pool.max_connections) {
            return Err(Error::field(Code::Invalid, "pool.max_connections"));
        }
        if self.pool.min_connections > self.pool.max_connections {
            return Err(Error::field(Code::Invalid, "pool.min_connections"));
        }

        // 重连关系检查：指数倍数为 1 至 8、抖动为 0% 至 50%，首次延迟不得超过最大延迟。
        if !(1..=8).contains(&self.reconnect.multiplier) {
            return Err(Error::field(Code::Invalid, "reconnect.multiplier"));
        }
        if self.reconnect.jitter_percent > 50 {
            return Err(Error::field(Code::Invalid, "reconnect.jitter_percent"));
        }
        if self.reconnect.initial_delay > self.reconnect.max_delay {
            return Err(Error::field(Code::Invalid, "reconnect.initial_delay"));
        }
        Ok(())
    }

    /// 把公开退避配置转换为 Fred 的无限连接恢复策略。
    ///
    /// Fred 使用固定毫秒抖动，因此选择“初始延迟 × 百分比”作为绝对上限；随着退避增长，
    /// 实际相对抖动只会更小，绝不会超过调用方配置的百分比。
    pub(crate) fn reconnect_policy(&self) -> Result<ReconnectPolicy> {
        // Fred 接受 u32 整毫秒；这里再次无损转换，防止未经 validate 的内部调用绕过边界。
        let minimum = exact_milliseconds(self.reconnect.initial_delay)
            .and_then(|value| u32::try_from(value).ok())
            .ok_or_else(|| Error::field(Code::Invalid, "reconnect.initial_delay"))?;
        let maximum = exact_milliseconds(self.reconnect.max_delay)
            .and_then(|value| u32::try_from(value).ok())
            .ok_or_else(|| Error::field(Code::Invalid, "reconnect.max_delay"))?;
        // Fred 使用固定毫秒抖动，因此按首次延迟的 0%..50% 计算绝对抖动上限。
        let jitter = u64::from(minimum).saturating_mul(u64::from(self.reconnect.jitter_percent)) / 100;
        let mut policy = ReconnectPolicy::new_exponential(0, minimum, maximum, self.reconnect.multiplier);
        policy.set_jitter(u32::try_from(jitter).map_err(|_| Error::field(Code::Invalid, "reconnect.jitter_percent"))?);
        Ok(policy)
    }

    /// 解析 Fred 拓扑、拒绝 Cluster，并应用数据库和可选自定义 rustls 连接器。
    pub(crate) fn fred_config(&self) -> Result<FredConfig> {
        let mut native = FredConfig::from_url(&self.endpoint).map_err(|error| Error::driver(Code::Invalid, error))?;
        if matches!(native.server, ServerConfig::Clustered { .. }) {
            return Err(Error::field(Code::Invalid, "topology"));
        }
        native.database = Some(self.database);

        if let Some(tls) = &self.tls {
            // Fred 的 Sentinel 主节点是在运行期重新构造的，无法继承固定服务名；因此只对 Standalone 开放覆盖。
            if let Some(server_name) = &tls.server_name {
                match &mut native.server {
                    ServerConfig::Centralized { server } => {
                        server.tls_server_name = Some(server_name.clone().into());
                    }
                    _ => return Err(Error::field(Code::Invalid, "tls.server_name")),
                }
            }
            native.tls = Some(tls.fred_config()?);
        }
        Ok(native)
    }
}

/// 最多读取 1 MiB 加一个探测字节，防止错误路径把任意大证书文件载入内存。
fn read_tls_file(path: &Path, field: &'static str) -> Result<Vec<u8>> {
    let file = File::open(path).map_err(|error| Error::field_driver(Code::Unavailable, field, error))?;
    let mut content = Vec::new();
    file.take(MAXIMUM_TLS_FILE_BYTES + 1)
        .read_to_end(&mut content)
        .map_err(|error| Error::field_driver(Code::Unavailable, field, error))?;
    if u64::try_from(content.len()).unwrap_or(u64::MAX) > MAXIMUM_TLS_FILE_BYTES {
        return Err(Error::field(Code::Capacity, field));
    }
    Ok(content)
}

/// 要求 `value` 是指定闭区间内的整毫秒 Duration。
fn validate_duration(value: Duration, minimum: Duration, maximum: Duration, field: &'static str) -> Result<()> {
    if exact_milliseconds(value).is_none() || value < minimum || value > maximum {
        return Err(Error::field(Code::Invalid, field));
    }
    Ok(())
}

/// 把 Duration 转为不丢失精度的 `u64` 整毫秒；亚毫秒和溢出返回 `None`。
fn exact_milliseconds(value: Duration) -> Option<u64> {
    if value.subsec_nanos() % 1_000_000 != 0 {
        return None;
    }
    u64::try_from(value.as_millis()).ok()
}

#[cfg(test)]
#[path = "../tests/internal/config.rs"]
mod tests;
