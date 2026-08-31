use std::future::Future;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use fred::interfaces::{ClientLike, FredResult};
use fred::types::Builder;
use fred::types::config::{Config as FredConfig, ConnectionConfig, DynamicPoolConfig, PerformanceConfig, ReconnectPolicy};
use tokio::sync::Mutex as AsyncMutex;
use tokio_util::sync::CancellationToken;

use crate::config::Config;
use crate::error::{Code, Error, Result};

/// 一个可供独立 Verdandi 领域客户端共享的 Redis 客户端。
#[derive(Clone)]
pub struct Client {
    inner: Arc<Inner>,
}

/// `Client` 克隆共享的 Fred 驱动、连接配置和关闭状态。
struct Inner {
    fred_config: FredConfig,
    fred_connection: ConnectionConfig,
    fred_performance: PerformanceConfig,
    fred_policy: ReconnectPolicy,
    driver: fred::clients::DynamicPool,
    timeout: Duration,
    connect_timeout: Duration,
    pool_max: usize,
    commands: AtomicUsize,
    scale_gate: AsyncMutex<()>,
    shutdown: CancellationToken,
    closed: AtomicBool,
    close_gate: AsyncMutex<()>,
    close_finished: AtomicBool,
    close_error: Mutex<Option<Error>>,
}

/// CommandGuard 统计正在等待结果的命令数，使动态连接池只在真实并发压力下扩展。
pub(crate) struct CommandGuard {
    owner: Arc<Inner>,
}

impl Drop for CommandGuard {
    /// 归还一次根命令活动计数；计数只用于扩容提示，不参与关闭所有权。
    fn drop(&mut self) {
        self.owner.commands.fetch_sub(1, Ordering::AcqRel);
    }
}

impl Client {
    /// 建立并验证一条共享 Redis 传输。
    ///
    /// `config` 只描述 Redis 连接、重连和普通命令超时。此函数不会加载领域脚本、
    /// 读取 Zone 策略、打开持久化文件或启动领域后台任务。成功返回的 `Client`
    /// 拥有传输；配置或首次 `PING` 失败时返回稳定 Verdandi 错误并关闭已建立的驱动。
    pub async fn open(config: Config) -> Result<Self> {
        config.validate()?;
        let reconnect_policy = config.reconnect_policy()?;
        let fred_config = config.fred_config()?;
        // Fred 每个 Client 是一条复用连接；DynamicPool 从最小连接数起步，
        // 由 Verdandi 的活动命令计数按需扩展，并让 Fred 回收超过最小值的空闲连接。
        let mut builder = Builder::from_config(fred_config.clone());
        builder.with_performance_config(|performance| {
            performance.broadcast_channel_capacity = 4096;
            performance.default_command_timeout = config.timeout;
        });
        builder.with_connection_config(|connection| {
            connection.connection_timeout = config.connect_timeout;
            connection.internal_command_timeout = config.connect_timeout;
            // 1 表示只允许首次发送；连接断开后不自动重放结果可能不明确的命令。
            connection.max_command_attempts = 1;
        });
        builder.set_pool_config(DynamicPoolConfig {
            min_clients: config.pool.min_connections,
            max_clients: config.pool.max_connections,
            max_idle_time: config.pool.idle_timeout,
            ..DynamicPoolConfig::default()
        });
        builder.set_policy(reconnect_policy.clone());
        let fred_connection = builder.get_connection_config().clone();
        let fred_performance = builder.get_performance_config().clone();
        let driver = builder.build_dynamic_pool().map_err(|error| Error::driver(Code::Invalid, error))?;
        // 驱动初始化只包含建连和内部握手，使用 connect_timeout；普通 PING 随后使用 timeout。
        tokio::time::timeout(config.connect_timeout, driver.init())
            .await
            .map_err(|error| Error::driver(Code::Deadline, error))?
            .map_err(|error| Error::driver(Code::Unavailable, error))?;
        let inner = Arc::new(Inner {
            fred_config,
            fred_connection,
            fred_performance,
            fred_policy: reconnect_policy,
            driver,
            timeout: config.timeout,
            connect_timeout: config.connect_timeout,
            pool_max: config.pool.max_connections,
            commands: AtomicUsize::new(0),
            scale_gate: AsyncMutex::new(()),
            shutdown: CancellationToken::new(),
            closed: AtomicBool::new(false),
            close_gate: AsyncMutex::new(()),
            close_finished: AtomicBool::new(false),
            close_error: Mutex::new(None),
        });
        let client = Self { inner };
        if let Err(error) = client.command::<String, _>(client.driver().ping(None), Code::Unavailable).await {
            // 初始化尚未向调用方转移所有权；失败路径在返回前完成有界 Fred 清理。
            client.inner.start_shutdown();
            client.inner.finish_close().await;
            return Err(error);
        }
        // 空闲扫描间隔不超过一秒且不大于 idle_timeout 的一半，回收误差有界且任务数量固定为一。
        let idle_scan = (config.pool.idle_timeout / 2).min(Duration::from_secs(1));
        client.inner.driver.start_scale_task(idle_scan);
        Ok(client)
    }

    /// 广播永久传输关闭并等待 Fred 驱动退出。
    ///
    /// 此操作幂等，不等待 Registration 或 Catalog 自己拥有的任务；需要确定关闭顺序时，
    /// 应先关闭各领域客户端。失败返回第一次驱动关闭的稳定错误。
    pub async fn close(&self) -> Result<()> {
        self.inner.start_shutdown();
        self.inner.finish_close().await;
        self.inner
            .close_error
            .lock()
            .map_err(|_| Error::new(Code::Corrupt))?
            .clone()
            .map_or(Ok(()), Err)
    }

    /// 从共享动态池选择一条 Fred 命令连接；返回的廉价克隆不拥有池关闭权。
    pub(crate) fn driver(&self) -> fred::clients::Client {
        self.inner.driver.next()
    }

    /// 返回每条普通 Redis 操作的固定超时。
    pub(crate) fn timeout(&self) -> Duration {
        self.inner.timeout
    }

    /// 借用根 Client 的单向关闭令牌。
    ///
    /// 令牌取消只表示永久根关闭，不表示暂时断网或 Sentinel 主节点切换。
    pub(crate) fn shutdown(&self) -> &CancellationToken {
        &self.inner.shutdown
    }

    /// 按 `capacity` 构造一个使用相同 Client 配置和重连策略的专用 Pub/Sub 客户端。
    ///
    /// 该客户端由调用领域独立初始化、取消和等待；根 Client 关闭后返回 `Closed`，
    /// 无效的 Fred 配置转换为稳定 `Invalid` 错误。
    pub(crate) fn subscriber(&self, capacity: usize) -> Result<fred::clients::SubscriberClient> {
        if self.is_closed() {
            return Err(Error::new(Code::Closed));
        }
        let mut builder = Builder::from_config(self.inner.fred_config.clone());
        let mut performance = self.inner.fred_performance.clone();
        performance.broadcast_channel_capacity = capacity;
        builder.set_performance_config(performance);
        builder.set_connection_config(self.inner.fred_connection.clone());
        builder.set_policy(self.inner.fred_policy.clone());
        builder.build_subscriber_client().map_err(|error| Error::driver(Code::Invalid, error))
    }

    /// 无锁报告根 Client 是否已开始永久关闭。
    pub(crate) fn is_closed(&self) -> bool {
        self.inner.closed.load(Ordering::Acquire)
    }

    /// 执行一条由调用方构造的 Fred Future。
    ///
    /// `future` 是尚未等待的单次命令，`code` 表示未知传输结果应采用的读或写语义。
    /// 返回成功值，或在根关闭、超时和驱动失败时返回稳定错误；写入类 `Ambiguous`
    /// 不会被关闭或截止错误降级为可安全重试的结果。
    pub(crate) async fn command<T, F>(&self, future: F, code: Code) -> Result<T>
    where
        F: Future<Output = FredResult<T>>,
    {
        if self.inner.shutdown.is_cancelled() {
            return Err(Error::new(Code::Closed));
        }
        let _guard = self.begin_command().await;
        // 同时等待命令超时和所有者关闭，不为普通命令额外创建令牌或观察任务。
        let timeout = tokio::time::timeout(self.inner.timeout, future);
        tokio::pin!(timeout);
        tokio::select! {
            _ = self.inner.shutdown.cancelled() => Err(Error::new(if code == Code::Ambiguous { Code::Ambiguous } else { Code::Closed })),
            result = &mut timeout => match result {
                Ok(Ok(value)) => Ok(value),
                Ok(Err(error)) => Err(Error::driver(code, error)),
                Err(error) => Err(Error::driver(if code == Code::Ambiguous { Code::Ambiguous } else { Code::Deadline }, error)),
            },
        }
    }

    /// 统计一条即将等待结果的命令，并在并发数超过现有连接数时尝试扩展一条连接。
    ///
    /// 扩容门使用 `try_lock`：只有一个命令承担连接建立，其他命令继续复用现有连接，
    /// 因而连接故障不会把整个命令路径串行化。
    pub(crate) async fn begin_command(&self) -> CommandGuard {
        let active = self.inner.commands.fetch_add(1, Ordering::AcqRel) + 1;
        if active > self.inner.driver.size() && self.inner.driver.size() < self.inner.pool_max {
            if let Ok(_gate) = self.inner.scale_gate.try_lock() {
                if active > self.inner.driver.size() && self.inner.driver.size() < self.inner.pool_max {
                    let _ = tokio::time::timeout(self.inner.connect_timeout, self.inner.driver.scale(1)).await;
                }
            }
        }
        CommandGuard {
            owner: Arc::clone(&self.inner),
        }
    }
}

impl Drop for Inner {
    /// 处理最后一个根或领域持有的 Client 克隆被释放时的异步关闭降级路径。
    ///
    /// Drop 无法等待：它先同步广播关闭，再仅在当前线程存在 Tokio Runtime 时调度
    /// Fred 清理。确定释放仍由显式 `Client::close().await` 保证。
    fn drop(&mut self) {
        self.start_shutdown();
        if self.close_finished.load(Ordering::Acquire) {
            return;
        }
        if let Ok(runtime) = tokio::runtime::Handle::try_current() {
            let driver = self.driver.clone();
            let timeout = self.timeout;
            runtime.spawn(async move {
                let _ = tokio::time::timeout(timeout, driver.quit()).await;
            });
        }
    }
}

impl Inner {
    /// 原子地进入永久关闭状态，并且只在第一次转换时取消根令牌。
    fn start_shutdown(&self) {
        if !self.closed.swap(true, Ordering::AcqRel) {
            self.shutdown.cancel();
        }
    }

    /// 在异步互斥门内最多执行一次 Fred `quit`，并缓存其稳定结果。
    ///
    /// 并发显式 Close 与 Drop 调度会串行汇合；操作超时产生 `Deadline`。
    async fn finish_close(&self) {
        let _gate = self.close_gate.lock().await;
        if self.close_finished.load(Ordering::Acquire) {
            return;
        }
        // 只有持有关闭门的首个调用执行驱动退出，其余调用直接复用缓存结果。
        let error = match tokio::time::timeout(self.timeout, self.driver.quit()).await {
            Ok(Ok(())) => None,
            Ok(Err(error)) => Some(Error::driver(Code::Unavailable, error)),
            Err(error) => Some(Error::driver(Code::Deadline, error)),
        };
        if let Ok(mut stored) = self.close_error.lock() {
            *stored = error;
        }
        self.close_finished.store(true, Ordering::Release);
    }
}
