package catalog

import (
	verdandi "github.com/LaconisIves/verdandi/sdk/go"
)

// Snapshot 是 Entry 当前本地状态的一份独立强类型投影。
type Snapshot[T any] struct {
	// Revision 是该快照表示的最后一个完整 revision。
	Revision uint64
	// Status 描述同步、存在和删除状态。
	Status Status
	// Synchronized 只在 Present、Absent 或 Deleted 时为 true。
	Synchronized bool
	// Value 在存在完整值时非 nil；Synchronizing、Unavailable 或 Closed 时可能保留最后完整但陈旧的数据。
	Value *T
}

// Load 使用调用方选择的应用类型解码 Entry 当前状态，且不执行 Redis 或磁盘 I/O。
// P 从 *T 推导，调用方只需指定 T；解码输入是独立字段副本，失败不会修改 Entry。
func (entry *Entry) Load[
	T any,
	P interface {
		*T
		verdandi.Decoder
	},
]() (Snapshot[T], error) {
	if entry == nil {
		return Snapshot[T]{Status: StatusClosed},
			newError(verdandi.CodeInvalid, "entry", 0, nil)
	}
	state := entry.state.Load()
	if state == nil {
		return Snapshot[T]{Status: StatusSynchronizing}, nil
	}
	snapshot := Snapshot[T]{
		Revision:     state.revision,
		Status:       state.status,
		Synchronized: synchronizedStatus(state.status),
	}
	if state.kind == 0 {
		return snapshot, nil
	}
	var value T
	if err := P(&value).Decode(cloneFields(state.fields)); err != nil {
		return snapshot, newError(verdandi.CodeCorrupt, "value", state.revision, err)
	}
	snapshot.Value = &value
	return snapshot, nil
}

// synchronizedStatus 判断状态是否代表已通过权威同步的 Present、Absent 或 Deleted。
func synchronizedStatus(status Status) bool {
	switch status {
	case StatusPresent, StatusAbsent, StatusDeleted:
		return true
	default:
		return false
	}
}
