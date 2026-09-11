//! 推流接收与稀疏累计水位. 更新延迟使用发布端单调时钟, 不相减两端时钟.
use crate::{
    auth::Auth,
    generated::{Completed, Control, Domain, Forward, Progress, control::Command, event::Event as Kind},
    push_io,
    push_state::Shape,
};
use prost::bytes::Bytes;
use serde_json::{Value, json};
use std::{
    collections::BTreeMap,
    io,
    net::SocketAddr,
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::{
    task::JoinSet,
    time::{Instant, sleep, sleep_until, timeout},
};
use tokio_util::sync::CancellationToken;

#[derive(Clone, Copy)]
pub struct Load {
    pub address: SocketAddr,
    pub grpc: bool,
    pub fanout: usize,
    pub seconds: f64,
    pub pause_ms: u64,
    pub shape: Shape,
}

#[derive(Default)]
struct Pending {
    pings: BTreeMap<u64, (Instant, Instant)>,
    publishes: BTreeMap<u64, Instant>,
}

struct Samples {
    updates: u64,
    bytes: u64,
    snapshots: u64,
    elapsed: f64,
    control: Vec<u64>,
    scheduled: Vec<u64>,
    publish: Vec<u64>,
    queue: Vec<u64>,
    state_hash: String,
    cancelled_pings: usize,
    completion: Completed,
    progress_sent: u64,
    progress_skipped: u64,
    registries: u64,
    catalogs: u64,
}

pub async fn run(auth: Arc<Auth>, load: Load) -> io::Result<Value> {
    load.shape.validate()?;
    let mut tasks = JoinSet::new();
    for index in 0..load.fanout {
        let auth = auth.clone();
        tasks.spawn(async move { receiver(auth, load, index + 1 == load.fanout).await });
    }
    let mut samples = Vec::with_capacity(load.fanout);
    while let Some(sample) = tasks.join_next().await {
        samples.push(sample.map_err(io::Error::other)??);
    }
    let first = samples.first().ok_or_else(|| io::Error::other("no recipients"))?;
    if samples.iter().any(|s| s.state_hash != first.state_hash || s.updates != first.updates) {
        return Err(io::Error::other("recipient state mismatch"));
    }
    let elapsed = samples.iter().map(|s| s.elapsed).fold(0.0, f64::max);
    let updates: u64 = samples.iter().map(|s| s.updates).sum();
    let bytes: u64 = samples.iter().map(|s| s.bytes).sum();
    let control = merge(samples.iter().map(|s| &s.control));
    let scheduled = merge(samples.iter().map(|s| &s.scheduled));
    let publish = merge(samples.iter().map(|s| &s.publish));
    let queue = merge(samples.iter().map(|s| &s.queue));
    let mut report = json!({
        "workload": "shared_source_push", "transport": if load.grpc { "grpc" } else { "tcp" },
        "fanout": load.fanout, "pause_ms": load.pause_ms, "seconds": elapsed,
        "updates_per_recipient": first.updates, "completed": updates, "failed": 0,
        "messages_per_second": updates as f64 / elapsed, "effective_mib_per_second": bytes as f64 / elapsed / 1048576.0,
        "received_data_bytes": bytes, "snapshot_fragments": samples.iter().map(|s| s.snapshots).sum::<u64>(),
        "p50_ms": percentile(&control, 0.50), "p95_ms": percentile(&control, 0.95), "p99_ms": percentile(&control, 0.99),
        "scheduled_control_p99_ms": percentile(&scheduled, 0.99),
        "control_replies": control.iter().sum::<u64>(), "cancelled_pings_at_completion": samples.iter().map(|s| s.cancelled_pings).sum::<usize>(),
        "publish_replies": publish.iter().sum::<u64>(), "publish_p99_ms": percentile(&publish, 0.99),
        "dispatch_queue_p99_ms": percentile(&queue, 0.99), "state_sha256": first.state_hash,
        "per_update_ack": false, "latency_bucket_ms": 0.1,
    });
    let details = json!({
        "registry_entries": load.shape.registries, "catalog_entries": load.shape.catalogs, "catalog_bytes": load.shape.bytes,
        "registry_deliveries": samples.iter().map(|s| s.registries).sum::<u64>(),
        "catalog_deliveries": samples.iter().map(|s| s.catalogs).sum::<u64>(),
        "progress_sent": samples.iter().map(|s| s.progress_sent).sum::<u64>(),
        "progress_skipped": samples.iter().map(|s| s.progress_skipped).sum::<u64>(),
        "expired_latency_samples": samples.iter().map(|s| s.completion.expired_samples).sum::<u64>(),
        "sampled_updates": samples.iter().map(|s| s.completion.sampled_updates).sum::<u64>(),
        // 分位数不能直接求平均. 报告各接收端中最差分位数, 同时保留完整的逐端统计.
        "update_p50_ms": samples.iter().map(|s| s.completion.update_p50_micros).max().unwrap_or(0) as f64 / 1000.0,
        "update_p95_ms": samples.iter().map(|s| s.completion.update_p95_micros).max().unwrap_or(0) as f64 / 1000.0,
        "update_p99_ms": samples.iter().map(|s| s.completion.update_p99_micros).max().unwrap_or(0) as f64 / 1000.0,
        "recipient_update_latency": samples.iter().map(|s| json!({"samples":s.completion.sampled_updates,
            "p50_us":s.completion.update_p50_micros,"p95_us":s.completion.update_p95_micros,
            "p99_us":s.completion.update_p99_micros,"expired":s.completion.expired_samples})).collect::<Vec<_>>(),
        "final_backlog": 0,
    });
    report.as_object_mut().ok_or_else(closed)?.extend(details.as_object().ok_or_else(closed)?.clone());
    Ok(report)
}

async fn receiver(auth: Arc<Auth>, load: Load, slow: bool) -> io::Result<Samples> {
    let mut state = load.shape.state();
    let mut connection = push_io::connect(load.address, &auth, load.grpc).await?;
    connection.send.send(Control { command: Some(Command::Join(true)) }).await.map_err(io::Error::other)?;
    let ready = timeout(Duration::from_secs(10), connection.receive.recv()).await?.ok_or_else(closed)??;
    if ready.event != Some(Kind::Ready(true)) {
        return Err(io::Error::other("ready event required"));
    }
    let start = Instant::now();
    let stop = CancellationToken::new();
    let pending = Arc::new(Mutex::new(Pending::default()));
    let mut control_task = JoinSet::new();
    control_task.spawn(controls(connection.send.clone(), pending.clone(), stop.clone(), start, load.seconds));
    let mut samples = Samples {
        updates: 0,
        bytes: 0,
        snapshots: 0,
        elapsed: 0.0,
        control: vec![0; 100001],
        scheduled: vec![0; 100001],
        publish: vec![0; 100001],
        queue: vec![0; 100001],
        state_hash: String::new(),
        cancelled_pings: 0,
        completion: Completed::default(),
        progress_sent: 0,
        progress_skipped: 0,
        registries: 0,
        catalogs: 0,
    };
    let mut paused = false;
    let mut drained = false;
    loop {
        if slow && load.pause_ms > 0 && !paused && start.elapsed().as_secs_f64() >= load.seconds / 2.0 {
            paused = true;
            // 只暂停接收消费, 独立上行任务仍继续发控制消息, 避免把慢端延迟藏进发压器停顿.
            sleep(Duration::from_millis(load.pause_ms)).await;
        }
        let message = timeout(Duration::from_secs(10), connection.receive.recv()).await?.ok_or_else(closed)??;
        match message.event {
            Some(Kind::Update(update)) => {
                let domain = Domain::try_from(update.domain).map_err(io::Error::other)?;
                if drained
                    || domain == Domain::Unspecified
                    || update.revision != samples.updates + 1
                    || update.key != load.shape.key(domain, update.revision)
                    || update.data.len() > 1024
                {
                    return Err(io::Error::other("missing or invalid update revision"));
                }
                record(&mut samples.queue, message.queue_micros);
                samples.updates = update.revision;
                samples.bytes += update.data.len() as u64;
                *state.get_mut(&update.key).ok_or_else(closed)? = (update.revision, update.data);
                if domain == Domain::Registry {
                    samples.registries += 1;
                } else {
                    samples.catalogs += 1;
                }
                if update.revision % 128 == 0 {
                    let progress = Control { command: Some(Command::Progress(Progress { revision: update.revision, r#final: false })) };
                    match connection.send.try_send(progress) {
                        Ok(()) => samples.progress_sent += 1,
                        Err(tokio::sync::mpsc::error::TrySendError::Full(_)) => samples.progress_skipped += 1,
                        Err(error) => return Err(io::Error::other(error)),
                    }
                }
            }
            Some(Kind::Snapshot(bytes)) => {
                if bytes.len() != 16384 {
                    return Err(io::Error::other("invalid snapshot fragment"));
                }
                samples.bytes += bytes.len() as u64;
                samples.snapshots += 1;
                record(&mut samples.queue, message.queue_micros);
            }
            Some(Kind::Pong(id)) => {
                let (due, queued) = pending.lock().map_err(|_| closed())?.pings.remove(&id).ok_or_else(closed)?;
                record(&mut samples.control, queued.elapsed().as_micros() as u64);
                record(&mut samples.scheduled, due.elapsed().as_micros() as u64);
            }
            Some(Kind::Accepted(id)) => {
                let due = pending.lock().map_err(|_| closed())?.publishes.remove(&id).ok_or_else(closed)?;
                record(&mut samples.publish, due.elapsed().as_micros() as u64);
            }
            Some(Kind::Drained(revision)) if !drained && revision == samples.updates => {
                drained = true;
                samples.elapsed = start.elapsed().as_secs_f64();
                stop.cancel();
                while let Some(result) = control_task.join_next().await {
                    result.map_err(io::Error::other)??;
                }
                // 最后一次可靠水位与早先控制消息保持流内顺序, 不把尾部缺失样本当成成功.
                connection.send.send(Control { command: Some(Command::Progress(Progress { revision, r#final: true })) }).await.map_err(io::Error::other)?;
            }
            Some(Kind::Completed(completion)) if drained && completion.revision == samples.updates => {
                if completion.sampled_updates + completion.expired_samples != samples.progress_sent {
                    return Err(io::Error::other("missing latency samples"));
                }
                samples.completion = completion;
                break;
            }
            _ => return Err(io::Error::other("invalid push event or completion watermark")),
        }
    }
    stop.cancel();
    while let Some(result) = control_task.join_next().await {
        result.map_err(io::Error::other)??;
    }
    let pending = pending.lock().map_err(|_| closed())?;
    if !pending.publishes.is_empty() {
        return Err(io::Error::other("unacknowledged publication"));
    }
    samples.cancelled_pings = pending.pings.len();
    // 每个接收端独立维护同一内存状态, 完成后校验全表摘要, 不只比较字节计数.
    let mut hash = ring::digest::Context::new(&ring::digest::SHA256);
    let ordered: BTreeMap<_, _> = state.into_iter().collect();
    for (key, (revision, bytes)) in ordered {
        hash.update(&(key.len() as u64).to_le_bytes());
        hash.update(key.as_bytes());
        hash.update(&revision.to_le_bytes());
        hash.update(&(bytes.len() as u64).to_le_bytes());
        hash.update(&bytes);
    }
    samples.state_hash = hash.finish().as_ref().iter().map(|byte| format!("{byte:02x}")).collect();
    Ok(samples)
}

async fn controls(
    send: tokio::sync::mpsc::Sender<Control>,
    pending: Arc<Mutex<Pending>>,
    stop: CancellationToken,
    start: Instant,
    seconds: f64,
) -> io::Result<()> {
    let mut sequence = 1;
    loop {
        let due = start + Duration::from_millis(sequence * 50);
        if due.duration_since(start).as_secs_f64() >= seconds * 0.9 {
            stop.cancelled().await;
            return Ok(());
        }
        tokio::select! { biased; () = stop.cancelled() => return Ok(()), () = sleep_until(due) => {} }
        {
            let mut pending = pending.lock().map_err(|_| closed())?;
            if pending.pings.len() >= 512 {
                return Err(io::Error::other("control backlog limit"));
            }
            pending.pings.insert(sequence, (due, Instant::now()));
        }
        tokio::select! {
            biased;
            () = stop.cancelled() => return Ok(()),
            result = send.send(Control { command: Some(Command::Ping(sequence)) }) => { result.map_err(io::Error::other)?; }
        }
        // 发布请求只有 5/s, 并提前停止以便验证全部确认. 绝大多数业务消息仍是服务端主动推流.
        if sequence % 4 == 0 && due.duration_since(start).as_secs_f64() < seconds * 0.8 {
            pending.lock().map_err(|_| closed())?.publishes.insert(sequence, Instant::now());
            let command = Control { command: Some(Command::Publish(Forward { request: sequence, data: Bytes::from_static(&[0x6f; 128]) })) };
            tokio::select! {
                biased;
                () = stop.cancelled() => return Ok(()),
                result = send.send(command) => { result.map_err(io::Error::other)?; }
            }
        }
        sequence += 1;
    }
}

fn record(histogram: &mut [u64], micros: u64) {
    histogram[(micros / 100).min(100000) as usize] += 1;
}
fn merge<'a>(histograms: impl Iterator<Item = &'a Vec<u64>>) -> Vec<u64> {
    let mut total = vec![0; 100001];
    for histogram in histograms {
        for (sum, value) in total.iter_mut().zip(histogram) {
            *sum += value;
        }
    }
    total
}
fn percentile(histogram: &[u64], percentile: f64) -> f64 {
    let target = (histogram.iter().sum::<u64>() as f64 * percentile).ceil() as u64;
    if target == 0 {
        return 0.0;
    }
    let mut count = 0;
    for (bucket, value) in histogram.iter().enumerate() {
        count += value;
        if count >= target {
            return (bucket + 1) as f64 / 10.0;
        }
    }
    10000.1
}
fn closed() -> io::Error {
    io::Error::other("push stream closed or invalid control correlation")
}
