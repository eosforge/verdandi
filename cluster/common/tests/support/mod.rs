//! 测试专用 TLS 登记端与公开证书夹具. 不代替真实 Go/Rust 跨进程回归.
use crate::{
    PeerConfig,
    identity::{Identity, ProcessIdentity},
    protocol::{
        Hello, LoginRequest, Member, NodeRole, RegistrationChallenge, RegistrationRequest, RegistrationResponse, SessionPacket,
        admission_server::{Admission, AdmissionServer},
        peer_transport_client::PeerTransportClient,
    },
};
use prost::{Message, bytes::Bytes};
use ring::signature::{Ed25519KeyPair, KeyPair};
use std::{
    collections::BTreeMap,
    io,
    net::SocketAddr,
    path::PathBuf,
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::{
    io::{AsyncRead, AsyncWrite, ReadBuf},
    net::{TcpListener, TcpStream},
    task::{JoinHandle, JoinSet},
    time,
};
use tokio_util::sync::CancellationToken;

pub(crate) const WAIT: Duration = Duration::from_secs(6);
pub(crate) fn fixture(role: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../tests/fixtures").join(role)
}
pub(crate) fn localhost() -> SocketAddr {
    SocketAddr::from(([127, 0, 0, 1], 0))
}
pub(crate) fn config(role: &str, supervisor: SocketAddr) -> PeerConfig {
    let mut config =
        PeerConfig::new("alpha", localhost(), supervisor.to_string(), fixture(role)).with_heartbeat(Duration::from_millis(120), Duration::from_millis(600));
    config.handshake_timeout = Duration::from_secs(2);
    config.reconnect_min = Duration::from_millis(20);
    config.reconnect_max = Duration::from_millis(100);
    config
}
pub(crate) fn signed(member: &Member) -> io::Result<Hello> {
    let pem = std::fs::read(fixture("pulsar").join("admission.key"))?;
    let key = rustls_pemfile::private_key(&mut pem.as_slice())?.ok_or_else(|| io::Error::other("missing fixture key"))?;
    let key = Ed25519KeyPair::from_pkcs8_maybe_unchecked(key.secret_der()).map_err(|_| io::Error::other("invalid fixture key"))?;
    // 公开夹具使用 PKCS#8 v1, 公钥不嵌入私钥文件, 因而显式核对独立公钥.
    let public = std::fs::read(fixture("pulsar").join("admission.pub"))?;
    if key.public_key().as_ref() != public {
        return Err(io::Error::other("fixture signing key mismatch"));
    }
    let admission = Bytes::from(member.encode_to_vec());
    let mut input = b"verdandi-admission-v4\0".to_vec();
    input.extend_from_slice(&admission);
    Ok(Hello {
        protocol_major: 4,
        protocol_minor: 0,
        max_frame_bytes: 4096,
        admission,
        admission_signature: Bytes::copy_from_slice(key.sign(&input).as_ref()),
    })
}
pub(crate) fn member(identity: &Identity, process: &ProcessIdentity, address: SocketAddr, epoch: u64) -> Member {
    Member {
        cluster_id: "alpha".into(),
        peer_id: process.id.clone(),
        principal: identity.endpoint_principal("alpha", &address.to_string()),
        advertise: address.to_string(),
        epoch,
        role: crate::protocol::NodeRole::Star as i32,
        group: "default".into(),
    }
}

/// 每个测试独占模型. 模式只修改响应, 保持被测 Peer 使用真实 TLS 和完整启动流程.
#[derive(Clone, Copy, Default)]
pub(crate) enum Mode {
    #[default]
    Normal,
    LoseFirstResponse,
    Truncate,
    OmitSelf,
    Duplicate,
}
#[derive(Default)]
struct Model {
    members: BTreeMap<String, Member>,
    requests: usize,
}
pub(crate) struct Supervisor {
    pub(crate) address: SocketAddr,
    state: Arc<Mutex<Model>>,
    cancellation: CancellationToken,
    task: Option<JoinHandle<io::Result<()>>>,
}
impl Supervisor {
    pub(crate) async fn start(mode: Mode) -> io::Result<Self> {
        Self::bind(localhost(), mode).await
    }
    pub(crate) async fn bind(address: SocketAddr, mode: Mode) -> io::Result<Self> {
        // 配置错误在构造时暴露, 不把签名失败隐藏为客户端超时.
        signed(&Member::default())?;
        let listener = TcpListener::bind(address).await?;
        let address = listener.local_addr()?;
        let identity = Arc::new(Identity::load(&fixture("pulsar"))?);
        let state = Arc::new(Mutex::new(Model::default()));
        let cancellation = CancellationToken::new();
        let (model, token) = (state.clone(), cancellation.clone());
        let task = tokio::spawn(async move {
            let mut tasks = JoinSet::new();
            loop {
                tokio::select! {
                    biased;
                    () = token.cancelled() => break,
                    Some(result) = tasks.join_next() => { result.map_err(io::Error::other)??; }
                    accepted = listener.accept() => {
                        let (stream, _) = accepted?;
                        let (identity, model) = (identity.clone(), model.clone());
                        tasks.spawn(async move {
                            // 断开的测试客户端属于输入, 不使夹具自身停止. 每条连接都有总期限.
                            let _ = time::timeout(WAIT, serve(stream, &identity, model, mode)).await;
                            Ok::<_, io::Error>(())
                        });
                    }
                }
            }
            tasks.abort_all();
            while tasks.join_next().await.is_some() {}
            Ok(())
        });
        Ok(Self {
            address,
            state,
            cancellation,
            task: Some(task),
        })
    }
    pub(crate) fn requests(&self) -> io::Result<usize> {
        Ok(self.state.lock().map_err(|_| io::Error::other("model poisoned"))?.requests)
    }
    pub(crate) async fn shutdown(mut self) -> io::Result<()> {
        self.cancellation.cancel();
        if let Some(task) = self.task.take() {
            task.await.map_err(io::Error::other)??;
        }
        Ok(())
    }
}
impl Drop for Supervisor {
    fn drop(&mut self) {
        self.cancellation.cancel();
        if let Some(task) = self.task.take() {
            task.abort();
        }
    }
}

use std::{
    pin::Pin,
    task::{Context, Poll},
};
use tokio_rustls::TlsStream;
use tokio_stream::{StreamExt, wrappers::ReceiverStream};
use tonic::{Request, Response, Status};

// 测试连接同样有独立取消所有者, 避免断言失败时遗留 HTTP/2 后台 I/O.
struct TestTls {
    stream: TlsStream<TcpStream>,
    stop: Pin<Box<dyn std::future::Future<Output = ()> + Send>>,
}
impl tonic::transport::server::Connected for TestTls {
    type ConnectInfo = ();
    fn connect_info(&self) {}
}
impl AsyncRead for TestTls {
    fn poll_read(mut self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &mut ReadBuf<'_>) -> Poll<io::Result<()>> {
        if self.stop.as_mut().poll(cx).is_ready() {
            return Poll::Ready(Err(io::ErrorKind::ConnectionAborted.into()));
        }
        Pin::new(&mut self.stream).poll_read(cx, buf)
    }
}
impl AsyncWrite for TestTls {
    fn poll_write(mut self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &[u8]) -> Poll<io::Result<usize>> {
        if self.stop.as_mut().poll(cx).is_ready() {
            return Poll::Ready(Err(io::ErrorKind::ConnectionAborted.into()));
        }
        Pin::new(&mut self.stream).poll_write(cx, buf)
    }
    fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.stream).poll_flush(cx)
    }
    fn poll_shutdown(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.stream).poll_shutdown(cx)
    }
}
pub(crate) struct RpcSession {
    pub(crate) reader: tonic::Streaming<SessionPacket>,
    pub(crate) writer: tokio::sync::mpsc::Sender<SessionPacket>,
    cancellation: CancellationToken,
}
impl Drop for RpcSession {
    fn drop(&mut self) {
        self.cancellation.cancel();
    }
}
impl RpcSession {
    pub(crate) async fn connect(identity: &Identity, address: SocketAddr) -> io::Result<Self> {
        let socket = TcpStream::connect(address).await?;
        let stream = identity.connect_rpc(socket, &address.ip().to_string()).await?;
        let cancellation = CancellationToken::new();
        let stream = TestTls {
            stream,
            stop: Box::pin(cancellation.clone().cancelled_owned()),
        };
        let once = Arc::new(Mutex::new(Some(stream)));
        let connector = tower::service_fn(move |_| {
            let stream = once
                .lock()
                .map_err(|_| io::Error::other("fixture lock"))
                .and_then(|mut once| once.take().ok_or_else(|| io::Error::other("fixture reconnect")));
            async move { stream.map(hyper_util::rt::TokioIo::new) }
        });
        let channel = tonic::transport::Endpoint::from_shared(format!("http://{address}"))
            .map_err(io::Error::other)?
            .connect_with_connector(connector)
            .await
            .map_err(io::Error::other)?;
        let (writer, outgoing) = tokio::sync::mpsc::channel(4);
        let reader = PeerTransportClient::new(channel)
            .open_session(ReceiverStream::new(outgoing))
            .await
            .map_err(io::Error::other)?
            .into_inner();
        Ok(Self { reader, writer, cancellation })
    }
    pub(crate) async fn send(&self, body: crate::protocol::session_packet::Body) -> io::Result<()> {
        self.writer.send(SessionPacket { body: Some(body) }).await.map_err(io::Error::other)
    }
    pub(crate) async fn receive(&mut self) -> io::Result<crate::protocol::session_packet::Body> {
        time::timeout(WAIT, self.reader.message())
            .await?
            .map_err(io::Error::other)?
            .and_then(|m| m.body)
            .ok_or_else(|| io::ErrorKind::UnexpectedEof.into())
    }
}
struct Service {
    state: Arc<Mutex<Model>>,
    mode: Mode,
}
fn principal(username: &str, cluster: &str, address: &str) -> String {
    ring::digest::digest(&ring::digest::SHA256, format!("{username}\0{cluster}\0{address}").as_bytes())
        .as_ref()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}
fn login(username: &str, password: &str) -> Result<(), Status> {
    if !matches!(username, "stars" | "planets") || password != "verdandi-public-test-only" {
        return Err(Status::unauthenticated("invalid fixture login"));
    }
    Ok(())
}
#[tonic::async_trait]
impl Admission for Service {
    async fn challenge(&self, request: Request<LoginRequest>) -> Result<Response<RegistrationChallenge>, Status> {
        let r = request.into_inner();
        login(&r.username, &r.password)?;
        let key = principal(&r.username, &r.cluster_id, &r.advertise);
        let state = self.state.lock().map_err(|_| Status::internal("fixture lock"))?;
        Ok(Response::new(RegistrationChallenge {
            cluster_id: "alpha".into(),
            expected_epoch: state.members.get(&key).map_or(0, |m| m.epoch),
        }))
    }
    async fn register(&self, request: Request<RegistrationRequest>) -> Result<Response<RegistrationResponse>, Status> {
        let request = request.into_inner();
        login(&request.username, &request.password)?;
        let principal = principal(&request.username, &request.cluster_id, &request.advertise);
        let (local, mut members, count) = {
            let mut state = self.state.lock().map_err(|_| Status::internal("fixture lock"))?;
            let old = state.members.get(&principal);
            let next = if old.is_some_and(|m| m.peer_id == request.peer_id && m.advertise == request.advertise) {
                old.map_or(1, |m| m.epoch)
            } else if old.map_or(0, |m| m.epoch) == request.expected_epoch {
                request.expected_epoch + 1
            } else {
                return Err(Status::aborted("fixture CAS conflict"));
            };
            let local = Member {
                cluster_id: request.cluster_id,
                peer_id: request.peer_id,
                principal: principal.clone(),
                advertise: request.advertise,
                epoch: next,
                role: request.role,
                group: request.group,
            };
            state.members.insert(principal, local.clone());
            state.requests += 1;
            (local, state.members.values().cloned().collect::<Vec<_>>(), state.requests)
        };
        if matches!(self.mode, Mode::LoseFirstResponse) && count == 1 {
            return Err(Status::unavailable("fixture response lost"));
        }
        if matches!(self.mode, Mode::Truncate) {
            return Err(Status::unavailable("fixture incomplete response"));
        }
        members.retain(|m| m.role == NodeRole::Star as i32);
        members.sort_by(|a, b| a.peer_id.cmp(&b.peer_id));
        if local.role == NodeRole::Planet as i32 {
            members.sort_by(|a, b| (a.group != local.group, &a.peer_id).cmp(&(b.group != local.group, &b.peer_id)));
            members.truncate(8);
        }
        if matches!(self.mode, Mode::OmitSelf) {
            members.retain(|m| m != &local);
        }
        if matches!(self.mode, Mode::Duplicate) {
            members.push(local.clone());
        }
        let hello = signed(&local).map_err(|_| Status::internal("fixture signing"))?;
        Ok(Response::new(RegistrationResponse {
            members,
            admission: hello.admission,
            signature: hello.admission_signature,
        }))
    }
}
async fn serve(stream: TcpStream, identity: &Identity, state: Arc<Mutex<Model>>, mode: Mode) -> io::Result<()> {
    let stream = identity.accept_rpc(stream).await?;
    let stream = TestTls {
        stream,
        stop: Box::pin(std::future::pending()),
    };
    let incoming = tokio_stream::once(Ok::<_, io::Error>(stream)).chain(tokio_stream::pending());
    tonic::transport::Server::builder()
        .add_service(AdmissionServer::new(Service { state, mode }))
        .serve_with_incoming(incoming)
        .await
        .map_err(io::Error::other)
}
