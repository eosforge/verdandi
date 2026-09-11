//! 推流的两套薄适配器. 身份验证和调度器相同, 只替换消息读写与编码入口.
use crate::{
    auth::{Auth, ConnectionInfo, HELLO_LIMIT, MESSAGE_LIMIT},
    generated::{Control, Event, transport_probe_client::TransportProbeClient},
    push::Hub,
    transport::bound_channel,
    wire::{HELLO_ID, Reader, Writer},
};
use prost::Message;
use std::{
    io,
    net::SocketAddr,
    pin::Pin,
    sync::{Arc, atomic::Ordering},
    task::{Context, Poll},
    time::Duration,
};
use tokio::{net::TcpStream, sync::mpsc, task::JoinSet, time::timeout};
use tokio_rustls::TlsStream;
use tokio_stream::{Stream, wrappers::ReceiverStream};
use tonic::{Request, Response, Status, metadata::MetadataValue};

const CONTROL_ID: u16 = 3;
const EVENT_ID: u16 = 4;

pub struct Connection {
    pub send: mpsc::Sender<Control>,
    pub receive: mpsc::Receiver<io::Result<Event>>,
    _tasks: JoinSet<()>,
}

/// RPC response 销毁时取消读任务和调度任务, 不把任务泄露给 Tokio 全局运行时.
pub struct ReplyStream {
    inner: ReceiverStream<io::Result<Event>>,
    _tasks: JoinSet<()>,
}
impl Stream for ReplyStream {
    type Item = Result<Event, Status>;
    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        Pin::new(&mut self.inner).poll_next(cx).map(|item| item.map(|result| result.map_err(|_| Status::data_loss("push session requires resynchronization"))))
    }
}

pub fn grpc_server(request: Request<tonic::Streaming<Control>>, auth: &Auth, hub: Arc<Hub>) -> Result<Response<ReplyStream>, Status> {
    let info = request.extensions().get::<ConnectionInfo>().ok_or_else(|| Status::unauthenticated("missing TLS context"))?;
    let proof = request
        .metadata()
        .get_bin("hello-bin")
        .ok_or_else(|| Status::unauthenticated("missing proof"))?
        .to_bytes()
        .map_err(|_| Status::unauthenticated("invalid proof"))?;
    auth.verify(proof, info, false).map_err(|_| Status::unauthenticated("session rejected"))?;
    info.authenticated.store(true, Ordering::Release);
    let reply = auth.proof();
    let mut incoming = request.into_inner();
    let (send, input) = mpsc::channel(8);
    let (output, receive) = mpsc::channel(16);
    let mut tasks = JoinSet::new();
    tasks.spawn(async move {
        loop {
            match incoming.message().await {
                Ok(Some(message)) => {
                    if send.send(Ok(message)).await.is_err() {
                        break;
                    }
                }
                Ok(None) => break,
                Err(error) => {
                    let _ = send.send(Err(io::Error::other(error))).await;
                    break;
                }
            }
        }
    });
    tasks.spawn(async move {
        if let Err(error) = hub.session(input, output.clone()).await {
            eprintln!("push session ended: {error}");
            let _ = output.send(Err(error)).await;
        }
    });
    let mut response = Response::new(ReplyStream { inner: ReceiverStream::new(receive), _tasks: tasks });
    response.metadata_mut().insert_bin("hello-bin", MetadataValue::from_bytes(&reply));
    Ok(response)
}

pub async fn tcp_server(stream: TlsStream<TcpStream>, auth: &Auth, hub: Arc<Hub>) -> io::Result<()> {
    let info = ConnectionInfo::from_tls(&stream)?;
    let (read, write) = tokio::io::split(stream);
    let (mut read, mut write) = (Reader::new(read), Writer::new(write));
    timeout(Duration::from_secs(5), async {
        let proof = read.read(HELLO_ID, HELLO_LIMIT).await?.ok_or_else(closed)?;
        auth.verify(proof, &info, false)?;
        write.bytes(HELLO_ID, &auth.proof(), HELLO_LIMIT).await
    })
    .await??;
    let (send, input) = mpsc::channel(8);
    let (output, mut receive) = mpsc::channel::<io::Result<Event>>(16);
    let mut tasks = JoinSet::new();
    tasks.spawn(async move {
        loop {
            match read.read(CONTROL_ID, MESSAGE_LIMIT).await {
                Ok(Some(bytes)) => {
                    if send.send(Control::decode(bytes).map_err(io::Error::other)).await.is_err() {
                        break;
                    }
                }
                Ok(None) => break,
                Err(error) => {
                    let _ = send.send(Err(error)).await;
                    break;
                }
            }
        }
    });
    // 调度结束后仍然排空最后的 Completed 帧. 不提前 abort 写任务吞掉尾部消息.
    let writer = async {
        while let Some(message) = receive.recv().await {
            write.batch(EVENT_ID, batch(message, &mut receive), MESSAGE_LIMIT).await?;
        }
        write.shutdown().await
    };
    if let Err(error) = tokio::try_join!(hub.session(input, output), writer) {
        // 保留实验流失败的原因, 避免只看到对端 TLS EOF 就误判为 TLS 本身的故障.
        eprintln!("push session ended: {error}");
        return Err(error);
    }
    Ok(())
}

pub async fn connect(address: SocketAddr, auth: &Auth, grpc: bool) -> io::Result<Connection> {
    timeout(Duration::from_secs(5), connect_inner(address, auth, grpc)).await?
}

async fn connect_inner(address: SocketAddr, auth: &Auth, grpc: bool) -> io::Result<Connection> {
    let tcp = TcpStream::connect(address).await?;
    tcp.set_nodelay(true)?;
    let stream = auth.identity.connect(tcp, &address.ip().to_string()).await?;
    let info = ConnectionInfo::from_tls(&stream)?;
    let proof = auth.proof();
    let (send, mut outbound) = mpsc::channel::<Control>(8);
    let (inbound, receive) = mpsc::channel(16);
    let mut tasks = JoinSet::new();
    if grpc {
        let mut client =
            TransportProbeClient::new(bound_channel(address, stream).await?).max_encoding_message_size(MESSAGE_LIMIT).max_decoding_message_size(MESSAGE_LIMIT);
        let mut request = Request::new(ReceiverStream::new(outbound));
        request.metadata_mut().insert_bin("hello-bin", MetadataValue::from_bytes(&proof));
        let response = client.synchronize(request).await.map_err(io::Error::other)?;
        let proof = response.metadata().get_bin("hello-bin").ok_or_else(closed)?.to_bytes().map_err(io::Error::other)?;
        auth.verify(proof, &info, true)?;
        let mut stream = response.into_inner();
        tasks.spawn(async move {
            loop {
                match stream.message().await {
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
        auth.verify(read.read(HELLO_ID, HELLO_LIMIT).await?.ok_or_else(closed)?, &info, true)?;
        let failure = inbound.clone();
        tasks.spawn(async move {
            while let Some(message) = outbound.recv().await {
                let result = write.batch(CONTROL_ID, batch(message, &mut outbound).map(Ok), MESSAGE_LIMIT).await;
                if let Err(error) = result {
                    let _ = failure.send(Err(error)).await;
                    return;
                }
            }
            let _ = write.shutdown().await;
        });
        tasks.spawn(async move {
            loop {
                match read.read(EVENT_ID, MESSAGE_LIMIT).await {
                    Ok(Some(bytes)) => {
                        if inbound.send(Event::decode(bytes).map_err(io::Error::other)).await.is_err() {
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
    Ok(Connection { send, receive, _tasks: tasks })
}

fn closed() -> io::Error {
    io::Error::other("push stream closed")
}

// 最多合并 16 个已经就绪的消息, 不为了凑批次等待新消息. 队列暂空时最后一条也会及时刷新.
fn batch<T>(first: T, receive: &mut mpsc::Receiver<T>) -> impl Iterator<Item = T> {
    std::iter::once(first).chain(std::iter::from_fn(move || receive.try_recv().ok()).take(15))
}
