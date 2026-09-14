//! 进程组合层: 参数,runtime,信号和日志. 网络任务始终由库 Peer 持有.

use verdandi_peer_common::app::{cli, logging, signal};

use serde_json::json;
use std::{
    io::{self, Write},
    process::ExitCode,
};
use tokio::sync::broadcast::error::RecvError;
use verdandi_peer::Peer;

/// 帮助/正常退出为 0, 参数错误为 2, 启动或运行失败为 1. 不依赖 stdin 的状态.
pub(crate) fn main() -> ExitCode {
    verdandi_peer_common::app::launch("peer", env!("CARGO_PKG_VERSION"), run)
}

/// 同时观察信号,诊断事件和根任务结果. 任一失败都先走有界关闭, 再向进程入口返回.
async fn run(options: cli::Options, output: &mut impl Write) -> io::Result<()> {
    let mut shutdown = signal::Shutdown::new()?;
    let mut peer = Peer::start(options.network).await?;
    let mut events = peer.subscribe();
    let mut result = logging::write(
        output,
        "peer",
        "INFO",
        "listening",
        json!({"address": peer.listen_address(), "peer_id": peer.peer_id(), "version": env!("CARGO_PKG_VERSION")}),
    );
    let mut status_clock = tokio::time::interval(std::time::Duration::from_secs(options.status_seconds.max(1)));
    status_clock.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    if result.is_ok() {
        result = loop {
            // 根任务完成不会被日志流或持续输入饿死, 不需要独立 reporter 任务及其额外退出通道.
            tokio::select! {
                biased;
                result = peer.wait() => break result.and_then(|()| Err(io::Error::other("peer network stopped unexpectedly"))),
                result = shutdown.wait() => break result,
                _ = status_clock.tick(), if options.status_seconds != 0 => {
                    let status = match peer.status() { Ok(status) => status, Err(error) => break Err(error) };
                    if let Err(error) = logging::write(output, "peer", "INFO", "status", json!({"peer_id": peer.peer_id(), "initialized": status.initialized,
                        "members": status.members, "inbound": status.inbound, "outbound": status.outbound,
                        "planet_inbound": status.planet_inbound})) { break Err(error); }
                }
                event = events.recv() => {
                    let logged = match event {
                        Ok(event) => logging::event(output, "peer", event),
                        Err(RecvError::Lagged(count)) => logging::write(output, "peer", "WARN", "diagnostic_lagged", json!({"count": count})),
                        Err(RecvError::Closed) => break Err(io::Error::other("peer diagnostic channel closed")),
                    };
                    if let Err(error) = logged { break Err(error); }
                }
            }
        };
    }
    // 日志错误不能跳过网络清理. timeout 丢弃 shutdown future 时, Peer::Drop 会取消并 abort 根任务.
    let stopping = logging::write(output, "peer", "INFO", "stopping", json!({}));
    let stopped = tokio::time::timeout(options.shutdown_timeout, peer.shutdown())
        .await
        .unwrap_or_else(|_| Err(io::Error::new(io::ErrorKind::TimedOut, "peer shutdown timed out")));
    result.and(stopping).and(stopped)?;
    logging::write(output, "peer", "INFO", "stopped", json!({}))
}
