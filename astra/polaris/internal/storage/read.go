package storage

import (
	"context"

	"gorm.io/gorm"
)

// readOnly 限制 SQL 生命周期, 完整载荷准备结束后才返回, 网络发送不得进入本闭包.
func (store *Store) readOnly(ctx context.Context, read func(*gorm.DB) error) error {
	store.life.RLock()
	defer store.life.RUnlock()
	if store.closed {
		return ErrClosed
	}
	ctx, cancel := context.WithTimeout(ctx, store.limits.Timeout)
	defer cancel()
	if err := store.maintain(ctx); err != nil {
		return err
	}
	return store.read.WithContext(ctx).Transaction(read)
}

// List 在一个读事务捕获已持久存在的 Scope/版本清单, 不复制全库载荷.
func (store *Store) List(ctx context.Context) (positions []Position, err error) {
	err = store.readOnly(ctx, func(tx *gorm.DB) error {
		if err := tx.Table("scopes").Select("sector,spectrum,version").Order("sector,spectrum").Limit(int(store.limits.Scopes) + 1).Find(&positions).Error; err != nil {
			return ErrUnavailable
		}
		if int64(len(positions)) > store.limits.Scopes {
			return ErrCapacity
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	return positions, nil
}

// Version 仅查询持久位置, 不为管理读取或快照共享预先装入整个 Scope 的内容.
func (store *Store) Version(ctx context.Context, scope Scope) (version Version, err error) {
	if !scope.Valid() {
		return 0, ErrInput
	}
	err = store.readOnly(ctx, func(tx *gorm.DB) error {
		state, _, err := current(tx, scope)
		version = state.Version
		return err
	})
	return version, err
}

// Load 捕获一个 Scope 的完整一致内容, 合法未创建范围返回版本零空基线.
func (store *Store) Load(ctx context.Context, scope Scope) (snapshot Snapshot, err error) {
	if !scope.Valid() {
		return Snapshot{}, ErrInput
	}
	err = store.readOnly(ctx, func(tx *gorm.DB) error {
		snapshot, err = store.snapshot(tx, scope)
		return err
	})
	if err != nil {
		return Snapshot{}, err
	}
	return snapshot, nil
}

// snapshot 必须在已取得的读事务中使用, Scope 版本及所有记录不会跨 SQL 快照拼接.
func (store *Store) snapshot(tx *gorm.DB, scope Scope) (Snapshot, error) {
	state, _, err := current(tx, scope)
	if err != nil {
		return Snapshot{}, err
	}
	result := Snapshot{Position: Position{Scope: scope, Version: state.Version}}
	if state.Records > store.limits.Records || state.Bytes > store.limits.Bytes {
		return Snapshot{}, ErrCapacity
	}
	// SQL Rows 逐条受计费限制, 避免坏库仅伪造小计数就让 Find 分配任意多的大载荷.
	rows, err := tx.Table("records").Select("key,value").Where("sector=? AND spectrum=?", scope.Sector, scope.Spectrum).Order("key").Limit(int(state.Records) + 1).Rows()
	if err != nil {
		return Snapshot{}, ErrUnavailable
	}
	defer rows.Close()
	var total int64
	for rows.Next() {
		var record Record
		if err := rows.Scan(&record.Key, &record.Value); err != nil || !text(record.Key, 1024) || record.Value == nil || len(record.Value) > 1<<20 {
			return Snapshot{}, ErrCorrupt
		}
		total += int64(len(record.Key) + len(record.Value))
		if total > state.Bytes || int64(len(result.Records)) >= state.Records {
			return Snapshot{}, ErrCorrupt
		}
		result.Records = append(result.Records, record)
	}
	if rows.Err() != nil {
		return Snapshot{}, ErrUnavailable
	}
	if total != state.Bytes || int64(len(result.Records)) != state.Records {
		return Snapshot{}, ErrCorrupt
	}
	return result, nil
}

// Since 获取完整连续后缀, 超前游标不是空成功; 调用方在 ErrHistory 时改取完整快照.
func (store *Store) Since(ctx context.Context, scope Scope, after Version) (replay Replay, err error) {
	if !scope.Valid() {
		return Replay{}, ErrInput
	}
	err = store.readOnly(ctx, func(tx *gorm.DB) error {
		state, _, err := current(tx, scope)
		if err != nil {
			return err
		}
		replay, err = store.history(tx, scope, state, after)
		return err
	})
	if err != nil {
		return Replay{}, err
	}
	return replay, nil
}

// history 在同一 SQL 快照内验证窗口与连续性, 单次返回受历史条数/字节预算约束.
func (store *Store) history(tx *gorm.DB, scope Scope, state group, after Version) (Replay, error) {
	if after > state.Version {
		return Replay{}, ErrVersion
	}
	if state.Retained < 0 || uint64(state.Retained) > uint64(state.Version) || state.Retained > store.limits.History || state.Backlog < 0 || state.Backlog > store.limits.Backlog {
		return Replay{}, ErrCorrupt
	}
	if after < state.Version-Version(state.Retained) {
		return Replay{}, ErrHistory
	}
	result := Replay{Version: state.Version}
	rows, err := tx.Table("history").Select("version,key,value,erase,cost").Where("sector=? AND spectrum=? AND version>?", scope.Sector, scope.Spectrum, after).Order("version").Limit(int(state.Retained) + 1).Rows()
	if err != nil {
		return Replay{}, ErrUnavailable
	}
	defer rows.Close()
	position := after
	var total int64
	for rows.Next() {
		var change Change
		var cost int64
		if err := rows.Scan(&change.Version, &change.Key, &change.Value, &change.Erase, &cost); err != nil || position == state.Version || change.Version != position+1 || !text(change.Key, 1024) || change.Value == nil || len(change.Value) > 1<<20 || change.Erase && len(change.Value) != 0 || cost != int64(64+len(change.Key)+len(change.Value)) {
			return Replay{}, ErrCorrupt
		}
		total += cost
		if total > state.Backlog {
			return Replay{}, ErrCorrupt
		}
		position = change.Version
		result.Changes = append(result.Changes, change)
	}
	if rows.Err() != nil {
		return Replay{}, ErrUnavailable
	}
	if position != state.Version {
		return Replay{}, ErrCorrupt
	}
	return result, nil
}
