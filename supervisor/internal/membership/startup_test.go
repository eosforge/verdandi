// 验证服务端持久幂等索引, 并发, 容量及失败后的原子性.
package membership

import (
	"errors"
	bolt "go.etcd.io/bbolt"
	"path/filepath"
	"sync"
	"testing"
)

func TestRetiredStartupRemainsRejectedAfterSupervisorRestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "members.db")
	store, err := Open(path, 1, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	first := candidate(1)
	if _, err := store.Register("alpha", first, startup(first)); err != nil {
		t.Fatal(err)
	}
	latest := first
	// 多次替换后仍拒绝最早请求, 不是只保留上一次 ID 的两槽缓存.
	for index := 2; index <= 20; index++ {
		latest.ID = candidate(index).ID
		if _, err := store.Register("alpha", latest, startup(latest)); err != nil {
			t.Fatal(err)
		}
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = Open(path, 1, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.Register("alpha", first, startup(first)); !errors.Is(err, ErrConflict) {
		t.Fatal("retired request returned", err)
	}
	retry := latest
	retry.ID = "unused-newly-generated-id"
	values, err := store.Register("alpha", retry, startup(latest))
	if err != nil || values[0].ID != latest.ID || values[0].Epoch != 20 {
		t.Fatal("retry changed committed result", values, err)
	}
}

func TestStartupCapacityDoesNotEvictOrBlockCurrentRetry(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "members.db"), 1, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	store.maximumStarts = 2
	first := candidate(1)
	next := first
	next.ID = candidate(2).ID
	for _, m := range []Member{first, next} {
		if _, err := store.Register("alpha", m, startup(m)); err != nil {
			t.Fatal(err)
		}
	}
	extra := first
	extra.ID = candidate(3).ID
	if _, err := store.Register("alpha", extra, startup(extra)); !errors.Is(err, ErrCapacity) {
		t.Fatal("startup budget ignored", err)
	}
	if _, err := store.Register("alpha", first, startup(first)); !errors.Is(err, ErrConflict) {
		t.Fatal("old request resurrected at capacity", err)
	}
	values, err := store.Register("alpha", next, startup(next))
	if err != nil || values[0].ID != next.ID || values[0].Epoch != 2 {
		t.Fatal("capacity broke existing member", err)
	}
}

func TestConcurrentDuplicateStartupCommitsOnlyOnce(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "members.db"), 1, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	var workers sync.WaitGroup
	results := make(chan []Member, 16)
	failures := make(chan error, 16)
	for index := range 16 {
		workers.Go(func() {
			m := candidate(1)
			m.ID = candidate(index + 1).ID
			result, err := store.Register("alpha", m, startup(candidate(1)))
			results <- result
			failures <- err
		})
	}
	workers.Wait()
	close(results)
	close(failures)
	for err := range failures {
		if err != nil {
			t.Fatal(err)
		}
	}
	id := ""
	for values := range results {
		if len(values) != 1 || values[0].Epoch != 1 {
			t.Fatal("duplicate committed twice", values)
		}
		if id == "" {
			id = values[0].ID
		} else if id != values[0].ID {
			t.Fatal("duplicate received different identities")
		}
	}
}

func TestCorruptStartupIndexFailsClosed(t *testing.T) {
	for _, kind := range []string{"length", "future-epoch", "counter", "nested"} {
		t.Run(kind, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "members.db")
			store, err := Open(path, 1, DefaultMaximumStarts)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := store.Register("alpha", candidate(1), startup(candidate(1))); err != nil {
				t.Fatal(err)
			}
			if err := store.db.Update(func(tx *bolt.Tx) error {
				starts := tx.Bucket([]byte("alpha")).Bucket(startBucket)
				switch kind {
				case "length":
					return starts.Put(startup(candidate(1)), []byte{1})
				case "counter":
					return starts.SetSequence(2)
				case "nested":
					_, err := starts.CreateBucket([]byte("invalid"))
					return err
				default:
					value := append([]byte(nil), starts.Get(startup(candidate(1)))...)
					value[39] = 2
					return starts.Put(startup(candidate(1)), value)
				}
			}); err != nil {
				t.Fatal(err)
			}
			if err := store.Close(); err != nil {
				t.Fatal(err)
			}
			reopened, err := Open(path, 1, DefaultMaximumStarts)
			if reopened != nil {
				reopened.Close()
			}
			if !errors.Is(err, ErrInvalid) {
				t.Fatal("corrupt startup index accepted", err)
			}
		})
	}
}
