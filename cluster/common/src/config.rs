//! Peer 静态配置, 校验不执行网络 I/O 或修改全局环境.
use std::{io, net::SocketAddr, path::PathBuf, time::Duration};

/// 已校验的 Supervisor 入口, DNS 只在有总期限的连接阶段解析.
#[derive(Clone, Debug)]
pub struct SupervisorAddress {
    /// 已校验的主机名或数值 IP.
    pub host: String,
    /// 非零目标端口.
    pub port: u16,
}
impl SupervisorAddress {
    /// 解析入口只校验文本, 不执行 DNS; 连接阶段另有总期限.
    pub fn parse(value: &str) -> io::Result<Self> {
        if let Ok(address) = value.parse::<SocketAddr>() {
            validate_remote_address("supervisor", address)?;
            return Ok(Self {
                host: address.ip().to_string(),
                port: address.port(),
            });
        }
        let (host, port) = value.rsplit_once(':').ok_or_else(|| invalid("supervisor requires HOST:PORT"))?;
        let port = port
            .parse::<u16>()
            .ok()
            .filter(|port| *port != 0)
            .ok_or_else(|| invalid("invalid supervisor port"))?;
        if host.is_empty()
            || host.len() > 253
            || host.split('.').any(|part| {
                part.is_empty()
                    || part.len() > 63
                    || part.starts_with('-')
                    || part.ends_with('-')
                    || !part.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
            })
        {
            return Err(invalid("invalid supervisor host"));
        }
        Ok(Self { host: host.to_owned(), port })
    }
}

/// Peer 的部署配置. 进程 UUID 在 start 时生成, 不从配置或磁盘恢复.
#[derive(Clone, Debug)]
pub struct PeerConfig {
    /// 进程角色, 默认 Star; Planet 入口强制使用 Planet, 仍须通过账号授权.
    pub role: crate::protocol::NodeRole,
    /// 连接偏好组, 默认 default, 1..64 个安全 ASCII 字节; 不改变数据 scope.
    pub group: String,
    /// 必填群组, 1..64 个安全 ASCII 字节, 大小写敏感.
    pub cluster_id: String,
    /// 必填本地监听地址, 测试允许端口零, 通配监听需显式 advertise.
    pub listen: SocketAddr,
    /// 默认 None, 使用实际绑定地址; 显式值必须为具体单播 IP 和非零端口.
    pub advertise: Option<SocketAddr>,
    /// 必填 Supervisor HOST:PORT, 不存在绕过登记的 seed 模式.
    pub supervisor: String,
    /// 必填身份目录, 包含 ca.pem, cert.pem, key.pem, admission.pub 和 login.json.
    pub identity: PathBuf,
    /// 每群组节点上限, 默认 64, 范围 1..4096, 包含自身.
    pub max_peers: usize,
    /// TCP/DNS 单次连接期限, 默认 3 秒, 范围 1 ms..24 h.
    pub connect_timeout: Duration,
    /// TLS, gRPC 和 Hello 或登记的总期限, 默认 5 秒, 范围 1 ms..24 h.
    pub handshake_timeout: Duration,
    /// 心跳间隔默认 10 秒, 范围 1 ms..24 h.
    pub heartbeat_interval: Duration,
    /// Ping 写入至对应 Pong 的总期限默认 5 秒, 范围 1 ms..24 h.
    pub pong_timeout: Duration,
    /// 退避下限默认 100 ms, 范围 1 ms..24 h, 不超过上限.
    pub reconnect_min: Duration,
    /// 退避上限默认 5 秒, 范围 1 ms..24 h, 包含抖动.
    pub reconnect_max: Duration,
    /// 稳定连接窗口默认 5 秒, 范围 1 ms..24 h, 达到后重置连续失败次数.
    pub stable_connection: Duration,
    /// 入站连接预算默认 128, 范围 1..65536, with_max_peers 调整为节点上限的两倍.
    pub max_inbound_connections: usize,
    /// 同时拨号数默认 4, 范围 1..4096, 与全节点 250 ms 拨号节拍共同限流.
    pub max_concurrent_dials: usize,
    /// 声明的普通帧上限默认 1 MiB, 当前控制消息另有阶段上限.
    pub max_frame_bytes: u32,
    /// 有损诊断环默认 256, 范围 1..65536, 不参与成员状态正确性.
    pub event_capacity: usize,
}
impl PeerConfig {
    /// 创建部署配置. 所有字段在打开 socket 前重新验证, 缺失身份材料不会降级为明文.
    #[must_use]
    pub fn new(cluster_id: impl Into<String>, listen: SocketAddr, supervisor: impl Into<String>, identity: impl Into<PathBuf>) -> Self {
        Self {
            role: crate::protocol::NodeRole::Star,
            group: "default".into(),
            cluster_id: cluster_id.into(),
            listen,
            advertise: None,
            supervisor: supervisor.into(),
            identity: identity.into(),
            max_peers: 64,
            connect_timeout: Duration::from_secs(3),
            handshake_timeout: Duration::from_secs(5),
            heartbeat_interval: Duration::from_secs(10),
            pong_timeout: Duration::from_secs(5),
            reconnect_min: Duration::from_millis(100),
            reconnect_max: Duration::from_secs(5),
            stable_connection: Duration::from_secs(5),
            max_inbound_connections: 128,
            max_concurrent_dials: 4,
            max_frame_bytes: 1024 * 1024,
            event_capacity: 256,
        }
    }
    /// 设置连接偏好组, 默认 default, 使用与群组相同的名称约束.
    #[must_use]
    pub fn with_group(mut self, group: impl Into<String>) -> Self {
        self.group = group.into();
        self
    }
    /// 设置具体可达地址. 默认使用 listener 的实际地址, 不接受通配或端口零.
    #[must_use]
    pub fn with_advertise(mut self, address: SocketAddr) -> Self {
        self.advertise = Some(address);
        self
    }
    /// 设置部署容量, 默认 64, 范围 1..4096, 包含自身, 同时调整入站预算.
    #[must_use]
    pub fn with_max_peers(mut self, maximum: usize) -> Self {
        self.max_peers = maximum;
        self.max_inbound_connections = maximum.saturating_mul(2);
        self
    }
    /// 设置本端心跳, 默认 10 秒间隔和 5 秒响应期限, 两者范围 1 ms..24 h.
    #[must_use]
    pub fn with_heartbeat(mut self, interval: Duration, timeout: Duration) -> Self {
        self.heartbeat_interval = interval;
        self.pong_timeout = timeout;
        self
    }
    /// 验证静态配置, 不读取身份文件, 不创建后台任务.
    pub fn validate(&self) -> io::Result<()> {
        validate_name("cluster_id", &self.cluster_id)?;
        validate_name("group", &self.group)?;
        if !matches!(self.role, crate::protocol::NodeRole::Star | crate::protocol::NodeRole::Planet) {
            return Err(invalid("explicit node role is required"));
        }
        SupervisorAddress::parse(&self.supervisor)?;
        if self.identity.as_os_str().is_empty() {
            return Err(invalid("identity directory is required"));
        }
        if !(1..=4096).contains(&self.max_peers) {
            return Err(invalid("max_peers must be between 1 and 4096"));
        }
        if self.listen.ip().is_unspecified() && self.advertise.is_none() {
            return Err(invalid("advertise is required for an unspecified listen address"));
        }
        if self.listen.ip().is_multicast() {
            return Err(invalid("multicast listen address is not supported"));
        }
        if let Some(address) = self.advertise {
            validate_remote_address("advertise", address)?;
        }
        // 所有进入 Instant 加法的外部时间都有统一上限, 极端 Duration 不能触发计时器 panic.
        let bounds = Duration::from_millis(1)..=Duration::from_secs(24 * 60 * 60);
        for (field, duration) in [
            ("connect_timeout", self.connect_timeout),
            ("handshake_timeout", self.handshake_timeout),
            ("heartbeat_interval", self.heartbeat_interval),
            ("pong_timeout", self.pong_timeout),
            ("reconnect_min", self.reconnect_min),
            ("reconnect_max", self.reconnect_max),
            ("stable_connection", self.stable_connection),
        ] {
            if !bounds.contains(&duration) {
                return Err(invalid(&format!("{field} must be between 1 ms and 24 hours")));
            }
        }
        if self.reconnect_min > self.reconnect_max {
            return Err(invalid("reconnect_min exceeds reconnect_max"));
        }
        // 同时限制实际资源工作量及 Tokio channel/semaphore 的构造参数, 不能只检查非零.
        if !(1..=65536).contains(&self.max_inbound_connections)
            || !(1..=4096).contains(&self.max_concurrent_dials)
            || !(1..=65536).contains(&self.event_capacity)
            || self.max_frame_bytes < crate::protocol::MIN_FRAME_BYTES
        {
            return Err(invalid("invalid resource bounds"));
        }
        Ok(())
    }
}
/// 群组名统一使用安全 ASCII, 避免两种语言对 Unicode 或空白归一化产生不同解释.
pub fn validate_name(field: &str, value: &str) -> io::Result<()> {
    if value.is_empty() || value.len() > 64 || !value.bytes().all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-')) {
        return Err(invalid(&format!("invalid {field}")));
    }
    Ok(())
}
/// 远端地址必须可直接拨号, 不接受多播, 映射别名或只在本机有效的 IPv6 scope.
pub fn validate_remote_address(field: &str, address: SocketAddr) -> io::Result<()> {
    let invalid_v6 = matches!(address, SocketAddr::V6(v6) if v6.scope_id() != 0 || v6.flowinfo() != 0 || v6.ip().to_ipv4_mapped().is_some());
    if address.ip().is_unspecified() || address.ip().is_multicast() || address.port() == 0 || invalid_v6 {
        return Err(invalid(&format!("invalid {field} address")));
    }
    Ok(())
}
/// 统一静态配置错误类别, 在 socket 和任务创建之前返回.
fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

#[cfg(test)]
#[path = "../tests/unit/config.rs"]
mod tests;
