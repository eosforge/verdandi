//! Peer 根对象及其全部异步任务的所有权管理.
//!
//! `Peer` 是网络生命周期的唯一根. 它持有根取消令牌和 supervisor 的 `JoinHandle`.
//! supervisor 再通过 `JoinSet` 持有成员拨号任务与入站会话, 从而在正常关闭时逐层取消并等待,
//! 不让任何连接任务脱离拥有者继续运行.

use std::{
    io,
    net::SocketAddr,
    sync::{Arc, atomic::AtomicU64},
    time::Duration,
};

use tokio::{
    net::{TcpListener, TcpStream},
    sync::{Mutex, Semaphore, broadcast},
    task::{JoinHandle, JoinSet},
    time::{self, Instant},
};
use tokio_util::sync::CancellationToken;
use verdandi_peer_common::retry::{jitter_salt, next_generation, retry_delay};

use crate::{
    config::PeerConfig,
    connection::{ConnectionDirection, ConnectionSnapshot, PeerEvent, SessionContext, emit, run_session},
    identity::{Identity, ProcessIdentity},
    protocol::{Hello, Member},
    registration,
    topology::{DialCandidate, NetworkStatus, Topology},
};

#[cfg(test)]
#[path = "../tests/unit/server.rs"]
mod tests;

/// 一个已经绑定 listener 并拥有全部基础网络任务的 Peer.
///
/// 调用方应通过 `shutdown` 取消并等待全部任务. 直接 Drop 会取消并中止 supervisor, 作为异常路径的资源回收兜底.
pub struct Peer {
    /// 本进程的 UUID, 重试和重连不变, 下次启动重新生成.
    peer_id: String,
    /// 操作系统最终绑定的地址. 当配置端口为零时, 这里保存实际分配的端口.
    listen_address: SocketAddr,
    /// 整棵任务树的根取消令牌. clone 得到关联句柄, child_token 得到向下传播的子边界.
    cancellation: CancellationToken,
    /// 诊断广播发送端. `Peer` 持有一份, 因而运行期间订阅通道不会提前关闭.
    events: broadcast::Sender<PeerEvent>,
    /// 有界候选与实际会话索引. 诊断快照不依赖有损事件广播.
    topology: Arc<Topology>,
    /// 根任务句柄放在 Option 中, 使 `shutdown` 或 `Drop` 可以恰好取走一次所有权.
    supervisor: Option<JoinHandle<io::Result<()>>>,
}

impl Peer {
    /// 校验配置并绑定 listener, 随后登记并为每个成员启动一个串行拨号任务.
    ///
    /// 返回前 listener 已绑定, 完整登记成功前拒绝互联. 失败不会留下后台任务; 本方法不下载依赖或修改进程环境.
    pub async fn start(config: PeerConfig) -> io::Result<Self> {
        config.validate()?;
        if config.role != crate::protocol::NodeRole::Star {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "Star entry requires Star role"));
        }
        let identity = Arc::new(Identity::load(&config.identity)?);
        let process = Arc::new(ProcessIdentity::new()?);
        let peer_id = process.id.clone();
        let listener = TcpListener::bind(config.listen).await?;
        let listen_address = listener.local_addr()?;
        let advertise = config.advertise.unwrap_or(listen_address);
        crate::config::validate_remote_address("effective advertise", advertise)?;
        identity.validate_endpoint(advertise)?;
        let cancellation = CancellationToken::new();
        let (events, _) = broadcast::channel(config.event_capacity);
        let topology = Arc::new(Topology::new(
            config.cluster_id.clone(),
            peer_id.clone(),
            identity.endpoint_principal(&config.cluster_id, &advertise.to_string()),
            advertise,
            config.max_peers,
            cancellation.child_token(),
        ));
        let supervisor = tokio::spawn(start_network(
            listener,
            config,
            identity,
            process,
            topology.clone(),
            events.clone(),
            cancellation.child_token(),
        ));
        Ok(Self {
            peer_id,
            listen_address,
            cancellation,
            events,
            topology,
            supervisor: Some(supervisor),
        })
    }
    /// 返回当前进程的 UUID, 不使用持久部署凭据替代进程标识.
    #[must_use]
    pub fn peer_id(&self) -> &str {
        &self.peer_id
    }
    /// 从短锁索引读取完整状态, 不依赖诊断事件是否丢失.
    pub fn status(&self) -> io::Result<NetworkStatus> {
        self.topology.status()
    }

    /// 返回操作系统实际绑定的监听地址; 测试端口零会在这里变为具体端口.
    #[must_use]
    pub fn listen_address(&self) -> SocketAddr {
        self.listen_address
    }

    /// 创建一个有界事件订阅者.
    ///
    /// 订阅者只收到创建之后的事件; lagged 表示诊断丢失, 不改变连接状态.
    #[must_use]
    pub fn subscribe(&self) -> broadcast::Receiver<PeerEvent> {
        // 每次调用创建独立游标. Receiver 不会回放订阅前已经离开环形缓冲区的事件.
        self.events.subscribe()
    }

    /// 查询当前实际活跃的会话. 每个远端最多返回 Inbound 和 Outbound 两条记录.
    pub fn connections(&self) -> io::Result<Vec<ConnectionSnapshot>> {
        self.topology.connections()
    }

    /// 取消 listener, 拨号 supervisor 和所有会话, 并等待它们结束.
    ///
    /// 可观察的 supervisor panic 或 listener I/O 错误会转换为 `io::Error` 返回; 正常重复取消由 token 幂等处理.
    pub async fn shutdown(mut self) -> io::Result<()> {
        // cancel 是幂等操作, 并唤醒所有正在等待 `cancelled()` 的后代任务.
        self.cancellation.cancel();
        self.wait().await
    }

    /// 等待根网络任务结束并取得其结果, 不主动请求关闭.
    ///
    /// 可用于与操作系统信号同时等待, 避免网络已经退出但进程仍报告存活.
    /// 取消这个等待不会分离任务; 句柄仍由 Peer 持有. 结果取走后再次等待返回 Ok.
    pub async fn wait(&mut self) -> io::Result<()> {
        // 保留句柄直到 await 完成, select 的另一分支胜出时仍能通过 shutdown 回收.
        let Some(supervisor) = self.supervisor.as_mut() else {
            return Ok(());
        };

        // 第一个 `?` 处理 JoinError, 第二层 io::Result 作为本方法结果直接返回.
        let result = supervisor.await.map_err(|error| io::Error::other(format!("peer network task failed: {error}")));
        self.supervisor.take();
        self.cancellation.cancel();
        result?
    }
}

impl Drop for Peer {
    /// 在调用方遗漏显式 shutdown 时取消并中止根任务, 防止 listener 被永久分离.
    fn drop(&mut self) {
        self.cancellation.cancel();
        // Drop 不能执行 `.await`, 所以只能 abort. 正常路径应使用 `shutdown` 等待有序清理.
        if let Some(supervisor) = self.supervisor.take() {
            supervisor.abort();
        }
    }
}

/// 全部连接共享的不可变配置和有界资源. 不为每个成员复制整个配置和身份材料.
struct NetworkContext {
    /// 启动前校验过的不可变配置, 不在会话中读取环境变量.
    config: PeerConfig,
    /// 全部会话借用的本地握手消息.
    local: Arc<Hello>,
    /// 已签名的本端成员记录和只读安全上下文.
    member: Arc<Member>,
    identity: Arc<Identity>,
    process: Arc<ProcessIdentity>,
    /// 候选地址与活跃会话的唯一索引.
    topology: Arc<Topology>,
    /// 有界且可丢失的诊断通道.
    events: broadcast::Sender<PeerEvent>,
    /// 只分配会话代际, 不发布其他内存状态.
    generations: AtomicU64,
    /// 只限制同时进行的 TCP connect, 不限制已建立会话数.
    dial_permits: Semaphore,
    /// 所有候选共享的拨号起始节拍.
    dial_clock: Mutex<time::Interval>,
}

impl NetworkContext {
    /// 在连接边界创建 session 上下文, 让入站与出站始终使用同一套配置和协议规则.
    fn session(&self, direction: ConnectionDirection, generation: u64, cancellation: CancellationToken, expected_peer_id: Option<String>) -> SessionContext {
        SessionContext {
            local: self.local.clone(),
            direction,
            generation,
            cancellation,
            expected_peer_id,
            expected_member: None,
            handshake_timeout: self.config.handshake_timeout,
            heartbeat_interval: self.config.heartbeat_interval,
            pong_timeout: self.config.pong_timeout,
            member: self.member.clone(),
            identity: self.identity.clone(),
            events: self.events.clone(),
        }
    }
}

/// 初始化期间保留 listener, 但拒绝群组连接, 直到完整名单验证并一次安装成功.
async fn start_network(
    listener: TcpListener,
    config: PeerConfig,
    identity: Arc<Identity>,
    process: Arc<ProcessIdentity>,
    topology: Arc<Topology>,
    events: broadcast::Sender<PeerEvent>,
    cancellation: CancellationToken,
) -> io::Result<()> {
    let advertise = config.advertise.unwrap_or(listener.local_addr()?);
    let joined = {
        let mut registrar = registration::Registrar::default();
        let registration = registration::join(&config, &identity, &process, advertise, &events, &cancellation, &mut registrar);
        tokio::pin!(registration);
        loop {
            tokio::select! {
                biased;
                () = cancellation.cancelled() => return Ok(()),
                result = &mut registration => break result?,
                accepted = listener.accept() => { let (stream, _) = accepted?; drop(stream); }
            }
        }
    };
    let count = joined.members.len();
    topology.initialize(joined.members)?;
    emit(&events, PeerEvent::Initialized { members: count });
    let start = Instant::now() + Duration::from_millis(jitter_salt(process.id.as_bytes()) % 250);
    let mut dial_clock = time::interval_at(start, Duration::from_millis(250));
    dial_clock.set_missed_tick_behavior(time::MissedTickBehavior::Delay);
    let network = Arc::new(NetworkContext {
        dial_permits: Semaphore::new(config.max_concurrent_dials),
        generations: AtomicU64::new(1),
        dial_clock: Mutex::new(dial_clock),
        config,
        local: joined.local,
        member: joined.member,
        identity,
        process,
        topology,
        events,
    });
    run_network(listener, network, cancellation).await
}

/// 运行 listener 与所有候选 supervisor. 拓扑变化只唤醒一次扫描, 不逐条创建无界工作队列.
async fn run_network(listener: TcpListener, network: Arc<NetworkContext>, cancellation: CancellationToken) -> io::Result<()> {
    let inbound_permits = Arc::new(Semaphore::new(network.config.max_inbound_connections));
    let mut changes = network.topology.subscribe();
    let mut tasks = JoinSet::new();
    let mut result = loop {
        // take_pending 在短锁内标记任务已启动, 重复发现同一地址不会重复 spawn.
        let pending = match network.topology.take_pending() {
            Ok(pending) => pending,
            Err(error) => break Err(error),
        };
        for candidate in pending {
            tasks.spawn(supervise_candidate(candidate, network.clone()));
        }
        tokio::select! {
            () = cancellation.cancelled() => break Ok(()),
            changed = changes.changed() => {
                if changed.is_err() { break Err(io::Error::other("topology notifications closed")); }
            }
            accepted = listener.accept() => {
                let (stream, source_address) = match accepted { Ok(connection) => connection, Err(error) => break Err(error) };
                let permit = match inbound_permits.clone().try_acquire_owned() {
                    Ok(permit) => permit,
                    Err(_) => {
                        emit(&network.events, PeerEvent::ConnectionRejected {
                            direction: ConnectionDirection::Inbound, address: source_address, error_kind: io::ErrorKind::WouldBlock,
                        });
                        continue;
                    }
                };
                let generation = match next_generation(&network.generations) { Ok(generation) => generation, Err(error) => break Err(error) };
                let context = network.session(ConnectionDirection::Inbound, generation, cancellation.child_token(), None);
                let topology = network.topology.clone();
                tasks.spawn(async move {
                    // permit 与会话任务同寿命, 所有正常和异常返回都会归还入站容量.
                    let _permit = permit;
                    let _ = run_session(stream, source_address, context, move |snapshot, cancel| topology.register(snapshot, cancel, None)).await;
                    Ok(())
                });
            }
            joined = tasks.join_next(), if !tasks.is_empty() => {
                match joined {
                    Some(Ok(Err(error))) => break Err(error),
                    Some(Err(error)) => break Err(io::Error::other(format!("peer child task failed: {error}"))),
                    Some(Ok(Ok(()))) | None => {},
                }
            }
        }
    };
    // 先取消再完整回收, 首个错误不能让后续任务跳过 join.
    cancellation.cancel();
    network.topology.cancel_dials();
    while let Some(joined) = tasks.join_next().await {
        match joined {
            Ok(Ok(())) => {}
            Ok(Err(error)) if result.is_ok() => result = Err(error),
            Err(error) if result.is_ok() => result = Err(io::Error::other(format!("peer child task failed during shutdown: {error}"))),
            Ok(Err(_)) | Err(_) => {}
        }
    }
    result
}

/// 每个候选地址一个串行 supervisor. 已有同 ID 出站连接时暂停, 不让地址别名相互抢占连接.
async fn supervise_candidate(candidate: Arc<DialCandidate>, network: Arc<NetworkContext>) -> io::Result<()> {
    let cancellation = &candidate.cancellation;
    let mut changes = network.topology.subscribe();
    let mut failures = 0_u32;
    loop {
        changes.borrow_and_update();
        if cancellation.is_cancelled() {
            return Ok(());
        }
        if !network.topology.should_dial(&candidate)? {
            tokio::select! {
                biased;
                () = cancellation.cancelled() => return Ok(()),
                result = changes.changed() => result.map_err(|_| io::Error::other("topology notifications closed"))?,
            }
            continue;
        }
        // 同时限制并发 connect 和全节点拨号起始频率. 异步 Mutex 只串行化节拍领取,
        // 不持有拓扑的阻塞锁, 也不让慢 connect 占据整个拨号调度器.
        let admission = async {
            let permit = network.dial_permits.acquire().await.map_err(|_| io::Error::other("dial admission closed"))?;
            network.dial_clock.lock().await.tick().await;
            Ok::<_, io::Error>(permit)
        };
        let permit = tokio::select! {
            biased;
            () = cancellation.cancelled() => return Ok(()),
            result = admission => result?,
        };
        // 等待节拍期间可能已有另一地址完成同 ID 握手, 必须在 connect 前重新检查.
        if !network.topology.should_dial(&candidate)? {
            continue;
        };
        let connection = tokio::select! {
            biased;
            () = cancellation.cancelled() => return Ok(()),
            result = time::timeout(network.config.connect_timeout, TcpStream::connect(candidate.address)) => result,
        }
        .unwrap_or_else(|_| Err(io::Error::new(io::ErrorKind::TimedOut, "peer connect timed out")));
        drop(permit);
        let mut established = None;
        match connection {
            Ok(stream) => {
                let generation = next_generation(&network.generations)?;
                let context = network.session(
                    ConnectionDirection::Outbound,
                    generation,
                    cancellation.child_token(),
                    Some(candidate.peer_id.clone()),
                );
                let _ = run_session(stream, candidate.address, context, |snapshot, cancel| {
                    let lease = network.topology.register(snapshot, cancel, Some(&candidate))?;
                    established = Some(Instant::now());
                    Ok(lease)
                })
                .await;
                if cancellation.is_cancelled() {
                    return Ok(());
                }
                failures = if established.is_some_and(|start| start.elapsed() >= network.config.stable_connection) {
                    0
                } else {
                    failures.saturating_add(1)
                };
            }
            Err(error) => {
                emit(
                    &network.events,
                    PeerEvent::ConnectFailed {
                        address: candidate.address,
                        error_kind: error.kind(),
                    },
                );
                failures = failures.saturating_add(1);
            }
        }
        let delay = retry_delay(
            candidate.address,
            failures,
            network.config.reconnect_min,
            network.config.reconnect_max,
            jitter_salt(network.process.id.as_bytes()),
        );
        // 发现变化不能越过原有退避. 只有候选淘汰或 Peer 关闭会提前打断等待.
        tokio::select! {
            () = cancellation.cancelled() => return Ok(()),
            () = time::sleep(delay) => {},
        }
    }
}
