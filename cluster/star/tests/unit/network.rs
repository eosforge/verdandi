//! 真实 TCP/TLS 的公开 API 回归. 断言来自可重复读取的状态, 不依赖有损事件数量.
use crate::{Peer, test_support::*};
use std::{io, time::Duration};
use tokio::{net::TcpListener, time};

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn concurrent_registration_converges_and_restart_reuses_capacity() -> io::Result<()> {
    let supervisor = Supervisor::start(Mode::Normal).await?;
    let (a, b, c) = tokio::try_join!(
        Peer::start(config("peer-a", supervisor.address).with_max_peers(3)),
        Peer::start(config("peer-b", supervisor.address).with_max_peers(3)),
        Peer::start(config("peer-c", supervisor.address).with_max_peers(3))
    )?;
    mesh(&[&a, &b, &c]).await?;
    let (old_id, address) = (c.peer_id().to_owned(), c.listen_address());
    c.shutdown().await?;
    let mut next = config("peer-c", supervisor.address).with_max_peers(3);
    next.listen = address;
    let c = Peer::start(next).await?;
    assert_ne!(c.peer_id(), old_id);
    mesh(&[&a, &b, &c]).await?;
    for peer in [&a, &b] {
        assert!(peer.connections()?.iter().all(|s| s.remote.peer_id != old_id));
    }
    // 关闭登记端后继续跨多个心跳周期保活, 已初始化群组不依赖周期对账.
    supervisor.shutdown().await?;
    time::sleep(Duration::from_millis(800)).await;
    mesh(&[&a, &b, &c]).await?;
    tokio::try_join!(a.shutdown(), b.shutdown(), c.shutdown())?;
    drop(TcpListener::bind(address).await?);
    Ok(())
}

#[tokio::test]
async fn registration_response_loss_is_idempotent() -> io::Result<()> {
    let supervisor = Supervisor::start(Mode::LoseFirstResponse).await?;
    let peer = Peer::start(config("peer-a", supervisor.address)).await?;
    let id = peer.peer_id().to_owned();
    mesh(&[&peer]).await?;
    assert!(supervisor.requests()? >= 2);
    assert_eq!(peer.peer_id(), id);
    peer.shutdown().await?;
    supervisor.shutdown().await
}

#[tokio::test]
async fn offline_start_waits_and_retries_when_supervisor_arrives() -> io::Result<()> {
    let reserve = TcpListener::bind(localhost()).await?;
    let address = reserve.local_addr()?;
    drop(reserve);
    let peer = Peer::start(config("peer-a", address)).await?;
    time::sleep(Duration::from_millis(200)).await;
    assert!(!peer.status()?.initialized);
    assert!(peer.connections()?.is_empty());
    let supervisor = Supervisor::bind(address, Mode::Normal).await?;
    mesh(&[&peer]).await?;
    peer.shutdown().await?;
    supervisor.shutdown().await
}

#[tokio::test]
async fn incomplete_or_invalid_list_never_initializes() -> io::Result<()> {
    for mode in [Mode::Truncate, Mode::OmitSelf, Mode::Duplicate] {
        let supervisor = Supervisor::start(mode).await?;
        let mut peer = Peer::start(config("peer-a", supervisor.address)).await?;
        if matches!(mode, Mode::Truncate) {
            time::timeout(WAIT, async {
                while supervisor.requests()? < 2 {
                    time::sleep(Duration::from_millis(10)).await;
                }
                Ok::<_, io::Error>(())
            })
            .await??;
        } else {
            assert!(time::timeout(WAIT, peer.wait()).await?.is_err());
        }
        assert!(!peer.status()?.initialized);
        assert!(peer.connections()?.is_empty());
        peer.shutdown().await?;
        supervisor.shutdown().await?;
    }
    Ok(())
}

#[tokio::test]
async fn cancellation_and_drop_release_uninitialized_listener() -> io::Result<()> {
    // 夹具保留 TCP 监听却不完成 TLS, 覆盖停在握手中的取消.
    let blocked = TcpListener::bind(localhost()).await?;
    for explicit in [false, true] {
        let peer = Peer::start(config("peer-a", blocked.local_addr()?)).await?;
        let address = peer.listen_address();
        if explicit {
            time::timeout(Duration::from_secs(1), peer.shutdown()).await??;
        } else {
            drop(peer);
        }
        time::timeout(WAIT, async {
            loop {
                if let Ok(listener) = TcpListener::bind(address).await {
                    drop(listener);
                    return;
                }
                time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await?;
    }
    Ok(())
}

/// 中断透明 TCP 转发会破坏当前 TLS 会话, 两端必须在登记端离线时使用原身份重连.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn transport_loss_reconnects_offline_without_changing_process_identity() -> io::Result<()> {
    use tokio::{io::copy_bidirectional, net::TcpStream, sync::watch, task::JoinSet};
    use tokio_util::sync::CancellationToken;
    let supervisor = Supervisor::start(Mode::Normal).await?;
    let reservation = TcpListener::bind(localhost()).await?;
    let target = reservation.local_addr()?;
    drop(reservation);
    let listener = TcpListener::bind(localhost()).await?;
    let advertised = listener.local_addr()?;
    let cancel = CancellationToken::new();
    let child = cancel.clone();
    let (cut, cuts) = watch::channel(0_u64);
    let relay = tokio::spawn(async move {
        let mut tasks = JoinSet::new();
        loop {
            tokio::select! {
                biased;
                () = child.cancelled() => break,
                Some(_) = tasks.join_next() => {},
                accepted = listener.accept() => {
                    let (mut inbound, _) = accepted?;
                    let mut cuts = cuts.clone();
                    cuts.borrow_and_update();
                    tasks.spawn(async move {
                        let connection = async { let mut outbound = TcpStream::connect(target).await?; copy_bidirectional(&mut inbound, &mut outbound).await };
                        tokio::select! { result = connection => { let _ = result; }, _ = cuts.changed() => {} }
                    });
                }
            }
        }
        tasks.abort_all();
        while tasks.join_next().await.is_some() {}
        Ok::<_, io::Error>(())
    });
    // 失败路径也必须 abort 中继, 避免 assertion 提前退出后留下独立任务.
    struct RelayGuard(tokio::task::AbortHandle);
    impl Drop for RelayGuard {
        fn drop(&mut self) {
            self.0.abort();
        }
    }
    let _guard = RelayGuard(relay.abort_handle());
    let mut first = config("peer-a", supervisor.address).with_advertise(advertised);
    first.listen = target;
    let a = Peer::start(first).await?;
    let b = Peer::start(config("peer-b", supervisor.address)).await?;
    mesh(&[&a, &b]).await?;
    let before = b
        .connections()?
        .into_iter()
        .find(|s| s.direction == crate::ConnectionDirection::Outbound)
        .ok_or_else(|| io::Error::other("outbound session missing"))?;
    supervisor.shutdown().await?;
    cut.send_replace(1);
    time::timeout(WAIT, async {
        loop {
            if b.connections()?
                .iter()
                .any(|s| s.direction == before.direction && s.generation > before.generation && s.remote.peer_id == before.remote.peer_id)
            {
                return Ok::<_, io::Error>(());
            }
            time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await??;
    mesh(&[&a, &b]).await?;
    tokio::try_join!(a.shutdown(), b.shutdown())?;
    cancel.cancel();
    relay.await.map_err(io::Error::other)??;
    Ok(())
}
