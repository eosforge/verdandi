use std::future::Future;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use arc_swap::ArcSwap;
use fred::interfaces::FredResult;
use fred::prelude::*;
use fred::types::Value;
use tokio::sync::{Notify, broadcast};
use tokio_util::sync::CancellationToken;

use crate::client::Client as BaseClient;
use crate::error::{Code, Error, Result};
use crate::lifecycle::{Activity, Guard as LifecycleGuard};

use super::config::{Config, RegistrationLimits, RuntimeConfig, ZONE_CONFIG_FIELDS, ZoneConfig, parse_zone_config};
use super::script::RegistrationScripts;

/// 附着到一个共享 Verdandi 根 Client 的 Registration 领域状态。
pub struct Client {
    pub(crate) inner: Arc<ClientInner>,
}

pub(crate) struct ClientInner {
    base: BaseClient,
    pub config: RuntimeConfig,
    pub scripts: RegistrationScripts,
    pub limits: ArcSwap<ZoneConfig>,
    pub shutdown: CancellationToken,
    errors: broadcast::Sender<Error>,
    configuration_refresh: Mutex<ConfigurationRefreshState>,
    activity: Arc<Activity>,
}

pub(crate) type ActiveGuard = LifecycleGuard;

struct ConfigurationRefreshState {
    users: usize,
    worker: Option<Arc<ConfigurationRefreshWorker>>,
}

struct ConfigurationRefreshWorker {
    cancel: CancellationToken,
    finished: AtomicBool,
    done: Notify,
}

pub(crate) struct ConfigurationRefreshLease {
    owner: Arc<ClientInner>,
    worker: Arc<ConfigurationRefreshWorker>,
    released: bool,
}

impl Client {
    /// 把 Registration 策略、Lua 与同步设置附着到现有根 `base` Client。
    ///
    /// `config` 提供独立 Zone 和本地资源策略。构造会验证 Redis 8、补齐并读取 Zone 默认策略、
    /// 预加载脚本，但不会启动常驻任务；配置刷新仅在 Registration/Selector 实际存在时惰性启动。
    pub async fn open(base: &BaseClient, config: Config) -> Result<Self> {
        config.validate()?;
        if base.is_closed() {
            return Err(Error::new(Code::Closed));
        }
        let base = base.clone();
        let shutdown = base.shutdown().child_token();
        let activity = Arc::new(Activity::new(shutdown.clone()));
        let runtime = RuntimeConfig::new(&base, config);
        let initial_policy = runtime.initial_policy;
        let error_buffer_capacity = runtime.registration_error_buffer_capacity;
        let inner = Arc::new(ClientInner {
            config: runtime,
            base,
            scripts: RegistrationScripts::new(),
            limits: ArcSwap::from_pointee(initial_policy),
            shutdown,
            errors: broadcast::channel(error_buffer_capacity).0,
            configuration_refresh: Mutex::new(ConfigurationRefreshState { users: 0, worker: None }),
            activity,
        });
        inner.bootstrap().await?;
        if inner.shutdown.is_cancelled() {
            return Err(Error::new(Code::Closed));
        }
        Ok(Self { inner })
    }

    /// 订阅尽力而为的异步 Registration/Selector 诊断。
    ///
    /// 广播接收者可能因落后而丢失旧诊断；协议状态应通过具体操作结果和 Selector 状态判断。
    pub fn subscribe_errors(&self) -> broadcast::Receiver<Error> {
        self.inner.errors.subscribe()
    }

    /// 返回当前最后一份完整有效的 Redis Zone Registration 限制快照。
    pub fn registration_limits(&self) -> RegistrationLimits {
        self.inner.limits.load().limits()
    }

    /// 立即从 Redis 重读并原子发布一份完整 Zone 配置。
    ///
    /// 读取或校验失败保留上一份有效快照并返回错误。
    pub async fn refresh_configuration(&self) -> Result<()> {
        self.inner.refresh_configuration().await
    }

    /// 永久关闭 Registration 领域，并等待其接纳的 Registration、Selector 和配置刷新工作退出。
    ///
    /// Close 幂等，不关闭共享根传输；并发调用在异步关闭门汇合。
    pub async fn close(&self) -> Result<()> {
        self.inner.start_shutdown();
        self.inner.finish_close().await;
        Ok(())
    }
}

impl Clone for Client {
    /// 克隆公开 Client 句柄并增加独立于内部任务 `Arc` 的公开所有者计数。
    fn clone(&self) -> Self {
        self.inner.activity.add_handle();
        Self {
            inner: Arc::clone(&self.inner),
        }
    }
}

impl Drop for Client {
    /// 最后一个公开句柄释放时广播领域关闭，但不在 Drop 中阻塞等待。
    ///
    /// 确定任务汇合由显式 `close().await` 保证；已有任务直接观察 CancellationToken。
    fn drop(&mut self) {
        if self.inner.activity.drop_handle() {
            self.inner.start_shutdown();
        }
    }
}

impl ClientInner {
    /// 原子封住新工作，并且只在第一次调用时取消领域令牌。
    fn start_shutdown(&self) {
        self.activity.start_shutdown();
    }

    /// 串行等待全部 ActiveGuard 释放，并标记显式关闭完成。
    ///
    /// 此函数不需要额外关闭观察任务；并发 Close 通过 `close_gate` 复用完成状态。
    async fn finish_close(&self) {
        self.activity.finish_close().await;
    }

    /// 借用共享 Fred 命令客户端；生命周期由根 Client 所有。
    pub(crate) fn driver(&self) -> fred::clients::Client {
        self.base.driver()
    }

    /// 用根配置创建容量为 `capacity` 的专用 SubscriberClient。
    ///
    /// 返回客户端尚未初始化，由 Selector 负责启动、订阅、取消和释放。
    pub(crate) fn subscriber(&self, capacity: usize) -> Result<fred::clients::SubscriberClient> {
        self.base.subscriber(capacity)
    }

    /// 通过根 Client 执行 `future`，并按 `code` 保留读失败或不明确写入语义。
    pub(crate) async fn command<T, F>(&self, future: F, code: Code) -> Result<T>
    where
        F: Future<Output = FredResult<T>>,
    {
        self.base.command(future, code).await
    }

    /// 依次完成 Redis 8 校验、Zone 策略安装/读取和 Registration Lua 预加载。
    ///
    /// 任一步失败都不发布半初始化 Client，也不启动后台任务。
    async fn bootstrap(&self) -> Result<()> {
        self.require_redis8().await?;
        let limits = self.read_zone_config(true).await?;
        self.limits.store(Arc::new(limits));
        self.command(self.scripts.load(&self.driver()), Code::Unavailable).await?;
        Ok(())
    }

    /// 通过 HELLO 2 回复要求 Redis 主版本至少为 8。
    ///
    /// Redis 8 是 Registry Hash-field TTL 命令的协议前提；缺失或非法 version 返回 Corrupt。
    async fn require_redis8(&self) -> Result<()> {
        let response: Value = self
            .command(self.driver().custom(fred::cmd!("HELLO"), vec![Value::from(2)]), Code::Unavailable)
            .await?;
        let version = hello_version(response)?;
        let major = version
            .split('.')
            .next()
            .and_then(|value| value.parse::<u64>().ok())
            .ok_or_else(|| Error::field(Code::Corrupt, "redis_version"))?;
        if major < 8 {
            return Err(Error::field(Code::Protocol, "redis_version"));
        }
        Ok(())
    }

    /// 在关闭栅栏内登记一项长期领域工作，并返回自动递减 Guard。
    ///
    /// 领域显式关闭或根子令牌已取消时返回 Closed；成功后 Client::close 会等待 Guard 释放。
    pub(crate) fn admit(self: &Arc<Self>) -> Result<ActiveGuard> {
        self.activity.admit()
    }

    /// 从 Redis 读取、校验并原子替换 Zone 配置。
    ///
    /// 关闭后拒绝刷新；失败不改变 `limits` 中最后一份有效快照。
    pub(crate) async fn refresh_configuration(&self) -> Result<()> {
        if self.activity.is_closed() {
            return Err(Error::new(Code::Closed));
        }
        let limits = self.read_zone_config(false).await?;
        self.limits.store(Arc::new(limits));
        Ok(())
    }

    /// 用一次 HMGET 读取完整 Zone 配置。
    ///
    /// `install_defaults` 为 true 时用逐字段 HSETNX 补齐缺失默认值后重读，已有管理值永不覆盖。
    /// 最终回复必须由 `parse_zone_config` 完整验证。
    async fn read_zone_config(&self, install_defaults: bool) -> Result<ZoneConfig> {
        let key = format!("verdandi:config:{}", self.config.zone);
        let mut value: Value = self.command(self.driver().hmget(&key, &ZONE_CONFIG_FIELDS), Code::Unavailable).await?;
        let missing = matches!(&value, Value::Array(values) if values.iter().any(Value::is_null));
        // HSETNX 只写缺失字段，可与管理员并发配置而不覆盖其值；随后必须整体重读。
        if missing && install_defaults {
            let defaults = self.config.initial_policy.values();
            let Value::Array(values) = &value else {
                return Err(Error::field(Code::Corrupt, "verdandi:config"));
            };
            for (index, current) in values.iter().enumerate() {
                if current.is_null() {
                    let _: bool = self
                        .command(
                            self.driver().hsetnx(&key, ZONE_CONFIG_FIELDS[index], defaults[index].clone()),
                            Code::Unavailable,
                        )
                        .await?;
                }
            }
            value = self.command(self.driver().hmget(&key, &ZONE_CONFIG_FIELDS), Code::Unavailable).await?;
        }
        parse_zone_config(value)
    }

    /// 为一个 Registration 或 Selector 获取共享配置刷新租约。
    ///
    /// 首个用户惰性创建恰好一个刷新任务和 ActiveGuard，后续用户只增加引用计数。
    /// 计数溢出或领域关闭返回错误，不泄漏部分租约。
    pub(crate) fn acquire_configuration_refresh(self: &Arc<Self>) -> Result<ConfigurationRefreshLease> {
        let mut state = self.configuration_refresh.lock().map_err(|_| Error::new(Code::Corrupt))?;
        let worker = if let Some(worker) = &state.worker {
            Arc::clone(worker)
        } else {
            let guard = self.admit()?;
            let worker = Arc::new(ConfigurationRefreshWorker {
                cancel: self.shutdown.child_token(),
                finished: AtomicBool::new(false),
                done: Notify::new(),
            });
            state.worker = Some(Arc::clone(&worker));
            let owner = Arc::clone(self);
            let running = Arc::clone(&worker);
            tokio::spawn(async move {
                let _guard = guard;
                owner.configuration_refresh_loop(&running).await;
                running.finished.store(true, Ordering::Release);
                running.done.notify_waiters();
            });
            worker
        };
        state.users = state
            .users
            .checked_add(1)
            .ok_or_else(|| Error::field(Code::Capacity, "configuration_refresh"))?;
        Ok(ConfigurationRefreshLease {
            owner: Arc::clone(self),
            worker,
            released: false,
        })
    }

    /// 按当前 Redis 配置给出的抖动周期刷新 Zone 策略。
    ///
    /// `worker` 提供独立取消边界；刷新失败只报告并保留上一快照，下一周期继续尝试。
    async fn configuration_refresh_loop(&self, worker: &ConfigurationRefreshWorker) {
        loop {
            let interval = self.limits.load().configuration_refresh;
            tokio::select! {
                biased;
                () = worker.cancel.cancelled() => return,
                () = tokio::time::sleep(jittered_interval(interval, self.config.policy_refresh_jitter_percent)) => {
                    if let Err(error) = self.refresh_configuration().await {
                        self.report(error);
                    }
                }
            }
        }
    }

    /// 释放一个属于 `worker` 的配置刷新用户。
    ///
    /// 返回 true 表示最后一个用户已离开并触发任务取消；计数或 worker 身份不一致返回 Corrupt。
    fn release_configuration_refresh(&self, worker: &Arc<ConfigurationRefreshWorker>) -> Result<bool> {
        let mut state = self.configuration_refresh.lock().map_err(|_| Error::new(Code::Corrupt))?;
        if state.users == 0 {
            return Err(Error::field(Code::Corrupt, "configuration_refresh"));
        }
        state.users -= 1;
        let last = state.users == 0;
        if last {
            if state.worker.as_ref().is_none_or(|running| !Arc::ptr_eq(running, worker)) {
                return Err(Error::field(Code::Corrupt, "configuration_refresh"));
            }
            state.worker = None;
            worker.cancel.cancel();
        }
        Ok(last)
    }

    /// 尽力广播异步 `error`；没有接收者或接收者落后不会阻塞领域任务。
    pub(crate) fn report(&self, error: Error) {
        let _ = self.errors.send(error);
    }
}

/// 从 HELLO 数组 `value` 中提取 UTF-8 `version` 字段。
///
/// 回复非交替名称/值、缺失 version、类型错误或非法 UTF-8 均返回 Corrupt。
fn hello_version(value: Value) -> Result<String> {
    let Value::Array(values) = value else {
        return Err(Error::field(Code::Corrupt, "redis_version"));
    };
    let mut values = values.into_iter();
    while let Some(name) = values.next() {
        let Some(value) = values.next() else {
            return Err(Error::field(Code::Corrupt, "redis_version"));
        };
        if name.into_owned_bytes().as_deref() != Some(b"version") {
            continue;
        }
        let bytes = value.into_owned_bytes().ok_or_else(|| Error::field(Code::Corrupt, "redis_version"))?;
        return String::from_utf8(bytes).map_err(|_| Error::field(Code::Corrupt, "redis_version"));
    }
    Err(Error::field(Code::Corrupt, "redis_version"))
}

/// 在 `interval` 的正负 10% 范围生成刷新等待时间。
///
/// 亚 10ms 导致整数 span 为零时保留原值；毫秒转换与上界加法均使用饱和处理。
fn jittered_interval(interval: Duration, percent: u8) -> Duration {
    let milliseconds = u64::try_from(interval.as_millis()).unwrap_or(u64::MAX);
    let span = milliseconds.saturating_mul(u64::from(percent)) / 100;
    if span == 0 {
        return interval;
    }
    Duration::from_millis(fastrand::u64(milliseconds - span..=milliseconds.saturating_add(span)))
}

impl ConfigurationRefreshWorker {
    /// 等待刷新任务发布 `finished`，并用 Notify 双重检查避免丢失唤醒。
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
}

impl ConfigurationRefreshLease {
    /// 显式释放当前刷新租约；若为最后用户则等待共享任务退出。
    ///
    /// `self` 被消费以保证最多释放一次；内部错误原样返回。
    pub(crate) async fn release(mut self) -> Result<()> {
        let last = self.owner.release_configuration_refresh(&self.worker)?;
        self.released = true;
        if last {
            self.worker.wait_finished().await;
        }
        Ok(())
    }
}

impl Drop for ConfigurationRefreshLease {
    /// 为未显式 release 的租约执行非阻塞降级释放。
    ///
    /// Drop 无法等待最后任务；领域 Client 的 ActiveGuard 会保证显式 Client Close 仍可汇合。
    fn drop(&mut self) {
        if self.released {
            return;
        }
        match self.owner.release_configuration_refresh(&self.worker) {
            Ok(_) => {}
            Err(_) => self.worker.cancel.cancel(),
        }
        self.released = true;
    }
}

#[cfg(test)]
#[path = "../../tests/internal/registration/client.rs"]
mod tests;
