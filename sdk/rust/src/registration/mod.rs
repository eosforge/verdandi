//! 强类型 Registration 生命周期与 Selector 同步。
//!
//! [`Registration`] 在应用准备完毕后才发布；[`Selector`] 维护经过同步确认的本地
//! Registry 视图，并执行进程内选择事务。

mod client;
mod clock;
pub(crate) mod config;
mod deadline;
mod event;
mod pending;
pub(crate) mod script;
mod selector;

pub use client::Client;
pub use config::{Config, RegistrationLimits};
pub use selector::{Candidate, CandidateRef, Candidates, Choice, Meta, RetainedCandidate, SelectionSnapshot, Selector, SelectorOptions};

use std::collections::BTreeMap;
use std::marker::PhantomData;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use tokio::sync::{Mutex as AsyncMutex, Notify, OwnedSemaphorePermit, Semaphore, broadcast, mpsc, oneshot};

use self::client::{ActiveGuard, ClientInner};
use self::script::{RegistrationScriptKind, register_arguments, renew_arguments, unregister_arguments, update_arguments};
use crate::Fields;
use crate::error::{Code, Error, Result};
use crate::fields::{FieldValue, MAX_HASH_FIELD_EXPIRE_AT_MS, MAX_SAFE_INTEGER, encode_value, same_field_structure, validate_field, validate_record};
use crate::identifier::valid_type;

/// 一个 Registration 的完整进程启动状态。
#[derive(Clone, Debug)]
pub(crate) struct RegistrationConfig {
    /// Client Zone 内的 Registry Type。
    pub type_name: String,
    /// 固定 Redis 租约时长，必须能精确表示为毫秒。
    pub ttl: Duration,
    /// 自动续期间隔；`None` 使用 `ttl` 的三分之一。
    pub renew_interval: Option<Duration>,
    /// 供上层路由或兼容性策略使用的正应用 Version。
    pub version: u64,
    /// 生命周期内不可变的固定顶层 Attr 结构。
    pub attr: Fields,
    /// 字段集合固定、字段值可变的顶层 Data 结构。
    pub data: Fields,
}

/// 一次非空内容变化。
#[derive(Clone, Debug, Default)]
pub(crate) struct Update {
    /// 新的正应用 Version；`None` 表示保持不变。
    pub version: Option<u64>,
    /// 已变化的 Data 值；每个字段必须已经存在于固定结构中。
    pub data: Fields,
}

/// 一个 Registration 的单格 Fields 合并邮箱。
///
/// 后写的同名 Data 字段和 Version 覆盖先写值；等待者数量由独立 Semaphore 限制，
/// 因此这里不会形成请求对象队列，也不会为每次 Update 保留完整结构副本。
#[derive(Default)]
struct RegistrationMailbox {
    version: Option<u64>,
    data: Fields,
    updates: Vec<RegistrationWaiter>,
    renews: Vec<RegistrationWaiter>,
}

/// 一次已进入邮箱操作的结果端与容量许可。
///
/// permit 跟随邮箱而不是调用 Future：即使调用方丢弃 Future，容量也要等 worker
/// 处理或拒绝该操作后才归还，避免取消风暴绕过缓冲上限。
struct RegistrationWaiter {
    result: oneshot::Sender<Result<()>>,
    _permit: OwnedSemaphorePermit,
}

/// worker 从邮箱一次性接管的拥有型批次。
struct RegistrationBatch {
    version: Option<u64>,
    data: Fields,
    updates: Vec<RegistrationWaiter>,
    renews: Vec<RegistrationWaiter>,
}

impl RegistrationMailbox {
    /// 合并一个已验证 Update；同名 Data 和 Version 使用最后进入临界区的值。
    fn merge_update(&mut self, update: Update, result: oneshot::Sender<Result<()>>, permit: OwnedSemaphorePermit) {
        if update.version.is_some() {
            self.version = update.version;
        }
        self.data.extend(update.data);
        self.updates.push(RegistrationWaiter { result, _permit: permit });
    }

    /// 记录一个不携带 Fields 的显式 Renew 等待者。
    fn push_renew(&mut self, result: oneshot::Sender<Result<()>>, permit: OwnedSemaphorePermit) {
        self.renews.push(RegistrationWaiter { result, _permit: permit });
    }

    /// 接管当前全部内容；空邮箱不产生批次。
    fn take(&mut self) -> Option<RegistrationBatch> {
        if self.updates.is_empty() && self.renews.is_empty() {
            return None;
        }
        Some(RegistrationBatch {
            version: self.version.take(),
            data: std::mem::take(&mut self.data),
            updates: std::mem::take(&mut self.updates),
            renews: std::mem::take(&mut self.renews),
        })
    }
}

struct RegistrationState {
    revision: u64,
    timestamp: u64,
    ttl_ms: u64,
    version: u64,
    attr: Fields,
    data: Fields,
    uncertain: bool,
    healthy: bool,
}

struct RegistrationShared {
    client: Arc<ClientInner>,
    type_name: String,
    uuid: String,
    renew_interval: Duration,
    data_shape: Fields,
    wake: mpsc::Sender<()>,
    admission: Arc<Semaphore>,
    mailbox: Mutex<RegistrationMailbox>,
    closed: AtomicBool,
    close_notify: Notify,
    done: Notify,
    finished: AtomicBool,
    final_error: Mutex<Option<Error>>,
    revision: AtomicU64,
    timestamp: AtomicU64,
    errors: broadcast::Sender<Error>,
}

/// 一个进程启动 UUID 的完整生命周期核心，拥有一个串行交接点和一个同步任务。
pub(crate) struct RegistrationCore {
    shared: Arc<RegistrationShared>,
}

/// 本地强类型 Registration 配置；构造不执行 Redis I/O，也不启动续期任务。
#[derive(Clone, Debug)]
pub struct RegistrationOptions {
    /// Client Zone 内的 Registry Type。
    pub type_name: String,
    /// 固定 Redis 租约时长，必须能精确表示为毫秒。
    pub ttl: Duration,
    /// 自动续期间隔；`None` 使用 `ttl` 的三分之一。
    pub renew_interval: Option<Duration>,
    /// 供上层路由或兼容性策略使用的正应用 Version。
    pub version: u64,
}

/// 一个由应用拥有的强类型 Registration 生命周期。
pub struct Registration<A: FieldValue, D: FieldValue> {
    client: Arc<ClientInner>,
    options: RegistrationOptions,
    uuid: String,
    core: OnceLock<RegistrationCore>,
    data_shape: OnceLock<Fields>,
    lifecycle: AsyncMutex<()>,
    terminal: AtomicBool,
    marker: PhantomData<fn() -> (A, D)>,
}

impl<A: FieldValue, D: FieldValue> Registration<A, D> {
    /// 创建新进程 UUID，但不发布状态或启动任务。
    ///
    /// `client` 提供 Zone 与生命周期，`options` 定义 Type/TTL/续期/Version；构造仅做本地校验。
    /// 同一对象后续 Register 失败可用原 UUID 重试，进程重启则必须构造新对象获得新 UUID。
    pub fn new(client: &Client, options: RegistrationOptions) -> Result<Self> {
        if !valid_type(&options.type_name) {
            return Err(Error::field(Code::Invalid, "type"));
        }
        if options.version == 0 || options.version > MAX_SAFE_INTEGER {
            return Err(Error::field(Code::Invalid, "@version"));
        }
        duration_milliseconds(options.ttl)?;
        let renew_interval = options.renew_interval.unwrap_or(options.ttl / 3);
        if renew_interval < client.inner.config.minimum_renew_interval || renew_interval > options.ttl / 3 {
            return Err(Error::field(Code::Invalid, "renew_interval"));
        }
        let uuid = new_uuid()?;
        Ok(Self {
            client: Arc::clone(&client.inner),
            options,
            uuid,
            core: OnceLock::new(),
            data_shape: OnceLock::new(),
            lifecycle: AsyncMutex::new(()),
            terminal: AtomicBool::new(false),
            marker: PhantomData,
        })
    }

    /// 返回构造时分配、在本对象生命周期内不变的进程 UUID。
    pub fn uuid(&self) -> &str {
        &self.uuid
    }

    /// 报告完整状态是否已成功发布且唯一 worker 尚未终止。
    pub fn is_registered(&self) -> bool {
        self.core.get().is_some_and(|core| !core.shared.finished.load(Ordering::Acquire)) && !self.terminal.load(Ordering::Acquire)
    }

    /// 返回最新期望内容 revision；首次 Register 成功前为零。
    pub fn revision(&self) -> u64 {
        self.core.get().map_or(0, RegistrationCore::revision)
    }

    /// 返回最近一次 Redis 确认的 Unix 毫秒 timestamp；首次 Register 成功前为零。
    pub fn timestamp(&self) -> u64 {
        self.core.get().map_or(0, RegistrationCore::timestamp)
    }

    /// 首次 Register 成功后订阅有界续期与恢复诊断；尚未 Register 时返回 `None`。
    pub fn subscribe_errors(&self) -> Option<broadcast::Receiver<Error>> {
        self.core.get().map(RegistrationCore::subscribe_errors)
    }

    /// 在应用准备完毕后发布完整 `attr` 和 `data`。
    ///
    /// 两个强类型值先编码并按当前 Redis Zone 配置验证；成功后启动此 UUID 唯一 worker。
    /// 重复 Register 返回 Contract；失败不设置 core，调用方可在同一进程对象上重试。
    pub async fn register(&self, attr: &A, data: &D) -> Result<()> {
        let _lifecycle = self.lifecycle.lock().await;
        if self.terminal.load(Ordering::Acquire) {
            return Err(Error::new(Code::Closed));
        }
        if let Some(core) = self.core.get() {
            return Err(Error::field(Code::Contract, "register").with_revision(core.revision()));
        }
        // lifecycle 锁串行化 Register 与 Unregister，避免 core/terminal 发生交错发布。
        let encoded_attr = encode_value(attr, "attr")?;
        let encoded_data = encode_value(data, "data")?;
        let core = self
            .client
            .register_with_uuid(
                RegistrationConfig {
                    type_name: self.options.type_name.clone(),
                    ttl: self.options.ttl,
                    renew_interval: self.options.renew_interval,
                    version: self.options.version,
                    attr: encoded_attr,
                    data: encoded_data.clone(),
                },
                self.uuid.clone(),
            )
            .await?;
        let data_shape = encoded_data.into_keys().map(|name| (name, Vec::new())).collect();
        self.data_shape.set(data_shape).map_err(|_| Error::field(Code::Corrupt, "registration"))?;
        self.core.set(core).map_err(|_| Error::field(Code::Corrupt, "registration"))?;
        Ok(())
    }

    /// 编码完整期望 `data`，但只向 Redis 发送实际变化的顶层字段。
    ///
    /// Data 字段集合必须与首次 Register 完全一致；精确 no-op 不访问 Redis或推进 revision。
    pub async fn update(&self, data: &D) -> Result<()> {
        self.update_inner(None, data).await
    }

    /// 只修改应用 `version` 和内容 revision，不改 Data。
    ///
    /// Version 必须是安全范围内正整数；相同 Version 是 no-op。
    pub async fn set_version(&self, version: u64) -> Result<()> {
        if self.terminal.load(Ordering::Acquire) {
            return Err(Error::new(Code::Closed));
        }
        if version == 0 || version > MAX_SAFE_INTEGER {
            return Err(Error::field(Code::Invalid, "@version"));
        }
        let core = self.core.get().ok_or_else(|| Error::field(Code::Contract, "register"))?;
        core.update(Update {
            version: Some(version),
            data: Fields::new(),
        })
        .await
    }

    /// 在同一次原子 Update 中修改 `version` 和完整期望 `data` 中的实际变化字段。
    pub async fn update_content(&self, version: u64, data: &D) -> Result<()> {
        self.update_inner(Some(version), data).await
    }

    /// 立即刷新固定租约，但不改变内容 revision。
    ///
    /// 成功会重置下一次自动续期计时；尚未 Register 或已终止返回错误。
    pub async fn renew(&self) -> Result<()> {
        if self.terminal.load(Ordering::Acquire) {
            return Err(Error::new(Code::Closed));
        }
        let core = self.core.get().ok_or_else(|| Error::field(Code::Contract, "register"))?;
        core.renew().await
    }

    /// 终止本地句柄，并在已经发布且传输健康时优雅 Unregister。
    ///
    /// 调用会封住新操作、等待已交接请求并等待唯一 worker；Register 前调用仅标记终止。
    pub async fn unregister(&self) -> Result<()> {
        let _lifecycle = self.lifecycle.lock().await;
        self.terminal.store(true, Ordering::Release);
        let Some(core) = self.core.get() else {
            return Ok(());
        };
        core.close().await
    }

    /// [`Self::unregister`] 的惯用资源清理别名，语义完全一致。
    pub async fn close(&self) -> Result<()> {
        self.unregister().await
    }

    /// 统一实现带可选 `version` 的完整 Data 更新。
    ///
    /// `data` 编码后必须与首次注册的固定字段结构一致；仅差异字段交给单 Registration worker。
    async fn update_inner(&self, version: Option<u64>, data: &D) -> Result<()> {
        if self.terminal.load(Ordering::Acquire) {
            return Err(Error::new(Code::Closed));
        }
        if version.is_some_and(|value| value == 0 || value > MAX_SAFE_INTEGER) {
            return Err(Error::field(Code::Invalid, "@version"));
        }
        let core = self.core.get().ok_or_else(|| Error::field(Code::Contract, "register"))?;
        let encoded = encode_value(data, "data")?;
        let data_shape = self.data_shape.get().ok_or_else(|| Error::field(Code::Corrupt, "registration"))?;
        if !same_field_structure(data_shape, &encoded) {
            return Err(Error::field(Code::Contract, "data").with_revision(core.revision()));
        }
        core.update(Update { version, data: encoded }).await
    }
}

impl ClientInner {
    /// 用指定 `uuid` 发布完整 `config` 并创建此 Registration 的唯一 worker。
    ///
    /// 此内部入口刷新 Zone 配置、验证完整记录、建立单项交接通道，并等待初始 Register 确认。
    /// 任一步失败会尽力清理 Redis 状态且不返回半初始化核心。
    pub(crate) async fn register_with_uuid(self: &Arc<Self>, config: RegistrationConfig, uuid: String) -> Result<RegistrationCore> {
        if !valid_type(&config.type_name) {
            return Err(Error::field(Code::Invalid, "type"));
        }
        let ttl_ms = duration_milliseconds(config.ttl)?;
        let renew_interval = config.renew_interval.unwrap_or(config.ttl / 3);
        if renew_interval < self.config.minimum_renew_interval || renew_interval > config.ttl / 3 {
            return Err(Error::field(Code::Invalid, "renew_interval"));
        }

        // 每个新 Registration 在发布前显式刷新策略，确保后台可修改配置尽快生效。
        self.refresh_configuration().await?;
        let limits = self.limits.load_full();
        validate_record(&uuid, 1, ttl_ms, config.version, &config.attr, &config.data, &limits)?;
        let guard = self.admit()?;
        let data_shape = config.data.keys().map(|name| (name.clone(), Vec::new())).collect();
        // MPSC 只携带容量一的唤醒信号；Fields、Version 和等待者由短临界区邮箱直接合并。
        let (wake, receiver) = mpsc::channel(1);
        let (errors, _) = broadcast::channel(self.config.registration_error_buffer_capacity);
        let shared = Arc::new(RegistrationShared {
            client: Arc::clone(self),
            type_name: config.type_name,
            uuid,
            renew_interval,
            data_shape,
            wake,
            admission: Arc::new(Semaphore::new(self.config.registration_buffer_capacity)),
            mailbox: Mutex::new(RegistrationMailbox::default()),
            closed: AtomicBool::new(false),
            close_notify: Notify::new(),
            done: Notify::new(),
            finished: AtomicBool::new(false),
            final_error: Mutex::new(None),
            revision: AtomicU64::new(0),
            timestamp: AtomicU64::new(0),
            errors,
        });
        let state = RegistrationState {
            revision: 1,
            timestamp: 0,
            ttl_ms,
            version: config.version,
            attr: config.attr,
            data: config.data,
            uncertain: false,
            healthy: false,
        };
        // worker 串行拥有 desired/confirmed 状态和续期计时器；调用方只在短临界区合并 Fields。
        let (ready, ready_receiver) = oneshot::channel();
        let worker = Arc::clone(&shared);
        tokio::spawn(async move {
            run_registration(worker, guard, receiver, state, ready).await;
        });
        receive_result(ready_receiver).await?;
        Ok(RegistrationCore { shared })
    }
}

impl RegistrationCore {
    /// 返回唯一 worker 已接纳的最新期望内容 revision。
    pub fn revision(&self) -> u64 {
        self.shared.revision.load(Ordering::Acquire)
    }

    /// 返回最近一次 Redis 确认的 Unix 毫秒 timestamp。
    pub fn timestamp(&self) -> u64 {
        self.shared.timestamp.load(Ordering::Acquire)
    }

    /// 订阅有界异步续期与恢复失败；落后接收者可能丢失旧诊断。
    pub fn subscribe_errors(&self) -> broadcast::Receiver<Error> {
        self.shared.errors.subscribe()
    }

    /// 把一个已由 SDK 拥有的 `update` 合并进此 Registration 的单格 Fields 邮箱。
    ///
    /// permit 限制同时等待结果的调用数；同名字段以后写覆盖先写，worker 每次接管一份合并状态。
    /// 精确 no-op 仍由 worker 依据当前 desired state 判定，不访问 Redis或推进 revision。
    pub async fn update(&self, update: Update) -> Result<()> {
        self.shared.validate_buffered_update(&update)?;
        let admission = Arc::clone(&self.shared.admission).acquire_owned().await.map_err(|_| Error::new(Code::Closed))?;
        if self.shared.closed.load(Ordering::Acquire) {
            return Err(Error::new(Code::Closed));
        }
        let (sender, receiver) = oneshot::channel();
        {
            let mut mailbox = self.shared.mailbox.lock().map_err(|_| Error::new(Code::Corrupt))?;
            if self.shared.closed.load(Ordering::Acquire) {
                return Err(Error::new(Code::Closed));
            }
            mailbox.merge_update(update, sender, admission);
        }
        self.shared.signal();
        receive_result(receiver).await
    }

    /// 立即交接一次 Renew，刷新固定租约而不改变内容 revision。
    ///
    /// Renew 与 Update 共用等待者容量，但不占用 Data 字段；同批有效 Update 已刷新 TTL 时不重复写 Redis。
    pub async fn renew(&self) -> Result<()> {
        let admission = Arc::clone(&self.shared.admission).acquire_owned().await.map_err(|_| Error::new(Code::Closed))?;
        if self.shared.closed.load(Ordering::Acquire) {
            return Err(Error::new(Code::Closed));
        }
        let (sender, receiver) = oneshot::channel();
        {
            let mut mailbox = self.shared.mailbox.lock().map_err(|_| Error::new(Code::Corrupt))?;
            if self.shared.closed.load(Ordering::Acquire) {
                return Err(Error::new(Code::Closed));
            }
            mailbox.push_renew(sender, admission);
        }
        self.shared.signal();
        receive_result(receiver).await
    }

    /// 封住新写入、等待当前已交接操作、删除健康 Redis 状态并等待唯一 worker 终止。
    ///
    /// 重复调用返回相同终止结果；不确定或不健康状态不冒险发送可能影响恢复语义的清理。
    pub async fn close(&self) -> Result<()> {
        if !self.shared.closed.swap(true, Ordering::AcqRel) {
            self.shared.admission.close();
            self.shared.close_notify.notify_one();
        }
        self.shared.wait_finished().await;
        self.shared.terminal_result()
    }
}

impl Drop for RegistrationCore {
    /// 释放核心时发出非阻塞终止通知；Drop 不能等待，显式 `close().await` 提供确定汇合。
    fn drop(&mut self) {
        if !self.shared.closed.swap(true, Ordering::AcqRel) {
            self.shared.admission.close();
            self.shared.close_notify.notify_one();
        }
    }
}

/// 运行一个 Registration 的唯一串行生命周期 worker。
///
/// `registration`/`_guard` 保持领域与 UUID 状态存活，`receiver` 只承载该 UUID 的容量一唤醒，
/// `state` 只由此任务修改，`ready` 返回首次完整 Register 结果。退出前释放共享配置刷新租约。
async fn run_registration(
    registration: Arc<RegistrationShared>,
    _guard: ActiveGuard,
    mut receiver: mpsc::Receiver<()>,
    mut state: RegistrationState,
    ready: oneshot::Sender<Result<()>>,
) {
    // 首次完整 Register 成功并验证回复后，才发布 revision/timestamp 和启动配置刷新租约。
    let operation = registration
        .client
        .call_registration(
            RegistrationScriptKind::Register,
            &registration.type_name,
            &registration.uuid,
            register_arguments(&registration.uuid, state.revision, state.ttl_ms, state.version, &state.attr, &state.data),
            true,
        )
        .await;
    let reply = match operation {
        Ok(reply) if reply.revision == state.revision && reply.timestamp != 0 => reply,
        Ok(reply) => {
            registration.client.best_effort_unregister(&registration.type_name, &registration.uuid).await;
            let error = Error::field(Code::Corrupt, "reply").with_revision(reply.revision);
            let _ = ready.send(Err(error.clone()));
            registration.complete(Some(error));
            return;
        }
        Err(error) => {
            registration.client.best_effort_unregister(&registration.type_name, &registration.uuid).await;
            let _ = ready.send(Err(error.clone()));
            registration.complete(Some(error));
            return;
        }
    };
    state.timestamp = reply.timestamp;
    state.healthy = true;
    registration.revision.store(state.revision, Ordering::Release);
    registration.timestamp.store(state.timestamp, Ordering::Release);
    let configuration = match registration.client.acquire_configuration_refresh() {
        Ok(configuration) => configuration,
        Err(error) => {
            registration.client.best_effort_unregister(&registration.type_name, &registration.uuid).await;
            let _ = ready.send(Err(error.clone()));
            registration.complete(Some(error));
            return;
        }
    };
    if ready.send(Ok(())).is_err() {
        registration.client.best_effort_unregister(&registration.type_name, &registration.uuid).await;
        let error = configuration.release().await.err();
        registration.complete(error);
        return;
    }

    // 一个可重置 Sleep 同时服务自动续期；任何成功内容写入或显式 Renew 都重置下一周期。
    let renewal = tokio::time::sleep(bounded_timer(jittered(
        registration.renew_interval,
        registration.client.config.renew_jitter_percent,
    )));
    tokio::pin!(renewal);
    let terminal_error = loop {
        // biased 顺序优先领域关闭与显式 Close；唤醒只提示邮箱可能存在一份合并状态。
        tokio::select! {
            biased;
            () = registration.client.shutdown.cancelled() => {
                break shutdown_registration(&registration, &state, &mut receiver).await;
            }
            () = registration.close_notify.notified() => {
                break close_registration(&registration, &mut state, &mut receiver).await;
            }
            signal = receiver.recv() => {
                let Some(()) = signal else {
                    break shutdown_registration(&registration, &state, &mut receiver).await;
                };
                if let Some(batch) = registration.take_batch() {
                    let wrote = handle_registration_batch(&registration, &mut state, batch).await;
                    if wrote {
                        reset_renewal(
                            renewal.as_mut(),
                            registration.renew_interval,
                            registration.client.config.renew_jitter_percent,
                        );
                    } else if renewal.as_ref().is_elapsed() {
                        let result = registration.renew_state(&mut state).await;
                        registration.report_result(&result);
                        reset_renewal(
                            renewal.as_mut(),
                            registration.renew_interval,
                            registration.client.config.renew_jitter_percent,
                        );
                    }
                }
            }
            () = &mut renewal => {
                let result = registration.renew_state(&mut state).await;
                registration.report_result(&result);
                reset_renewal(
                    renewal.as_mut(),
                    registration.renew_interval,
                    registration.client.config.renew_jitter_percent,
                );
            }
        }
    };
    let release_error = configuration.release().await.err();
    registration.complete(terminal_error.or(release_error));
}

/// 处理一份从邮箱原子接管的 Fields 批次。
///
/// Update 成功且实际推进 revision 时已经刷新 TTL，同批 Renew 共享成功；Update no-op 或失败时，
/// Renew 仍独立执行。返回值表示本批是否成功完成一次 TTL 写入。
async fn handle_registration_batch(registration: &Arc<RegistrationShared>, state: &mut RegistrationState, batch: RegistrationBatch) -> bool {
    let mut wrote = false;
    if !batch.updates.is_empty() {
        let previous_revision = state.revision;
        let outcome = registration
            .update_state(
                state,
                Update {
                    version: batch.version,
                    data: batch.data,
                },
            )
            .await;
        wrote = outcome.is_ok() && state.revision != previous_revision;
        complete_registration_waiters(batch.updates, &outcome);
    }
    if !batch.renews.is_empty() {
        let outcome = if wrote { Ok(()) } else { registration.renew_state(state).await };
        wrote = wrote || outcome.is_ok();
        complete_registration_waiters(batch.renews, &outcome);
    }
    wrote
}

/// 显式 Registration Close 时排空已经进入 Fields 邮箱的工作，再执行健康状态 Unregister。
async fn close_registration(registration: &Arc<RegistrationShared>, state: &mut RegistrationState, receiver: &mut mpsc::Receiver<()>) -> Option<Error> {
    receiver.close();
    while let Some(batch) = registration.take_batch() {
        handle_registration_batch(registration, state, batch).await;
    }
    registration.finish(state).await.err()
}

/// 领域或根关闭时停止准入、拒绝尚未取走的单项交接，并有界尝试健康状态清理。
///
/// 与显式 Registration Close 相同，此路径不执行尚未开始的业务写；`receiver` 中请求统一返回 Closed。
async fn shutdown_registration(registration: &Arc<RegistrationShared>, state: &RegistrationState, receiver: &mut mpsc::Receiver<()>) -> Option<Error> {
    registration.closed.store(true, Ordering::Release);
    registration.admission.close();
    receiver.close();
    registration.reject_mailbox();
    let result = tokio::time::timeout(registration.client.config.timeout, registration.finish(state)).await;
    match result {
        Ok(Ok(())) => None,
        Ok(Err(error)) => Some(error),
        Err(error) => Some(Error::driver(Code::Deadline, error)),
    }
}

/// 用同一 `outcome` 完成一组共享批次结果的等待者。
fn complete_registration_waiters(waiters: Vec<RegistrationWaiter>, outcome: &Result<()>) {
    for waiter in waiters {
        let _ = waiter.result.send(outcome.clone());
    }
}

/// 把自动 `renewal` 重置到当前时间加抖动后的 `interval`，并限制极端 Timer 时长。
fn reset_renewal(mut renewal: std::pin::Pin<&mut tokio::time::Sleep>, interval: Duration, jitter_percent: u8) {
    renewal
        .as_mut()
        .reset(tokio::time::Instant::now() + bounded_timer(jittered(interval, jitter_percent)));
}

/// 判断 `error` 是否意味着写入结果或回复内容不确定，因而必须以完整 Register 恢复。
fn uncertain_registration_outcome(error: &Error) -> bool {
    matches!(error.code(), Code::Ambiguous | Code::Corrupt)
}

impl RegistrationShared {
    /// 在进入共享邮箱前完成每个调用可独立判断的 Version、字段结构和值容量校验。
    ///
    /// 合并后完整记录大小和 revision 上限仍由唯一 worker 针对最终 desired state 校验。
    fn validate_buffered_update(&self, update: &Update) -> Result<()> {
        if update.version.is_none() && update.data.is_empty() {
            return Err(Error::field(Code::Contract, "update"));
        }
        if update.version.is_some_and(|version| version == 0 || version > MAX_SAFE_INTEGER) {
            return Err(Error::field(Code::Invalid, "@version"));
        }
        let limits = self.client.limits.load_full();
        for (name, value) in &update.data {
            if !self.data_shape.contains_key(name) {
                return Err(Error::field(Code::Contract, name));
            }
            validate_field(name, value, limits.field_name_max_bytes, limits.data_value_max_bytes)?;
        }
        Ok(())
    }

    /// 发送一个可合并的容量一唤醒；邮箱已经保存完整工作，重复唤醒无需排队。
    fn signal(&self) {
        match self.wake.try_send(()) {
            Ok(()) | Err(mpsc::error::TrySendError::Full(()) | mpsc::error::TrySendError::Closed(())) => {}
        }
    }

    /// 原子接管当前 Fields 邮箱并清空共享引用，让后续调用立即形成下一批。
    fn take_batch(&self) -> Option<RegistrationBatch> {
        // 临界区不调用应用代码或执行 await；若外部 panic 使锁中毒，取回拥有值仍比永久悬挂等待者安全。
        let mut mailbox = self.mailbox.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        mailbox.take()
    }

    /// 让异常关闭时仍留在邮箱中的全部等待者立即收到 Closed。
    fn reject_mailbox(&self) {
        let Some(batch) = self.take_batch() else {
            return;
        };
        let outcome = Err(Error::new(Code::Closed));
        complete_registration_waiters(batch.updates, &outcome);
        complete_registration_waiters(batch.renews, &outcome);
    }

    /// 在唯一 worker 内验证并应用一个实际 Update。
    ///
    /// `state` 同时保存期望和最近确认状态；`update` 只含可选 Version 与 Data 候选。
    /// 确定失败不改变 desired；Ambiguous/Corrupt 会提交 desired 并标记 uncertain，供下次完整 Register 对齐。
    async fn update_state(&self, state: &mut RegistrationState, update: Update) -> Result<()> {
        if update.version.is_none() && update.data.is_empty() {
            return Err(Error::field(Code::Contract, "update"));
        }
        let version = update.version.unwrap_or(state.version);
        if version == 0 || version > MAX_SAFE_INTEGER {
            return Err(Error::field(Code::Invalid, "@version"));
        }

        let mut changed = BTreeMap::new();
        for (name, value) in update.data {
            let Some(current) = state.data.get(&name) else {
                return Err(Error::field(Code::Contract, name));
            };
            if current != &value {
                changed.insert(name, value);
            }
        }
        let version_changed = version != state.version;
        if !version_changed && changed.is_empty() {
            return Ok(());
        }
        if state.revision >= MAX_SAFE_INTEGER {
            return Err(Error::field(Code::Capacity, "@revision").with_revision(state.revision));
        }

        // Redis 参数 Vec 拥有编码 patch；把原 changed 值移入 desired state，避免固定结构 map 复制后再克隆一次值。
        let incremental_arguments = (!state.uncertain).then(|| update_arguments(&self.uuid, state.revision + 1, version_changed.then_some(version), &changed));
        let next_data = if changed.is_empty() {
            None
        } else {
            let mut data = state.data.clone();
            data.extend(changed);
            Some(data)
        };
        let desired_data = next_data.as_ref().unwrap_or(&state.data);
        let next_revision = state.revision + 1;
        let limits = self.client.limits.load_full();
        validate_record(&self.uuid, next_revision, state.ttl_ms, version, &state.attr, desired_data, &limits)?;

        let result = if state.uncertain {
            self.client
                .call_registration(
                    RegistrationScriptKind::Register,
                    &self.type_name,
                    &self.uuid,
                    register_arguments(&self.uuid, next_revision, state.ttl_ms, version, &state.attr, desired_data),
                    true,
                )
                .await
        } else {
            let arguments = incremental_arguments.ok_or_else(|| Error::field(Code::Corrupt, "update_arguments"))?;
            let first = self
                .client
                .call_registration(RegistrationScriptKind::Update, &self.type_name, &self.uuid, arguments, true)
                .await;
            // 异步副本提升后可能只含较旧确认 revision；完整 Register 同时修复缺失和明确 backend-behind transition。
            if matches!(&first, Err(error) if matches!(error.code(), Code::Missing | Code::Transition)) {
                self.client
                    .call_registration(
                        RegistrationScriptKind::Register,
                        &self.type_name,
                        &self.uuid,
                        register_arguments(&self.uuid, next_revision, state.ttl_ms, version, &state.attr, desired_data),
                        true,
                    )
                    .await
            } else {
                first
            }
        };

        match result {
            Ok(reply) => {
                if reply.revision != next_revision || reply.timestamp == 0 {
                    self.commit_desired(state, next_revision, version, next_data);
                    state.uncertain = true;
                    state.healthy = false;
                    return Err(Error::field(Code::Corrupt, "reply"));
                }
                self.commit_desired(state, next_revision, version, next_data);
                state.timestamp = reply.timestamp;
                state.uncertain = false;
                state.healthy = true;
                self.timestamp.store(reply.timestamp, Ordering::Release);
                Ok(())
            }
            Err(error) => {
                if uncertain_registration_outcome(&error) {
                    self.commit_desired(state, next_revision, version, next_data);
                    state.uncertain = true;
                    state.healthy = false;
                }
                Err(error)
            }
        }
    }

    /// 提交新的期望 `revision`、`version` 和可选完整 `data`，并原子发布公开 revision。
    fn commit_desired(&self, state: &mut RegistrationState, revision: u64, version: u64, data: Option<Fields>) {
        state.revision = revision;
        state.version = version;
        if let Some(data) = data {
            state.data = data;
        }
        self.revision.store(revision, Ordering::Release);
    }

    /// 在唯一 worker 内刷新租约或用完整 Register 修复不确定状态。
    ///
    /// `state` revision 不变化；成功更新 Redis timestamp 和健康标志，Ambiguous/Corrupt 标记 uncertain。
    async fn renew_state(&self, state: &mut RegistrationState) -> Result<()> {
        let result = if state.uncertain {
            self.client
                .call_registration(
                    RegistrationScriptKind::Register,
                    &self.type_name,
                    &self.uuid,
                    register_arguments(&self.uuid, state.revision, state.ttl_ms, state.version, &state.attr, &state.data),
                    true,
                )
                .await
        } else {
            let first = self
                .client
                .call_registration(
                    RegistrationScriptKind::Renew,
                    &self.type_name,
                    &self.uuid,
                    renew_arguments(&self.uuid, state.revision),
                    true,
                )
                .await;
            if matches!(&first, Err(error) if matches!(error.code(), Code::Missing | Code::Transition)) {
                self.client
                    .call_registration(
                        RegistrationScriptKind::Register,
                        &self.type_name,
                        &self.uuid,
                        register_arguments(&self.uuid, state.revision, state.ttl_ms, state.version, &state.attr, &state.data),
                        true,
                    )
                    .await
            } else {
                first
            }
        };

        match result {
            Ok(reply) => {
                if reply.revision != state.revision || reply.timestamp == 0 {
                    state.uncertain = true;
                    state.healthy = false;
                    return Err(Error::field(Code::Corrupt, "reply"));
                }
                state.timestamp = reply.timestamp;
                state.uncertain = false;
                state.healthy = true;
                self.timestamp.store(reply.timestamp, Ordering::Release);
                Ok(())
            }
            Err(error) => {
                if uncertain_registration_outcome(&error) {
                    state.uncertain = true;
                    state.healthy = false;
                }
                Err(error)
            }
        }
    }

    /// 在 `state` 健康且确定时发送精确 UUID Unregister。
    ///
    /// Missing 视为已经清理；不健康/不确定状态跳过写入，交由 TTL 删除。
    async fn finish(&self, state: &RegistrationState) -> Result<()> {
        if !state.healthy || state.uncertain {
            return Ok(());
        }
        match self
            .client
            .call_registration(
                RegistrationScriptKind::Unregister,
                &self.type_name,
                &self.uuid,
                unregister_arguments(&self.uuid),
                true,
            )
            .await
        {
            Err(error) if error.code() == Code::Missing => Ok(()),
            result => result.map(|_| ()),
        }
    }

    /// 把失败 `result` 同时广播给此 Registration 和领域 Client 的诊断通道。
    fn report_result(&self, result: &Result<()>) {
        if let Err(error) = result {
            let _ = self.errors.send(error.clone());
            self.client.report(error.clone());
        }
    }

    /// 最多一次发布 worker 终止，并可保存最终 `error` 供所有 Close 调用共享。
    fn complete(&self, error: Option<Error>) {
        if self.finished.load(Ordering::Acquire) {
            return;
        }
        if let Some(error) = error {
            if let Ok(mut stored) = self.final_error.lock() {
                *stored = Some(error);
            }
        }
        self.finished.store(true, Ordering::Release);
        self.done.notify_waiters();
    }

    /// 等待唯一 worker 发布 finished；Notify 在复查前启用以避免丢失唤醒。
    async fn wait_finished(&self) {
        while !self.finished.load(Ordering::Acquire) {
            let notified = self.done.notified();
            tokio::pin!(notified);
            notified.as_mut().enable();
            if self.finished.load(Ordering::Acquire) {
                break;
            }
            notified.await;
        }
    }

    /// 返回 worker 保存的终止错误；锁中毒映射为 Corrupt，无错误时成功。
    fn terminal_result(&self) -> Result<()> {
        self.final_error.lock().map_err(|_| Error::new(Code::Corrupt))?.clone().map_or(Ok(()), Err)
    }
}

impl ClientInner {
    /// 尽力删除 `type_name`/`uuid` 的 Redis Registration，不覆盖调用方主错误。
    async fn best_effort_unregister(&self, type_name: &str, uuid: &str) {
        let _ = self
            .call_registration(RegistrationScriptKind::Unregister, type_name, uuid, unregister_arguments(uuid), true)
            .await;
    }
}

/// 等待单次请求 `receiver` 并展开其 Result；发送端消失映射为 Closed。
async fn receive_result(receiver: oneshot::Receiver<Result<()>>) -> Result<()> {
    receiver.await.map_err(|_| Error::new(Code::Closed))?
}

/// 把正数、整毫秒精度 `value` 转为 Hash-field TTL 安全范围内的 `u64` 毫秒。
fn duration_milliseconds(value: Duration) -> Result<u64> {
    if value.is_zero() || value.subsec_nanos() % 1_000_000 != 0 {
        return Err(Error::field(Code::Invalid, "ttl"));
    }
    let value = u64::try_from(value.as_millis()).map_err(|_| Error::field(Code::Invalid, "ttl"))?;
    if value == 0 || value > MAX_HASH_FIELD_EXPIRE_AT_MS {
        return Err(Error::field(Code::Invalid, "ttl"));
    }
    Ok(value)
}

/// 用系统随机源生成 128 位 Registration UUID，并编码为 32 个小写十六进制字符。
fn new_uuid() -> Result<String> {
    let mut bytes = [0_u8; 16];
    getrandom::fill(&mut bytes).map_err(|error| Error::driver(Code::Unavailable, error))?;
    Ok(hex_lower(&bytes))
}

/// 把任意 `bytes` 编码为无分隔符的小写十六进制字符串。
fn hex_lower(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut output = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        output.push(char::from(HEX[usize::from(byte >> 4)]));
        output.push(char::from(HEX[usize::from(byte & 0x0f)]));
    }
    output
}

/// 在 `interval` 的正负 10% 范围采样 Duration，减少大量 Registration 同时续期。
fn jittered(interval: Duration, percent: u8) -> Duration {
    let milliseconds = interval.as_millis();
    let span = milliseconds.saturating_mul(u128::from(percent)) / 100;
    if span == 0 {
        return interval;
    }
    let low = milliseconds - span;
    let high = milliseconds + span;
    let sampled = fastrand::u128(low..=high);
    Duration::from_millis(u64::try_from(sampled).unwrap_or(u64::MAX))
}

/// 把极端 `value` 限制为 Tokio Timer 可稳定表示的一年；正常协议周期不受影响。
fn bounded_timer(value: Duration) -> Duration {
    value.min(Duration::from_secs(365 * 24 * 60 * 60))
}

#[cfg(test)]
#[path = "../../tests/internal/registration/mod.rs"]
mod tests;
