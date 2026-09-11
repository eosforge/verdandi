//! 启动参数和有界时间输入, 不访问网络或下载工具.
use super::*;
use crate::test_support::{config, localhost};
#[test]
fn supervisor_endpoint_and_capacity_are_bounded() {
    for value in [
        "",
        "127.0.0.1:0",
        "0.0.0.0:80",
        "[::]:80",
        "bad/name:443",
        "host:65536",
        "-host:80",
        "host..test:80",
    ] {
        assert!(SupervisorAddress::parse(value).is_err(), "accepted {value}");
    }
    for value in ["127.0.0.1:443", "[::1]:443", "supervisor.example:443"] {
        assert!(SupervisorAddress::parse(value).is_ok());
    }
    let mut valid = config("peer-a", SocketAddr::from(([127, 0, 0, 1], 7442)));
    assert!(valid.validate().is_ok());
    for maximum in [0, 4097, usize::MAX] {
        assert!(valid.clone().with_max_peers(maximum).validate().is_err());
    }
    for duration in [Duration::ZERO, Duration::from_nanos(1), Duration::from_secs(86401), Duration::MAX] {
        assert!(valid.clone().with_heartbeat(duration, Duration::from_secs(1)).validate().is_err());
        assert!(valid.clone().with_heartbeat(Duration::from_secs(1), duration).validate().is_err());
    }
    valid.listen = SocketAddr::from(([0, 0, 0, 0], 0));
    assert!(valid.validate().is_err());
    assert!(valid.with_advertise(SocketAddr::from(([127, 0, 0, 1], 7001))).validate().is_ok());
    assert!(validate_remote_address("test", localhost()).is_err());
}

#[test]
fn every_timer_rejects_unrepresentable_or_unbounded_input() {
    let setters: [fn(&mut PeerConfig, Duration); 7] = [
        |c, v| c.connect_timeout = v,
        |c, v| c.handshake_timeout = v,
        |c, v| c.heartbeat_interval = v,
        |c, v| c.pong_timeout = v,
        |c, v| c.reconnect_min = v,
        |c, v| c.reconnect_max = v,
        |c, v| c.stable_connection = v,
    ];
    for (index, set) in setters.into_iter().enumerate() {
        for duration in [Duration::ZERO, Duration::from_nanos(1), Duration::from_secs(86401), Duration::MAX] {
            let mut settings = config("peer-a", SocketAddr::from(([127, 0, 0, 1], 7442)));
            set(&mut settings, duration);
            assert_eq!(settings.validate().err().map(|e| e.kind()), Some(io::ErrorKind::InvalidInput), "timer {index}");
        }
    }
}

#[test]
fn resource_constructor_limits_are_checked_before_runtime_allocation() {
    type SetCapacity = fn(&mut PeerConfig, usize);
    let setters: [(SetCapacity, usize); 3] = [
        (|c, v| c.max_inbound_connections = v, 65536),
        (|c, v| c.max_concurrent_dials = v, 4096),
        (|c, v| c.event_capacity = v, 65536),
    ];
    for (set, maximum) in setters {
        for value in [0, maximum + 1, usize::MAX] {
            let mut settings = config("peer-a", SocketAddr::from(([127, 0, 0, 1], 7442)));
            set(&mut settings, value);
            assert!(settings.validate().is_err(), "accepted capacity {value}");
        }
        for value in [1, maximum] {
            let mut settings = config("peer-a", SocketAddr::from(([127, 0, 0, 1], 7442)));
            set(&mut settings, value);
            assert!(settings.validate().is_ok(), "rejected capacity {value}");
        }
    }
}

#[test]
fn timer_extremes_and_retry_relationship_remain_usable() {
    for duration in [Duration::from_millis(1), Duration::from_secs(86400)] {
        let mut settings = config("peer-a", SocketAddr::from(([127, 0, 0, 1], 7442)));
        settings.connect_timeout = duration;
        settings.handshake_timeout = duration;
        settings.heartbeat_interval = duration;
        settings.pong_timeout = duration;
        settings.reconnect_min = duration;
        settings.reconnect_max = duration;
        settings.stable_connection = duration;
        assert!(settings.validate().is_ok());
        assert!(std::time::Instant::now().checked_add(duration).is_some());
        settings.reconnect_min = duration + Duration::from_millis(1);
        assert!(settings.validate().is_err());
    }
}
