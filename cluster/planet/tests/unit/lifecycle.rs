//! Planet 等待、根任务异常和旧租约释放都不能丢失服务所有权.
use super::*;
use crate::test_support::{WAIT, config, localhost, member};

#[tokio::test]
async fn canceling_wait_preserves_shutdown_and_port_ownership() -> io::Result<()> {
    let unavailable = TcpListener::bind(localhost()).await?;
    let mut planet = Planet::start(config("planet-a", unavailable.local_addr()?)).await?;
    let address = planet.listen_address();
    assert!(time::timeout(Duration::from_millis(30), planet.wait()).await.is_err());
    assert!(!planet.status()?.initialized);
    time::timeout(WAIT, planet.shutdown()).await??;
    drop(TcpListener::bind(address).await?);
    Ok(())
}

#[tokio::test]
async fn root_failure_is_observable_and_releases_listener() -> io::Result<()> {
    let unavailable = TcpListener::bind(localhost()).await?;
    let mut planet = Planet::start(config("planet-a", unavailable.local_addr()?)).await?;
    let address = planet.listen_address();
    if let Some(task) = &planet.task {
        task.abort();
    }
    assert!(time::timeout(WAIT, planet.wait()).await?.is_err());
    planet.shutdown().await?;
    drop(TcpListener::bind(address).await?);
    Ok(())
}

#[test]
fn refresh_keeps_observed_epoch_and_retry_quarantine_until_newer_identity() -> io::Result<()> {
    let identity = Identity::load(&crate::test_support::fixture("peer-a"))?;
    let current = member(&identity, &ProcessIdentity::new()?, SocketAddr::from(([127, 0, 0, 1], 7443)), 3);
    let next = Instant::now() + Duration::from_secs(5);
    let previous = [Candidate {
        member: current.clone(),
        failures: 7,
        next,
        quarantined: true,
        tried: true,
    }];
    for epoch in [2, 3] {
        let mut old = current.clone();
        old.epoch = epoch;
        let refreshed = candidates(vec![old], &previous);
        assert_eq!(refreshed[0].member, current);
        assert_eq!(refreshed[0].failures, 7);
        assert_eq!(refreshed[0].next, next);
        assert!(refreshed[0].quarantined);
        assert!(!refreshed[0].tried);
    }
    let mut replacement = current;
    replacement.epoch += 1;
    replacement.peer_id = ProcessIdentity::new()?.id;
    let refreshed = candidates(vec![replacement], &previous);
    assert!(!refreshed[0].quarantined);
    assert_eq!(refreshed[0].failures, 0);
    Ok(())
}

#[test]
fn stale_lease_cannot_clear_new_upstream() -> io::Result<()> {
    use verdandi_peer_common::RemotePeer;
    let state = Arc::new(Mutex::new(PlanetStatus {
        initialized: true,
        candidates: 1,
        upstream: Some(ConnectionSnapshot {
            direction: ConnectionDirection::Outbound,
            generation: 2,
            remote: RemotePeer {
                peer_id: ProcessIdentity::new()?.id,
                principal: "1".repeat(64),
                epoch: 1,
                role: NodeRole::Star,
                group: "default".into(),
                advertise: SocketAddr::from(([127, 0, 0, 1], 7443)),
                protocol_minor: 0,
                max_frame_bytes: 4096,
            },
        }),
    }));
    drop(Lease {
        state: state.clone(),
        generation: 1,
    });
    assert!(state.lock().map_err(|_| io::Error::other("test lock"))?.upstream.is_some());
    drop(Lease {
        state: state.clone(),
        generation: 2,
    });
    assert!(state.lock().map_err(|_| io::Error::other("test lock"))?.upstream.is_none());
    Ok(())
}
