package registration

import (
	"bytes"
	"context"
	"sync/atomic"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
)

// SelectorOptions 标识一个需要同步到本地并用于选择的 Registry。
type SelectorOptions struct {
	// Type 标识 Client Zone 内的 Registry。
	Type string
}

// Candidate 在选择回调内是借用视图，从 Selector 返回时则是独立副本。
// 回调必须把 Attr/Data 指针视为只读；本地负载预测只能通过 Candidates.Mutate 暂存。
type Candidate[A, D any] struct {
	// Meta 是 Redis/协议管理的 Registration 头部值副本。
	Meta Meta
	// Attr 在回调内指向不可变放置属性，脱离返回值中指向调用方独占副本。
	Attr *A
	// Data 在回调内是只读服务数据；调用 Mutate 可让同一次策略后续读取观察到暂存值。
	Data *D

	transaction *selectionTransaction[A, D]
	token       uint64
	index       int
}

// RetainedCandidate 是额外保留一个 TTL、不可被 One/Any 选择的恢复数据。
type RetainedCandidate[A, D any] struct {
	// Candidate 是脱离活动视图的完整值副本，不携带有效选择事务令牌。
	Candidate Candidate[A, D]
	// RetainedUntil 是本地 RedisClock 计算的 Unix 毫秒删除截止时间。
	RetainedUntil uint64
}

// SelectionSnapshot 是一份脱离 Selector、顺序确定的强类型 Registry 全局视图。
type SelectionSnapshot[A, D any] struct {
	// Generation 在完成新的 Redis 连接代同步后递增。
	Generation uint64
	// Synchronized 表示该视图是否通过最近一次权威恢复 PING/PONG 栅栏。
	Synchronized bool
	// Candidates 按 UUID 顺序包含所有活动 Registration 的独立副本。
	Candidates []Candidate[A, D]
	// Retained 包含仅用于恢复观察、不可选择的独立 Registration 副本。
	Retained []RetainedCandidate[A, D]
}

// Candidates 是传给 One/Any 回调的有序借用视图。
// 切片及其中 Attr/Data 指针只能在回调期间使用，不得在回调返回后保留。
type Candidates[A, D any] []Candidate[A, D]

type localOverlay[D any] struct {
	revision uint64
	data     D
	base     fields
	fields   fields
}

type selectionEntry[A, D any] struct {
	record     *selectorRecord
	attr       A
	data       D
	dataFields fields
	staged     bool
}

// overlayCommit 是一次选择事务通过全部校验后待统一发布的本地预测。
type overlayCommit[D any] struct {
	uuid    string
	overlay localOverlay[D]
}

type selectionTransaction[A, D any] struct {
	selector *Selector[A, D]
	view     *selectorView
	entries  []selectionEntry[A, D]
	selected []uint64
	commits  []overlayCommit[D]
	token    uint64
}

// Selector 持有一个强类型本地视图，并串行化本地选择和预测修改。
// 选择回调在调用方协程同步执行，不进行 Redis 或磁盘 I/O。
type Selector[A, D any] struct {
	core *selectorCore

	encodeAttr func(A) (fields, error)
	encodeData func(D) (fields, error)
	decodeAttr func(fields) (A, error)
	decodeData func(fields) (D, error)
	// decodeOwnedAttr/Data 只接管 Encoder 刚返回且已经脱离内部视图的字段。
	decodeOwnedAttr func(fields) (A, error)
	decodeOwnedData func(fields) (D, error)

	operation   chan struct{}
	overlays    map[string]localOverlay[D]
	overlayView *selectorView
	transaction selectionTransaction[A, D]
	candidates  Candidates[A, D]
	closed      atomic.Bool
}

// Selector 先订阅 Registry，再执行权威分页读取，并在首次 PING/PONG 栅栏同步后返回。
// ctx 控制首次同步；options 指定 Type。AP/DP 从 A/D 推导，要求其指针实现 Decoder。
// 成功对象的生命周期与 ctx 脱离，并拥有一个长期监听协程和最多一个临时同步协程。
func (client *Client) Selector[
	A verdandi.Encoder,
	D verdandi.Encoder,
	AP interface {
		*A
		verdandi.Decoder
	},
	DP interface {
		*D
		verdandi.Decoder
	},
](ctx context.Context, options SelectorOptions) (*Selector[A, D], error) {
	runtime, err := runtimeFor(client)
	if err != nil {
		return nil, err
	}
	core, err := runtime.selectRegistry(ctx, selectorConfig{Type: options.Type}, projectSelectorAttr[A, AP], projectSelectorData[D, DP])
	if err != nil {
		return nil, err
	}
	selector := &Selector[A, D]{
		core:      core,
		operation: make(chan struct{}, 1),
		overlays:  make(map[string]localOverlay[D]),
	}
	selector.operation <- struct{}{}
	selector.transaction.selector = selector
	selector.encodeAttr = encodeSelectorAttr
	selector.encodeData = encodeSelectorData
	selector.decodeAttr = decodeSelectorAttr[A, AP]
	selector.decodeData = decodeSelectorData[D, DP]
	selector.decodeOwnedAttr = decodeOwnedSelectorAttr[A, AP]
	selector.decodeOwnedData = decodeOwnedSelectorData[D, DP]
	return selector, nil
}

// Mutate 在当前选择事务中暂存 index 对应候选的本地 Data 变化。
// mutate 接收独立可写副本；同一回调的后续读取可观察变化，只有外层 One/Any 成功才提交到 overlay。
// 索引、事务令牌、字段结构或容量无效时回滚并返回稳定错误。
func (candidates Candidates[A, D]) Mutate(index int, mutate func(*D) error) error {
	if index < 0 || index >= len(candidates) || mutate == nil {
		return protocolError(codeInvalid, "mutation", 0)
	}
	candidate := &candidates[index]
	transaction := candidate.transaction
	if transaction == nil || candidate.token != transaction.token || candidate.index != index || index >= len(transaction.entries) {
		return protocolError(codeContract, "candidate", 0)
	}
	entry := &transaction.entries[index]
	// 每次从当前事务字段重新解码，避免应用回调原地修改借用 Data 绕过 Mutate。
	next, err := transaction.selector.decodeData(entry.dataFields)
	if err != nil {
		return err
	}
	if err := mutate(&next); err != nil {
		return err
	}
	encoded, err := transaction.selector.encodeData(next)
	if err != nil {
		return err
	}
	if !sameFieldStructure(entry.dataFields, encoded) {
		return protocolError(codeContract, "data", entry.record.meta.Revision)
	}
	if err := validateFields(nil, encoded, transaction.selector.core.client.limits()); err != nil {
		return err
	}
	entry.data = next
	entry.dataFields = encoded
	entry.staged = true
	return nil
}

// One 执行一次本地选择事务，并最多返回一个候选。
// choose 返回 false 表示无匹配；外来/过期 Candidate、回调错误或 ctx 结束都会回滚所有暂存预测。
// 回调持锁期间应保持短暂；强制超时不会中断正在执行的 Go 函数，只会在回调返回后阻止提交。
func (selector *Selector[A, D]) One(
	ctx context.Context,
	choose func(Candidates[A, D]) (Candidate[A, D], bool, error),
) (Candidate[A, D], bool, error) {
	var zero Candidate[A, D]
	if choose == nil {
		return zero, false, protocolError(codeInvalid, "callback", 0)
	}
	if err := selector.acquire(ctx); err != nil {
		return zero, false, err
	}
	defer selector.release()
	transaction, candidates, err := selector.begin()
	if err != nil {
		return zero, false, err
	}
	selected, ok, err := choose(candidates)
	if err != nil || !ok {
		return zero, false, err
	}
	if err := ctx.Err(); err != nil {
		return zero, false, wrapContext(err)
	}
	entry, attr, data, err := transaction.selectedEntry(selected)
	if err != nil {
		return zero, false, err
	}
	// 先构造完全脱离内部状态的返回值；解码失败时不得提交本地预测。
	result, err := transaction.detachedOwned(entry, attr, data)
	if err != nil {
		return zero, false, err
	}
	if err := transaction.commit(); err != nil {
		return zero, false, err
	}
	return result, true, nil
}

// Any 执行一次本地选择事务，并返回零个或多个互不重复的候选。
// 空结果表示无匹配并回滚暂存修改；重复、外来或过期 Candidate 返回 contract 且全部回滚。
func (selector *Selector[A, D]) Any(
	ctx context.Context,
	choose func(Candidates[A, D]) ([]Candidate[A, D], error),
) ([]Candidate[A, D], error) {
	if choose == nil {
		return nil, protocolError(codeInvalid, "callback", 0)
	}
	if err := selector.acquire(ctx); err != nil {
		return nil, err
	}
	defer selector.release()
	transaction, candidates, err := selector.begin()
	if err != nil {
		return nil, err
	}
	selected, err := choose(candidates)
	if err != nil || len(selected) == 0 {
		return nil, err
	}
	if err := ctx.Err(); err != nil {
		return nil, wrapContext(err)
	}
	// selected 使用代号数组做 O(1) 去重，避免为每次 Any 分配临时 map。
	transaction.prepareSelected()
	result := make([]Candidate[A, D], 0, len(selected))
	for _, candidate := range selected {
		entry, attr, data, entryErr := transaction.selectedEntry(candidate)
		if entryErr != nil {
			return nil, entryErr
		}
		if transaction.selected[candidate.index] == transaction.token {
			return nil, protocolError(codeContract, "candidate", entry.record.meta.Revision)
		}
		transaction.selected[candidate.index] = transaction.token
		detached, detachErr := transaction.detachedOwned(entry, attr, data)
		if detachErr != nil {
			return nil, detachErr
		}
		result = append(result, detached)
	}
	if err := transaction.commit(); err != nil {
		return nil, err
	}
	return result, nil
}

// Snapshot 返回一份完整、独立的强类型视图，不执行 Redis 或磁盘 I/O。
// ctx 只控制等待 Selector 本地事务锁；视图未同步时返回 unavailable。
func (selector *Selector[A, D]) Snapshot(ctx context.Context) (SelectionSnapshot[A, D], error) {
	if err := selector.acquire(ctx); err != nil {
		return SelectionSnapshot[A, D]{}, err
	}
	defer selector.release()
	transaction, candidates, err := selector.begin()
	if err != nil {
		return SelectionSnapshot[A, D]{}, err
	}
	view := transaction.view
	result := SelectionSnapshot[A, D]{
		Generation:   view.generation,
		Synchronized: view.synchronized,
		Candidates:   make([]Candidate[A, D], 0, len(candidates)),
		Retained:     make([]RetainedCandidate[A, D], 0, len(view.orderedRetained)),
	}
	for index := range transaction.entries {
		candidate, detachErr := transaction.detached(&transaction.entries[index])
		if detachErr != nil {
			return SelectionSnapshot[A, D]{}, detachErr
		}
		result.Candidates = append(result.Candidates, candidate)
	}
	for _, retained := range view.orderedRetained {
		candidate, detachErr := selector.detachedRecord(retained.record)
		if detachErr != nil {
			return SelectionSnapshot[A, D]{}, detachErr
		}
		result.Retained = append(result.Retained, RetainedCandidate[A, D]{
			Candidate:     candidate,
			RetainedUntil: retained.until,
		})
	}
	return result, nil
}

// Find 按 uuid 查找一个活动候选并返回独立副本，不执行 Redis I/O。
// 未找到返回零值、false、nil；视图未同步时返回 unavailable。
func (selector *Selector[A, D]) Find(ctx context.Context, uuid string) (Candidate[A, D], bool, error) {
	var zero Candidate[A, D]
	if err := selector.acquire(ctx); err != nil {
		return zero, false, err
	}
	defer selector.release()
	view := selector.core.view.Load()
	if view == nil || !view.synchronized {
		return zero, false, protocolError(codeUnavailable, "selector", 0)
	}
	if err := selector.reconcileOverlays(view); err != nil {
		return zero, false, err
	}
	record := view.records[uuid]
	if record == nil {
		return zero, false, nil
	}
	candidate, err := selector.detachedActiveRecord(record)
	return candidate, err == nil, err
}

// FindRetained 按 uuid 返回一条不可选择的 retained 恢复记录副本。
// 未找到返回零值、false、nil；它不会回退查找活动候选。
func (selector *Selector[A, D]) FindRetained(ctx context.Context, uuid string) (RetainedCandidate[A, D], bool, error) {
	if err := selector.acquire(ctx); err != nil {
		return RetainedCandidate[A, D]{}, false, err
	}
	defer selector.release()
	view := selector.core.view.Load()
	if view == nil || !view.synchronized {
		return RetainedCandidate[A, D]{}, false, protocolError(codeUnavailable, "selector", 0)
	}
	retained, exists := view.retained[uuid]
	if !exists {
		return RetainedCandidate[A, D]{}, false, nil
	}
	candidate, err := selector.detachedRecord(retained.record)
	if err != nil {
		return RetainedCandidate[A, D]{}, false, err
	}
	return RetainedCandidate[A, D]{Candidate: candidate, RetainedUntil: retained.until}, true, nil
}

// Errors 返回 Selector 同步和恢复的有界异步诊断通道；关闭后通道关闭。
func (selector *Selector[A, D]) Errors() <-chan error {
	if selector == nil || selector.core == nil {
		return nil
	}
	return selector.core.Errors()
}

// Close 取消同步并等待该 Selector 拥有的全部协程退出。
// ctx 只限制本次等待；关闭状态一旦发布便不可恢复。
func (selector *Selector[A, D]) Close(ctx context.Context) error {
	if selector == nil || selector.core == nil {
		return nil
	}
	selector.closed.Store(true)
	return selector.core.Close(ctx)
}

// begin 取得当前已同步不可变视图，并初始化一个可供回调使用的选择事务。
func (selector *Selector[A, D]) begin() (*selectionTransaction[A, D], Candidates[A, D], error) {
	if selector.closed.Load() || selector.core == nil {
		return nil, nil, protocolError(codeClosed, "", 0)
	}
	view := selector.core.view.Load()
	if view == nil || !view.synchronized {
		return nil, nil, protocolError(codeUnavailable, "selector", 0)
	}
	return selector.beginView(view)
}

// beginView 对 view 先对账本地 overlay，再复用内部切片构造借用 Candidate 列表。
// 调用方必须已持有 selector.operation；返回事务只在下一次 begin 前有效。
func (selector *Selector[A, D]) beginView(view *selectorView) (*selectionTransaction[A, D], Candidates[A, D], error) {
	if err := selector.reconcileOverlays(view); err != nil {
		return nil, nil, err
	}
	// 递增 token 使上一次回调保留的 Candidate 立即失效；溢出时清空去重代号后从一重新开始。
	transaction := &selector.transaction
	transaction.view = view
	transaction.token++
	if transaction.token == 0 {
		clear(transaction.selected[:cap(transaction.selected)])
		transaction.token = 1
	}
	previousEntries := len(transaction.entries)
	transaction.entries = transaction.entries[:0]
	if cap(transaction.entries) < len(view.orderedRecords) {
		transaction.entries = make([]selectionEntry[A, D], 0, len(view.orderedRecords))
		previousEntries = 0
	}
	previousCandidates := len(selector.candidates)
	candidates := selector.candidates[:0]
	if cap(candidates) < len(view.orderedRecords) {
		candidates = make(Candidates[A, D], 0, len(view.orderedRecords))
		previousCandidates = 0
	}
	// 记录中的强类型投影在同步发布前已构造；这里仅断言类型并叠加本地预测 overlay。
	for _, record := range view.orderedRecords {
		attr, ok := record.projectedAttr.(A)
		if !ok {
			return nil, nil, protocolError(codeCorrupt, "attr", record.meta.Revision)
		}
		data, ok := record.projectedData.(D)
		if !ok {
			return nil, nil, protocolError(codeCorrupt, "data", record.meta.Revision)
		}
		dataFields := record.data
		if overlay, exists := selector.overlays[record.meta.UUID]; exists {
			data = overlay.data
			dataFields = overlay.fields
		}
		transaction.entries = append(transaction.entries, selectionEntry[A, D]{
			record:     record,
			attr:       attr,
			data:       data,
			dataFields: dataFields,
		})
		index := len(transaction.entries) - 1
		entry := &transaction.entries[index]
		candidates = append(candidates, Candidate[A, D]{
			Meta:        record.meta,
			Attr:        &entry.attr,
			Data:        &entry.data,
			transaction: transaction,
			token:       transaction.token,
			index:       index,
		})
	}
	// 只在视图缩小时清除复用数组尾部，避免已删除记录被旧指针长期保活；稳定视图不增加清零遍历。
	if len(transaction.entries) < previousEntries {
		clear(transaction.entries[len(transaction.entries):previousEntries])
	}
	if len(candidates) < previousCandidates {
		clear(candidates[len(candidates):previousCandidates])
	}
	selector.candidates = candidates
	return transaction, candidates, nil
}

// prepareSelected 调整 Any 去重代号切片长度，尽量复用已分配容量。
func (transaction *selectionTransaction[A, D]) prepareSelected() {
	if cap(transaction.selected) < len(transaction.entries) {
		transaction.selected = make([]uint64, len(transaction.entries))
	} else {
		transaction.selected = transaction.selected[:len(transaction.entries)]
	}
}

// reconcileOverlays 把远端新视图合并进本地负载预测。
// 远端改变的字段覆盖预测，未改变字段保留本地值；记录消失时删除对应 overlay。
func (selector *Selector[A, D]) reconcileOverlays(view *selectorView) error {
	// 视图不可变；指针相同即可证明所有 overlay 已与这份精确远端状态对账。
	if selector.overlayView == view {
		return nil
	}
	for uuid, overlay := range selector.overlays {
		record := view.records[uuid]
		if record == nil {
			delete(selector.overlays, uuid)
			continue
		}
		if record.meta.Revision != overlay.revision {
			// overlay 和远端记录字段都已归 SDK 所有且不可变；只复制 map，字段字节可安全共享。
			fields := cloneFieldMap(overlay.fields)
			for name, value := range record.data {
				if !bytes.Equal(value, overlay.base[name]) {
					fields[name] = value
				}
			}
			data, err := selector.decodeData(fields)
			if err != nil {
				return err
			}
			selector.overlays[uuid] = localOverlay[D]{
				revision: record.meta.Revision,
				data:     data,
				base:     record.data,
				fields:   fields,
			}
		}
	}
	selector.overlayView = view
	return nil
}

// selectedEntry 验证 candidate 属于当前事务且应用没有原地修改其 Meta/Attr/Data。
// 返回的 Attr/Data 是 Encoder 新建并转移给 SDK 的独立字段，可直接用于构造脱离结果。
func (transaction *selectionTransaction[A, D]) selectedEntry(candidate Candidate[A, D]) (*selectionEntry[A, D], fields, fields, error) {
	if candidate.transaction != transaction || candidate.token != transaction.token || candidate.index < 0 || candidate.index >= len(transaction.entries) {
		return nil, nil, nil, protocolError(codeContract, "candidate", 0)
	}
	entry := &transaction.entries[candidate.index]
	if candidate.Meta.UUID != entry.record.meta.UUID {
		return nil, nil, nil, protocolError(codeContract, "candidate", entry.record.meta.Revision)
	}
	attr, err := transaction.selector.encodeAttr(entry.attr)
	if err != nil {
		return nil, nil, nil, err
	}
	data, err := transaction.selector.encodeData(entry.data)
	if err != nil {
		return nil, nil, nil, err
	}
	if !fieldsEqual(attr, entry.record.attr) || !fieldsEqual(data, entry.dataFields) {
		return nil, nil, nil, protocolError(codeContract, "candidate", entry.record.meta.Revision)
	}
	return entry, attr, data, nil
}

// commit 把事务中通过 Mutate 暂存的 Data 原子保存为本地 overlay。
// 它先构造全部提交项，任何解码失败都不会改变现有 overlay；调用方已在进入前验证候选集合。
func (transaction *selectionTransaction[A, D]) commit() error {
	transaction.commits = transaction.commits[:0]
	for index := range transaction.entries {
		entry := &transaction.entries[index]
		if !entry.staged {
			continue
		}
		data, err := transaction.selector.decodeData(entry.dataFields)
		if err != nil {
			clear(transaction.commits)
			transaction.commits = transaction.commits[:0]
			return err
		}
		transaction.commits = append(transaction.commits, overlayCommit[D]{
			uuid: entry.record.meta.UUID,
			overlay: localOverlay[D]{
				revision: entry.record.meta.Revision,
				data:     data,
				// record.data 和 dataFields 都已归 SDK 所有且后续只读，可安全共享而不再深拷贝。
				base:   entry.record.data,
				fields: entry.dataFields,
			},
		})
	}
	// 只有全部应用解码与复制成功后才改变可见 overlay，保持 One/Any 的本地事务原子性。
	for index := range transaction.commits {
		commit := &transaction.commits[index]
		transaction.selector.overlays[commit.uuid] = commit.overlay
	}
	// 清除复用缓冲中的指针，避免事务对象额外保活已经由 overlays 接管的历史值。
	clear(transaction.commits)
	transaction.commits = transaction.commits[:0]
	return nil
}

// detachedActiveRecord 返回活动记录的独立候选副本，并在存在时应用已对账的本地 overlay。
func (selector *Selector[A, D]) detachedActiveRecord(record *selectorRecord) (Candidate[A, D], error) {
	if overlay, exists := selector.overlays[record.meta.UUID]; exists {
		attr, err := selector.decodeAttr(record.attr)
		if err != nil {
			return Candidate[A, D]{}, err
		}
		data, err := selector.decodeData(overlay.fields)
		if err != nil {
			return Candidate[A, D]{}, err
		}
		return Candidate[A, D]{Meta: record.meta, Attr: &attr, Data: &data}, nil
	}
	return selector.detachedRecord(record)
}

// detached 从事务 entry 的当前字段构造独立 Candidate，不携带可复用的事务令牌。
func (transaction *selectionTransaction[A, D]) detached(entry *selectionEntry[A, D]) (Candidate[A, D], error) {
	attr, err := transaction.selector.decodeAttr(entry.record.attr)
	if err != nil {
		return Candidate[A, D]{}, err
	}
	data, err := transaction.selector.decodeData(entry.dataFields)
	if err != nil {
		return Candidate[A, D]{}, err
	}
	return Candidate[A, D]{Meta: entry.record.meta, Attr: &attr, Data: &data}, nil
}

// detachedOwned 接管 selectedEntry 刚生成的独立字段并构造返回值，不重复深拷贝内部 Fields。
func (transaction *selectionTransaction[A, D]) detachedOwned(entry *selectionEntry[A, D], attrFields fields, dataFields fields) (Candidate[A, D], error) {
	attr, err := transaction.selector.decodeOwnedAttr(attrFields)
	if err != nil {
		return Candidate[A, D]{}, err
	}
	data, err := transaction.selector.decodeOwnedData(dataFields)
	if err != nil {
		return Candidate[A, D]{}, err
	}
	return Candidate[A, D]{Meta: entry.record.meta, Attr: &attr, Data: &data}, nil
}

// detachedRecord 从原始不可变 record 构造独立 Candidate，不叠加本地 overlay。
func (selector *Selector[A, D]) detachedRecord(record *selectorRecord) (Candidate[A, D], error) {
	attr, err := selector.decodeAttr(record.attr)
	if err != nil {
		return Candidate[A, D]{}, err
	}
	data, err := selector.decodeData(record.data)
	if err != nil {
		return Candidate[A, D]{}, err
	}
	return Candidate[A, D]{Meta: record.meta, Attr: &attr, Data: &data}, nil
}

// acquire 使用容量为一的通道串行化选择事务，并允许 ctx 在等待本地锁时取消。
// 成功后调用方必须 defer release；回调执行期间令牌不会被强制抢占。
func (selector *Selector[A, D]) acquire(ctx context.Context) error {
	if selector == nil || selector.closed.Load() {
		return protocolError(codeClosed, "", 0)
	}
	if ctx == nil {
		return protocolError(codeInvalid, "context", 0)
	}
	select {
	case <-selector.operation:
		return nil
	case <-ctx.Done():
		return wrapContext(ctx.Err())
	}
}

// release 归还一次成功 acquire 获得的选择事务令牌。
func (selector *Selector[A, D]) release() {
	selector.operation <- struct{}{}
}
