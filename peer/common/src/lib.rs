//! Star 与 Planet 共用的认证传输和控制协议, 不持有角色拓扑或业务权威.
pub mod app;
mod codec;
pub mod config;
pub mod connection;
pub mod identity;
pub mod protocol;
pub mod registration;
pub mod retry;
mod rpc;
pub use config::PeerConfig;
pub use connection::{ConnectionDirection, ConnectionSnapshot, PeerEvent, RemotePeer};
// 夹具同时被两个角色测试引用, 各自只使用其中一部分.
#[allow(dead_code)]
#[cfg(test)]
#[path = "../tests/support/mod.rs"]
mod test_support;
