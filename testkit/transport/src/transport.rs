//! 仅用于对照的两种传输. 相同认证、消息上限、应用队列和连接生命周期.
use crate::generated::{
    transport_probe_client::TransportProbeClient,
    transport_probe_server::{TransportProbe, TransportProbeServer},
};
use crate::{
    Transfer,
    auth::{Auth, ConnectionInfo, HELLO_LIMIT, MESSAGE_LIMIT},
    wire::{HELLO_ID, Reader, TRANSFER_ID, Writer},
};
use hyper_util::rt::TokioIo;
use prost::Message;
use std::{
    io,
    net::SocketAddr,
    pin::Pin,
    sync::{Arc, Mutex},
    task::{Context, Poll},
    time::Duration,
};
use tokio::{
    io::{AsyncRead, AsyncWrite, ReadBuf},
    net::{TcpListener, TcpStream},
    sync::mpsc,
    task::JoinSet,
    time::timeout,
};
use tokio_rustls::TlsStream;
use tokio_stream::{Stream, StreamExt, wrappers::ReceiverStream};
use tokio_util::sync::CancellationToken;
use tonic::{
    Request, Response, Status,
    metadata::MetadataValue,
    transport::{Endpoint, Server, server::Connected},
};

const DEADLINE: Duration = Duration::from_secs(5);
const QUEUE: usize = 16;
// 覆盖 64 * 16 KiB 应用飞行窗口. 单个连接只允许一条 RPC, 不使用自动增长窗口.
const FLOW_WINDOW: u32 = 1048576;

/// 任务随会话销毁而取消. 队列满时反压, 不在后台无限排队.
pub struct Session {
    pub send: mpsc::Sender<Transfer>,
    pub receive: mpsc::Receiver<io::Result<Transfer>>,
    _tasks: JoinSet<()>,
}

/// Tonic 将这里的连接信息放入 Request extensions. 不接受请求自报的 TLS 身份.
struct BoundTls {
    stream: TlsStream<TcpStream>,
    info: ConnectionInfo,
    // 活跃 HTTP/2 连接持续持有配额, 不能在 TLS 握手结束时提前归还.
    _permit: Arc<tokio::sync::OwnedSemaphorePermit>,
    // Hyper 内部连接任务不归外层 JoinSet 所有. 停止必须唤醒并关闭每条实际 I/O.
    stop: Pin<Box<dyn std::future::Future<Output = ()> + Send>>,
    hello_deadline: Pin<Box<tokio::time::Sleep>>,
}
impl BoundTls {
    fn stopped(&mut self, cx: &mut Context<'_>) -> bool {
        use std::future::Future;
        self.stop.as_mut().poll(cx).is_ready()
            || (!self.info.authenticated.load(std::sync::atomic::Ordering::Acquire) && self.hello_deadline.as_mut().poll(cx).is_ready())
    }
}
impl Connected for BoundTls {
    type ConnectInfo = ConnectionInfo;
    fn connect_info(&self) -> Self::ConnectInfo {
        self.info.clone()
    }
}
impl AsyncRead for BoundTls {
    fn poll_read(mut self: Pin<&mut Self>, cx: &mut Context<'_>, buffer: &mut ReadBuf<'_>) -> Poll<io::Result<()>> {
        if self.stopped(cx) {
            return Poll::Ready(Err(io::ErrorKind::ConnectionAborted.into()));
        }
        Pin::new(&mut self.stream).poll_read(cx, buffer)
    }
}
impl AsyncWrite for BoundTls {
    fn poll_write(mut self: Pin<&mut Self>, cx: &mut Context<'_>, data: &[u8]) -> Poll<io::Result<usize>> {
        if self.stopped(cx) {
            return Poll::Ready(Err(io::ErrorKind::ConnectionAborted.into()));
        }
        Pin::new(&mut self.stream).poll_write(cx, data)
    }
    fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        if self.stopped(cx) {
            return Poll::Ready(Err(io::ErrorKind::ConnectionAborted.into()));
        }
        Pin::new(&mut self.stream).poll_flush(cx)
    }
    fn poll_shutdown(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.stream).poll_shutdown(cx)
    }
}

struct GrpcService(Arc<Auth>, Option<Arc<crate::push::Hub>>);

#[tonic::async_trait]
impl TransportProbe for GrpcService {
    type ExchangeStream = Pin<Box<dyn Stream<Item = Result<Transfer, Status>> + Send>>;
    type SynchronizeStream = crate::push_io::ReplyStream;

    async fn synchronize(&self, request: Request<tonic::Streaming<crate::generated::Control>>) -> Result<Response<Self::SynchronizeStream>, Status> {
        let hub = self.1.clone().ok_or_else(|| Status::unimplemented("push scenario is disabled"))?;
        crate::push_io::grpc_server(request, &self.0, hub)
    }

    async fn exchange(&self, request: Request<tonic::Streaming<Transfer>>) -> Result<Response<Self::ExchangeStream>, Status> {
        let info = request.extensions().get::<ConnectionInfo>().ok_or_else(|| Status::unauthenticated("missing TLS context"))?;
        let proof = request
            .metadata()
            .get_bin("hello-bin")
            .ok_or_else(|| Status::unauthenticated("missing proof"))?
            .to_bytes()
            .map_err(|_| Status::unauthenticated("invalid proof"))?;
        self.0.verify(proof, info, false).map_err(|_| Status::unauthenticated("session rejected"))?;
        info.authenticated.store(true, std::sync::atomic::Ordering::Release);
        let reply = self.0.proof();
        // 不额外创建无界回声队列. HTTP/2 响应拉取请求流, 背压由流控和消息预算约束.
        let stream = request.into_inner().map(|message| message.and_then(validate));
        let mut response = Response::new(Box::pin(stream) as Self::ExchangeStream);
        response.metadata_mut().insert_bin("hello-bin", MetadataValue::from_bytes(&reply));
        Ok(response)
    }
}

fn validate(message: Transfer) -> Result<Transfer, Status> {
    if message.body.len() > 16384 {
        return Err(Status::resource_exhausted("payload limit"));
    }
    Ok(message)
}

pub async fn serve(listener: TcpListener, auth: Arc<Auth>, grpc: bool, stop: CancellationToken) -> io::Result<()> {
    serve_push(listener, auth, grpc, stop, None).await
}

pub async fn serve_push(listener: TcpListener, auth: Arc<Auth>, grpc: bool, stop: CancellationToken, hub: Option<Arc<crate::push::Hub>>) -> io::Result<()> {
    // 接收队列和未完成 TLS 握手均受约束. 本原型至多接收 128 条并行连接.
    let mut tasks = JoinSet::new();
    let permits = Arc::new(tokio::sync::Semaphore::new(128));
    let (incoming, receiver) = mpsc::channel::<io::Result<BoundTls>>(16);
    if grpc {
        let service =
            TransportProbeServer::new(GrpcService(auth.clone(), hub.clone())).max_decoding_message_size(MESSAGE_LIMIT).max_encoding_message_size(MESSAGE_LIMIT);
        let token = stop.clone();
        tasks.spawn(async move {
            Server::builder()
                .max_concurrent_streams(1)
                .http2_max_header_list_size(4096)
                .initial_stream_window_size(FLOW_WINDOW)
                .initial_connection_window_size(FLOW_WINDOW)
                .add_service(service)
                .serve_with_incoming_shutdown(ReceiverStream::new(receiver), token.cancelled_owned())
                .await
                .map_err(io::Error::other)
        });
    } else {
        drop(receiver);
    }
    loop {
        tokio::select! {
            () = stop.cancelled() => break,
            completed = tasks.join_next(), if !tasks.is_empty() => {
                if let Some(completed) = completed { completed.map_err(io::Error::other)??; }
            }
            accepted = listener.accept() => {
                let (tcp, _) = accepted?;
                let Ok(permit) = permits.clone().try_acquire_owned() else { drop(tcp); continue; };
                let (auth, incoming, permit, token, hub) = (auth.clone(), incoming.clone(), Arc::new(permit), stop.clone(), hub.clone());
                tasks.spawn(async move {
                    // 无效远端只结束自己的连接, 不让监听器退出. 身份和 TLS 错误不打印敏感字段.
                    let operation = async {
                        let stream = timeout(DEADLINE, auth.accept(tcp)).await??;
                        if grpc {
                            let info = ConnectionInfo::from_tls(&stream)?;
                            incoming.send(Ok(BoundTls { stream, info, _permit: permit.clone(), stop: Box::pin(token.cancelled_owned()), hello_deadline: Box::pin(tokio::time::sleep(DEADLINE)) }))
                                .await.map_err(|_| io::Error::other("incoming queue closed"))?;
                        } else if let Some(hub) = hub {
                            crate::push_io::tcp_server(stream, &auth, hub).await?;
                        } else {
                            tcp_echo(stream, &auth).await?;
                        }
                        Ok::<_, io::Error>(())
                    };
                    let _ = operation.await;
                    drop(permit);
                    Ok(())
                });
            }
        }
    }
    // 测试停止是强制生命周期边界. JoinSet 取消并等待自己拥有的任务, 不遗留连接.
    tasks.abort_all();
    while tasks.join_next().await.is_some() {}
    Ok(())
}

async fn tcp_echo(stream: TlsStream<TcpStream>, auth: &Auth) -> io::Result<()> {
    let info = ConnectionInfo::from_tls(&stream)?;
    let (read, write) = tokio::io::split(stream);
    let (mut read, mut write) = (Reader::new(read), Writer::new(write));
    timeout(DEADLINE, async {
        let hello = read.read(HELLO_ID, HELLO_LIMIT).await?.ok_or_else(eof)?;
        auth.verify(hello, &info, false)?;
        write.bytes(HELLO_ID, &auth.proof(), HELLO_LIMIT).await
    })
    .await??;
    while let Some(bytes) = read.read(TRANSFER_ID, MESSAGE_LIMIT).await? {
        let message = validate(Transfer::decode(bytes)?).map_err(io::Error::other)?;
        write.message(&message, MESSAGE_LIMIT).await?;
    }
    write.shutdown().await
}

pub async fn connect(address: SocketAddr, auth: &Auth, grpc: bool) -> io::Result<Session> {
    timeout(DEADLINE, connect_inner(address, auth, grpc, None)).await?
}

/// 覆盖凭证仅供白盒篡改测试, 合法 bearer 凭证可以跨 TLS 连接复用.
async fn connect_inner(address: SocketAddr, auth: &Auth, grpc: bool, override_proof: Option<prost::bytes::Bytes>) -> io::Result<Session> {
    let tcp = TcpStream::connect(address).await?;
    tcp.set_nodelay(true)?;
    let stream = auth.identity.connect(tcp, &address.ip().to_string()).await?;
    let info = ConnectionInfo::from_tls(&stream)?;
    let proof = override_proof.unwrap_or_else(|| auth.proof());
    let (sender, outbound) = mpsc::channel::<Transfer>(QUEUE);
    let (inbound, receiver) = mpsc::channel(QUEUE);
    let mut tasks = JoinSet::new();
    if grpc {
        let channel = bound_channel(address, stream).await?;
        let mut client = TransportProbeClient::new(channel).max_decoding_message_size(MESSAGE_LIMIT).max_encoding_message_size(MESSAGE_LIMIT);
        let mut request = Request::new(ReceiverStream::new(outbound));
        request.metadata_mut().insert_bin("hello-bin", MetadataValue::from_bytes(&proof));
        let response = client.exchange(request).await.map_err(io::Error::other)?;
        let proof = response.metadata().get_bin("hello-bin").ok_or_else(|| io::Error::other("missing server proof"))?.to_bytes().map_err(io::Error::other)?;
        auth.verify(proof, &info, true)?;
        let mut stream = response.into_inner();
        tasks.spawn(async move {
            loop {
                let next = tokio::select! {
                    () = inbound.closed() => break,
                    message = stream.message() => message,
                };
                match next {
                    Ok(Some(message)) => {
                        if inbound.send(Ok(message)).await.is_err() {
                            break;
                        }
                    }
                    Ok(None) => break,
                    Err(error) => {
                        let _ = inbound.send(Err(io::Error::other(error))).await;
                        break;
                    }
                }
            }
        });
    } else {
        let (read, write) = tokio::io::split(stream);
        let (mut read, mut write) = (Reader::new(read), Writer::new(write));
        write.bytes(HELLO_ID, &proof, HELLO_LIMIT).await?;
        let proof = read.read(HELLO_ID, HELLO_LIMIT).await?.ok_or_else(eof)?;
        auth.verify(proof, &info, true)?;
        let failure = inbound.clone();
        tasks.spawn(async move {
            let mut outbound = outbound;
            while let Some(message) = outbound.recv().await {
                if let Err(error) = write.message(&message, MESSAGE_LIMIT).await {
                    let _ = failure.send(Err(error)).await;
                    return;
                }
            }
            let _ = write.shutdown().await;
        });
        tasks.spawn(async move {
            loop {
                match read.read(TRANSFER_ID, MESSAGE_LIMIT).await {
                    Ok(Some(bytes)) => {
                        if inbound.send(Transfer::decode(bytes).map_err(io::Error::other)).await.is_err() {
                            break;
                        }
                    }
                    Ok(None) => break,
                    Err(error) => {
                        let _ = inbound.send(Err(error)).await;
                        break;
                    }
                }
            }
        });
    }
    Ok(Session { send: sender, receive: receiver, _tasks: tasks })
}

fn eof() -> io::Error {
    io::Error::from(io::ErrorKind::UnexpectedEof)
}

/// 所有原型 RPC 共用这个一次性已认证连接, 禁止复用旧 metadata 透明换 TCP.
pub async fn bound_channel(address: SocketAddr, stream: TlsStream<TcpStream>) -> io::Result<tonic::transport::Channel> {
    let once = Arc::new(Mutex::new(Some(stream)));
    let connector = tower::service_fn(move |_| {
        let stream = once
            .lock()
            .map_err(|_| io::Error::other("connector poisoned"))
            .and_then(|mut once| once.take().ok_or_else(|| io::Error::other("new authenticated session required")));
        async move { stream.map(TokioIo::new) }
    });
    // TLS 已在自定义 connector 中完成. http URI 仅用于 h2 建链, 不产生明文连接.
    Endpoint::from_shared(format!("http://{address}"))
        .map_err(io::Error::other)?
        .initial_stream_window_size(FLOW_WINDOW)
        .initial_connection_window_size(FLOW_WINDOW)
        .connect_with_connector(connector)
        .await
        .map_err(io::Error::other)
}

#[cfg(test)]
#[path = "../tests/transport.rs"]
mod tests;
