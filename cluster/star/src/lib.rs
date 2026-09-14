//! Star 服务负责成员网络与连接调度, 认证传输由 common 复用.
mod server;
mod topology;
pub use server::Peer;
pub use topology::NetworkStatus;
pub use verdandi_peer_common::{ConnectionDirection, ConnectionSnapshot, PeerConfig, PeerEvent, RemotePeer};
use verdandi_peer_common::{config, connection, identity, protocol, registration};
#[cfg(test)]
#[path = "../tests/unit/network.rs"]
mod network_tests;
#[cfg(test)]
#[path = "../tests/support/mod.rs"]
mod test_support;
#[cfg(test)]
#[path = "../tests/unit/wire_network.rs"]
mod wire_tests;
