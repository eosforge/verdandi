package storage

import (
	"context"
	"database/sql/driver"
	"strings"

	"github.com/mattn/go-sqlite3"
	"gorm.io/gorm"
)

// schema 仅在显式初始化时执行. STRICT/CHECK 和复合主键禁止静默转换和重复记录.
// 四表: 全局元数据 (单行)、范围状态、当前记录、保留历史, 均 WITHOUT ROWID 按主键聚簇.
var schema = []string{
	`CREATE TABLE meta (id INTEGER PRIMARY KEY CHECK(id=1), format INTEGER NOT NULL CHECK(format=1), galaxy TEXT NOT NULL, username TEXT NOT NULL, advertise TEXT NOT NULL, scopes INTEGER NOT NULL CHECK(scopes>=0), bytes INTEGER NOT NULL CHECK(bytes>=0)) STRICT`,
	`CREATE TABLE scopes (sector TEXT NOT NULL, spectrum TEXT NOT NULL, version BLOB NOT NULL CHECK(length(version)=8), records INTEGER NOT NULL CHECK(records>=0), bytes INTEGER NOT NULL CHECK(bytes>=0), retained INTEGER NOT NULL CHECK(retained>=0), backlog INTEGER NOT NULL CHECK(backlog>=0), PRIMARY KEY(sector,spectrum)) STRICT, WITHOUT ROWID`,
	`CREATE TABLE records (sector TEXT NOT NULL, spectrum TEXT NOT NULL, key TEXT NOT NULL, value BLOB NOT NULL, PRIMARY KEY(sector,spectrum,key), FOREIGN KEY(sector,spectrum) REFERENCES scopes(sector,spectrum)) STRICT, WITHOUT ROWID`,
	`CREATE TABLE history (sector TEXT NOT NULL, spectrum TEXT NOT NULL, version BLOB NOT NULL CHECK(length(version)=8), key TEXT NOT NULL, value BLOB NOT NULL, erase INTEGER NOT NULL CHECK(erase IN (0,1)), cost INTEGER NOT NULL CHECK(cost>0), PRIMARY KEY(sector,spectrum,version), FOREIGN KEY(sector,spectrum) REFERENCES scopes(sector,spectrum)) STRICT, WITHOUT ROWID`,
}

// connection 在每次实际建立连接时设置并读取同步参数, 不依赖连接池偶然选中的连接.
// conn 为新建 SQLite 连接; 设置行上限与同步参数并读回核对, 不符即不可用.
func connection(conn *sqlite3.SQLiteConn) error {
	// 单条合法记录最大约 1 MiB, 在 SQLite 解码层限制异常大行, 不等 Go Scan 分配后再拒绝.
	conn.SetLimit(sqlite3.SQLITE_LIMIT_LENGTH, 2<<20)
	conn.SetLimit(sqlite3.SQLITE_LIMIT_SQL_LENGTH, 64<<10)
	// 关闭自动检查点依赖与不受信 schema, 参数逐连接设置.
	for _, statement := range []string{"PRAGMA wal_autocheckpoint=1000", "PRAGMA trusted_schema=OFF"} {
		if _, err := conn.Exec(statement, nil); err != nil {
			return ErrUnavailable
		}
	}
	// 关键参数读回核对: WAL 模式、同步级别、外键、检查点阈值, 任一不符即拒绝该连接.
	for query, expected := range map[string]driver.Value{"PRAGMA journal_mode": "wal", "PRAGMA synchronous": int64(2), "PRAGMA foreign_keys": int64(1), "PRAGMA wal_autocheckpoint": int64(1000)} {
		rows, err := conn.Query(query, nil)
		if err != nil {
			return ErrUnavailable
		}
		value := make([]driver.Value, 1)
		err = rows.Next(value)
		closed := rows.Close()
		if err != nil || closed != nil || value[0] != expected {
			return ErrUnavailable
		}
	}
	return nil
}

// validate 在监听之前核对格式、部署绑定、表定义及活动内容计费, 不自动修复坏库.
// binding 为期望部署身份; 任何不符都返回对应错误, 从不修改库内容.
func (store *Store) validate(ctx context.Context, binding Binding) error {
	return store.read.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		// 先做完整性快速检查, 坏页直接判损坏.
		var integrity string
		if err := tx.Raw("PRAGMA quick_check").Scan(&integrity).Error; err != nil || integrity != "ok" {
			return ErrCorrupt
		}
		// 表定义必须与 schema 逐字一致 (空白归一化后), 数量与内容都对.
		var definitions []struct{ SQL string }
		if err := tx.Raw("SELECT sql FROM sqlite_schema WHERE sql IS NOT NULL ORDER BY name").Scan(&definitions).Error; err != nil || len(definitions) != len(schema) {
			return ErrCorrupt
		}
		wanted := make(map[string]bool, len(schema))
		for _, definition := range schema {
			wanted[strings.Join(strings.Fields(definition), " ")] = true
		}
		for _, definition := range definitions {
			if !wanted[strings.Join(strings.Fields(definition.SQL), " ")] {
				return ErrCorrupt
			}
		}
		// 元数据行必须存在且格式为 1, 部署绑定必须一致.
		var meta metadata
		if err := tx.Table("meta").Where("id=1").Take(&meta).Error; err != nil || meta.Format != 1 {
			return ErrCorrupt
		}
		if meta.Galaxy != binding.Galaxy || meta.Username != binding.Username || meta.Advertise != binding.Advertise {
			return ErrBinding
		}
		// 逐范围核对状态计数、实际记录、快照一致性与历史连续性.
		var scopes []struct {
			Scope
			State group `gorm:"embedded"`
		}
		if err := tx.Table("scopes").Limit(int(store.limits.Scopes) + 1).Find(&scopes).Error; err != nil {
			return ErrCorrupt
		}
		if int64(len(scopes)) > store.limits.Scopes || meta.Scopes != int64(len(scopes)) || meta.Bytes > store.limits.Total {
			return ErrCapacity
		}
		var total int64
		for _, scope := range scopes {
			state := scope.State
			// 状态形状先行校验: 范围合法、版本非零、计数在限内且自洽.
			if !scope.Scope.Valid() || state.Version == 0 || state.Records > store.limits.Records || state.Bytes > store.limits.Bytes || state.Retained > store.limits.History || state.Backlog > store.limits.Backlog || state.Retained < 0 || uint64(state.Retained) > uint64(state.Version) {
				return ErrCorrupt
			}
			// 实际记录数与字节必须与状态一致.
			var actual struct {
				Records int64
				Bytes   int64
			}
			if err := tx.Raw("SELECT COUNT(*) AS records, COALESCE(SUM(length(CAST(key AS BLOB))+length(value)),0) AS bytes FROM records WHERE sector=? AND spectrum=?", scope.Sector, scope.Spectrum).Scan(&actual).Error; err != nil || actual.Records != state.Records || actual.Bytes != state.Bytes {
				return ErrCorrupt
			}
			// 每次只验证当前范围, 避免为启动检查复制整个数据库载荷.
			if _, err := store.snapshot(tx, scope.Scope); err != nil {
				return err
			}
			// 保留历史计数必须与实际一致, 且后缀连续可重放.
			var retained struct {
				Count int64
				Bytes int64
			}
			if err := tx.Raw("SELECT COUNT(*) AS count, COALESCE(SUM(cost),0) AS bytes FROM history WHERE sector=? AND spectrum=?", scope.Sector, scope.Spectrum).Scan(&retained).Error; err != nil || retained.Count != state.Retained || retained.Bytes != state.Backlog {
				return ErrCorrupt
			}
			if _, err := store.history(tx, scope.Scope, state, state.Version-Version(state.Retained)); err != nil {
				return err
			}
			total += actual.Bytes
		}
		// 全库字节必须与元数据一致.
		if total != meta.Bytes {
			return ErrCorrupt
		}
		// 复合外键损坏不能因为 orphan 行不在 Scope 遍历中而被漏掉.
		rows, err := tx.Raw("PRAGMA foreign_key_check").Rows()
		if err != nil {
			return ErrCorrupt
		}
		invalid := rows.Next()
		err = rows.Err()
		closed := rows.Close()
		if invalid || err != nil || closed != nil {
			return ErrCorrupt
		}
		return nil
	})
}
