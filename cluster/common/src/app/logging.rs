//! 有界事件的 JSON 行日志. 输出错误向上返回, 不使用可能在 BrokenPipe 时 panic 的 println.

use crate::PeerEvent;
use serde_json::{Value, json};
use std::{
    io::{self, Write},
    time::{SystemTime, UNIX_EPOCH},
};

/// 每条日志独立编码, 不保留历史或后台发送队列; writer 由进程入口拥有.
pub fn write(output: &mut impl Write, component: &str, level: &str, event: &str, fields: Value) -> io::Result<()> {
    let record = json!({"time_unix_ms": SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis() as u64,
        "level": level, "component": component, "event": event, "fields": fields});
    serde_json::to_writer(&mut *output, &record)?;
    output.write_all(b"\n")?;
    output.flush()
}

/// 只输出已校验身份与稳定错误类别, 不记录对端原始消息或 payload.
pub fn event(output: &mut impl Write, component: &str, event: PeerEvent) -> io::Result<()> {
    let (level, name, fields) = match event {
        PeerEvent::Initialized { members } => ("INFO", "initialized", json!({"members": members})),
        PeerEvent::RegistrationRetry { error_kind } => ("WARN", "registration_retry", json!({"error_kind": format!("{error_kind:?}")})),
        PeerEvent::Connected { direction, generation, remote } => (
            "INFO",
            "connected",
            json!({"direction": format!("{direction:?}"), "generation": generation, "peer_id": remote.peer_id, "advertise": remote.advertise, "role": format!("{:?}", remote.role), "group": remote.group}),
        ),
        PeerEvent::Disconnected {
            direction,
            generation,
            remote,
            error_kind,
        } => (
            "INFO",
            "disconnected",
            json!({"direction": format!("{direction:?}"), "generation": generation, "peer_id": remote.peer_id, "error_kind": format!("{error_kind:?}")}),
        ),
        PeerEvent::ConnectFailed { address, error_kind } => ("WARN", "connect_failed", json!({"address": address, "error_kind": format!("{error_kind:?}")})),
        PeerEvent::ConnectionRejected {
            direction,
            address,
            error_kind,
        } => (
            "WARN",
            "connection_rejected",
            json!({"direction": format!("{direction:?}"), "address": address, "error_kind": format!("{error_kind:?}")}),
        ),
    };
    write(output, component, level, name, fields)
}
