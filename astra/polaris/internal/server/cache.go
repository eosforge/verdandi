package server

import (
	"context"
	"sync"

	"github.com/eosforge/verdandi/astra/polaris/internal/storage"
)

// cache 仅保留正在准备/发送的快照, 无引用后立即解除拥有关系, 不长期复制第二份权威库.
type cache struct {
	mutex   sync.Mutex
	store   *storage.Store
	entries map[position]*prepared
	used    int64
	maximum int64
	reserve int64
}

// position 表示请求的最低版本, 并发相同目标复用一次有界 SQL 准备.
type position struct {
	storage.Scope
	version storage.Version
}

// prepared 的 snapshot/error 在关闭 ready 后只读; references 和预算仅在 cache.mutex 下修改.
type prepared struct {
	ready      chan struct{}
	snapshot   storage.Snapshot
	err        error
	references int
}

// pin 明确持有一份只读快照及预算责任, release 幂等且不能提前于网络实际使用完成.
type pin struct {
	Snapshot *storage.Snapshot
	cache    *cache
	key      position
	entry    *prepared
	once     sync.Once
}

// newCache 为每次完整准备预留最大范围、记录索引和一页发送空间, 不读取数据库统计后赌范围不增长.
func newCache(store *storage.Store, limits storage.Limits, maximum int64) *cache {
	return &cache{store: store, entries: make(map[position]*prepared), maximum: maximum, reserve: limits.Bytes + limits.Records*128 + 2<<20}
}

// acquire 的返回快照至少为 minimum, 每次 SQL 事务完成后才向调用方发布载荷.
func (cache *cache) acquire(ctx context.Context, scope storage.Scope, minimum storage.Version) (*pin, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	key := position{Scope: scope, version: minimum}
	cache.mutex.Lock()
	entry := cache.entries[key]
	create := entry == nil
	if create {
		if cache.reserve > cache.maximum-cache.used {
			cache.mutex.Unlock()
			return nil, storage.ErrCapacity
		}
		entry = &prepared{ready: make(chan struct{})}
		cache.entries[key] = entry
		cache.used += cache.reserve
	}
	entry.references++
	cache.mutex.Unlock()
	result := &pin{cache: cache, key: key, entry: entry}
	if create {
		entry.snapshot, entry.err = cache.store.Load(ctx, scope)
		if entry.err == nil && entry.snapshot.Version < minimum {
			entry.err = storage.ErrVersion
		}
		close(entry.ready)
	}
	select {
	case <-ctx.Done():
		result.release()
		return nil, ctx.Err()
	case <-entry.ready:
		if err := ctx.Err(); err != nil {
			result.release()
			return nil, err
		}
		if entry.err != nil {
			result.release()
			return nil, entry.err
		}
		result.Snapshot = &entry.snapshot
		return result, nil
	}
}

// release 只操作引用计数, 从 cache 移除最后引用后不继续缓存已无人使用的旧版本.
func (pin *pin) release() {
	pin.once.Do(func() {
		pin.cache.mutex.Lock()
		defer pin.cache.mutex.Unlock()
		pin.entry.references--
		if pin.entry.references == 0 {
			delete(pin.cache.entries, pin.key)
			pin.cache.used -= pin.cache.reserve
		}
	})
}
