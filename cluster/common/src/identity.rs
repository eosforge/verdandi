//! TLS 服务端身份, Supervisor 账号和签名凭证. 不持有额外进程签名密钥.
use crate::{
    config::{validate_name, validate_remote_address},
    protocol::{Hello, Member, ProtocolErrorCode, SessionError},
};
use prost::{Message, bytes::Bytes};
use ring::{
    digest,
    rand::{SecureRandom, SystemRandom},
    signature,
};
use rustls::{ClientConfig, RootCertStore, ServerConfig, pki_types::ServerName};
use std::{
    fs::File,
    io::{self, Read},
    path::Path,
    sync::Arc,
};
use tokio::net::TcpStream;
use tokio_rustls::{TlsAcceptor, TlsConnector, TlsStream};

/// 部署材料只读共享. Debug 不输出证书, 私钥或签名材料.
pub struct Identity {
    /// 共享客户端配置, 所有连接重新进行证书检查.
    pub client: Arc<ClientConfig>,
    /// 共享服务端配置, 应用身份由签名凭证校验.
    server: Arc<ServerConfig>,
    /// gRPC 复用同一服务端身份与信任根, 显式协商 h2.
    rpc_client: Arc<ClientConfig>,
    rpc_server: Arc<ServerConfig>,
    /// 账号材料不实现 Debug, 仅用于 Supervisor 登录.
    username: String,
    password: String,
    /// 只持有 Supervisor 准入公钥, 不持有其签名私钥.
    authority: [u8; 32],
    /// 启动时核对本机服务端证书, 避免登记一个其他节点无法验证的端点.
    certificates: Vec<rustls::pki_types::CertificateDer<'static>>,
    verifier: Arc<rustls::client::WebPkiServerVerifier>,
}
/// 本进程新生成的 UUID. 不落盘, 重试和重连复用, 不把登录账号作为节点 ID.
pub struct ProcessIdentity {
    /// 本进程内固定的规范 UUIDv4.
    pub id: String,
}
impl ProcessIdentity {
    /// 系统随机源失败即停止启动, 不使用时间戳或固定值降级.
    pub fn new() -> io::Result<Self> {
        let random = SystemRandom::new();
        let mut uuid = [0_u8; 16];
        random.fill(&mut uuid).map_err(|_| failure("system randomness unavailable"))?;
        // UUIDv4 的版本与 variant 位固定, 其余位保留随机性.
        uuid[6] = (uuid[6] & 15) | 0x40;
        uuid[8] = (uuid[8] & 63) | 0x80;
        Ok(Self { id: hex(&uuid) })
    }
}
impl Identity {
    /// 有界读取 TLS 材料、登录配置和准入公钥, 配置错误在联网前失败.
    pub fn load(directory: &Path) -> io::Result<Self> {
        let ca = read_file(&directory.join("ca.pem"))?;
        let cert = read_file(&directory.join("cert.pem"))?;
        let key = read_file(&directory.join("key.pem"))?;
        let authority = read_file(&directory.join("admission.pub"))?
            .try_into()
            .map_err(|_| failure("admission.pub must contain 32 raw bytes"))?;
        // 信任根来自显式目录, 不导入系统信任或开启 insecure 模式.
        let mut roots = RootCertStore::empty();
        for certificate in rustls_pemfile::certs(&mut ca.as_slice()) {
            roots.add(certificate?).map_err(|_| failure("invalid CA certificate"))?;
        }
        if roots.is_empty() {
            return Err(failure("CA certificate is required"));
        }
        let certificates = rustls_pemfile::certs(&mut cert.as_slice()).collect::<io::Result<Vec<_>>>()?;
        let key = rustls_pemfile::private_key(&mut key.as_slice())?.ok_or_else(|| failure("TLS private key is required"))?;
        // client 与 server 共用同一密码提供者和信任根, 禁用连接恢复票据.
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let verifier = rustls::client::WebPkiServerVerifier::builder_with_provider(Arc::new(roots.clone()), provider.clone())
            .build()
            .map_err(|_| failure("invalid TLS trust roots"))?;
        let mut client = ClientConfig::builder_with_provider(provider.clone())
            .with_protocol_versions(&[&rustls::version::TLS13])
            .map_err(|_| failure("TLS 1.3 unavailable"))?
            .with_root_certificates(roots.clone())
            .with_no_client_auth();
        client.resumption = rustls::client::Resumption::disabled();
        let mut server = ServerConfig::builder_with_provider(provider)
            .with_protocol_versions(&[&rustls::version::TLS13])
            .map_err(|_| failure("TLS 1.3 unavailable"))?
            .with_no_client_auth()
            .with_single_cert(certificates.clone(), key)
            .map_err(|_| failure("invalid TLS server identity"))?;
        server.send_tls13_tickets = 0;
        let mut rpc_client = client.clone();
        let mut rpc_server = server.clone();
        rpc_client.alpn_protocols = vec![b"h2".to_vec()];
        rpc_server.alpn_protocols = vec![b"h2".to_vec()];
        // 登录配置严格限制字段, 避免密码被错误拼写后默认为空或进入诊断输出.
        let login: serde_json::Value = serde_json::from_slice(&read_file(&directory.join("login.json"))?).map_err(|_| failure("invalid login.json"))?;
        let object = login
            .as_object()
            .filter(|value| value.len() == 2)
            .ok_or_else(|| failure("login.json requires username and password"))?;
        let username = object
            .get("username")
            .and_then(|v| v.as_str())
            .ok_or_else(|| failure("login username is required"))?
            .to_owned();
        let password = object
            .get("password")
            .and_then(|v| v.as_str())
            .ok_or_else(|| failure("login password is required"))?
            .to_owned();
        validate_name("username", &username)?;
        if password.is_empty() || password.len() > 1024 {
            return Err(failure("password length must be 1..1024 bytes"));
        }
        Ok(Self {
            client: Arc::new(client),
            server: Arc::new(server),
            rpc_client: Arc::new(rpc_client),
            rpc_server: Arc::new(rpc_server),
            username,
            password,
            authority,
            certificates,
            verifier,
        })
    }
    /// 账号鉴权不再校验客户端证书, 因此本地启动显式验证公布地址的服务端证书.
    pub fn validate_endpoint(&self, address: std::net::SocketAddr) -> io::Result<()> {
        use rustls::client::danger::ServerCertVerifier;
        let (leaf, chain) = self.certificates.split_first().ok_or_else(|| failure("missing TLS certificate"))?;
        let name = ServerName::IpAddress(address.ip().into());
        self.verifier
            .verify_server_cert(leaf, chain, &name, &[], rustls::pki_types::UnixTime::now())
            .map_err(|_| failure("local TLS certificate does not authorize advertised endpoint"))?;
        Ok(())
    }
    /// 目标身份来自配置的 Supervisor 主机或已经验证的成员地址, 不关闭主机名检查.
    pub async fn connect(&self, stream: TcpStream, host: &str) -> io::Result<TlsStream<TcpStream>> {
        let name = ServerName::try_from(host.to_owned()).map_err(|_| failure("invalid TLS target identity"))?;
        Ok(TlsConnector::from(self.client.clone()).connect(name, stream).await?.into())
    }
    /// 接收入站 TLS, 由上层总期限约束.
    pub async fn accept(&self, stream: TcpStream) -> io::Result<TlsStream<TcpStream>> {
        Ok(TlsAcceptor::from(self.server.clone()).accept(stream).await?.into())
    }
    /// gRPC 出站连接验证服务端主机名和证书, 只允许 TLS 1.3 和 h2.
    pub async fn connect_rpc(&self, stream: TcpStream, host: &str) -> io::Result<TlsStream<TcpStream>> {
        let name = ServerName::try_from(host.to_owned()).map_err(|_| failure("invalid TLS target identity"))?;
        let stream: TlsStream<_> = TlsConnector::from(self.rpc_client.clone()).connect(name, stream).await?.into();
        require_h2(&stream)?;
        Ok(stream)
    }
    /// gRPC 入站连接只接受 TLS 1.3/h2, 应用准入由后续 Hello 处理.
    pub async fn accept_rpc(&self, stream: TcpStream) -> io::Result<TlsStream<TcpStream>> {
        let stream: TlsStream<_> = TlsAcceptor::from(self.rpc_server.clone()).accept(stream).await?.into();
        require_h2(&stream)?;
        Ok(stream)
    }
    /// 准入签名验证原始正文, 不依赖 Go/Rust 对同一个 Protobuf 值重新编码的字节顺序.
    pub fn admission(&self, payload: &Bytes, signed: &[u8]) -> Result<Member, SessionError> {
        if payload.len() > 1024 || signed.len() != 64 {
            return Err(unauthorized());
        }
        let mut input = Vec::with_capacity(24 + payload.len());
        input.extend_from_slice(b"verdandi-admission-v4\0");
        input.extend_from_slice(payload);
        signature::UnparsedPublicKey::new(&signature::ED25519, self.authority)
            .verify(&input, signed)
            .map_err(|_| unauthorized())?;
        let member = Member::decode(payload.clone()).map_err(|_| unauthorized())?;
        validate_member(&member).map_err(|_| unauthorized())?;
        Ok(member)
    }
    /// 账号与端点共同区分部署槽位. 同账号的不同端点不会互相替换; UUID 仍独立标识本次进程.
    pub fn endpoint_principal(&self, cluster: &str, address: &str) -> String {
        hex(digest::digest(&digest::SHA256, format!("{}\0{}\0{}", self.username, cluster, address).as_bytes()).as_ref())
    }
    /// 仅登记模块读取密码, 不传给节点间 RPC 或公开状态.
    pub(crate) fn credentials(&self) -> (&str, &str) {
        (&self.username, &self.password)
    }
    /// bearer credential 只验证签名及正文, 不再声称它绑定当前 TLS 会话.
    pub fn verify(&self, hello: &Hello) -> Result<Member, SessionError> {
        self.admission(&hello.admission, &hello.admission_signature)
    }
}
/// ALPN 是传输选择, 不替代后续凭证与角色验证.
fn require_h2(stream: &TlsStream<TcpStream>) -> io::Result<()> {
    if stream.get_ref().1.alpn_protocol() != Some(b"h2") {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "gRPC requires h2 ALPN"));
    }
    Ok(())
}
/// 公共成员约束与 Go membership 对齐. 列表和直接 Hello 都调用这一入口.
pub fn validate_member(member: &Member) -> io::Result<()> {
    validate_name("cluster_id", &member.cluster_id)?;
    validate_name("group", &member.group)?;
    if !matches!(
        crate::protocol::NodeRole::try_from(member.role),
        Ok(crate::protocol::NodeRole::Star | crate::protocol::NodeRole::Planet)
    ) {
        return Err(failure("invalid member role"));
    }
    if !valid_uuid(&member.peer_id) || !lower_hex(&member.principal, 64) || member.epoch == 0 {
        return Err(failure("invalid member identity"));
    }
    let address = member.advertise.parse().map_err(|_| failure("invalid member address"))?;
    validate_remote_address("member", address)?;
    if member.advertise != address.to_string() {
        return Err(failure("member address is not canonical"));
    }
    Ok(())
}
/// 长度和 ASCII 编码先通过校验再读取版本位, 防止畸形短字符串造成越界.
pub fn valid_uuid(value: &str) -> bool {
    lower_hex(value, 32) && value.as_bytes()[12] == b'4' && matches!(value.as_bytes()[16], b'8' | b'9' | b'a' | b'b')
}
/// 安全身份只接受定长小写十六进制, 不进行大小写或 Unicode 归一化.
fn lower_hex(value: &str, length: usize) -> bool {
    value.len() == length && value.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
/// 把摘要或 UUID 编成独立拥有的小写字符串, 返回值不借用输入.
fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    bytes
        .iter()
        .flat_map(|byte| [char::from(DIGITS[usize::from(byte >> 4)]), char::from(DIGITS[usize::from(byte & 15)])])
        .collect()
}
/// 只读打开部署材料, 文件增长时仍通过 limit 阻止突破 16 KiB 预算.
fn read_file(path: &Path) -> io::Result<Vec<u8>> {
    let file = File::open(path)?;
    let metadata = file.metadata()?;
    if !metadata.is_file() || metadata.len() > 16 * 1024 {
        return Err(failure("identity file must be a regular file no larger than 16 KiB"));
    }
    let mut data = Vec::new();
    file.take(16 * 1024 + 1).read_to_end(&mut data)?;
    if data.len() > 16 * 1024 {
        return Err(failure("identity file exceeds 16 KiB"));
    }
    Ok(data)
}
/// 授权失败使用固定线上枚举, 不泄露签名失败的内部细节.
fn unauthorized() -> SessionError {
    SessionError::Reject(ProtocolErrorCode::Unauthorized)
}
/// 本地材料格式失败终止启动, 不把错误降级成明文或反复网络重试.
fn failure(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

#[cfg(test)]
#[path = "../tests/unit/identity.rs"]
mod tests;
