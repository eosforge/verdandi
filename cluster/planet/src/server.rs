//! Planet 拥有一个串行上游任务. 只有握手完成后安装活动连接, 关闭旧流后才尝试下一入口.
use std::{
    io,
    net::SocketAddr,
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::{
    net::{TcpListener, TcpStream},
    sync::broadcast,
    task::JoinHandle,
    time::{self, Instant},
};
use tokio_util::sync::CancellationToken;
use verdandi_peer_common::{
    ConnectionDirection, ConnectionSnapshot, PeerConfig, PeerEvent,
    connection::{SessionContext, emit, run_session},
    identity::{Identity, ProcessIdentity},
    protocol::{Member, NodeRole},
    registration::{self, Registrar},
    retry::{jitter_salt, retry_delay},
};

/// Planet 当前连接状态. initialized 只证明已经准入, 不证明业务缓存已同步.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct PlanetStatus {
    /// 是否取得并验证自身准入和有界候选名单.
    pub initialized: bool,
    /// 当前已获知的 Star 候选数, 范围 0..8.
    pub candidates: usize,
    /// 唯一活动上游, None 表示尚未连接或正在切换.
    pub upstream: Option<ConnectionSnapshot>,
}

/// Planet 服务根, 持有 listener, 候选任务和全部状态的关闭边界.
pub struct Planet {
    id: String,
    address: SocketAddr,
    state: Arc<Mutex<PlanetStatus>>,
    events: broadcast::Sender<PeerEvent>,
    cancellation: CancellationToken,
    task: Option<JoinHandle<io::Result<()>>>,
}
impl Planet {
    /// 校验配置并绑定监听, 新进程必须先由 Supervisor 授权为 Planet.
    pub async fn start(mut config: PeerConfig) -> io::Result<Self> {
        config.role = NodeRole::Planet;
        config.validate()?;
        let identity = Arc::new(Identity::load(&config.identity)?);
        let process = Arc::new(ProcessIdentity::new()?);
        let listener = TcpListener::bind(config.listen).await?;
        let address = listener.local_addr()?;
        let advertise = config.advertise.unwrap_or(address);
        verdandi_peer_common::config::validate_remote_address("effective advertise", advertise)?;
        identity.validate_endpoint(advertise)?;
        let state = Arc::new(Mutex::new(PlanetStatus::default()));
        let (events, _) = broadcast::channel(config.event_capacity);
        let cancellation = CancellationToken::new();
        let id = process.id.clone();
        let task_state = state.clone();
        let task_events = events.clone();
        let cancel = cancellation.clone();
        let task = tokio::spawn(async move {
            let upstream = upstreams(config, advertise, identity, process, task_state, task_events, cancel.clone());
            tokio::pin!(upstream);
            loop {
                tokio::select! {
                    biased;
                    () = cancel.cancelled() => return Ok(()),
                    result = &mut upstream => return result,
                    // SDK 接入尚未实现, 关闭入站流而不伪造业务或 Star/Planet 级联会话.
                    accepted = listener.accept() => { let (stream, _) = accepted?; drop(stream); }
                }
            }
        });
        Ok(Self {
            id,
            address,
            state,
            events,
            cancellation,
            task: Some(task),
        })
    }
    /// 当前进程 UUID, 重连不变, 重启更换.
    #[must_use]
    pub fn peer_id(&self) -> &str {
        &self.id
    }
    /// 当前实际绑定地址, 测试端口零在启动后解析为真实端口.
    #[must_use]
    pub fn listen_address(&self) -> SocketAddr {
        self.address
    }
    /// 从实际索引读取状态, 不由有损诊断事件累加计算.
    pub fn status(&self) -> io::Result<PlanetStatus> {
        self.state
            .lock()
            .map(|state| state.clone())
            .map_err(|_| io::Error::other("Planet state lock poisoned"))
    }
    /// 订阅有界且允许丢失的诊断事件.
    #[must_use]
    pub fn subscribe(&self) -> broadcast::Receiver<PeerEvent> {
        self.events.subscribe()
    }
    /// 观察根任务错误, 取消此等待不会分离任务或丢失关闭能力.
    pub async fn wait(&mut self) -> io::Result<()> {
        let Some(task) = self.task.as_mut() else { return Ok(()) };
        let result = task.await.map_err(io::Error::other);
        self.task.take();
        self.cancellation.cancel();
        result?
    }
    /// 取消并等待所有网络资源释放, 不留下独立重连任务.
    pub async fn shutdown(mut self) -> io::Result<()> {
        self.cancellation.cancel();
        self.wait().await
    }
}
impl Drop for Planet {
    fn drop(&mut self) {
        self.cancellation.cancel();
        if let Some(task) = self.task.take() {
            task.abort();
        }
    }
}

/// 某一代连接的登记租约, 取消 future 或异常返回也会清空对应活动上游.
struct Lease {
    state: Arc<Mutex<PlanetStatus>>,
    generation: u64,
}
impl Drop for Lease {
    fn drop(&mut self) {
        if let Ok(mut state) = self.state.lock()
            && state.upstream.as_ref().is_some_and(|session| session.generation == self.generation)
        {
            state.upstream = None;
        }
    }
}

/// 每个候选只有最新身份, 退避计数和期限, 不为候选创建独立拨号任务.
struct Candidate {
    member: Member,
    failures: u32,
    next: Instant,
    quarantined: bool,
    /// 一轮内每个候选最多尝试一次, 慢失败不能饿死末尾候选和跨组入口.
    tried: bool,
}
fn candidates(members: Vec<Member>, previous: &[Candidate]) -> Vec<Candidate> {
    members
        .into_iter()
        .map(|member| {
            // 刷新名单不能回退已经通过直接握手观察到的部署代次.
            let old = previous.iter().find(|old| old.member.principal == member.principal);
            match old {
                Some(old) if old.member.epoch >= member.epoch => Candidate {
                    member: old.member.clone(),
                    failures: old.failures,
                    next: old.next,
                    quarantined: old.quarantined,
                    tried: false,
                },
                _ => Candidate {
                    member,
                    failures: 0,
                    next: Instant::now(),
                    quarantined: false,
                    tried: false,
                },
            }
        })
        .collect()
}

/// 首次准入可等待 Supervisor 恢复. 后续刷新有总期限, 不因管理端离线永久阻塞已知候选重连.
async fn upstreams(
    config: PeerConfig,
    advertise: SocketAddr,
    identity: Arc<Identity>,
    process: Arc<ProcessIdentity>,
    state: Arc<Mutex<PlanetStatus>>,
    events: broadcast::Sender<PeerEvent>,
    cancellation: CancellationToken,
) -> io::Result<()> {
    let mut registrar = Registrar::default();
    let mut joined = registration::join(&config, &identity, &process, advertise, &events, &cancellation, &mut registrar).await?;
    let mut choices = candidates(std::mem::take(&mut joined.members), &[]);
    {
        let mut status = state.lock().map_err(|_| io::Error::other("Planet state lock poisoned"))?;
        status.initialized = true;
        status.candidates = choices.len();
    }
    emit(&events, PeerEvent::Initialized { members: choices.len() });
    let salt = jitter_salt(process.id.as_bytes());
    let mut generation = 0_u64;
    let mut refresh_at = Instant::now() + Duration::from_secs(5);
    let mut dial_at = Instant::now();
    loop {
        if cancellation.is_cancelled() {
            return Ok(());
        }
        let now = Instant::now();
        // 只有已知入口都尝试失败或名单为空才查询另一批. 健康会话在 attempt 内保活, 不进入此分支.
        if now >= refresh_at && choices.iter().all(|candidate| candidate.failures > 0 || candidate.quarantined) {
            registrar.next_candidates();
            let refreshed = time::timeout(
                Duration::from_secs(5),
                registration::join(&config, &identity, &process, advertise, &events, &cancellation, &mut registrar),
            )
            .await;
            match refreshed {
                Ok(Ok(next)) => {
                    if next.member != joined.member {
                        return Err(io::Error::new(io::ErrorKind::InvalidData, "Planet admission changed during refresh"));
                    }
                    choices = candidates(next.members, &choices);
                    state.lock().map_err(|_| io::Error::other("Planet state lock poisoned"))?.candidates = choices.len();
                }
                Ok(Err(error)) => return Err(error),
                Err(_) => {} // 管理端离线不清空已经获准的候选.
            }
            refresh_at = Instant::now() + Duration::from_secs(5);
        }
        // 一轮扫过全部候选后才重新尝试, 防止少数慢失败入口不断抢占本组或备用入口.
        if choices.iter().all(|candidate| candidate.tried || candidate.quarantined) {
            for candidate in &mut choices {
                candidate.tried = false;
            }
        }
        let next = choices
            .iter()
            .enumerate()
            .filter(|(_, candidate)| !candidate.tried && !candidate.quarantined && candidate.next <= Instant::now())
            .min_by_key(|(_, candidate)| {
                (
                    candidate.member.group != config.group,
                    jitter_salt(candidate.member.principal.as_bytes()) ^ salt,
                )
            })
            .map(|(index, _)| index);
        let Some(index) = next else {
            let wake = choices
                .iter()
                .filter(|candidate| !candidate.tried && !candidate.quarantined)
                .map(|candidate| candidate.next)
                .min()
                .unwrap_or(refresh_at)
                .min(refresh_at);
            time::sleep_until(wake.max(Instant::now() + Duration::from_millis(10))).await;
            continue;
        };
        // 单进程最多每 250 ms 启动一次拨号, 不同时连接多台 Star.
        time::sleep_until(dial_at).await;
        dial_at = Instant::now() + Duration::from_millis(250);
        generation = generation.checked_add(1).ok_or_else(|| io::Error::other("Planet generation exhausted"))?;
        let candidate = &mut choices[index];
        candidate.tried = true;
        let address: SocketAddr = candidate.member.advertise.parse().map_err(io::Error::other)?;
        let context = SessionContext {
            local: joined.local.clone(),
            member: joined.member.clone(),
            identity: identity.clone(),
            direction: ConnectionDirection::Outbound,
            generation,
            expected_peer_id: None,
            expected_member: Some(candidate.member.clone()),
            handshake_timeout: config.handshake_timeout,
            heartbeat_interval: config.heartbeat_interval,
            pong_timeout: config.pong_timeout,
            cancellation: cancellation.child_token(),
            events: events.clone(),
        };
        let mut established = None;
        let result = match time::timeout(config.connect_timeout, TcpStream::connect(address)).await {
            Ok(Ok(stream)) => {
                run_session(stream, address, context, |snapshot, _| {
                    let mut status = state.lock().map_err(|_| io::Error::other("Planet state lock poisoned"))?;
                    if status.upstream.is_some() {
                        return Err(io::Error::new(io::ErrorKind::AlreadyExists, "Planet already has an upstream"));
                    }
                    candidate.member = member_from_session(&config.cluster_id, &snapshot);
                    status.upstream = Some(snapshot);
                    established = Some(Instant::now());
                    Ok(Lease {
                        state: state.clone(),
                        generation,
                    })
                })
                .await
            }
            Ok(Err(error)) => Err(error),
            Err(_) => Err(io::Error::new(io::ErrorKind::TimedOut, "Planet connect timed out")),
        };
        // 只从真正完成身份验证的时刻计算稳定连接窗口, 不把慢 TLS 握手当作稳定会话.
        candidate.failures = if established.is_some_and(|start| start.elapsed() >= config.stable_connection) {
            1
        } else {
            candidate.failures.saturating_add(1)
        };
        if let Err(error) = result {
            candidate.quarantined = matches!(error.kind(), io::ErrorKind::PermissionDenied | io::ErrorKind::InvalidData);
            emit(
                &events,
                PeerEvent::ConnectFailed {
                    address,
                    error_kind: error.kind(),
                },
            );
        }
        candidate.next = Instant::now() + retry_delay(address, candidate.failures, config.reconnect_min, config.reconnect_max, salt);
    }
}

/// 保存对端已签名的当前身份, 用于下一次连接的最低代次约束.
fn member_from_session(cluster: &str, snapshot: &ConnectionSnapshot) -> Member {
    let remote = &snapshot.remote;
    Member {
        cluster_id: cluster.into(),
        peer_id: remote.peer_id.clone(),
        principal: remote.principal.clone(),
        advertise: remote.advertise.to_string(),
        epoch: remote.epoch,
        role: remote.role as i32,
        group: remote.group.clone(),
    }
}

#[cfg(test)]
#[path = "../tests/unit/lifecycle.rs"]
mod tests;
