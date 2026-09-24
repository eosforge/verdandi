package storage

import (
	"context"
	"database/sql"
	"errors"
	"net/netip"
	"net/url"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"

	"github.com/mattn/go-sqlite3"
	"gorm.io/driver/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"
)

// Store 持有一个权威库的独占责任. 所有调用有限时, Close 等实际 SQL 工作退出后释放锁.
type Store struct {
	life      sync.RWMutex       // 生命周期读写锁, 关闭与读写互斥.
	closed    bool               // 关闭标志, 置位后所有操作拒绝.
	poisoned  atomic.Bool        // 事务异常标志, 置位后写入返回不确定, 由重启恢复.
	writer    chan struct{}      // 串行写入许可 (容量 1), 单写者模型.
	file      *os.File           // 库文件独占锁句柄, 关闭时释放.
	path      string             // 库文件绝对路径.
	limits    Limits             // 已校验资源配置, 构造后不变.
	read      *gorm.DB           // 读连接 (ORM 层), 与写分离.
	write     *gorm.DB           // 写连接 (ORM 层), 事务经此执行.
	readSQL   *sql.DB            // 读连接池 (底层), 关闭时释放.
	writeSQL  *sql.DB            // 写连接池 (底层), 关闭时释放.
	changed   chan struct{}      // 全局变更通知, 关闭表示有新提交.
	notify    sync.Mutex         // 保护订阅集合与通知通道.
	feeds     map[*Feed]struct{} // 活跃订阅集合.
	feedBytes int64              // 全部订阅预算总和, 受 128 MiB 约束.
}

// driverOnce 只注册固定连接初始化器, 不注册随请求变化的驱动或动态 DSN.
var driverOnce sync.Once

// Limits 返回实际已校验的只读配置副本, 服务准备预算不得另传较小配置规避计费.
func (store *Store) Limits() Limits { return store.limits }

// Open 显式区分新库初始化和已有库恢复; 失败保留已有文件, 不删除 WAL 或重建错误数据库.
// path/binding/limits/initialize 为库路径、部署身份、资源配置与初始化标志.
// 返回打开的库, 失败保留已有文件并返回错误.
func Open(ctx context.Context, path string, binding Binding, limits Limits, initialize bool) (_ *Store, result error) {
	// 参数组合校验: 播报地址规范、配置合法、命名合法、路径非空.
	endpoint, err := netip.ParseAddrPort(binding.Advertise)
	if !limits.valid() || !name(binding.Galaxy) || !name(binding.Username) || err != nil || endpoint.String() != binding.Advertise || endpoint.Port() == 0 || endpoint.Addr().IsUnspecified() || endpoint.Addr().IsMulticast() || endpoint.Addr().Is4In6() || endpoint.Addr().Zone() != "" || path == "" {
		return nil, ErrInput
	}
	// 路径取绝对值, 后续锁与连接都基于它.
	absolute, err := filepath.Abs(path)
	if err != nil {
		return nil, ErrInput
	}
	// 先取文件独占锁, 防止双进程同库.
	file, err := lock(absolute)
	if err != nil {
		return nil, err
	}
	store := &Store{file: file, path: absolute, limits: limits, writer: make(chan struct{}, 1), changed: make(chan struct{}), feeds: make(map[*Feed]struct{})}
	// 任何后续失败都关闭已分配资源, 不泄漏锁与连接.
	defer func() {
		if result != nil {
			store.Close()
		}
	}()
	// 初始化模式创建新文件 (已存在即失败), 建表在后继写连接上执行.
	if initialize {
		created, err := os.OpenFile(absolute, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			return nil, ErrUnavailable
		}
		if err = created.Close(); err != nil {
			return nil, ErrUnavailable
		}
	}
	// 打开前拒绝符号链接与非普通文件, 不把错误路径交给 SQLite 或意外跟随其他库.
	info, err := os.Lstat(absolute)
	if err != nil || !info.Mode().IsRegular() {
		return nil, ErrUnavailable
	}
	// 注册驱动 (仅一次), 建立写连接, 失败同样触发 deferred Close.
	driverOnce.Do(func() { sql.Register("astra-polaris", &sqlite3.SQLiteDriver{ConnectHook: connection}) })
	ctx, cancel := context.WithTimeout(ctx, limits.Timeout)
	defer cancel()
	if store.write, store.writeSQL, err = database(ctx, absolute, true); err != nil {
		return nil, err
	}
	// 初始化模式在写连接上建表并写入初始元数据, 之后同步父目录确认文件落盘.
	if initialize {
		err = store.transaction(ctx, func(tx *gorm.DB) error {
			for _, statement := range schema {
				if err := tx.Exec(statement).Error; err != nil {
					return ErrCorrupt
				}
			}
			return tx.Exec("INSERT INTO meta(id,format,galaxy,username,advertise,scopes,bytes) VALUES(1,1,?,?,?,0,0)", binding.Galaxy, binding.Username, binding.Advertise).Error
		})
		if err != nil {
			return nil, err
		}
		// SQLite FULL 确认内容; 初始化额外同步父目录, 不把新文件目录项留作未确认状态.
		directory, err := os.Open(filepath.Dir(absolute))
		if err != nil {
			return nil, ErrUnavailable
		}
		synced := directory.Sync()
		closed := directory.Close()
		if synced != nil || closed != nil {
			return nil, ErrUncertain
		}
	}
	// 建立读连接, 然后做全库恢复校验 (格式/绑定/计数), 失败拒绝启动.
	if store.read, store.readSQL, err = database(ctx, absolute, false); err != nil {
		return nil, err
	}
	if err = store.validate(ctx, binding); err != nil {
		return nil, err
	}
	return store, nil
}

// database 分离单写连接和有限读池, 读事务使用 deferred, 不提前夺取写入锁.
// path/writer 为库路径与是否为写连接; 返回 ORM 句柄与连接池, 失败关闭已建池.
func database(ctx context.Context, path string, writer bool) (*gorm.DB, *sql.DB, error) {
	// 连接参数固定: WAL、FULL 同步、1 秒忙等待、外键; 读 deferred、写 immediate.
	parameters := url.Values{"mode": {"rw"}, "_journal_mode": {"WAL"}, "_synchronous": {"FULL"}, "_busy_timeout": {"1000"}, "_foreign_keys": {"1"}, "_txlock": {"deferred"}}
	maximum := 4
	if writer {
		parameters.Set("_txlock", "immediate")
		maximum = 1
	}
	location := url.URL{Scheme: "file", Path: path, RawQuery: parameters.Encode()}
	pool, err := sql.Open("astra-polaris", location.String())
	if err != nil {
		return nil, nil, ErrUnavailable
	}
	// 池大小固定, 写单连接, 读四连接.
	pool.SetMaxOpenConns(maximum)
	pool.SetMaxIdleConns(maximum)
	if err = pool.PingContext(ctx); err != nil {
		pool.Close()
		return nil, nil, ErrUnavailable
	}
	// ORM 关闭默认事务与日志, 事务由调用方显式管理.
	db, err := gorm.Open(sqlite.New(sqlite.Config{Conn: pool}), &gorm.Config{SkipDefaultTransaction: true, Logger: logger.Discard, DisableAutomaticPing: true})
	if err != nil {
		pool.Close()
		return nil, nil, ErrUnavailable
	}
	return db, pool, nil
}

// Close 幂等退出, 不删除任何库文件或独占锁文件, 先结束连接再释放进程锁.
func (store *Store) Close() error {
	// 写锁保证关闭与读写互斥, 重复关闭直接返回.
	store.life.Lock()
	defer store.life.Unlock()
	if store.closed {
		return nil
	}
	store.closed = true
	// 先结束所有订阅并唤醒等待者, 再关连接.
	store.notify.Lock()
	close(store.changed)
	for feed := range store.feeds {
		feed.mutex.Lock()
		feed.stop(ErrClosed)
		feed.mutex.Unlock()
	}
	clear(store.feeds)
	store.feedBytes = 0
	store.notify.Unlock()
	// 依次关闭读写池与锁文件, 错误合并后统一报不可用.
	var result error
	if store.readSQL != nil {
		result = errors.Join(result, store.readSQL.Close())
	}
	if store.writeSQL != nil {
		result = errors.Join(result, store.writeSQL.Close())
	}
	result = errors.Join(result, store.file.Close())
	if result != nil {
		return ErrUnavailable
	}
	return nil
}

// Changed 在读取当前版本之前捕获, 该通道关闭后重新检查状态; 不为每个 Star 排队事件.
// 返回当前通知通道, 调用方等待关闭后重读状态.
func (store *Store) Changed() <-chan struct{} {
	store.notify.Lock()
	defer store.notify.Unlock()
	return store.changed
}

// transaction 在已串行接纳的写入边界运行. 任何 COMMIT 错误均保守冻结写入, 由重启恢复确认.
// apply 为事务闭包; 中毒后直接返回不确定, 提交失败冻结写入.
func (store *Store) transaction(ctx context.Context, apply func(*gorm.DB) error) error {
	// 已中毒直接拒绝, 不再尝试写入.
	if store.poisoned.Load() {
		return ErrUncertain
	}
	// 开启事务, 开启失败即不可用.
	tx := store.write.WithContext(ctx).Begin()
	if tx.Error != nil {
		return ErrUnavailable
	}
	// 包括准备闭包 panic 的退出路径. 已提交/明确回滚后返回 ErrTxDone 不改变原结果.
	defer tx.Rollback()
	if err := apply(tx); err != nil {
		// 闭包失败时回滚, 回滚异常 (非已结束) 即中毒.
		if rollback := tx.Rollback().Error; rollback != nil && !errors.Is(rollback, sql.ErrTxDone) {
			store.poisoned.Store(true)
			return ErrUncertain
		}
		return err
	}
	// 提交失败同样中毒, 由重启恢复确认.
	if err := tx.Commit().Error; err != nil {
		store.poisoned.Store(true)
		return ErrUncertain
	}
	return nil
}

// maintain 只在 WAL 文件达到阈值时尝试有限 PASSIVE 检查点, 不每次提交强制截断文件.
// 全部页已检查点时大文件可复用, 不因物理保留尺寸永久拒绝后续工作.
func (store *Store) maintain(ctx context.Context) error {
	// WAL 不存在或未达阈值时免去检查点, 仍有一次文件状态查询.
	info, err := os.Stat(store.path + "-wal")
	if os.IsNotExist(err) || err == nil && info.Size() < store.limits.WAL {
		return nil
	}
	if err != nil {
		return ErrUnavailable
	}
	// PASSIVE 检查点, 忙或未完全检查点即返回不可用 (下次提交重试).
	var progress struct {
		Busy         int64
		Log          int64
		Checkpointed int64
	}
	if err := store.write.WithContext(ctx).Raw("PRAGMA wal_checkpoint(PASSIVE)").Scan(&progress).Error; err != nil || progress.Busy != 0 || progress.Log > progress.Checkpointed {
		return ErrUnavailable
	}
	return nil
}

// name 与基础设施账号/Galaxy 的既有安全 ASCII 约定一致, 不接受路径或端点别名.
// value 为待校验名; 1..64 字节安全 ASCII 才合法.
func name(value string) bool {
	if len(value) == 0 || len(value) > 64 {
		return false
	}
	for _, char := range value {
		if !(char >= 'a' && char <= 'z' || char >= 'A' && char <= 'Z' || char >= '0' && char <= '9' || char == '.' || char == '_' || char == '-') {
			return false
		}
	}
	return true
}
