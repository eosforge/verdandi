package catalog

import (
	"strconv"
	"sync/atomic"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

// Status 标识一个 Entry 是否包含权威当前数据，或正处于恢复/终止阶段。
type Status uint8

const (
	// StatusSynchronizing 在修复期间保留最后完整值但不宣称当前。
	StatusSynchronizing Status = iota + 1
	// StatusPresent 包含一份已同步完整值。
	StatusPresent
	// StatusAbsent 表示已同步缺失且没有保留删除 revision。
	StatusAbsent
	// StatusDeleted 表示已同步缺失并持有已知删除 revision。
	StatusDeleted
	// StatusUnavailable 表示修复暂时无法推进，同时保留最后完整值。
	StatusUnavailable
	// StatusClosed 是 Subscriber 关闭后的终止状态。
	StatusClosed
)

// rawState 是原子发布到 Entry 的不可变内部状态。
type rawState struct {
	revision        uint64
	replaceRevision uint64
	status          Status
	kind            Kind
	encodedBytes    int
	fields          verdandi.Fields
}

// initialState 创建只带 status 的空不可变状态。
func initialState(status Status) *rawState {
	return &rawState{status: status}
}

// withStatus 浅复制 state 并替换状态；字段 map 保持内部不可变共享。
func (state *rawState) withStatus(status Status) *rawState {
	if state == nil {
		return initialState(status)
	}
	next := *state
	next.status = status
	return &next
}

// Entry 在 Replace、Patch、Delete、恢复和重建期间始终绑定同一个 Path。
// 调用方可长期保存指针，并通过原子方法读取最新本地状态。
type Entry struct {
	path  Path
	state atomic.Pointer[rawState]
}

// newEntry 创建绑定 path、初始为 status 的稳定 Entry。
func newEntry(path Path, status Status) *Entry {
	entry := &Entry{path: path}
	entry.state.Store(initialState(status))
	return entry
}

// Path 返回 Entry 的不可变身份；nil 接收者返回零值 Path。
func (entry *Entry) Path() Path {
	if entry == nil {
		return Path{}
	}
	return entry.path
}

// Status 返回当前本地同步状态；nil 接收者返回 Closed。
func (entry *Entry) Status() Status {
	if entry == nil {
		return StatusClosed
	}
	state := entry.state.Load()
	if state == nil {
		return StatusSynchronizing
	}
	return state.status
}

// Revision 返回该 Path 最后已知完整 revision；尚无完整状态时返回零。
func (entry *Entry) Revision() uint64 {
	if entry == nil {
		return 0
	}
	state := entry.state.Load()
	if state == nil {
		return 0
	}
	return state.revision
}

// Synchronized 报告 Status 是否为 Present、Absent 或 Deleted。
func (entry *Entry) Synchronized() bool {
	return synchronizedStatus(entry.Status())
}

// deletedState 根据 revision 构造 Deleted；零 revision 表示没有 tombstone 的 Absent。
func deletedState(revision uint64) *rawState {
	if revision == 0 {
		return &rawState{status: StatusAbsent}
	}
	return &rawState{revision: revision, status: StatusDeleted}
}

// formatRevision 返回 revision 的规范十进制文本。
func formatRevision(value uint64) string {
	return strconv.FormatUint(value, 10)
}
