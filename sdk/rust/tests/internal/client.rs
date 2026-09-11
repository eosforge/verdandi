use super::*;
use tokio::net::{TcpListener, TcpStream};

async fn read(socket: &TcpStream, buffer: &mut [u8]) -> std::io::Result<usize> {
    loop {
        socket.readable().await?;
        match socket.try_read(buffer) {
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => continue,
            result => return result,
        }
    }
}

#[tokio::test]
async fn cancelling_root_open_during_handshake_closes_the_owned_socket() -> std::result::Result<(), Box<dyn std::error::Error>> {
    let listener = TcpListener::bind("127.0.0.1:0").await?;
    let mut config = Config::new(format!("redis://{}", listener.local_addr()?));
    config.connect_timeout = Duration::from_secs(5);
    let opening = tokio::spawn(Client::open(config));
    let (socket, _) = tokio::time::timeout(Duration::from_secs(2), listener.accept()).await??;
    let mut buffer = [0_u8; 4096];
    assert!(tokio::time::timeout(Duration::from_secs(2), read(&socket, &mut buffer)).await?? > 0);
    // 服务器刻意不应答 HELLO；取消恰好发生在驱动已启动、公开 Client 尚未返回时。
    opening.abort();
    match opening.await {
        Err(error) => assert!(error.is_cancelled()),
        Ok(_) => panic!("constructor completed before handshake cancellation"),
    }
    tokio::time::timeout(Duration::from_secs(2), async {
        loop {
            match read(&socket, &mut buffer).await {
                Ok(0) | Err(_) => break,
                Ok(_) => {}
            }
        }
    })
    .await?;
    Ok(())
}

#[tokio::test(flavor = "current_thread")]
async fn connection_ownership_covers_cancellation_before_the_driver_first_poll() -> std::result::Result<(), Box<dyn std::error::Error>> {
    let listener = TcpListener::bind("127.0.0.1:0").await?;
    let config = FredConfig::from_url(&format!("redis://{}", listener.local_addr()?))?;
    let client = Builder::from_config(config).build_subscriber_client()?;
    let connection = ConnectionTask::start(&client);
    drop(connection);
    tokio::task::yield_now().await;
    assert!(tokio::time::timeout(Duration::from_millis(50), listener.accept()).await.is_err());
    Ok(())
}
