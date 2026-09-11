//! 共用的推流调度器. 一个发布源更新内存状态后广播, 每个接收端独立有界排队.
use crate::{
    generated::{Completed, Control, Domain, Event, Update, control::Command, event::Event as Kind},
    push_state::{Histogram, Shape},
};
use prost::bytes::Bytes;
use std::{
    collections::{HashMap, VecDeque},
    io,
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};
use tokio::{
    sync::{Notify, broadcast, mpsc},
    task::JoinSet,
    time::{Instant, interval, timeout},
};
use tokio_util::sync::CancellationToken;

#[derive(Clone, Copy)]
pub struct Config {
    pub recipients: usize,
    pub rate: u64,
    pub seconds: f64,
    pub burst: bool,
    pub shape: Shape,
    pub mode: Mode,
}

#[derive(Clone, Copy, PartialEq)]
pub enum Mode {
    Mixed,
    Registry,
    Catalog,
}

impl Default for Config {
    fn default() -> Self {
        Self { recipients: 1, rate: 2000, seconds: 3.0, burst: false, shape: Shape::default(), mode: Mode::Mixed }
    }
}

#[derive(Clone)]
struct Queued {
    event: Event,
    created: Instant,
}

struct Store {
    revision: u64,
    values: HashMap<String, (u64, Bytes)>,
    stamps: Vec<Option<(u64, Instant)>>,
    finished: bool,
}

struct Model {
    config: Config,
    store: Mutex<Store>,
    updates: broadcast::Sender<Queued>,
    joined: AtomicUsize,
    ready: Notify,
    stop: CancellationToken,
}

/// 调度任务由 Hub 拥有, 任务只捕获 Model, 避免 Arc 自引用导致无法回收.
pub struct Hub {
    model: Arc<Model>,
    _tasks: JoinSet<()>,
}

impl Hub {
    pub fn new(config: Config, stop: CancellationToken) -> io::Result<Arc<Self>> {
        config.shape.validate()?;
        if !(1..=64).contains(&config.recipients) || !(1..=200000).contains(&config.rate) || !(0.2..=30.0).contains(&config.seconds) {
            return Err(io::Error::other("invalid push configuration"));
        }
        let (updates, _) = broadcast::channel(512);
        let model = Arc::new(Model {
            config,
            store: Mutex::new(Store { revision: 0, values: config.shape.state(), stamps: vec![None; 16384], finished: false }),
            updates,
            joined: AtomicUsize::new(0),
            ready: Notify::new(),
            stop: stop.child_token(),
        });
        let mut tasks = JoinSet::new();
        let source = model.clone();
        tasks.spawn(async move {
            if produce(&source).await.is_err() {
                source.stop.cancel();
            }
        });
        Ok(Arc::new(Self { model, _tasks: tasks }))
    }

    pub async fn session(&self, mut input: mpsc::Receiver<io::Result<Control>>, output: mpsc::Sender<io::Result<Event>>) -> io::Result<()> {
        let first = timeout(Duration::from_secs(5), input.recv()).await?.ok_or_else(closed)??;
        if first.command != Some(Command::Join(true)) {
            return Err(io::Error::other("join required"));
        }
        // 必须先订阅广播再登记就绪. 最后一位加入者触发发布源, 不丢失第一条更新.
        let mut updates = self.model.updates.subscribe();
        let joined = self.model.joined.fetch_add(1, Ordering::AcqRel) + 1;
        if joined > self.model.config.recipients {
            return Err(io::Error::other("recipient limit"));
        }
        if joined == self.model.config.recipients {
            self.model.ready.notify_one();
        }
        let mut controls = VecDeque::with_capacity(8);
        let mut data: Option<Queued> = None;
        let mut watermark = None;
        let mut deadline = Instant::now() + Duration::from_secs(60);
        let mut latency = Histogram::default();
        let mut expired = 0;
        let mut last_progress = 0;
        loop {
            tokio::select! {
                biased;
                () = self.model.stop.cancelled() => return Err(closed()),
                () = output.closed() => return Err(closed()),
                () = tokio::time::sleep_until(deadline) => return Err(io::Error::other("final progress timeout")),
                command = input.recv(), if controls.len() < 8 => {
                    let command = command.ok_or_else(closed)??;
                    let kind = match command.command {
                        Some(Command::Ping(nonce)) => Kind::Pong(nonce),
                        Some(Command::Publish(forward)) if forward.request > 0 && forward.data.len() <= 1024 => {
                            if apply(&self.model, Domain::Catalog, forward.data, Instant::now())? { Kind::Accepted(forward.request) } else { Kind::Rejected(forward.request) }
                        }
                        Some(Command::Progress(progress)) => {
                            if progress.r#final {
                                if watermark != Some(progress.revision) { return Err(io::Error::other("invalid final progress")); }
                                Kind::Completed(Completed {
                                    revision: progress.revision, sampled_updates: latency.count(),
                                    update_p50_micros: latency.percentile(0.50), update_p95_micros: latency.percentile(0.95),
                                    update_p99_micros: latency.percentile(0.99), expired_samples: expired,
                                })
                            } else {
                                let store = self.model.store.lock().map_err(|_| closed())?;
                                if progress.revision <= last_progress || progress.revision > store.revision { return Err(io::Error::other("invalid progress revision")); }
                                last_progress = progress.revision;
                                match store.stamps[progress.revision as usize % store.stamps.len()] {
                                    Some((revision, stamp)) if revision == progress.revision => latency.record(stamp.elapsed().as_micros() as u64),
                                    _ => expired += 1,
                                }
                                // 累计水位只用于抽样, 不逐条产生反向应答, 保留推流占主导的流量结构.
                                continue;
                            }
                        }
                        _ => return Err(io::Error::other("invalid push control")),
                    };
                    controls.push_back(Queued { event: Event { queue_micros: 0, event: Some(kind) }, created: Instant::now() });
                }
                permit = output.reserve(), if !controls.is_empty() || data.is_some() => {
                    let mut next = controls.pop_front().or_else(|| data.take()).ok_or_else(closed)?;
                    let completed = matches!(next.event.event, Some(Kind::Completed(_)));
                    if let Some(Kind::Drained(revision)) = next.event.event {
                        watermark = Some(revision);
                        deadline = Instant::now() + Duration::from_secs(5);
                    }
                    next.event.queue_micros = next.created.elapsed().as_micros().min(u128::from(u64::MAX)) as u64;
                    permit.map_err(|_| closed())?.send(Ok(next.event));
                    if completed { return Ok(()); }
                }
                update = updates.recv(), if data.is_none() => {
                    // 慢端超过共享环容量必须显式失败, 不允许悄悄跳过版本继续宣称同步成功.
                    data = Some(update.map_err(|_| io::Error::other("push receiver lagged; resynchronization required"))?);
                }
            }
        }
    }
}

fn apply(model: &Model, domain: Domain, data: Bytes, scheduled: Instant) -> io::Result<bool> {
    let mut store = model.store.lock().map_err(|_| io::Error::other("store poisoned"))?;
    if store.finished {
        return Ok(false);
    }
    store.revision += 1;
    let revision = store.revision;
    let key = model.config.shape.key(domain, revision);
    *store.values.get_mut(&key).ok_or_else(closed)? = (revision, data.clone());
    let stamp_index = revision as usize % store.stamps.len();
    store.stamps[stamp_index] = Some((revision, scheduled));
    // 版本递增和广播同处一个短锁域. 各接收端共享 Bytes, 不复制每个接收端的完整业务数据.
    publish(model, Kind::Update(Update { revision, key, data, domain: domain as i32 }));
    Ok(true)
}

fn publish(model: &Model, event: Kind) {
    let _ = model.updates.send(Queued { event: Event { queue_micros: 0, event: Some(event) }, created: Instant::now() });
}

async fn produce(model: &Model) -> io::Result<()> {
    tokio::select! {
        () = model.stop.cancelled() => return Ok(()),
        ready = timeout(Duration::from_secs(8), model.ready.notified()) => { ready?; }
    }
    publish(model, Kind::Ready(true));
    let start = Instant::now();
    let target = (model.config.rate as f64 * model.config.seconds) as u64;
    let mut count = 0;
    let mut snapshots = 0;
    let mut tick = interval(Duration::from_millis(if model.config.burst { 50 } else { 1 }));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    while count < target {
        tokio::select! { () = model.stop.cancelled() => return Ok(()), _ = tick.tick() => {} }
        let due = ((start.elapsed().as_secs_f64() * model.config.rate as f64) as u64).min(target);
        while count < due {
            let domain = match model.config.mode {
                Mode::Registry => Domain::Registry,
                Mode::Catalog => Domain::Catalog,
                Mode::Mixed => {
                    if count % 4 == 0 {
                        Domain::Registry
                    } else {
                        Domain::Catalog
                    }
                }
            };
            let size = if domain == Domain::Registry { 128 } else { model.config.shape.bytes };
            // 每条消息有新内容与字符串键. 计划发布时间参与确认延迟, 发压器落后也不能隐藏尾部.
            let scheduled = start + Duration::from_secs_f64((count + 1) as f64 / model.config.rate as f64);
            let mut bytes = vec![0x5a; size];
            bytes[..8].copy_from_slice(&count.to_le_bytes());
            apply(model, domain, Bytes::from(bytes), scheduled)?;
            count += 1;
            if count % 64 == 0 {
                tokio::task::yield_now().await;
            }
        }
        if model.config.mode == Mode::Mixed && start.elapsed().as_millis() / 500 > snapshots {
            snapshots += 1;
            // 少量 16 KiB 快照片段穿插更新. 这里只验证传输调度, 不模拟完整快照恢复状态机.
            for _ in 0..4 {
                publish(model, Kind::Snapshot(Bytes::from(vec![0x3c; 16384])));
            }
        }
    }
    let mut store = model.store.lock().map_err(|_| io::Error::other("store poisoned"))?;
    store.finished = true;
    publish(model, Kind::Drained(store.revision));
    Ok(())
}

fn closed() -> io::Error {
    io::Error::other("push session closed")
}

#[cfg(test)]
#[path = "../tests/push.rs"]
mod tests;
