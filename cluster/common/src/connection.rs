//! 单条 TCP 会话的握手, 身份校验和关闭流程.
//!
//! 新 socket 在本模块中仍是不可信输入. 只有 Hello 完整解码并通过集群,
//! Peer ID 和地址校验后, 才会生成 `Connected` 事件和 `RemotePeer`.

use std::{io, net::SocketAddr, sync::Arc, time::Duration};

use tokio::{
    net::TcpStream,
    sync::{broadcast, mpsc},
    time::{self, Instant},
};
use tokio_util::sync::CancellationToken;

use crate::{
    identity::Identity,
    protocol::{
        Hello, MAX_HELLO_FRAME_BYTES, MIN_FRAME_BYTES, Member, PROTOCOL_MAJOR, Ping, Pong, ProtocolError, ProtocolErrorCode, SessionError, SessionPacket,
        session_packet::Body,
    },
};

#[cfg(test)]
#[path = "../tests/unit/connection.rs"]
mod tests;

/// 当前 TCP 会话相对本 Peer 的建立方向.
///
/// `Copy` 表示该小枚举按值复制没有独立资源. 将它传给事件时不会移动或释放连接.
#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum ConnectionDirection {
    /// 由本 Peer 主动拨号建立.
    Outbound,
    /// 由本 Peer listener 接受.
    Inbound,
}

/// 完成 Hello 校验后的远端身份快照.
///
/// 该值拥有其中的 `String`, 因而不借用临时接收缓冲区. `Clone` 用于让连接事件和
/// 当前会话各自保存一份稳定描述, 不共享可变状态.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RemotePeer {
    /// 远端本次进程 UUID, 已验证 Supervisor 准入与进程私钥持有证明.
    pub peer_id: String,
    /// Supervisor 授权的部署证书指纹.
    pub principal: String,
    /// 同一部署身份当前的进程代次.
    pub epoch: u64,
    /// Supervisor 已签名的节点角色.
    pub role: crate::protocol::NodeRole,
    /// Supervisor 已签名的连接偏好组, 不用于授权 scope.
    pub group: String,
    /// 远端声明供其他 Peer 主动连接的地址.
    pub advertise: SocketAddr,
    /// 远端当前协议 minor; Core 只在相同 major 内协商能力.
    pub protocol_minor: u32,
    /// 远端声明的单帧字节上限.
    pub max_frame_bytes: u32,
}

/// 已认证会话的快照. 实际登记和 RAII 清理由各角色拥有.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ConnectionSnapshot {
    /// 本地连接方向.
    pub direction: ConnectionDirection,
    /// 本进程会话代次.
    pub generation: u64,
    /// 已认证的远端身份.
    pub remote: RemotePeer,
}

/// 基础网络生命周期产生的有界诊断事件.
///
/// 事件通道只用于观测和测试; 接收者落后时会丢失旧事件, 不参与连接正确性.
/// 每个变体都拥有自己的数据, 因而事件可以安全地跨异步任务发送.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PeerEvent {
    /// 已取得并完整安装 Supervisor 名单, 不等同于所有连接已经建立.
    Initialized {
        /// 完整名单中的节点数, 包含自身.
        members: usize,
    },
    /// 登记连接暂时失败, 保留 UUID 和原请求执行有界重试.
    RegistrationRetry {
        /// 有限错误类别, 不暴露身份材料.
        error_kind: io::ErrorKind,
    },
    /// TLS 与 Hello 认证均完成, 远端身份可以用于后续 session registry.
    Connected {
        /// 本地会话方向.
        direction: ConnectionDirection,
        /// 当前 Peer 实例内单调递增的会话 generation.
        generation: u64,
        /// 经过校验的远端描述.
        remote: RemotePeer,
    },
    /// 已验证会话关闭; 该 generation 不再允许更新未来状态.
    Disconnected {
        /// 本地会话方向.
        direction: ConnectionDirection,
        /// 被关闭的会话 generation.
        generation: u64,
        /// 已验证的远端描述.
        remote: RemotePeer,
        /// 远端关闭或本地取消时为空, I/O/协议失败时给出稳定错误类别.
        error_kind: Option<io::ErrorKind>,
    },
    /// 主动连接在 TCP 建立前失败; supervisor 会执行有界退避.
    ConnectFailed {
        /// 本次拨号目标.
        address: SocketAddr,
        /// 不包含系统错误正文的稳定类别.
        error_kind: io::ErrorKind,
    },
    /// TCP 已建立, 但容量或 Hello 校验失败, 连接不会进入已验证集合.
    ConnectionRejected {
        /// 本地会话方向.
        direction: ConnectionDirection,
        /// TCP 对端地址; 身份未验证时不能使用 advertise 代替.
        address: SocketAddr,
        /// 不包含远端原始字段的稳定类别.
        error_kind: io::ErrorKind,
    },
}

/// 一条会话运行期间共享且不可变的本地上下文.
///
/// session task 独占该值, 因此 generation, 取消边界和事件目标不会在异步执行中被替换.
/// 把相关参数封装成结构体也让调用点明确交出这些句柄的所有权.
pub struct SessionContext {
    /// 所有会话共享的本地 Hello. 克隆 `Arc` 只增加引用计数, 不复制字符串内容.
    pub local: Arc<Hello>,
    /// 当前连接由拨号还是 accept 建立.
    pub direction: ConnectionDirection,
    /// 当前 Peer 实例为此会话分配的防旧任务编号.
    pub generation: u64,
    /// 整个 Hello 交换的总时限.
    pub handshake_timeout: Duration,
    /// 握手成功或匹配 Pong 后, 等待多久发送下一次 Ping.
    pub heartbeat_interval: Duration,
    /// Ping 从开始写入到收到匹配 Pong 的总时限.
    pub pong_timeout: Duration,
    /// 已签名的本端成员记录和部署身份, 所有连接只读共享.
    pub member: Arc<Member>,
    /// 部署安全配置只读共享, 每条连接都验证证书.
    pub identity: Arc<Identity>,
    /// Planet 验证候选部署身份, 允许同部署更高 epoch 的 Star 重启.
    pub expected_member: Option<Member>,
    /// 候选的预期进程 UUID, None 只用于尚未识别的入站连接.
    pub expected_peer_id: Option<String>,
    /// 只控制当前会话的取消令牌. 父令牌取消会传播到该子令牌.
    pub cancellation: CancellationToken,
    /// 有界诊断事件发送端. 发送失败不会改变会话结果.
    pub events: broadcast::Sender<PeerEvent>,
}

/// 完成可取消的握手和默认保活. 调用方拥有 task, 并负责等待函数结束.
pub async fn run_session<L: Send>(
    stream: TcpStream,
    source_address: SocketAddr,
    context: SessionContext,
    register: impl FnOnce(ConnectionSnapshot, CancellationToken) -> io::Result<L> + Send,
) -> io::Result<()> {
    let handshake_deadline = Instant::now() + context.handshake_timeout;
    stream.set_nodelay(true)?;
    // TLS 和应用握手共用一个总期限. 未完成 TLS 的连接不能开始应用握手.
    let secured = tokio::select! {
        biased;
        () = context.cancellation.cancelled() => return Ok(()),
        result = time::timeout_at(handshake_deadline, async {
            match context.direction {
                ConnectionDirection::Inbound => context.identity.accept_rpc(stream).await,
                ConnectionDirection::Outbound => context.identity.connect_rpc(stream, &source_address.ip().to_string()).await,
            }
        }) => result.unwrap_or_else(|_| Err(io::Error::new(io::ErrorKind::TimedOut, "TLS handshake timed out"))),
    };
    let stream = match secured {
        Ok(stream) => stream,
        Err(error) => {
            emit(
                &context.events,
                PeerEvent::ConnectionRejected {
                    direction: context.direction,
                    address: source_address,
                    error_kind: error.kind(),
                },
            );
            return Err(error);
        }
    };
    // TLS, HTTP/2 建链和双方 Hello 共用总期限. 每个物理连接只开放一个双向 RPC.
    let opened = tokio::select! {
        biased;
        () = context.cancellation.cancelled() => return Ok(()),
        result = time::timeout_at(handshake_deadline,
            crate::rpc::open(stream, source_address, context.direction == ConnectionDirection::Inbound, &context.cancellation)) => result,
    };
    let (mut reader, mut writer, _lifetime) = match opened {
        Ok(Ok(opened)) => opened,
        result => {
            let error = match result {
                Ok(Err(error)) => error,
                _ => io::Error::new(io::ErrorKind::TimedOut, "gRPC handshake timed out"),
            };
            emit(
                &context.events,
                PeerEvent::ConnectionRejected {
                    direction: context.direction,
                    address: source_address,
                    error_kind: error.kind(),
                },
            );
            return Err(error);
        }
    };

    // 取消覆盖 Hello 入队, gRPC 读取和字段校验. biased 在同时就绪时优先处理关闭.
    let result = tokio::select! {
        biased;
        () = context.cancellation.cancelled() => return Ok(()),
        result = time::timeout_at(handshake_deadline, handshake(&mut reader, &mut writer, &context)) => {
            result.unwrap_or(Err(SessionError::Reject(ProtocolErrorCode::HandshakeTimeout)))
        }
    };
    let remote = match result {
        Ok(remote) => remote,
        Err(error) => {
            emit(
                &context.events,
                PeerEvent::ConnectionRejected {
                    direction: context.direction,
                    address: source_address,
                    error_kind: error.kind(),
                },
            );
            // 错误通知沿用握手剩余期限, 不在握手超时后额外等待一次完整写超时.
            send_rejection(&mut writer, &error, handshake_deadline, MAX_HELLO_FRAME_BYTES, &context.cancellation).await;
            return Err(error.into_io());
        }
    };
    // 握手完成和取消可能同时发生. 已请求关闭时不再发布一个新的可用会话.
    if context.cancellation.is_cancelled() {
        return Ok(());
    }
    // 只有直接握手才能激活候选. 与转述 ID 不符时不把该地址悄悄登记为另一个节点.
    let registered = if context.expected_peer_id.as_ref().is_some_and(|expected| expected != &remote.peer_id) {
        Err(io::Error::new(io::ErrorKind::InvalidData, "discovered peer ID mismatch"))
    } else {
        register(
            ConnectionSnapshot {
                direction: context.direction,
                generation: context.generation,
                remote: remote.clone(),
            },
            context.cancellation.clone(),
        )
    };
    let lease = match registered {
        Ok(lease) => lease,
        Err(error) => {
            emit(
                &context.events,
                PeerEvent::ConnectionRejected {
                    direction: context.direction,
                    address: source_address,
                    error_kind: error.kind(),
                },
            );
            return Err(error);
        }
    };
    if context.direction == ConnectionDirection::Inbound {
        // 重复方向、容量和代次检查也通过后才披露本端凭证. 入队失败时 lease 自动撤销本次安装.
        tokio::select! {
            biased;
            () = context.cancellation.cancelled() => return Ok(()),
            result = write_before(&mut writer, Body::Hello((*context.local).clone()), MAX_HELLO_FRAME_BYTES, handshake_deadline) => {
                result.map_err(SessionError::into_io)?;
            }
        }
    }
    emit(
        &context.events,
        PeerEvent::Connected {
            direction: context.direction,
            generation: context.generation,
            remote: remote.clone(),
        },
    );

    // 双方使用较小的接收上限. 取消保活 future 后直接关闭整个 RPC, 不在失败流上恢复读取.
    let maximum = context.local.max_frame_bytes.min(remote.max_frame_bytes).min(MAX_HELLO_FRAME_BYTES);
    let result = tokio::select! {
        biased;
        () = context.cancellation.cancelled() => Ok(()),
        result = keep_alive(&mut reader, &mut writer, &context, maximum) => result,
    };
    if let Err(error) = &result {
        // 只通知本端协议拒绝. 传输失败或队列超时直接关闭 RPC, 不发送额外错误响应.
        let deadline = Instant::now() + context.pong_timeout.min(Duration::from_millis(250));
        send_rejection(&mut writer, error, deadline, maximum, &context.cancellation).await;
    }
    // 先归还会话登记. watch 唤醒等待该 ID 的拨号任务, 旧 generation 不能删除新登记.
    drop(lease);
    emit(
        &context.events,
        PeerEvent::Disconnected {
            direction: context.direction,
            generation: context.generation,
            remote,
            error_kind: result.as_ref().err().map(SessionError::kind),
        },
    );
    // 返回时 Lifetime 取消实际 TLS I/O, 不增加脱离角色根任务的清理拥有者.
    result.map_err(SessionError::into_io)
}

/// 拨号方先提交 Hello. 接收方验证准入后再返回 bearer 凭证, 避免向匿名调用方披露.
async fn handshake(
    reader: &mut tonic::Streaming<SessionPacket>,
    writer: &mut mpsc::Sender<SessionPacket>,
    context: &SessionContext,
) -> Result<RemotePeer, SessionError> {
    if context.direction == ConnectionDirection::Outbound {
        send(writer, Body::Hello((*context.local).clone()), MAX_HELLO_FRAME_BYTES).await?;
    }
    let message = receive(reader, MAX_HELLO_FRAME_BYTES)
        .await?
        .ok_or_else(|| io::Error::new(io::ErrorKind::UnexpectedEof, "connection closed before hello"))?;
    // ProtocolError 永远是终止信号, 即使其枚举值未知也不能触发错误回声.
    let remote = match message {
        Body::Rejection(_) => Err(SessionError::RemoteRejection),
        Body::Hello(hello) => validate_remote(context, hello),
        _ => Err(SessionError::Reject(ProtocolErrorCode::UnexpectedMessage)),
    }?;
    Ok(remote)
}

/// 每个方向只保存一个递增编号和一个可选截止时间, 无需 Ping 队列或额外心跳任务.
async fn keep_alive(
    reader: &mut tonic::Streaming<SessionPacket>,
    writer: &mut mpsc::Sender<SessionPacket>,
    context: &SessionContext,
    maximum: u32,
) -> Result<(), SessionError> {
    let mut request_id = 0_u64;
    let mut waiting_until = None;
    let mut next_ping = Instant::now() + context.heartbeat_interval;
    loop {
        // 跨心跳定时器保留同一个 gRPC 读取 future, 不用反复取消读取来驱动保活.
        let message = {
            let incoming = receive(reader, maximum);
            tokio::pin!(incoming);
            loop {
                let deadline = waiting_until.unwrap_or(next_ping);
                tokio::select! {
                    biased;
                    () = time::sleep_until(deadline) => {
                        if waiting_until.is_some() { return Err(pong_timed_out()); }
                        request_id = request_id.checked_add(1).ok_or_else(|| io::Error::other("ping request id exhausted"))?;
                        let deadline = Instant::now() + context.pong_timeout;
                        waiting_until = Some(deadline);
                        write_before(writer, Body::Ping(Ping { request_id }), maximum, deadline).await?;
                    }
                    message = &mut incoming => break message?,
                }
            }
        };
        let Some(message) = message else { return Ok(()) };
        if waiting_until.is_some_and(|deadline| Instant::now() >= deadline) {
            return Err(pong_timed_out());
        }
        match message {
            Body::Ping(ping) => {
                require_request_id(ping.request_id)?;
                let deadline = waiting_until.unwrap_or_else(|| Instant::now() + context.pong_timeout);
                write_before(writer, Body::Pong(Pong { request_id: ping.request_id }), maximum, deadline).await?;
            }
            Body::Pong(pong) => {
                require_request_id(pong.request_id)?;
                if waiting_until.is_some() && pong.request_id == request_id {
                    waiting_until = None;
                    next_ping = Instant::now() + context.heartbeat_interval;
                }
            }
            Body::Rejection(_) => return Err(SessionError::RemoteRejection),
            _ => return Err(SessionError::Reject(ProtocolErrorCode::UnexpectedMessage)),
        }
    }
}

/// 为消息进入有界队列设置绝对期限. 中途超时会向上传播并结束整个会话.
async fn write_before(writer: &mut mpsc::Sender<SessionPacket>, message: Body, maximum: u32, deadline: Instant) -> Result<(), SessionError> {
    // timeout_at 可能先轮询内部 future. 显式检查避免已过期的操作继续入队.
    if Instant::now() >= deadline {
        return Err(pong_timed_out());
    }
    time::timeout_at(deadline, send(writer, message, maximum)).await.map_err(|_| pong_timed_out())?
}

/// 仅向仍有剩余写期限的连接尽力发送一个有限错误码, 本地取消始终优先.
async fn send_rejection(writer: &mut mpsc::Sender<SessionPacket>, error: &SessionError, deadline: Instant, maximum: u32, cancellation: &CancellationToken) {
    let SessionError::Reject(code) = error else { return };
    if Instant::now() >= deadline {
        return;
    }
    let message = ProtocolError { code: *code as i32 };
    tokio::select! {
        biased;
        () = cancellation.cancelled() => {},
        _ = time::timeout_at(deadline, send(writer, Body::Rejection(message), maximum)) => {},
    }
}

/// 本端队列有界, 发送完成仅表示交给 RPC 驱动. Ping 的绝对期限继续覆盖真实传输和匹配 Pong.
async fn send(writer: &mpsc::Sender<SessionPacket>, body: Body, maximum: u32) -> Result<(), SessionError> {
    use prost::Message;
    let message = SessionPacket { body: Some(body) };
    if message.encoded_len() > maximum as usize {
        return Err(SessionError::Reject(ProtocolErrorCode::ResourceLimit));
    }
    writer
        .send(message)
        .await
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "RPC output closed"))?;
    Ok(())
}

/// Tonic 在解码前限制消息大小, 此处再执行协商预算和显式 oneof 阶段规则.
async fn receive(reader: &mut tonic::Streaming<SessionPacket>, maximum: u32) -> Result<Option<Body>, SessionError> {
    use prost::Message;
    let Some(message) = reader.message().await.map_err(crate::protocol::rpc_error)? else {
        return Ok(None);
    };
    if message.encoded_len() > maximum as usize {
        return Err(SessionError::Reject(ProtocolErrorCode::ResourceLimit));
    }
    message.body.map(Some).ok_or(SessionError::Reject(ProtocolErrorCode::UnexpectedMessage))
}

/// 将固定的本地保活失败转换成稳定 I/O 类别, 不携带远端输入.
fn pong_timed_out() -> SessionError {
    io::Error::new(io::ErrorKind::TimedOut, "peer heartbeat response or write timed out").into()
}

/// 默认零值不能关联到一轮真实探测, 必须在处理 Ping/Pong 前拒绝.
fn require_request_id(request_id: u64) -> Result<(), SessionError> {
    if request_id == 0 {
        return Err(SessionError::Reject(ProtocolErrorCode::UnexpectedMessage));
    }
    Ok(())
}

/// 消息类型正确之后继续验证字段语义. 身份声明还必须通过 Supervisor 准入签名.
fn validate_remote(context: &SessionContext, hello: Hello) -> Result<RemotePeer, SessionError> {
    if hello.protocol_major != PROTOCOL_MAJOR {
        return Err(SessionError::Reject(ProtocolErrorCode::ProtocolMismatch));
    }
    if hello.max_frame_bytes < MIN_FRAME_BYTES {
        return Err(SessionError::Reject(ProtocolErrorCode::InvalidHello));
    }
    let member = context.identity.verify(&hello)?;
    if member.cluster_id != context.member.cluster_id {
        return Err(SessionError::Reject(ProtocolErrorCode::ClusterMismatch));
    }
    if member.peer_id == context.member.peer_id || member.principal == context.member.principal {
        return Err(SessionError::Reject(ProtocolErrorCode::SelfConnection));
    }
    let local_role = crate::protocol::NodeRole::try_from(context.member.role).map_err(|_| SessionError::Reject(ProtocolErrorCode::Unauthorized))?;
    let role = crate::protocol::NodeRole::try_from(member.role).map_err(|_| SessionError::Reject(ProtocolErrorCode::Unauthorized))?;
    // 连接方向是角色契约: Planet 只能主动连接 Star, Star 不反拨 Planet, 禁止 Planet 级联.
    let allowed = matches!(
        (local_role, role, context.direction),
        (crate::protocol::NodeRole::Star, crate::protocol::NodeRole::Star, _)
            | (crate::protocol::NodeRole::Star, crate::protocol::NodeRole::Planet, ConnectionDirection::Inbound)
            | (
                crate::protocol::NodeRole::Planet,
                crate::protocol::NodeRole::Star,
                ConnectionDirection::Outbound
            )
    );
    if !allowed {
        return Err(SessionError::Reject(ProtocolErrorCode::Unauthorized));
    }
    if let Some(expected) = &context.expected_member
        && (member.principal != expected.principal
            || member.advertise != expected.advertise
            || member.epoch < expected.epoch
            || (member.epoch == expected.epoch && member != *expected))
    {
        return Err(SessionError::Reject(ProtocolErrorCode::Unauthorized));
    }
    let advertise = member.advertise.parse().map_err(|_| SessionError::Reject(ProtocolErrorCode::InvalidHello))?;
    Ok(RemotePeer {
        peer_id: member.peer_id,
        principal: member.principal,
        epoch: member.epoch,
        role,
        group: member.group,
        advertise,
        protocol_minor: hello.protocol_minor,
        max_frame_bytes: hello.max_frame_bytes,
    })
}

/// 诊断通道允许落后或没有订阅者, 这些情况不改变网络正确性.
pub fn emit(events: &broadcast::Sender<PeerEvent>, event: PeerEvent) {
    let _ = events.send(event);
}
