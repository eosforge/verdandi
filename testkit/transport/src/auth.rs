//! 公开测试账号签发的 bearer 凭据. 复用生产验证器, 签发器只用于隔离原型.
use prost::{Message, bytes::Bytes};
use ring::signature::{Ed25519KeyPair, KeyPair};
use rustls::ServerConfig;
use std::{
    fs, io,
    path::Path,
    sync::{Arc, atomic::AtomicBool},
};
use tokio::net::TcpStream;
use tokio_rustls::{TlsAcceptor, TlsStream};
use verdandi_peer_common::{
    identity::{Identity, ProcessIdentity},
    protocol::{Hello, Member, NodeRole},
};

/// 所有认证元数据硬限制为 2 KiB, 不信任对方声明的消息预算.
pub const HELLO_LIMIT: usize = 2048;
/// 两套传输使用相同的最大 Protobuf 正文和流窗口.
pub const MESSAGE_LIMIT: usize = 32768;

pub struct Auth {
    pub identity: Identity,
    hello: Hello,
    acceptor: TlsAcceptor,
}

/// 每条物理连接的会话占用状态, 不承担 exporter 绑定.
#[derive(Clone)]
pub struct ConnectionInfo {
    pub authenticated: Arc<AtomicBool>,
}

impl ConnectionInfo {
    pub fn from_tls(_stream: &TlsStream<TcpStream>) -> io::Result<Self> {
        Ok(Self { authenticated: Arc::new(AtomicBool::new(false)) })
    }
}

impl Auth {
    pub fn fixture(root: &Path, role: NodeRole, server: bool, grpc: bool) -> io::Result<Self> {
        let name = if server {
            "peer-a"
        } else if role == NodeRole::Planet {
            "planet-a"
        } else {
            "peer-b"
        };
        let directory = root.join(name);
        let mut identity = Identity::load(&directory)?;
        let process = ProcessIdentity::new()?;
        let member = Member {
            cluster_id: "alpha".into(),
            peer_id: process.id.clone(),
            principal: identity.endpoint_principal("alpha", "127.0.0.1:39001"),
            advertise: "127.0.0.1:39001".into(),
            epoch: 1,
            role: role as i32,
            group: "default".into(),
        };
        // 只使用仓库公开的测试签名密钥. 本程序不模拟真实 Pulsar/CAS 或候选管理.
        let pem = fs::read(root.join("pulsar/admission.key"))?;
        let key = rustls_pemfile::private_key(&mut pem.as_slice())?.ok_or_else(|| io::Error::other("missing fixture key"))?;
        let signer = Ed25519KeyPair::from_pkcs8_maybe_unchecked(key.secret_der()).map_err(|_| io::Error::other("invalid fixture key"))?;
        if signer.public_key().as_ref() != fs::read(root.join("pulsar/admission.pub"))? {
            return Err(io::Error::other("fixture key mismatch"));
        }
        let admission = Bytes::from(member.encode_to_vec());
        let mut input = b"verdandi-admission-v4\0".to_vec();
        input.extend_from_slice(&admission);
        let hello = Hello {
            protocol_major: 4,
            protocol_minor: 0,
            max_frame_bytes: MESSAGE_LIMIT as u32,
            admission,
            admission_signature: Bytes::copy_from_slice(signer.sign(&input).as_ref()),
        };
        // 原型单独构建服务端配置以设置 h2 ALPN, 不为测试扩大生产 Identity 的公开接口.
        // 两侧均使用 TLS 1.3, ring, 明确信任根和服务端证书校验, 禁用会话恢复.
        let cert = fs::read(directory.join("cert.pem"))?;
        let key = fs::read(directory.join("key.pem"))?;
        let certs = rustls_pemfile::certs(&mut cert.as_slice()).collect::<io::Result<Vec<_>>>()?;
        let key = rustls_pemfile::private_key(&mut key.as_slice())?.ok_or_else(|| io::Error::other("missing TLS key"))?;
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let mut config = ServerConfig::builder_with_provider(provider)
            .with_protocol_versions(&[&rustls::version::TLS13])
            .map_err(io::Error::other)?
            .with_no_client_auth()
            .with_single_cert(certs, key)
            .map_err(io::Error::other)?;
        config.send_tls13_tickets = 0;
        if grpc {
            config.alpn_protocols = vec![b"h2".to_vec()];
            Arc::make_mut(&mut identity.client).alpn_protocols = config.alpn_protocols.clone();
        }
        Ok(Self { identity, hello, acceptor: TlsAcceptor::from(Arc::new(config)) })
    }

    pub async fn accept(&self, tcp: TcpStream) -> io::Result<TlsStream<TcpStream>> {
        tcp.set_nodelay(true)?;
        Ok(self.acceptor.accept(tcp).await?.into())
    }

    pub fn proof(&self) -> Bytes {
        Bytes::from(self.hello.encode_to_vec())
    }

    pub fn verify(&self, bytes: Bytes, _info: &ConnectionInfo, require_star: bool) -> io::Result<()> {
        if bytes.len() > HELLO_LIMIT {
            return Err(io::Error::other("authentication metadata limit"));
        }
        let hello = Hello::decode(bytes)?;
        let member = self.identity.verify(&hello).map_err(|e| e.into_io())?;
        if hello.protocol_major != 4
            || hello.max_frame_bytes != MESSAGE_LIMIT as u32
            || member.cluster_id != "alpha"
            || (require_star && member.role != NodeRole::Star as i32)
        {
            return Err(io::Error::other("session policy rejected"));
        }
        Ok(())
    }
}
