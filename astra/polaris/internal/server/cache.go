package server

import (
	"context"
	"sync"

	"github.com/eosforge/verdandi/astra/polaris/internal/storage"
)

// cache 仅保留正在准备/发送的快照, 无引用后立即解除拥有关系, 不长期复制第二份权威库.
type cache struct {
	mutex   sync.Mutex             // 保护条目表与预算, 准备 SQL 在锁外执行.
	store   *storage.Store         // 权威持久库, 未命中时加载快照.
	entries map[position]*prepared // 在途准备表, 键为范围+最低版本, 相同目标复用一次准备.
	used    int64                  // 已占共享预算, 按预留下限计量.
	maximum int64                  // 共享预算上限, 超限拒绝新准备.
	reserve int64                  // 单次准备预留 (最大范围+索引+一页), 不按实际大小精算.
}

// position 表示请求的最低版本, 并发相同目标复用一次有界 SQL 准备.
type position struct {
	storage.Scope                 // 快照范围.
	version       storage.Version // 请求的最低版本, 低于此版本的快照不可用.
}

// prepared 的 snapshot/error 在关闭 ready 后只读; references 和预算仅在 cache.mutex 下修改.
type prepared struct {
	ready      chan struct{}    // 准备完成信号, 关闭后快照/错误只读.
	snapshot   storage.Snapshot // 冻结快照, 就绪后不可变.
	err        error            // 准备错误, 就绪后只读.
	references int              // 引用计数, 归零即移除条目并归还预算.
}

// pin 明确持有一份只读快照及预算责任, release 幂等且不能提前于网络实际使用完成.
type pin struct {
	Snapshot *storage.Snapshot // 钉住的只读快照, 释放前有效.
	cache    *cache            // 所属缓存, 释放时归还引用.
	key      position          // 条目键, 释放时定位条目.
	entry    *prepared         // 条目指针, 释放时减引用.
	once     sync.Once         // 保证释放幂等, 重复调用无副作用.
}

// newCache 为每次完整准备预留最大范围、记录索引和一页发送空间, 不读取数据库统计后赌范围不增长.
// store/limits 为依赖与限额; maximum 为共享预算上限; reserve 按最大范围预留.
// 返回快照缓存, 预算不足的新准备会被拒绝.
func newCache(store *storage.Store, limits storage.Limits, maximum int64) *cache {
	return &cache{store: store, entries: make(map[position]*prepared), maximum: maximum, reserve: limits.Bytes + limits.Records*128 + 2<<20}
}

// acquire 的返回快照至少为 minimum, 每次 SQL 事务完成后才向调用方发布载荷.
// ctx/scope/minimum 为上下文、范围与最低版本; 返回钉住快照, 失败返回错误.
// 并发相同目标复用一次准备, 预算不足或上下文取消即失败.
func (cache *cache) acquire(ctx context.Context, scope storage.Scope, minimum storage.Version) (*pin, error) {
	// 上下文已取消直接返回, 不占条目.
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	key := position{Scope: scope, version: minimum}
	// 锁内查找或创建条目: 命中则复用, 未命中则预占预算后创建, 预算不足直接拒绝.
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
		// 首个请求者在锁外执行 SQL 加载, 完成后关闭 ready 发布结果.
		entry.snapshot, entry.err = cache.store.Load(ctx, scope)
		if entry.err == nil && entry.snapshot.Version < minimum {
			entry.err = storage.ErrVersion
		}
		close(entry.ready)
	}
	// 等待准备完成或上下文取消, 任何失败路径都释放本次引用.
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
// 幂等, 可重复调用.
func (pin *pin) release() {
	pin.once.Do(func() {
		pin.cache.mutex.Lock()
		defer pin.cache.mutex.Unlock()
		pin.entry.references--
		// 最后引用释放时移除条目并归还预算, 不保留旧快照.
		if pin.entry.references == 0 {
			delete(pin.cache.entries, pin.key)
			pin.cache.used -= pin.cache.reserve
		}
	})
}
