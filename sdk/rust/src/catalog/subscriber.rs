use std::collections::btree_map::Entry as BTreeEntry;
use std::collections::{BTreeMap, BTreeSet};
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, RwLock};

use fred::clients::SubscriberClient;
use fred::prelude::*;
use fred::types::Value;
use fred::types::config::Server;
use fred::types::scan::Scanner;
use fred::types::sorted_sets::{ZRange, ZRangeBound, ZRangeKind};
use futures_util::{StreamExt, stream};
use tokio::sync::{Mutex as AsyncMutex, Notify, broadcast, mpsc, oneshot};
use tokio_util::sync::CancellationToken;

use crate::{Code, Error, Fields, Result};

use super::client::{ActiveGuard, Client, ClientInner};
use super::event::{CatalogEvent, EventKind, decode_event};
use super::model::{
    Entry, Kind, MAX_FIELDS, MAX_REVISION, NormalizedSubscription, Path, RawState, Status, Subscription, canonical_u64, canonical_usize, deleted_key, live_key,
    meta_key, read_keys, validate_value, zone_prefix,
};
use super::scripts::{ScriptKind, parse_revision, take_string, value_pairs, value_string};

/// 由一个常驻 Pub/Sub 读取任务和一个仅在存在权威工作时创建的临时同步任务驱动的完整内存原始视图。
pub struct Subscriber {
    inner: Arc<SubscriberInner>,
}

struct SubscriberInner {
    client: Arc<ClientInner>,
    subscription: NormalizedSubscription,
    scope: String,
    channel_prefix: String,
    pubsub: SubscriberClient,
    entries: RwLock<BTreeMap<Path, Entry>>,
    view_bytes: Mutex<u64>,
    cursor: AtomicU64,
    scope_status: AtomicU8,
    errors: broadcast::Sender<Error>,
    cancel: CancellationToken,
    sync: Mutex<SyncState>,
    fence: mpsc::Sender<FenceRequest>,
    persistence: AsyncMutex<()>,
    store_error_reported: AtomicBool,
    workers: AtomicUsize,
    done: Notify,
    guard: Mutex<Option<ActiveGuard>>,
    closed: AtomicBool,
}

#[derive(Default)]
struct SyncBatch {
    scope: bool,
    force_full: bool,
    align: bool,
    paths: BTreeSet<Path>,
    waiters: Vec<oneshot::Sender<Result<()>>>,
}

#[derive(Default)]
struct SyncState {
    batch: SyncBatch,
    running: bool,
}

impl SyncState {
    /// 取走当前非空批次；没有工作时原子释放临时任务槽。
    ///
    /// 请求方使用同一把锁观察 `running`，因此请求要么进入当前任务的下一批，
    /// 要么在本函数释放槽后创建新任务，不存在空闲转换期间的漏唤醒窗口。
    fn take_or_stop(&mut self) -> Option<SyncBatch> {
        if !self.batch.scope && self.batch.paths.is_empty() {
            self.running = false;
            return None;
        }
        Some(std::mem::take(&mut self.batch))
    }
}

struct FenceRequest {
    done: oneshot::Sender<()>,
}

#[derive(Clone, Copy)]
struct ZoneMetadata {
    revision: u64,
    floor: u64,
}

impl Subscriber {
    /// 先订阅，再权威对齐 `subscription` 请求的范围，最后返回可用 Subscriber。
    ///
    /// `client` 提供 Zone、脚本和共享传输。构造会初始化专用 Pub/Sub 连接、完成全部订阅、
    /// 恢复可选检查点、启动一个常驻读取任务及一个临时初始同步任务，并在 `sync_timeout` 内等待首轮对齐。
    /// 任一步失败都会取消并等待已启动任务，不返回半同步对象。
    pub async fn new(client: &Client, subscription: Subscription) -> Result<Self> {
        let subscription = NormalizedSubscription::new(&client.inner.config.zone, subscription)?;
        let guard = client.inner.admit()?;
        let pubsub = client.inner.subscriber(client.inner.config.event_buffer_capacity)?;
        let mut messages = pubsub.message_rx();
        let mut reconnects = pubsub.reconnect_rx();
        tokio::time::timeout(client.inner.config.timeout, pubsub.init())
            .await
            .map_err(|error| Error::driver(Code::Deadline, error))?
            .map_err(|error| Error::driver(Code::Unavailable, error))?;
        while reconnects.try_recv().is_ok() {}
        if !subscription.channels.is_empty() {
            client.inner.command(pubsub.subscribe(subscription.channels.clone()), Code::Unavailable).await?;
        }
        if !subscription.patterns.is_empty() {
            client
                .inner
                .command(pubsub.psubscribe(subscription.patterns.clone()), Code::Unavailable)
                .await?;
        }

        // 有界栅栏通道只串行化 PING 完成证明；临时同步任务通过互斥状态直接合并，不需要常驻唤醒接收者。
        let (errors, _) = broadcast::channel(client.inner.config.error_buffer_capacity);
        let (fence, fence_receiver) = mpsc::channel(16);
        let cancel = client.inner.shutdown.child_token();
        let inner = Arc::new(SubscriberInner {
            client: Arc::clone(&client.inner),
            scope: subscription.checkpoint_scope(),
            channel_prefix: format!("{}:", zone_prefix(&client.inner.config.zone)),
            subscription,
            pubsub,
            entries: RwLock::new(BTreeMap::new()),
            view_bytes: Mutex::new(0),
            cursor: AtomicU64::new(0),
            scope_status: AtomicU8::new(status_byte(Status::Synchronizing)),
            errors,
            cancel,
            sync: Mutex::new(SyncState::default()),
            fence,
            persistence: AsyncMutex::new(()),
            store_error_reported: AtomicBool::new(false),
            workers: AtomicUsize::new(1),
            done: Notify::new(),
            guard: Mutex::new(Some(guard)),
            closed: AtomicBool::new(false),
        });
        // 检查点只提供陈旧起点，恢复的 Entry 统一标记为 Synchronizing 后再与 Redis 对齐。
        inner.restore().await;
        for path in &inner.subscription.paths {
            let _ = inner.entry(path, Status::Synchronizing);
        }

        // 先登记临时初始同步，再启动常驻读取任务；即使连接立即失败，动态任务计数也不会提前完成终止收尾。
        let (ready, receiver) = oneshot::channel();
        inner.request_scope(false, Some(ready));
        let reader = Arc::clone(&inner);
        tokio::spawn(async move {
            reader.read_loop(&mut messages, &mut reconnects, fence_receiver).await;
            reader.finish_worker().await;
        });

        // 通过普通合并队列请求初始同步，使初始化与运行期修复遵循同一状态机。
        let result = tokio::time::timeout(inner.client.config.sync_timeout, receiver).await;
        match result {
            Ok(Ok(Ok(()))) => Ok(Self { inner }),
            Ok(Ok(Err(error))) => {
                inner.cancel.cancel();
                inner.wait_finished().await;
                Err(error)
            }
            Ok(Err(_)) => {
                inner.cancel.cancel();
                inner.wait_finished().await;
                Err(Error::new(Code::Closed))
            }
            Err(error) => {
                inner.cancel.cancel();
                inner.wait_finished().await;
                Err(Error::driver(Code::Deadline, error))
            }
        }
    }

    /// 返回 `path` 对应的稳定本地 Entry，不执行 Redis 或磁盘 I/O。
    ///
    /// Path 不在订阅范围或 Subscriber 已关闭时返回 `None`；已覆盖但缺失的 Path
    /// 仍返回状态为 Absent/Synchronizing 的稳定句柄。
    pub fn find(&self, path: &Path) -> Option<Entry> {
        if self.inner.closed.load(Ordering::Acquire) || !self.inner.subscription.covers(path) {
            return None;
        }
        let status = byte_status(self.inner.scope_status.load(Ordering::Acquire));
        Some(self.inner.entry(path, if status == Status::Present { Status::Absent } else { status }))
    }

    /// 订阅尽力而为的异步恢复与检查点诊断。
    ///
    /// 广播接收者落后时可能丢失旧诊断；正确性状态应通过 Entry/Status 判断。
    pub fn subscribe_errors(&self) -> broadcast::Receiver<Error> {
        self.inner.errors.subscribe()
    }

    /// 取消并等待常驻读取任务及当前可选同步任务退出。
    ///
    /// Close 幂等，不删除 Redis Catalog 数据或关闭共享 Client；完成后全部 Entry 为 Closed。
    pub async fn close(&self) -> Result<()> {
        self.inner.cancel.cancel();
        self.inner.wait_finished().await;
        Ok(())
    }
}

impl Drop for Subscriber {
    /// 释放句柄时同步发出取消；Drop 不能等待，确定汇合由显式 `close().await` 提供。
    fn drop(&mut self) {
        self.inner.cancel.cancel();
    }
}

impl SubscriberInner {
    /// 从可选本地检查点恢复 cursor 与覆盖范围内的 Entry。
    ///
    /// 阻塞存储工作在 blocking 池执行；任何文件错误只停用检查点并报告一次，
    /// 不阻止后续 Redis 权威同步。
    async fn restore(self: &Arc<Self>) {
        let Some(checkpoint) = self.client.checkpoint.clone() else {
            return;
        };
        let zone = self.client.config.zone.clone();
        let scope = self.scope.clone();
        let maximum = self.client.config.max_record_bytes;
        let result = tokio::task::spawn_blocking(move || checkpoint.load(&zone, &scope, maximum)).await;
        match result {
            Ok(Ok((cursor, states))) => {
                let total = states.values().try_fold(0_u64, |total, state| {
                    u64::try_from(state.encoded_bytes).ok().and_then(|size| total.checked_add(size))
                });
                let Some(total) = total else {
                    self.report(Error::field(Code::Capacity, "max_view_bytes"));
                    return;
                };
                if self.client.config.max_view_bytes != 0 && total > self.client.config.max_view_bytes {
                    // 检查点只负责加速；超出当前内存预算时整份放弃并从 Redis 权威状态重建。
                    self.report(Error::field(Code::Capacity, "max_view_bytes"));
                    return;
                }
                self.cursor.store(cursor, Ordering::Release);
                if let Ok(mut entries) = self.entries.write() {
                    for (path, state) in states {
                        if self.subscription.covers(&path) {
                            let entry = Entry::new(path.clone(), Status::Synchronizing);
                            entry.store(state.with_status(Status::Synchronizing));
                            entries.insert(path, entry);
                        }
                    }
                }
                if let Ok(mut view_bytes) = self.view_bytes.lock() {
                    *view_bytes = total;
                }
            }
            Ok(Err(error)) => self.disable_store(error),
            Err(error) => self.disable_store(error.to_string()),
        }
    }

    /// 返回 `path` 的稳定 Entry；首次出现时以 `status` 创建。
    ///
    /// 先读锁快查，缺失时再写锁二次确认。锁中毒时返回独立 Closed Entry，避免 panic。
    fn entry(&self, path: &Path, status: Status) -> Entry {
        if let Ok(entries) = self.entries.read() {
            if let Some(entry) = entries.get(path) {
                return entry.clone();
            }
        }
        let mut entries = match self.entries.write() {
            Ok(entries) => entries,
            Err(_) => return Entry::new(path.clone(), Status::Closed),
        };
        entries.entry(path.clone()).or_insert_with(|| Entry::new(path.clone(), status)).clone()
    }

    /// 持续处理 Pub/Sub 消息、连接重建和同步栅栏请求。
    ///
    /// `messages`/`reconnects` 来自专用 Fred 客户端，`fences` 由同步任务发送。
    /// `cancel` 结束循环后在有界超时内尽力退出 Pub/Sub 驱动。
    async fn read_loop(
        self: &Arc<Self>,
        messages: &mut broadcast::Receiver<fred::types::Message>,
        reconnects: &mut broadcast::Receiver<Server>,
        mut fences: mpsc::Receiver<FenceRequest>,
    ) {
        // biased 顺序优先响应关闭和栅栏，避免持续消息流饿死同步完成证明。
        loop {
            tokio::select! {
                biased;
                () = self.cancel.cancelled() => break,
                Some(fence) = fences.recv() => {
                    self.drain_messages(messages).await;
                    let _ = fence.done.send(());
                }
                reconnect = reconnects.recv() => {
                    match reconnect {
                        Ok(_) | Err(broadcast::error::RecvError::Lagged(_)) => {
                            self.mark_scope(Status::Synchronizing);
                            self.resubscribe().await;
                        }
                        Err(broadcast::error::RecvError::Closed) => break,
                    }
                }
                message = messages.recv() => {
                    match message {
                        Ok(message) => self.handle_message(message).await,
                        Err(broadcast::error::RecvError::Lagged(_)) => {
                            self.mark_scope(Status::Unavailable);
                            self.report(Error::field(Code::Unavailable, "pubsub_lag"));
                            self.request_scope(false, None);
                        }
                        Err(broadcast::error::RecvError::Closed) => break,
                    }
                }
            }
        }
        let _ = tokio::time::timeout(self.client.config.timeout, self.pubsub.quit()).await;
    }

    /// 非阻塞排空 `messages` 当前已排队的所有通知。
    ///
    /// 此函数只由读取任务调用；Lagged 会把范围标记不可用并请求权威修复。
    async fn drain_messages(self: &Arc<Self>, messages: &mut broadcast::Receiver<fred::types::Message>) {
        loop {
            match messages.try_recv() {
                Ok(message) => self.handle_message(message).await,
                Err(broadcast::error::TryRecvError::Lagged(_)) => {
                    self.mark_scope(Status::Unavailable);
                    self.report(Error::field(Code::Unavailable, "pubsub_lag"));
                    self.request_scope(false, None);
                }
                Err(broadcast::error::TryRecvError::Empty | broadcast::error::TryRecvError::Closed) => return,
            }
        }
    }

    /// 在 Fred 报告重连后重复恢复全部频道/模式订阅。
    ///
    /// 每次失败标记 Unavailable、报告错误并退避 250ms；成功后请求范围对齐。
    async fn resubscribe(self: &Arc<Self>) {
        let mut failures = 0_u32;
        loop {
            if self.cancel.is_cancelled() {
                return;
            }
            match self.client.command(self.pubsub.resubscribe_all(), Code::Unavailable).await {
                Ok(()) => {
                    self.request_scope(false, None);
                    return;
                }
                Err(error) => {
                    self.mark_scope(Status::Unavailable);
                    self.report(error);
                }
            }
            let delay = self.client.config.recovery_delay(failures);
            failures = failures.saturating_add(1);
            tokio::select! {
                () = self.cancel.cancelled() => return,
                () = tokio::time::sleep(delay) => {}
            }
        }
    }

    /// 校验一条 Fred Pub/Sub `message` 的频道、覆盖范围和二进制负载并应用事件。
    ///
    /// 频道/消息损坏不会直接修改 Entry，而会报告诊断并请求范围或精确 Path 修复。
    async fn handle_message(self: &Arc<Self>, message: fred::types::Message) {
        let channel = message.channel.to_string();
        let Some(member) = channel.strip_prefix(&self.channel_prefix) else {
            self.report(Error::field(Code::Target, "channel"));
            self.request_scope(false, None);
            return;
        };
        let Some(path) = Path::from_member(member) else {
            self.report(Error::field(Code::Target, "channel"));
            self.request_scope(false, None);
            return;
        };
        if !self.subscription.covers(&path) {
            self.report(Error::field(Code::Target, "channel"));
            self.request_scope(false, None);
            return;
        }
        let Some(payload) = message.value.into_owned_bytes() else {
            self.report(Error::field(Code::Corrupt, "notification"));
            self.request_path(path, None);
            return;
        };
        match decode_event(&payload, &path, self.client.config.max_record_bytes) {
            Ok(event) => self.apply_event(event).await,
            Err(error) => {
                self.report(error);
                self.request_path(path, None);
            }
        }
    }

    /// 用 revision CAS 把完整 `event` 应用到稳定 Entry。
    ///
    /// 旧/重复事件直接忽略；Patch 仅在本地完整基准、Kind 与 encoded_bytes 全部匹配时应用，
    /// 否则请求权威 Path 读取。CAS 竞争会重新基于最新状态判断。
    async fn apply_event(self: &Arc<Self>, event: CatalogEvent) {
        let entry = self.entry(&event.path, Status::Synchronizing);
        loop {
            let current = entry.state();
            if event.revision <= current.revision {
                return;
            }
            // Replace/Delete 可独立构造完整状态；Patch 必须在当前不可变基准上合并并复核容量。
            let next = match event.kind {
                EventKind::Replace => RawState {
                    revision: event.revision,
                    replace_revision: event.revision,
                    status: Status::Present,
                    kind: event.value_kind,
                    encoded_bytes: event.encoded_bytes,
                    fields: Arc::clone(&event.fields),
                },
                EventKind::Delete => RawState {
                    revision: event.revision,
                    replace_revision: 0,
                    status: Status::Deleted,
                    kind: None,
                    encoded_bytes: 0,
                    fields: Arc::new(Fields::new()),
                },
                EventKind::Patch => {
                    if !current.complete_present() || current.revision != event.base_revision || current.kind != event.value_kind {
                        self.request_path(event.path.clone(), None);
                        return;
                    }
                    let mut fields = current.fields.as_ref().clone();
                    fields.extend(event.fields.iter().map(|(name, value)| (name.clone(), value.clone())));
                    let Some(kind) = current.kind else {
                        self.request_path(event.path.clone(), None);
                        return;
                    };
                    let Ok(encoded_bytes) = validate_value(kind, &fields, self.client.config.max_record_bytes) else {
                        self.request_path(event.path.clone(), None);
                        return;
                    };
                    if encoded_bytes != event.encoded_bytes {
                        self.request_path(event.path.clone(), None);
                        return;
                    }
                    RawState {
                        revision: event.revision,
                        replace_revision: current.replace_revision,
                        status: Status::Present,
                        kind: current.kind,
                        encoded_bytes,
                        fields: Arc::new(fields),
                    }
                }
            };
            let next = match self.install_state(&entry, &current, next) {
                Ok(next) => next,
                Err(error) => {
                    self.report(error);
                    self.request_path(event.path.clone(), None);
                    return;
                }
            };
            if let Some(next) = next {
                self.persist_entry(&entry, next).await;
                return;
            }
        }
    }

    /// 在同一短锁内提交 Entry CAS 与完整值字节总量，防止并发事件突破本地视图预算。
    fn install_state(&self, entry: &Entry, current: &Arc<RawState>, next: RawState) -> Result<Option<Arc<RawState>>> {
        let mut total = self.view_bytes.lock().map_err(|_| Error::field(Code::Corrupt, "max_view_bytes"))?;
        let observed = entry.state();
        if !Arc::ptr_eq(&observed, current) {
            return Ok(None);
        }
        let current_bytes = u64::try_from(current.encoded_bytes).map_err(|_| Error::field(Code::Corrupt, "max_view_bytes"))?;
        let next_bytes = u64::try_from(next.encoded_bytes).map_err(|_| Error::field(Code::Corrupt, "max_view_bytes"))?;
        let projected = total
            .checked_sub(current_bytes)
            .and_then(|value| value.checked_add(next_bytes))
            .ok_or_else(|| Error::field(Code::Corrupt, "max_view_bytes"))?;
        if self.client.config.max_view_bytes != 0 && projected > self.client.config.max_view_bytes {
            return Err(Error::field(Code::Capacity, "max_view_bytes"));
        }
        let installed = entry.compare_and_swap(current, next);
        if installed.is_some() {
            *total = projected;
        }
        Ok(installed)
    }

    /// 合并一次全范围同步请求。
    ///
    /// `force_full` 要求跳过增量索引；`waiter` 可等待本批完成。范围请求覆盖已排队 Path。
    /// 若当前没有同步任务，本次请求同时取得唯一临时任务槽并负责创建任务。
    fn request_scope(self: &Arc<Self>, force_full: bool, waiter: Option<oneshot::Sender<Result<()>>>) {
        let start = if let Ok(mut sync) = self.sync.lock() {
            sync.batch.scope = true;
            sync.batch.force_full |= force_full;
            sync.batch.align = true;
            sync.batch.paths.clear();
            if let Some(waiter) = waiter {
                sync.batch.waiters.push(waiter);
            }
            self.start_sync_locked(&mut sync)
        } else {
            false
        };
        if start {
            self.spawn_sync_worker();
        }
    }

    /// 合并一个精确 `path` 修复请求，并可附加完成 `waiter`。
    ///
    /// 若已有范围请求则无需重复保存 Path；对应 Entry 立即标为 Synchronizing。
    fn request_path(self: &Arc<Self>, path: Path, waiter: Option<oneshot::Sender<Result<()>>>) {
        let entry = self.entry(&path, Status::Synchronizing);
        mark_entry(&entry, Status::Synchronizing);
        let start = if let Ok(mut sync) = self.sync.lock() {
            if !sync.batch.scope {
                if sync.batch.paths.contains(&path) || sync.batch.paths.len() < self.client.config.event_buffer_capacity {
                    sync.batch.paths.insert(path);
                } else {
                    // Path 合并集合满时退化为范围恢复，保持内存有界且仍由权威索引找回全部变化。
                    sync.batch.scope = true;
                    sync.batch.align = true;
                    sync.batch.paths.clear();
                }
            }
            if let Some(waiter) = waiter {
                sync.batch.waiters.push(waiter);
            }
            self.start_sync_locked(&mut sync)
        } else {
            false
        };
        if start {
            self.spawn_sync_worker();
        }
    }

    /// 在持有同步状态锁时为非空 pending 取得唯一临时任务所有权。
    ///
    /// workers 在释放锁和 `spawn` 前增加，使 Close 不会错过已经获准启动的任务。
    fn start_sync_locked(&self, sync: &mut SyncState) -> bool {
        if sync.running || self.cancel.is_cancelled() || self.closed.load(Ordering::Acquire) {
            return false;
        }
        sync.running = true;
        self.workers.fetch_add(1, Ordering::AcqRel);
        true
    }

    /// 为已登记的临时任务创建 Tokio task；任务自己负责结束计数与最后收尾。
    fn spawn_sync_worker(self: &Arc<Self>) {
        let synchronizer = Arc::clone(self);
        tokio::spawn(async move {
            synchronizer.sync_loop().await;
            synchronizer.finish_worker().await;
        });
    }

    /// 在互斥锁内取走当前待同步工作；空状态会原子释放临时任务槽。
    fn take_batch(&self) -> Option<SyncBatch> {
        match self.sync.lock() {
            Ok(mut sync) => sync.take_or_stop(),
            Err(poisoned) => poisoned.into_inner().take_or_stop(),
        }
    }

    /// owner 取消且批次尚未取空时释放当前临时任务槽。
    fn stop_sync_worker(&self) {
        let mut sync = match self.sync.lock() {
            Ok(sync) => sync,
            Err(poisoned) => poisoned.into_inner(),
        };
        sync.running = false;
    }

    /// 终止收尾取走全部尚未完成的请求，不改变已经停止的临时任务状态。
    fn drain_pending(&self) -> SyncBatch {
        let mut sync = match self.sync.lock() {
            Ok(sync) => sync,
            Err(poisoned) => poisoned.into_inner(),
        };
        std::mem::take(&mut sync.batch)
    }

    /// 把当前批次的对齐责任和 waiter 合并回同步期间新到达的待处理工作。
    ///
    /// 返回 true 表示存在后续工作；相关范围/Entry 会重新标为 Synchronizing。
    fn carry_batch(&self, batch: &mut SyncBatch) -> bool {
        let mut sync = match self.sync.lock() {
            Ok(sync) => sync,
            Err(poisoned) => poisoned.into_inner(),
        };
        if !sync.batch.scope && sync.batch.paths.is_empty() {
            return false;
        }
        sync.batch.align |= batch.align;
        sync.batch.waiters.append(&mut batch.waiters);
        let scope = sync.batch.scope;
        let paths = sync.batch.paths.clone();
        drop(sync);
        if scope {
            self.mark_scope(Status::Synchronizing);
        } else {
            for path in paths {
                mark_entry(&self.entry(&path, Status::Synchronizing), Status::Synchronizing);
            }
        }
        true
    }

    /// 原子更新范围 `status`，并把当前全部 Entry 标记为相同非终止状态。
    fn mark_scope(&self, status: Status) {
        self.scope_status.store(status_byte(status), Ordering::Release);
        let entries = self
            .entries
            .read()
            .map(|entries| entries.values().cloned().collect::<Vec<_>>())
            .unwrap_or_default();
        for entry in entries {
            mark_entry(&entry, status);
        }
    }

    /// 在一次成功权威对齐后发布范围健康，并按内容把每个 Entry 恢复为 Present/Absent/Deleted。
    fn mark_aligned(&self) {
        self.scope_status.store(status_byte(Status::Present), Ordering::Release);
        let entries = self
            .entries
            .read()
            .map(|entries| entries.values().cloned().collect::<Vec<_>>())
            .unwrap_or_default();
        for entry in entries {
            loop {
                let current = entry.state();
                let status = if current.kind.is_some() {
                    Status::Present
                } else if current.revision != 0 {
                    Status::Deleted
                } else {
                    Status::Absent
                };
                if current.status == status || entry.compare_and_swap(&current, current.with_status(status)).is_some() {
                    break;
                }
            }
        }
    }

    /// 尽力广播一个异步 `error`；没有接收者或接收者落后不会阻塞工作任务。
    fn report(&self, error: Error) {
        let _ = self.errors.send(error);
    }

    /// 永久停用本地检查点，并且只报告第一次 `detail`。
    ///
    /// Redis 同步继续工作；错误文本由 Error 构造限长，避免磁盘错误淹没诊断。
    fn disable_store(&self, detail: String) {
        if let Some(checkpoint) = &self.client.checkpoint {
            checkpoint.disable();
        }
        if !self.store_error_reported.swap(true, Ordering::AcqRel) {
            self.report(Error::field_driver(Code::Unavailable, "local_store_path", detail));
        }
    }

    /// 串行持久化 `entry` 的候选 `state`。
    ///
    /// 写入前再次确认该 `Arc` 仍是 Entry 当前状态，防止慢磁盘把迟到旧 revision 写回。
    /// 阻塞 redb 操作在 blocking 池执行，失败会停用整个检查点。
    async fn persist_entry(&self, entry: &Entry, state: Arc<RawState>) {
        let Some(checkpoint) = self.client.checkpoint.clone() else {
            return;
        };
        let _guard = self.persistence.lock().await;
        if !Arc::ptr_eq(&entry.state(), &state) || checkpoint.disabled() {
            return;
        }
        let zone = self.client.config.zone.clone();
        let scope = self.scope.clone();
        let path = entry.path().clone();
        let maximum = self.client.config.max_record_bytes;
        let result = tokio::task::spawn_blocking(move || checkpoint.save_entry(&zone, &scope, &path, &state, maximum)).await;
        match result {
            Ok(Ok(())) => {}
            Ok(Err(error)) => self.disable_store(error),
            Err(error) => self.disable_store(error.to_string()),
        }
    }

    /// 串行、单调持久化范围 `revision` cursor。
    ///
    /// 未配置或已停用检查点时为空操作；阻塞 I/O 在 blocking 池执行。
    async fn persist_cursor(&self, revision: u64) {
        let Some(checkpoint) = self.client.checkpoint.clone() else {
            return;
        };
        let _guard = self.persistence.lock().await;
        if checkpoint.disabled() {
            return;
        }
        let zone = self.client.config.zone.clone();
        let scope = self.scope.clone();
        let result = tokio::task::spawn_blocking(move || checkpoint.save_cursor(&zone, &scope, revision)).await;
        match result {
            Ok(Ok(())) => {}
            Ok(Err(error)) => self.disable_store(error),
            Err(error) => self.disable_store(error.to_string()),
        }
    }

    /// 标记一个拥有型任务结束；最后一个任务负责拒绝残留请求、发布终止状态并释放 Client Guard。
    async fn finish_worker(self: &Arc<Self>) {
        if self.workers.fetch_sub(1, Ordering::AcqRel) != 1 {
            return;
        }
        let mut pending = self.drain_pending();
        notify_waiters(&mut pending.waiters, Err(Error::new(Code::Closed)));
        self.closed.store(true, Ordering::Release);
        self.mark_scope(Status::Closed);
        if let Ok(mut guard) = self.guard.lock() {
            guard.take();
        }
        self.done.notify_waiters();
    }

    /// 等待常驻读取任务及当前可选临时同步任务全部调用 `finish_worker`。
    ///
    /// Notify 在复查前启用，避免最后任务完成与 waiter 注册之间丢失唤醒。
    async fn wait_finished(&self) {
        while self.workers.load(Ordering::Acquire) != 0 {
            let notified = self.done.notified();
            tokio::pin!(notified);
            notified.as_mut().enable();
            if self.workers.load(Ordering::Acquire) == 0 {
                break;
            }
            notified.await;
        }
    }

    /// 串行处理合并后的范围或 Path 权威修复批次，队列取空后立即退出。
    ///
    /// 每批受 `sync_timeout` 限制。无显式 waiter 的暂时失败会退避后自动重排，
    /// 初始化 waiter 则立即收到精确结果。
    async fn sync_loop(self: &Arc<Self>) {
        let mut failures = 0_u32;
        loop {
            if self.cancel.is_cancelled() {
                self.stop_sync_worker();
                return;
            }
            // 取出稳定批次后再做 I/O，使 Pub/Sub 读取任务可继续合并新的恢复请求。
            let Some(mut batch) = self.take_batch() else {
                return;
            };
            if batch.scope {
                self.mark_scope(Status::Synchronizing);
            } else {
                for path in &batch.paths {
                    mark_entry(&self.entry(path, Status::Synchronizing), Status::Synchronizing);
                }
            }
            // 一次范围或精确修复共享总超时，防止分页/并发读取无限占有同步任务。
            let operation = tokio::time::timeout(self.client.config.sync_timeout, async {
                if batch.scope {
                    self.synchronize_scope(batch.force_full).await
                } else {
                    self.synchronize_exact(batch.paths.clone()).await
                }
            });
            let result = tokio::select! {
                () = self.cancel.cancelled() => {
                    notify_waiters(&mut batch.waiters, Err(Error::new(Code::Closed)));
                    self.stop_sync_worker();
                    return;
                }
                result = operation => result,
            };
            let result = match result {
                Ok(result) => result,
                Err(error) => Err(Error::driver(Code::Deadline, error)),
            };
            if let Err(error) = result {
                let had_waiters = !batch.waiters.is_empty();
                if batch.scope || batch.align {
                    self.mark_scope(Status::Unavailable);
                } else {
                    for path in &batch.paths {
                        mark_entry(&self.entry(path, Status::Unavailable), Status::Unavailable);
                    }
                }
                self.report(error.clone());
                notify_waiters(&mut batch.waiters, Err(error.clone()));
                if !had_waiters && matches!(error.code(), Code::Unavailable | Code::Deadline) {
                    let delay = self.client.config.recovery_delay(failures);
                    failures = failures.saturating_add(1);
                    tokio::select! {
                        () = self.cancel.cancelled() => {
                            self.stop_sync_worker();
                            return;
                        },
                        () = tokio::time::sleep(delay) => {}
                    }
                    if batch.scope || batch.align {
                        self.request_scope(batch.force_full, None);
                    } else {
                        for path in batch.paths {
                            self.request_path(path, None);
                        }
                    }
                }
                continue;
            }
            failures = 0;
            if batch.align {
                self.mark_aligned();
            }
            if self.carry_batch(&mut batch) {
                continue;
            }
            notify_waiters(&mut batch.waiters, Ok(()));
        }
    }

    /// 把整个订阅范围收敛到权威 Redis 状态。
    ///
    /// `force_full` 跳过 cursor 增量索引。宽范围使用 metadata/floor 决定全量或增量，
    /// 精确 Path 范围直接读取固定集合。读取后以 Pub/Sub PING 和再次元数据读取验证顺序。
    async fn synchronize_scope(self: &Arc<Self>, mut force_full: bool) -> Result<()> {
        if !self.subscription.broad() {
            let metadata = self.read_metadata().await?;
            let maximum = self.synchronize_paths(self.subscription.paths.clone()).await?;
            self.ping_fence().await?;
            let after = self.read_metadata().await?;
            if maximum > after.revision {
                return Err(Error::field(Code::Corrupt, "@revision").with_revision(maximum));
            }
            self.cursor.store(metadata.revision, Ordering::Release);
            self.persist_cursor(metadata.revision).await;
            return Ok(());
        }
        // floor 在同步期间跨过起始 revision 时，本轮增量证明失效，必须在同一调用内全量重试。
        loop {
            let metadata = self.read_metadata().await?;
            let cursor = self.cursor.load(Ordering::Acquire);
            let full = force_full || cursor == 0 || cursor > metadata.revision || cursor < metadata.floor;
            let mut paths = if full {
                self.collect_full_paths().await?
            } else {
                self.collect_changed_paths(cursor, metadata.revision).await?
            };
            paths.extend(self.subscription.paths.iter().cloned());
            if full {
                paths.extend(self.entry_paths());
            }
            let maximum = self.synchronize_paths(paths).await?;
            self.ping_fence().await?;
            let after = self.read_metadata().await?;
            if maximum > after.revision {
                return Err(Error::field(Code::Corrupt, "@revision").with_revision(maximum));
            }
            if metadata.revision < after.floor {
                force_full = true;
                continue;
            }
            self.cursor.store(metadata.revision, Ordering::Release);
            self.persist_cursor(metadata.revision).await;
            return Ok(());
        }
    }

    /// 权威读取 `paths`，完成 Pub/Sub 栅栏并验证观察到的最大 revision 不超出全局元数据。
    async fn synchronize_exact(self: &Arc<Self>, paths: BTreeSet<Path>) -> Result<()> {
        let maximum = self.synchronize_paths(paths).await?;
        self.ping_fence().await?;
        let metadata = self.read_metadata().await?;
        if maximum > metadata.revision {
            return Err(Error::field(Code::Corrupt, "@revision").with_revision(maximum));
        }
        Ok(())
    }

    /// 以配置的 `max_inflight_reads` 并发只读 Lua 同步 `paths`。
    ///
    /// 每个 Path 捕获自己的不可变 `base`，解析出完整候选后用 CAS 发布；竞争失败表示
    /// Pub/Sub 已发布更新，候选不会覆盖更近状态。返回所有回复中的最大 revision。
    async fn synchronize_paths(self: &Arc<Self>, paths: BTreeSet<Path>) -> Result<u64> {
        let reads = stream::iter(paths.into_iter().map(|path| {
            let subscriber = Arc::clone(self);
            async move {
                let entry = subscriber.entry(&path, Status::Synchronizing);
                let base = entry.state();
                let local_revision = if base.complete_present() { base.revision } else { 0 };
                let value = subscriber
                    .client
                    .call_script(
                        ScriptKind::Read,
                        read_keys(&subscriber.client.config.zone, &path),
                        vec![path.member().into(), local_revision.to_string().into()],
                        false,
                    )
                    .await?;
                let candidate = parse_read_reply(value, &base, subscriber.client.config.max_record_bytes)?;
                Ok::<_, Error>((entry, base, candidate))
            }
        }))
        .buffer_unordered(self.client.config.max_inflight_reads);
        tokio::pin!(reads);

        // 逐项发布与持久化，任一错误终止本批；Entry 在批完成前保持 Synchronizing。
        let mut maximum = 0_u64;
        while let Some(result) = reads.next().await {
            let (entry, base, candidate) = result?;
            maximum = maximum.max(candidate.revision);
            let installed = self.install_state(&entry, &base, candidate)?;
            if let Some(candidate) = installed {
                self.persist_entry(&entry, candidate).await;
            }
        }
        Ok(maximum)
    }

    /// 读取并校验 Zone 全局 `@revision` 与 `@floor_revision` 元数据。
    ///
    /// 缺失 Hash 表示全零初始状态；任何额外/缺失字段或 floor 大于 revision 都视为 Corrupt。
    async fn read_metadata(&self) -> Result<ZoneMetadata> {
        let values: BTreeMap<String, String> = self
            .client
            .command(self.client.driver().hgetall(meta_key(&self.client.config.zone)), Code::Unavailable)
            .await?;
        if values.is_empty() {
            return Ok(ZoneMetadata { revision: 0, floor: 0 });
        }
        if values.len() != 2 {
            return Err(Error::field(Code::Corrupt, "catalog_meta"));
        }
        let revision = canonical_u64(
            values.get("@revision").ok_or_else(|| Error::field(Code::Corrupt, "@revision"))?,
            true,
            "@revision",
        )?;
        let floor = canonical_u64(
            values.get("@floor_revision").ok_or_else(|| Error::field(Code::Corrupt, "@floor_revision"))?,
            true,
            "@floor_revision",
        )?;
        if floor > revision {
            return Err(Error::field(Code::Corrupt, "@floor_revision"));
        }
        Ok(ZoneMetadata { revision, floor })
    }

    /// 从 live/deleted ZSET 收集 `(from, through]` 范围内被订阅覆盖的 Path。
    ///
    /// 两个边界均为安全 revision；分页以严格递增 score 推进，非整数、倒退或越界索引返回 Corrupt。
    async fn collect_changed_paths(&self, from: u64, through: u64) -> Result<BTreeSet<Path>> {
        let mut paths = BTreeSet::new();
        if through <= from {
            return Ok(paths);
        }
        for key in [live_key(&self.client.config.zone), deleted_key(&self.client.config.zone)] {
            let mut last = from;
            loop {
                let minimum = ZRange {
                    kind: ZRangeKind::Exclusive,
                    range: ZRangeBound::Score(last as f64),
                };
                let values: Vec<(String, f64)> = self
                    .client
                    .command(
                        self.client
                            .driver()
                            .zrangebyscore(key.clone(), minimum, through as f64, true, Some((0, self.client.config.scan_page_size as i64))),
                        Code::Unavailable,
                    )
                    .await?;
                if values.is_empty() {
                    break;
                }
                let count = values.len();
                for (member, score) in values {
                    let revision = exact_score(score)?;
                    if revision <= last || revision > through {
                        return Err(Error::field(Code::Corrupt, "catalog_index"));
                    }
                    let path = Path::from_member(&member).ok_or_else(|| Error::field(Code::Corrupt, "catalog_index"))?;
                    if self.subscription.covers(&path) {
                        paths.insert(path);
                    }
                    last = revision;
                }
                if count < self.client.config.scan_page_size {
                    break;
                }
            }
        }
        Ok(paths)
    }

    /// 用 ZSCAN 分页枚举 live 与 deleted 索引中全部被订阅覆盖的 Path。
    ///
    /// 每个 score 与 member 都重新校验，BTreeSet 去重并提供确定顺序。
    async fn collect_full_paths(&self) -> Result<BTreeSet<Path>> {
        let mut paths = BTreeSet::new();
        for key in [live_key(&self.client.config.zone), deleted_key(&self.client.config.zone)] {
            let scanner = self.client.driver().zscan(key, "*", Some(self.client.config.scan_page_size as u32));
            tokio::pin!(scanner);
            while let Some(page) = scanner.as_mut().next().await {
                let mut page = page.map_err(|error| Error::driver(Code::Unavailable, error))?;
                let values = page.take_results().ok_or_else(|| Error::field(Code::Corrupt, "catalog_index"))?;
                for (member, score) in values {
                    exact_score(score)?;
                    let member = value_string(member)?;
                    let path = Path::from_member(&member).ok_or_else(|| Error::field(Code::Corrupt, "catalog_index"))?;
                    if self.subscription.covers(&path) {
                        paths.insert(path);
                    }
                }
            }
        }
        Ok(paths)
    }

    /// 返回当前 Entry map 中仍被订阅覆盖的 Path 快照，不在后续 I/O 期间持锁。
    fn entry_paths(&self) -> BTreeSet<Path> {
        self.entries
            .read()
            .map(|entries| entries.keys().filter(|path| self.subscription.covers(path)).cloned().collect())
            .unwrap_or_default()
    }

    /// 在专用 Pub/Sub 连接上建立一个顺序栅栏。
    ///
    /// 随机 PING 响应证明 Redis 已处理此前命令；随后向读取任务发送 FenceRequest，
    /// 要求其排空本地消息队列。两步完成后，之前的通知才不会落在快照发布之后。
    async fn ping_fence(&self) -> Result<()> {
        let token = format!("catalog-fence-{}", fastrand::u64(..));
        let response: Value = self.client.command(self.pubsub.ping(Some(token.clone())), Code::Unavailable).await?;
        let response = match response {
            Value::Array(mut values) if values.len() == 2 => value_string(values.remove(1))?,
            value => value_string(value)?,
        };
        if response != token {
            return Err(Error::field(Code::Corrupt, "pubsub_fence"));
        }
        let (done, receiver) = oneshot::channel();
        self.fence.send(FenceRequest { done }).await.map_err(|_| Error::new(Code::Closed))?;
        receiver.await.map_err(|_| Error::new(Code::Closed))
    }
}

/// 用 CAS 把 `entry` 标记为 `status`，但永不覆盖终止 Closed。
fn mark_entry(entry: &Entry, status: Status) {
    loop {
        let current = entry.state();
        if current.status == Status::Closed || current.status == status {
            return;
        }
        if entry.compare_and_swap(&current, current.with_status(status)).is_some() {
            return;
        }
    }
}

/// 把公开 `Status` 编码为 AtomicU8 使用的稳定进程内值。
fn status_byte(status: Status) -> u8 {
    match status {
        Status::Synchronizing => 1,
        Status::Present => 2,
        Status::Absent => 3,
        Status::Deleted => 4,
        Status::Unavailable => 5,
        Status::Closed => 6,
    }
}

/// 把 AtomicU8 值解码为 `Status`；未知值保守回退为 Synchronizing。
fn byte_status(value: u8) -> Status {
    match value {
        2 => Status::Present,
        3 => Status::Absent,
        4 => Status::Deleted,
        5 => Status::Unavailable,
        6 => Status::Closed,
        _ => Status::Synchronizing,
    }
}

/// 向全部 `waiters` 发送同一克隆 `result` 并原地清空列表；接收端已离开时忽略发送失败。
fn notify_waiters(waiters: &mut Vec<oneshot::Sender<Result<()>>>, result: Result<()>) {
    for waiter in waiters.drain(..) {
        let _ = waiter.send(result.clone());
    }
}

/// 把 Redis ZSET 的 `score` 精确转换为安全正 revision。
///
/// NaN、无穷、非整数、零值或超过 `MAX_REVISION` 返回 Corrupt，避免 float64 静默舍入。
fn exact_score(score: f64) -> Result<u64> {
    if !score.is_finite() || score < 1.0 || score > MAX_REVISION as f64 || score.fract() != 0.0 {
        return Err(Error::field(Code::Corrupt, "catalog_index"));
    }
    Ok(score as u64)
}

// Read 回复的严格解码独立维护，但通过 include 保持在本模块的私有命名空间，
// 避免为纯实现拆分扩大任何类型或辅助函数的可见性。
include!("subscriber_read.rs");

#[cfg(test)]
#[path = "../../tests/internal/catalog/subscriber.rs"]
mod tests;
