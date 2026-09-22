package storage

import (
	"bytes"
	"context"
	"errors"
	"math"

	"gorm.io/gorm"
)

// Commit 仅在 SQLite 持久提交后返回成功. 参数在调用期间由调用方保持只读, 不执行网络通知.
// 已保留的同版本同内容请求返回原版本, 历史已丢失则明确返回 ErrUncertain.
func (store *Store) Commit(ctx context.Context, scope Scope, change Change) (Version, error) {
	if !scope.Valid() || !text(change.Key, 1024) || change.Version == 0 || len(change.Value) > 1<<20 || change.Erase && len(change.Value) != 0 {
		return 0, ErrInput
	}
	store.life.RLock()
	defer store.life.RUnlock()
	if store.closed {
		return 0, ErrClosed
	}
	ctx, cancel := context.WithTimeout(ctx, store.limits.Timeout)
	defer cancel()
	select {
	case store.writer <- struct{}{}:
		defer func() { <-store.writer }()
	case <-ctx.Done():
		return 0, ctx.Err()
	}
	if err := store.maintain(ctx); err != nil {
		return 0, err
	}
	// 可能需要跨 RPC 持有的正文在持久提交前复制一次, 后续目标只共享不可变 Event.
	// 即使当前暂时没有订阅, 也不能在提交后才借用请求缓冲, 因为新 Subscribe 可与提交并发.
	event := &Event{Scope: scope, Change: change}
	event.Value = bytes.Clone(change.Value)
	applied := false
	err := store.transaction(ctx, func(tx *gorm.DB) error {
		state, exists, err := current(tx, scope)
		if err != nil {
			return err
		}
		if change.Version <= state.Version {
			return confirm(tx, scope, change)
		}
		if state.Version == Version(math.MaxUint64) || change.Version != state.Version+1 {
			return ErrVersion
		}
		var meta metadata
		if err := tx.Table("meta").Where("id=1").Take(&meta).Error; err != nil {
			return ErrUnavailable
		}
		if !exists && meta.Scopes >= store.limits.Scopes {
			return ErrCapacity
		}
		// 只取旧载荷长度, 高频覆盖无需从 SQLite 读回并复制旧 Buffer.
		var old struct{ Size int64 }
		row := tx.Table("records").Select("length(value) AS size").Where("sector=? AND spectrum=? AND key=?", scope.Sector, scope.Spectrum, change.Key).Scan(&old)
		if row.Error != nil {
			return ErrUnavailable
		}
		previous := state.Bytes
		if row.RowsAffected != 0 {
			state.Records--
			state.Bytes -= int64(len(change.Key)) + old.Size
		}
		if !change.Erase {
			state.Records++
			state.Bytes += int64(len(change.Key) + len(change.Value))
		}
		if state.Records < 0 || state.Bytes < 0 {
			return ErrCorrupt
		}
		if state.Records > store.limits.Records || state.Bytes > store.limits.Bytes || meta.Bytes+state.Bytes-previous > store.limits.Total {
			return ErrCapacity
		}
		if !exists {
			if err := tx.Exec("INSERT INTO scopes(sector,spectrum,version,records,bytes,retained,backlog) VALUES(?,?,?,0,0,0,0)", scope.Sector, scope.Spectrum, Version(0)).Error; err != nil {
				return ErrUnavailable
			}
			meta.Scopes++
		}
		// 显式非 nil 空 BLOB 保留零字节 Set, 不借 ORM 的零值省略或 SQL NULL 表达 Delete.
		value := change.Value
		if value == nil {
			value = []byte{}
		}
		if change.Erase {
			err = tx.Exec("DELETE FROM records WHERE sector=? AND spectrum=? AND key=?", scope.Sector, scope.Spectrum, change.Key).Error
		} else {
			err = tx.Exec("INSERT INTO records(sector,spectrum,key,value) VALUES(?,?,?,?) ON CONFLICT(sector,spectrum,key) DO UPDATE SET value=excluded.value", scope.Sector, scope.Spectrum, change.Key, value).Error
		}
		if err != nil {
			return ErrUnavailable
		}
		if err = store.retain(tx, scope, change, value, &state); err != nil {
			return err
		}
		result := tx.Exec("UPDATE scopes SET version=?,records=?,bytes=?,retained=?,backlog=? WHERE sector=? AND spectrum=? AND version=?", change.Version, state.Records, state.Bytes, state.Retained, state.Backlog, scope.Sector, scope.Spectrum, state.Version)
		if result.Error != nil || result.RowsAffected != 1 {
			return ErrVersion
		}
		result = tx.Exec("UPDATE meta SET scopes=?,bytes=? WHERE id=1 AND scopes=? AND bytes=?", meta.Scopes, meta.Bytes+state.Bytes-previous, meta.Scopes-boolint(!exists), meta.Bytes)
		if result.Error != nil || result.RowsAffected != 1 {
			return ErrCorrupt
		}
		applied = true
		return nil
	})
	if err != nil {
		return 0, err
	}
	if applied {
		// 所有目标共享一次状态通知, 唤醒后从持久历史取连续提交, 不按 Star 复制事件队列.
		store.notify.Lock()
		for feed := range store.feeds {
			feed.push(event)
		}
		close(store.changed)
		store.changed = make(chan struct{})
		store.notify.Unlock()
	}
	return change.Version, nil
}

// current 区分合法未创建的零版本范围和损坏/失败的查询, 不由读取偷建 Scope.
func current(tx *gorm.DB, scope Scope) (group, bool, error) {
	var state group
	err := tx.Table("scopes").Where("sector=? AND spectrum=?", scope.Sector, scope.Spectrum).Take(&state).Error
	if errors.Is(err, gorm.ErrRecordNotFound) {
		return group{}, false, nil
	}
	if err != nil {
		return group{}, false, ErrUnavailable
	}
	return state, true, nil
}

// confirm 只用仍持久保留的原提交正文确认, 不从当前最终值倒推旧请求已成功.
func confirm(tx *gorm.DB, scope Scope, expected Change) error {
	var actual Change
	err := tx.Table("history").Where("sector=? AND spectrum=? AND version=?", scope.Sector, scope.Spectrum, expected.Version).Take(&actual).Error
	if errors.Is(err, gorm.ErrRecordNotFound) {
		return ErrUncertain
	}
	if err != nil {
		return ErrUnavailable
	}
	if actual.Key != expected.Key || actual.Erase != expected.Erase || !bytes.Equal(actual.Value, expected.Value) {
		return ErrVersion
	}
	return nil
}

// retain 同事务裁剪连续历史前缀; 单项放不下时清空历史, 仍允许合法权威提交生效.
func (store *Store) retain(tx *gorm.DB, scope Scope, change Change, value []byte, state *group) error {
	cost := int64(64 + len(change.Key) + len(value))
	if store.limits.History == 0 || cost > store.limits.Backlog {
		if err := tx.Exec("DELETE FROM history WHERE sector=? AND spectrum=?", scope.Sector, scope.Spectrum).Error; err != nil {
			return ErrUnavailable
		}
		state.Retained, state.Backlog = 0, 0
		return nil
	}
	if err := tx.Exec("INSERT INTO history(sector,spectrum,version,key,value,erase,cost) VALUES(?,?,?,?,?,?,?)", scope.Sector, scope.Spectrum, change.Version, change.Key, value, change.Erase, cost).Error; err != nil {
		return ErrUnavailable
	}
	state.Retained++
	state.Backlog += cost
	if state.Retained <= store.limits.History && state.Backlog <= store.limits.Backlog {
		return nil
	}
	// 沿既有主键只消费需要淘汰的前缀, 不在稳态每次提交都分配/复制整段历史元数据.
	rows, err := tx.Table("history").Select("version,cost").Where("sector=? AND spectrum=?", scope.Sector, scope.Spectrum).Order("version").Limit(int(state.Retained)).Rows()
	if err != nil {
		return ErrUnavailable
	}
	defer rows.Close() // 扫描失败仍释放查询; 正常路径在 DELETE 前显式关闭, 不让活动游标跨过同表写入.
	var through Version
	for (state.Retained > store.limits.History || state.Backlog > store.limits.Backlog) && rows.Next() {
		var version Version
		var cost int64
		if err := rows.Scan(&version, &cost); err != nil {
			return ErrUnavailable
		}
		state.Retained--
		state.Backlog -= cost
		through = version
	}
	if rows.Err() != nil {
		return ErrUnavailable
	}
	if err := rows.Close(); err != nil {
		return ErrUnavailable
	}
	if through == 0 || state.Retained > store.limits.History || state.Backlog < 0 || state.Backlog > store.limits.Backlog {
		return ErrCorrupt
	}
	if err := tx.Exec("DELETE FROM history WHERE sector=? AND spectrum=? AND version<=?", scope.Sector, scope.Spectrum, through).Error; err != nil {
		return ErrUnavailable
	}
	return nil
}

// boolint 只将本次是否新增范围转换成数据库计数增量.
func boolint(value bool) int64 {
	if value {
		return 1
	}
	return 0
}
