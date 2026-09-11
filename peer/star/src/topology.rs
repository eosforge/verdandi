//! 成员与会话的唯一索引. 短锁只保护内存, 不在锁内进行网络 I/O.
use crate::{
    connection::{ConnectionDirection, ConnectionSnapshot},
    identity::validate_member,
    protocol::{Member, NodeRole},
};
use std::{
    collections::{BTreeMap, BTreeSet},
    io,
    net::SocketAddr,
    sync::{Arc, Mutex, MutexGuard},
};
use tokio::sync::watch;
use tokio_util::sync::CancellationToken;

/// 从当前索引读取的完整计数快照, 不由有损事件累加计算.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NetworkStatus {
    /// 是否已经收齐并验证 Supervisor 完整名单.
    pub initialized: bool,
    /// 已知成员数, 初始化后包含自身, 初始化前为零.
    pub members: usize,
    /// 当前入站活跃会话数.
    pub inbound: usize,
    /// 当前出站活跃会话数.
    pub outbound: usize,
    /// 当前已验证的 Planet 入站连接, 不计入 Star 全互联计数.
    pub planet_inbound: usize,
}
/// 每个已验证成员一个拨号任务, 淘汰旧实例时取消其句柄.
pub(crate) struct DialCandidate {
    pub(crate) address: SocketAddr,
    pub(crate) peer_id: String,
    principal: String,
    pub(crate) cancellation: CancellationToken,
}
struct Session {
    snapshot: ConnectionSnapshot,
    cancellation: CancellationToken,
}
/// 成员拥有一个拨号候选与每方向最多一个活跃会话, 不为断线保存消息队列.
struct Entry {
    member: Member,
    candidate: Arc<DialCandidate>,
    /// 由根任务在 take_pending 时设置, 后续重试在同一个子任务内串行执行.
    started: bool,
    sessions: BTreeMap<ConnectionDirection, Session>,
}
#[derive(Default)]
struct State {
    initialized: bool,
    /// 以账号/端点指纹为键, 同端点重启只替换值, 不增加永久名额.
    members: BTreeMap<String, Entry>,
}
/// 一个部署凭据只保留一个当前实例, 新代次取消旧实例全部任务并复用成员名额.
pub(crate) struct Topology {
    cluster: String,
    local_id: String,
    local_principal: String,
    local_address: SocketAddr,
    maximum: usize,
    cancellation: CancellationToken,
    state: Mutex<State>,
    /// 合并变更通知, 不在心跳中携带全网 revision.
    changes: watch::Sender<()>,
}
impl Topology {
    /// 保存已校验本地身份和 1..4096 容量, 初始索引为空且禁止登记会话.
    pub(crate) fn new(
        cluster: String,
        local_id: String,
        local_principal: String,
        local_address: SocketAddr,
        maximum: usize,
        cancellation: CancellationToken,
    ) -> Self {
        Self {
            cluster,
            local_id,
            local_principal,
            local_address,
            maximum,
            cancellation,
            state: Mutex::default(),
            changes: watch::channel(()).0,
        }
    }
    /// 首次名单先完整校验, 再一次安装. 不允许逐项可见或根据不完整名单启动拨号.
    pub(crate) fn initialize(&self, members: Vec<Member>) -> io::Result<()> {
        if members.is_empty() || members.len() > self.maximum {
            return Err(invalid("invalid complete member count"));
        }
        let mut principals = BTreeSet::new();
        let mut addresses = BTreeSet::new();
        let mut previous = None;
        let mut found_self = false;
        let mut entries = BTreeMap::new();
        for member in members {
            validate_member(&member)?;
            if member.role != NodeRole::Star as i32
                || member.cluster_id != self.cluster
                || previous.as_ref().is_some_and(|id| id >= &member.peer_id)
                || !principals.insert(member.principal.clone())
                || !addresses.insert(member.advertise.clone())
            {
                return Err(invalid("invalid complete member ordering or identity"));
            }
            previous = Some(member.peer_id.clone());
            if member.principal == self.local_principal {
                if member.peer_id != self.local_id || member.advertise != self.local_address.to_string() {
                    return Err(invalid("complete list has conflicting local identity"));
                }
                found_self = true;
            } else {
                if member.peer_id == self.local_id || member.advertise == self.local_address.to_string() {
                    return Err(invalid("complete list aliases local peer"));
                }
                entries.insert(member.principal.clone(), self.entry(member)?);
            }
        }
        if !found_self {
            return Err(invalid("complete list omits local peer"));
        }
        let mut state = self.lock()?;
        if state.initialized {
            return Err(invalid("topology already initialized"));
        }
        state.members = entries;
        state.initialized = true;
        drop(state);
        self.changes.send_replace(());
        Ok(())
    }
    /// 返回容量一的变更游标, 只唤醒重新读索引, 不传递需要重放的事件.
    pub(crate) fn subscribe(&self) -> watch::Receiver<()> {
        self.changes.subscribe()
    }
    /// 关闭根拨号边界, 取消所有现有及后续创建的子任务令牌.
    pub(crate) fn cancel_dials(&self) {
        self.cancellation.cancel();
    }
    /// 锁中毒传播为根服务错误, 不从可能不一致的索引继续运行.
    fn lock(&self) -> io::Result<MutexGuard<'_, State>> {
        self.state.lock().map_err(|_| io::Error::other("topology lock poisoned"))
    }
    /// 先构造完整成员与取消边界, 返回前不修改共享索引.
    fn entry(&self, member: Member) -> io::Result<Entry> {
        let address = member.advertise.parse().map_err(|_| invalid("invalid member address"))?;
        Ok(Entry {
            candidate: Arc::new(DialCandidate {
                address,
                peer_id: member.peer_id.clone(),
                principal: member.principal.clone(),
                cancellation: self.cancellation.child_token(),
            }),
            started: member.role != NodeRole::Star as i32,
            member,
            sessions: BTreeMap::new(),
        })
    }
    /// 短锁内取得尚未启动的成员并标记, 保证每个实例只创建一个拨号拥有者.
    pub(crate) fn take_pending(&self) -> io::Result<Vec<Arc<DialCandidate>>> {
        Ok(self
            .lock()?
            .members
            .values_mut()
            .filter_map(|entry| {
                if entry.started {
                    None
                } else {
                    entry.started = true;
                    Some(entry.candidate.clone())
                }
            })
            .collect())
    }
    /// 同时核对 Arc 身份和出站索引, 旧实例的任务即使迟到也不能继续拨号.
    pub(crate) fn should_dial(&self, candidate: &Arc<DialCandidate>) -> io::Result<bool> {
        Ok(self
            .lock()?
            .members
            .get(&candidate.principal)
            .is_some_and(|entry| Arc::ptr_eq(candidate, &entry.candidate) && !entry.sessions.contains_key(&ConnectionDirection::Outbound)))
    }
    /// 只学习对端已签名的自身信息, 不传播名单. 高代次替换必须来自同一部署凭据.
    pub(crate) fn register(
        self: &Arc<Self>,
        snapshot: ConnectionSnapshot,
        cancellation: CancellationToken,
        candidate: Option<&Arc<DialCandidate>>,
    ) -> io::Result<ConnectionLease> {
        let remote = &snapshot.remote;
        let member = Member {
            cluster_id: self.cluster.clone(),
            peer_id: remote.peer_id.clone(),
            principal: remote.principal.clone(),
            advertise: remote.advertise.to_string(),
            epoch: remote.epoch,
            role: remote.role as i32,
            group: remote.group.clone(),
        };
        validate_member(&member)?;
        if member.principal == self.local_principal || member.peer_id == self.local_id || remote.advertise == self.local_address {
            return Err(invalid("self connection"));
        }
        if remote.role == NodeRole::Planet && snapshot.direction != ConnectionDirection::Inbound {
            return Err(invalid("Planet connections must be inbound"));
        }
        let mut state = self.lock()?;
        if !state.initialized {
            return Err(io::Error::new(io::ErrorKind::NotConnected, "topology not initialized"));
        }
        if let Some(candidate) = candidate
            && (candidate.cancellation.is_cancelled()
                || !state
                    .members
                    .get(&candidate.principal)
                    .is_some_and(|entry| Arc::ptr_eq(candidate, &entry.candidate)))
        {
            return Err(invalid("stale dial candidate"));
        }
        for entry in state.members.values().filter(|entry| entry.member.principal != member.principal) {
            if entry.member.peer_id == member.peer_id || entry.member.advertise == member.advertise {
                return Err(invalid("member identity or address conflict"));
            }
        }
        let replace = match state.members.get(&member.principal) {
            Some(entry) if member.role != entry.member.role || member.advertise != entry.member.advertise => {
                return Err(invalid("deployment role or endpoint changed"));
            }
            Some(entry) if member.epoch < entry.member.epoch => return Err(invalid("stale member incarnation")),
            Some(entry) if member.epoch == entry.member.epoch => {
                if member != entry.member {
                    return Err(invalid("conflicting member incarnation"));
                }
                false
            }
            Some(_) => true,
            None => {
                let count = state.members.values().filter(|entry| entry.member.role == member.role).count();
                let maximum = if remote.role == NodeRole::Star { self.maximum - 1 } else { self.maximum };
                if count >= maximum {
                    return Err(io::Error::new(io::ErrorKind::WouldBlock, "member capacity reached"));
                }
                true
            }
        };
        if replace {
            // 先构造可验证的新条目, 然后取消旧拨号和会话. 旧 lease 的 Drop 不能删除新记录.
            let entry = self.entry(member.clone())?;
            if let Some(old) = state.members.insert(member.principal.clone(), entry) {
                old.candidate.cancellation.cancel();
                for session in old.sessions.values() {
                    session.cancellation.cancel();
                }
            }
        }
        let entry = state.members.get_mut(&member.principal).ok_or_else(|| invalid("member disappeared"))?;
        if entry
            .sessions
            .get(&snapshot.direction)
            .is_some_and(|session| !session.cancellation.is_cancelled())
        {
            return Err(io::Error::new(io::ErrorKind::AlreadyExists, "peer direction already connected"));
        }
        let direction = snapshot.direction;
        let generation = snapshot.generation;
        entry.sessions.insert(direction, Session { snapshot, cancellation });
        drop(state);
        self.changes.send_replace(());
        Ok(ConnectionLease {
            topology: self.clone(),
            principal: member.principal,
            direction,
            generation,
        })
    }
    /// 返回独立的有界会话快照, 调用者不持有索引锁或可变内部引用.
    pub(crate) fn connections(&self) -> io::Result<Vec<ConnectionSnapshot>> {
        Ok(self
            .lock()?
            .members
            .values()
            .flat_map(|entry| entry.sessions.values().map(|session| session.snapshot.clone()))
            .collect())
    }
    /// 在同一锁内计算初始化和连接计数, 不从有损诊断事件重建状态.
    pub(crate) fn status(&self) -> io::Result<NetworkStatus> {
        let state = self.lock()?;
        let mut status = NetworkStatus {
            initialized: state.initialized,
            members: if state.initialized {
                state.members.values().filter(|entry| entry.member.role == NodeRole::Star as i32).count() + 1
            } else {
                0
            },
            inbound: 0,
            outbound: 0,
            planet_inbound: 0,
        };
        for entry in state.members.values() {
            if entry.member.role == NodeRole::Planet as i32 {
                status.planet_inbound += usize::from(entry.sessions.contains_key(&ConnectionDirection::Inbound));
                continue;
            }
            status.inbound += usize::from(entry.sessions.contains_key(&ConnectionDirection::Inbound));
            status.outbound += usize::from(entry.sessions.contains_key(&ConnectionDirection::Outbound));
        }
        Ok(status)
    }
}
/// 会话关闭只删除自身 generation, 不能影响后来建立的连接或新的进程实例.
pub(crate) struct ConnectionLease {
    topology: Arc<Topology>,
    principal: String,
    direction: ConnectionDirection,
    generation: u64,
}
impl Drop for ConnectionLease {
    /// 只删除同一 generation 的会话, 然后唤醒等待重连的拥有者.
    fn drop(&mut self) {
        if let Ok(mut state) = self.topology.state.lock()
            && let Some(entry) = state.members.get_mut(&self.principal)
            && entry
                .sessions
                .get(&self.direction)
                .is_some_and(|session| session.snapshot.generation == self.generation)
        {
            entry.sessions.remove(&self.direction);
        }
        self.topology.changes.send_replace(());
    }
}
/// 成员语义冲突使用固定本地错误类别, 不把远端输入写入诊断正文.
fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

#[cfg(test)]
#[path = "../tests/unit/topology.rs"]
mod tests;
