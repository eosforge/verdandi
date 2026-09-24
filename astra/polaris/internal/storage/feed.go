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
	mutex   sync.Mutex    // 保护队列与状态, 与 store.notify 的嵌套顺序固定.
	store   *Store        // 所属库, 关闭时遍历结束所有 Feed.
	queue   []*Event      // 待提取有序后缀, Take 后清空转交.
	bytes   int64         // 队列字节, 与 limit 比较判定超限.
	limit   int64         // 本目标正文预算, 构造时确定.
	items   int           // 本目标条数上限, 防微小消息元数据膨胀.
	changed chan struct{} // 就绪通知, 关闭表示有新事件或终止.
	err     error         // 终止原因, 置位后所有操作返回它.
}

// Subscribe 必须先于本轮清单/快照读取, 返回后所有新持久提交都进入有界后缀.
// limit 为该目标待发送正文预算, items 限制微小消息元数据; limit 至多 16 MiB, items 至多 65536.
// 返回订阅句柄, 订阅数与总预算受控, 超限返回容量错误.
func (store *Store) Subscribe(limit int64, items int) (*Feed, error) {
	// 参数有界校验, 不接受零或超大预算.
	if limit < 1 || limit > 16<<20 || items < 1 || items > 65536 {
		return nil, ErrInput
	}
	// 读锁防关闭中订阅.
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
	// 先从库集合摘除并归还预算, 再结束本 Feed, 顺序固定防死锁.
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
// 返回就绪通道, 关闭表示有新事件或终止, 调用方随后用 Take 提取.
func (feed *Feed) Ready() <-chan struct{} {
	feed.mutex.Lock()
	defer feed.mutex.Unlock()
	return feed.changed
}

// Take 转交当前完整有序后缀的引用, 消费者应先发完才再次提取, 不继续排队多份在途批次.
// 已返回事件的预算由服务发送层持有, Feed 的配额只约束尚未提取的后缀.
// 返回有序事件, 终止后返回终止原因.
func (feed *Feed) Take() ([]*Event, error) {
	feed.mutex.Lock()
	defer feed.mutex.Unlock()
	if feed.err != nil {
		return nil, feed.err
	}
	// 转交队列引用并清空, 预算清零, 新事件重新累积.
	result := feed.queue
	feed.queue = nil
	feed.bytes = 0
	return result, nil
}

// push 仅由持久成功通知持有 store.notify 时调用, 不在磁盘事务中通知网络.
// event 为已持久事件; 超限即终止本 Feed, 首事件到达即唤醒等待者.
func (feed *Feed) push(event *Event) {
	feed.mutex.Lock()
	defer feed.mutex.Unlock()
	if feed.err != nil {
		return
	}
	cost := int64(128 + len(event.Sector) + len(event.Spectrum) + len(event.Key) + len(event.Value))
	// 条数或字节超限即终止, 不阻塞权威提交.
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
// err 为终止原因, 仅首次生效.
func (feed *Feed) stop(err error) {
	if feed.err == nil {
		feed.err = err
		feed.queue = nil
		feed.bytes = 0
		close(feed.changed)
	}
}
