//! 真正 TLS 会话验证 Planet 单上游及恢复. 准入证书权限另由真实 Go 服务测试覆盖.
use crate::{Planet, test_support::*};
use std::{io, time::Duration};
use tokio::{net::TcpListener, time};
use verdandi_peer::Peer;

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn slow_local_candidates_cannot_starve_a_remote_star() -> io::Result<()> {
    let supervisor = Supervisor::start(Mode::Normal).await?;
    let mut stalled = Vec::new();
    for identity in ["peer-a", "peer-b"] {
        let star = Peer::start(config(identity, supervisor.address).with_group("local")).await?;
        time::timeout(WAIT, async {
            while !star.status()?.initialized {
                time::sleep(Duration::from_millis(10)).await;
            }
            Ok::<_, io::Error>(())
        })
        .await??;
        let address = star.listen_address();
        star.shutdown().await?;
        // 保留 TCP listener 却不 accept/TLS 应答. 连接成功后直到握手期限才失败.
        stalled.push(TcpListener::bind(address).await?);
    }
    let remote = Peer::start(config("peer-c", supervisor.address).with_group("remote")).await?;
    time::timeout(WAIT, async {
        while !remote.status()?.initialized {
            time::sleep(Duration::from_millis(10)).await;
        }
        Ok::<_, io::Error>(())
    })
    .await??;
    let mut settings = config("planet-a", supervisor.address).with_group("local");
    settings.handshake_timeout = Duration::from_millis(400);
    settings.reconnect_min = Duration::from_millis(10);
    settings.reconnect_max = Duration::from_millis(20);
    let planet = Planet::start(settings).await?;
    // 若只挑选“退避已到期的本组入口”, 第一个失败者在第二次握手结束前就会再次抢占.
    upstream(&planet, &remote).await?;
    assert_eq!(planet.status()?.candidates, 3);
    planet.shutdown().await?;
    remote.shutdown().await?;
    supervisor.shutdown().await?;
    drop(stalled);
    Ok(())
}

async fn upstream(planet: &Planet, star: &Peer) -> io::Result<()> {
    time::timeout(Duration::from_secs(10), async {
        loop {
            if planet
                .status()?
                .upstream
                .as_ref()
                .is_some_and(|session| session.remote.peer_id == star.peer_id())
                && star.status()?.planet_inbound == 1
            {
                return Ok(());
            }
            time::sleep(Duration::from_millis(20)).await;
        }
    })
    .await?
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn empty_candidate_list_refreshes_when_a_star_joins() -> io::Result<()> {
    let supervisor = Supervisor::start(Mode::Normal).await?;
    let planet = Planet::start(config("planet-a", supervisor.address)).await?;
    time::timeout(WAIT, async {
        while !planet.status()?.initialized {
            time::sleep(Duration::from_millis(10)).await;
        }
        Ok::<_, io::Error>(())
    })
    .await??;
    assert_eq!(planet.status()?.candidates, 0);
    let star = Peer::start(config("peer-a", supervisor.address)).await?;
    upstream(&planet, &star).await?;
    planet.shutdown().await?;
    star.shutdown().await?;
    supervisor.shutdown().await
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn local_group_then_cross_group_failover_without_supervisor() -> io::Result<()> {
    let supervisor = Supervisor::start(Mode::Normal).await?;
    let a = Peer::start(config("peer-a", supervisor.address).with_group("local")).await?;
    let b = Peer::start(config("peer-b", supervisor.address).with_group("remote")).await?;
    // Peer::start 只完成绑定, 必须等待真实 Star 网格收敛后才断开 Supervisor.
    time::timeout(WAIT, async {
        while a.status()?.outbound != 1 || b.status()?.outbound != 1 || a.status()?.inbound != 1 || b.status()?.inbound != 1 {
            time::sleep(Duration::from_millis(10)).await;
        }
        Ok::<_, io::Error>(())
    })
    .await??;
    let planet = Planet::start(config("planet-a", supervisor.address).with_group("local")).await?;
    upstream(&planet, &a).await?;
    let id = planet.peer_id().to_owned();
    assert_eq!(a.status()?.members, 2);
    assert_eq!(a.status()?.planet_inbound, 1);
    // 管理端与主用 Star 都离线, Planet 仍凭原有身份连接跨组备用入口.
    supervisor.shutdown().await?;
    a.shutdown().await?;
    upstream(&planet, &b).await?;
    assert_eq!(planet.peer_id(), id);
    assert_eq!(b.status()?.members, 2);
    assert_eq!(b.status()?.planet_inbound, 1);
    assert_eq!(planet.status()?.upstream.as_ref().map(|s| s.remote.group.as_str()), Some("remote"));
    let address = planet.listen_address();
    planet.shutdown().await?;
    b.shutdown().await?;
    drop(TcpListener::bind(address).await?);
    Ok(())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn cached_candidate_accepts_higher_star_epoch_but_no_new_planet_offline() -> io::Result<()> {
    let supervisor = Supervisor::start(Mode::Normal).await?;
    let supervisor_address = supervisor.address;
    let a = Peer::start(config("peer-a", supervisor.address)).await?;
    let planet = Planet::start(config("planet-a", supervisor.address)).await?;
    upstream(&planet, &a).await?;
    let old_id = a.peer_id().to_owned();
    let address = a.listen_address();
    a.shutdown().await?;
    let mut next = config("peer-a", supervisor.address);
    next.listen = address;
    let a = Peer::start(next).await?;
    assert_ne!(old_id, a.peer_id());
    upstream(&planet, &a).await?;
    supervisor.shutdown().await?;
    let old_planet = planet.peer_id().to_owned();
    planet.shutdown().await?;
    let next = Planet::start(config("planet-a", supervisor_address)).await?;
    assert_ne!(old_planet, next.peer_id());
    time::sleep(Duration::from_millis(250)).await;
    assert!(!next.status()?.initialized);
    assert!(next.status()?.upstream.is_none());
    let address = next.listen_address();
    time::timeout(Duration::from_secs(1), next.shutdown()).await??;
    a.shutdown().await?;
    drop(TcpListener::bind(address).await?);
    Ok(())
}
