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
	life              sync.RWMutex
	closed            bool
	poisoned          atomic.Bool
	writer            chan struct{}
	file              *os.File
	path              string
	limits            Limits
	read, write       *gorm.DB
	readSQL, writeSQL *sql.DB
	changed           chan struct{}
	notify            sync.Mutex
	feeds             map[*Feed]struct{}
	feedBytes         int64
}

// driverOnce 只注册固定连接初始化器, 不注册随请求变化的驱动或动态 DSN.
var driverOnce sync.Once

// Limits 返回实际已校验的只读配置副本, 服务准备预算不得另传较小配置规避计费.
func (store *Store) Limits() Limits { return store.limits }

// Open 显式区分新库初始化和已有库恢复; 失败保留已有文件, 不删除 WAL 或重建错误数据库.
func Open(ctx context.Context, path string, binding Binding, limits Limits, initialize bool) (_ *Store, result error) {
	endpoint, err := netip.ParseAddrPort(binding.Advertise)
	if !limits.valid() || !name(binding.Galaxy) || !name(binding.Username) || err != nil || endpoint.String() != binding.Advertise || endpoint.Port() == 0 || endpoint.Addr().IsUnspecified() || endpoint.Addr().IsMulticast() || endpoint.Addr().Is4In6() || endpoint.Addr().Zone() != "" || path == "" {
		return nil, ErrInput
	}
	absolute, err := filepath.Abs(path)
	if err != nil {
		return nil, ErrInput
	}
	file, err := lock(absolute)
	if err != nil {
		return nil, err
	}
	store := &Store{file: file, path: absolute, limits: limits, writer: make(chan struct{}, 1), changed: make(chan struct{}), feeds: make(map[*Feed]struct{})}
	defer func() {
		if result != nil {
			store.Close()
		}
	}()
	if initialize {
		created, err := os.OpenFile(absolute, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			return nil, ErrUnavailable
		}
		if err = created.Close(); err != nil {
			return nil, ErrUnavailable
		}
	}
	info, err := os.Lstat(absolute)
	if err != nil || !info.Mode().IsRegular() {
		return nil, ErrUnavailable
	}
	driverOnce.Do(func() { sql.Register("astra-polaris", &sqlite3.SQLiteDriver{ConnectHook: connection}) })
	ctx, cancel := context.WithTimeout(ctx, limits.Timeout)
	defer cancel()
	if store.write, store.writeSQL, err = database(ctx, absolute, true); err != nil {
		return nil, err
	}
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
	if store.read, store.readSQL, err = database(ctx, absolute, false); err != nil {
		return nil, err
	}
	if err = store.validate(ctx, binding); err != nil {
		return nil, err
	}
	return store, nil
}

// database 分离单写连接和有限读池, 读事务使用 deferred, 不提前夺取写入锁.
func database(ctx context.Context, path string, writer bool) (*gorm.DB, *sql.DB, error) {
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
	pool.SetMaxOpenConns(maximum)
	pool.SetMaxIdleConns(maximum)
	if err = pool.PingContext(ctx); err != nil {
		pool.Close()
		return nil, nil, ErrUnavailable
	}
	db, err := gorm.Open(sqlite.New(sqlite.Config{Conn: pool}), &gorm.Config{SkipDefaultTransaction: true, Logger: logger.Discard, DisableAutomaticPing: true})
	if err != nil {
		pool.Close()
		return nil, nil, ErrUnavailable
	}
	return db, pool, nil
}

// Close 幂等退出, 不删除任何库文件或独占锁文件, 先结束连接再释放进程锁.
func (store *Store) Close() error {
	store.life.Lock()
	defer store.life.Unlock()
	if store.closed {
		return nil
	}
	store.closed = true
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
func (store *Store) Changed() <-chan struct{} {
	store.notify.Lock()
	defer store.notify.Unlock()
	return store.changed
}

// transaction 在已串行接纳的写入边界运行. 任何 COMMIT 错误均保守冻结写入, 由重启恢复确认.
func (store *Store) transaction(ctx context.Context, apply func(*gorm.DB) error) error {
	if store.poisoned.Load() {
		return ErrUncertain
	}
	tx := store.write.WithContext(ctx).Begin()
	if tx.Error != nil {
		return ErrUnavailable
	}
	// 包括准备闭包 panic 的退出路径. 已提交/明确回滚后返回 ErrTxDone 不改变原结果.
	defer tx.Rollback()
	if err := apply(tx); err != nil {
		if rollback := tx.Rollback().Error; rollback != nil && !errors.Is(rollback, sql.ErrTxDone) {
			store.poisoned.Store(true)
			return ErrUncertain
		}
		return err
	}
	if err := tx.Commit().Error; err != nil {
		store.poisoned.Store(true)
		return ErrUncertain
	}
	return nil
}

// maintain 只在 WAL 文件达到阈值时尝试有限 PASSIVE 检查点, 不每次提交强制截断文件.
// 全部页已检查点时大文件可复用, 不因物理保留尺寸永久拒绝后续工作.
func (store *Store) maintain(ctx context.Context) error {
	info, err := os.Stat(store.path + "-wal")
	if os.IsNotExist(err) || err == nil && info.Size() < store.limits.WAL {
		return nil
	}
	if err != nil {
		return ErrUnavailable
	}
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
