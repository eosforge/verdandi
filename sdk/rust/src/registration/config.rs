use std::time::Duration;

use fred::types::Value;

use crate::client::Client;
use crate::error::{Code, Error, Result};
use crate::identifier::valid_zone;

pub(crate) const PROTOCOL_VERSION: &str = "v1";
pub(crate) const ZONE_CONFIG_FIELDS: [&str; 8] = [
    "protocol",
    "registration_attr_max_fields",
    "registration_data_max_fields",
    "registration_max_field_name_bytes",
    "registration_attr_max_field_value_bytes",
    "registration_data_max_field_value_bytes",
    "registration_max_bytes",
    "configuration_refresh_ms",
];

/// 一个 Zone 的 Registration 与 Selector 本地行为配置。
#[derive(Clone, Debug)]
pub struct Config {
    /// 区分大小写的管理 Zone 标识；无默认值，必须为 1 至 32 字节的大小写 ASCII 字母。
    pub zone: String,
    /// 每个 Registration Fields 邮箱同时接纳的结果等待者上限；默认 8，允许范围为 1 至 256。
    ///
    /// 邮箱始终只有一个合并中的字段集合；该值只限制等待结果的调用数。
    pub registration_buffer_capacity: usize,
    /// Registration 领域和单个 Registration 的异步诊断缓冲容量；默认 16，允许范围为 1 至 1024。
    pub registration_error_buffer_capacity: usize,
    /// RegistrationOptions 允许配置的最短显式或自动续期间隔；默认 100 毫秒，允许范围为 10 毫秒至 60 秒且必须为整毫秒。
    pub minimum_renew_interval: Duration,
    /// 自动续期间隔的正负抖动百分比；默认 10，允许范围为 0 至 50，零表示禁用。
    pub renew_jitter_percent: u8,
    /// Redis Zone 策略刷新周期的正负抖动百分比；默认 10，允许范围为 0 至 50，零表示禁用。
    pub policy_refresh_jitter_percent: u8,
    /// 首次创建 `verdandi:config:<zone>` 时用于补齐缺项的默认策略。
    ///
    /// 默认值和范围见 `RegistrationLimits`；Redis 已存在的管理员值不会被覆盖，后续刷新仍以 Redis 完整快照为准。
    pub policy: RegistrationLimits,
    /// Selector 每页 Registry/记录读取的最大条目数；默认 256，允许范围为 1 至 1024。
    pub selector_page_size: usize,
    /// 初始同步期间最多合并的 Registration 事件条目数；默认 4096，允许范围为 1 至 65536。
    pub selector_event_buffer: usize,
    /// 初始同步期间合并事件允许占用的估算总字节数；默认 64 MiB，允许范围为 1 字节至 1 GiB。
    pub selector_event_bytes: usize,
    /// 事件变化合并后发布新本地视图的最短间隔；默认 10 毫秒，允许范围为 0 至 1 秒且必须为整毫秒。
    pub selector_publish_interval: Duration,
    /// 一代 Selector 权威同步允许持续的最长时间；默认 30 秒，允许范围为 100 毫秒至 1 小时且必须为整毫秒。
    pub selector_sync_timeout: Duration,
    /// Selector 活跃不可变视图允许占用的估算总字节数；默认 256 MiB，允许范围为 1 字节至 1 GiB。
    pub selector_max_bytes: usize,
    /// 非明确删除记录的额外保留预算；`None` 使用 64 MiB，显式值允许范围为 0 至 1 GiB，零禁用 retained 视图。
    pub selector_retained_bytes: Option<usize>,
    /// 连接级 RedisClock 的周期校准间隔；默认 30 秒，允许范围为 1 秒至 1 小时且必须为整毫秒。
    pub clock_refresh: Duration,
    /// 每次 RedisClock 样本额外保守加入的时钟不确定度；默认 1 毫秒，允许范围为 0 至 1 秒且必须为整毫秒。
    pub clock_uncertainty: Duration,
    /// 每个 Selector 的异步诊断广播容量；默认 16，允许范围为 1 至 1024。
    pub selector_error_buffer_capacity: usize,
    /// Selector 首轮恢复重试的基础延迟；默认 100 毫秒，允许范围为 10 毫秒至 5 秒且必须为整毫秒。
    pub selector_recovery_initial_delay: Duration,
    /// Selector 恢复指数退避的最大延迟；默认 5 秒，允许范围为 100 毫秒至 30 秒且必须为整毫秒。
    pub selector_recovery_max_delay: Duration,
    /// Selector 连续恢复失败后的指数增长倍数；默认 2，允许范围为 1 至 8。
    pub selector_recovery_multiplier: u8,
    /// Selector 恢复延迟的正负随机抖动百分比；默认 50，允许范围为 0 至 50，零表示禁用。
    pub selector_recovery_jitter_percent: u8,
}

impl Config {
    /// 用 `zone` 创建带生产默认值的 Registration 配置。
    ///
    /// 此函数不校验或执行 I/O；`Client::open` 会统一验证调用方后续修改的字段。
    pub fn new(zone: impl Into<String>) -> Self {
        Self {
            zone: zone.into(),
            registration_buffer_capacity: 8,
            registration_error_buffer_capacity: 16,
            minimum_renew_interval: Duration::from_millis(100),
            renew_jitter_percent: 10,
            policy_refresh_jitter_percent: 10,
            policy: RegistrationLimits::default(),
            selector_page_size: 256,
            selector_event_buffer: 4096,
            selector_event_bytes: 64 * 1024 * 1024,
            selector_publish_interval: Duration::from_millis(10),
            selector_sync_timeout: Duration::from_secs(30),
            selector_max_bytes: 256 * 1024 * 1024,
            selector_retained_bytes: None,
            clock_refresh: Duration::from_secs(30),
            clock_uncertainty: Duration::from_millis(1),
            selector_error_buffer_capacity: 16,
            selector_recovery_initial_delay: Duration::from_millis(100),
            selector_recovery_max_delay: Duration::from_secs(5),
            selector_recovery_multiplier: 2,
            selector_recovery_jitter_percent: 50,
        }
    }

    /// 在不建立连接的情况下校验 Registration、Selector 和初始 Zone 策略。
    pub fn check(&self) -> Result<()> {
        self.validate()
    }

    /// 校验 Zone、分页、事件、同步、视图、retained 与时钟资源上限。
    ///
    /// 任一值超过对应范围时返回 `Invalid`；允许零的抖动、发布间隔、时钟误差和 retained 预算在下方分组单独检查。
    pub(super) fn validate(&self) -> Result<()> {
        // Zone 检查：必须是 1 至 32 字节、只包含大小写 ASCII 字母的非空标识。
        if !valid_zone(&self.zone) {
            return Err(Error::field(Code::Invalid, "zone"));
        }

        // Registration 本地检查：分别定位邮箱等待者、诊断缓冲和最短续期间隔的非法值。
        if !(1..=256).contains(&self.registration_buffer_capacity) {
            return Err(Error::field(Code::Invalid, "registration.buffer_capacity"));
        }
        if !(1..=1024).contains(&self.registration_error_buffer_capacity) {
            return Err(Error::field(Code::Invalid, "registration.error_buffer_capacity"));
        }
        if !valid_duration(self.minimum_renew_interval, Duration::from_millis(10), Duration::from_secs(60)) {
            return Err(Error::field(Code::Invalid, "registration.min_renew_interval"));
        }

        // Registration 策略检查：两个抖动均为 0% 至 50%，初始 Redis Zone 策略的每个字段也必须合法。
        if self.renew_jitter_percent > 50 {
            return Err(Error::field(Code::Invalid, "registration.renew_jitter_percent"));
        }
        if self.policy_refresh_jitter_percent > 50 {
            return Err(Error::field(Code::Invalid, "registration.policy_refresh_jitter_percent"));
        }
        if !self.policy.valid() {
            return Err(Error::field(Code::Invalid, "registration.policy"));
        }

        // Selector 同步缓存检查：分别定位页大小、待处理 UUID 数和事件字节预算。
        if !(1..=1024).contains(&self.selector_page_size) {
            return Err(Error::field(Code::Invalid, "selector.scan_page_size"));
        }
        if !(1..=65_536).contains(&self.selector_event_buffer) {
            return Err(Error::field(Code::Invalid, "selector.max_pending_entries"));
        }
        if !(1..=1024 * 1024 * 1024).contains(&self.selector_event_bytes) {
            return Err(Error::field(Code::Invalid, "selector.max_pending_bytes"));
        }

        // Selector 同步时间检查：发布间隔为 0 至 1 秒，同步超时为 100 毫秒至 1 小时，两者都必须是整毫秒。
        if !valid_duration(self.selector_publish_interval, Duration::ZERO, Duration::from_secs(1)) {
            return Err(Error::field(Code::Invalid, "selector.view_publish_interval"));
        }
        if !valid_duration(self.selector_sync_timeout, Duration::from_millis(100), Duration::from_secs(3600)) {
            return Err(Error::field(Code::Invalid, "selector.sync_timeout"));
        }

        // Selector 视图预算检查：活动视图为 1 字节至 1 GiB，显式 retained 预算为 0 至 1 GiB。
        if !(1..=1024 * 1024 * 1024).contains(&self.selector_max_bytes) {
            return Err(Error::field(Code::Invalid, "selector.max_active_bytes"));
        }
        if self.selector_retained_bytes.is_some_and(|bytes| bytes > 1024 * 1024 * 1024) {
            return Err(Error::field(Code::Invalid, "selector.max_retained_bytes"));
        }

        // RedisClock 检查：校准周期为 1 秒至 1 小时，附加误差为 0 至 1 秒，两者都必须是整毫秒。
        if !valid_duration(self.clock_refresh, Duration::from_secs(1), Duration::from_secs(3600)) {
            return Err(Error::field(Code::Invalid, "selector.clock_refresh_interval"));
        }
        if !valid_duration(self.clock_uncertainty, Duration::ZERO, Duration::from_secs(1)) {
            return Err(Error::field(Code::Invalid, "selector.clock_uncertainty"));
        }

        // Selector 恢复检查：分别定位诊断缓冲、两个延迟、指数倍数、抖动和延迟关系。
        if !(1..=1024).contains(&self.selector_error_buffer_capacity) {
            return Err(Error::field(Code::Invalid, "selector.error_buffer_capacity"));
        }
        if !valid_duration(self.selector_recovery_initial_delay, Duration::from_millis(10), Duration::from_secs(5)) {
            return Err(Error::field(Code::Invalid, "selector.recovery.initial_delay"));
        }
        if !valid_duration(self.selector_recovery_max_delay, Duration::from_millis(100), Duration::from_secs(30)) {
            return Err(Error::field(Code::Invalid, "selector.recovery.max_delay"));
        }
        if self.selector_recovery_initial_delay > self.selector_recovery_max_delay {
            return Err(Error::field(Code::Invalid, "selector.recovery.initial_delay"));
        }
        if !(1..=8).contains(&self.selector_recovery_multiplier) {
            return Err(Error::field(Code::Invalid, "selector.recovery.multiplier"));
        }
        if self.selector_recovery_jitter_percent > 50 {
            return Err(Error::field(Code::Invalid, "selector.recovery.jitter_percent"));
        }
        Ok(())
    }
}

/// 保存通过 Config::validate 后的不可变 Registration/Selector 运行参数。
pub(crate) struct RuntimeConfig {
    /// 已校验的 1 至 32 字节纯 ASCII 字母 Zone。
    pub zone: String,
    /// 根 Client 提供的普通命令超时；根配置默认值为 2 秒，范围为 10 毫秒至 15 秒。
    pub timeout: Duration,
    /// 每个 Registration 邮箱实际允许的等待者数；默认值为 8，范围为 1 至 256。
    pub registration_buffer_capacity: usize,
    /// Registration 诊断缓冲实际容量；默认值为 16，范围为 1 至 1024。
    pub registration_error_buffer_capacity: usize,
    /// 实际最短续期间隔；默认值为 100 毫秒，范围为 10 毫秒至 60 秒。
    pub minimum_renew_interval: Duration,
    /// 实际续期抖动百分比；默认值为 10，范围为 0 至 50。
    pub renew_jitter_percent: u8,
    /// 实际策略刷新抖动百分比；默认值为 10，范围为 0 至 50。
    pub policy_refresh_jitter_percent: u8,
    /// 已通过容量检查的初始 Redis Zone 策略；具体默认值和范围见 `ZoneConfig` 与 `RegistrationLimits`。
    pub initial_policy: ZoneConfig,
    /// HSCAN 与批量读取实际页大小；默认值为 256，范围为 1 至 1024。
    pub selector_page_size: usize,
    /// 同步期间待处理 UUID 实际上限；默认值为 4096，范围为 1 至 65536。
    pub selector_event_buffer: usize,
    /// 同步期间待处理事件实际字节上限；默认值为 64 MiB，范围为 1 字节至 1 GiB。
    pub selector_event_bytes: usize,
    /// 本地视图实际最短发布间隔；默认值为 10 毫秒，范围为 0 至 1 秒。
    pub selector_publish_interval: Duration,
    /// 一代同步实际总超时；默认值为 30 秒，范围为 100 毫秒至 1 小时。
    pub selector_sync_timeout: Duration,
    /// 活动视图实际字节预算；默认值为 256 MiB，范围为 1 字节至 1 GiB。
    pub selector_max_bytes: usize,
    /// retained 视图实际字节预算；默认值为 64 MiB，范围为 0 至 1 GiB，零表示禁用。
    selector_retained_bytes: usize,
    /// RedisClock 实际重新校准周期；默认值为 30 秒，范围为 1 秒至 1 小时。
    pub clock_refresh: Duration,
    /// RedisClock 样本实际附加误差；默认值为 1 毫秒，范围为 0 至 1 秒。
    pub clock_uncertainty: Duration,
    /// 每个 Selector 的实际诊断缓冲容量；默认值为 16，范围为 1 至 1024。
    pub selector_error_buffer_capacity: usize,
    /// Selector 实际首次恢复延迟；默认值为 100 毫秒，范围为 10 毫秒至 5 秒。
    pub selector_recovery_initial_delay: Duration,
    /// Selector 实际最大恢复延迟；默认值为 5 秒，范围为 100 毫秒至 30 秒。
    pub selector_recovery_max_delay: Duration,
    /// Selector 实际恢复指数倍数；默认值为 2，范围为 1 至 8。
    pub selector_recovery_multiplier: u8,
    /// Selector 实际恢复抖动百分比；默认值为 50，范围为 0 至 50。
    pub selector_recovery_jitter_percent: u8,
}

impl RuntimeConfig {
    /// 把公开 `config` 与共享根 `client` 的普通命令超时合并为不可变运行时配置。
    ///
    /// 调用前必须已验证 Config；`selector_retained_bytes=None` 在此规范化为 64 MiB。
    pub(super) fn new(client: &Client, config: Config) -> Self {
        Self {
            zone: config.zone,
            timeout: client.timeout(),
            registration_buffer_capacity: config.registration_buffer_capacity,
            registration_error_buffer_capacity: config.registration_error_buffer_capacity,
            minimum_renew_interval: config.minimum_renew_interval,
            renew_jitter_percent: config.renew_jitter_percent,
            policy_refresh_jitter_percent: config.policy_refresh_jitter_percent,
            initial_policy: ZoneConfig::from(config.policy),
            selector_page_size: config.selector_page_size,
            selector_event_buffer: config.selector_event_buffer,
            selector_event_bytes: config.selector_event_bytes,
            selector_publish_interval: config.selector_publish_interval,
            selector_sync_timeout: config.selector_sync_timeout,
            selector_max_bytes: config.selector_max_bytes,
            selector_retained_bytes: config.selector_retained_bytes.unwrap_or(64 * 1024 * 1024),
            clock_refresh: config.clock_refresh,
            clock_uncertainty: config.clock_uncertainty,
            selector_error_buffer_capacity: config.selector_error_buffer_capacity,
            selector_recovery_initial_delay: config.selector_recovery_initial_delay,
            selector_recovery_max_delay: config.selector_recovery_max_delay,
            selector_recovery_multiplier: config.selector_recovery_multiplier,
            selector_recovery_jitter_percent: config.selector_recovery_jitter_percent,
        }
    }

    /// 返回已规范化的 Selector retained 字节预算，零表示禁用。
    pub(crate) fn effective_selector_retained_bytes(&self) -> usize {
        self.selector_retained_bytes
    }
}

/// 当前生效的 Redis Zone Registration 容量配置快照。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RegistrationLimits {
    /// Attr 允许的最大顶层字段数；默认 16，允许范围为 1 至 128。
    pub attr_max_fields: usize,
    /// Data 允许的最大顶层字段数；默认 32，允许范围为 1 至 128。
    pub data_max_fields: usize,
    /// 应用字段名允许的最大字节数；默认 64，允许范围为 1 至 64。
    pub field_name_max_bytes: usize,
    /// 单个 Attr 字段值允许的最大字节数；默认 128，允许范围为 1 至 16384。
    pub attr_value_max_bytes: usize,
    /// 单个 Data 字段值允许的最大字节数；默认 128，允许范围为 1 至 16384。
    pub data_value_max_bytes: usize,
    /// 完整 Registration Hash 允许的最大估算字节数；默认 16384，允许范围为 1 至 65536。
    pub record_max_bytes: usize,
    /// 后台任务下一次读取 Zone 配置的基础周期；默认 30 秒，允许范围为 1 秒至 24 小时且必须为整毫秒。
    pub configuration_refresh: Duration,
}

impl Default for RegistrationLimits {
    /// 返回 Attr/Data 16/32 个字段、64 字节字段名、128 字节单值、16 KiB 记录和 30 秒刷新的稳定默认值。
    fn default() -> Self {
        Self {
            attr_max_fields: 16,
            data_max_fields: 32,
            field_name_max_bytes: 64,
            attr_value_max_bytes: 128,
            data_value_max_bytes: 128,
            record_max_bytes: 16 * 1024,
            configuration_refresh: Duration::from_secs(30),
        }
    }
}

impl RegistrationLimits {
    /// 校验一份将用于 Redis HSETNX 的完整默认策略。
    fn valid(self) -> bool {
        ZoneConfig::from(self).valid()
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct ZoneConfig {
    /// Redis 中当前生效的 Attr 字段数上限，范围为 1 至 128。
    pub attr_max_fields: usize,
    /// Redis 中当前生效的 Data 字段数上限，范围为 1 至 128。
    pub data_max_fields: usize,
    /// Redis 中当前生效的应用字段名字节上限，范围为 1 至 64。
    pub field_name_max_bytes: usize,
    /// Redis 中当前生效的单个 Attr 值字节上限，范围为 1 至 16384。
    pub attr_value_max_bytes: usize,
    /// Redis 中当前生效的单个 Data 值字节上限，范围为 1 至 16384。
    pub data_value_max_bytes: usize,
    /// Redis 中当前生效的完整记录字节上限，范围为 1 至 65536。
    pub record_max_bytes: usize,
    /// Redis 中当前生效的策略刷新周期，范围为 1 秒至 24 小时且必须为整毫秒。
    pub configuration_refresh: Duration,
}

impl From<RegistrationLimits> for ZoneConfig {
    /// 把已校验公开策略转换成热路径内部表示。
    fn from(value: RegistrationLimits) -> Self {
        Self {
            attr_max_fields: value.attr_max_fields,
            data_max_fields: value.data_max_fields,
            field_name_max_bytes: value.field_name_max_bytes,
            attr_value_max_bytes: value.attr_value_max_bytes,
            data_value_max_bytes: value.data_value_max_bytes,
            record_max_bytes: value.record_max_bytes,
            configuration_refresh: value.configuration_refresh,
        }
    }
}

impl Default for ZoneConfig {
    /// 返回 16/32 个 Attr/Data 字段、64 字节字段名、128 字节单值、16 KiB 记录和 30 秒刷新的默认配置。
    fn default() -> Self {
        RegistrationLimits::default().into()
    }
}

impl ZoneConfig {
    /// 校验完整 Zone 配置的容量与刷新周期。
    fn valid(self) -> bool {
        // 字段数量检查：Attr 与 Data 都必须允许 1 至 128 个顶层字段。
        if !(1..=128).contains(&self.attr_max_fields) || !(1..=128).contains(&self.data_max_fields) {
            return false;
        }

        // 单字段检查：名字最多 64 字节，Attr/Data 单值分别最多 16 KiB，三者最小均为 1 字节。
        if !(1..=64).contains(&self.field_name_max_bytes)
            || !(1..=16 * 1024).contains(&self.attr_value_max_bytes)
            || !(1..=16 * 1024).contains(&self.data_value_max_bytes)
        {
            return false;
        }

        // 完整记录检查：允许范围为 1 字节至 64 KiB。
        if !(1..=64 * 1024).contains(&self.record_max_bytes) {
            return false;
        }

        // 刷新周期检查：允许范围为 1 秒至 24 小时，并且必须能精确表示为整毫秒。
        valid_duration(self.configuration_refresh, Duration::from_secs(1), Duration::from_secs(24 * 60 * 60))
    }

    /// 按 `ZONE_CONFIG_FIELDS` 固定顺序生成规范字符串值，用于 HSETNX 初始化。
    pub(crate) fn values(self) -> [String; 8] {
        [
            PROTOCOL_VERSION.to_owned(),
            self.attr_max_fields.to_string(),
            self.data_max_fields.to_string(),
            self.field_name_max_bytes.to_string(),
            self.attr_value_max_bytes.to_string(),
            self.data_value_max_bytes.to_string(),
            self.record_max_bytes.to_string(),
            self.configuration_refresh.as_millis().to_string(),
        ]
    }

    /// 将内部 ZoneConfig 转成公开只读 RegistrationLimits 值快照。
    pub(crate) fn limits(self) -> RegistrationLimits {
        RegistrationLimits {
            attr_max_fields: self.attr_max_fields,
            data_max_fields: self.data_max_fields,
            field_name_max_bytes: self.field_name_max_bytes,
            attr_value_max_bytes: self.attr_value_max_bytes,
            data_value_max_bytes: self.data_value_max_bytes,
            record_max_bytes: self.record_max_bytes,
            configuration_refresh: self.configuration_refresh,
        }
    }

    /// 返回协议允许的绝对容量上限；只用于测试与最大记录恢复资格。
    pub(crate) const fn protocol_ceiling() -> Self {
        Self {
            attr_max_fields: 128,
            data_max_fields: 128,
            field_name_max_bytes: 64,
            attr_value_max_bytes: 16 * 1024,
            data_value_max_bytes: 16 * 1024,
            record_max_bytes: 64 * 1024,
            configuration_refresh: Duration::from_secs(30),
        }
    }
}

/// 解析 HMGET 返回的完整 Zone 配置 `value`。
///
/// 回复必须按 `ZONE_CONFIG_FIELDS` 具有恰好八个非空规范字符串；协议版本、正整数、
/// 各容量上限和 1 秒至 24 小时刷新范围全部验证后才返回完整 ZoneConfig。
pub(crate) fn parse_zone_config(value: Value) -> Result<ZoneConfig> {
    // 回复类型检查：HMGET 必须返回数组，其他 RESP 类型都视为损坏。
    let Value::Array(values) = value else {
        return Err(Error::field(Code::Corrupt, "verdandi:config"));
    };
    // 回复形状检查：数组必须严格包含协议规定的八个位置。
    if values.len() != ZONE_CONFIG_FIELDS.len() {
        return Err(Error::field(Code::Corrupt, "verdandi:config"));
    }
    // 先把所有位置转换为拥有型 UTF-8 文本，任何缺失都拒绝整份配置而不是部分采用。
    let mut text = Vec::with_capacity(values.len());
    for (index, value) in values.into_iter().enumerate() {
        if value.is_null() {
            return Err(Error::field(Code::Missing, ZONE_CONFIG_FIELDS[index]));
        }
        let Some(bytes) = value.into_owned_bytes() else {
            return Err(Error::field(Code::Corrupt, ZONE_CONFIG_FIELDS[index]));
        };
        let value = String::from_utf8(bytes).map_err(|_| Error::field(Code::Corrupt, ZONE_CONFIG_FIELDS[index]))?;
        text.push(value);
    }
    // 协议检查：第一个位置必须是当前 SDK 支持的 v1。
    if text[0] != PROTOCOL_VERSION {
        return Err(Error::field(Code::Protocol, "protocol"));
    }
    // 要求十进制文本往返一致，拒绝前导零、符号及平台 usize 溢出。
    let mut limits = [0_usize; 7];
    for index in 1..text.len() {
        let parsed = text[index]
            .parse::<usize>()
            .map_err(|_| Error::field(Code::Invalid, ZONE_CONFIG_FIELDS[index]))?;
        if parsed == 0 || parsed.to_string() != text[index] {
            return Err(Error::field(Code::Invalid, ZONE_CONFIG_FIELDS[index]));
        }
        limits[index - 1] = parsed;
    }
    let config = ZoneConfig {
        attr_max_fields: limits[0],
        data_max_fields: limits[1],
        field_name_max_bytes: limits[2],
        attr_value_max_bytes: limits[3],
        data_value_max_bytes: limits[4],
        record_max_bytes: limits[5],
        configuration_refresh: Duration::from_millis(u64::try_from(limits[6]).map_err(|_| Error::field(Code::Capacity, "configuration_refresh_ms"))?),
    };
    // 容量检查：只有所有字段和刷新周期都在 ZoneConfig::valid 固定范围内才接受快照。
    if !config.valid() {
        return Err(Error::field(Code::Capacity, "verdandi:config"));
    }
    Ok(config)
}

/// 校验 Duration 是指定闭区间内的整毫秒值。
fn valid_duration(value: Duration, minimum: Duration, maximum: Duration) -> bool {
    value.subsec_nanos() % 1_000_000 == 0 && value >= minimum && value <= maximum
}

#[cfg(test)]
#[path = "../../tests/internal/registration/config.rs"]
mod tests;
