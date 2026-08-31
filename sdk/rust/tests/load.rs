use std::error::Error as StdError;
use std::sync::Arc;
use std::time::{Duration, Instant};

use fred::prelude::*;
use fred::types::{InfoKind, Value};
use tokio::sync::{Mutex, Semaphore};
use tokio::task::JoinSet;
use verdandi::registration::{Client as RegistrationClient, Config as RegistrationConfig, Registration as TypedRegistration, RegistrationOptions};
use verdandi::registration::{Selector as TypedSelector, SelectorOptions};
use verdandi::{Client, Config, Fields};

type Registration = TypedRegistration<Fields, Fields>;
type Selector = TypedSelector<Fields, Fields>;

const REGISTRATIONS: usize = 500;

// Redis command statistics are endpoint-wide, so the three qualification
// profiles must not overlap when a caller selects the complete ignored suite.
static LOAD_PROFILE: Mutex<()> = Mutex::const_new(());

fn qualification_rounds() -> Result<usize, Box<dyn StdError>> {
    let Some(value) = std::env::var("VERDANDI_LOAD_SECONDS").ok() else {
        return Ok(10);
    };
    let seconds = value.parse::<usize>()?;
    if !(1..=3600).contains(&seconds) {
        return Err(format!("VERDANDI_LOAD_SECONDS={value:?}, want 1..3600").into());
    }
    Ok(seconds)
}

fn qualification_fanout() -> Result<usize, Box<dyn StdError>> {
    let Some(value) = std::env::var("VERDANDI_SELECTOR_FANOUT").ok() else {
        return Ok(1);
    };
    let fanout = value.parse::<usize>()?;
    if !(1..=64).contains(&fanout) {
        return Err(format!("VERDANDI_SELECTOR_FANOUT={value:?}, want 1..64").into());
    }
    Ok(fanout)
}

fn qualification_scale() -> Result<usize, Box<dyn StdError>> {
    let Some(value) = std::env::var("VERDANDI_SCALE_REGISTRATIONS").ok() else {
        return Ok(5_000);
    };
    let count = value.parse::<usize>()?;
    if !(1..=100_000).contains(&count) {
        return Err(format!("VERDANDI_SCALE_REGISTRATIONS={value:?}, want 1..100000").into());
    }
    Ok(count)
}

#[tokio::test(flavor = "multi_thread", worker_threads = 8)]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn registration_selector_qualification_load() -> Result<(), Box<dyn StdError>> {
    let _profile = LOAD_PROFILE.lock().await;
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let rounds = qualification_rounds()?;
    let fanout = qualification_fanout()?;
    let zone = unique_zone()?;
    let redis = direct_client(&endpoint).await?;
    let mut config = Config::new(&endpoint);
    config.timeout = Duration::from_secs(5);
    let transport = Client::open(config).await?;
    let registration_config = RegistrationConfig {
        zone: zone.clone(),
        selector_page_size: 64,
        selector_event_buffer: 8192,
        selector_publish_interval: Duration::from_millis(1),
        clock_refresh: Duration::from_secs(1),
        ..RegistrationConfig::new(&zone)
    };
    let client = RegistrationClient::open(&transport, registration_config).await?;
    let task_baseline = tokio::runtime::Handle::current().metrics().num_alive_tasks();

    let register_started = Instant::now();
    let (registrations, register_latencies) = register_many(
        &client,
        REGISTRATIONS,
        "proxy",
        Duration::from_secs(6 * 60 * 60),
        Duration::from_secs(2 * 60 * 60),
    )
    .await?;
    let register_elapsed = register_started.elapsed();

    let selector_started = Instant::now();
    let selector = Selector::new(&client, SelectorOptions { type_name: "proxy".to_owned() }).await?;
    let selector_elapsed = selector_started.elapsed();
    let initial = selector.snapshot().await?;
    if !initial.synchronized || initial.candidates.len() != REGISTRATIONS {
        return Err(format!("initial snapshot synchronized={} records={}", initial.synchronized, initial.candidates.len()).into());
    }
    let mut selectors = Vec::with_capacity(fanout);
    selectors.push(selector);
    while selectors.len() < fanout {
        selectors.push(Selector::new(&client, SelectorOptions { type_name: "proxy".to_owned() }).await?);
    }
    let active_tasks = tokio::runtime::Handle::current().metrics().num_alive_tasks();
    let minimum_tasks = task_baseline + REGISTRATIONS;
    let maximum_tasks = minimum_tasks + fanout * 16 + 64;
    if active_tasks < minimum_tasks || active_tasks > maximum_tasks {
        return Err(format!(
            "unexpected one-task-per-Registration topology: baseline={task_baseline} active={active_tasks} expected={minimum_tasks}..={maximum_tasks}"
        )
        .into());
    }

    reset_stats(&redis).await?;
    let (update_latencies, schedule_lags, updates_elapsed) = update_at_cadence(&registrations, rounds).await?;
    for selector in &selectors {
        wait_snapshot(selector, REGISTRATIONS, u64::try_from(rounds + 1)?).await?;
    }
    let (evalsha_calls, evalsha_microseconds) = command_stat(&redis, "cmdstat_evalsha").await?;
    let key_memory = key_memory(&redis, &zone, &registrations).await?;

    let close_started = Instant::now();
    close_many(&registrations).await?;
    let close_elapsed = close_started.elapsed();
    for selector in &selectors {
        wait_snapshot(selector, 0, 0).await?;
        selector.close().await?;
    }
    client.close().await?;
    transport.close().await?;
    let final_tasks = wait_task_ceiling(task_baseline + 8, Duration::from_secs(5)).await;
    if final_tasks > task_baseline + 8 {
        return Err(format!("runtime tasks did not return near baseline: baseline={task_baseline} final={final_tasks}").into());
    }
    let _: i64 = redis.del(format!("verdandi:config:{zone}")).await?;
    redis.quit().await?;

    let register_percentiles = latency_summary(&register_latencies);
    let update_percentiles = latency_summary(&update_latencies);
    let lag_percentiles = latency_summary(&schedule_lags);
    println!(
        "register count={REGISTRATIONS} elapsed={register_elapsed:?} rate={:.1}/s p50={:?} p95={:?} p99={:?} max={:?}",
        REGISTRATIONS as f64 / register_elapsed.as_secs_f64(),
        register_percentiles.0,
        register_percentiles.1,
        register_percentiles.2,
        register_percentiles.3
    );
    println!("selector initial HSCAN records={REGISTRATIONS} page_size=64 first_elapsed={selector_elapsed:?} subscriber_fanout={fanout}");
    println!(
        concat!(
            "update count={} cadence={}/s elapsed={:?} completed_rate={:.1}/s ",
            "operation_p50={:?} operation_p95={:?} operation_p99={:?} operation_max={:?} ",
            "schedule_lag_p50={:?} schedule_lag_p95={:?} schedule_lag_p99={:?} schedule_lag_max={:?}"
        ),
        update_latencies.len(),
        REGISTRATIONS,
        updates_elapsed,
        update_latencies.len() as f64 / updates_elapsed.as_secs_f64(),
        update_percentiles.0,
        update_percentiles.1,
        update_percentiles.2,
        update_percentiles.3,
        lag_percentiles.0,
        lag_percentiles.1,
        lag_percentiles.2,
        lag_percentiles.3
    );
    println!(
        "Redis EVALSHA calls={evalsha_calls} total={evalsha_microseconds}us average={:.2}us",
        evalsha_microseconds as f64 / evalsha_calls as f64
    );
    println!(
        "graceful unregister count={REGISTRATIONS} elapsed={close_elapsed:?} rate={:.1}/s",
        REGISTRATIONS as f64 / close_elapsed.as_secs_f64()
    );
    println!("Redis MEMORY USAGE for config, Registry, and 500 Registration keys: {key_memory} bytes");
    println!("Tokio tasks one-per-Registration: baseline={task_baseline} active={active_tasks} final={final_tasks}");
    let completed_rate = update_latencies.len() as f64 / updates_elapsed.as_secs_f64();
    if completed_rate < REGISTRATIONS as f64 * 0.98 {
        return Err(format!("sustained update completion rate {completed_rate:.1}/s fell below 98% of offered 500/s").into());
    }
    if update_percentiles.2 > Duration::from_secs(1) || lag_percentiles.2 > Duration::from_secs(1) {
        return Err(format!(
            "sustained update p99 exceeded one second: operation={:?} schedule_lag={:?}",
            update_percentiles.2, lag_percentiles.2
        )
        .into());
    }
    Ok(())
}

async fn wait_task_ceiling(ceiling: usize, timeout: Duration) -> usize {
    let deadline = Instant::now() + timeout;
    loop {
        let tasks = tokio::runtime::Handle::current().metrics().num_alive_tasks();
        if tasks <= ceiling || Instant::now() >= deadline {
            return tasks;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 8)]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn registration_selector_renewal_load() -> Result<(), Box<dyn StdError>> {
    let _profile = LOAD_PROFILE.lock().await;
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let seconds = qualification_rounds()?.max(5);
    let zone = unique_zone()?;
    let redis = direct_client(&endpoint).await?;
    let mut config = Config::new(&endpoint);
    config.timeout = Duration::from_secs(5);
    let transport = Client::open(config).await?;
    let registration_config = RegistrationConfig {
        zone: zone.clone(),
        selector_page_size: 64,
        selector_event_buffer: 8192,
        selector_publish_interval: Duration::from_millis(1),
        clock_refresh: Duration::from_secs(1),
        ..RegistrationConfig::new(&zone)
    };
    let client = RegistrationClient::open(&transport, registration_config).await?;
    let (registrations, _) = register_many(&client, REGISTRATIONS, "renew", Duration::from_secs(3), Duration::from_secs(1)).await?;
    let mut errors: Vec<_> = registrations.iter().filter_map(|registration| registration.subscribe_errors()).collect();
    if errors.len() != registrations.len() {
        return Err("published Registration omitted its diagnostic receiver".into());
    }
    let selector = Selector::new(&client, SelectorOptions { type_name: "renew".to_owned() }).await?;
    wait_snapshot(&selector, REGISTRATIONS, 1).await?;
    reset_stats(&redis).await?;
    let started = Instant::now();
    tokio::time::sleep(Duration::from_secs(u64::try_from(seconds)?)).await;
    let elapsed = started.elapsed();
    wait_snapshot(&selector, REGISTRATIONS, 1).await?;
    let (evalsha_calls, evalsha_microseconds) = command_stat(&redis, "cmdstat_evalsha").await?;
    let rate = evalsha_calls as f64 / elapsed.as_secs_f64();
    if rate < REGISTRATIONS as f64 * 0.8 || rate > REGISTRATIONS as f64 * 1.2 {
        return Err(format!("automatic renewal rate {rate:.1}/s is outside 80%..120% of 500/s").into());
    }
    for receiver in &mut errors {
        match receiver.try_recv() {
            Err(tokio::sync::broadcast::error::TryRecvError::Empty) => {}
            Err(error) => return Err(format!("renewal diagnostic channel: {error}").into()),
            Ok(error) => return Err(format!("automatic renewal error: {error}").into()),
        }
    }
    println!(
        "renew live={REGISTRATIONS} duration={elapsed:?} calls={evalsha_calls} rate={rate:.1}/s Redis_EVALSHA_average={:.2}us",
        evalsha_microseconds as f64 / evalsha_calls as f64
    );

    close_many(&registrations).await?;
    wait_snapshot(&selector, 0, 0).await?;
    selector.close().await?;
    client.close().await?;
    transport.close().await?;
    let _: i64 = redis.del(format!("verdandi:config:{zone}")).await?;
    redis.quit().await?;
    Ok(())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 8)]
#[ignore = "requires an isolated Redis 8 endpoint in VERDANDI_REDIS_URL"]
async fn registration_selector_scale_recovery() -> Result<(), Box<dyn StdError>> {
    let _profile = LOAD_PROFILE.lock().await;
    let endpoint = std::env::var("VERDANDI_REDIS_URL")?;
    let count = qualification_scale()?;
    let zone = unique_zone()?;
    let redis = direct_client(&endpoint).await?;
    let mut config = Config::new(&endpoint);
    config.timeout = Duration::from_secs(5);
    let transport = Client::open(config).await?;
    let registration_config = RegistrationConfig {
        zone: zone.clone(),
        selector_page_size: 64,
        selector_event_buffer: 65_536,
        selector_publish_interval: Duration::from_millis(1),
        clock_refresh: Duration::from_secs(1),
        ..RegistrationConfig::new(&zone)
    };
    let client = RegistrationClient::open(&transport, registration_config).await?;

    let register_started = Instant::now();
    let (registrations, _) = register_many(&client, count, "scale", Duration::from_secs(6 * 60 * 60), Duration::from_secs(2 * 60 * 60)).await?;
    let register_elapsed = register_started.elapsed();
    let select_started = Instant::now();
    let selector = Selector::new(&client, SelectorOptions { type_name: "scale".to_owned() }).await?;
    wait_snapshot(&selector, count, 1).await?;
    let select_elapsed = select_started.elapsed();

    let close_started = Instant::now();
    close_many(&registrations).await?;
    wait_snapshot(&selector, 0, 0).await?;
    let close_elapsed = close_started.elapsed();
    selector.close().await?;
    client.close().await?;
    transport.close().await?;
    let _: i64 = redis.del(format!("verdandi:config:{zone}")).await?;
    redis.quit().await?;
    println!(
        concat!(
            "scale recovery registrations={} register_elapsed={:?} register_rate={:.1}/s HSCAN_page_size=64 ",
            "sync_elapsed={:?} unregister_elapsed={:?} unregister_rate={:.1}/s"
        ),
        count,
        register_elapsed,
        count as f64 / register_elapsed.as_secs_f64(),
        select_elapsed,
        close_elapsed,
        count as f64 / close_elapsed.as_secs_f64(),
    );
    Ok(())
}

async fn register_many(
    client: &RegistrationClient,
    count: usize,
    type_name: &str,
    ttl: Duration,
    renew_interval: Duration,
) -> Result<(Vec<Arc<Registration>>, Vec<Duration>), Box<dyn StdError>> {
    let semaphore = Arc::new(Semaphore::new(64));
    let mut tasks = JoinSet::new();
    for index in 0..count {
        let permit = Arc::clone(&semaphore).acquire_owned().await?;
        let client = client.clone();
        let type_name = type_name.to_owned();
        tasks.spawn(async move {
            let started = Instant::now();
            let registration = Registration::new(
                &client,
                RegistrationOptions {
                    type_name,
                    ttl,
                    renew_interval: Some(renew_interval),
                    version: 1,
                },
            );
            let registration = match registration {
                Ok(registration) => {
                    let attr = fields([("role", b"worker".as_slice())]);
                    let data = fields([("load", b"0".as_slice())]);
                    registration.register(&attr, &data).await.map(|()| registration)
                }
                Err(error) => Err(error),
            };
            drop(permit);
            (index, registration, started.elapsed())
        });
    }
    let mut registrations = vec![None; count];
    let mut latencies = vec![Duration::ZERO; count];
    while let Some(result) = tasks.join_next().await {
        let (index, registration, latency) = result?;
        registrations[index] = Some(Arc::new(registration?));
        latencies[index] = latency;
    }
    let registrations = registrations.into_iter().collect::<Option<Vec<_>>>().ok_or("missing Registration result")?;
    Ok((registrations, latencies))
}

async fn update_at_cadence(registrations: &[Arc<Registration>], rounds: usize) -> Result<(Vec<Duration>, Vec<Duration>, Duration), Box<dyn StdError>> {
    let mut tasks = JoinSet::new();
    let start = Instant::now() + Duration::from_millis(100);
    for (index, registration) in registrations.iter().enumerate() {
        let registration = Arc::clone(registration);
        tasks.spawn(async move {
            let mut values = Vec::with_capacity(rounds);
            for round in 0..rounds {
                let offset = Duration::from_secs(u64::try_from(round).unwrap_or(u64::MAX))
                    + Duration::from_nanos(u64::try_from(index).unwrap_or(u64::MAX) * 1_000_000_000 / u64::try_from(REGISTRATIONS).unwrap_or(1));
                let target = start + offset;
                tokio::time::sleep_until(tokio::time::Instant::from_std(target)).await;
                let started = Instant::now();
                let lag = started.saturating_duration_since(target);
                let data = fields([("load", (round + 1).to_string().as_bytes())]);
                registration.update(&data).await?;
                values.push((started.elapsed(), lag));
            }
            Ok::<_, verdandi::Error>((index, values))
        });
    }
    let mut latencies = vec![Duration::ZERO; registrations.len() * rounds];
    let mut lags = vec![Duration::ZERO; registrations.len() * rounds];
    while let Some(result) = tasks.join_next().await {
        let (index, values) = result??;
        for (round, (latency, lag)) in values.into_iter().enumerate() {
            let slot = round * registrations.len() + index;
            latencies[slot] = latency;
            lags[slot] = lag;
        }
    }
    Ok((latencies, lags, start.elapsed()))
}

async fn close_many(registrations: &[Arc<Registration>]) -> Result<(), Box<dyn StdError>> {
    let semaphore = Arc::new(Semaphore::new(64));
    let mut tasks = JoinSet::new();
    for registration in registrations {
        let permit = Arc::clone(&semaphore).acquire_owned().await?;
        let registration = Arc::clone(registration);
        tasks.spawn(async move {
            let result = registration.close().await;
            drop(permit);
            result
        });
    }
    while let Some(result) = tasks.join_next().await {
        result??;
    }
    Ok(())
}

async fn wait_snapshot(selector: &Selector, count: usize, revision: u64) -> Result<(), Box<dyn StdError>> {
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        let snapshot = selector.snapshot().await;
        if snapshot.as_ref().is_ok_and(|snapshot| {
            snapshot.candidates.len() == count && snapshot.candidates.iter().all(|candidate| revision == 0 || candidate.meta.revision == revision)
        }) {
            return Ok(());
        }
        if Instant::now() >= deadline {
            let records = snapshot.as_ref().map_or(0, |snapshot| snapshot.candidates.len());
            return Err(format!("snapshot did not converge: records={} expected={count} revision={revision}", records).into());
        }
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
}

async fn direct_client(endpoint: &str) -> Result<fred::clients::Client, fred::error::Error> {
    let config = fred::types::config::Config::from_url(endpoint)?;
    let client = Builder::from_config(config).build()?;
    client.init().await?;
    Ok(client)
}

async fn reset_stats(redis: &fred::clients::Client) -> Result<(), fred::error::Error> {
    let _: Value = redis.custom(fred::cmd!("CONFIG"), vec![Value::from("RESETSTAT")]).await?;
    Ok(())
}

async fn command_stat(redis: &fred::clients::Client, command: &str) -> Result<(i64, i64), Box<dyn StdError>> {
    let info: String = redis.info(Some(InfoKind::CommandStats)).await?;
    let prefix = format!("{command}:calls=");
    for line in info.lines() {
        let Some(values) = line.trim().strip_prefix(&prefix) else {
            continue;
        };
        let mut fields = values.split(',');
        let calls = fields.next().ok_or("missing calls")?.parse()?;
        let microseconds = fields.next().and_then(|value| value.strip_prefix("usec=")).ok_or("missing usec")?.parse()?;
        return Ok((calls, microseconds));
    }
    Err(format!("INFO commandstats omitted {command}").into())
}

async fn key_memory(redis: &fred::clients::Client, zone: &str, registrations: &[Arc<Registration>]) -> Result<i64, Box<dyn StdError>> {
    let mut keys = vec![format!("verdandi:config:{zone}"), format!("verdandi:registry:{zone}:proxy")];
    keys.extend(
        registrations
            .iter()
            .map(|registration| format!("verdandi:registration:{zone}:proxy:{}", registration.uuid())),
    );
    let mut total = 0_i64;
    for key in keys {
        let bytes: Option<i64> = redis.custom(fred::cmd!("MEMORY"), vec![Value::from("USAGE"), Value::from(key)]).await?;
        total += bytes.ok_or("MEMORY USAGE omitted a live key")?;
    }
    Ok(total)
}

fn fields<const N: usize>(values: [(&str, &[u8]); N]) -> Fields {
    values.into_iter().map(|(name, value)| (name.to_owned(), value.to_vec())).collect()
}

fn unique_zone() -> Result<String, String> {
    let mut random = [0_u8; 10];
    getrandom::fill(&mut random).map_err(|error| format!("random Zone: {error}"))?;
    let mut zone = String::from("RustLoad");
    for value in random {
        zone.push(char::from(b'a' + value % 26));
    }
    Ok(zone)
}

fn latency_summary(values: &[Duration]) -> (Duration, Duration, Duration, Duration) {
    let mut ordered = values.to_vec();
    ordered.sort_unstable();
    let percentile = |value: usize| {
        let index = (ordered.len() * value).div_ceil(100).saturating_sub(1);
        ordered[index]
    };
    (percentile(50), percentile(95), percentile(99), ordered[ordered.len() - 1])
}
