//! 真实 TLS/gRPC 双向流, 准入和保活边界. 字节分帧由 HTTP/2 负责.
use crate::{
    Peer,
    identity::{Identity, ProcessIdentity},
    protocol::*,
    test_support::*,
};
use session_packet::Body;
use std::{io, time::Duration};
use tokio::{
    net::{TcpListener, TcpStream},
    time,
};

struct Client {
    identity: Identity,
    hello: Hello,
    _listener: TcpListener,
}
impl Client {
    async fn new() -> io::Result<Self> {
        let identity = Identity::load(&fixture("peer-b"))?;
        let process = ProcessIdentity::new()?;
        let listener = TcpListener::bind(localhost()).await?;
        let hello = signed(&member(&identity, &process, listener.local_addr()?, 1))?;
        Ok(Self {
            identity,
            hello,
            _listener: listener,
        })
    }
    async fn open(&self, peer: &Peer) -> io::Result<RpcSession> {
        RpcSession::connect(&self.identity, peer.listen_address()).await
    }
    async fn connect(&self, peer: &Peer) -> io::Result<RpcSession> {
        let mut stream = self.open(peer).await?;
        stream.send(Body::Hello(self.hello.clone())).await?;
        assert!(matches!(stream.receive().await?, Body::Hello(_)));
        Ok(stream)
    }
}
#[tokio::test]
async fn unauthenticated_stream_does_not_disclose_bearer() -> io::Result<()> {
    let (_supervisor, peer) = start().await?;
    let client = Client::new().await?;
    let mut anonymous = client.open(&peer).await?;
    assert!(time::timeout(Duration::from_millis(50), anonymous.reader.message()).await.is_err());
    anonymous.send(Body::Ping(Ping { request_id: 1 })).await?;
    // closed 只接受拒绝/EOF, 不能先收到服务端的签名凭证.
    closed(&mut anonymous).await?;
    peer.shutdown().await
}
async fn start() -> io::Result<(Supervisor, Peer)> {
    let supervisor = Supervisor::start(Mode::Normal).await?;
    let peer = Peer::start(config("peer-a", supervisor.address)).await?;
    mesh(&[&peer]).await?;
    Ok((supervisor, peer))
}
async fn ping(stream: &mut RpcSession) -> io::Result<u64> {
    match stream.receive().await? {
        Body::Ping(p) => Ok(p.request_id),
        other => Err(io::Error::other(format!("expected ping, got {other:?}"))),
    }
}
async fn closed(stream: &mut RpcSession) -> io::Result<()> {
    time::timeout(WAIT, async {
        loop {
            match stream.reader.message().await {
                Err(_) | Ok(None) => return Ok(()),
                Ok(Some(SessionPacket {
                    body: Some(Body::Rejection(_)),
                })) => {}
                other => return Err(io::Error::other(format!("unexpected packet before close: {other:?}"))),
            }
        }
    })
    .await?
}
#[tokio::test]
async fn bidirectional_push_and_heartbeat_interleave() -> io::Result<()> {
    let (_supervisor, peer) = start().await?;
    let client = Client::new().await?;
    let mut stream = client.connect(&peer).await?;
    for id in 900..905 {
        let request_id = ping(&mut stream).await?;
        stream.send(Body::Ping(Ping { request_id: id })).await?;
        assert!(matches!(stream.receive().await?,Body::Pong(p) if p.request_id==id));
        stream.send(Body::Pong(Pong { request_id })).await?;
    }
    peer.shutdown().await?;
    closed(&mut stream).await
}
#[tokio::test]
async fn unrelated_traffic_and_stale_pong_do_not_extend_deadline() -> io::Result<()> {
    let (_supervisor, peer) = start().await?;
    let client = Client::new().await?;
    let mut stream = client.connect(&peer).await?;
    let first = ping(&mut stream).await?;
    stream.send(Body::Pong(Pong { request_id: first })).await?;
    let second = ping(&mut stream).await?;
    assert!(second > first);
    stream.send(Body::Pong(Pong { request_id: first })).await?;
    stream.send(Body::Ping(Ping { request_id: 888 })).await?;
    assert!(matches!(stream.receive().await?,Body::Pong(p) if p.request_id==888));
    closed(&mut stream).await?;
    peer.shutdown().await
}
#[tokio::test]
async fn handshake_rejects_wrong_phase_unsigned_and_oversized_messages() -> io::Result<()> {
    let (_supervisor, peer) = start().await?;
    let client = Client::new().await?;
    let mut tampered = client.hello.clone();
    tampered.admission_signature = vec![0; 64].into();
    let mut oversized = client.hello.clone();
    oversized.admission = vec![0; 8192].into();
    for body in [
        Body::Ping(Ping { request_id: 1 }),
        Body::Hello(Hello::default()),
        Body::Hello(tampered),
        Body::Hello(oversized),
    ] {
        let mut stream = client.open(&peer).await?;
        stream.send(body).await?;
        closed(&mut stream).await?;
        assert!(peer.connections()?.is_empty());
    }
    let mut empty = client.open(&peer).await?;
    empty.writer.send(SessionPacket { body: None }).await.map_err(io::Error::other)?;
    closed(&mut empty).await?;
    peer.shutdown().await
}
#[tokio::test]
async fn valid_bearer_reconnects_but_duplicate_session_is_rejected() -> io::Result<()> {
    let (_supervisor, peer) = start().await?;
    let client = Client::new().await?;
    let mut first = client.connect(&peer).await?;
    let id = ping(&mut first).await?;
    first.send(Body::Pong(Pong { request_id: id })).await?;
    let mut second = client.open(&peer).await?;
    second.send(Body::Hello(client.hello.clone())).await?;
    closed(&mut second).await?;
    drop(first);
    time::timeout(WAIT, async {
        while !peer.connections()?.is_empty() {
            time::sleep(Duration::from_millis(10)).await;
        }
        Ok::<_, io::Error>(())
    })
    .await??;
    let mut reconnected = client.connect(&peer).await?;
    assert_ne!(ping(&mut reconnected).await?, 0);
    peer.shutdown().await
}
#[tokio::test]
async fn invalid_controls_terminate_and_errors_are_not_echoed() -> io::Result<()> {
    for body in [
        Body::Hello(Hello::default()),
        Body::Ping(Ping { request_id: 0 }),
        Body::Pong(Pong { request_id: 0 }),
        Body::Rejection(ProtocolError { code: 999 }),
    ] {
        let (_supervisor, peer) = start().await?;
        let client = Client::new().await?;
        let mut stream = client.connect(&peer).await?;
        let no_echo = matches!(body, Body::Rejection(_));
        stream.send(body).await?;
        if no_echo {
            assert!(matches!(time::timeout(WAIT, stream.reader.message()).await?, Err(_) | Ok(None)));
        } else {
            closed(&mut stream).await?;
        }
        peer.shutdown().await?;
    }
    Ok(())
}
#[tokio::test]
async fn silent_tls_and_missing_hello_cannot_hold_shutdown() -> io::Result<()> {
    let (_supervisor, peer) = start().await?;
    let address = peer.listen_address();
    let _silent = TcpStream::connect(address).await?;
    let client = Client::new().await?;
    let mut partial = client.open(&peer).await?;
    time::timeout(Duration::from_secs(1), peer.shutdown()).await??;
    closed(&mut partial).await?;
    drop(TcpListener::bind(address).await?);
    Ok(())
}
#[tokio::test]
async fn full_inbound_budget_rejects_more_work_and_recovers() -> io::Result<()> {
    let supervisor = Supervisor::start(Mode::Normal).await?;
    let mut limits = config("peer-a", supervisor.address);
    limits.max_inbound_connections = 2;
    let peer = Peer::start(limits).await?;
    mesh(&[&peer]).await?;
    let mut events = peer.subscribe();
    let first = TcpStream::connect(peer.listen_address()).await?;
    let second = TcpStream::connect(peer.listen_address()).await?;
    time::sleep(Duration::from_millis(40)).await;
    let _overflow = TcpStream::connect(peer.listen_address()).await?;
    time::timeout(WAIT, async {
        loop {
            if matches!(
                events.recv().await.map_err(io::Error::other)?,
                crate::PeerEvent::ConnectionRejected {
                    error_kind: io::ErrorKind::WouldBlock,
                    ..
                }
            ) {
                return Ok::<_, io::Error>(());
            }
        }
    })
    .await??;
    drop((first, second));
    time::sleep(Duration::from_millis(40)).await;
    let client = Client::new().await?;
    let mut recovered = client.connect(&peer).await?;
    assert_ne!(ping(&mut recovered).await?, 0);
    peer.shutdown().await
}
