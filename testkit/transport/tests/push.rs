//! 对照传输共用同一发布源, 验证连续水位、最终缓存摘要与慢端显式失败.
use super::*;
use crate::{auth::Auth, push_measure, transport};
use std::path::PathBuf;
use tokio::net::TcpListener;
use verdandi_peer_common::protocol::NodeRole;

#[test]
fn hot_updates_retain_full_registry_and_catalog_state() -> io::Result<()> {
    let shape = Shape { registries: 10000, catalogs: 1000, hot_keys: 16, ..Shape::default() };
    shape.validate()?;
    assert_eq!(shape.state().len(), 11000);
    assert_eq!(shape.key(Domain::Catalog, 1), shape.key(Domain::Catalog, 17));
    assert_eq!(shape.key(Domain::Registry, 2), shape.key(Domain::Registry, 18));
    assert!(Shape { hot_keys: 1001, ..shape }.validate().is_err());
    Ok(())
}

#[tokio::test]
async fn publication_queued_after_drain_gets_explicit_rejection() -> io::Result<()> {
    let stop = CancellationToken::new();
    let hub = Hub::new(Config { recipients: 1, rate: 10, seconds: 0.2, mode: Mode::Catalog, ..Config::default() }, stop.clone())?;
    let (control, input) = mpsc::channel(8);
    let (output, mut receive) = mpsc::channel(16);
    let mut tasks = JoinSet::new();
    tasks.spawn(async move { hub.session(input, output).await });
    control.send(Ok(Control { command: Some(Command::Join(true)) })).await.map_err(io::Error::other)?;
    let revision = timeout(Duration::from_secs(2), async {
        loop {
            if let Some(Kind::Drained(revision)) = receive.recv().await.ok_or_else(closed)??.event {
                return Ok::<_, io::Error>(revision);
            }
        }
    })
    .await??;
    control
        .send(Ok(Control { command: Some(Command::Publish(crate::generated::Forward { request: 7, data: Bytes::from_static(b"late") })) }))
        .await
        .map_err(io::Error::other)?;
    let rejected = timeout(Duration::from_secs(2), receive.recv()).await?.ok_or_else(closed)??;
    assert_eq!(rejected.event, Some(Kind::Rejected(7)));
    control.send(Ok(Control { command: Some(Command::Progress(crate::generated::Progress { revision, r#final: true })) })).await.map_err(io::Error::other)?;
    assert!(matches!(timeout(Duration::from_secs(2), receive.recv()).await?.ok_or_else(closed)??.event, Some(Kind::Completed(_))));
    while let Some(result) = tasks.join_next().await {
        result.map_err(io::Error::other)??;
    }
    stop.cancel();
    Ok(())
}

#[tokio::test]
async fn shared_push_converges_for_both_roles_and_transports() -> io::Result<()> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../peer/tests/fixtures");
    for grpc in [false, true] {
        for role in [NodeRole::Star, NodeRole::Planet] {
            let listener = TcpListener::bind("127.0.0.1:0").await?;
            let address = listener.local_addr()?;
            let auth = Arc::new(Auth::fixture(&root, NodeRole::Star, true, grpc)?);
            let client = Arc::new(Auth::fixture(&root, role, false, grpc)?);
            let stop = CancellationToken::new();
            let hub = Hub::new(Config { recipients: 4, rate: 4000, seconds: 0.65, ..Config::default() }, stop.clone())?;
            let mut tasks = JoinSet::new();
            tasks.spawn(transport::serve_push(listener, auth, grpc, stop.clone(), Some(hub)));
            let result = push_measure::run(client, push_measure::Load { address, grpc, fanout: 4, seconds: 0.65, pause_ms: 0, shape: Shape::default() }).await;
            stop.cancel();
            while let Some(result) = tasks.join_next().await {
                result.map_err(io::Error::other)??;
            }
            let report = result?;
            assert!(report["updates_per_recipient"].as_u64().is_some_and(|n| n >= 2600));
            assert_eq!(report["per_update_ack"], false);
            assert_eq!(report["expired_latency_samples"], 0);
            assert!(report["sampled_updates"].as_u64().is_some_and(|n| n >= 80));
            assert!(report["snapshot_fragments"].as_u64().is_some_and(|n| n >= 16));
            assert!(report["control_replies"].as_u64().is_some_and(|n| n > 0));
            assert!(report["publish_replies"].as_u64().is_some_and(|n| n > 0));
        }
    }
    Ok(())
}

#[tokio::test]
async fn receiver_beyond_ring_capacity_must_request_resynchronization() -> io::Result<()> {
    let stop = CancellationToken::new();
    let hub = Hub::new(Config { recipients: 1, rate: 20000, seconds: 1.0, ..Config::default() }, stop.clone())?;
    let (control, input) = mpsc::channel(8);
    let (output, mut receive) = mpsc::channel(1);
    let mut tasks = JoinSet::new();
    tasks.spawn(async move { hub.session(input, output).await });
    control.send(Ok(Control { command: Some(Command::Join(true)) })).await.map_err(io::Error::other)?;
    tokio::time::sleep(Duration::from_millis(150)).await;
    timeout(Duration::from_secs(2), async { while receive.recv().await.is_some() {} }).await?;
    let result = tasks.join_next().await.ok_or_else(closed)?.map_err(io::Error::other)?;
    stop.cancel();
    assert!(result.is_err_and(|error| error.to_string().contains("resynchronization required")));
    Ok(())
}

#[tokio::test]
async fn catalog_push_preserves_large_registry_and_measures_slow_application() -> io::Result<()> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../peer/tests/fixtures");
    for grpc in [false, true] {
        let shape = Shape { registries: 100000, catalogs: 1000, bytes: 64, ..Shape::default() };
        let listener = TcpListener::bind("127.0.0.1:0").await?;
        let address = listener.local_addr()?;
        let auth = Arc::new(Auth::fixture(&root, NodeRole::Star, true, grpc)?);
        let client = Arc::new(Auth::fixture(&root, NodeRole::Planet, false, grpc)?);
        let stop = CancellationToken::new();
        let hub = Hub::new(Config { recipients: 2, rate: 2000, seconds: 1.0, shape, mode: Mode::Catalog, ..Config::default() }, stop.clone())?;
        let mut tasks = JoinSet::new();
        tasks.spawn(transport::serve_push(listener, auth, grpc, stop.clone(), Some(hub)));
        let result = push_measure::run(client, push_measure::Load { address, grpc, fanout: 2, seconds: 1.0, pause_ms: 200, shape }).await;
        stop.cancel();
        while let Some(result) = tasks.join_next().await {
            result.map_err(io::Error::other)??;
        }
        let report = result?;
        assert_eq!(report["registry_entries"], 100000);
        assert_eq!(report["registry_deliveries"], 0);
        assert_eq!(report["snapshot_fragments"], 0);
        assert_eq!(report["expired_latency_samples"], 0);
        assert!(report["update_p99_ms"].as_f64().is_some_and(|n| n >= 100.0));
    }
    Ok(())
}
