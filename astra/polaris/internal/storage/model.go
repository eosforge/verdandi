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
	ErrInput       = errors.New("invalid almanac input")              // 输入非法 (范围/键/版本/载荷形状).
	ErrVersion     = errors.New("almanac version conflict")           // 版本冲突 (跳号或内容不一致).
	ErrHistory     = errors.New("almanac history unavailable")        // 连续历史缺失 (已淘汰).
	ErrUncertain   = errors.New("almanac commit cannot be confirmed") // 提交无法确认 (历史丢失且非当前).
	ErrCapacity    = errors.New("almanac capacity exhausted")         // 容量超限 (范围/记录/字节/预算).
	ErrClosed      = errors.New("almanac storage closed")             // 库已关闭.
	ErrCorrupt     = errors.New("almanac database invalid")           // 库损坏 (计数不一致或类型错误).
	ErrBinding     = errors.New("almanac deployment mismatch")        // 部署身份不匹配 (恢复时校验).
	ErrUnavailable = errors.New("almanac storage unavailable")        // 存储不可用 (SQL 错误等).
)

// Version 使用固定 8 字节大端 BLOB, 保留整个 uint64 范围及 SQLite 的字典序索引.
type Version uint64

// Value 返回独立 BLOB; 不将高位版本转换成有符号整数或浮点数.
func (version Version) Value() (driver.Value, error) {
	return binary.BigEndian.AppendUint64(nil, uint64(version)), nil
}

// Scan 拒绝 SQLite 动态类型转换, 失败时不覆盖调用方原值.
// input 为数据库值, 必须为 8 字节 BLOB, 否则报库损坏.
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
	Sector   string // 第一层分组 (分区), 1..128 字节; __ 开头为内部范围.
	Spectrum string // 第二层分组 (频谱), 1..128 字节.
}

// Valid 允许内部 __ 分组, 外部业务入口自行禁止访问内部范围.
func (scope Scope) Valid() bool {
	return text(scope.Sector, 128) && text(scope.Spectrum, 128)
}

// Binding 固定首次初始化的部署身份, 不保存密码或进程 ID.
type Binding struct {
	Galaxy    string // 集群标识, 初始化后不可变.
	Username  string // 基础设施账号, 初始化后不可变.
	Advertise string // 播报地址, 初始化后不可变.
}

// Limits 为单库受控资源配置, 默认每范围 64 MiB/65536 条, 总活动正文 256 MiB.
// History/Backlog 可为零以关闭增量窗口; 其余容量及 Timeout 必须为正.
type Limits struct {
	Scopes  int64         // 范围数上限.
	Records int64         // 单范围记录数上限.
	Bytes   int64         // 单范围字节上限.
	Total   int64         // 全库活动正文上限.
	History int64         // 单范围保留历史条数 (可为零关闭).
	Backlog int64         // 单范围历史字节上限 (可为零关闭).
	Timeout time.Duration // 单次数据库工作超时.
	WAL     int64         // WAL 触发检查点阈值.
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
	Version Version // 期望版本, 必须恰好为当前+1 (重放除外).
	Key     string  // 键, 1..1024 字节合法文本.
	Value   []byte  // 载荷, 上限 1 MiB; 删除时必须为空.
	Erase   bool    // 是否删除, 与设值互斥.
}

// Record 为完整当前值, 空 Value 仍表示存在的记录.
type Record struct {
	Key   string // 键.
	Value []byte // 当前载荷, 空仍表示记录存在 (零字节 Set).
}

// Position 标识一个已提交 Scope, 已清空的 Scope 仍保留版本.
type Position struct {
	Scope           // 范围.
	Version Version // 已提交版本.
}

// Snapshot 的版本和记录来自同一个有限读事务, 返回后不再持有 SQL 连接或游标.
type Snapshot struct {
	Position          // 快照位置 (范围+版本).
	Records  []Record // 快照记录, 与版本同属一次读事务.
}

// Replay 只返回完整连续后缀; 缺失历史返回 ErrHistory, 不返回部分成功.
type Replay struct {
	Version Version  // 后缀末版本.
	Changes []Change // 连续变更, 版本严格 +1 递增.
}

// text 检查线上文本的字节边界和 UTF-8, 不进行大小写或 Unicode 归一化.
func text(value string, maximum int) bool {
	return len(value) > 0 && len(value) <= maximum && !strings.ContainsRune(value, 0) && utf8.ValidString(value)
}

// group 只在 SQL 映射中使用, 计费与数据/历史始终在同一事务更新.
type group struct {
	Version  Version // 当前版本.
	Records  int64   // 记录数.
	Bytes    int64   // 活动字节 (键+载荷).
	Retained int64   // 保留历史条数.
	Backlog  int64   // 保留历史字节.
}

// metadata 的计数让热写入不必 SUM 全部 Scope; 启动恢复重新检查持久计数.
type metadata struct {
	Format    int64  // 库格式版本, 当前为 1.
	Galaxy    string // 初始化集群标识.
	Username  string // 初始化基础设施账号.
	Advertise string // 初始化播报地址.
	Scopes    int64  // 范围总数.
	Bytes     int64  // 全库活动字节.
}
