//! 命令行只负责配置, 不生成身份或启动任务. 注入缓存由项目包装脚本负责.
use crate::PeerConfig;
use std::{net::SocketAddr, time::Duration};
#[cfg(test)]
#[path = "../../tests/unit/cli.rs"]
mod tests;

/// 经命令行解析的节点进程配置, 不创建网络或读取身份文件.
pub struct Options {
    /// 节点网络参数.
    pub network: PeerConfig,
    /// Tokio 工作线程数, 默认 2, 范围 1..64.
    pub workers: usize,
    /// 有界关闭期限, 默认 5 秒, 范围 1..60 秒.
    pub shutdown_timeout: Duration,
    /// 默认零关闭周期快照, 1..3600 秒用于显式诊断和可重复的系统测试观测.
    pub status_seconds: u64,
}
impl Options {
    /// 消费拥有的参数字符串, 帮助返回 None; 校验失败时不启动 runtime 或读取身份文件.
    pub fn parse(arguments: impl IntoIterator<Item = String>) -> Result<Option<Self>, String> {
        let (mut cluster, mut listen, mut advertise, mut supervisor, mut identity, mut group) = (None, None, None, None, None, None);
        let (mut workers, mut shutdown, mut maximum, mut status, mut heartbeat, mut pong) = (None, None, None, None, None, None);
        let mut arguments = arguments.into_iter();
        while let Some(argument) = arguments.next() {
            if argument == "--help" || argument == "-h" {
                return Ok(None);
            }
            let (option, inline) = argument.split_once('=').map_or((argument.as_str(), None), |(key, value)| (key, Some(value)));
            if !matches!(
                option,
                "--cluster"
                    | "--listen"
                    | "--advertise"
                    | "--super"
                    | "--identity"
                    | "--group"
                    | "--max-peers"
                    | "--worker-threads"
                    | "--shutdown-timeout-seconds"
                    | "--status-interval-seconds"
                    | "--heartbeat-interval-ms"
                    | "--pong-timeout-ms"
            ) {
                return Err(format!("unknown option: {option}"));
            }
            let value = inline
                .map(str::to_owned)
                .or_else(|| arguments.next())
                .filter(|v| !v.is_empty() && !v.starts_with("--"))
                .ok_or_else(|| format!("missing value for {option}"))?;
            match option {
                "--group" => set_once(&mut group, value, option)?,
                "--cluster" => set_once(&mut cluster, value, option)?,
                "--listen" => set_once(&mut listen, address(&value, option)?, option)?,
                "--advertise" => set_once(&mut advertise, address(&value, option)?, option)?,
                "--super" => set_once(&mut supervisor, value, option)?,
                "--identity" => set_once(&mut identity, value, option)?,
                "--max-peers" => set_once(&mut maximum, number(&value, option, 1, 4096)?, option)?,
                "--worker-threads" => set_once(&mut workers, number(&value, option, 1, 64)?, option)?,
                "--shutdown-timeout-seconds" => set_once(&mut shutdown, number(&value, option, 1, 60)?, option)?,
                "--status-interval-seconds" => set_once(&mut status, number(&value, option, 0, 3600)?, option)?,
                "--heartbeat-interval-ms" => set_once(&mut heartbeat, number(&value, option, 1, 86_400_000)?, option)?,
                "--pong-timeout-ms" => set_once(&mut pong, number(&value, option, 1, 86_400_000)?, option)?,
                _ => unreachable!("option validated above"),
            }
        }
        let mut network = PeerConfig::new(
            cluster.ok_or("missing --cluster")?,
            listen.ok_or("missing --listen")?,
            supervisor.ok_or("missing --super")?,
            identity.unwrap_or_else(|| "identity".into()),
        )
        .with_group(group.unwrap_or_else(|| "default".into()))
        .with_max_peers(maximum.unwrap_or(64) as usize)
        .with_heartbeat(Duration::from_millis(heartbeat.unwrap_or(10_000)), Duration::from_millis(pong.unwrap_or(5000)));
        if let Some(address) = advertise {
            network = network.with_advertise(address);
        }
        network.validate().map_err(|error| error.to_string())?;
        Ok(Some(Self {
            network,
            workers: workers.unwrap_or(2) as usize,
            shutdown_timeout: Duration::from_secs(shutdown.unwrap_or(5)),
            status_seconds: status.unwrap_or(0),
        }))
    }
}
/// 每个选项只允许一次, 避免重复安全参数被静默覆盖.
fn set_once<T>(destination: &mut Option<T>, value: T, option: &str) -> Result<(), String> {
    if destination.replace(value).is_some() {
        return Err(format!("duplicate {option}"));
    }
    Ok(())
}
/// 仅解析数值 SocketAddr, 不在 CLI 阶段产生 DNS 或网络访问.
fn address(value: &str, option: &str) -> Result<SocketAddr, String> {
    value.parse().map_err(|_| format!("invalid address for {option}"))
}
/// 显式单位和上下界由调用点提供, 零不自动表示无限制.
fn number(value: &str, option: &str, minimum: u64, maximum: u64) -> Result<u64, String> {
    value
        .parse()
        .ok()
        .filter(|value| (minimum..=maximum).contains(value))
        .ok_or_else(|| format!("{option} must be between {minimum} and {maximum}"))
}
/// 两种节点共用的选项说明, 可执行入口替换程序名.
pub const HELP: &str = "Usage: peer --listen=IP:PORT --super=HOST:PORT --cluster=ID [options]\n\
    --identity=identity         Directory: ca.pem, cert.pem, key.pem, admission.pub, login.json\n\
    --advertise=IP:PORT          Reachable address for wildcard listeners\n\
    --group=default             Preferred connection group, not a data partition\n\
    --max-peers=64               Cluster capacity including self [1, 4096]\n\
    --heartbeat-interval-ms=10000 / --pong-timeout-ms=5000\n\
    --worker-threads=2           Tokio workers [1, 64]\n\
    --shutdown-timeout-seconds=5 Graceful shutdown limit [1, 60]\n\
    --status-interval-seconds=0  Periodic actual-state snapshots, 0 disables [0, 3600]\n\
    --help / --version          Show help or build version\n\
    A fresh UUID is generated at startup. TLS and Supervisor account login are required.\n\
    Stop with Ctrl+C or SIGTERM. stdin is not a control channel.\n";
