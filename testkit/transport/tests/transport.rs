//! 对两种传输执行同一认证、取消、半关闭和消息边界检查.
use super::*;
use prost::bytes::Bytes;
use std::path::PathBuf;
use verdandi_peer_common::protocol::NodeRole;

struct Fixture {
    address: SocketAddr,
    auth: Arc<Auth>,
    stop: CancellationToken,
    task: tokio::task::JoinHandle<io::Result<()>>,
}

impl Fixture {
    async fn start(grpc: bool, role: NodeRole) -> io::Result<Self> {
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../peer/tests/fixtures");
        let server = Arc::new(Auth::fixture(&root, NodeRole::Star, true, grpc)?);
        let auth = Arc::new(Auth::fixture(&root, role, false, grpc)?);
        let listener = TcpListener::bind("127.0.0.1:0").await?;
        let address = listener.local_addr()?;
        let stop = CancellationToken::new();
        let task = tokio::spawn(serve(listener, server, grpc, stop.clone()));
        Ok(Self { address, auth, stop, task })
    }

    async fn finish(&mut self) -> io::Result<()> {
        self.stop.cancel();
        timeout(DEADLINE, &mut self.task).await?.map_err(io::Error::other)??;
        // 监听端口已释放. TIME_WAIT 不妨碍客户端确认不能建立新的服务连接.
        assert!(TcpStream::connect(self.address).await.is_err());
        Ok(())
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        self.stop.cancel();
        self.task.abort();
    }
}

#[tokio::test]
async fn both_roles_exchange_exact_payload_and_half_close() -> io::Result<()> {
    for grpc in [false, true] {
        for role in [NodeRole::Star, NodeRole::Planet] {
            let mut fixture = Fixture::start(grpc, role).await?;
            let mut session = connect(fixture.address, &fixture.auth, grpc).await?;
            for size in [0, 256, 1024, 16384] {
                let message = Transfer { sequence: size as u64, body: Bytes::from(vec![42; size]) };
                session.send.send(message.clone()).await.map_err(io::Error::other)?;
                assert_eq!(timeout(DEADLINE, session.receive.recv()).await?.ok_or_else(eof)??, message);
            }
            drop(session.send);
            assert!(timeout(DEADLINE, session.receive.recv()).await?.is_none());
            fixture.finish().await?;
        }
    }
    Ok(())
}

#[tokio::test]
async fn bearer_credential_is_reusable_on_new_connection() -> io::Result<()> {
    for grpc in [false, true] {
        let mut fixture = Fixture::start(grpc, NodeRole::Planet).await?;
        let old = fixture.auth.identity.connect(TcpStream::connect(fixture.address).await?, "127.0.0.1").await?;
        let proof = fixture.auth.proof();
        drop(old);
        assert!(timeout(DEADLINE, connect_inner(fixture.address, &fixture.auth, grpc, Some(proof))).await?.is_ok());
        // 一次恶意请求不能破坏监听器或阻止之后合法会话.
        let session = connect(fixture.address, &fixture.auth, grpc).await?;
        drop(session);
        fixture.finish().await?;
    }
    Ok(())
}

#[tokio::test]
async fn missing_and_oversized_proofs_are_rejected() -> io::Result<()> {
    for grpc in [false, true] {
        let mut fixture = Fixture::start(grpc, NodeRole::Star).await?;
        for proof in [Bytes::new(), Bytes::from(vec![0; HELLO_LIMIT + 1])] {
            assert!(timeout(DEADLINE, connect_inner(fixture.address, &fixture.auth, grpc, Some(proof))).await?.is_err());
        }
        fixture.finish().await?;
    }
    Ok(())
}

#[tokio::test]
async fn application_and_codec_limits_reject_oversized_data() -> io::Result<()> {
    for grpc in [false, true] {
        let mut fixture = Fixture::start(grpc, NodeRole::Star).await?;
        for size in [16385, MESSAGE_LIMIT + 1] {
            let mut session = connect(fixture.address, &fixture.auth, grpc).await?;
            session.send.send(Transfer { sequence: 1, body: Bytes::from(vec![1; size]) }).await.map_err(io::Error::other)?;
            assert!(!matches!(timeout(DEADLINE, session.receive.recv()).await?, Some(Ok(_))));
        }
        fixture.finish().await?;
    }
    Ok(())
}

#[tokio::test]
async fn cancellation_and_slow_reader_release_session() -> io::Result<()> {
    for grpc in [false, true] {
        let mut fixture = Fixture::start(grpc, NodeRole::Planet).await?;
        for _ in 0..20 {
            let session = connect(fixture.address, &fixture.auth, grpc).await?;
            drop(session);
        }
        let session = connect(fixture.address, &fixture.auth, grpc).await?;
        let body = Bytes::from(vec![1; 16384]);
        // 不读取应答, 必须在有界缓冲耗尽后反压. 100 ms 后取消仍能退出.
        let blocked = timeout(Duration::from_millis(100), async {
            for sequence in 0..100000 {
                session.send.send(Transfer { sequence, body: body.clone() }).await.map_err(io::Error::other)?;
            }
            Ok::<_, io::Error>(())
        })
        .await;
        assert!(blocked.is_err());
        drop(session);
        fixture.finish().await?;
    }
    Ok(())
}

#[tokio::test]
async fn stopped_server_terminates_active_stream() -> io::Result<()> {
    for grpc in [false, true] {
        let mut fixture = Fixture::start(grpc, NodeRole::Star).await?;
        let mut session = connect(fixture.address, &fixture.auth, grpc).await?;
        fixture.finish().await?;
        assert!(!matches!(timeout(DEADLINE, session.receive.recv()).await?, Some(Ok(_))));
    }
    Ok(())
}

#[tokio::test]
async fn tcp_rejects_truncated_wrong_id_and_excessive_length() -> io::Result<()> {
    for bytes in [vec![0], vec![0, 2, 0, 0, 0, 4, 1], vec![0, 3, 0, 0, 0, 0], vec![0, 2, 255, 255, 255, 255]] {
        let (mut write, read) = tokio::io::duplex(64);
        use tokio::io::AsyncWriteExt;
        write.write_all(&bytes).await?;
        write.shutdown().await?;
        assert!(Reader::new(read).read(TRANSFER_ID, MESSAGE_LIMIT).await.is_err());
    }
    Ok(())
}

#[tokio::test]
async fn tls_without_application_authentication_expires() -> io::Result<()> {
    use tokio::io::AsyncReadExt;
    for grpc in [false, true] {
        let mut fixture = Fixture::start(grpc, NodeRole::Star).await?;
        let mut stream = fixture.auth.identity.connect(TcpStream::connect(fixture.address).await?, "127.0.0.1").await?;
        // 可以收到 HTTP/2 settings, 但不发送 Hello/RPC. 两种传输都应回收未认证连接.
        timeout(DEADLINE + Duration::from_secs(2), async {
            let mut bytes = [0; 4096];
            loop {
                match stream.read(&mut bytes).await {
                    Ok(0) | Err(_) => break,
                    Ok(_) => {}
                }
            }
        })
        .await?;
        fixture.finish().await?;
    }
    Ok(())
}

#[tokio::test]
async fn a_planet_cannot_impersonate_the_upstream_star() -> io::Result<()> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../peer/tests/fixtures");
    for grpc in [false, true] {
        let server = Arc::new(Auth::fixture(&root, NodeRole::Planet, true, grpc)?);
        let client = Auth::fixture(&root, NodeRole::Planet, false, grpc)?;
        let listener = TcpListener::bind("127.0.0.1:0").await?;
        let address = listener.local_addr()?;
        let stop = CancellationToken::new();
        let task = tokio::spawn(serve(listener, server, grpc, stop.clone()));
        let result = connect(address, &client, grpc).await;
        stop.cancel();
        task.await.map_err(io::Error::other)??;
        assert!(result.is_err());
    }
    Ok(())
}
