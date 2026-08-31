package registration

import "sort"

// pendingChange 表示同一 Registration 所有连续待处理事件的完整逻辑效果。
// baseRevision 是合并 Update 可安全应用到的本地版本；repair 表示必须回读权威 Hash。
type pendingChange struct {
	event          registrationEvent
	baseRevision   uint64
	latestRevision uint64
	repair         bool
}

// pendingChanges 为每个 UUID 最多保留一个合并项，并同时限制条目数和估算字节数。
type pendingChanges struct {
	entries    map[string]pendingChange
	bytes      int
	maxEntries int
	maxBytes   int
}

// newPendingChanges 创建一个空合并缓冲区；maxEntries 与 maxBytes 均由 Selector 配置提供。
func newPendingChanges(maxEntries int, maxBytes int) pendingChanges {
	return pendingChanges{
		entries:    make(map[string]pendingChange),
		maxEntries: maxEntries,
		maxBytes:   maxBytes,
	}
}

// add 接管 event 的 map 与字节切片，并按 UUID 合并连续变化。
// 订阅读取者交付后不再保留 event；容量拒绝时，接近字节上限的路径保证原缓冲区不被部分修改。
func (pending *pendingChanges) add(event registrationEvent) error {
	current, exists := pending.entries[event.uuid]
	previousBytes := 0
	if exists {
		previousBytes = pendingChangeSize(current)
	}

	var next pendingChange
	var err error
	if exists {
		// 合并结果最多增长一个 incoming 的估算大小。普通路径直接修改已拥有 map；
		// 接近上限时先复制，使最终容量检查失败仍保持原队列事务性。
		incomingBytes := registrationEventSize(event)
		copyCurrent := pending.bytes > pending.maxBytes-incomingBytes
		next, err = mergePendingChange(current, event, copyCurrent)
	} else {
		next = initialPendingChange(event)
	}
	if err != nil {
		return err
	}

	nextBytes := pending.bytes - previousBytes + pendingChangeSize(next)
	if !exists && len(pending.entries) >= pending.maxEntries {
		return protocolError(codeCapacity, "selector_event_entries", 0)
	}
	if nextBytes > pending.maxBytes {
		return protocolError(codeCapacity, "selector_event_bytes", 0)
	}
	pending.entries[event.uuid] = next
	pending.bytes = nextBytes
	return nil
}

// drain 按 UUID 排序返回全部合并项，并原地清空缓冲区供下一轮复用。
// 空缓冲区返回 nil；排序保证修复和测试顺序确定。
func (pending *pendingChanges) drain() []pendingChange {
	if len(pending.entries) == 0 {
		return nil
	}
	uuids := make([]string, 0, len(pending.entries))
	for uuid := range pending.entries {
		uuids = append(uuids, uuid)
	}
	sort.Strings(uuids)
	changes := make([]pendingChange, 0, len(uuids))
	for _, uuid := range uuids {
		changes = append(changes, pending.entries[uuid])
	}
	clear(pending.entries)
	pending.bytes = 0
	return changes
}

// initialPendingChange 从第一条事件建立合并状态，并推导 Update/Renew 的基准版本。
func initialPendingChange(event registrationEvent) pendingChange {
	change := pendingChange{event: event, latestRevision: event.revision}
	if event.kind == "update" {
		change.baseRevision = event.revision - 1
	} else if event.kind == "renew" {
		change.baseRevision = event.revision
	}
	return change
}

// mergePendingChange 把 incoming 合并到 current，并验证生命周期与版本连续性。
// copyCurrent 指示修改前是否深拷贝 current 的字段，以保证容量拒绝时不改变原状态。
func mergePendingChange(current pendingChange, incoming registrationEvent, copyCurrent bool) (pendingChange, error) {
	if incoming.kind == "unregister" {
		return initialPendingChange(incoming), nil
	}
	if current.event.kind == "unregister" {
		return pendingChange{}, protocolError(codeTransition, "@uuid", 0)
	}
	// 一旦进入 repair，只有足够新的完整 Register 能重新建立可直接应用的基线。
	if current.repair {
		if incoming.kind == "register" && incoming.revision >= current.latestRevision {
			return initialPendingChange(incoming), nil
		}
		if incoming.revision > current.latestRevision {
			current.latestRevision = incoming.revision
			current.event.revision = incoming.revision
		}
		if incoming.timestamp > current.event.timestamp {
			current.event.timestamp = incoming.timestamp
		}
		return current, nil
	}

	if incoming.kind == "register" {
		if incoming.revision < current.latestRevision {
			return current, nil
		}
		next := initialPendingChange(incoming)
		if incoming.revision == current.latestRevision {
			if current.event.kind == "register" && !sameRegistrationContent(current.event, incoming) {
				return repairPendingChange(current, incoming), nil
			}
			if current.event.timestamp > next.event.timestamp {
				next.event.timestamp = current.event.timestamp
			}
		}
		return next, nil
	}

	switch incoming.kind {
	case "update":
		return mergePendingUpdate(current, incoming, copyCurrent), nil
	case "renew":
		return mergePendingRenew(current, incoming), nil
	default:
		return pendingChange{}, protocolError(codeInvalid, "&kind", 0)
	}
}

// mergePendingUpdate 合并一条新 Update；版本重复冲突或间隙会退化为定向修复标记。
func mergePendingUpdate(current pendingChange, incoming registrationEvent, copyCurrent bool) pendingChange {
	if incoming.revision <= current.latestRevision {
		if incoming.revision == current.latestRevision && current.event.kind == "update" &&
			(!sameOptionalVersion(current.event, incoming) || !fieldsEqual(current.event.data, incoming.data)) {
			return repairPendingChange(current, incoming)
		}
		return current
	}
	if incoming.revision != current.latestRevision+1 {
		return repairPendingChange(current, incoming)
	}

	// 完整 Register 可在原地吸收连续字段 Patch，前提是 Patch 不引入未知 Data 字段。
	if current.event.kind == "register" {
		next := current
		if copyCurrent {
			next.event = cloneRegistrationEvent(current.event)
		}
		for name, value := range incoming.data {
			if _, exists := next.event.data[name]; !exists {
				return repairPendingChange(current, incoming)
			}
			next.event.data[name] = value
		}
		next.event.revision = incoming.revision
		next.event.timestamp = max(current.event.timestamp, incoming.timestamp)
		if incoming.hasVersion {
			next.event.version = incoming.version
			next.event.hasVersion = true
		}
		next.latestRevision = incoming.revision
		return next
	}

	// Renew 后的首个 Update 替换事件主体但保留旧基准；连续 Update 则按字段覆盖。
	next := current
	if current.event.kind == "renew" {
		next.event = incoming
		next.baseRevision = current.baseRevision
	} else {
		if copyCurrent {
			next.event = cloneRegistrationEvent(current.event)
		}
		if next.event.data == nil {
			next.event.data = make(fields, len(incoming.data))
		}
		for name, value := range incoming.data {
			next.event.data[name] = value
		}
		if incoming.hasVersion {
			next.event.version = incoming.version
			next.event.hasVersion = true
		}
		next.event.revision = incoming.revision
	}
	next.event.timestamp = max(current.event.timestamp, incoming.timestamp)
	next.latestRevision = incoming.revision
	return next
}

// mergePendingRenew 合并同版本续期，只提升时间戳；跨版本续期要求权威修复。
func mergePendingRenew(current pendingChange, incoming registrationEvent) pendingChange {
	if incoming.revision < current.latestRevision {
		return current
	}
	if incoming.revision > current.latestRevision {
		return repairPendingChange(current, incoming)
	}
	if incoming.timestamp > current.event.timestamp {
		current.event.timestamp = incoming.timestamp
	}
	return current
}

// repairPendingChange 把无法安全合并的序列压缩为一个有界定向修复请求。
// 它保留观察到的最大版本和时间戳，帮助后续完整 Register 判断是否足够新。
func repairPendingChange(current pendingChange, incoming registrationEvent) pendingChange {
	latest := max(current.latestRevision, incoming.revision)
	timestamp := max(current.event.timestamp, incoming.timestamp)
	return pendingChange{
		event: registrationEvent{
			kind:      "repair",
			uuid:      incoming.uuid,
			revision:  latest,
			timestamp: timestamp,
		},
		latestRevision: latest,
		repair:         true,
	}
}

// cloneRegistrationEvent 深拷贝 Attr/Data，其余标量按值复制。
func cloneRegistrationEvent(event registrationEvent) registrationEvent {
	event.attr = cloneFields(event.attr)
	event.data = cloneFields(event.data)
	return event
}

// sameRegistrationContent 比较完整 Register 的 TTL、Version、Attr 和 Data，不比较时间戳。
func sameRegistrationContent(left registrationEvent, right registrationEvent) bool {
	return left.ttl == right.ttl && left.version == right.version &&
		fieldsEqual(left.attr, right.attr) && fieldsEqual(left.data, right.data)
}

// sameOptionalVersion 比较 Update 是否都省略 Version，或都携带相同 Version。
func sameOptionalVersion(left registrationEvent, right registrationEvent) bool {
	return left.hasVersion == right.hasVersion && (!left.hasVersion || left.version == right.version)
}

// pendingChangeSize 返回一个合并项的保守内存估算，用于本地缓冲容量控制。
func pendingChangeSize(change pendingChange) int {
	return registrationEventSize(change.event)
}

// registrationEventSize 估算事件持有的字符串、字段 map 和字节切片内存。
// bookkeeping 是每条记录的固定保守开销，不用于线协议长度计算。
func registrationEventSize(event registrationEvent) int {
	const bookkeeping = 128
	size := bookkeeping + len(event.uuid) + len(event.kind)
	for name, value := range event.attr {
		size += 16 + len(name) + len(value)
	}
	for name, value := range event.data {
		size += 16 + len(name) + len(value)
	}
	return size
}
