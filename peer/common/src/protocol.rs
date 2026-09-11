//! 生成的 gRPC/Protobuf 类型与会话错误, 不自行实现 TCP 分帧.

use std::io;

// 生成源码随协议一同提交, 普通构建不调用 protoc. bytes 字段由独立生成器统一配置.
#[allow(missing_docs)]
mod wire {
    include!("generated/verdandi.peer.v1.rs");
    include!("generated/message_ids.rs");
}
pub use wire::{
    Hello, MessageId, NodeRole, Ping, PlanetRegistrationResponse, Pong, ProtocolError, ProtocolErrorCode, RegistrationChallenge, RegistrationRequest,
    RegistrationResponse, WireMessage, registration_response::Member,
};
pub use wire::{LoginRequest, SessionPacket, admission_client, admission_server, peer_transport_client, peer_transport_server, session_packet};

/// 握手使用独立硬上限, 防止依赖未验证 Hello 中的容量声明.
pub const MAX_HELLO_FRAME_BYTES: u32 = 4096;
/// 即使远端只需要很小的数据帧, 也必须留足完整控制帧的空间.
pub const MIN_FRAME_BYTES: u32 = 1024;
/// gRPC 与账号准入主版本, 明确拒绝旧 TCP 协议.
pub const PROTOCOL_MAJOR: u32 = 4;
/// 当前兼容次版本.
pub const PROTOCOL_MINOR: u32 = 0;

/// gRPC 状态映射为重试与诊断类别, 丢弃远端正文和 details 中可能包含的敏感材料.
/// Unknown 可由断开的 HTTP/2 I/O 产生, 不把它当作永久身份失败而隔离 Planet 候选.
pub(crate) fn rpc_error(status: tonic::Status) -> io::Error {
    use io::ErrorKind;
    use tonic::Code;
    let kind = match status.code() {
        Code::Unauthenticated | Code::PermissionDenied => ErrorKind::PermissionDenied,
        Code::AlreadyExists | Code::Aborted => ErrorKind::AlreadyExists,
        Code::Cancelled => ErrorKind::Interrupted,
        Code::Unknown | Code::Unavailable => ErrorKind::ConnectionAborted,
        Code::ResourceExhausted => ErrorKind::WouldBlock,
        Code::DeadlineExceeded => ErrorKind::TimedOut,
        _ => ErrorKind::InvalidData,
    };
    io::Error::new(kind, "gRPC request or stream failed")
}

/// 区分传输错误, 本端协议拒绝和远端拒绝, 避免对错误消息再回复错误消息.
#[derive(Debug)]
pub enum SessionError {
    /// 保留操作系统或本地超时产生的原始 I/O 类别.
    Io(io::Error),
    /// 本端发现语义或容量错误, 可以在剩余写入期限内尽力通知远端.
    Reject(ProtocolErrorCode),
    /// 远端已发送 ProtocolError, 本端直接结束连接.
    RemoteRejection,
}

impl SessionError {
    /// 返回供公开诊断使用的稳定类别, 不暴露远端字段正文.
    pub fn kind(&self) -> io::ErrorKind {
        match self {
            Self::Io(error) => error.kind(),
            Self::Reject(ProtocolErrorCode::HandshakeTimeout) => io::ErrorKind::TimedOut,
            Self::Reject(ProtocolErrorCode::Unauthorized) => io::ErrorKind::PermissionDenied,
            Self::Reject(_) | Self::RemoteRejection => io::ErrorKind::InvalidData,
        }
    }

    /// 在会话退出边界转换为库 API 的 io::Error.
    pub fn into_io(self) -> io::Error {
        match self {
            Self::Io(error) => error,
            error => io::Error::new(error.kind(), "peer protocol rejected the connection"),
        }
    }
}

impl From<io::Error> for SessionError {
    /// 允许会话 I/O 使用 `?` 原样传播传输失败.
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

#[cfg(test)]
#[path = "../tests/unit/protocol.rs"]
mod tests;
