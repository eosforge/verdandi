//! 向 Supervisor 登记, 保留首次 CAS 基线, 按角色校验完整 Star 名单或有界 Planet 候选.
use crate::{
    config::{PeerConfig, SupervisorAddress},
    connection::{PeerEvent, emit},
    identity::{Identity, ProcessIdentity},
    protocol::{Hello, LoginRequest, Member, NodeRole, PROTOCOL_MAJOR, PROTOCOL_MINOR, RegistrationRequest, admission_client::AdmissionClient},
};
use std::{io, net::SocketAddr, sync::Arc, time::Duration};
use tokio::{net::TcpStream, sync::broadcast, time};
use tokio_util::sync::CancellationToken;

/// 同一进程共用首次 CAS 基线和候选轮次, 刷新名单不能获取新代次覆盖较新实例.
#[derive(Default)]
pub struct Registrar {
    expected_epoch: Option<u64>,
    candidate_round: u32,
}
impl Registrar {
    /// 无可用上游时轮换候选. 此计数不参与安全排序, 回绕只会重新访问候选集合.
    pub fn next_candidates(&mut self) {
        self.candidate_round = self.candidate_round.wrapping_add(1);
    }
}
/// 已收齐登记应答的临时结果. 拓扑层还会原子校验安装整个成员列表.
pub struct Joined {
    /// 本进程独立的已签名 bearer 凭证, 节点间连接复用.
    pub local: Arc<Hello>,
    /// 已核对本地身份的成员记录.
    pub member: Arc<Member>,
    /// Star 收到完整 Star 名单, Planet 收到局部 Star 候选. 校验失败不得部分发布.
    pub members: Vec<Member>,
}
/// 连接失败可重试, 身份/协议错误或 CAS 冲突停止启动. 账号和 UUID 在整个循环中保持不变.
pub async fn join(
    config: &PeerConfig,
    identity: &Identity,
    process: &ProcessIdentity,
    address: SocketAddr,
    events: &broadcast::Sender<PeerEvent>,
    cancellation: &CancellationToken,
    registrar: &mut Registrar,
) -> io::Result<Joined> {
    let endpoint = SupervisorAddress::parse(&config.supervisor)?;
    let mut failures = 0_u32;
    loop {
        let attempt = time::timeout(config.handshake_timeout, register(config, &endpoint, identity, process, address, registrar));
        let result = tokio::select! {
            biased;
            () = cancellation.cancelled() => return Err(io::Error::new(io::ErrorKind::Interrupted, "registration cancelled")),
            result = attempt => result.unwrap_or_else(|_| Err(io::Error::new(io::ErrorKind::TimedOut, "registration timed out"))),
        };
        match result {
            Ok(joined) => return Ok(joined),
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::InvalidData | io::ErrorKind::InvalidInput | io::ErrorKind::PermissionDenied | io::ErrorKind::AlreadyExists
                ) =>
            {
                return Err(error);
            }
            Err(error) => {
                failures = failures.saturating_add(1);
                let salt = process.id.bytes().fold(0_u64, |sum, byte| sum.wrapping_mul(31).wrapping_add(u64::from(byte)));
                let ceiling = Duration::from_millis(200).saturating_mul(1_u32 << failures.min(5)).min(Duration::from_secs(5));
                let delay =
                    ceiling / 2 + Duration::from_millis(salt.wrapping_add(u64::from(failures)) % (u64::try_from(ceiling.as_millis() / 2).unwrap_or(2500) + 1));
                emit(events, PeerEvent::RegistrationRetry { error_kind: error.kind() });
                tokio::select! {
                    () = cancellation.cancelled() => return Err(io::Error::new(io::ErrorKind::Interrupted, "registration cancelled")),
                    () = time::sleep(delay) => {}
                }
            }
        }
    }
}
/// 单次登记拥有 TCP/TLS 流, 无论成功或失败均在返回时关闭.
async fn register(
    config: &PeerConfig,
    endpoint: &SupervisorAddress,
    identity: &Identity,
    process: &ProcessIdentity,
    address: SocketAddr,
    registrar: &mut Registrar,
) -> io::Result<Joined> {
    let stream = time::timeout(config.connect_timeout, TcpStream::connect((endpoint.host.as_str(), endpoint.port)))
        .await
        .map_err(|_| io::Error::new(io::ErrorKind::TimedOut, "supervisor connect timed out"))??;
    stream.set_nodelay(true)?;
    let stream = identity.connect_rpc(stream, &endpoint.host).await?;
    let authority = stream.get_ref().0.peer_addr()?.to_string();
    let (channel, _lifetime) = crate::rpc::channel(stream, &authority).await?;
    let mut client = AdmissionClient::new(channel)
        .max_decoding_message_size(2 * 1024 * 1024)
        .max_encoding_message_size(4096);
    let (username, password) = identity.credentials();
    // 首次登录读取 CAS 基线. 响应丢失后保留原基线, 防止旧进程追赶并覆盖新实例.
    if registrar.expected_epoch.is_none() {
        let challenge = client
            .challenge(LoginRequest {
                username: username.into(),
                password: password.into(),
                cluster_id: config.cluster_id.clone(),
                advertise: address.to_string(),
            })
            .await
            .map_err(crate::protocol::rpc_error)?
            .into_inner();
        if challenge.cluster_id != config.cluster_id {
            return Err(invalid("supervisor cluster mismatch"));
        }
        registrar.expected_epoch = Some(challenge.expected_epoch);
    }
    let expected_epoch = registrar.expected_epoch.ok_or_else(|| invalid("missing registration baseline"))?;
    let response = client
        .register(RegistrationRequest {
            username: username.into(),
            password: password.into(),
            cluster_id: config.cluster_id.clone(),
            peer_id: process.id.clone(),
            advertise: address.to_string(),
            expected_epoch,
            role: config.role as i32,
            group: config.group.clone(),
            candidate_round: registrar.candidate_round,
        })
        .await
        .map_err(crate::protocol::rpc_error)?
        .into_inner();
    let (members, admission, signature) = (response.members, response.admission, response.signature);
    let principal = identity.endpoint_principal(&config.cluster_id, &address.to_string());
    if config.role == NodeRole::Planet {
        validate_candidates(&members, &config.cluster_id, &config.group, &principal)?;
    }
    // 准入必须绑定本地实际生成的身份, 不能接受服务端返回的另一进程凭据.
    let member = identity.admission(&admission, &signature).map_err(|error| error.into_io())?;
    if member.cluster_id != config.cluster_id
        || member.peer_id != process.id
        || member.principal != principal
        || member.advertise != address.to_string()
        || member.role != config.role as i32
        || member.group != config.group
        || expected_epoch.checked_add(1) != Some(member.epoch)
    {
        return Err(invalid("registration does not match this process"));
    }
    if config.role == NodeRole::Star && (members.len() > config.max_peers || !members.iter().any(|item| item == &member)) {
        return Err(invalid("incomplete registration member list"));
    }
    Ok(Joined {
        local: Arc::new(Hello {
            protocol_major: PROTOCOL_MAJOR,
            protocol_minor: PROTOCOL_MINOR,
            max_frame_bytes: config.max_frame_bytes,
            admission,
            admission_signature: signature,
        }),
        member: Arc::new(member),
        members,
    })
}
/// 违反登记协议的结果终止本次进程启动, 不刷新 CAS 基线.
fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

/// 候选名单有界, 只含同 Galaxy 的 Star, 本组优先且各级按 UUID 排序, 不包含自身部署.
pub fn validate_candidates(members: &[Member], cluster: &str, group: &str, principal: &str) -> io::Result<()> {
    use std::collections::BTreeSet;
    if members.len() > 8 {
        return Err(invalid("too many Planet candidates"));
    }
    let mut ids = BTreeSet::new();
    let mut principals = BTreeSet::new();
    let mut addresses = BTreeSet::new();
    let mut previous = None;
    for member in members {
        crate::identity::validate_member(member)?;
        let order = (member.group != group, member.peer_id.as_str());
        if member.role != NodeRole::Star as i32
            || member.cluster_id != cluster
            || member.principal == principal
            || !ids.insert(&member.peer_id)
            || !principals.insert(&member.principal)
            || !addresses.insert(&member.advertise)
            || previous.is_some_and(|old| old >= order)
        {
            return Err(invalid("invalid Planet candidates"));
        }
        previous = Some(order);
    }
    Ok(())
}

#[cfg(test)]
#[path = "../tests/unit/registration.rs"]
mod tests;
