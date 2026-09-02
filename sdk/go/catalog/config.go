package catalog

import (
	mathrand "math/rand/v2"
	"path/filepath"
	"strings"
	"time"
	"unicode/utf8"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	"github.com/eosforge/verdandi/sdk/go/internal/validate"
)

const (
	maximumFields              = 65_536
	maximumEncodedBytes        = 4 * 1024 * 1024
	maximumRevision     uint64 = 1<<53 - 1
)

// Config 定义 Catalog 的 Zone、同步资源限制、恢复策略和可选本地检查点。
// Redis 连接、连接池及普通命令超时由共享 verdandi.Client 提供。
type Config struct {
	// Zone 是区分大小写的管理隔离标识；无默认值，仅允许 1 至 32 个 ASCII 字母。
	Zone string
	// SyncTimeout 限制初始同步或一次权威修复；零值使用 30 秒，允许范围为 100 毫秒至 1 小时且必须为整毫秒。
	SyncTimeout time.Duration
	// ScanPageSize 是一次 Catalog 索引扫描的目标条目数；零值使用 256，允许范围为 1 至 4096。
	ScanPageSize int
	// MaxInflightReads 是一次权威同步可并行或批量提交的 Path 读取数；零值使用 32，允许范围为 1 至 256。
	MaxInflightReads int
	// EventBufferCapacity 限制待修复 Path 集合；零值使用 256，允许范围为 1 至 65536，达到上限时退化为一次范围同步。
	EventBufferCapacity int
	// ErrorBufferCapacity 是每个 Subscriber 的尽力而为异步诊断容量；零值使用 64，允许范围为 1 至 4096。
	ErrorBufferCapacity int
	// MaxViewBytes 限制一个 Subscriber 持有的完整值编码字节总量；零值表示不额外限制，允许范围为 0 至 64 GiB。
	MaxViewBytes int64
	// MaxRecordBytes 限制一条完整 Catalog 结构化值；零值使用 512 KiB，允许范围为 1 字节至 4 MiB。
	MaxRecordBytes int
	// RecoveryInitialDelay 是权威修复首次失败后的重试基础延迟；零值使用 250 毫秒，允许范围为 10 毫秒至 5 秒且必须为整毫秒。
	RecoveryInitialDelay time.Duration
	// RecoveryMaxDelay 是连续修复失败后的退避上限；零值使用 5 秒，允许范围为 100 毫秒至 30 秒且必须为整毫秒。
	RecoveryMaxDelay time.Duration
	// RecoveryMultiplier 是连续修复失败后的指数增长倍数；零值使用 2，允许范围为 1 至 8。
	RecoveryMultiplier int
	// RecoveryJitterPercent 是从计算退避中随机扣除的百分比上限；nil 使用 10，显式值允许范围为 0 至 50，指向零值禁用。
	RecoveryJitterPercent *int
	// LocalStorePath 启用可丢弃的 bbolt 重启检查点；默认空字符串并禁用，非空路径必须能转换为绝对规范路径。
	LocalStorePath string
}

// Check 在不建立连接或打开检查点文件的情况下校验 Catalog 配置。
// 根命令超时不属于本结构；这里仅为构造私有运行参数提供一个合法占位值。
func (config Config) Check() error {
	_, err := config.normalize(2 * time.Second)
	return err
}

// recoveryDelay 返回不超过最大值的指数退避，并从结果中扣除配置百分比内的随机抖动。
func (config runtimeConfig) recoveryDelay(attempt int) time.Duration {
	delay := config.recoveryInitial
	// 指数增长检查：逐次乘以 1 至 8 的倍数，并在乘法溢出前钳制到 recoveryMax。
	for range attempt {
		if delay >= config.recoveryMax || config.recoveryFactor <= 1 {
			break
		}
		if delay > config.recoveryMax/time.Duration(config.recoveryFactor) {
			delay = config.recoveryMax
			break
		}
		delay *= time.Duration(config.recoveryFactor)
	}
	if delay > config.recoveryMax {
		delay = config.recoveryMax
	}
	// 抖动检查：从延迟中随机扣除 0%..recoveryJitter，配置为零时不调用随机数生成器。
	span := delay * time.Duration(config.recoveryJitter) / 100
	if span <= 0 {
		return delay
	}
	return delay - time.Duration(mathrand.Int64N(int64(span)+1))
}

// runtimeConfig 保存展开默认值并校验后的不可变 Catalog 运行参数。
type runtimeConfig struct {
	// Zone 是已校验的 1 至 32 字节纯 ASCII 字母管理隔离标识。
	Zone string
	// timeout 是根 Client 提供的普通命令超时；根配置默认值为 2 秒，范围为 10 毫秒至 15 秒。
	timeout time.Duration
	// syncTimeout 是一代 Catalog 同步或修复的实际超时；默认展开值为 30 秒，范围为 100 毫秒至 1 小时。
	syncTimeout time.Duration
	// scanPageSize 是索引扫描的实际页大小；默认展开值为 256，范围为 1 至 4096。
	scanPageSize int
	// maxInflightReads 是权威同步的实际并发读取上限；默认展开值为 32，范围为 1 至 256。
	maxInflightReads int
	// eventBuffer 是待修复 Path 的实际容量；默认展开值为 256，范围为 1 至 65536。
	eventBuffer int
	// errorBuffer 是每个 Subscriber 的实际诊断缓冲容量；默认展开值为 64，范围为 1 至 4096。
	errorBuffer int
	// maxViewBytes 是 Subscriber 完整值的实际总字节预算；默认展开值为 0，范围为 0 至 64 GiB，零表示不额外限制。
	maxViewBytes int64
	// maxRecordBytes 是单条 Catalog 完整值的实际字节上限；默认展开值为 512 KiB，范围为 1 字节至 4 MiB。
	maxRecordBytes int
	// recoveryInitial 是权威修复实际首次退避；默认展开值为 250 毫秒，范围为 10 毫秒至 5 秒。
	recoveryInitial time.Duration
	// recoveryMax 是权威修复实际最大退避；默认展开值为 5 秒，范围为 100 毫秒至 30 秒。
	recoveryMax time.Duration
	// recoveryFactor 是权威修复实际指数倍数；默认展开值为 2，范围为 1 至 8。
	recoveryFactor int
	// recoveryJitter 是权威修复实际随机扣减百分比；默认展开值为 10，范围为 0 至 50。
	recoveryJitter int
	// localStorePath 是清理后的绝对检查点路径；默认空字符串并且不启用本地检查点。
	localStorePath string
}

// normalize 校验配置和根 timeout，并展开默认值与绝对本地存储路径。
func (config Config) normalize(timeout time.Duration) (runtimeConfig, error) {
	// Zone 检查：必须是 1 至 32 字节、只包含大小写 ASCII 字母的非空标识。
	if !validate.Zone(config.Zone) {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "zone", 0, nil)
	}
	// 先复制全部公开值，再只在私有运行时字段中展开默认值和派生值，避免修改调用方持有的 Config。
	result := runtimeConfig{
		Zone:             config.Zone,
		timeout:          timeout,
		syncTimeout:      config.SyncTimeout,
		scanPageSize:     config.ScanPageSize,
		maxInflightReads: config.MaxInflightReads,
		eventBuffer:      config.EventBufferCapacity,
		errorBuffer:      config.ErrorBufferCapacity,
		maxViewBytes:     config.MaxViewBytes,
		maxRecordBytes:   config.MaxRecordBytes,
		recoveryInitial:  config.RecoveryInitialDelay,
		recoveryMax:      config.RecoveryMaxDelay,
		recoveryFactor:   config.RecoveryMultiplier,
	}
	var ok bool
	// 同步超时检查：默认 30 秒，必须是 100 毫秒至 1 小时内的整毫秒值。
	result.syncTimeout, ok = validate.Duration(
		result.syncTimeout,
		30*time.Second,
		100*time.Millisecond,
		time.Hour,
	)
	if !ok {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.sync_timeout", 0, nil)
	}
	// 同步容量默认值：扫描页 256、并发读取 32、事件缓冲 256、诊断缓冲 64。
	if result.scanPageSize == 0 {
		result.scanPageSize = 256
	}
	if result.maxInflightReads == 0 {
		result.maxInflightReads = 32
	}
	if result.eventBuffer == 0 {
		result.eventBuffer = 256
	}
	if result.errorBuffer == 0 {
		result.errorBuffer = 64
	}
	// 单记录默认值：零值展开为 512 KiB，稍后检查 4 MiB 协议上限。
	if result.maxRecordBytes == 0 {
		result.maxRecordBytes = 512 * 1024
	}
	// 首次恢复延迟检查：默认 250 毫秒，必须是 10 毫秒至 5 秒内的整毫秒值。
	result.recoveryInitial, ok = validate.Duration(
		result.recoveryInitial,
		250*time.Millisecond,
		10*time.Millisecond,
		5*time.Second,
	)
	if !ok {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.recovery.initial_delay", 0, nil)
	}
	// 最大恢复延迟检查：默认 5 秒，必须是 100 毫秒至 30 秒内的整毫秒值。
	result.recoveryMax, ok = validate.Duration(
		result.recoveryMax,
		5*time.Second,
		100*time.Millisecond,
		30*time.Second,
	)
	if !ok {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.recovery.max_delay", 0, nil)
	}
	// 恢复指数倍数默认值：零值展开为 2。
	if result.recoveryFactor == 0 {
		result.recoveryFactor = 2
	}
	// 恢复抖动检查：nil 使用 10%，显式值允许 0% 至 50%。
	result.recoveryJitter, ok = validate.OptionalInt(
		config.RecoveryJitterPercent,
		10,
		0,
		50,
	)
	if !ok {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.recovery.jitter_percent", 0, nil)
	}
	// 根命令超时检查：Catalog 只接受根 Client 已规范化的正数超时。
	if result.timeout <= 0 {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "timeout", 0, nil)
	}

	// 同步容量检查：分别定位扫描页、并发读取、事件缓冲和诊断缓冲的越界值。
	if result.scanPageSize < 1 || result.scanPageSize > 4096 {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.scan_page_size", 0, nil)
	}
	if result.maxInflightReads < 1 || result.maxInflightReads > 256 {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.max_inflight_reads", 0, nil)
	}
	if result.eventBuffer < 1 || result.eventBuffer > 65_536 {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.event_buffer_capacity", 0, nil)
	}
	if result.errorBuffer < 1 || result.errorBuffer > 4096 {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.error_buffer_capacity", 0, nil)
	}

	// 内存与记录检查：完整视图预算为 0 至 64 GiB，单条记录上限为 1 字节至 4 MiB。
	if result.maxViewBytes < 0 || result.maxViewBytes > 64*1024*1024*1024 {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.max_view_bytes", 0, nil)
	}
	if result.maxRecordBytes < 1 || result.maxRecordBytes > maximumEncodedBytes {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.max_record_bytes", 0, nil)
	}

	// 恢复关系检查：指数倍数为 1 至 8，首次恢复延迟不得超过最大恢复延迟。
	if result.recoveryFactor < 1 || result.recoveryFactor > 8 {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.recovery.multiplier", 0, nil)
	}
	if result.recoveryInitial > result.recoveryMax {
		return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.recovery.initial_delay", 0, nil)
	}

	// 检查点路径检查：空字符串禁用；非空 UTF-8 路径最多 4096 字节、不得含 NUL，并须能转换为绝对路径。
	if config.LocalStorePath != "" {
		if !utf8.ValidString(config.LocalStorePath) || len(config.LocalStorePath) > 4096 || strings.IndexByte(config.LocalStorePath, 0) >= 0 {
			return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.local_store_path", 0, nil)
		}
		path, err := filepath.Abs(config.LocalStorePath)
		if err != nil {
			return runtimeConfig{}, newError(verdandi.CodeInvalid, "catalog.local_store_path", 0, err)
		}
		result.localStorePath = filepath.Clean(path)
	}
	return result, nil
}
