//! Planet 角色服务, 共用协议与会话机制, 独立管理单活动上游.

mod server;
pub use server::{Planet, PlanetStatus};
pub use verdandi_peer_common::PeerConfig;

#[cfg(test)]
use verdandi_peer_common::{identity, protocol};
#[cfg(test)]
#[path = "../tests/unit/network.rs"]
mod network_tests;
#[cfg(test)]
#[allow(dead_code)]
#[path = "../../common/tests/support/mod.rs"]
mod test_support;
