//go:build linux && cgo

package storage

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"math"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"gorm.io/gorm"
)

// fixture 为每例创建独立库和锁文件, 不使用共享端口、全局数据库或外部服务.
func fixture(t *testing.T, limits Limits) (*Store, string, Binding) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "polaris.db")
	binding := Binding{Galaxy: "test", Username: "polaris", Advertise: "127.0.0.1:45101"}
	store, err := Open(t.Context(), path, binding, limits, true)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := store.Close(); err != nil {
			t.Error(err)
		}
	})
	return store, path, binding
}

// TestDatabasePath 验证恢复入口不跟随别名绕过按路径加锁, 拒绝失败不影响原库的活动连接.
func TestDatabasePath(t *testing.T) {
	t.Parallel()
	original, path, binding := fixture(t, Default())
	alias := filepath.Join(t.TempDir(), "alias.db")
	if err := os.Symlink(path, alias); err != nil {
		t.Fatal(err)
	}
	opened, err := Open(t.Context(), alias, binding, Default(), false)
	if opened != nil {
		opened.Close()
		t.Fatal("database symlink unexpectedly accepted")
	}
	if !errors.Is(err, ErrUnavailable) {
		t.Fatal("database symlink did not fail before SQLite access", err)
	}
	if _, err := original.Load(t.Context(), Scope{"routes", "main"}); err != nil {
		t.Fatal("rejected alias changed the original database", err)
	}
}

// requireCommit 同时核对持久回执版本, 不把 nil 错误当作唯一证据.
func requireCommit(t *testing.T, store *Store, scope Scope, change Change) {
	t.Helper()
	version, err := store.Commit(t.Context(), scope, change)
	if err != nil || version != change.Version {
		t.Fatalf("commit version=%d error=%v", version, err)
	}
}

// TestCommit 覆盖新范围、空值、无操作提交、重放依据与异内容冲突.
func TestCommit(t *testing.T) {
	t.Parallel()
	store, _, _ := fixture(t, Default())
	scope := Scope{"routes", "main"}
	before := store.Changed()
	snapshot, err := store.Load(t.Context(), scope)
	if err != nil || snapshot.Version != 0 || len(snapshot.Records) != 0 {
		t.Fatal(snapshot, err)
	}
	positions, err := store.List(t.Context())
	if err != nil || len(positions) != 0 {
		t.Fatal(positions, err)
	}
	requireCommit(t, store, scope, Change{Version: 1, Key: "empty"})
	select {
	case <-before:
	default:
		t.Fatal("commit did not notify")
	}
	snapshot, err = store.Load(t.Context(), scope)
	if err != nil || snapshot.Version != 1 || len(snapshot.Records) != 1 || snapshot.Records[0].Value == nil || len(snapshot.Records[0].Value) != 0 {
		t.Fatal(snapshot, err)
	}
	before = store.Changed()
	requireCommit(t, store, scope, Change{Version: 1, Key: "empty", Value: []byte{}})
	select {
	case <-before:
		t.Fatal("retry notified as new commit")
	default:
	}
	if _, err = store.Commit(t.Context(), scope, Change{Version: 1, Key: "empty", Erase: true}); !errors.Is(err, ErrVersion) {
		t.Fatal(err)
	}
	requireCommit(t, store, scope, Change{Version: 2, Key: "empty"})
	requireCommit(t, store, scope, Change{Version: 3, Key: "missing", Erase: true})
	requireCommit(t, store, scope, Change{Version: 4, Key: "empty", Erase: true})
	snapshot, err = store.Load(t.Context(), scope)
	if err != nil || snapshot.Version != 4 || len(snapshot.Records) != 0 {
		t.Fatal(snapshot, err)
	}
	replay, err := store.Since(t.Context(), scope, 0)
	if err != nil || replay.Version != 4 || len(replay.Changes) != 4 || !replay.Changes[3].Erase {
		t.Fatal(replay, err)
	}
	// 相同 Key 在独立 Scope 内的版本仍从 1 开始.
	requireCommit(t, store, Scope{"routes", "other"}, Change{Version: 1, Key: "empty", Value: []byte{7}})
}

// TestHistory 验证有限窗口、超大单项、零预算和后续连续后缀恢复.
func TestHistory(t *testing.T) {
	for _, mode := range []string{"count", "bytes", "zero"} {
		t.Run(mode, func(t *testing.T) {
			t.Parallel()
			limits := Default()
			limits.History = 2
			if mode == "bytes" {
				limits.Backlog = 66
			}
			if mode == "zero" {
				limits.History = 0
			}
			store, _, _ := fixture(t, limits)
			scope := Scope{"s", "p"}
			for version := Version(1); version <= 3; version++ {
				requireCommit(t, store, scope, Change{Version: version, Key: "k", Value: []byte{byte(version)}})
			}
			if _, err := store.Since(t.Context(), scope, 0); !errors.Is(err, ErrHistory) {
				t.Fatal(err)
			}
			if _, err := store.Commit(t.Context(), scope, Change{Version: 1, Key: "k", Value: []byte{1}}); !errors.Is(err, ErrUncertain) {
				t.Fatal(err)
			}
			if replay, err := store.Since(t.Context(), scope, 3); err != nil || len(replay.Changes) != 0 {
				t.Fatal(replay, err)
			}
			if _, err := store.Since(t.Context(), scope, 4); !errors.Is(err, ErrVersion) {
				t.Fatal(err)
			}
			if mode == "bytes" {
				requireCommit(t, store, scope, Change{Version: 4, Key: "k", Value: make([]byte, 512<<10)})
				if _, err := store.Since(t.Context(), scope, 3); !errors.Is(err, ErrHistory) {
					t.Fatal(err)
				}
				requireCommit(t, store, scope, Change{Version: 5, Key: "k", Value: []byte{5}})
				if replay, err := store.Since(t.Context(), scope, 4); err != nil || len(replay.Changes) != 1 || replay.Changes[0].Version != 5 {
					t.Fatal(replay, err)
				}
			}
		})
	}
}

// TestHistoryPrefix 验证只裁剪必要连续前缀, 同时覆盖条数/字节触发、多项淘汰及跨 Scope 隔离.
func TestHistoryPrefix(t *testing.T) {
	t.Parallel()
	limits := Default()
	limits.History, limits.Backlog = 4, 450
	store, _, _ := fixture(t, limits)
	scope := Scope{"s", "p"}
	other := Scope{"s", "other"}
	requireCommit(t, store, other, Change{Version: 1, Key: "k", Value: []byte{9}})
	for version := Version(1); version <= 4; version++ {
		requireCommit(t, store, scope, Change{Version: version, Key: "k", Value: []byte{byte(version)}})
	}
	// 单条计费为 315, 总计 579; 淘汰两条各 66 的前缀后剩余 447, 不能把整段历史清空.
	requireCommit(t, store, scope, Change{Version: 5, Key: "k", Value: make([]byte, 250)})
	for _, boundary := range []struct {
		since Version
		last  Version
	}{{2, 5}, {3, 6}} {
		if boundary.last == 6 {
			requireCommit(t, store, scope, Change{Version: 6, Key: "k", Value: []byte{6}})
		}
		if _, err := store.Since(t.Context(), scope, boundary.since-1); !errors.Is(err, ErrHistory) {
			t.Fatal("evicted prefix accepted", err)
		}
		replay, err := store.Since(t.Context(), scope, boundary.since)
		if err != nil || len(replay.Changes) != 3 {
			t.Fatal("retained suffix lost", replay, err)
		}
		for index, change := range replay.Changes {
			if change.Version != boundary.since+Version(index)+1 {
				t.Fatal("retained suffix is not continuous", replay)
			}
		}
	}
	if replay, err := store.Since(t.Context(), other, 0); err != nil || len(replay.Changes) != 1 || replay.Changes[0].Version != 1 {
		t.Fatal("other scope history changed", replay, err)
	}
}

// TestRestart 验证部署绑定、独占、显式初始化、持久版本及重启后的旧提交确认.
func TestRestart(t *testing.T) {
	t.Parallel()
	store, path, binding := fixture(t, Default())
	scope := Scope{"__auth", "comet"}
	change := Change{Version: 1, Key: "key", Value: []byte{1, 2}}
	requireCommit(t, store, scope, change)
	if duplicate, err := Open(t.Context(), path, binding, Default(), false); err == nil {
		duplicate.Close()
		t.Fatal("second owner accepted")
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	if duplicate, err := Open(t.Context(), path, binding, Default(), true); err == nil {
		duplicate.Close()
		t.Fatal("existing database overwritten")
	}
	wrong := binding
	wrong.Username = "other"
	if duplicate, err := Open(t.Context(), path, wrong, Default(), false); !errors.Is(err, ErrBinding) {
		if duplicate != nil {
			duplicate.Close()
		}
		t.Fatal(err)
	}
	reopened, err := Open(t.Context(), path, binding, Default(), false)
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	requireCommit(t, reopened, scope, change)
	snapshot, err := reopened.Load(t.Context(), scope)
	if err != nil || snapshot.Version != 1 || len(snapshot.Records) != 1 || !bytes.Equal(snapshot.Records[0].Value, change.Value) {
		t.Fatal(snapshot, err)
	}
}

// TestAtomicity 在内容已写、历史尚未写时注入 SQL 失败, 外部必须仍看到完整旧状态.
func TestAtomicity(t *testing.T) {
	t.Parallel()
	store, _, _ := fixture(t, Default())
	scope := Scope{"s", "p"}
	requireCommit(t, store, scope, Change{Version: 1, Key: "k", Value: []byte{1}})
	if err := store.write.Exec("CREATE TRIGGER fail_history BEFORE INSERT ON history BEGIN SELECT RAISE(ABORT,'injected'); END").Error; err != nil {
		t.Fatal(err)
	}
	if _, err := store.Commit(t.Context(), scope, Change{Version: 2, Key: "k", Value: []byte{2}}); err == nil {
		t.Fatal("failed transaction succeeded")
	}
	snapshot, err := store.Load(t.Context(), scope)
	if err != nil || snapshot.Version != 1 || !bytes.Equal(snapshot.Records[0].Value, []byte{1}) {
		t.Fatal(snapshot, err)
	}
	if replay, err := store.Since(t.Context(), scope, 0); err != nil || len(replay.Changes) != 1 {
		t.Fatal(replay, err)
	}
	if err := store.write.Exec("DROP TRIGGER fail_history").Error; err != nil {
		t.Fatal(err)
	}
	requireCommit(t, store, scope, Change{Version: 2, Key: "k", Value: []byte{2}})
}

// TestUncertain 用延迟外键错误命中 COMMIT 边界, 不把未知结果继续用作下一次写入基线.
func TestUncertain(t *testing.T) {
	t.Parallel()
	store, _, _ := fixture(t, Default())
	err := store.transaction(t.Context(), func(tx *gorm.DB) error {
		if err := tx.Exec("PRAGMA defer_foreign_keys=ON").Error; err != nil {
			return err
		}
		return tx.Exec("INSERT INTO records VALUES('missing','scope','key',X'01')").Error
	})
	if !errors.Is(err, ErrUncertain) || !store.poisoned.Load() {
		t.Fatal(err)
	}
	if _, err = store.Commit(t.Context(), Scope{"s", "p"}, Change{Version: 1, Key: "k"}); !errors.Is(err, ErrUncertain) {
		t.Fatal(err)
	}
}

// TestVersion 验证高位版本的字节序和非法 SQL 类型, 不依赖浮点或有符号转换.
func TestVersion(t *testing.T) {
	for _, input := range []uint64{0, 1, 1<<63 - 1, 1 << 63, math.MaxUint64} {
		var version Version
		if err := version.Scan(binary.BigEndian.AppendUint64(nil, input)); err != nil || uint64(version) != input {
			t.Fatal(version, err)
		}
		value, err := version.Value()
		if err != nil || !bytes.Equal(value.([]byte), binary.BigEndian.AppendUint64(nil, input)) {
			t.Fatal(value, err)
		}
	}
	for _, input := range []any{nil, int64(1), "00000000", []byte{1}, make([]byte, 9)} {
		version := Version(9)
		if err := version.Scan(input); !errors.Is(err, ErrCorrupt) || version != 9 {
			t.Fatal(version, err)
		}
	}
}

// TestExhaustion 在隔离库设置合法最高版本边界, 验证 SQLite 索引与重启不截断 uint64.
func TestExhaustion(t *testing.T) {
	t.Parallel()
	store, path, binding := fixture(t, Default())
	scope := Scope{"s", "p"}
	requireCommit(t, store, scope, Change{Version: 1, Key: "absent", Erase: true})
	if err := store.write.Exec("DELETE FROM history").Error; err != nil {
		t.Fatal(err)
	}
	if err := store.write.Exec("UPDATE scopes SET version=?,retained=0,backlog=0", Version(math.MaxUint64-1)).Error; err != nil {
		t.Fatal(err)
	}
	requireCommit(t, store, scope, Change{Version: Version(math.MaxUint64), Key: "k", Value: []byte{1}})
	if _, err := store.Commit(t.Context(), scope, Change{Version: 0, Key: "k"}); !errors.Is(err, ErrInput) {
		t.Fatal(err)
	}
	store.Close()
	reopened, err := Open(t.Context(), path, binding, Default(), false)
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	snapshot, err := reopened.Load(t.Context(), scope)
	if err != nil || snapshot.Version != Version(math.MaxUint64) {
		t.Fatal(snapshot, err)
	}
}

// TestLimits 检查输入、范围容量和失败不推进版本; Delete 仍释放活动容量.
func TestLimits(t *testing.T) {
	t.Parallel()
	limits := Default()
	limits.Scopes, limits.Records, limits.Bytes, limits.Total = 1, 1, 2, 2
	store, _, _ := fixture(t, limits)
	scope := Scope{"s", "p"}
	requireCommit(t, store, scope, Change{Version: 1, Key: "k", Value: []byte{1}})
	for _, change := range []Change{{Version: 2, Key: "k", Value: []byte{1, 2}}, {Version: 2, Key: "x"}} {
		if _, err := store.Commit(t.Context(), scope, change); !errors.Is(err, ErrCapacity) {
			t.Fatal(err)
		}
	}
	if _, err := store.Commit(t.Context(), Scope{"s", "new"}, Change{Version: 1, Key: "k"}); !errors.Is(err, ErrCapacity) {
		t.Fatal(err)
	}
	for _, change := range []Change{{Version: 0, Key: "k"}, {Version: 2, Key: ""}, {Version: 2, Key: "\xff"}, {Version: 2, Key: "a\x00b"}, {Version: 2, Key: "k", Value: make([]byte, 1<<20+1)}, {Version: 2, Key: "k", Erase: true, Value: []byte{1}}} {
		if _, err := store.Commit(t.Context(), scope, change); !errors.Is(err, ErrInput) {
			t.Fatal(err)
		}
	}
	requireCommit(t, store, scope, Change{Version: 2, Key: "k", Erase: true})
	requireCommit(t, store, scope, Change{Version: 3, Key: "x", Value: []byte{3}})
}

// TestCancellation 取消等待写入额度的请求, 不需要睡眠制造随机竞争.
func TestCancellation(t *testing.T) {
	t.Parallel()
	store, _, _ := fixture(t, Default())
	store.writer <- struct{}{}
	ctx, cancel := context.WithCancel(t.Context())
	cancel()
	if _, err := store.Commit(ctx, Scope{"s", "p"}, Change{Version: 1, Key: "k"}); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	<-store.writer
	requireCommit(t, store, Scope{"s", "p"}, Change{Version: 1, Key: "k"})
	store.Close()
	if _, err := store.Load(t.Context(), Scope{"s", "p"}); !errors.Is(err, ErrClosed) {
		t.Fatal(err)
	}
}

// TestConcurrent 同一预期版本的不同内容至多提交一次, 快照与回放均对应胜出的版本.
func TestConcurrent(t *testing.T) {
	t.Parallel()
	store, _, _ := fixture(t, Default())
	scope := Scope{"s", "p"}
	var work sync.WaitGroup
	results := make(chan error, 16)
	for index := range 16 {
		work.Go(func() {
			_, err := store.Commit(t.Context(), scope, Change{Version: 1, Key: "k", Value: []byte{byte(index)}})
			results <- err
		})
	}
	work.Wait()
	close(results)
	success := 0
	for err := range results {
		if err == nil {
			success++
		} else if !errors.Is(err, ErrVersion) {
			t.Fatal(err)
		}
	}
	if success != 1 {
		t.Fatal("successful writers", success)
	}
	snapshot, err := store.Load(t.Context(), scope)
	if err != nil || snapshot.Version != 1 || len(snapshot.Records) != 1 {
		t.Fatal(snapshot, err)
	}
}

// TestInvalidDatabase 拒绝缺库、未知格式和非法部署, 不自动创建或清空权威数据.
func TestInvalidDatabase(t *testing.T) {
	t.Parallel()
	path := filepath.Join(t.TempDir(), "absent.db")
	binding := Binding{"g", "p", "127.0.0.1:45102"}
	if store, err := Open(t.Context(), path, binding, Default(), false); err == nil {
		store.Close()
		t.Fatal("missing database accepted")
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("missing database created", err)
	}
	if err := os.WriteFile(path, []byte("not a sqlite database"), 0600); err != nil {
		t.Fatal(err)
	}
	if store, err := Open(t.Context(), path, binding, Default(), false); err == nil {
		store.Close()
		t.Fatal("invalid database accepted")
	}
	limits := Default()
	limits.Timeout = 31 * time.Second
	if store, err := Open(t.Context(), path, binding, limits, false); !errors.Is(err, ErrInput) {
		if store != nil {
			store.Close()
		}
		t.Fatal(err)
	}
}
