use std::collections::{BTreeSet, HashMap};
use std::marker::PhantomData;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use arc_swap::ArcSwap;
use fred::clients::SubscriberClient;
use fred::prelude::*;
use fred::types::config::Server;
use fred::types::scan::Scanner;
use fred::types::{Message, Value};
use futures_util::StreamExt;
use tokio::sync::{Mutex as AsyncMutex, Notify, broadcast, mpsc, oneshot};
use tokio::task::JoinHandle;
use tokio_util::sync::CancellationToken;

use super::client::{ActiveGuard, Client, ClientInner};
use super::clock::RedisClock;
use super::config::ZoneConfig;
use super::deadline::DeadlineQueue;
use super::event::{RegistrationEvent, StoredRecord, decode_registration_event, parse_stored_record, redis_positive_u64};
use super::pending::{PendingChange, PendingChanges};
use crate::Fields;
use crate::error::{Code, Error, Result};
use crate::fields::{FieldValue, MAX_HASH_FIELD_EXPIRE_AT_MS, clone_value, encode_value, registration_size, same_field_structure, validate_fields};
use crate::identifier::valid_type;

/// 一个 Registry 选择目标。
#[derive(Clone, Debug)]
pub(crate) struct SelectorConfig {
    /// Client Zone 内的 Registry Type。
    pub type_name: String,
}

/// 协议拥有的 Registration 元数据。
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Meta {
    /// 精确的进程启动身份。
    pub uuid: String,
    /// 每个 Registration 独立的内容 revision。
    pub revision: u64,
    /// 最近发布的 Redis Unix 毫秒 timestamp。
    pub timestamp: u64,
    /// 生命周期内不可变的毫秒租约时长。
    pub ttl: u64,
    /// 正应用 Version。
    pub version: u64,
}

struct SelectorRecord {
    meta: Meta,
    attr: Arc<Fields>,
    data: Arc<Fields>,
    deadline: u64,
    size: usize,
}

#[derive(Clone)]
struct RetainedSelectorRecord {
    record: Arc<SelectorRecord>,
    until: u64,
}

struct SelectorView {
    generation: u64,
    synchronized: bool,
    records: HashMap<String, Arc<SelectorRecord>>,
    ordered_records: Vec<Arc<SelectorRecord>>,
    retained: HashMap<String, RetainedSelectorRecord>,
    ordered_retained: Vec<RetainedSelectorRecord>,
}

#[derive(Clone)]
struct SelectorState {
    records: HashMap<String, Arc<SelectorRecord>>,
    deadlines: DeadlineQueue,
    bytes: usize,
    retained: HashMap<String, RetainedSelectorRecord>,
    retained_deadlines: DeadlineQueue,
    retained_bytes: usize,
}

struct Generation {
    subscriber: SubscriberClient,
    channel: String,
    messages: broadcast::Receiver<Message>,
    reconnects: broadcast::Receiver<Server>,
    driver_errors: broadcast::Receiver<(fred::error::Error, Option<Server>)>,
    commands: mpsc::Sender<SynchronizationCommand>,
    command_receiver: mpsc::Receiver<SynchronizationCommand>,
    pending: PendingChanges,
}

#[derive(Clone)]
struct GenerationSync {
    subscriber: SubscriberClient,
    commands: mpsc::Sender<SynchronizationCommand>,
}

struct SelectorSynchronization {
    state: SelectorState,
    clock: RedisClock,
    full: bool,
}

enum SelectorSynchronizationWork {
    Full,
    Repair { repair: BTreeSet<String>, clock: RedisClock },
}

enum SynchronizationCommand {
    Drain(oneshot::Sender<Result<Vec<PendingChange>>>),
}

struct SelectorShared {
    client: Arc<ClientInner>,
    type_name: String,
    owner: CancellationToken,
    closed: AtomicBool,
    finished: AtomicBool,
    done: Notify,
    errors: broadcast::Sender<Error>,
    final_error: Mutex<Option<Error>>,
    view: ArcSwap<SelectorView>,
}

/// 已完成权威同步并在本地物化的 Registry 视图核心。
pub(crate) struct SelectorCore {
    shared: Arc<SelectorShared>,
}

struct StartupGuard(Option<CancellationToken>);

impl Drop for StartupGuard {
    /// 构造未成功转移所有权时取消 Selector owner，避免初始化失败泄漏任务。
    fn drop(&mut self) {
        if let Some(token) = self.0.take() {
            token.cancel();
        }
    }
}

impl Client {
    /// 先订阅 Registry，再分页扫描，并只在首份 PING 栅栏视图同步后返回。
    ///
    /// `config.type_name` 选择 Client Zone 内的 Registry。成功对象拥有一个长期监听任务；
    /// 全量/修复同步任务仅在需要时临时存在。初始化失败会取消并等待已启动工作。
    pub(crate) async fn select(&self, config: SelectorConfig) -> Result<SelectorCore> {
        if !valid_type(&config.type_name) {
            return Err(Error::field(Code::Invalid, "type"));
        }
        let guard = self.inner.admit()?;
        let owner = self.inner.shutdown.child_token();
        let mut startup = StartupGuard(Some(owner.clone()));
        let (errors, _) = broadcast::channel(self.inner.config.selector_error_buffer_capacity);
        let shared = Arc::new(SelectorShared {
            client: Arc::clone(&self.inner),
            type_name: config.type_name,
            owner,
            closed: AtomicBool::new(false),
            finished: AtomicBool::new(false),
            done: Notify::new(),
            errors,
            final_error: Mutex::new(None),
            view: ArcSwap::from_pointee(empty_view(0, false)),
        });
        // worker 在同一长期任务内管理连接代际；ready 只传递首代权威同步结果。
        let (ready_sender, ready_receiver) = oneshot::channel();
        let worker = Arc::clone(&shared);
        tokio::spawn(async move {
            worker.run(guard, ready_sender).await;
        });

        match ready_receiver.await {
            Ok(Ok(())) => {
                startup.0 = None;
                Ok(SelectorCore { shared })
            }
            Ok(Err(error)) => {
                shared.owner.cancel();
                shared.wait_finished().await;
                Err(error)
            }
            Err(_) => {
                shared.owner.cancel();
                shared.wait_finished().await;
                Err(Error::new(Code::Closed))
            }
        }
    }
}

impl SelectorCore {
    /// 订阅有界异步同步与恢复诊断；落后接收者可能丢失旧消息。
    pub fn subscribe_errors(&self) -> broadcast::Receiver<Error> {
        self.shared.errors.subscribe()
    }

    /// 取消同步、关闭专用 Subscriber 并等待长期 worker 退出。
    ///
    /// 重复调用返回同一终止结果；此操作不关闭 Registration Client 或共享根传输。
    pub async fn close(&self) -> Result<()> {
        if !self.shared.closed.swap(true, Ordering::AcqRel) {
            self.shared.owner.cancel();
        }
        self.shared.wait_finished().await;
        self.shared.terminal_result()
    }
}

impl Drop for SelectorCore {
    /// 释放核心时发出非阻塞取消；显式 `close().await` 才提供确定任务汇合。
    fn drop(&mut self) {
        if !self.shared.closed.swap(true, Ordering::AcqRel) {
            self.shared.owner.cancel();
        }
    }
}

/// 强类型 Selector 配置。
#[derive(Clone, Debug)]
pub struct SelectorOptions {
    /// Client Zone 内要同步的 Registry Type。
    pub type_name: String,
}

/// 一次选择操作返回的独立强类型 Registration。
pub struct Candidate<A, D> {
    /// Redis 管理的 Registration 头部。
    pub meta: Meta,
    /// 生命周期内不可变的放置属性。
    pub attr: A,
    /// 已提交本地预测与远端更新对齐后的服务 Data。
    pub data: D,
}

/// 仅在策略回调期间可见的借用型强类型 Registration。
pub struct CandidateRef<'a, A, D> {
    /// Redis 管理的 Registration 头部借用。
    pub meta: &'a Meta,
    /// 不可变放置属性借用。
    pub attr: &'a A,
    /// 服务 Data 借用，包含本事务已经暂存的变化。
    pub data: &'a D,
    choice: Choice,
}

impl<A, D> CandidateRef<'_, A, D> {
    /// 返回仅供当前 One/Any 回调及 `Candidates::mutate` 使用的不透明身份。
    pub const fn choice(&self) -> Choice {
        self.choice
    }
}

/// 仅在当前回调事务中有效的不透明 Candidate 身份。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Choice {
    token: u64,
    index: usize,
}

/// 一个独立、不可参与选择的恢复候选。
pub struct RetainedCandidate<A, D> {
    /// 仅供恢复观察的独立 Registration。
    pub candidate: Candidate<A, D>,
    /// 本地 RedisClock 语义下的 Unix 毫秒保留截止时间。
    pub retained_until: u64,
}

/// 一份独立且顺序确定的强类型 Registry 视图。
pub struct SelectionSnapshot<A, D> {
    /// 此视图对应的 Redis 连接代际。
    pub generation: u64,
    /// 此视图是否已经越过最近恢复栅栏。
    pub synchronized: bool,
    /// 按 UUID 确定顺序排列的独立活跃 Registrations。
    pub candidates: Vec<Candidate<A, D>>,
    /// 仅供恢复观察的独立 Registrations。
    pub retained: Vec<RetainedCandidate<A, D>>,
}

struct CachedCandidate<A, D> {
    revision: u64,
    attr: A,
    data: D,
}

struct LocalOverlay<D> {
    revision: u64,
    data: D,
    base: Arc<Fields>,
    fields: Fields,
}

struct SelectionState<A, D> {
    cache: HashMap<String, CachedCandidate<A, D>>,
    overlays: HashMap<String, LocalOverlay<D>>,
    view: Option<Arc<SelectorView>>,
    selected: Vec<u64>,
    token: u64,
}

impl<A, D> SelectionState<A, D> {
    /// 推进当前选择事务 token，并在 `u64` 回绕时清空复用选择标记。
    ///
    /// 返回值永不为零；旧 Choice 因 token 不匹配而失效。
    fn advance_token(&mut self) -> u64 {
        self.token = self.token.wrapping_add(1);
        if self.token == 0 {
            self.selected.fill(0);
            self.token = 1;
        }
        self.token
    }
}

struct StagedCandidate<D> {
    revision: u64,
    data: D,
    base: Arc<Fields>,
    fields: Fields,
}

/// 同步选择策略使用的借用候选集合，不能逃逸其回调生命周期。
pub struct Candidates<'a, A: FieldValue, D: FieldValue> {
    selector: &'a Selector<A, D>,
    state: &'a mut SelectionState<A, D>,
    view: Arc<SelectorView>,
    staged: HashMap<String, StagedCandidate<D>>,
    token: u64,
}

impl<A: FieldValue, D: FieldValue> Candidates<'_, A, D> {
    /// 返回当前事务中按 UUID 排序的活跃候选数量。
    pub fn len(&self) -> usize {
        self.view.ordered_records.len()
    }

    /// 报告当前事务是否没有活跃候选。
    pub fn is_empty(&self) -> bool {
        self.view.ordered_records.is_empty()
    }

    /// 按确定 UUID 顺序借用 `index` 位置的 Candidate。
    ///
    /// 越界返回 `None`；Data 优先显示本回调 staged 值，其次是已提交本地 overlay，最后是远端缓存。
    pub fn get(&self, index: usize) -> Option<CandidateRef<'_, A, D>> {
        let record = self.view.ordered_records.get(index)?;
        let cached = self.state.cache.get(&record.meta.uuid)?;
        let data = self
            .staged
            .get(&record.meta.uuid)
            .map(|staged| &staged.data)
            .or_else(|| self.state.overlays.get(&record.meta.uuid).map(|overlay| &overlay.data))
            .unwrap_or(&cached.data);
        Some(CandidateRef {
            meta: &record.meta,
            attr: &cached.attr,
            data,
            choice: Choice { token: self.token, index },
        })
    }

    /// 在当前事务中暂存一次本地 Data `mutate`。
    ///
    /// `choice` 必须来自当前回调；闭包收到独立可变 D，完成后重新编码并验证固定字段结构和 Zone 容量。
    /// 后续 get 可见暂存值，但只有外层 One/Any 成功选择后才整体提交 overlay。
    pub fn mutate(&mut self, choice: Choice, mutate: impl FnOnce(&mut D) -> Result<()>) -> Result<()> {
        let record = self.validate_choice(choice)?;
        let cached = self
            .state
            .cache
            .get(&record.meta.uuid)
            .ok_or_else(|| Error::field(Code::Corrupt, "candidate"))?;
        let current = self
            .staged
            .get(&record.meta.uuid)
            .map(|staged| &staged.data)
            .or_else(|| self.state.overlays.get(&record.meta.uuid).map(|overlay| &overlay.data))
            .unwrap_or(&cached.data);
        // 先克隆并在独立值上运行应用逻辑，闭包错误不会改变既有 overlay 或缓存。
        let mut next = clone_value(current, "data")?;
        mutate(&mut next)?;
        let encoded = encode_value(&next, "data")?;
        if !same_field_structure(record.data.as_ref(), &encoded) {
            return Err(Error::field(Code::Contract, "data").with_revision(record.meta.revision));
        }
        let limits = self.selector.raw.shared.client.limits.load_full();
        validate_fields(&Fields::new(), &encoded, &limits)?;
        self.staged.insert(
            record.meta.uuid.clone(),
            StagedCandidate {
                revision: record.meta.revision,
                data: next,
                // 远端记录字段由 Arc 保证不可变生命周期；overlay 只读比较，无需复制整份基准。
                base: Arc::clone(&record.data),
                fields: encoded,
            },
        );
        Ok(())
    }

    /// 验证 `choice` 的事务 token 和索引，并返回对应内部记录。
    fn validate_choice(&self, choice: Choice) -> Result<&Arc<SelectorRecord>> {
        if choice.token != self.token {
            return Err(Error::field(Code::Contract, "candidate"));
        }
        self.view
            .ordered_records
            .get(choice.index)
            .ok_or_else(|| Error::field(Code::Contract, "candidate"))
    }

    /// 把 `choice` 当前可见的 Meta/Attr/Data 深克隆为调用方独占 Candidate。
    fn detach(&self, choice: Choice) -> Result<Candidate<A, D>> {
        let record = self.validate_choice(choice)?;
        let cached = self
            .state
            .cache
            .get(&record.meta.uuid)
            .ok_or_else(|| Error::field(Code::Corrupt, "candidate"))?;
        let data = self
            .staged
            .get(&record.meta.uuid)
            .map(|staged| &staged.data)
            .or_else(|| self.state.overlays.get(&record.meta.uuid).map(|overlay| &overlay.data))
            .unwrap_or(&cached.data);
        Ok(Candidate {
            meta: record.meta.clone(),
            attr: clone_value(&cached.attr, "attr")?,
            data: clone_value(data, "data")?,
        })
    }

    /// 扩展可复用选择标记数组以覆盖当前视图，不缩小已有容量。
    fn prepare_selected(&mut self) {
        if self.state.selected.len() < self.view.ordered_records.len() {
            self.state.selected.resize(self.view.ordered_records.len(), 0);
        }
    }

    /// 标记 `choice` 已在当前 Any 结果中使用，并拒绝过期、越界或重复选择。
    fn mark_selected(&mut self, choice: Choice) -> Result<()> {
        if choice.token != self.token || choice.index >= self.view.ordered_records.len() {
            return Err(Error::field(Code::Contract, "candidate"));
        }
        if self.state.selected[choice.index] == self.token {
            return Err(Error::field(Code::Contract, "candidate"));
        }
        self.state.selected[choice.index] = self.token;
        Ok(())
    }

    /// 原子提交本回调全部 staged Data 为持久本地 overlay。
    ///
    /// 所有可能失败的克隆、应用闭包、编码和选择验证均已在此前完成，因此此阶段只移动拥有型值。
    fn commit(&mut self) -> Result<()> {
        for (uuid, staged) in self.staged.drain() {
            self.state.overlays.insert(
                uuid,
                LocalOverlay {
                    revision: staged.revision,
                    data: staged.data,
                    base: staged.base,
                    fields: staged.fields,
                },
            );
        }
        Ok(())
    }
}

/// 带进程内事务型负载预测的强类型本地 Registry 视图。
pub struct Selector<A: FieldValue, D: FieldValue> {
    raw: SelectorCore,
    selection: AsyncMutex<SelectionState<A, D>>,
    closed: AtomicBool,
    marker: PhantomData<fn() -> (A, D)>,
}

impl<A: FieldValue, D: FieldValue> Selector<A, D> {
    /// 先订阅、再加载，并在首份栅栏视图完成后构造强类型 Selector。
    ///
    /// `client` 提供 Zone 与共享传输，`options.type_name` 选择 Registry；首次强类型解码按需发生。
    pub async fn new(client: &Client, options: SelectorOptions) -> Result<Self> {
        let raw = client.select(SelectorConfig { type_name: options.type_name }).await?;
        Ok(Self {
            raw,
            selection: AsyncMutex::new(SelectionState {
                cache: HashMap::new(),
                overlays: HashMap::new(),
                view: None,
                selected: Vec::new(),
                token: 0,
            }),
            closed: AtomicBool::new(false),
            marker: PhantomData,
        })
    }

    /// 在本地视图上执行一次同步选择策略，并返回零或一个独立 Candidate。
    ///
    /// `timeout` 同时限制事务锁等待和回调完成后的总截止检查；`choose` 本身同步执行，
    /// Rust 无法安全强制终止它。回调返回的 Choice 必须属于当前 Candidates，失败不提交 staged overlay。
    pub async fn one<F>(&self, timeout: Duration, choose: F) -> Result<Option<Candidate<A, D>>>
    where
        F: FnOnce(&mut Candidates<'_, A, D>) -> Result<Option<Choice>>,
    {
        if timeout.is_zero() {
            return Err(Error::field(Code::Invalid, "timeout"));
        }
        let deadline = Instant::now().checked_add(timeout).ok_or_else(|| Error::field(Code::Invalid, "timeout"))?;
        // 锁只覆盖强类型缓存、overlay 和同步回调；不执行 Redis I/O。
        let mut state = tokio::time::timeout(timeout, self.selection.lock())
            .await
            .map_err(|error| Error::driver(Code::Deadline, error))?;
        let mut candidates = self.begin(&mut state)?;
        let Some(choice) = choose(&mut candidates)? else {
            return Ok(None);
        };
        if Instant::now() >= deadline {
            return Err(Error::new(Code::Deadline));
        }
        let selected = candidates.detach(choice)?;
        candidates.commit()?;
        Ok(Some(selected))
    }

    /// 在本地视图上执行一次同步策略，并返回零或多个唯一 Candidate。
    ///
    /// `timeout` 与 One 语义一致；`choose` 返回的每个 Choice 必须来自本事务且不可重复。
    /// 全部选择验证和脱离成功后才提交本回调 staged overlay。
    pub async fn any<F>(&self, timeout: Duration, choose: F) -> Result<Vec<Candidate<A, D>>>
    where
        F: FnOnce(&mut Candidates<'_, A, D>) -> Result<Vec<Choice>>,
    {
        if timeout.is_zero() {
            return Err(Error::field(Code::Invalid, "timeout"));
        }
        let deadline = Instant::now().checked_add(timeout).ok_or_else(|| Error::field(Code::Invalid, "timeout"))?;
        let mut state = tokio::time::timeout(timeout, self.selection.lock())
            .await
            .map_err(|error| Error::driver(Code::Deadline, error))?;
        let mut candidates = self.begin(&mut state)?;
        let choices = choose(&mut candidates)?;
        if choices.is_empty() {
            return Ok(Vec::new());
        }
        if Instant::now() >= deadline {
            return Err(Error::new(Code::Deadline));
        }
        candidates.prepare_selected();
        let mut selected = Vec::with_capacity(choices.len());
        for choice in choices {
            candidates.mark_selected(choice)?;
            selected.push(candidates.detach(choice)?);
        }
        candidates.commit()?;
        Ok(selected)
    }

    /// 不执行 Redis I/O，返回一份完整、独立且按 UUID 排序的强类型视图。
    ///
    /// 半同步状态明确返回 `Unavailable`；活跃 Data 包含已对齐本地 overlay，retained 仅供恢复观察。
    pub async fn snapshot(&self) -> Result<SelectionSnapshot<A, D>> {
        let mut state = self.selection.lock().await;
        let view = self.raw.shared.view.load_full();
        if !view.synchronized {
            return Err(Error::field(Code::Unavailable, "selector"));
        }
        self.reconcile(&mut state, &view)?;
        let mut candidates = Vec::with_capacity(view.ordered_records.len());
        for record in &view.ordered_records {
            candidates.push(self.detach_record(&state, record)?);
        }
        let mut retained = Vec::with_capacity(view.ordered_retained.len());
        for value in &view.ordered_retained {
            retained.push(RetainedCandidate {
                candidate: Candidate {
                    meta: value.record.meta.clone(),
                    attr: A::decode_fields(value.record.attr.as_ref()).map_err(|error| error.with_field_if_empty("attr"))?,
                    data: D::decode_fields(value.record.data.as_ref()).map_err(|error| error.with_field_if_empty("data"))?,
                },
                retained_until: value.until,
            });
        }
        Ok(SelectionSnapshot {
            generation: view.generation,
            synchronized: view.synchronized,
            candidates,
            retained,
        })
    }

    /// 不执行 Redis I/O，按 `uuid` 返回一个独立活跃 Candidate。
    ///
    /// 视图半同步时返回 `Unavailable`，UUID 不存在时返回 `None`。
    pub async fn find(&self, uuid: &str) -> Result<Option<Candidate<A, D>>> {
        let mut state = self.selection.lock().await;
        let view = self.raw.shared.view.load_full();
        if !view.synchronized {
            return Err(Error::field(Code::Unavailable, "selector"));
        }
        self.reconcile(&mut state, &view)?;
        view.records.get(uuid).map(|record| self.detach_record(&state, record)).transpose()
    }

    /// 不执行 Redis I/O，按 `uuid` 返回一个独立 retained 恢复候选。
    ///
    /// retained 不参与 One/Any；视图半同步时返回 `Unavailable`，不存在时返回 `None`。
    pub fn find_retained(&self, uuid: &str) -> Result<Option<RetainedCandidate<A, D>>> {
        let view = self.raw.shared.view.load();
        if !view.synchronized {
            return Err(Error::field(Code::Unavailable, "selector"));
        }
        let Some(retained) = view.retained.get(uuid) else {
            return Ok(None);
        };
        Ok(Some(RetainedCandidate {
            candidate: Candidate {
                meta: retained.record.meta.clone(),
                attr: A::decode_fields(retained.record.attr.as_ref()).map_err(|error| error.with_field_if_empty("attr"))?,
                data: D::decode_fields(retained.record.data.as_ref()).map_err(|error| error.with_field_if_empty("data"))?,
            },
            retained_until: retained.until,
        }))
    }

    /// 订阅有界 Selector 同步诊断；落后接收者可能丢失旧消息。
    pub fn subscribe_errors(&self) -> broadcast::Receiver<Error> {
        self.raw.subscribe_errors()
    }

    /// 永久关闭强类型 Selector，取消同步并等待其拥有的任务退出。
    pub async fn close(&self) -> Result<()> {
        self.closed.store(true, Ordering::Release);
        self.raw.close().await
    }

    /// 在已锁定 `state` 上开始一个同步本地选择事务。
    ///
    /// 函数拒绝 Closed/半同步视图，先对齐强类型缓存与 overlay，再生成仅本事务有效 token。
    fn begin<'a>(&'a self, state: &'a mut SelectionState<A, D>) -> Result<Candidates<'a, A, D>> {
        if self.closed.load(Ordering::Acquire) {
            return Err(Error::new(Code::Closed));
        }
        let view = self.raw.shared.view.load_full();
        if !view.synchronized {
            return Err(Error::field(Code::Unavailable, "selector"));
        }
        self.reconcile(state, &view)?;
        let token = state.advance_token();
        Ok(Candidates {
            selector: self,
            state,
            view,
            staged: HashMap::new(),
            token,
        })
    }

    /// 把强类型缓存和本地 overlay 收敛到新的不可变 `view`。
    ///
    /// 相同 Arc 身份直接返回。远端 revision 变化时，仅远端实际改变的 Data 字段覆盖本地预测；
    /// 未被远端改变的 overlay 字段继续保留。新增记录按需解码，任何失败不更新 view 身份。
    fn reconcile(&self, state: &mut SelectionState<A, D>, view: &Arc<SelectorView>) -> Result<()> {
        // SelectorView 不可变，Arc 身份相同即可证明强类型缓存和全部 overlay 已与其对齐。
        if state.view.as_ref().is_some_and(|current| Arc::ptr_eq(current, view)) {
            return Ok(());
        }
        state
            .cache
            .retain(|uuid, cached| view.records.get(uuid).is_some_and(|record| record.meta.revision == cached.revision));
        state.overlays.retain(|uuid, _| view.records.contains_key(uuid));
        // 逐字段比较 overlay 的远端基准；远端修改优先，本地未冲突预测继续保留。
        for (uuid, overlay) in &mut state.overlays {
            let record = view.records.get(uuid).ok_or_else(|| Error::field(Code::Corrupt, "candidate"))?;
            if record.meta.revision == overlay.revision {
                continue;
            }
            for (name, value) in record.data.iter() {
                if overlay.base.get(name) != Some(value) {
                    overlay.fields.insert(name.clone(), value.clone());
                }
            }
            overlay.data = D::decode_fields(&overlay.fields).map_err(|error| error.with_field_if_empty("data"))?;
            overlay.base = Arc::clone(&record.data);
            overlay.revision = record.meta.revision;
        }
        // 只解码新 revision 记录；稳定记录复用应用类型对象，避免每次选择重复解码。
        for record in view.records.values() {
            if state.cache.contains_key(&record.meta.uuid) {
                continue;
            }
            let attr = A::decode_fields(record.attr.as_ref()).map_err(|error| error.with_field_if_empty("attr"))?;
            let data = D::decode_fields(record.data.as_ref()).map_err(|error| error.with_field_if_empty("data"))?;
            state.cache.insert(
                record.meta.uuid.clone(),
                CachedCandidate {
                    revision: record.meta.revision,
                    attr,
                    data,
                },
            );
        }
        state.view = Some(Arc::clone(view));
        Ok(())
    }

    /// 从已对齐 `state` 克隆 `record` 的独立 Candidate，Data 优先使用本地 overlay。
    fn detach_record(&self, state: &SelectionState<A, D>, record: &SelectorRecord) -> Result<Candidate<A, D>> {
        let cached = state.cache.get(&record.meta.uuid).ok_or_else(|| Error::field(Code::Corrupt, "candidate"))?;
        let data = state.overlays.get(&record.meta.uuid).map_or(&cached.data, |overlay| &overlay.data);
        Ok(Candidate {
            meta: record.meta.clone(),
            attr: clone_value(&cached.attr, "attr")?,
            data: clone_value(data, "data")?,
        })
    }
}

impl SelectorShared {
    /// 运行 Selector 唯一长期连接/监听 worker，并串行管理连接代际。
    ///
    /// `_guard` 维持 Registration Client 准入，`ready` 返回首代同步结果。每代最多额外启动
    /// 一个临时全量或定向修复任务；断线后保留有界 recovery state 并指数退避重建。
    async fn run(self: Arc<Self>, _guard: ActiveGuard, ready: oneshot::Sender<Result<()>>) {
        let mut ready = Some(ready);
        let mut generation_number = 0_u64;
        let mut failures = 0_u32;
        let mut state = SelectorState::empty();
        let mut last_clock: Option<RedisClock> = None;
        // 每次 Generation 都拥有独立 Subscriber；任何重连信号会废弃整代并从 subscribe-before-scan 重建。
        while !self.owner.is_cancelled() {
            let opened = tokio::time::timeout(self.client.config.selector_sync_timeout, Generation::open(&self.client, &self.type_name)).await;
            let mut generation = match opened {
                Ok(Ok(generation)) => generation,
                Ok(Err(error)) => {
                    if let Some(clock) = last_clock {
                        state.expire(clock.upper_now(), self.client.config.effective_selector_retained_bytes());
                    }
                    self.publish_state(&state, generation_number, false);
                    self.report(error.clone());
                    if let Some(sender) = ready.take() {
                        let _ = sender.send(Err(error));
                        break;
                    }
                    if !wait_cancelled(
                        &self.owner,
                        retry_delay(
                            failures,
                            self.client.config.selector_recovery_initial_delay,
                            self.client.config.selector_recovery_max_delay,
                            self.client.config.selector_recovery_multiplier,
                            self.client.config.selector_recovery_jitter_percent,
                        ),
                    )
                    .await
                    {
                        break;
                    }
                    failures = failures.saturating_add(1);
                    continue;
                }
                Err(error) => {
                    let error = Error::driver(Code::Deadline, error);
                    if let Some(clock) = last_clock {
                        state.expire(clock.upper_now(), self.client.config.effective_selector_retained_bytes());
                    }
                    self.publish_state(&state, generation_number, false);
                    self.report(error.clone());
                    if let Some(sender) = ready.take() {
                        let _ = sender.send(Err(error));
                        break;
                    }
                    if !wait_cancelled(
                        &self.owner,
                        retry_delay(
                            failures,
                            self.client.config.selector_recovery_initial_delay,
                            self.client.config.selector_recovery_max_delay,
                            self.client.config.selector_recovery_multiplier,
                            self.client.config.selector_recovery_jitter_percent,
                        ),
                    )
                    .await
                    {
                        break;
                    }
                    failures = failures.saturating_add(1);
                    continue;
                }
            };

            // 只有全量同步和 PING 栅栏成功后才推进公开 generation 编号并清零退避失败计数。
            let previous_generation = generation_number;
            let live = self
                .listen_generation(&mut generation, &mut state, &mut last_clock, &mut generation_number, &mut ready)
                .await;
            if generation_number != previous_generation {
                failures = 0;
            }
            generation.close(self.client.config.timeout).await;
            if self.owner.is_cancelled() {
                break;
            }
            if let Some(clock) = last_clock {
                state.expire(clock.upper_now(), self.client.config.effective_selector_retained_bytes());
            }
            self.publish_state(&state, generation_number, false);
            if let Err(error) = live {
                self.report(error.clone());
                if let Some(sender) = ready.take() {
                    let _ = sender.send(Err(error));
                    break;
                }
            }
            if !wait_cancelled(
                &self.owner,
                retry_delay(
                    failures,
                    self.client.config.selector_recovery_initial_delay,
                    self.client.config.selector_recovery_max_delay,
                    self.client.config.selector_recovery_multiplier,
                    self.client.config.selector_recovery_jitter_percent,
                ),
            )
            .await
            {
                break;
            }
            failures = failures.saturating_add(1);
        }
        // 终止时发布空且不可用视图，确保调用方不会继续选择已失去租约证明的记录。
        if let Some(sender) = ready.take() {
            let _ = sender.send(Err(Error::new(Code::Closed)));
        }
        self.publish(SelectorState::empty(), generation_number, false);
        self.finished.store(true, Ordering::Release);
        self.done.notify_waiters();
    }

    /// 在一个已订阅 Generation 内处理消息、同步任务、时钟校准、TTL 和视图发布。
    ///
    /// `generation` 由当前长期 worker 独占；`state`/`last_clock` 跨代保留有界恢复内容；
    /// `generation_number` 仅在完整同步成功后推进；`ready` 只完成一次首代构造。
    async fn listen_generation(
        self: &Arc<Self>,
        generation: &mut Generation,
        state: &mut SelectorState,
        last_clock: &mut Option<RedisClock>,
        generation_number: &mut u64,
        ready: &mut Option<oneshot::Sender<Result<()>>>,
    ) -> Result<()> {
        // 两个可重置 Sleep 避免为每次事件新建 Timer；未安排时停在一年后的有界远点。
        let far = Duration::from_secs(365 * 24 * 60 * 60);
        let publish_sleep = tokio::time::sleep(far);
        let expiry_sleep = tokio::time::sleep(far);
        tokio::pin!(publish_sleep);
        tokio::pin!(expiry_sleep);
        let mut clock_tick = tokio::time::interval(self.client.config.clock_refresh);
        clock_tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        clock_tick.tick().await;
        let mut publish_pending = false;
        let mut expiry_pending = false;
        let mut synchronized = false;
        let mut advance_generation = false;
        let mut synchronization = Some(self.spawn_synchronization(generation, state, SelectorSynchronizationWork::Full));

        // 监听任务是 Generation 中唯一读取 Pub/Sub receiver 的所有者；同步任务通过 Drain 命令取栅栏前事件。
        let outcome = async {
            loop {
                let synchronization_running = synchronization.is_some();
                tokio::select! {
                    () = self.owner.cancelled() => {
                        if let Some(task) = synchronization.take() {
                            task.abort();
                            let _ = task.await;
                        }
                        return Ok(());
                    }
                    command = generation.command_receiver.recv(), if synchronization_running => {
                        let Some(SynchronizationCommand::Drain(sender)) = command else {
                            return Err(Error::new(Code::Unavailable));
                        };
                        let result = drain_ready_generation_inputs(generation)
                            .map(|()| generation.pending.drain());
                        let failure = result.as_ref().err().cloned();
                        let _ = sender.send(result);
                        if let Some(error) = failure {
                            if let Some(task) = synchronization.take() {
                                task.abort();
                                let _ = task.await;
                            }
                            return Err(error);
                        }
                    }
                    message = generation.messages.recv() => {
                        let message = message.map_err(message_receive_error)?;
                        let event = decode_subscription_message(&generation.channel, message, &ZoneConfig::protocol_ceiling())?;
                        generation.pending.add(event)?;
                        if !synchronized || synchronization_running {
                            continue;
                        }
                        let changes = generation.pending.drain();
                        let clock = last_clock.ok_or_else(|| Error::field(Code::Corrupt, "redis_clock"))?;
                        let (changed, repair) = self.apply_pending(state, changes, &ZoneConfig::protocol_ceiling(), clock)?;
                        if !repair.is_empty() {
                            synchronized = false;
                            publish_pending = false;
                            self.mark_unavailable(*generation_number);
                            synchronization = Some(self.spawn_synchronization(
                                generation,
                                state,
                                SelectorSynchronizationWork::Repair { repair, clock },
                            ));
                            continue;
                        }
                        if changed {
                            self.mark_dirty(state, *generation_number, publish_sleep.as_mut(), &mut publish_pending);
                        }
                        schedule_expiry(state, clock, expiry_sleep.as_mut(), &mut expiry_pending);
                    }
                    reconnect = generation.reconnects.recv() => {
                        return match reconnect {
                            Ok(_) => Err(Error::field(Code::Unavailable, "subscription_generation")),
                            Err(error) => Err(message_receive_error(error)),
                        };
                    }
                    driver_error = generation.driver_errors.recv() => {
                        return match driver_error {
                            Ok((error, _)) => Err(Error::driver(Code::Unavailable, error)),
                            Err(error) => Err(message_receive_error(error)),
                        };
                    }
                    joined = wait_for_synchronization(&mut synchronization), if synchronization_running => {
                        let result = joined.map_err(|error| Error::driver(Code::Unavailable, error))?;
                        synchronization = None;
                        let result = result?;
                        // 临时任务返回完整候选状态；先合并其执行期间缓存的事件，再原子发布同步视图。
                        *state = result.state;
                        *last_clock = Some(result.clock);
                        advance_generation |= result.full;
                        let (_, repair) = self.apply_pending(
                            state,
                            generation.pending.drain(),
                            &ZoneConfig::protocol_ceiling(),
                            result.clock,
                        )?;
                        if !repair.is_empty() {
                            synchronization = Some(self.spawn_synchronization(
                                generation,
                                state,
                                SelectorSynchronizationWork::Repair {
                                    repair,
                                    clock: result.clock,
                                },
                            ));
                            continue;
                        }
                        state.expire(
                            result.clock.upper_now(),
                            self.client.config.effective_selector_retained_bytes(),
                        );
                        if advance_generation {
                            *generation_number = generation_number.saturating_add(1);
                            advance_generation = false;
                        }
                        synchronized = true;
                        publish_pending = false;
                        self.publish_state(state, *generation_number, true);
                        if let Some(sender) = ready.take() {
                            let _ = sender.send(Ok(()));
                        }
                        schedule_expiry(state, result.clock, expiry_sleep.as_mut(), &mut expiry_pending);
                    }
                    _ = clock_tick.tick(), if synchronized => {
                        let clock = self.client.calibrate_clock().await?;
                        *last_clock = Some(clock);
                        if state.expire(clock.upper_now(), self.client.config.effective_selector_retained_bytes()) != 0 {
                            self.mark_dirty(state, *generation_number, publish_sleep.as_mut(), &mut publish_pending);
                        }
                        schedule_expiry(state, clock, expiry_sleep.as_mut(), &mut expiry_pending);
                    }
                    () = &mut expiry_sleep, if synchronized && expiry_pending => {
                        expiry_pending = false;
                        let clock = last_clock.ok_or_else(|| Error::field(Code::Corrupt, "redis_clock"))?;
                        if state.expire(clock.upper_now(), self.client.config.effective_selector_retained_bytes()) != 0 {
                            self.mark_dirty(state, *generation_number, publish_sleep.as_mut(), &mut publish_pending);
                        }
                        schedule_expiry(state, clock, expiry_sleep.as_mut(), &mut expiry_pending);
                    }
                    () = &mut publish_sleep, if synchronized && publish_pending => {
                        publish_pending = false;
                        self.publish_state(state, *generation_number, true);
                    }
                }
            }
        }
        .await;
        // Generation 退出时中止并等待唯一临时同步任务，防止它在新代开始后继续写旧状态。
        if let Some(task) = synchronization.take() {
            task.abort();
            let _ = task.await;
        }
        outcome
    }

    /// 为当前 `generation` 启动一个临时全量或定向修复任务。
    ///
    /// `previous` 按值克隆但内部记录使用 Arc 共享；`work` 决定是否校准新 RedisClock 和推进代际。
    /// 每项工作整体受 `selector_sync_timeout` 限制。
    fn spawn_synchronization(
        self: &Arc<Self>,
        generation: &Generation,
        previous: &SelectorState,
        work: SelectorSynchronizationWork,
    ) -> JoinHandle<Result<SelectorSynchronization>> {
        let worker = Arc::clone(self);
        let generation = generation.synchronizer();
        let previous = previous.clone();
        tokio::spawn(async move {
            match work {
                SelectorSynchronizationWork::Full => {
                    tokio::time::timeout(worker.client.config.selector_sync_timeout, worker.synchronize_open(&generation, &previous))
                        .await
                        .map_err(|error| Error::driver(Code::Deadline, error))?
                        .map(|(state, clock)| SelectorSynchronization { state, clock, full: true })
                }
                SelectorSynchronizationWork::Repair { repair, clock } => tokio::time::timeout(
                    worker.client.config.selector_sync_timeout,
                    worker.repair_snapshot(&generation, previous, repair, clock),
                )
                .await
                .map_err(|error| Error::driver(Code::Deadline, error))?
                .map(|state| SelectorSynchronization { state, clock, full: false }),
            }
        })
    }

    /// 执行新连接代际的 subscribe-before-scan 全量同步。
    ///
    /// `generation` 提供同一订阅连接的 PING/Drain 栅栏，`previous` 只作为 retained 恢复来源。
    /// 函数分页 HSCAN Registry、按 revision 提示抓取记录，最多进行三轮定向 repair。
    async fn synchronize_open(&self, generation: &GenerationSync, previous: &SelectorState) -> Result<(SelectorState, RedisClock)> {
        let clock = self.client.calibrate_clock().await?;
        let limits = ZoneConfig::protocol_ceiling();
        let mut state = previous.recovery_state(clock.upper_now(), self.client.config.effective_selector_retained_bytes());
        let registry = registry_key(&self.client.config.zone, &self.type_name);
        let count = u32::try_from(self.client.config.selector_page_size).map_err(|_| Error::field(Code::Capacity, "selector_page_size"))?;
        // HSCAN 不提供原子快照；每页提示与订阅期间事件最终由同连接 PING/Drain 栅栏调和。
        let scanner = self.client.driver().hscan(registry, "*", Some(count));
        tokio::pin!(scanner);
        loop {
            let page = tokio::time::timeout(self.client.config.timeout, scanner.as_mut().next())
                .await
                .map_err(|error| Error::driver(Code::Deadline, error))?;
            let Some(page) = page else {
                break;
            };
            let mut page = page.map_err(|error| Error::driver(Code::Unavailable, error))?;
            let values = page.take_results().ok_or_else(|| Error::field(Code::Corrupt, "registry"))?;
            let mut hints = HashMap::with_capacity(values.len());
            for (key, value) in values.inner() {
                let uuid = key.into_string().ok_or_else(|| Error::field(Code::Corrupt, "registry"))?;
                let revision = redis_positive_u64(value).ok_or_else(|| Error::field(Code::Corrupt, "registry"))?;
                if !crate::fields::valid_uuid(&uuid) {
                    return Err(Error::field(Code::Corrupt, "registry"));
                }
                hints.insert(uuid, revision);
            }
            self.fetch_records(&mut state, &hints, &limits, clock).await?;
        }

        // 有界三轮定向修复吸收 scan/事件之间的 revision 间隙；持续变化则整代失败重建。
        let mut repair = self.fence(generation, &mut state, &limits, clock).await?;
        for _ in 0..3 {
            if repair.is_empty() {
                break;
            }
            repair = self.repair(generation, &mut state, repair, &limits, clock).await?;
        }
        if !repair.is_empty() {
            return Err(Error::field(Code::Transition, "selector_repair"));
        }
        state.expire(clock.upper_now(), self.client.config.effective_selector_retained_bytes());
        Ok((state, clock))
    }

    /// 在已有 `state` 上执行一次有界定向修复快照。
    ///
    /// `repair` 是需权威读取的 UUID 集合，`clock` 保持当前连接代际时间基准；最多三轮后仍有间隙返回 Transition。
    async fn repair_snapshot(
        &self,
        generation: &GenerationSync,
        mut state: SelectorState,
        mut repair: BTreeSet<String>,
        clock: RedisClock,
    ) -> Result<SelectorState> {
        let limits = ZoneConfig::protocol_ceiling();
        for _ in 0..3 {
            if repair.is_empty() {
                break;
            }
            repair = self.repair(generation, &mut state, repair, &limits, clock).await?;
        }
        if !repair.is_empty() {
            return Err(Error::field(Code::Transition, "selector_repair"));
        }
        state.expire(clock.upper_now(), self.client.config.effective_selector_retained_bytes());
        Ok(state)
    }

    /// 根据 Registry revision `hints` 抓取并安装一批 Registration。
    ///
    /// `state` 提供可复用记录，`limits` 约束完整解析，`clock` 判定 TTL。相同 revision 只 HMGET
    /// `@revision/@timestamp`；变化或回退再 HGETALL。全部 Redis 命令按 UUID 排序并 pipeline 执行。
    async fn fetch_records(&self, state: &mut SelectorState, hints: &HashMap<String, u64>, limits: &ZoneConfig, clock: RedisClock) -> Result<()> {
        if hints.is_empty() {
            return Ok(());
        }
        let mut uuids: Vec<_> = hints.keys().cloned().collect();
        uuids.sort_unstable();
        // Header 路径复用不可变 Attr/Data；只有检测到 revision 变化才进入第二批完整回读。
        enum FetchPlan {
            Header(Arc<SelectorRecord>),
            Full,
        }
        let mut plans = Vec::with_capacity(uuids.len());
        let pipeline = self.client.driver().pipeline();
        for uuid in &uuids {
            let key = registration_key(&self.client.config.zone, &self.type_name, uuid);
            match state.record(uuid) {
                Some((cached, _)) if cached.meta.revision == hints[uuid] => {
                    let _: Value = pipeline
                        .hmget(key, ("@revision", "@timestamp"))
                        .await
                        .map_err(|error| Error::driver(Code::Unavailable, error))?;
                    plans.push(FetchPlan::Header(cached));
                }
                _ => {
                    let _: Value = pipeline.hgetall(key).await.map_err(|error| Error::driver(Code::Unavailable, error))?;
                    plans.push(FetchPlan::Full);
                }
            }
        }
        let values: Vec<Value> = self.client.command(pipeline.all(), Code::Unavailable).await?;
        if values.len() != uuids.len() {
            return Err(Error::field(Code::Corrupt, "registration_pipeline"));
        }
        let mut fallback = Vec::new();
        for ((uuid, value), plan) in uuids.iter().zip(values).zip(plans) {
            match plan {
                FetchPlan::Full => {
                    self.install_fetched_record(state, uuid, hints[uuid], value, limits, clock)?;
                }
                FetchPlan::Header(cached) => {
                    let Value::Array(header) = value else {
                        return Err(Error::field(Code::Corrupt, "registration_header"));
                    };
                    if header.len() != 2 {
                        return Err(Error::field(Code::Corrupt, "registration_header"));
                    }
                    if header.iter().any(Value::is_null) {
                        state.retain_uuid(uuid, clock.upper_now(), self.client.config.effective_selector_retained_bytes());
                        continue;
                    }
                    let revision = redis_positive_u64(header[0].clone()).ok_or_else(|| Error::field(Code::Corrupt, "registration_header"))?;
                    let timestamp = redis_positive_u64(header[1].clone()).ok_or_else(|| Error::field(Code::Corrupt, "registration_header"))?;
                    if revision < hints[uuid] {
                        return Err(Error::field(Code::Transition, "@revision").with_revision(revision));
                    }
                    if revision != cached.meta.revision {
                        fallback.push(uuid.clone());
                        continue;
                    }
                    let deadline = timestamp
                        .checked_add(cached.meta.ttl)
                        .filter(|value| *value <= MAX_HASH_FIELD_EXPIRE_AT_MS)
                        .ok_or_else(|| Error::field(Code::Corrupt, "@timestamp"))?;
                    let mut meta = cached.meta.clone();
                    meta.timestamp = timestamp;
                    let next = Arc::new(SelectorRecord {
                        meta,
                        attr: Arc::clone(&cached.attr),
                        data: cached.data.clone(),
                        deadline,
                        size: cached.size,
                    });
                    if deadline <= clock.upper_now() {
                        state.retain_record(next, clock.upper_now(), self.client.config.effective_selector_retained_bytes());
                    } else {
                        self.set_record(state, next)?;
                    }
                }
            }
        }
        // 第一批 header 显示 revision 已变化时，集中发出第二条 HGETALL pipeline，避免逐 UUID 往返。
        if !fallback.is_empty() {
            let pipeline = self.client.driver().pipeline();
            for uuid in &fallback {
                let _: Value = pipeline
                    .hgetall(registration_key(&self.client.config.zone, &self.type_name, uuid))
                    .await
                    .map_err(|error| Error::driver(Code::Unavailable, error))?;
            }
            let values: Vec<Value> = self.client.command(pipeline.all(), Code::Unavailable).await?;
            if values.len() != fallback.len() {
                return Err(Error::field(Code::Corrupt, "registration_pipeline"));
            }
            for (uuid, value) in fallback.into_iter().zip(values) {
                self.install_fetched_record(state, &uuid, hints[&uuid], value, limits, clock)?;
            }
        }
        state.expire(clock.upper_now(), self.client.config.effective_selector_retained_bytes());
        Ok(())
    }

    /// 解析一个权威 Hash `value` 并按 Registry `hint` 安装到 `state`。
    ///
    /// 缺失 Hash 将旧记录转 retained；后端 revision 小于 hint 返回 Transition；过期记录不进入活跃视图。
    fn install_fetched_record(&self, state: &mut SelectorState, uuid: &str, hint: u64, value: Value, limits: &ZoneConfig, clock: RedisClock) -> Result<()> {
        let Some(record) = parse_stored_record(uuid, value, limits)? else {
            state.retain_uuid(uuid, clock.upper_now(), self.client.config.effective_selector_retained_bytes());
            return Ok(());
        };
        if record.meta.revision < hint {
            return Err(Error::field(Code::Transition, "@revision").with_revision(record.meta.revision));
        }
        let record = internal_record(record);
        if record.deadline <= clock.upper_now() {
            state.retain_record(record, clock.upper_now(), self.client.config.effective_selector_retained_bytes());
        } else {
            self.set_record(state, record)?;
        }
        Ok(())
    }

    /// 在同一订阅连接发送随机 PING，并请求监听任务排空此前已就绪消息。
    ///
    /// `state` 接收 drained 变化，`limits`/`clock` 验证内容与 TTL；返回仍需权威 repair 的 UUID。
    async fn fence(&self, generation: &GenerationSync, state: &mut SelectorState, limits: &ZoneConfig, clock: RedisClock) -> Result<BTreeSet<String>> {
        let nonce = nonce()?;
        let pong: Value = self.client.command(generation.subscriber.ping(Some(nonce.clone())), Code::Unavailable).await?;
        if !valid_pong(pong, &nonce) {
            return Err(Error::field(Code::Corrupt, "pong"));
        }
        let changes = generation.drain(self.client.config.timeout).await?;
        let (_, repair) = self.apply_pending(state, changes, limits, clock)?;
        Ok(repair)
    }

    /// 对 `repair` UUID 集合执行权威读取，再重复同连接栅栏。
    ///
    /// 后端暂时落后提示会留给栅栏后的下一轮，而确定错误直接返回。
    async fn repair(
        &self,
        generation: &GenerationSync,
        state: &mut SelectorState,
        repair: BTreeSet<String>,
        limits: &ZoneConfig,
        clock: RedisClock,
    ) -> Result<BTreeSet<String>> {
        let hints = repair.into_iter().map(|uuid| (uuid, 1)).collect();
        match self.fetch_records(state, &hints, limits, clock).await {
            Err(error) if error.code() == Code::Transition => {}
            result => result?,
        }
        self.fence(generation, state, limits, clock).await
    }

    /// 按 UUID 顺序把合并后的 `changes` 应用到 `state`。
    ///
    /// `limits` 与 `clock` 分别约束字段/容量和 TTL；返回是否改变视图及去重后的 repair UUID 集合。
    fn apply_pending(
        &self,
        state: &mut SelectorState,
        changes: Vec<PendingChange>,
        limits: &ZoneConfig,
        clock: RedisClock,
    ) -> Result<(bool, BTreeSet<String>)> {
        let mut changed = false;
        let mut repair = BTreeSet::new();
        for change in changes {
            if change.repair {
                repair.insert(change.event.uuid);
                continue;
            }
            let uuid = change.event.uuid.clone();
            let (applied, needs_repair) = self.apply_pending_change(state, change, limits, clock)?;
            changed |= applied;
            if needs_repair {
                repair.insert(uuid);
            }
        }
        Ok((changed, repair))
    }

    /// 应用一个已经按 UUID 合并的 `change`。
    ///
    /// 非 Update 直接走生命周期事件；Update 要求本地 revision 至少覆盖合并基准，否则请求 repair。
    fn apply_pending_change(&self, state: &mut SelectorState, change: PendingChange, limits: &ZoneConfig, clock: RedisClock) -> Result<(bool, bool)> {
        if change.event.kind != "update" {
            return self.apply_event(state, change.event, limits, clock);
        }
        let Some((current, _)) = state.record(&change.event.uuid) else {
            return Ok((false, true));
        };
        if change.event.revision <= current.meta.revision {
            return Ok((false, false));
        }
        if current.meta.revision < change.base_revision {
            return Ok((false, true));
        }

        self.apply_update(state, change.event, current, limits, clock)
    }

    /// 在 `current` 完整记录上应用一条连续 Update `event`。
    ///
    /// `limits` 约束更新后的完整记录，`clock` 判定租约；固定 Data 中出现未知字段返回 repair。
    /// 函数用缓存 record size 做精确 revision/version/value 长度增量，避免每次遍历完整 Attr/Data。
    fn apply_update(
        &self,
        state: &mut SelectorState,
        event: RegistrationEvent,
        current: Arc<SelectorRecord>,
        limits: &ZoneConfig,
        clock: RedisClock,
    ) -> Result<(bool, bool)> {
        // 解码已校验 patch 名称和值；固定结构与缓存字节增量在局部更新时间保持完整记录契约。
        let mut size = selector_record_size(&current);
        size = replace_size(size, selector_decimal_digits(current.meta.revision), selector_decimal_digits(event.revision))?;
        size = replace_size(
            size,
            selector_decimal_digits(current.meta.version),
            selector_decimal_digits(if event.has_version { event.version } else { current.meta.version }),
        )?;
        let mut data = current.data.as_ref().clone();
        for (name, value) in event.data {
            let Some(field) = data.get_mut(&name) else {
                return Ok((false, true));
            };
            size = replace_size(size, field.len(), value.len())?;
            *field = value;
        }
        let version = if event.has_version { event.version } else { current.meta.version };
        let deadline = event
            .timestamp
            .checked_add(current.meta.ttl)
            .filter(|value| *value <= MAX_HASH_FIELD_EXPIRE_AT_MS)
            .ok_or_else(|| Error::field(Code::Corrupt, "@timestamp"))?;
        if size > limits.record_max_bytes {
            return Err(Error::field(Code::Capacity, "registration"));
        }
        let next = Arc::new(SelectorRecord {
            meta: Meta {
                uuid: event.uuid,
                revision: event.revision,
                timestamp: event.timestamp,
                ttl: current.meta.ttl,
                version,
            },
            attr: Arc::clone(&current.attr),
            data: Arc::new(data),
            deadline,
            size,
        });
        if deadline <= clock.upper_now() {
            return Ok((
                state.retain_record(next, clock.upper_now(), self.client.config.effective_selector_retained_bytes()),
                false,
            ));
        }
        self.set_record(state, next)?;
        Ok((true, false))
    }

    /// 把一条完整生命周期 `event` 应用到活跃/retained `state`。
    ///
    /// Register 可重建完整状态，Update 要求 revision 连续，Renew 只提升 timestamp，Unregister 立即 purge。
    /// 返回视图是否变化和是否需要定向权威 repair；旧事件幂等忽略。
    fn apply_event(&self, state: &mut SelectorState, event: RegistrationEvent, limits: &ZoneConfig, clock: RedisClock) -> Result<(bool, bool)> {
        let (current, active) = state.record(&event.uuid).map_or((None, false), |(record, active)| (Some(record), active));
        // 当前记录可能来自活跃或 retained；同 UUID 新鲜事件可将 retained 恢复为活跃。
        match event.kind.as_str() {
            "unregister" => Ok((self.remove_record(state, &event.uuid), false)),
            "register" => {
                if current.as_ref().is_some_and(|value| event.revision < value.meta.revision) {
                    return Ok((false, false));
                }
                let deadline = event
                    .timestamp
                    .checked_add(event.ttl)
                    .filter(|value| *value <= MAX_HASH_FIELD_EXPIRE_AT_MS)
                    .ok_or_else(|| Error::field(Code::Corrupt, "@timestamp"))?;
                let mut next = SelectorRecord {
                    meta: Meta {
                        uuid: event.uuid,
                        revision: event.revision,
                        timestamp: event.timestamp,
                        ttl: event.ttl,
                        version: event.version,
                    },
                    attr: Arc::new(event.attr),
                    data: Arc::new(event.data),
                    deadline,
                    size: 0,
                };
                next.size = registration_size(&next.meta.uuid, next.meta.revision, next.meta.ttl, next.meta.version, &next.attr, &next.data);
                if let Some(current) = &current {
                    if next.meta.revision == current.meta.revision {
                        if next.meta.version != current.meta.version
                            || next.meta.ttl != current.meta.ttl
                            || next.attr.as_ref() != current.attr.as_ref()
                            || next.data.as_ref() != current.data.as_ref()
                        {
                            return Ok((false, true));
                        }
                        if current.meta.timestamp > next.meta.timestamp {
                            next.meta.timestamp = current.meta.timestamp;
                            next.deadline = current.deadline;
                        }
                    }
                }
                if next.deadline <= clock.upper_now() {
                    return Ok((
                        state.retain_record(Arc::new(next), clock.upper_now(), self.client.config.effective_selector_retained_bytes()),
                        false,
                    ));
                }
                let changed = !active
                    || current.as_ref().is_none_or(|current| {
                        current.meta.revision != next.meta.revision
                            || current.meta.version != next.meta.version
                            || current.attr.as_ref() != next.attr.as_ref()
                            || current.data.as_ref() != next.data.as_ref()
                    });
                self.set_record(state, Arc::new(next))?;
                Ok((changed, false))
            }
            "update" => {
                let Some(current) = current else {
                    return Ok((false, true));
                };
                if event.revision <= current.meta.revision {
                    return Ok((false, false));
                }
                if event.revision != current.meta.revision + 1 {
                    return Ok((false, true));
                }
                self.apply_update(state, event, current, limits, clock)
            }
            "renew" => {
                let Some(current) = current else {
                    return Ok((false, true));
                };
                if event.revision < current.meta.revision {
                    return Ok((false, false));
                }
                if event.revision > current.meta.revision {
                    return Ok((false, true));
                }
                if event.timestamp <= current.meta.timestamp {
                    return Ok((false, false));
                }
                let deadline = event
                    .timestamp
                    .checked_add(current.meta.ttl)
                    .filter(|value| *value <= MAX_HASH_FIELD_EXPIRE_AT_MS)
                    .ok_or_else(|| Error::field(Code::Corrupt, "@timestamp"))?;
                let mut meta = current.meta.clone();
                meta.timestamp = event.timestamp;
                let next = Arc::new(SelectorRecord {
                    meta,
                    attr: Arc::clone(&current.attr),
                    data: Arc::clone(&current.data),
                    deadline,
                    size: current.size,
                });
                if deadline <= clock.upper_now() {
                    return Ok((
                        state.retain_record(next, clock.upper_now(), self.client.config.effective_selector_retained_bytes()),
                        false,
                    ));
                }
                self.set_record(state, next)?;
                Ok((!active, false))
            }
            _ => Err(Error::field(Code::Invalid, "&kind")),
        }
    }

    /// 在活跃 `state` 中插入或替换完整 `record`，并同步容量和 deadline 索引。
    ///
    /// 超过 `selector_max_bytes` 时事务性拒绝；同 UUID retained 状态会被移除。
    fn set_record(&self, state: &mut SelectorState, record: Arc<SelectorRecord>) -> Result<()> {
        let previous_size = state.records.get(&record.meta.uuid).map_or(0, |previous| selector_record_size(previous));
        let next_bytes = state
            .bytes
            .checked_sub(previous_size)
            .and_then(|bytes| bytes.checked_add(selector_record_size(&record)))
            .ok_or_else(|| Error::field(Code::Capacity, "selector_view"))?;
        if next_bytes > self.client.config.selector_max_bytes {
            return Err(Error::field(Code::Capacity, "selector_view"));
        }
        state.remove_retained(&record.meta.uuid);
        state.records.insert(record.meta.uuid.clone(), Arc::clone(&record));
        state.deadlines.set(&record.meta.uuid, record.deadline);
        state.bytes = next_bytes;
        Ok(())
    }

    /// 从活跃与 retained 状态完整删除 `uuid`，返回视图是否实际变化。
    fn remove_record(&self, state: &mut SelectorState, uuid: &str) -> bool {
        state.purge(uuid)
    }

    /// 标记 `state` 需要发布。
    ///
    /// `generation` 写入新视图；零 publish interval 立即发布，否则只在首个脏事件时重置复用 `sleep`。
    fn mark_dirty(&self, state: &SelectorState, generation: u64, mut sleep: std::pin::Pin<&mut tokio::time::Sleep>, pending: &mut bool) {
        if self.client.config.selector_publish_interval.is_zero() {
            self.publish_state(state, generation, true);
        } else if !*pending {
            sleep.as_mut().reset(tokio::time::Instant::now() + self.client.config.selector_publish_interval);
            *pending = true;
        }
    }

    /// 把 `state` 物化为不可变 SelectorView，并以 ArcSwap 原子发布指定同步状态。
    fn publish_state(&self, state: &SelectorState, generation: u64, synchronized: bool) {
        self.view.store(Arc::new(materialize_view(state, generation, synchronized)));
    }

    /// 发布指定 `generation` 的空半同步视图，使 One/Any 明确返回 Unavailable。
    fn mark_unavailable(&self, generation: u64) {
        self.view.store(Arc::new(empty_view(generation, false)));
    }

    /// 消费 `state` 并发布视图；所有内部记录仍通过 Arc 共享，不复制字段内容。
    fn publish(&self, state: SelectorState, generation: u64, synchronized: bool) {
        self.publish_state(&state, generation, synchronized);
    }

    /// 同时向 Selector 与领域 Client 尽力广播 `error`，不阻塞监听任务。
    fn report(&self, error: Error) {
        let _ = self.errors.send(error.clone());
        self.client.report(error);
    }

    /// 等待唯一长期 worker 发布 finished；Notify 在复查前启用以避免丢失唤醒。
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

/// 等待可选临时 `synchronization` 任务。
///
/// Some 时返回 JoinHandle 结果；None 时永久 pending，使带条件的 `tokio::select!` 分支类型稳定。
async fn wait_for_synchronization(
    synchronization: &mut Option<JoinHandle<Result<SelectorSynchronization>>>,
) -> std::result::Result<Result<SelectorSynchronization>, tokio::task::JoinError> {
    match synchronization.as_mut() {
        Some(task) => task.await,
        None => std::future::pending().await,
    }
}

impl Generation {
    /// 为指定 `type_name` 建立一代独立的 Pub/Sub 订阅连接。
    ///
    /// `client` 提供 Redis 驱动、缓冲区限制和 zone，`type_name` 决定订阅的 Registry channel。
    /// 返回值持有本代消息、重连、驱动错误和同步命令通道；订阅握手期间发生重连会作为不可用失败。
    async fn open(client: &Arc<ClientInner>, type_name: &str) -> Result<Self> {
        let subscriber = client.subscriber(client.config.selector_event_buffer)?;
        let messages = subscriber.message_rx();
        let mut reconnects = subscriber.reconnect_rx();
        let driver_errors = subscriber.error_rx();
        client.command(subscriber.init(), Code::Unavailable).await?;
        drain_initial_reconnect(&mut reconnects)?;
        let channel = registry_key(&client.config.zone, type_name);
        client.command(subscriber.subscribe(channel.clone()), Code::Unavailable).await?;
        if !matches!(reconnects.try_recv(), Err(broadcast::error::TryRecvError::Empty)) {
            let _ = subscriber.quit().await;
            return Err(Error::field(Code::Unavailable, "subscription_generation"));
        }
        let (commands, command_receiver) = mpsc::channel(1);
        Ok(Self {
            subscriber,
            channel,
            messages,
            reconnects,
            driver_errors,
            commands,
            command_receiver,
            pending: PendingChanges::new(client.config.selector_event_buffer, client.config.selector_event_bytes),
        })
    }

    /// 创建仅暴露同步栅栏能力的轻量句柄。
    ///
    /// 返回句柄共享本代 subscriber，并通过容量为一的命令通道请求长期监听任务排空事件。
    fn synchronizer(&self) -> GenerationSync {
        GenerationSync {
            subscriber: self.subscriber.clone(),
            commands: self.commands.clone(),
        }
    }

    /// 在 `timeout` 内尽力退出本代 Pub/Sub 连接。
    ///
    /// 关闭用于资源回收，超时或驱动错误不覆盖监听任务已经确定的主终止结果。
    async fn close(&mut self, timeout: Duration) {
        let _ = tokio::time::timeout(timeout, self.subscriber.quit()).await;
    }
}

impl GenerationSync {
    /// 请求长期监听任务排空当前代已经接收的事件。
    ///
    /// `timeout` 同时约束命令入队和应答等待；返回值是按 UUID 合并后的变化，通道关闭映射为不可用。
    async fn drain(&self, timeout: Duration) -> Result<Vec<PendingChange>> {
        let (sender, receiver) = oneshot::channel();
        tokio::time::timeout(timeout, self.commands.send(SynchronizationCommand::Drain(sender)))
            .await
            .map_err(|error| Error::driver(Code::Deadline, error))?
            .map_err(|error| Error::driver(Code::Unavailable, error))?;
        tokio::time::timeout(timeout, receiver)
            .await
            .map_err(|error| Error::driver(Code::Deadline, error))?
            .map_err(|error| Error::driver(Code::Unavailable, error))?
    }
}

/// 非阻塞排空 `generation` 当前已经就绪的全部输入。
///
/// 消息被解码并合并到 pending；任何 lag、重连、驱动错误或通道关闭都会使本代失效并返回错误。
fn drain_ready_generation_inputs(generation: &mut Generation) -> Result<()> {
    let limits = ZoneConfig::protocol_ceiling();
    loop {
        match generation.messages.try_recv() {
            Ok(message) => generation.pending.add(decode_subscription_message(&generation.channel, message, &limits)?)?,
            Err(broadcast::error::TryRecvError::Empty) => break,
            Err(broadcast::error::TryRecvError::Lagged(_)) => {
                return Err(Error::field(Code::Capacity, "selector_event_buffer"));
            }
            Err(broadcast::error::TryRecvError::Closed) => {
                return Err(Error::new(Code::Unavailable));
            }
        }
    }
    match generation.reconnects.try_recv() {
        Ok(_) | Err(broadcast::error::TryRecvError::Lagged(_)) => {
            return Err(Error::field(Code::Unavailable, "subscription_generation"));
        }
        Err(broadcast::error::TryRecvError::Closed) => {
            return Err(Error::new(Code::Unavailable));
        }
        Err(broadcast::error::TryRecvError::Empty) => {}
    }
    match generation.driver_errors.try_recv() {
        Ok((error, _)) => return Err(Error::driver(Code::Unavailable, error)),
        Err(broadcast::error::TryRecvError::Lagged(_)) => {
            return Err(Error::field(Code::Unavailable, "subscription_generation"));
        }
        Err(broadcast::error::TryRecvError::Closed) => {
            return Err(Error::new(Code::Unavailable));
        }
        Err(broadcast::error::TryRecvError::Empty) => {}
    }
    Ok(())
}

/// 解码一条 Redis Pub/Sub `message`。
///
/// `channel` 是本代唯一允许的 Registry channel，`limits` 限制事件字段；channel 不匹配或 payload 非字节值均视为损坏。
fn decode_subscription_message(channel: &str, message: Message, limits: &ZoneConfig) -> Result<RegistrationEvent> {
    if &*message.channel != channel {
        return Err(Error::field(Code::Corrupt, "subscription_channel"));
    }
    let payload = message.value.into_owned_bytes().ok_or_else(|| Error::field(Code::Corrupt, "event"))?;
    decode_registration_event(&payload, limits)
}

impl SelectorState {
    /// 构造没有活跃记录、retained 记录和 deadline 索引的空工作状态。
    fn empty() -> Self {
        Self {
            records: HashMap::new(),
            deadlines: DeadlineQueue::with_capacity(0),
            bytes: 0,
            retained: HashMap::new(),
            retained_deadlines: DeadlineQueue::with_capacity(0),
            retained_bytes: 0,
        }
    }

    /// 按 `uuid` 查找记录，并返回共享记录及其是否处于活跃视图。
    ///
    /// 活跃记录优先；只存在 retained 记录时第二个返回值为 false；完全不存在时返回 None。
    fn record(&self, uuid: &str) -> Option<(Arc<SelectorRecord>, bool)> {
        if let Some(record) = self.records.get(uuid) {
            return Some((Arc::clone(record), true));
        }
        self.retained.get(uuid).map(|retained| (Arc::clone(&retained.record), false))
    }

    /// 从 retained 视图和 retained deadline 索引中删除 `uuid`。
    ///
    /// 同步扣减缓存字节；返回值表示是否实际删除了记录。
    fn remove_retained(&mut self, uuid: &str) -> bool {
        let removed = self.retained.remove(uuid);
        if let Some(retained) = &removed {
            self.retained_bytes = self.retained_bytes.saturating_sub(selector_record_size(&retained.record));
        }
        self.retained_deadlines.remove(uuid);
        removed.is_some()
    }

    /// 从活跃、retained 两套视图及其 deadline 索引中彻底删除 `uuid`。
    ///
    /// 返回值表示任意一套视图是否发生变化。
    fn purge(&mut self, uuid: &str) -> bool {
        let removed = self.records.remove(uuid);
        if let Some(record) = &removed {
            self.bytes = self.bytes.saturating_sub(selector_record_size(record));
        }
        self.deadlines.remove(uuid);
        self.remove_retained(uuid) || removed.is_some()
    }

    /// 把当前活跃 `uuid` 移入 retained 视图。
    ///
    /// `now` 是 RedisClock 上界时间，`limit` 是 retained 总字节上限；不存在活跃记录时返回 false。
    fn retain_uuid(&mut self, uuid: &str, now: u64, limit: usize) -> bool {
        let record = self.records.get(uuid).cloned();
        record.is_some_and(|record| self.retain_record(record, now, limit))
    }

    /// 按记录原 TTL 计算二次保留截止时间，并把 `record` 移入 retained 视图。
    ///
    /// `now` 用于拒绝已经越过二次保留期的记录，`limit` 控制 retained 总容量。
    fn retain_record(&mut self, record: Arc<SelectorRecord>, now: u64, limit: usize) -> bool {
        let until = record.deadline.saturating_add(record.meta.ttl);
        self.set_retained(record, until, now, limit)
    }

    /// 以明确的 `until` 截止时间安装 retained `record`。
    ///
    /// 安装会先移除同 UUID 的活跃/旧 retained 状态，再按 `limit` 从最早截止记录开始淘汰；
    /// `limit` 为零或 `until <= now` 时只执行清除，不保存记录。返回值表示视图是否发生变化。
    fn set_retained(&mut self, record: Arc<SelectorRecord>, until: u64, now: u64, limit: usize) -> bool {
        let uuid = record.meta.uuid.clone();
        let active = self.records.remove(&uuid);
        if let Some(active) = &active {
            self.bytes = self.bytes.saturating_sub(selector_record_size(active));
            self.deadlines.remove(&uuid);
        }
        let was_retained = self.remove_retained(&uuid);
        if limit == 0 || until <= now {
            return active.is_some() || was_retained;
        }

        self.retained_bytes = self.retained_bytes.saturating_add(selector_record_size(&record));
        self.retained.insert(uuid.clone(), RetainedSelectorRecord { record, until });
        self.retained_deadlines.set(&uuid, until);
        // 容量超限时按 retained 截止时间淘汰，避免扫描整张 HashMap 或按插入顺序长期保留过期数据。
        while self.retained_bytes > limit {
            let Some(evicted) = self.retained_deadlines.pop() else {
                break;
            };
            if let Some(retained) = self.retained.remove(&evicted) {
                self.retained_bytes = self.retained_bytes.saturating_sub(selector_record_size(&retained.record));
            }
        }
        true
    }

    /// 处理所有不晚于 `now` 的活跃租约和 retained 截止时间。
    ///
    /// 活跃记录先按 `limit` 进入二次保留，retained 到期后彻底删除；返回发生变化的记录数量。
    fn expire(&mut self, now: u64, limit: usize) -> usize {
        let mut changed = 0;
        while let Some(uuid) = self.deadlines.expire(now) {
            if let Some(record) = self.records.get(&uuid).cloned() {
                if self.retain_record(record, now, limit) {
                    changed += 1;
                }
            }
        }
        while let Some(uuid) = self.retained_deadlines.expire(now) {
            if let Some(retained) = self.retained.remove(&uuid) {
                self.retained_bytes = self.retained_bytes.saturating_sub(selector_record_size(&retained.record));
                changed += 1;
            }
        }
        changed
    }

    /// 从当前状态构造一次失联恢复状态。
    ///
    /// `now` 与 `limit` 重新验证 retained 容量和截止时间；全部活跃记录转入 retained，结果中不保留活跃视图。
    fn recovery_state(&self, now: u64, limit: usize) -> Self {
        let mut state = Self {
            records: HashMap::with_capacity(self.records.len()),
            deadlines: DeadlineQueue::with_capacity(self.records.len()),
            bytes: 0,
            retained: HashMap::with_capacity(self.records.len() + self.retained.len()),
            retained_deadlines: DeadlineQueue::with_capacity(self.records.len() + self.retained.len()),
            retained_bytes: 0,
        };
        for retained in self.retained.values() {
            state.set_retained(Arc::clone(&retained.record), retained.until, now, limit);
        }
        for record in self.records.values() {
            state.retain_record(Arc::clone(record), now, limit);
        }
        state
    }

    /// 返回活跃租约与 retained 截止时间中最早的一个；两套索引均为空时返回 None。
    fn next_deadline(&self) -> Option<u64> {
        match (self.deadlines.next(), self.retained_deadlines.next()) {
            (Some(active), Some(retained)) => Some(active.min(retained)),
            (Some(active), None) => Some(active),
            (None, Some(retained)) => Some(retained),
            (None, None) => None,
        }
    }
}

/// 把权威 Redis `StoredRecord` 转为 Selector 内部共享记录。
///
/// Attr/Data 各自放入 Arc，缓存完整记录字节数，供快照共享和后续局部更新避免重复计算。
fn internal_record(value: StoredRecord) -> Arc<SelectorRecord> {
    let mut record = SelectorRecord {
        meta: value.meta,
        attr: Arc::new(value.attr),
        data: Arc::new(value.data),
        deadline: value.deadline,
        size: 0,
    };
    record.size = registration_size(
        &record.meta.uuid,
        record.meta.revision,
        record.meta.ttl,
        record.meta.version,
        &record.attr,
        &record.data,
    );
    Arc::new(record)
}

/// 返回 `record` 的协议字节数。
///
/// 正常路径直接读取缓存；零值只作为兼容/防御路径重新计算完整 Meta、Attr 和 Data 大小。
fn selector_record_size(record: &SelectorRecord) -> usize {
    if record.size != 0 {
        return record.size;
    }
    registration_size(
        &record.meta.uuid,
        record.meta.revision,
        record.meta.ttl,
        record.meta.version,
        &record.attr,
        &record.data,
    )
}

/// 把总 `size` 中旧字段的 `previous` 字节替换为新字段的 `next` 字节。
///
/// 任一算术下溢/上溢均返回 Registration 容量错误，避免损坏 Selector 的容量记账。
fn replace_size(size: usize, previous: usize, next: usize) -> Result<usize> {
    size.checked_sub(previous)
        .and_then(|value| value.checked_add(next))
        .ok_or_else(|| Error::field(Code::Capacity, "registration"))
}

/// 计算无符号整数 `value` 的十进制位数，不分配临时字符串。
const fn selector_decimal_digits(mut value: u64) -> usize {
    let mut digits = 1;
    while value >= 10 {
        value /= 10;
        digits += 1;
    }
    digits
}

/// 按 `state` 最早 deadline 安排下一次过期唤醒。
///
/// `clock` 提供 Redis 时间上界，`sleep` 是长期 worker 复用的定时器，`pending` 标记定时器分支是否有效；
/// 单次等待最多一天，确保长期运行时会重新评估校准后的 RedisClock。
fn schedule_expiry(state: &SelectorState, clock: RedisClock, mut sleep: std::pin::Pin<&mut tokio::time::Sleep>, pending: &mut bool) {
    let Some(deadline) = state.next_deadline() else {
        *pending = false;
        return;
    };
    let now = clock.upper_now();
    let milliseconds = deadline.saturating_sub(now).min(24 * 60 * 60 * 1000);
    sleep.as_mut().reset(tokio::time::Instant::now() + Duration::from_millis(milliseconds));
    *pending = true;
}

/// 排空订阅初始化阶段由驱动产生的重连通知。
///
/// `receiver` 中正常通知可被消费；lag 或关闭意味着无法证明订阅代连续，分别返回不可用错误。
fn drain_initial_reconnect(receiver: &mut broadcast::Receiver<Server>) -> Result<()> {
    loop {
        match receiver.try_recv() {
            Ok(_) => {}
            Err(broadcast::error::TryRecvError::Empty) => return Ok(()),
            Err(broadcast::error::TryRecvError::Lagged(_)) => {
                return Err(Error::field(Code::Unavailable, "subscription_generation"));
            }
            Err(broadcast::error::TryRecvError::Closed) => {
                return Err(Error::new(Code::Unavailable));
            }
        }
    }
}

/// 把消息接收层的可展示 `error` 统一包装为 SDK 不可用错误。
fn message_receive_error<T: std::fmt::Display>(error: T) -> Error {
    Error::driver(Code::Unavailable, error)
}

/// 构造指定 `generation` 和 `synchronized` 标记的空 SelectorView。
fn empty_view(generation: u64, synchronized: bool) -> SelectorView {
    SelectorView {
        generation,
        synchronized,
        records: HashMap::new(),
        ordered_records: Vec::new(),
        retained: HashMap::new(),
        ordered_retained: Vec::new(),
    }
}

/// 把可变工作 `state` 物化为供读者无锁借用的不可变 SelectorView。
///
/// `generation` 标识订阅代，`synchronized` 决定选择 API 是否可用；记录通过 Arc 共享，不复制字段内容。
fn materialize_view(state: &SelectorState, generation: u64, synchronized: bool) -> SelectorView {
    // 直接排序共享记录，避免为每个 Registration 克隆 UUID，也避免候选访问再次回查 HashMap。
    let mut ordered_records: Vec<_> = state.records.values().cloned().collect();
    ordered_records.sort_unstable_by(|left, right| left.meta.uuid.cmp(&right.meta.uuid));
    let mut ordered_retained: Vec<_> = state.retained.values().cloned().collect();
    ordered_retained.sort_unstable_by(|left, right| left.record.meta.uuid.cmp(&right.record.meta.uuid));
    SelectorView {
        generation,
        synchronized,
        records: state.records.clone(),
        ordered_records,
        retained: state.retained.clone(),
        ordered_retained,
    }
}

/// 生成 `zone`、`type_name` 对应的 Registry Hash/PubSub key。
fn registry_key(zone: &str, type_name: &str) -> String {
    format!("verdandi:registry:{zone}:{type_name}")
}

/// 生成指定 `zone`、`type_name`、`uuid` 对应的 Registration Hash key。
fn registration_key(zone: &str, type_name: &str, uuid: &str) -> String {
    format!("verdandi:registration:{zone}:{type_name}:{uuid}")
}

/// 使用系统随机源生成 128 位十六进制 nonce，供同连接 Pub/Sub 栅栏 PING 使用。
fn nonce() -> Result<String> {
    let mut bytes = [0_u8; 16];
    getrandom::fill(&mut bytes).map_err(|error| Error::driver(Code::Unavailable, error))?;
    Ok(super::hex_lower(&bytes))
}

/// 验证 Redis `value` 是否是对 `nonce` 的 PONG。
///
/// 同时兼容普通字符串/字节响应和 RESP 数组响应；其他形态返回 false。
fn valid_pong(value: Value, nonce: &str) -> bool {
    match value {
        Value::String(value) => &*value == nonce,
        Value::Bytes(value) => value.as_ref() == nonce.as_bytes(),
        Value::Array(values) if values.len() == 2 => {
            let mut values = values.into_iter();
            let kind = values.next().and_then(Value::into_owned_bytes);
            let payload = values.next().and_then(Value::into_owned_bytes);
            kind.is_some_and(|kind| kind.eq_ignore_ascii_case(b"pong")) && payload.is_some_and(|payload| payload == nonce.as_bytes())
        }
        _ => false,
    }
}

/// 根据连续失败次数与领域配置计算带抖动的指数重试延迟。
///
/// 随机区间为 delay 减去 jitter 百分比至 delay，最终值不会超过 maximum。
fn retry_delay(failures: u32, initial: Duration, maximum: Duration, multiplier: u8, jitter_percent: u8) -> Duration {
    let mut delay = initial;
    for _ in 0..failures {
        delay = delay.saturating_mul(u32::from(multiplier)).min(maximum);
        if delay == maximum {
            break;
        }
    }
    let span = delay.saturating_mul(u32::from(jitter_percent)) / 100;
    let floor = delay.saturating_sub(span);
    let jitter = fastrand::u64(0..=u64::try_from(span.as_millis()).unwrap_or(u64::MAX));
    floor.saturating_add(Duration::from_millis(jitter))
}

/// 等待 `delay` 或 `token` 取消，返回是否完整等到延迟结束。
async fn wait_cancelled(token: &CancellationToken, delay: Duration) -> bool {
    tokio::select! {
        () = token.cancelled() => false,
        () = tokio::time::sleep(delay) => true,
    }
}

#[cfg(test)]
#[path = "../../tests/internal/registration/selector.rs"]
mod tests;
