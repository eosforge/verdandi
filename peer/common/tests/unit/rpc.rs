//! 真实 TLS/HTTP2 的后台 I/O 所有权, 不依赖服务拓扑来间接证明适配器行为.
use super::*;
use crate::{
    identity::Identity,
    protocol::Ping,
    test_support::{WAIT, fixture, localhost},
};
use tokio::{net::TcpListener, time};

type Session = (Streaming<SessionPacket>, mpsc::Sender<SessionPacket>, Lifetime);

async fn pair() -> io::Result<(Session, Session)> {
    let listener = TcpListener::bind(localhost()).await?;
    let address = listener.local_addr()?;
    let token = CancellationToken::new();
    let accepting = async {
        let (stream, remote) = listener.accept().await?;
        let stream = Identity::load(&fixture("peer-a"))?.accept_rpc(stream).await?;
        open(stream, remote, true, &token).await
    };
    let dialing = async {
        let stream = TcpStream::connect(address).await?;
        let stream = Identity::load(&fixture("peer-b"))?.connect_rpc(stream, "127.0.0.1").await?;
        open(stream, address, false, &token).await
    };
    time::timeout(WAIT, async { tokio::try_join!(accepting, dialing) }).await?
}

#[tokio::test]
async fn dropped_client_lifetime_closes_io_even_while_channel_handles_survive() -> io::Result<()> {
    let ((mut incoming, _outgoing, _server), (_response, sender, client)) = pair().await?;
    sender
        .send(SessionPacket {
            body: Some(crate::protocol::session_packet::Body::Ping(Ping { request_id: 1 })),
        })
        .await
        .map_err(io::Error::other)?;
    assert!(time::timeout(WAIT, incoming.message()).await?.map_err(io::Error::other)?.is_some());
    drop(client);
    assert!(matches!(time::timeout(WAIT, incoming.message()).await?, Err(_) | Ok(None)));
    Ok(())
}

#[tokio::test]
async fn dropped_server_lifetime_terminates_active_response_stream() -> io::Result<()> {
    let ((_incoming, _outgoing, server), (mut response, _sender, _client)) = pair().await?;
    drop(server);
    assert!(matches!(time::timeout(WAIT, response.message()).await?, Err(_) | Ok(None)));
    Ok(())
}

#[tokio::test]
async fn second_rpc_on_same_socket_is_rejected_after_first_stream_closes() -> io::Result<()> {
    let listener = TcpListener::bind(localhost()).await?;
    let address = listener.local_addr()?;
    let token = CancellationToken::new();
    let accepting = async {
        let (stream, remote) = listener.accept().await?;
        let stream = Identity::load(&fixture("peer-a"))?.accept_rpc(stream).await?;
        open(stream, remote, true, &token).await
    };
    let dialing = async {
        let stream = Identity::load(&fixture("peer-b"))?
            .connect_rpc(TcpStream::connect(address).await?, "127.0.0.1")
            .await?;
        let (channel, lifetime) = channel(stream, &address.to_string()).await?;
        let mut client = PeerTransportClient::new(channel);
        let (sender, receiver) = mpsc::channel(1);
        let response = client.open_session(ReceiverStream::new(receiver)).await.map_err(io::Error::other)?;
        Ok::<_, io::Error>((client, response, sender, lifetime))
    };
    let ((mut incoming, outgoing, _server), (mut client, response, sender, _client)) =
        time::timeout(WAIT, async { tokio::try_join!(accepting, dialing) }).await??;
    drop((response, sender, outgoing));
    assert!(matches!(time::timeout(WAIT, incoming.message()).await?, Err(_) | Ok(None)));
    let second = time::timeout(WAIT, client.open_session(tokio_stream::empty::<SessionPacket>())).await?;
    assert_eq!(second.err().map(|error| error.code()), Some(tonic::Code::AlreadyExists));
    Ok(())
}
