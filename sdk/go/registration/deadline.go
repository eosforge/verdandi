package registration

// deadlineItem 是最小堆中的一个 UUID 截止项。
type deadlineItem struct {
	uuid     string
	deadline uint64
}

// deadlineQueue 通过最小堆和 UUID 索引提供 O(log N) 更新、删除与到期弹出。
type deadlineQueue struct {
	items   []deadlineItem
	indices map[string]int
}

// newDeadlineQueue 创建最多预分配 capacity 项的空截止队列。
func newDeadlineQueue(capacity int) deadlineQueue {
	return deadlineQueue{
		items:   make([]deadlineItem, 0, capacity),
		indices: make(map[string]int, capacity),
	}
}

// set 插入或更新 uuid 的 deadline，并只沿可能破坏堆序的方向修复。
func (queue *deadlineQueue) set(uuid string, deadline uint64) {
	if index, exists := queue.indices[uuid]; exists {
		previous := queue.items[index].deadline
		queue.items[index].deadline = deadline
		if deadline < previous {
			queue.up(index)
		} else if deadline > previous {
			queue.down(index)
		}
		return
	}
	index := len(queue.items)
	queue.items = append(queue.items, deadlineItem{uuid: uuid, deadline: deadline})
	queue.indices[uuid] = index
	queue.up(index)
}

// remove 删除 uuid 并报告它是否存在；尾项填零以尽早释放字符串引用。
func (queue *deadlineQueue) remove(uuid string) bool {
	index, exists := queue.indices[uuid]
	if !exists {
		return false
	}
	last := len(queue.items) - 1
	delete(queue.indices, uuid)
	if index == last {
		queue.items[last] = deadlineItem{}
		queue.items = queue.items[:last]
		return true
	}
	// 用尾项填洞，再根据新值与父子关系选择向下或向上修复。
	queue.items[index] = queue.items[last]
	queue.items[last] = deadlineItem{}
	queue.items = queue.items[:last]
	queue.indices[queue.items[index].uuid] = index
	if !queue.down(index) {
		queue.up(index)
	}
	return true
}

// expire 在最早截止时间不晚于 now 时删除并返回对应 UUID。
func (queue *deadlineQueue) expire(now uint64) (string, bool) {
	if len(queue.items) == 0 || queue.items[0].deadline > now {
		return "", false
	}
	uuid := queue.items[0].uuid
	queue.remove(uuid)
	return uuid, true
}

// pop 无条件删除并返回当前最早项；空队列返回 false。
func (queue *deadlineQueue) pop() (string, bool) {
	if len(queue.items) == 0 {
		return "", false
	}
	uuid := queue.items[0].uuid
	queue.remove(uuid)
	return uuid, true
}

// next 只查看当前最早截止时间，不修改队列。
func (queue *deadlineQueue) next() (uint64, bool) {
	if len(queue.items) == 0 {
		return 0, false
	}
	return queue.items[0].deadline, true
}

// up 从 index 向父节点修复最小堆顺序。
func (queue *deadlineQueue) up(index int) {
	for index > 0 {
		parent := (index - 1) / 2
		if !queue.less(index, parent) {
			return
		}
		queue.swap(index, parent)
		index = parent
	}
}

// down 从 index 向较小子节点修复最小堆，并报告元素是否移动。
func (queue *deadlineQueue) down(index int) bool {
	start := index
	for {
		left := index*2 + 1
		if left >= len(queue.items) {
			return index != start
		}
		child := left
		right := left + 1
		if right < len(queue.items) && queue.less(right, left) {
			child = right
		}
		if !queue.less(child, index) {
			return index != start
		}
		queue.swap(child, index)
		index = child
	}
}

// less 比较两个堆项；相同截止时间按 UUID 排序，保证行为确定。
func (queue *deadlineQueue) less(left int, right int) bool {
	if queue.items[left].deadline == queue.items[right].deadline {
		return queue.items[left].uuid < queue.items[right].uuid
	}
	return queue.items[left].deadline < queue.items[right].deadline
}

// swap 交换两个堆项并同步维护 UUID 到索引的反向映射。
func (queue *deadlineQueue) swap(left int, right int) {
	queue.items[left], queue.items[right] = queue.items[right], queue.items[left]
	queue.indices[queue.items[left].uuid] = left
	queue.indices[queue.items[right].uuid] = right
}
