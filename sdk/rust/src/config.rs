use std::fs::File;
use std::io::{BufReader, Read};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use fred::rustls::client::WebPkiServerVerifier;
use fred::rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use fred::rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use fred::rustls::{ClientConfig as RustlsClientConfig, DigitallySignedStruct, DistinguishedName, Error as RustlsError, RootCertStore, SignatureScheme};
use fred::types::config::{Config as FredConfig, ReconnectPolicy, Server, ServerConfig, TlsConfig as FredTlsConfig, TlsConnector as FredTlsConnector};
use url::Url;

use crate::error::{Code, Error, Result};

const MAXIMUM_TLS_FILE_BYTES: u64 = 1024 * 1024;

/// Redis TLS 的语言原生信任、SNI 和客户端证书文件配置。
#[derive(Clone)]
pub struct TlsConfig {
    /// 是否把操作系统根证书加入信任集合；默认 true，false 时必须提供 `ca_file`。
    pub system_roots: bool,
    /// 可选的固定证书身份；Sentinel TLS 必须配置，且所有 Sentinel 与 Redis 数据节点证书都必须包含该身份。
    /// Standalone 省略时使用连接地址；UTF-8 字节数上限为 253。
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
            if server_name.is_empty()
                || server_name.trim() != server_name
                || server_name.bytes().any(|byte| byte.is_ascii_whitespace())
                || server_name.len() > 253
                || server_name.as_bytes().contains(&0)
                || ServerName::try_from(server_name.clone()).is_err()
            {
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
            if let Some(path) = path {
                let Some(text) = path.to_str() else {
                    return Err(Error::field(Code::Invalid, field));
                };
                if text.is_empty() || text.len() > 4096 || text.as_bytes().contains(&0) {
                    return Err(Error::field(Code::Invalid, field));
                }
            }
        }
        Ok(())
    }

    /// 有界读取根证书和可选客户端证书，构造 Fred 使用的 rustls 连接器。
    ///
    /// Sentinel 动态地址不得改变证书身份；该模式关闭地址派生的 SNI，并由固定校验器始终验证 `server_name`。
    fn fred_config(&self, sentinel: bool) -> Result<FredTlsConfig> {
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

        // 先构造标准 WebPKI 校验器；固定身份包装器只替换服务名，不跳过证书链、有效期或握手签名验证。
        let verifier = self
            .server_name
            .as_ref()
            .map(|name| {
                let name = ServerName::try_from(name.clone()).map_err(|error| Error::field_driver(Code::Invalid, "tls.server_name", error))?;
                let inner = WebPkiServerVerifier::builder(Arc::new(roots.clone()))
                    .build()
                    .map_err(|error| Error::field_driver(Code::Invalid, "tls.ca_file", error))?;
                Ok::<Arc<dyn ServerCertVerifier>, Error>(Arc::new(FixedServerVerifier { inner, name }))
            })
            .transpose()?;

        // rustls 在构造阶段验证客户端证书链、私钥编码以及二者是否匹配。
        let builder = RustlsClientConfig::builder().with_root_certificates(roots);
        let mut native = if let (Some(cert_path), Some(key_path)) = (&self.cert_file, &self.key_file) {
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
        if let Some(verifier) = verifier {
            native.dangerous().set_certificate_verifier(verifier);
        }
        if sentinel {
            // Fred 10.1 不能把固定 SNI 传播给 Sentinel 新发现的节点。Redis/Sentinel 不做 TLS 虚拟主机路由，
            // 因此禁用动态地址 SNI，并继续用上面的固定身份完成严格证书校验。
            native.enable_sni = false;
        }
        Ok(FredTlsConfig::from(FredTlsConnector::from(native)))
    }
}

/// 始终以部署配置的固定身份调用 rustls 标准校验器，避免 Sentinel 返回的地址成为信任输入。
#[derive(Debug)]
struct FixedServerVerifier {
    inner: Arc<dyn ServerCertVerifier>,
    name: ServerName<'static>,
}

impl ServerCertVerifier for FixedServerVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        ocsp_response: &[u8],
        now: UnixTime,
    ) -> std::result::Result<ServerCertVerified, RustlsError> {
        self.inner.verify_server_cert(end_entity, intermediates, &self.name, ocsp_response, now)
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        certificate: &CertificateDer<'_>,
        signature: &DigitallySignedStruct,
    ) -> std::result::Result<HandshakeSignatureValid, RustlsError> {
        self.inner.verify_tls12_signature(message, certificate, signature)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        certificate: &CertificateDer<'_>,
        signature: &DigitallySignedStruct,
    ) -> std::result::Result<HandshakeSignatureValid, RustlsError> {
        self.inner.verify_tls13_signature(message, certificate, signature)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.inner.supported_verify_schemes()
    }

    fn requires_raw_public_keys(&self) -> bool {
        self.inner.requires_raw_public_keys()
    }

    fn root_hint_subjects(&self) -> Option<&[DistinguishedName]> {
        self.inner.root_hint_subjects()
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
    /// 每次驱动重新建立连接前的固定等待；默认 100 毫秒，允许范围为 10 毫秒至 30 秒且必须为整毫秒。
    pub delay: Duration,
}

impl Default for ReconnectConfig {
    /// 返回固定 100 毫秒等待的稳定重连默认值。
    fn default() -> Self {
        Self {
            delay: Duration::from_millis(100),
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
    /// 驱动连接恢复等待；默认固定 100 毫秒，范围见 `ReconnectConfig`。
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
        // 端点检查：直接使用 Fred 的正式 URL 解析器，提前拒绝语法错误和明确不支持的 Cluster。
        let native = fred_config_from_url(&self.endpoint)?;
        if matches!(native.server, ServerConfig::Clustered { .. }) {
            return Err(Error::field(Code::Invalid, "topology"));
        }

        // 自定义 TLS 只做结构及拓扑校验；证书文件在建立 Redis Client 前才有界读取。
        if let Some(tls) = &self.tls {
            tls.validate()?;
            if matches!(native.server, ServerConfig::Sentinel { .. }) && tls.server_name.is_none() {
                return Err(Error::field(Code::Invalid, "tls.server_name"));
            }
        } else if matches!(native.server, ServerConfig::Sentinel { .. }) && native.uses_tls() {
            // rediss-sentinel URL 自带 TLS，但不能表达 Verdandi 要求的固定集群证书身份。
            return Err(Error::field(Code::Invalid, "tls.server_name"));
        }

        // 命令超时检查：必须是 10 毫秒至 15 秒内的整毫秒值。
        validate_duration(self.timeout, Duration::from_millis(10), Duration::from_secs(15), "timeout")?;

        // 建连超时检查：必须是 20 毫秒至 30 秒内的整毫秒值。
        validate_duration(self.connect_timeout, Duration::from_millis(20), Duration::from_secs(30), "connect_timeout")?;

        // 空闲回收检查：必须是 1 秒至 1 小时内的整毫秒值。
        validate_duration(self.pool.idle_timeout, Duration::from_secs(1), Duration::from_secs(3600), "pool.idle_timeout")?;

        // 驱动重连检查：必须是 10 毫秒至 30 秒内的整毫秒值。
        validate_duration(self.reconnect.delay, Duration::from_millis(10), Duration::from_secs(30), "reconnect.delay")?;

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

        Ok(())
    }

    /// 把公开固定等待转换为 Fred 的无限连接恢复策略。
    pub(crate) fn reconnect_policy(&self) -> Result<ReconnectPolicy> {
        // Fred 接受 u32 整毫秒；这里再次无损转换，防止未经 validate 的内部调用绕过边界。
        let delay = exact_milliseconds(self.reconnect.delay)
            .and_then(|value| u32::try_from(value).ok())
            .ok_or_else(|| Error::field(Code::Invalid, "reconnect.delay"))?;
        let mut policy = ReconnectPolicy::new_constant(0, delay);
        // Fred 的默认策略会额外增加 50ms 抖动；显式清零才能与 Go/C++ 固定等待契约一致。
        policy.set_jitter(0);
        Ok(policy)
    }

    /// 解析 Fred 拓扑、拒绝 Cluster，并应用数据库和可选自定义 rustls 连接器。
    pub(crate) fn fred_config(&self) -> Result<FredConfig> {
        let mut native = fred_config_from_url(&self.endpoint)?;
        if matches!(native.server, ServerConfig::Clustered { .. }) {
            return Err(Error::field(Code::Invalid, "topology"));
        }
        native.database = Some(self.database);

        if let Some(tls) = &self.tls {
            let sentinel = matches!(native.server, ServerConfig::Sentinel { .. });
            if let Some(server_name) = &tls.server_name {
                match &mut native.server {
                    ServerConfig::Centralized { server } => {
                        server.tls_server_name = Some(server_name.clone().into());
                    }
                    ServerConfig::Sentinel { hosts, .. } => {
                        for server in hosts {
                            server.tls_server_name = Some(server_name.clone().into());
                        }
                    }
                    _ => return Err(Error::field(Code::Invalid, "topology")),
                }
            }
            native.tls = Some(tls.fred_config(sentinel)?);
        }
        Ok(native)
    }
}

/// 使用 Fred 解析 URL，同时修复其 `node=` 参数不能表示方括号 IPv6 Sentinel 的限制。
fn fred_config_from_url(endpoint: &str) -> Result<FredConfig> {
    // endpoint 可以包含 ACL 密码；解析失败只暴露稳定字段，不把第三方错误中可能携带的原始 URL 写入诊断。
    let mut url = Url::parse(endpoint).map_err(|_| Error::field(Code::Invalid, "endpoint"))?;
    let pairs = url
        .query_pairs()
        .map(|(name, value)| (name.into_owned(), value.into_owned()))
        .collect::<Vec<_>>();
    let sentinel = url.scheme().ends_with("-sentinel") || pairs.iter().any(|(name, _)| name == "sentinelServiceName");
    if url.scheme().ends_with("-cluster") || (!sentinel && pairs.iter().any(|(name, _)| name == "node")) {
        return Err(Error::field(Code::Invalid, "topology"));
    }
    if !sentinel || !pairs.iter().any(|(name, _)| name == "node") {
        return FredConfig::from_url(endpoint).map_err(|_| Error::field(Code::Invalid, "endpoint"));
    }

    // 先让 Fred 解析凭据、数据库、TLS scheme 和 Sentinel 身份，再把已严格解析的附加节点补回。
    url.query_pairs_mut()
        .clear()
        .extend_pairs(pairs.iter().filter(|(name, _)| name != "node").map(|(name, value)| (name, value)));
    let mut config = FredConfig::from_url(url.as_str()).map_err(|_| Error::field(Code::Invalid, "endpoint"))?;
    let ServerConfig::Sentinel { hosts, .. } = &mut config.server else {
        return Err(Error::field(Code::Invalid, "topology"));
    };
    for (_, address) in pairs.iter().filter(|(name, _)| name == "node") {
        let (host, port) = split_endpoint(address).ok_or_else(|| Error::field(Code::Invalid, "endpoint"))?;
        hosts.push(Server::new(host, port));
    }
    Ok(config)
}

/// 解析域名、IPv4 或方括号 IPv6 的 host:port，供 Fred Sentinel 节点适配使用。
fn split_endpoint(address: &str) -> Option<(String, u16)> {
    let (host, port) = if let Some(rest) = address.strip_prefix('[') {
        let (host, port) = rest.split_once("]:")?;
        (host, port)
    } else {
        let (host, port) = address.rsplit_once(':')?;
        if host.contains(':') {
            return None;
        }
        (host, port)
    };
    let port = port.parse::<u16>().ok()?;
    (!host.is_empty() && port != 0).then(|| (host.to_owned(), port))
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
