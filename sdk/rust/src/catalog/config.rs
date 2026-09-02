use std::path::PathBuf;
use std::time::Duration;

use crate::identifier::valid_zone;
use crate::{Code, Error, Result};

use super::model::MAX_RECORD_BYTES;

/// Catalog 同步、资源边界和可选本地检查点配置。
#[derive(Clone)]
pub struct Config {
    /// 区分大小写的管理 Zone 标识；无默认值，必须为 1 至 32 字节的大小写 ASCII 字母。
    pub zone: String,
    /// 一次权威范围同步允许持续的最长时间；默认 30 秒，允许范围为 100 毫秒至 1 小时且必须为整毫秒。
    pub sync_timeout: Duration,
    /// 一次索引扫描的目标条目数；默认 256，允许范围为 1 至 4096。
    pub scan_page_size: usize,
    /// 一次权威同步允许并发读取的 Path 数量；默认 32，允许范围为 1 至 256。
    pub max_inflight_reads: usize,
    /// 待修复 Path 集合和 Fred Pub/Sub 广播的容量；默认 256，允许范围为 1 至 65536。
    pub event_buffer_capacity: usize,
    /// Subscriber 异步诊断广播容量；默认 64，允许范围为 1 至 4096。
    pub error_buffer_capacity: usize,
    /// 一个 Subscriber 可持有的完整值编码总字节；默认 0，允许范围为 0 至 64 GiB，零表示不额外限制。
    pub max_view_bytes: u64,
    /// 单个完整 Catalog 值允许的最大编码字节数；默认 512 KiB，允许范围为 1 字节至 4 MiB。
    pub max_record_bytes: usize,
    /// 首轮权威恢复重试基础延迟；默认 250 毫秒，允许范围为 10 毫秒至 5 秒且必须为整毫秒。
    pub recovery_initial_delay: Duration,
    /// 连续权威恢复失败后的最大延迟；默认 5 秒，允许范围为 100 毫秒至 30 秒且必须为整毫秒。
    pub recovery_max_delay: Duration,
    /// 权威恢复指数退避倍数；默认 2，允许范围为 1 至 8。
    pub recovery_multiplier: u32,
    /// 权威恢复延迟的随机抖动百分比；默认 10，允许范围为 0 至 50，零禁用。
    pub recovery_jitter_percent: u8,
    /// 可选的本地检查点文件；默认 `None` 并禁用，`Some` 必须为非空路径；它只加速恢复，不是权威数据源。
    pub local_store_path: Option<PathBuf>,
}

impl Config {
    /// 用 `zone` 创建带默认同步、容量和无持久化设置的 Catalog 配置。
    pub fn new(zone: impl Into<String>) -> Self {
        Self {
            zone: zone.into(),
            sync_timeout: Duration::from_secs(30),
            scan_page_size: 256,
            max_inflight_reads: 32,
            event_buffer_capacity: 256,
            error_buffer_capacity: 64,
            max_view_bytes: 0,
            max_record_bytes: 512 * 1024,
            recovery_initial_delay: Duration::from_millis(250),
            recovery_max_delay: Duration::from_secs(5),
            recovery_multiplier: 2,
            recovery_jitter_percent: 10,
            local_store_path: None,
        }
    }

    /// 在不建立连接或打开检查点文件的情况下校验 Catalog 配置。
    pub fn check(&self) -> Result<()> {
        self.validate()
    }

    /// 校验 Zone、同步、内存、恢复和可选检查点的全部单值及关系约束。
    ///
    /// 任一值为空或超过对应范围时返回精确字段的 `Invalid`，且不打开 Redis 或磁盘资源。
    pub(super) fn validate(&self) -> Result<()> {
        // Zone 检查：必须是 1 至 32 字节、只包含大小写 ASCII 字母的非空标识。
        if !valid_zone(&self.zone) {
            return Err(Error::field(Code::Invalid, "zone"));
        }

        // 同步时间检查：同步超时必须是 100 毫秒至 1 小时内的整毫秒值。
        if !valid_duration(self.sync_timeout, Duration::from_millis(100), Duration::from_secs(3600)) {
            return Err(Error::field(Code::Invalid, "catalog.sync_timeout"));
        }

        // 同步容量检查：分别定位扫描页、并发读取、事件缓冲和诊断缓冲的越界值。
        if !(1..=4096).contains(&self.scan_page_size) {
            return Err(Error::field(Code::Invalid, "catalog.scan_page_size"));
        }
        if !(1..=256).contains(&self.max_inflight_reads) {
            return Err(Error::field(Code::Invalid, "catalog.max_inflight_reads"));
        }
        if !(1..=65_536).contains(&self.event_buffer_capacity) {
            return Err(Error::field(Code::Invalid, "catalog.event_buffer_capacity"));
        }
        if !(1..=4096).contains(&self.error_buffer_capacity) {
            return Err(Error::field(Code::Invalid, "catalog.error_buffer_capacity"));
        }

        // 内存与记录检查：完整视图预算为 0 至 64 GiB，单条记录上限为 1 字节至协议 4 MiB 上限。
        if self.max_view_bytes > 64 * 1024 * 1024 * 1024 {
            return Err(Error::field(Code::Invalid, "catalog.max_view_bytes"));
        }
        if !(1..=MAX_RECORD_BYTES).contains(&self.max_record_bytes) {
            return Err(Error::field(Code::Invalid, "catalog.max_record_bytes"));
        }

        // 恢复检查：两个延迟均需合法，首次延迟不得超过上限，倍数和抖动分别单独定位。
        if !valid_duration(self.recovery_initial_delay, Duration::from_millis(10), Duration::from_secs(5)) {
            return Err(Error::field(Code::Invalid, "catalog.recovery.initial_delay"));
        }
        if !valid_duration(self.recovery_max_delay, Duration::from_millis(100), Duration::from_secs(30)) {
            return Err(Error::field(Code::Invalid, "catalog.recovery.max_delay"));
        }
        if self.recovery_initial_delay > self.recovery_max_delay {
            return Err(Error::field(Code::Invalid, "catalog.recovery.initial_delay"));
        }
        if !(1..=8).contains(&self.recovery_multiplier) {
            return Err(Error::field(Code::Invalid, "catalog.recovery.multiplier"));
        }
        if self.recovery_jitter_percent > 50 {
            return Err(Error::field(Code::Invalid, "catalog.recovery.jitter_percent"));
        }

        // 检查点路径检查：None 禁用；Some 必须是 1..4096 字节 UTF-8 且不含 NUL。
        if let Some(path) = &self.local_store_path {
            let Some(text) = path.to_str() else {
                return Err(Error::field(Code::Invalid, "catalog.local_store_path"));
            };
            if text.is_empty() || text.len() > 4096 || text.as_bytes().contains(&0) {
                return Err(Error::field(Code::Invalid, "catalog.local_store_path"));
            }
        }
        Ok(())
    }
}

/// 保存通过 Config::validate 后的不可变 Catalog 运行参数。
pub(super) struct RuntimeConfig {
    /// 已校验的 1 至 32 字节纯 ASCII 字母 Zone。
    pub zone: String,
    /// 根 Client 提供的普通命令超时；根配置默认值为 2 秒，范围为 10 毫秒至 15 秒。
    pub timeout: Duration,
    /// 一代同步或修复的实际超时；默认值为 30 秒，范围为 100 毫秒至 1 小时。
    pub sync_timeout: Duration,
    /// 索引扫描的实际页大小；默认值为 256，范围为 1 至 4096。
    pub scan_page_size: usize,
    /// 权威同步的实际并发读取上限；默认值为 32，范围为 1 至 256。
    pub max_inflight_reads: usize,
    /// 待修复 Path 的实际容量；默认值为 256，范围为 1 至 65536。
    pub event_buffer_capacity: usize,
    /// 每个 Subscriber 的实际诊断缓冲容量；默认值为 64，范围为 1 至 4096。
    pub error_buffer_capacity: usize,
    /// Subscriber 完整值的实际总字节预算；默认值为 0，范围为 0 至 64 GiB，零表示不额外限制。
    pub max_view_bytes: u64,
    /// 单条 Catalog 完整值的实际字节上限；默认值为 512 KiB，范围为 1 字节至 4 MiB。
    pub max_record_bytes: usize,
    /// 权威修复实际首次退避；默认值为 250 毫秒，范围为 10 毫秒至 5 秒。
    pub recovery_initial_delay: Duration,
    /// 权威修复实际最大退避；默认值为 5 秒，范围为 100 毫秒至 30 秒。
    pub recovery_max_delay: Duration,
    /// 权威修复实际指数倍数；默认值为 2，范围为 1 至 8。
    pub recovery_multiplier: u32,
    /// 权威修复实际随机抖动百分比；默认值为 10，范围为 0 至 50。
    pub recovery_jitter_percent: u8,
}

impl RuntimeConfig {
    /// 把已校验公开配置与根命令超时收拢为不再保留路径选项的运行时值。
    pub(super) fn new(timeout: Duration, config: Config) -> Self {
        Self {
            zone: config.zone,
            timeout,
            sync_timeout: config.sync_timeout,
            scan_page_size: config.scan_page_size,
            max_inflight_reads: config.max_inflight_reads,
            event_buffer_capacity: config.event_buffer_capacity,
            error_buffer_capacity: config.error_buffer_capacity,
            max_view_bytes: config.max_view_bytes,
            max_record_bytes: config.max_record_bytes,
            recovery_initial_delay: config.recovery_initial_delay,
            recovery_max_delay: config.recovery_max_delay,
            recovery_multiplier: config.recovery_multiplier,
            recovery_jitter_percent: config.recovery_jitter_percent,
        }
    }

    /// 计算不超过上限的指数恢复退避，并从结果中扣除配置百分比内的随机抖动。
    pub(super) fn recovery_delay(&self, attempt: u32) -> Duration {
        let mut delay = self.recovery_initial_delay;
        // 指数增长检查：逐次乘以 1 至 8 的倍数，并用饱和乘法和 min 钳制到 recovery_max_delay。
        for _ in 0..attempt {
            if delay >= self.recovery_max_delay || self.recovery_multiplier <= 1 {
                break;
            }
            delay = delay.saturating_mul(self.recovery_multiplier).min(self.recovery_max_delay);
        }
        // 抖动检查：从延迟中随机扣除 0%..recovery_jitter_percent，零抖动时不调用随机数生成器。
        let span = delay.saturating_mul(u32::from(self.recovery_jitter_percent)) / 100;
        if span.is_zero() {
            return delay;
        }
        let nanos = u64::try_from(span.as_nanos()).unwrap_or(u64::MAX);
        delay.saturating_sub(Duration::from_nanos(fastrand::u64(0..=nanos)))
    }
}

/// 校验 Duration 可无损表示为整毫秒且处于指定闭区间。
fn valid_duration(value: Duration, minimum: Duration, maximum: Duration) -> bool {
    value.subsec_nanos() % 1_000_000 == 0 && value >= minimum && value <= maximum
}

#[cfg(test)]
#[path = "../../tests/internal/catalog/client.rs"]
mod tests;
