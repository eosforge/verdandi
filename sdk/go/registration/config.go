package registration

import (
	"strconv"
	"time"

	"github.com/eosforge/verdandi/sdk/go/internal/validate"
)

var zoneConfigFields = [...]string{
	"protocol",
	"registration_attr_max_fields",
	"registration_data_max_fields",
	"registration_max_field_name_bytes",
	"registration_attr_max_field_value_bytes",
	"registration_data_max_field_value_bytes",
	"registration_max_bytes",
	"configuration_refresh_ms",
}

// Config 定义 Registration/Selector 的 Zone 身份和进程内行为。
// Redis 连接及普通命令超时由共享 verdandi.Client 提供。
type Config struct {
	// Zone 是区分大小写的管理隔离标识；无默认值，仅允许 1 至 32 个 ASCII 字母。
	Zone string
	// BufferCapacity 限制每个 Registration Fields 邮箱同时接纳的结果等待者；零值使用 8，允许范围为 1 至 256。
	// 它只限制等待者，不创建等量请求对象或 Fields 副本；邮箱始终只有一个合并中的字段集合。
	BufferCapacity int
	// ErrorBufferCapacity 是领域 Client 和每条 Registration 的异步诊断缓冲容量；零值使用 16，允许范围为 1 至 1024。
	ErrorBufferCapacity int
	// MinimumRenewInterval 是允许 RegistrationOptions 配置的最短续期间隔；零值使用 100 毫秒，允许范围为 10 毫秒至 60 秒且必须为整毫秒。
	MinimumRenewInterval time.Duration
	// RenewJitterPercent 是自动续期间隔的正负抖动百分比；nil 使用 10，显式值允许范围为 0 至 50，指向零值表示禁用抖动。
	RenewJitterPercent *int
	// PolicyRefreshJitterPercent 是 Redis Zone 策略刷新周期的正负抖动百分比；nil 使用 10，显式值允许范围为 0 至 50，指向零值表示禁用抖动。
	PolicyRefreshJitterPercent *int
	// Policy 是首次创建 verdandi:config:<zone> 时逐字段写入的默认 Registration 策略。
	// 每个零值字段使用 RegistrationLimits 注释中的默认值和范围；Redis 已存在的管理员值永远不会被覆盖。
	Policy RegistrationLimits
	// SelectorPageSize 是 HSCAN 和批量读取目标页大小；零值使用 256，允许范围为 1 至 1024。
	SelectorPageSize int
	// SelectorEventBuffer 限制同步期间待处理 UUID 数量；零值使用 4096，允许范围为 1 至 65536。
	SelectorEventBuffer int
	// SelectorEventBytes 限制合并后待处理变更的总字节；零值使用 64 MiB，允许范围为 1 字节至 1 GiB。
	SelectorEventBytes int
	// SelectorPublishInterval 合并对外可见视图更新；nil 使用 10 毫秒，显式值允许范围为 0 至 1 秒且必须为整毫秒，指向零值表示立即发布。
	SelectorPublishInterval *time.Duration
	// SelectorSyncTimeout 限制一代完整或定向同步；零值使用 30 秒，允许范围为 100 毫秒至 1 小时且必须为整毫秒。
	SelectorSyncTimeout time.Duration
	// SelectorMaxBytes 限制一个 Selector 的活动候选不可变视图；零值使用 256 MiB，允许范围为 1 字节至 1 GiB。
	SelectorMaxBytes int
	// SelectorRetainedBytes 限制不可选择的过期保留视图；nil 使用 64 MiB，显式值允许范围为 0 至 1 GiB，指向零值表示禁用。
	SelectorRetainedBytes *int
	// ClockRefresh 控制 RedisClock 重新校准周期；零值使用 30 秒，允许范围为 1 秒至 1 小时且必须为整毫秒。
	ClockRefresh time.Duration
	// ClockUncertainty 加到实测 RedisClock 上界；nil 使用 1 毫秒，显式值允许范围为 0 至 1 秒且必须为整毫秒，指向零值表示不额外增加人工误差。
	ClockUncertainty *time.Duration
	// SelectorErrorBufferCapacity 是每个 Selector 的异步诊断缓冲容量；零值使用 16，允许范围为 1 至 1024。
	SelectorErrorBufferCapacity int
	// SelectorRecoveryInitialDelay 是 Selector 首次恢复重试的基础延迟；零值使用 100 毫秒，允许范围为 10 毫秒至 5 秒且必须为整毫秒。
	SelectorRecoveryInitialDelay time.Duration
	// SelectorRecoveryMaxDelay 是 Selector 恢复退避上限；零值使用 5 秒，允许范围为 100 毫秒至 30 秒且必须为整毫秒。
	SelectorRecoveryMaxDelay time.Duration
	// SelectorRecoveryMultiplier 是连续恢复失败后的指数增长倍数；零值使用 2，允许范围为 1 至 8。
	SelectorRecoveryMultiplier int
	// SelectorRecoveryJitterPercent 是恢复延迟的随机抖动百分比；nil 使用 50，显式值允许范围为 0 至 50，指向零值表示禁用。
	SelectorRecoveryJitterPercent *int
}

// Check 在不建立连接的情况下校验 Registration、Selector 和初始 Zone 策略。
// 根命令超时不属于本结构；这里使用其默认值完成只读校验。
func (config Config) Check() error {
	_, err := config.normalize(2 * time.Second)
	return err
}

// runtimeConfig 保存展开默认值并校验后的进程内配置，供热路径直接读取。
type runtimeConfig struct {
	// Zone 是已校验的 1 至 32 字节纯 ASCII 字母管理隔离标识。
	Zone string
	// timeout 是根 Client 提供的普通命令超时；根配置默认值为 2 秒，范围为 10 毫秒至 15 秒。
	timeout time.Duration
	// registrationBuffer 是每个 Registration 邮箱实际允许的等待者数；默认展开值为 8，范围为 1 至 256。
	registrationBuffer int
	// registrationErrorBuffer 是 Registration 诊断缓冲实际容量；默认展开值为 16，范围为 1 至 1024。
	registrationErrorBuffer int
	// minimumRenewInterval 是实际最短续期间隔；默认展开值为 100 毫秒，范围为 10 毫秒至 60 秒。
	minimumRenewInterval time.Duration
	// renewJitterPercent 是实际续期抖动百分比；默认展开值为 10，范围为 0 至 50。
	renewJitterPercent int
	// policyRefreshJitter 是实际策略刷新抖动百分比；默认展开值为 10，范围为 0 至 50。
	policyRefreshJitter int
	// zoneDefaults 是补齐所有默认值并通过容量检查的初始 Redis Zone 策略；具体默认值和范围见 zoneConfig 字段与 defaultZoneConfig。
	zoneDefaults zoneConfig
	// selectorPageSize 是 HSCAN 与批量读取实际页大小；默认展开值为 256，范围为 1 至 1024。
	selectorPageSize int
	// selectorEventBuffer 是同步期间待处理 UUID 实际上限；默认展开值为 4096，范围为 1 至 65536。
	selectorEventBuffer int
	// selectorEventBytes 是同步期间待处理事件实际字节上限；默认展开值为 64 MiB，范围为 1 字节至 1 GiB。
	selectorEventBytes int
	// selectorPublishInterval 是本地视图实际最短发布间隔；默认展开值为 10 毫秒，范围为 0 至 1 秒。
	selectorPublishInterval time.Duration
	// selectorSyncTimeout 是一代同步实际总超时；默认展开值为 30 秒，范围为 100 毫秒至 1 小时。
	selectorSyncTimeout time.Duration
	// selectorMaxBytes 是活动视图实际字节预算；默认展开值为 256 MiB，范围为 1 字节至 1 GiB。
	selectorMaxBytes int
	// selectorRetainedBytes 是 retained 视图实际字节预算；默认展开值为 64 MiB，范围为 0 至 1 GiB，零表示禁用。
	selectorRetainedBytes int
	// clockRefresh 是 RedisClock 实际重新校准周期；默认展开值为 30 秒，范围为 1 秒至 1 小时。
	clockRefresh time.Duration
	// clockUncertainty 是 RedisClock 样本实际附加误差；默认展开值为 1 毫秒，范围为 0 至 1 秒。
	clockUncertainty time.Duration
	// selectorErrorBuffer 是每个 Selector 的实际诊断缓冲容量；默认展开值为 16，范围为 1 至 1024。
	selectorErrorBuffer int
	// selectorRecoveryInitial 是 Selector 实际首次恢复延迟；默认展开值为 100 毫秒，范围为 10 毫秒至 5 秒。
	selectorRecoveryInitial time.Duration
	// selectorRecoveryMax 是 Selector 实际最大恢复延迟；默认展开值为 5 秒，范围为 100 毫秒至 30 秒。
	selectorRecoveryMax time.Duration
	// selectorRecoveryFactor 是 Selector 实际恢复指数倍数；默认展开值为 2，范围为 1 至 8。
	selectorRecoveryFactor int
	// selectorRecoveryJitter 是 Selector 实际恢复抖动百分比；默认展开值为 50，范围为 0 至 50。
	selectorRecoveryJitter int
}

// RegistrationLimits 是客户端最后读取到的完整有效 Zone 策略值副本。
type RegistrationLimits struct {
	// AttrMaxFields 是一次 Registration 可包含的最大 Attr 字段数；零值使用 16，允许范围为 1 至 128。
	AttrMaxFields int
	// DataMaxFields 是一次 Registration 可包含的最大 Data 字段数；零值使用 32，允许范围为 1 至 128。
	DataMaxFields int
	// FieldNameMaxBytes 是单个应用字段名的最大字节数；零值使用 64，允许范围为 1 至 64。
	FieldNameMaxBytes int
	// AttrValueMaxBytes 是单个 Attr 值的最大字节数；零值使用 128，允许范围为 1 至 16384。
	AttrValueMaxBytes int
	// DataValueMaxBytes 是单个 Data 值的最大字节数；零值使用 128，允许范围为 1 至 16384。
	DataValueMaxBytes int
	// RecordMaxBytes 是一条完整 Registration 的最大协议字节数；零值使用 16384，允许范围为 1 至 65536。
	RecordMaxBytes int
	// ConfigurationRefresh 是客户端采用的后台策略刷新周期；零值使用 30 秒，允许范围为 1 秒至 24 小时且必须为整毫秒。
	ConfigurationRefresh time.Duration
}

// zoneConfig 是 Redis Hash 中共享策略的内部不可变表示。
type zoneConfig struct {
	// attrMaxFields 是 Redis 中当前生效的 Attr 字段数上限，范围为 1 至 128。
	attrMaxFields int
	// dataMaxFields 是 Redis 中当前生效的 Data 字段数上限，范围为 1 至 128。
	dataMaxFields int
	// fieldNameMaxBytes 是 Redis 中当前生效的应用字段名字节上限，范围为 1 至 64。
	fieldNameMaxBytes int
	// attrValueMaxBytes 是 Redis 中当前生效的单个 Attr 值字节上限，范围为 1 至 16384。
	attrValueMaxBytes int
	// dataValueMaxBytes 是 Redis 中当前生效的单个 Data 值字节上限，范围为 1 至 16384。
	dataValueMaxBytes int
	// recordMaxBytes 是 Redis 中当前生效的完整记录字节上限，范围为 1 至 65536。
	recordMaxBytes int
	// configurationRefresh 是 Redis 中当前生效的策略刷新周期，范围为 1 秒至 24 小时。
	configurationRefresh time.Duration
}

// normalize 校验公开配置，并把所有零值默认项展开到 runtimeConfig。
// timeout 来自根客户端；返回成功时所有容量和持续时间均在协议上限内。
func (config Config) normalize(timeout time.Duration) (runtimeConfig, error) {
	// Zone 检查：必须是 1 至 32 字节、只包含大小写 ASCII 字母的非空标识。
	if !validate.Zone(config.Zone) {
		return runtimeConfig{}, protocolError(codeInvalid, "zone", 0)
	}
	// 先复制全部公开值，再只在私有运行时字段中展开默认值，避免修改调用方持有的 Config。
	result := runtimeConfig{
		Zone:                    config.Zone,
		timeout:                 timeout,
		registrationBuffer:      config.BufferCapacity,
		registrationErrorBuffer: config.ErrorBufferCapacity,
		minimumRenewInterval:    config.MinimumRenewInterval,
		selectorPageSize:        config.SelectorPageSize,
		selectorEventBuffer:     config.SelectorEventBuffer,
		selectorEventBytes:      config.SelectorEventBytes,
		selectorSyncTimeout:     config.SelectorSyncTimeout,
		selectorMaxBytes:        config.SelectorMaxBytes,
		clockRefresh:            config.ClockRefresh,
		selectorErrorBuffer:     config.SelectorErrorBufferCapacity,
		selectorRecoveryInitial: config.SelectorRecoveryInitialDelay,
		selectorRecoveryMax:     config.SelectorRecoveryMaxDelay,
		selectorRecoveryFactor:  config.SelectorRecoveryMultiplier,
	}
	// Registration 本地容量默认值：邮箱等待者使用 8，异步诊断缓冲使用 16。
	if result.registrationBuffer == 0 {
		result.registrationBuffer = 8
	}
	if result.registrationErrorBuffer == 0 {
		result.registrationErrorBuffer = 16
	}
	var ok bool
	// Registration 续期下限检查：默认 100 毫秒，必须为 10 毫秒至 60 秒内的整毫秒值。
	result.minimumRenewInterval, ok = validate.Duration(
		result.minimumRenewInterval,
		100*time.Millisecond,
		10*time.Millisecond,
		60*time.Second,
	)
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "registration.min_renew_interval", 0)
	}
	// Registration 续期抖动检查：nil 使用 10%，显式值允许 0% 至 50%。
	result.renewJitterPercent, ok = validate.OptionalInt(config.RenewJitterPercent, 10, 0, 50)
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "registration.renew_jitter_percent", 0)
	}
	// Redis Zone 策略刷新抖动检查：nil 使用 10%，显式值允许 0% 至 50%。
	result.policyRefreshJitter, ok = validate.OptionalInt(
		config.PolicyRefreshJitterPercent,
		10,
		0,
		50,
	)
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "registration.policy_refresh_jitter_percent", 0)
	}
	// 初始 Redis Zone 策略检查：逐字段补齐默认值后，必须满足 RegistrationLimits 的全部容量范围。
	result.zoneDefaults, ok = config.Policy.normalize()
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "registration.policy", 0)
	}
	// Selector 同步容量默认值：页大小 256、待处理 UUID 4096、待处理事件 64 MiB。
	if result.selectorPageSize == 0 {
		result.selectorPageSize = 256
	}
	if result.selectorEventBuffer == 0 {
		result.selectorEventBuffer = 4096
	}
	if result.selectorEventBytes == 0 {
		result.selectorEventBytes = 64 * 1024 * 1024
	}
	// Selector 发布间隔检查：默认 10 毫秒，展开后必须为 0 至 1 秒内的整毫秒值。
	result.selectorPublishInterval, ok = validate.OptionalDuration(
		config.SelectorPublishInterval,
		10*time.Millisecond,
		0,
		time.Second,
	)
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.view_publish_interval", 0)
	}
	// Selector 同步超时检查：默认 30 秒，必须为 100 毫秒至 1 小时内的整毫秒值。
	result.selectorSyncTimeout, ok = validate.Duration(
		result.selectorSyncTimeout,
		30*time.Second,
		100*time.Millisecond,
		time.Hour,
	)
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.sync_timeout", 0)
	}
	// Selector 视图预算默认值：活动视图 256 MiB，retained 视图 64 MiB；指针零值可禁用 retained。
	if result.selectorMaxBytes == 0 {
		result.selectorMaxBytes = 256 * 1024 * 1024
	}
	result.selectorRetainedBytes = 64 * 1024 * 1024
	if config.SelectorRetainedBytes != nil {
		result.selectorRetainedBytes = *config.SelectorRetainedBytes
	}
	// RedisClock 校准周期检查：默认 30 秒，必须为 1 秒至 1 小时内的整毫秒值。
	result.clockRefresh, ok = validate.Duration(
		result.clockRefresh,
		30*time.Second,
		time.Second,
		time.Hour,
	)
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.clock_refresh_interval", 0)
	}
	// RedisClock 附加误差检查：默认 1 毫秒，展开后必须为 0 至 1 秒内的整毫秒值。
	result.clockUncertainty, ok = validate.OptionalDuration(
		config.ClockUncertainty,
		time.Millisecond,
		0,
		time.Second,
	)
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.clock_uncertainty", 0)
	}
	// Selector 诊断缓冲默认值：每个 Selector 使用 16 个异步诊断槽位。
	if result.selectorErrorBuffer == 0 {
		result.selectorErrorBuffer = 16
	}
	// Selector 首次恢复延迟检查：默认 100 毫秒，必须为 10 毫秒至 5 秒内的整毫秒值。
	result.selectorRecoveryInitial, ok = validate.Duration(
		result.selectorRecoveryInitial,
		100*time.Millisecond,
		10*time.Millisecond,
		5*time.Second,
	)
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.recovery.initial_delay", 0)
	}
	// Selector 最大恢复延迟检查：默认 5 秒，必须为 100 毫秒至 30 秒内的整毫秒值。
	result.selectorRecoveryMax, ok = validate.Duration(
		result.selectorRecoveryMax,
		5*time.Second,
		100*time.Millisecond,
		30*time.Second,
	)
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.recovery.max_delay", 0)
	}
	// Selector 恢复倍数默认值：零值展开为 2。
	if result.selectorRecoveryFactor == 0 {
		result.selectorRecoveryFactor = 2
	}
	// Selector 恢复抖动检查：nil 使用 50%，显式值允许 0% 至 50%。
	result.selectorRecoveryJitter, ok = validate.OptionalInt(
		config.SelectorRecoveryJitterPercent,
		50,
		0,
		50,
	)
	if !ok {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.recovery.jitter_percent", 0)
	}
	// 根命令超时检查：领域 Client 只能接受根 Client 已规范化的正数超时。
	if result.timeout <= 0 {
		return runtimeConfig{}, protocolError(codeInvalid, "timeout", 0)
	}

	// Registration 本地容量检查：分别定位邮箱等待者和诊断缓冲的越界值。
	if result.registrationBuffer < 1 || result.registrationBuffer > 256 {
		return runtimeConfig{}, protocolError(codeInvalid, "registration.buffer_capacity", 0)
	}
	if result.registrationErrorBuffer < 1 || result.registrationErrorBuffer > 1024 {
		return runtimeConfig{}, protocolError(codeInvalid, "registration.error_buffer_capacity", 0)
	}

	// Selector 同步缓存检查：分别定位页大小、待处理 UUID 数和事件字节预算。
	if result.selectorPageSize < 1 || result.selectorPageSize > 1024 {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.scan_page_size", 0)
	}
	if result.selectorEventBuffer < 1 || result.selectorEventBuffer > 65_536 {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.max_pending_entries", 0)
	}
	if result.selectorEventBytes < 1 || result.selectorEventBytes > 1024*1024*1024 {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.max_pending_bytes", 0)
	}

	// Selector 视图预算检查：活动视图为 1 字节至 1 GiB，retained 视图为 0 至 1 GiB。
	if result.selectorMaxBytes < 1 || result.selectorMaxBytes > 1024*1024*1024 {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.max_active_bytes", 0)
	}
	if result.selectorRetainedBytes < 0 || result.selectorRetainedBytes > 1024*1024*1024 {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.max_retained_bytes", 0)
	}

	// Selector 恢复检查：分别定位诊断缓冲、指数倍数和延迟关系。
	if result.selectorErrorBuffer < 1 || result.selectorErrorBuffer > 1024 {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.error_buffer_capacity", 0)
	}
	if result.selectorRecoveryFactor < 1 || result.selectorRecoveryFactor > 8 {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.recovery.multiplier", 0)
	}
	if result.selectorRecoveryInitial > result.selectorRecoveryMax {
		return runtimeConfig{}, protocolError(codeInvalid, "selector.recovery.initial_delay", 0)
	}
	return result, nil
}

// normalize 展开 Redis Zone 策略的逐字段默认值，并检查完整取值范围。
func (limits RegistrationLimits) normalize() (zoneConfig, bool) {
	result := zoneConfig{
		attrMaxFields:        limits.AttrMaxFields,
		dataMaxFields:        limits.DataMaxFields,
		fieldNameMaxBytes:    limits.FieldNameMaxBytes,
		attrValueMaxBytes:    limits.AttrValueMaxBytes,
		dataValueMaxBytes:    limits.DataValueMaxBytes,
		recordMaxBytes:       limits.RecordMaxBytes,
		configurationRefresh: limits.ConfigurationRefresh,
	}
	defaults := defaultZoneConfig()
	// 字段数量默认值：Attr 使用 16，Data 使用 32。
	if result.attrMaxFields == 0 {
		result.attrMaxFields = defaults.attrMaxFields
	}
	if result.dataMaxFields == 0 {
		result.dataMaxFields = defaults.dataMaxFields
	}
	// 单字段默认值：字段名使用 64 字节，Attr/Data 值分别使用 128 字节。
	if result.fieldNameMaxBytes == 0 {
		result.fieldNameMaxBytes = defaults.fieldNameMaxBytes
	}
	if result.attrValueMaxBytes == 0 {
		result.attrValueMaxBytes = defaults.attrValueMaxBytes
	}
	if result.dataValueMaxBytes == 0 {
		result.dataValueMaxBytes = defaults.dataValueMaxBytes
	}
	// 完整记录默认值：一条 Registration 最多使用 16 KiB。
	if result.recordMaxBytes == 0 {
		result.recordMaxBytes = defaults.recordMaxBytes
	}
	var ok bool
	// 策略刷新默认值：30 秒；合法值为 1 秒至 24 小时内的整毫秒值。
	result.configurationRefresh, ok = validate.Duration(
		result.configurationRefresh,
		defaults.configurationRefresh,
		time.Second,
		24*time.Hour,
	)
	return result, ok && result.valid()
}

// defaultZoneConfig 返回 SDK 首次初始化 Redis 配置 Hash 时采用的共享默认策略。
func defaultZoneConfig() zoneConfig {
	return zoneConfig{
		attrMaxFields:        16,
		dataMaxFields:        32,
		fieldNameMaxBytes:    64,
		attrValueMaxBytes:    128,
		dataValueMaxBytes:    128,
		recordMaxBytes:       16 * 1024,
		configurationRefresh: 30 * time.Second,
	}
}

// valid 检查从 Redis 读取或由公开限制展开的完整 Zone 配置。
func (config zoneConfig) valid() bool {
	// 字段数量检查：Attr 与 Data 都必须允许 1 至 128 个顶层字段。
	if config.attrMaxFields < 1 || config.attrMaxFields > 128 ||
		config.dataMaxFields < 1 || config.dataMaxFields > 128 {
		return false
	}

	// 单字段检查：名字最多 64 字节，Attr/Data 单值分别最多 16 KiB，三者最小均为 1 字节。
	if config.fieldNameMaxBytes < 1 || config.fieldNameMaxBytes > 64 ||
		config.attrValueMaxBytes < 1 || config.attrValueMaxBytes > 16*1024 ||
		config.dataValueMaxBytes < 1 || config.dataValueMaxBytes > 16*1024 {
		return false
	}

	// 完整记录检查：允许范围为 1 字节至 64 KiB。
	if config.recordMaxBytes < 1 || config.recordMaxBytes > 64*1024 {
		return false
	}

	// 刷新周期检查：允许范围为 1 秒至 24 小时，并且必须能精确表示为整毫秒。
	return config.configurationRefresh >= time.Second && config.configurationRefresh <= 24*time.Hour &&
		config.configurationRefresh%time.Millisecond == 0
}

// values 按 zoneConfigFields 的固定顺序编码配置，供 HSETNX 初始化使用。
func (config zoneConfig) values() []any {
	return []any{
		protocolVersion,
		strconv.Itoa(config.attrMaxFields),
		strconv.Itoa(config.dataMaxFields),
		strconv.Itoa(config.fieldNameMaxBytes),
		strconv.Itoa(config.attrValueMaxBytes),
		strconv.Itoa(config.dataValueMaxBytes),
		strconv.Itoa(config.recordMaxBytes),
		strconv.FormatInt(config.configurationRefresh.Milliseconds(), 10),
	}
}

// parseZoneConfig 解析一次完整 HMGET 结果，并校验规范十进制、协议版本和全部容量上限。
// values 必须与 zoneConfigFields 等长；任何缺项或非法项都会拒绝整份快照。
func parseZoneConfig(values []any) (zoneConfig, error) {
	// 回复形状检查：HMGET 必须严格返回协议规定的八个位置。
	if len(values) != len(zoneConfigFields) {
		return zoneConfig{}, protocolError(codeCorrupt, "verdandi:config", 0)
	}
	// 回复类型检查：八个位置都必须存在并且是 Redis 字符串。
	text := make([]string, len(values))
	for index, value := range values {
		if value == nil {
			return zoneConfig{}, protocolError(codeMissing, zoneConfigFields[index], 0)
		}
		var ok bool
		text[index], ok = value.(string)
		if !ok {
			return zoneConfig{}, protocolError(codeCorrupt, zoneConfigFields[index], 0)
		}
	}
	// 协议检查：第一个位置必须是当前 SDK 支持的 v1。
	if text[0] != protocolVersion {
		return zoneConfig{}, protocolError(codeProtocol, "protocol", 0)
	}

	// 共享解析原语拒绝符号、零、前导零、非数字和 31 位范围外的数值。
	limits := make([]int, len(text)-1)
	for index := 1; index < len(text); index++ {
		parsed, valid := validate.UintDecimal(text[index], (1<<31)-1, false)
		if !valid {
			return zoneConfig{}, protocolError(codeInvalid, zoneConfigFields[index], 0)
		}
		limits[index-1] = int(parsed)
	}
	config := zoneConfig{
		attrMaxFields:        limits[0],
		dataMaxFields:        limits[1],
		fieldNameMaxBytes:    limits[2],
		attrValueMaxBytes:    limits[3],
		dataValueMaxBytes:    limits[4],
		recordMaxBytes:       limits[5],
		configurationRefresh: time.Duration(limits[6]) * time.Millisecond,
	}
	// 容量检查：只有所有字段和刷新周期都在 zoneConfig.valid 的固定范围内才接受快照。
	if !config.valid() {
		return zoneConfig{}, protocolError(codeCapacity, "verdandi:config", 0)
	}
	return config, nil
}

// configKey 构造指定 zone 的共享 Registration 策略 Hash 键。
func configKey(zone string) string {
	return "verdandi:config:" + zone
}
