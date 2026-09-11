//! 有界 gRPC 队列, 绝对期限与角色方向规则.
use super::write_before;
use crate::identity::ProcessIdentity;
use crate::protocol::{Ping, SessionPacket, session_packet::Body};
use std::{io, time::Duration};
use tokio::time::Instant;
#[tokio::test]
async fn full_queue_obeys_absolute_deadline() -> io::Result<()> {
    let (mut writer, _reader) = tokio::sync::mpsc::channel(1);
    writer
        .send(SessionPacket {
            body: Some(Body::Ping(Ping { request_id: 1 })),
        })
        .await
        .map_err(io::Error::other)?;
    let result = write_before(&mut writer, Body::Ping(Ping { request_id: 2 }), 32, Instant::now() + Duration::from_millis(20)).await;
    assert!(matches!(result,Err(error) if error.kind()==io::ErrorKind::TimedOut));
    Ok(())
}
#[tokio::test]
async fn expired_or_oversized_send_does_not_enqueue() {
    let (mut writer, mut reader) = tokio::sync::mpsc::channel(1);
    assert!(write_before(&mut writer, Body::Ping(Ping { request_id: 1 }), 32, Instant::now()).await.is_err());
    assert!(
        write_before(&mut writer, Body::Ping(Ping { request_id: 1 }), 1, Instant::now() + Duration::from_secs(1))
            .await
            .is_err()
    );
    assert!(reader.try_recv().is_err());
}
#[test]
fn signed_roles_enforce_connection_direction() -> io::Result<()> {
    use super::*;
    use crate::{protocol::NodeRole, test_support::*};
    use std::sync::Arc;
    for (local_role, remote_role, direction, allowed) in [
        (NodeRole::Star, NodeRole::Star, ConnectionDirection::Inbound, true),
        (NodeRole::Star, NodeRole::Star, ConnectionDirection::Outbound, true),
        (NodeRole::Star, NodeRole::Planet, ConnectionDirection::Inbound, true),
        (NodeRole::Star, NodeRole::Planet, ConnectionDirection::Outbound, false),
        (NodeRole::Planet, NodeRole::Star, ConnectionDirection::Outbound, true),
        (NodeRole::Planet, NodeRole::Star, ConnectionDirection::Inbound, false),
        (NodeRole::Planet, NodeRole::Planet, ConnectionDirection::Outbound, false),
    ] {
        let identity = Arc::new(Identity::load(&fixture("peer-a"))?);
        let process = Arc::new(ProcessIdentity::new()?);
        let mut local = member(&identity, &process, "127.0.0.1:7001".parse().map_err(io::Error::other)?, 1);
        local.role = local_role as i32;
        let remote_identity = Identity::load(&fixture("peer-b"))?;
        let remote_process = ProcessIdentity::new()?;
        let mut remote = member(&remote_identity, &remote_process, "127.0.0.1:7002".parse().map_err(io::Error::other)?, 1);
        remote.role = remote_role as i32;
        let context = SessionContext {
            local: Arc::new(signed(&local)?),
            member: Arc::new(local),
            identity,

            direction,
            generation: 1,
            handshake_timeout: Duration::from_secs(1),
            heartbeat_interval: Duration::from_secs(1),
            pong_timeout: Duration::from_secs(1),
            expected_peer_id: None,
            expected_member: None,
            cancellation: CancellationToken::new(),
            events: broadcast::channel(8).0,
        };
        let hello = signed(&remote)?;
        assert_eq!(
            validate_remote(&context, hello).is_ok(),
            allowed,
            "{local_role:?} {remote_role:?} {direction:?}"
        );
    }
    Ok(())
}
