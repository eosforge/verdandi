// Package storage 实现 Polaris 唯一权威库, 不暴露 GORM 或 gRPC 类型.
package storage

import (
	"database/sql/driver"
	"encoding/binary"
	"errors"
	"strings"
	"time"
	"unicode/utf8"
)

// 稳定错误只表示存储语义, RPC 层独立映射状态, 不向客户端输出 SQL/路径/正文.
var (
	ErrInput       = errors.New("invalid almanac input")
	ErrVersion     = errors.New("almanac version conflict")
	ErrHistory     = errors.New("almanac history unavailable")
	ErrUncertain   = errors.New("almanac commit cannot be confirmed")
	ErrCapacity    = errors.New("almanac capacity exhausted")
	ErrClosed      = errors.New("almanac storage closed")
	ErrCorrupt     = errors.New("almanac database invalid")
	ErrBinding     = errors.New("almanac deployment mismatch")
	ErrUnavailable = errors.New("almanac storage unavailable")
)

// Version 使用固定 8 字节大端 BLOB, 保留整个 uint64 范围及 SQLite 的字典序索引.
type Version uint64

// Value 返回独立 BLOB; 不将高位版本转换成有符号整数或浮点数.
func (version Version) Value() (driver.Value, error) {
	return binary.BigEndian.AppendUint64(nil, uint64(version)), nil
}

// Scan 拒绝 SQLite 动态类型转换, 失败时不覆盖调用方原值.
func (version *Version) Scan(input any) error {
	bytes, ok := input.([]byte)
	if !ok || len(bytes) != 8 {
		return ErrCorrupt
	}
	*version = Version(binary.BigEndian.Uint64(bytes))
	return nil
}

// Scope 不拼接路径, Sector/Spectrum 分别按 UTF-8 原字节精确匹配.
type Scope struct {
	Sector   string
	Spectrum string
}

// Valid 允许内部 __ 分组, 外部业务入口自行禁止访问内部范围.
func (scope Scope) Valid() bool {
	return text(scope.Sector, 128) && text(scope.Spectrum, 128)
}

// Binding 固定首次初始化的部署身份, 不保存密码或进程 ID.
type Binding struct {
	Galaxy    string
	Username  string
	Advertise string
}

// Limits 为单库受控资源配置, 默认每范围 64 MiB/65536 条, 总活动正文 256 MiB.
// History/Backlog 可为零以关闭增量窗口; 其余容量及 Timeout 必须为正.
type Limits struct {
	Scopes  int64
	Records int64
	Bytes   int64
	Total   int64
	History int64
	Backlog int64
	Timeout time.Duration
	WAL     int64
}

// Default 返回工程初值, 不构成协议限制或 RSS/磁盘占用保证.
func Default() Limits {
	return Limits{Scopes: 4096, Records: 65536, Bytes: 64 << 20, Total: 256 << 20, History: 1000, Backlog: 8 << 20, Timeout: 5 * time.Second, WAL: 128 << 20}
}

// valid 防止配置导致有符号计费溢出或无界查询, 单次数据库工作最多 30 秒.
func (limits Limits) valid() bool {
	return limits.Scopes > 0 && limits.Scopes <= 16384 && limits.Records > 0 && limits.Records <= 1<<20 && limits.Bytes > 0 && limits.Bytes <= 1<<30 && limits.Total >= limits.Bytes && limits.Total <= 4<<30 && limits.History >= 0 && limits.History <= 65536 && limits.Backlog >= 0 && limits.Backlog <= 64<<20 && limits.Timeout > 0 && limits.Timeout <= 30*time.Second && limits.WAL >= 1<<20 && limits.WAL <= 4<<30
}

// Change 是单个权威提交. Erase 与零字节 Set 严格区分, Buffer 由本次调用独占.
type Change struct {
	Version Version
	Key     string
	Value   []byte
	Erase   bool
}

// Record 为完整当前值, 空 Value 仍表示存在的记录.
type Record struct {
	Key   string
	Value []byte
}

// Position 标识一个已提交 Scope, 已清空的 Scope 仍保留版本.
type Position struct {
	Scope
	Version Version
}

// Snapshot 的版本和记录来自同一个有限读事务, 返回后不再持有 SQL 连接或游标.
type Snapshot struct {
	Position
	Records []Record
}

// Replay 只返回完整连续后缀; 缺失历史返回 ErrHistory, 不返回部分成功.
type Replay struct {
	Version Version
	Changes []Change
}

// text 检查线上文本的字节边界和 UTF-8, 不进行大小写或 Unicode 归一化.
func text(value string, maximum int) bool {
	return len(value) > 0 && len(value) <= maximum && !strings.ContainsRune(value, 0) && utf8.ValidString(value)
}

// group 只在 SQL 映射中使用, 计费与数据/历史始终在同一事务更新.
type group struct {
	Version  Version
	Records  int64
	Bytes    int64
	Retained int64
	Backlog  int64
}

// metadata 的计数让热写入不必 SUM 全部 Scope; 启动恢复重新检查持久计数.
type metadata struct {
	Format    int64
	Galaxy    string
	Username  string
	Advertise string
	Scopes    int64
	Bytes     int64
}
