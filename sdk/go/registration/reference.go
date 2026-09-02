package registration

import (
	"context"
	"sync/atomic"
)

// ReferenceSchema 把应用 Attr/Data 绑定为代码生成的只读视图和 Data 编辑器。
// Attr、Data 与 Edit 只负责机械包装；CloneData 必须复制 Data 内所有可变引用成员。
// SDK 生成器会构造完整 Schema，普通业务代码不需要手工填写。
type ReferenceSchema[A, D, AR, DR, DE any] struct {
	// Attr 为不可变 Attr 值创建回调期只读视图。
	Attr func(*A) AR
	// Data 为当前 Data 值创建回调期只读视图。
	Data func(*D) DR
	// Edit 把受事务令牌保护的底层编辑器包装为生成的字段编辑器。
	Edit func(ReferenceEditor[A, D]) DE
	// CloneData 在第一次编辑前复制 Data，隔离不可变远端视图和既有本地预测。
	CloneData func(D) D
}

// ReferenceSlice 是生成只读视图用于延迟复制 slice 字段的轻量包装。
// 包装本身只借用不可变元素；Clone 在调用方真正读取该字段时返回独立的顶层 slice。
// verdandi-refgen 仅为不含指针、map、slice 等可变引用的元素类型生成此包装。
type ReferenceSlice[S ~[]E, E any] struct {
	values S
}

// NewReferenceSlice 借用一个已经由 Selector 内部所有且只读的 slice。
// 普通应用代码不需要直接调用；它是生成视图的机械构造边界。
func NewReferenceSlice[S ~[]E, E any](values S) ReferenceSlice[S, E] {
	return ReferenceSlice[S, E]{values: values}
}

// Clone 返回保持 nil/非 nil 语义的独立顶层 slice；元素按值复制。
func (slice ReferenceSlice[S, E]) Clone() S {
	if slice.values == nil {
		return nil
	}
	result := make(S, len(slice.values))
	copy(result, slice.values)
	return result
}

// ReferenceSelector 是 Selector 的可选高性能强类型引用入口。
// 它复用原 Selector 的串行事务锁、同步视图和本地 overlay，但不构造完整 Candidate 切片，
// 也不为选择结果创建脱离副本。生成的语言绑定通常会把这个类型嵌入更短的应用类型中。
type ReferenceSelector[A, D, AR, DR, DE any] struct {
	selector *Selector[A, D]
	schema   ReferenceSchema[A, D, AR, DR, DE]
	lease    referenceLease
}

// ReferenceCandidates 是选择回调期间有效的按需候选视图。
// Len/At 不执行 Redis、磁盘或解码操作；该值及其派生值都不得逃逸回调。
type ReferenceCandidates[A, D, AR, DR, DE any] struct {
	reference   *ReferenceSelector[A, D, AR, DR, DE]
	transaction *selectionTransaction[A, D]
	token       uint64
}

// ReferenceCandidate 是一个回调期只读候选句柄。
// Attr/Data 的具体视图由生成代码提供，不公开底层 *A 或 *D，因此正常 Go 代码无法原地改写它们。
type ReferenceCandidate[A, D, AR, DR, DE any] struct {
	reference   *ReferenceSelector[A, D, AR, DR, DE]
	transaction *selectionTransaction[A, D]
	value       *selectionEntry[A, D]
	token       uint64
	index       int
}

// ReferenceSelection 是策略显式选中的候选。
// 只有最终由 WithOne/WithAny 返回的 Selection 才会提交其本地 Data 编辑；其余编辑全部回滚。
type ReferenceSelection[A, D, AR, DR, DE any] struct {
	candidate ReferenceCandidate[A, D, AR, DR, DE]
}

// ReferenceEditor 为生成的字段 setter 提供受回调生命周期保护的内部编辑能力。
// 应用代码通常只能看到生成的具体 Editor；Apply 主要供生成代码使用。
type ReferenceEditor[A, D any] struct {
	transaction *selectionTransaction[A, D]
	lease       *referenceLease
	clone       func(D) D
	token       uint64
	index       int
}

// referenceLease 用原子令牌标记一次正在执行的引用回调。
// 零值表示回调已经结束，使误保留的生成 Editor 在后续调用时稳定失败而非改写复用缓冲。
type referenceLease struct {
	token atomic.Uint64
}

// NewReferenceSelector 校验生成绑定并创建一个共享原 Selector 状态的引用入口。
// 创建过程不启动协程、不复制当前视图，也不改变原 Selector 的生命周期。
func NewReferenceSelector[A, D, AR, DR, DE any](
	selector *Selector[A, D],
	schema ReferenceSchema[A, D, AR, DR, DE],
) (*ReferenceSelector[A, D, AR, DR, DE], error) {
	if selector == nil {
		return nil, protocolError(codeInvalid, "selector", 0)
	}
	if schema.Attr == nil || schema.Data == nil || schema.Edit == nil || schema.CloneData == nil {
		return nil, protocolError(codeInvalid, "schema", 0)
	}
	return &ReferenceSelector[A, D, AR, DR, DE]{selector: selector, schema: schema}, nil
}

// Len 返回当前回调事务的候选数量。
// Candidates 是借用值，回调结束后的任何调用都违反 API 生命周期约定。
func (candidates ReferenceCandidates[A, D, AR, DR, DE]) Len() int {
	if candidates.transaction == nil {
		return 0
	}
	return len(candidates.transaction.entries)
}

// At 按稳定 UUID 顺序返回一个只读候选。
// 越界时返回零值和 false；生命周期由外层回调约束，不在遍历热路径重复读取原子令牌。
func (candidates ReferenceCandidates[A, D, AR, DR, DE]) At(index int) (ReferenceCandidate[A, D, AR, DR, DE], bool) {
	if candidates.reference == nil || candidates.transaction == nil || index < 0 || index >= len(candidates.transaction.entries) {
		return ReferenceCandidate[A, D, AR, DR, DE]{}, false
	}
	return ReferenceCandidate[A, D, AR, DR, DE]{
		reference:   candidates.reference,
		transaction: candidates.transaction,
		value:       &candidates.transaction.entries[index],
		token:       candidates.token,
		index:       index,
	}, true
}

// Valid 报告候选是否仍属于当前正在执行的引用回调。
func (candidate ReferenceCandidate[A, D, AR, DR, DE]) Valid() bool {
	_, ok := candidate.checkedEntry()
	return ok
}

// Meta 返回 Redis/协议管理的 Registration 头部副本；零值候选返回零值。
func (candidate ReferenceCandidate[A, D, AR, DR, DE]) Meta() Meta {
	if candidate.value == nil || candidate.value.record == nil {
		return Meta{}
	}
	return candidate.value.record.meta
}

// Attr 返回代码生成的回调期只读 Attr 视图；零值候选返回 AR 零值。
func (candidate ReferenceCandidate[A, D, AR, DR, DE]) Attr() AR {
	if candidate.value == nil || candidate.reference == nil {
		var zero AR
		return zero
	}
	return candidate.reference.schema.Attr(&candidate.value.attr)
}

// Data 返回代码生成的回调期只读 Data 视图。
// 同一回调内先前通过 Editor 暂存的修改会立即可见；零值候选返回 DR 零值。
func (candidate ReferenceCandidate[A, D, AR, DR, DE]) Data() DR {
	if candidate.value == nil || candidate.reference == nil {
		var zero DR
		return zero
	}
	return candidate.reference.schema.Data(&candidate.value.data)
}

// Select 把只读候选转换为可由策略返回的 Selection。
// 转换本身不提交数据；WithOne/WithAny 会在回调成功后验证归属、去重并统一提交。
func (candidate ReferenceCandidate[A, D, AR, DR, DE]) Select() ReferenceSelection[A, D, AR, DR, DE] {
	return ReferenceSelection[A, D, AR, DR, DE]{candidate: candidate}
}

// Valid 报告 Selection 是否仍属于当前正在执行的引用回调。
func (selection ReferenceSelection[A, D, AR, DR, DE]) Valid() bool {
	return selection.candidate.Valid()
}

// Meta 返回所选候选的协议头部副本；零值 Selection 返回零值。
func (selection ReferenceSelection[A, D, AR, DR, DE]) Meta() Meta {
	return selection.candidate.Meta()
}

// Attr 返回所选候选的只读 Attr 视图；零值 Selection 返回 AR 零值。
func (selection ReferenceSelection[A, D, AR, DR, DE]) Attr() AR {
	return selection.candidate.Attr()
}

// Data 返回所选候选当前的只读 Data 视图；零值 Selection 返回 DR 零值。
func (selection ReferenceSelection[A, D, AR, DR, DE]) Data() DR {
	return selection.candidate.Data()
}

// Edit 返回代码生成的字段编辑器。
// 每个 setter 只修改事务内的隔离副本；回调失败、未选中、重复选择或 ctx 结束时不会发布 overlay。
func (selection ReferenceSelection[A, D, AR, DR, DE]) Edit() DE {
	candidate := selection.candidate
	if _, ok := candidate.checkedEntry(); !ok {
		var zero DE
		return zero
	}
	editor := ReferenceEditor[A, D]{
		transaction: candidate.transaction,
		lease:       &candidate.reference.lease,
		clone:       candidate.reference.schema.CloneData,
		token:       candidate.token,
		index:       candidate.index,
	}
	return candidate.reference.schema.Edit(editor)
}

// Apply 在第一次修改时克隆 Data，随后把 mutate 同步应用到当前事务副本。
// mutate 不得保留传入指针或启动异步修改；生成 setter 只传入不会逃逸的短闭包。
func (editor ReferenceEditor[A, D]) Apply(mutate func(*D)) error {
	if mutate == nil {
		return protocolError(codeInvalid, "mutation", 0)
	}
	entry, err := editor.entry()
	if err != nil {
		return err
	}
	if !entry.staged {
		entry.data = editor.clone(entry.data)
		entry.staged = true
	}
	mutate(&entry.data)
	return nil
}

// WithOne 执行一次不返回脱离副本的本地选择事务。
// choose 返回的唯一 Selection 决定允许提交的本地预测；其他候选即使被编辑也会回滚。
// 返回 true 表示策略选中了有效候选，false 表示正常无匹配。
func (reference *ReferenceSelector[A, D, AR, DR, DE]) WithOne(
	ctx context.Context,
	choose func(ReferenceCandidates[A, D, AR, DR, DE]) (ReferenceSelection[A, D, AR, DR, DE], bool, error),
) (bool, error) {
	if choose == nil {
		return false, protocolError(codeInvalid, "callback", 0)
	}
	if reference == nil || reference.selector == nil {
		return false, protocolError(codeClosed, "", 0)
	}
	if err := reference.selector.acquire(ctx); err != nil {
		return false, err
	}
	defer reference.selector.release()

	transaction, candidates, err := reference.begin()
	if err != nil {
		return false, err
	}
	defer reference.end(transaction.token)
	selected, ok, err := choose(candidates)
	if err != nil || !ok {
		return false, err
	}
	if err := ctx.Err(); err != nil {
		return false, wrapContext(err)
	}
	one := [...]ReferenceSelection[A, D, AR, DR, DE]{selected}
	if err := reference.commit(transaction, one[:]); err != nil {
		return false, err
	}
	return true, nil
}

// WithAny 执行一次不返回脱离副本的多选事务，并返回成功选择数量。
// selected 可以复用调用方切片；空结果正常回滚，重复或外来 Selection 返回 contract 并全部回滚。
func (reference *ReferenceSelector[A, D, AR, DR, DE]) WithAny(
	ctx context.Context,
	choose func(ReferenceCandidates[A, D, AR, DR, DE]) ([]ReferenceSelection[A, D, AR, DR, DE], error),
) (int, error) {
	if choose == nil {
		return 0, protocolError(codeInvalid, "callback", 0)
	}
	if reference == nil || reference.selector == nil {
		return 0, protocolError(codeClosed, "", 0)
	}
	if err := reference.selector.acquire(ctx); err != nil {
		return 0, err
	}
	defer reference.selector.release()

	transaction, candidates, err := reference.begin()
	if err != nil {
		return 0, err
	}
	defer reference.end(transaction.token)
	selected, err := choose(candidates)
	if err != nil || len(selected) == 0 {
		return 0, err
	}
	if err := ctx.Err(); err != nil {
		return 0, wrapContext(err)
	}
	if err := reference.commit(transaction, selected); err != nil {
		return 0, err
	}
	return len(selected), nil
}

// begin 初始化不构造完整 Candidate 切片的事务，并发布只在本次回调有效的 lease token。
func (reference *ReferenceSelector[A, D, AR, DR, DE]) begin() (
	*selectionTransaction[A, D],
	ReferenceCandidates[A, D, AR, DR, DE],
	error,
) {
	transaction, err := reference.selector.beginTransaction()
	if err != nil {
		return nil, ReferenceCandidates[A, D, AR, DR, DE]{}, err
	}
	reference.lease.token.Store(transaction.token)
	return transaction, ReferenceCandidates[A, D, AR, DR, DE]{
		reference:   reference,
		transaction: transaction,
		token:       transaction.token,
	}, nil
}

// end 仅撤销仍属于本次事务的 lease，避免未来改为嵌套内部流程时误关新令牌。
func (reference *ReferenceSelector[A, D, AR, DR, DE]) end(token uint64) {
	reference.lease.token.CompareAndSwap(token, 0)
}

// commit 先验证全部选择并编码所有已编辑 Data，之后才统一发布 overlay，保持多选原子性。
func (reference *ReferenceSelector[A, D, AR, DR, DE]) commit(
	transaction *selectionTransaction[A, D],
	selected []ReferenceSelection[A, D, AR, DR, DE],
) error {
	transaction.prepareSelected()
	transaction.commits = transaction.commits[:0]
	for _, selection := range selected {
		candidate := selection.candidate
		entry, ok := candidate.checkedEntry()
		if !ok || candidate.reference != reference || candidate.transaction != transaction {
			reference.clearCommits(transaction)
			return protocolError(codeContract, "candidate", 0)
		}
		if transaction.selected[candidate.index] == transaction.token {
			reference.clearCommits(transaction)
			return protocolError(codeContract, "candidate", entry.record.meta.Revision)
		}
		transaction.selected[candidate.index] = transaction.token
		if !entry.staged {
			continue
		}

		// 引用型 setter 只修改强类型事务副本；选中后统一编码一次并复用既有字段结构与容量规则。
		encoded, err := transaction.selector.encodeData(entry.data)
		if err != nil {
			reference.clearCommits(transaction)
			return err
		}
		if !sameFieldStructure(entry.dataFields, encoded) {
			reference.clearCommits(transaction)
			return protocolError(codeContract, "data", entry.record.meta.Revision)
		}
		if err := validateFields(nil, encoded, transaction.selector.core.client.limits()); err != nil {
			reference.clearCommits(transaction)
			return err
		}
		entry.dataFields = encoded
		transaction.commits = append(transaction.commits, overlayCommit[D]{
			uuid: entry.record.meta.UUID,
			overlay: localOverlay[D]{
				revision: entry.record.meta.Revision,
				data:     entry.data,
				base:     entry.record.data,
				fields:   encoded,
			},
		})
	}

	// 所有编码和规则检查成功后再改变可见 overlay；从这里开始只执行不会失败的本地赋值。
	for index := range transaction.commits {
		commit := &transaction.commits[index]
		transaction.selector.overlays[commit.uuid] = commit.overlay
	}
	reference.clearCommits(transaction)
	return nil
}

// clearCommits 清除复用提交缓冲中的 map/强类型引用，避免额外保活历史视图。
func (reference *ReferenceSelector[A, D, AR, DR, DE]) clearCommits(transaction *selectionTransaction[A, D]) {
	clear(transaction.commits)
	transaction.commits = transaction.commits[:0]
}

// checkedEntry 验证 Candidate 生命周期、索引和条目地址，再返回当前事务内值。
// 该检查只用于 Select/Edit/commit 边界，不进入只读策略遍历热路径。
func (candidate ReferenceCandidate[A, D, AR, DR, DE]) checkedEntry() (*selectionEntry[A, D], bool) {
	if candidate.reference == nil || candidate.transaction == nil || candidate.token == 0 ||
		candidate.reference.lease.token.Load() != candidate.token || candidate.transaction.token != candidate.token ||
		candidate.index < 0 || candidate.index >= len(candidate.transaction.entries) ||
		candidate.value != &candidate.transaction.entries[candidate.index] {
		return nil, false
	}
	return candidate.value, true
}

// entry 验证 Editor 生命周期和索引；任何回调外调用都返回稳定 contract 错误。
func (editor ReferenceEditor[A, D]) entry() (*selectionEntry[A, D], error) {
	if editor.transaction == nil || editor.lease == nil || editor.clone == nil || editor.token == 0 ||
		editor.lease.token.Load() != editor.token || editor.transaction.token != editor.token ||
		editor.index < 0 || editor.index >= len(editor.transaction.entries) {
		return nil, protocolError(codeContract, "candidate", 0)
	}
	return &editor.transaction.entries[editor.index], nil
}
