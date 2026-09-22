package storage

import (
	"sync"
)

// Event 是已持久提交的单条完整变更, 由 Feed 只读借出, 不允许调用方修改正文.
// 同一 Event 可被多个 Star 共享, Scope/版本顺序与持久提交一致.
type Event struct {
	Scope
	Change
}

// Feed 为正在初始同步/发送的目标保留有界提交后缀, 防止只依赖 SQLite 历史窗口碰运气.
// 超限直接结束该 Feed, 不阻止权威提交或无限固定历史; 消费者重建完整基线.
type Feed struct {
	mutex   sync.Mutex
	store   *Store
	queue   []*Event
	bytes   int64
	limit   int64
	items   int
	changed chan struct{}
	err     error
}

// Subscribe 必须先于本轮清单/快照读取, 返回后所有新持久提交都进入有界后缀.
// limit 为该目标待发送正文预算, items 限制微小消息元数据; limit 至多 16 MiB, items 至多 65536.
func (store *Store) Subscribe(limit int64, items int) (*Feed, error) {
	if limit < 1 || limit > 16<<20 || items < 1 || items > 65536 {
		return nil, ErrInput
	}
	store.life.RLock()
	defer store.life.RUnlock()
	if store.closed {
		return nil, ErrClosed
	}
	store.notify.Lock()
	defer store.notify.Unlock()
	// 订阅数和所有订阅的最大预算一起受控, 不因连接数乘法制造无界复制后缀.
	if len(store.feeds) >= 128 || limit > (128<<20)-store.feedBytes {
		return nil, ErrCapacity
	}
	feed := &Feed{store: store, limit: limit, items: items, changed: make(chan struct{})}
	store.feeds[feed] = struct{}{}
	store.feedBytes += limit
	return feed, nil
}

// Close 解除本 Feed 的归属和预算, 幂等; store.notify -> feed.mutex 是唯一嵌套锁顺序.
func (feed *Feed) Close() {
	feed.store.notify.Lock()
	defer feed.store.notify.Unlock()
	if _, exists := feed.store.feeds[feed]; exists {
		delete(feed.store.feeds, feed)
		feed.store.feedBytes -= feed.limit
	}
	feed.mutex.Lock()
	defer feed.mutex.Unlock()
	feed.stop(ErrClosed)
}

// Ready 在消费循环取数据之前捕获通知边界, 通知发生在两次操作之间也不会漏掉新提交.
func (feed *Feed) Ready() <-chan struct{} {
	feed.mutex.Lock()
	defer feed.mutex.Unlock()
	return feed.changed
}

// Take 转交当前完整有序后缀的引用, 消费者应先发完才再次提取, 不继续排队多份在途批次.
// 已返回事件的预算由服务发送层持有, Feed 的配额只约束尚未提取的后缀.
func (feed *Feed) Take() ([]*Event, error) {
	feed.mutex.Lock()
	defer feed.mutex.Unlock()
	if feed.err != nil {
		return nil, feed.err
	}
	result := feed.queue
	feed.queue = nil
	feed.bytes = 0
	return result, nil
}

// push 仅由持久成功通知持有 store.notify 时调用, 不在磁盘事务中通知网络.
func (feed *Feed) push(event *Event) {
	feed.mutex.Lock()
	defer feed.mutex.Unlock()
	if feed.err != nil {
		return
	}
	cost := int64(128 + len(event.Sector) + len(event.Spectrum) + len(event.Key) + len(event.Value))
	if len(feed.queue) == feed.items || cost > feed.limit-feed.bytes {
		feed.stop(ErrCapacity)
		return
	}
	feed.queue = append(feed.queue, event)
	feed.bytes += cost
	if len(feed.queue) == 1 {
		close(feed.changed)
		feed.changed = make(chan struct{})
	}
}

// stop 已持有 feed.mutex, 首次失败释放尚未交付正文并唤醒等待者, 不覆盖原失败原因.
func (feed *Feed) stop(err error) {
	if feed.err == nil {
		feed.err = err
		feed.queue = nil
		feed.bytes = 0
		close(feed.changed)
	}
}
