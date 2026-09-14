//! gRPC 会话适配. HTTP/2 分帧和流控交给 Tonic, 本层只拥有已认证 socket 和有界消息流.

use crate::protocol::{
    MAX_HELLO_FRAME_BYTES, SessionPacket,
    peer_transport_client::PeerTransportClient,
    peer_transport_server::{PeerTransport, PeerTransportServer},
};
use hyper_util::rt::TokioIo;
use std::{
    io,
    net::SocketAddr,
    pin::Pin,
    sync::{Arc, Mutex},
    task::{Context, Poll},
};
use tokio::{
    io::{AsyncRead, AsyncWrite, ReadBuf},
    net::TcpStream,
    sync::{mpsc, oneshot},
    task::JoinSet,
};
use tokio_rustls::TlsStream;
use tokio_stream::{Stream, StreamExt, wrappers::ReceiverStream};
use tokio_util::sync::CancellationToken;
use tonic::{
    Request, Response, Status, Streaming,
    transport::{Channel, Endpoint, Server, server::Connected},
};

// 当前只有小型控制消息. 队列对象和 HTTP/2 字节窗口分别限制, 不使用自适应无界增长.
const QUEUE: usize = 4;
const WINDOW: u32 = 64 * 1024;

/// 登记 RPC 借用一次已建立的 TLS, 返回的租约释放时关闭框架后台 I/O.
pub(crate) async fn channel(stream: TlsStream<TcpStream>, authority: &str) -> io::Result<(Channel, Lifetime)> {
    let cancellation = CancellationToken::new();
    let lifetime = Lifetime {
        cancellation: cancellation.clone(),
        tasks: JoinSet::new(),
    };
    let stream = BoundTls {
        stream,
        stop: Box::pin(cancellation.cancelled_owned()),
    };
    Ok((connect_channel(stream, authority).await?, lifetime))
}

/// connector 只消费一次 socket, 框架不得绕过角色层自行重拨或改变节点身份.
async fn connect_channel(stream: BoundTls, authority: &str) -> io::Result<Channel> {
    let once = Arc::new(Mutex::new(Some(stream)));
    let connector = tower::service_fn(move |_| {
        let stream = once.lock().map_err(|_| io::Error::other("connector state unavailable")).and_then(|mut once| {
            once.take()
                .ok_or_else(|| io::Error::new(io::ErrorKind::ConnectionAborted, "new session required"))
        });
        async move { stream.map(TokioIo::new) }
    });
    // 此 URI 仅提供 HTTP/2 authority, connector 只返回已完成 TLS 的 socket.
    Endpoint::from_shared(format!("http://{authority}"))
        .map_err(io::Error::other)?
        .initial_stream_window_size(WINDOW)
        .initial_connection_window_size(WINDOW)
        .connect_with_connector(connector)
        .await
        .map_err(io::Error::other)
}

/// RPC 的后台任务与实际 TLS I/O 同时归当前会话拥有. Drop 也覆盖握手中途取消.
pub(crate) struct Lifetime {
    cancellation: CancellationToken,
    tasks: JoinSet<io::Result<()>>,
}
impl Drop for Lifetime {
    /// 返回路径及 future 中途取消都关闭真实 I/O, 不能仅关闭应用队列.
    fn drop(&mut self) {
        self.cancellation.cancel();
        self.tasks.abort_all();
    }
}

/// Tonic 内部驱动任务也必须响应会话取消, 不能仅丢弃外层消息接收器.
struct BoundTls {
    stream: TlsStream<TcpStream>,
    stop: Pin<Box<dyn std::future::Future<Output = ()> + Send>>,
}
impl BoundTls {
    /// 轮询取消 future 同时登记 waker, 保证空闲连接也能被唤醒关闭.
    fn stopped(&mut self, cx: &mut Context<'_>) -> bool {
        self.stop.as_mut().poll(cx).is_ready()
    }
}
impl Connected for BoundTls {
    type ConnectInfo = ();
    fn connect_info(&self) {}
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
        if self.stopped(cx) {
            return Poll::Ready(Err(io::ErrorKind::ConnectionAborted.into()));
        }
        Pin::new(&mut self.stream).poll_shutdown(cx)
    }
}

#[cfg(test)]
#[path = "../tests/unit/rpc.rs"]
mod tests;

type Rendezvous = (oneshot::Sender<Streaming<SessionPacket>>, mpsc::Receiver<SessionPacket>);
struct Service(Mutex<Option<Rendezvous>>);

#[tonic::async_trait]
impl PeerTransport for Service {
    type OpenSessionStream = Pin<Box<dyn Stream<Item = Result<SessionPacket, Status>> + Send>>;

    async fn open_session(&self, request: Request<Streaming<SessionPacket>>) -> Result<Response<Self::OpenSessionStream>, Status> {
        // 每条连接只拥有一个应用会话, 重连由拓扑层重新分配连接租约.
        let (incoming, outgoing) = self
            .0
            .lock()
            .map_err(|_| Status::internal("session state unavailable"))?
            .take()
            .ok_or_else(|| Status::already_exists("one session per connection"))?;
        incoming.send(request.into_inner()).map_err(|_| Status::cancelled("session stopped"))?;
        Ok(Response::new(Box::pin(ReceiverStream::new(outgoing).map(Ok))))
    }
}

/// 复用上层已验证服务端身份的 TLS, 连接重建仍由拓扑层管理.
pub(crate) async fn open(
    stream: TlsStream<TcpStream>,
    address: SocketAddr,
    inbound: bool,
    cancellation: &CancellationToken,
) -> io::Result<(Streaming<SessionPacket>, mpsc::Sender<SessionPacket>, Lifetime)> {
    let token = cancellation.child_token();
    let mut lifetime = Lifetime {
        cancellation: token.clone(),
        tasks: JoinSet::new(),
    };
    let stream = BoundTls {
        stream,
        stop: Box::pin(token.clone().cancelled_owned()),
    };
    let (send, outgoing) = mpsc::channel(QUEUE);
    let receive = if inbound {
        let (accepted, incoming) = oneshot::channel();
        let service = PeerTransportServer::new(Service(Mutex::new(Some((accepted, outgoing)))))
            .max_decoding_message_size(MAX_HELLO_FRAME_BYTES as usize)
            .max_encoding_message_size(MAX_HELLO_FRAME_BYTES as usize);
        lifetime.tasks.spawn(async move {
            // 保持单连接 incoming 流有效直到取消; 不在 HTTP/2 建链后立即归还物理连接所有权.
            let incoming = tokio_stream::once(Ok::<_, io::Error>(stream)).chain(tokio_stream::pending());
            Server::builder()
                .max_concurrent_streams(1)
                .http2_max_header_list_size(4096)
                .initial_stream_window_size(WINDOW)
                .initial_connection_window_size(WINDOW)
                .add_service(service)
                .serve_with_incoming_shutdown(incoming, token.cancelled_owned())
                .await
                .map_err(io::Error::other)
        });
        incoming
            .await
            .map_err(|_| io::Error::new(io::ErrorKind::ConnectionAborted, "RPC handshake closed"))?
    } else {
        let channel = connect_channel(stream, &address.to_string()).await?;
        PeerTransportClient::new(channel)
            .max_decoding_message_size(MAX_HELLO_FRAME_BYTES as usize)
            .max_encoding_message_size(MAX_HELLO_FRAME_BYTES as usize)
            .open_session(ReceiverStream::new(outgoing))
            .await
            .map_err(io::Error::other)?
            .into_inner()
    };
    Ok((receive, send, lifetime))
}
