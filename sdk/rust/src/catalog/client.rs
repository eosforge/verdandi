use std::future::Future;
use std::sync::Arc;

use fred::interfaces::FredResult;
use fred::types::Value;
use tokio_util::sync::CancellationToken;

use crate::client::Client as BaseClient;
use crate::lifecycle::{Activity, Guard as LifecycleGuard};
use crate::{Code, Error, Result};

use super::checkpoint::Checkpoint;
use super::config::{Config, RuntimeConfig};
use super::model::MAX_REVISION;
use super::scripts::{ScriptKind, Scripts};

/// 附着到一个共享 Verdandi 根 Client 的 Catalog 领域状态。
pub struct Client {
    pub(super) inner: Arc<ClientInner>,
}

pub(super) struct ClientInner {
    base: BaseClient,
    pub config: RuntimeConfig,
    pub scripts: Scripts,
    pub checkpoint: Option<Arc<Checkpoint>>,
    pub shutdown: CancellationToken,
    activity: Arc<Activity>,
}

pub(super) type ActiveGuard = LifecycleGuard;

impl Client {
    /// 把 Catalog 脚本与可选检查点附着到一个共享根 `base` Client。
    ///
    /// `config` 提供独立 Zone、同步和容量策略。此函数加载 Lua 并可阻塞打开检查点，
    /// 但不会创建常驻任务；Publisher 和 Subscriber 均按需创建自己的工作。
    pub async fn open(base: &BaseClient, config: Config) -> Result<Self> {
        config.validate()?;
        if base.is_closed() {
            return Err(Error::new(Code::Closed));
        }
        let base = base.clone();
        // bbolt/redb 等价的本地文件 I/O 不得阻塞 Tokio 执行线程，故放入 blocking 池。
        let checkpoint = if let Some(path) = config.local_store_path.clone() {
            Some(Arc::new(
                tokio::task::spawn_blocking(move || Checkpoint::open(&path))
                    .await
                    .map_err(|error| Error::driver(Code::Unavailable, error))?
                    .map_err(|error| Error::driver(Code::Unavailable, error))?,
            ))
        } else {
            None
        };
        let shutdown = base.shutdown().child_token();
        // 配置模块集中收拢已校验公开值，并在丢弃路径选项前形成紧凑运行时配置。
        let runtime = RuntimeConfig::new(base.timeout(), config);
        let activity = Arc::new(Activity::new(shutdown.clone()));
        let inner = Arc::new(ClientInner {
            config: runtime,
            base,
            scripts: Scripts::new(),
            checkpoint,
            shutdown,
            activity,
        });
        inner.command(inner.scripts.load(&inner.driver()), Code::Unavailable).await?;
        if inner.shutdown.is_cancelled() {
            return Err(Error::new(Code::Closed));
        }
        Ok(Self { inner })
    }

    /// 终止且幂等地关闭 Catalog 领域，并等待已接纳工作释放。
    ///
    /// 此操作不关闭共享根传输，也不删除 Redis 数据；并发 Close 在异步关闭门汇合。
    pub async fn close(&self) -> Result<()> {
        self.inner.start_shutdown();
        self.inner.finish_close().await;
        Ok(())
    }
}

impl Clone for Client {
    /// 克隆一个公开 Client 句柄，并增加独立于内部任务 `Arc` 的公开所有者计数。
    fn clone(&self) -> Self {
        self.inner.activity.add_handle();
        Self {
            inner: Arc::clone(&self.inner),
        }
    }
}

impl Drop for Client {
    /// 最后一个公开 Client 句柄释放时广播领域关闭，但不在 Drop 中阻塞等待。
    ///
    /// 确定的任务汇合由显式 `close().await` 保证；已有任务直接观察 `shutdown`。
    fn drop(&mut self) {
        if self.inner.activity.drop_handle() {
            self.inner.start_shutdown();
        }
    }
}

impl ClientInner {
    /// 原子封住新准入，并且只在第一次调用时取消领域令牌。
    fn start_shutdown(&self) {
        self.activity.start_shutdown();
    }

    /// 串行等待所有 ActiveGuard 释放，并标记显式关闭完成。
    ///
    /// 此函数不依赖额外关闭观察任务；并发调用由 `close_gate` 串行且复用完成状态。
    async fn finish_close(&self) {
        self.activity.finish_close().await;
    }

    /// 借用共享 Fred 命令客户端；生命周期由根 Client 所有。
    pub(super) fn driver(&self) -> fred::clients::Client {
        self.base.driver()
    }

    /// 用根配置创建容量为 `capacity` 的专用 SubscriberClient。
    ///
    /// 返回客户端尚未初始化，由 Catalog Subscriber 负责启动、订阅、取消和释放。
    pub(super) fn subscriber(&self, capacity: usize) -> Result<fred::clients::SubscriberClient> {
        self.base.subscriber(capacity)
    }

    /// 通过根 Client 执行 `future`，并按 `code` 保留读失败或不明确写入语义。
    pub(super) async fn command<T, F>(&self, future: F, code: Code) -> Result<T>
    where
        F: Future<Output = FredResult<T>>,
    {
        self.base.command(future, code).await
    }

    /// 在关闭栅栏内接纳一项 Catalog 工作，并返回自动递减的 Guard。
    ///
    /// 根或领域令牌已经取消时返回 `Closed`。无锁双重检查确保与关闭并发时，
    /// 已增加的计数会在拒绝路径立即回滚。
    pub(super) fn admit(self: &Arc<Self>) -> Result<ActiveGuard> {
        self.activity.admit()
    }

    /// 调用 `kind` 对应的 Catalog Lua，并在 NOSCRIPT 时由脚本包装自动重载。
    ///
    /// `keys` 与 `arguments` 必须符合固定 ABI；`ambiguous` 决定传输响应丢失时是否
    /// 返回 `Ambiguous`，成功返回未经领域解析的 Fred Value。
    pub(super) async fn call_script(&self, kind: ScriptKind, keys: Vec<String>, arguments: Vec<Value>, ambiguous: bool) -> Result<Value> {
        let code = if ambiguous { Code::Ambiguous } else { Code::Unavailable };
        self.command(self.scripts.get(kind).evalsha_with_reload::<Value, _, _>(&self.driver(), keys, arguments), code)
            .await
    }

    /// 校验 `revision` 是协议安全范围内的正整数；`field` 用于稳定错误定位。
    pub(super) fn validate_revision(revision: u64, field: &str) -> Result<()> {
        if revision == 0 || revision > MAX_REVISION {
            return Err(Error::field(Code::Invalid, field));
        }
        Ok(())
    }
}
