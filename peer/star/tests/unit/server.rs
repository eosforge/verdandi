//! Star 根任务结果与关闭所有权. 共用退避测试由 Common 自己维护.

use super::Peer;
use std::{net::SocketAddr, time::Duration};

#[tokio::test]
async fn root_task_failure_is_observable_without_shutdown_signal() -> std::io::Result<()> {
    let mut peer = Peer::start(crate::test_support::config("peer-a", SocketAddr::from(([127, 0, 0, 1], 7442)))).await?;
    let address = peer.listen_address();
    if let Some(task) = &peer.supervisor {
        task.abort();
    }
    let result = tokio::time::timeout(Duration::from_secs(2), peer.wait()).await?;
    assert!(result.is_err());
    peer.shutdown().await?;
    let _released = tokio::net::TcpListener::bind(address).await?;
    Ok(())
}
