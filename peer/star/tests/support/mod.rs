//! 共享测试登记端, Star 网络收敛断言由本测试模块拥有.
#[allow(dead_code)]
#[path = "../../../common/tests/support/mod.rs"]
mod shared;
use crate::Peer;
pub(crate) use shared::*;
use std::{io, time::Duration};
use tokio::time;
pub(crate) async fn mesh(peers: &[&Peer]) -> io::Result<()> {
    time::timeout(WAIT, async {
        loop {
            let mut complete = true;
            for peer in peers {
                let status = peer.status()?;
                complete &= status.initialized && status.members == peers.len() && status.inbound == peers.len() - 1 && status.outbound == peers.len() - 1;
            }
            if complete {
                return Ok(());
            }
            time::sleep(Duration::from_millis(20)).await;
        }
    })
    .await?
}
