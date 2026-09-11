//! 有界负载发生器. 固定速率的延迟从计划发送时刻计起, 包含客户端排队时间.
use crate::{Transfer, auth::Auth, transport};
use prost::bytes::Bytes;
use serde_json::{Value, json};
use std::{collections::VecDeque, io, net::SocketAddr, sync::Arc, time::Duration};
use tokio::{
    sync::Barrier,
    task::JoinSet,
    time::{Instant, sleep_until, timeout},
};

pub struct Load {
    pub address: SocketAddr,
    pub grpc: bool,
    pub bytes: usize,
    pub fanout: usize,
    pub window: usize,
    pub seconds: f64,
    pub rate: u64,
}

#[derive(Default)]
struct Samples {
    completed: u64,
    sent: u64,
    elapsed: f64,
    cold_ms: f64,
    // 固定 100 us 分桶, 10 秒以上单独归入最后一桶. 不保存每条消息的无界样本.
    histogram: Vec<u64>,
}

pub async fn run(auth: Arc<Auth>, load: Load) -> io::Result<Value> {
    let load = Arc::new(load);
    let barrier = Arc::new(Barrier::new(load.fanout));
    let mut tasks = JoinSet::new();
    // 每个接收会话共享同一只读业务 payload, 编码工作由各传输按相同消息逐条执行.
    let body = Bytes::from(vec![0x5a; load.bytes]);
    for _ in 0..load.fanout {
        let (auth, load, barrier, body) = (auth.clone(), load.clone(), barrier.clone(), body.clone());
        tasks.spawn(async move {
            let cold = Instant::now();
            let mut session = transport::connect(load.address, &auth, load.grpc).await?;
            let cold_ms = cold.elapsed().as_secs_f64() * 1000.0;
            // 预热和建链不进入稳态统计. 相同流继续使用, 不重建 RPC 或 TLS.
            exercise(&mut session, &body, load.window, 0.25, 0).await?;
            timeout(Duration::from_secs(15), barrier.wait()).await?;
            let mut samples = exercise(&mut session, &body, load.window, load.seconds, load.rate / load.fanout as u64).await?;
            samples.cold_ms = cold_ms;
            Ok::<_, io::Error>(samples)
        });
    }
    let mut totals = Samples { histogram: vec![0; 100001], ..Samples::default() };
    let mut cold = Vec::with_capacity(load.fanout);
    while let Some(samples) = tasks.join_next().await {
        let samples = samples.map_err(io::Error::other)??;
        totals.sent += samples.sent;
        totals.completed += samples.completed;
        totals.elapsed = totals.elapsed.max(samples.elapsed);
        cold.push(samples.cold_ms);
        for (total, value) in totals.histogram.iter_mut().zip(samples.histogram) {
            *total += value;
        }
    }
    cold.sort_by(f64::total_cmp);
    Ok(json!({
        "transport": if load.grpc { "grpc" } else { "tcp" }, "bytes": load.bytes, "fanout": load.fanout,
        "window": load.window, "offered_rate": load.rate, "actual_offered_rate": load.rate / load.fanout as u64 * load.fanout as u64,
        "sent": totals.sent, "completed": totals.completed, "failed": totals.sent - totals.completed,
        "seconds": totals.elapsed, "messages_per_second": totals.completed as f64 / totals.elapsed,
        "effective_mib_per_second": totals.completed as f64 * load.bytes as f64 / totals.elapsed / 1048576.0,
        "p50_ms": percentile(&totals, 0.50), "p95_ms": percentile(&totals, 0.95), "p99_ms": percentile(&totals, 0.99),
        "latency_bucket_ms": 0.1, "cold_ms": cold,
        "mode": "bidirectional_echo", "runtime_workers": 2, "compression": false,
    }))
}

async fn exercise(session: &mut transport::Session, body: &Bytes, window: usize, seconds: f64, rate: u64) -> io::Result<Samples> {
    let start = Instant::now();
    let end = start + Duration::from_secs_f64(seconds);
    let deadline = end + Duration::from_secs(10);
    let mut pending = VecDeque::with_capacity(window);
    let mut samples = Samples { histogram: vec![0; 100001], ..Samples::default() };
    loop {
        // 固定负载不会因为响应慢就减少计划请求. 预算耗尽会让本轮失败, 不能隐瞒为低吞吐成功.
        let due = if rate == 0 { Instant::now() } else { start + Duration::from_secs_f64(samples.sent as f64 / rate as f64) };
        let planned = due < end;
        if !planned && pending.is_empty() {
            break;
        }
        let can_send = planned && pending.len() < window;
        let ready = can_send && Instant::now() >= due;
        tokio::select! {
            biased;
            reply = session.receive.recv(), if !pending.is_empty() => {
                let reply = reply.ok_or_else(|| io::Error::other("response stream closed"))??;
                let (sequence, due): (u64, Instant) = pending.pop_front().ok_or_else(|| io::Error::other("unexpected response"))?;
                if reply.sequence != sequence || reply.body != *body { return Err(io::Error::other("response data mismatch")); }
                let bucket = (due.elapsed().as_micros() / 100).min(100000) as usize;
                samples.histogram[bucket] += 1;
                samples.completed += 1;
            }
            permit = session.send.reserve(), if ready => {
                permit.map_err(io::Error::other)?.send(Transfer { sequence: samples.sent, body: body.clone() });
                pending.push_back((samples.sent, due));
                samples.sent += 1;
            }
            () = sleep_until(due), if can_send && !ready => {}
            () = sleep_until(deadline) => return Err(io::Error::other("measurement drain timeout")),
        }
    }
    samples.elapsed = start.elapsed().as_secs_f64();
    Ok(samples)
}

fn percentile(samples: &Samples, fraction: f64) -> f64 {
    let target = (samples.completed as f64 * fraction).ceil() as u64;
    let mut count = 0;
    for (bucket, value) in samples.histogram.iter().enumerate() {
        count += value;
        if count >= target {
            return (bucket + 1) as f64 / 10.0;
        }
    }
    10000.1
}
