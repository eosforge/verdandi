//! 原子名单安装, 容量与旧回调隔离.
use super::*;
use crate::RemotePeer;

fn member(index: u8) -> Member {
    Member {
        cluster_id: "alpha".into(),
        peer_id: format!("{index:08x}000040008000000000000000"),
        principal: format!("{index:064x}"),
        advertise: format!("127.0.0.1:{}", 7000 + u16::from(index)),
        epoch: 1,
        role: NodeRole::Star as i32,
        group: "default".into(),
    }
}

#[test]
fn planets_do_not_expand_mesh_or_consume_star_slots() -> io::Result<()> {
    let topology = topology(2)?;
    topology.initialize(vec![member(1), member(2)])?;
    assert_eq!(topology.take_pending()?.len(), 1);
    let mut planet = member(3);
    planet.role = NodeRole::Planet as i32;
    let lease = topology.register(snapshot(&planet, 1, ConnectionDirection::Inbound)?, CancellationToken::new(), None)?;
    assert_eq!(topology.status()?.members, 2);
    assert_eq!(topology.status()?.planet_inbound, 1);
    assert!(topology.take_pending()?.is_empty());
    assert!(
        topology
            .register(snapshot(&planet, 2, ConnectionDirection::Outbound)?, CancellationToken::new(), None)
            .is_err()
    );
    drop(lease);
    assert_eq!(topology.status()?.planet_inbound, 0);
    Ok(())
}
fn topology(maximum: usize) -> io::Result<Arc<Topology>> {
    let local = member(1);
    Ok(Arc::new(Topology::new(
        local.cluster_id,
        local.peer_id,
        local.principal,
        local.advertise.parse().map_err(io::Error::other)?,
        maximum,
        CancellationToken::new(),
    )))
}
fn snapshot(member: &Member, generation: u64, direction: ConnectionDirection) -> io::Result<ConnectionSnapshot> {
    Ok(ConnectionSnapshot {
        direction,
        generation,
        remote: RemotePeer {
            peer_id: member.peer_id.clone(),
            principal: member.principal.clone(),
            epoch: member.epoch,
            role: NodeRole::try_from(member.role).map_err(io::Error::other)?,
            group: member.group.clone(),
            advertise: member.advertise.parse().map_err(io::Error::other)?,
            protocol_minor: 0,
            max_frame_bytes: 4096,
        },
    })
}
#[test]
fn invalid_complete_lists_are_atomic() -> io::Result<()> {
    let mut wrong_self = member(1);
    wrong_self.advertise = "127.0.0.1:9999".into();
    let mut duplicate_principal = member(3);
    duplicate_principal.principal = member(2).principal;
    let mut wrong_cluster = member(3);
    wrong_cluster.cluster_id = "beta".into();
    let mut invalid_address = member(3);
    invalid_address.advertise = "0.0.0.0:1".into();
    let mut aliased_address = member(3);
    aliased_address.advertise = member(2).advertise;
    for members in [
        vec![],
        vec![member(2)],
        vec![member(2), member(1)],
        vec![member(1), member(2), member(2)],
        vec![wrong_self],
        vec![member(1), member(2), duplicate_principal],
        vec![member(1), wrong_cluster],
        vec![member(1), invalid_address],
        vec![member(1), member(2), aliased_address],
        vec![member(1), member(2), member(3), member(4)],
    ] {
        let topology = topology(3)?;
        assert!(topology.initialize(members).is_err());
        assert!(!topology.status()?.initialized);
        assert!(topology.take_pending()?.is_empty());
        topology.initialize(vec![member(1), member(2)])?;
        assert_eq!(topology.status()?.members, 2);
        assert!(topology.initialize(vec![member(1)]).is_err());
    }
    Ok(())
}
#[test]
fn restart_fences_old_tasks_and_stale_lease() -> io::Result<()> {
    let topology = topology(2)?;
    topology.initialize(vec![member(1), member(2)])?;
    let candidate = topology.take_pending()?.remove(0);
    assert!(topology.take_pending()?.is_empty());
    let old_cancel = CancellationToken::new();
    let old = topology.register(snapshot(&member(2), 1, ConnectionDirection::Inbound)?, old_cancel.clone(), None)?;
    let mut restarted = member(2);
    restarted.epoch = 2;
    restarted.peer_id = member(3).peer_id;
    let new = topology.register(snapshot(&restarted, 2, ConnectionDirection::Inbound)?, CancellationToken::new(), None)?;
    assert!(old_cancel.is_cancelled());
    assert!(candidate.cancellation.is_cancelled());
    assert!(!topology.should_dial(&candidate)?);
    drop(old);
    assert_eq!(topology.connections()?.len(), 1);
    assert_eq!(topology.connections()?[0].generation, 2);
    assert_eq!(topology.status()?.members, 2);
    assert!(
        topology
            .register(snapshot(&member(2), 3, ConnectionDirection::Inbound)?, CancellationToken::new(), None)
            .is_err()
    );
    assert!(
        topology
            .register(snapshot(&restarted, 4, ConnectionDirection::Inbound)?, CancellationToken::new(), None)
            .is_err()
    );
    assert!(
        topology
            .register(
                snapshot(&restarted, 5, ConnectionDirection::Outbound)?,
                CancellationToken::new(),
                Some(&candidate)
            )
            .is_err()
    );
    drop(new);
    assert!(topology.connections()?.is_empty());
    assert_eq!(topology.status()?.members, 2);
    Ok(())
}
#[test]
fn capacity_alias_and_uninitialized_sessions_fail_closed() -> io::Result<()> {
    let topology = topology(2)?;
    assert!(
        topology
            .register(snapshot(&member(2), 1, ConnectionDirection::Inbound)?, CancellationToken::new(), None)
            .is_err()
    );
    topology.initialize(vec![member(1), member(2)])?;
    for mut candidate in [member(1), member(3), member(4)] {
        if candidate == member(4) {
            candidate.advertise = member(2).advertise;
        }
        assert!(
            topology
                .register(snapshot(&candidate, 2, ConnectionDirection::Inbound)?, CancellationToken::new(), None)
                .is_err()
        );
    }
    let mut conflict = member(2);
    conflict.group = "changed".into();
    assert!(
        topology
            .register(snapshot(&conflict, 3, ConnectionDirection::Inbound)?, CancellationToken::new(), None)
            .is_err()
    );
    assert_eq!(topology.status()?.members, 2);
    Ok(())
}

#[test]
fn higher_epoch_cannot_move_endpoint_or_bypass_role_capacity() -> io::Result<()> {
    let topology = topology(2)?;
    topology.initialize(vec![member(1), member(2)])?;
    let old_cancel = CancellationToken::new();
    let lease = topology.register(snapshot(&member(2), 1, ConnectionDirection::Inbound)?, old_cancel.clone(), None)?;
    for field in 0..2 {
        let mut changed = member(2);
        changed.peer_id = member(3).peer_id;
        changed.epoch = 2;
        if field == 0 {
            changed.role = NodeRole::Planet as i32;
        } else {
            changed.advertise = member(3).advertise;
        }
        assert!(
            topology
                .register(snapshot(&changed, 2, ConnectionDirection::Inbound)?, CancellationToken::new(), None)
                .is_err()
        );
        assert!(!old_cancel.is_cancelled());
        assert_eq!(topology.status()?.members, 2);
        assert_eq!(topology.connections()?[0].generation, 1);
    }
    drop(lease);
    Ok(())
}
