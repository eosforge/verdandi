package storage

import (
	"context"
	"encoding/binary"
)

// CommitBatch 在一个事务中修改同一范围的唯一键, 版本只推进一次.
// 历史以空 Key 区分 AB02 批次编码, 数量使用 uint64, 不另设整批上限, 旧二进制遇到此历史会拒绝恢复, 不会逐键重放.
func (store *Store) CommitBatch(ctx context.Context, scope Scope, version Version, changes []Change) (Version, error) {
	if len(changes) == 0 {
		return 0, ErrInput
	}
	data := []byte("AB02")
	data = binary.BigEndian.AppendUint64(data, uint64(len(changes)))
	seen := make(map[string]bool, len(changes))
	for _, change := range changes {
		if !validChange(change) || seen[change.Key] || change.Version != 0 && change.Version != version {
			return 0, ErrInput
		}
		seen[change.Key] = true
		data = binary.BigEndian.AppendUint16(data, uint16(len(change.Key)))
		data = binary.BigEndian.AppendUint32(data, uint32(len(change.Value)))
		data = append(data, byte(boolint(change.Erase)))
		data = append(data, change.Key...)
		data = append(data, change.Value...)
	}
	return store.commit(ctx, scope, Change{Version: version, Value: data})
}

// validChange 不验证外层权威版本, 批次只有一个公共版本.
func validChange(change Change) bool {
	return text(change.Key, 1024) && len(change.Value) <= 1<<20 && (!change.Erase || len(change.Value) == 0)
}

// Entries 还原一个完整提交, 载荷借用不可变历史, 不复制正文; 非法编码不返回部分结果.
func (change Change) Entries() ([]Change, error) {
	if change.Key != "" {
		if !validChange(change) {
			return nil, ErrInput
		}
		return []Change{change}, nil
	}
	data := change.Value
	if change.Erase || len(data) < 12 || string(data[:4]) != "AB02" {
		return nil, ErrInput
	}
	count := binary.BigEndian.Uint64(data[4:12])
	data = data[12:]
	// 每项至少七字节头和一个 Key 字节; 先验证实际输入, 再转 int/分配, 拒绝伪造数量.
	if count == 0 || count > uint64(len(data)/8) {
		return nil, ErrInput
	}
	entries := make([]Change, 0, int(count))
	seen := make(map[string]bool, int(count))
	for range count {
		if len(data) < 7 {
			return nil, ErrInput
		}
		key, length, erase := int(binary.BigEndian.Uint16(data[:2])), binary.BigEndian.Uint32(data[2:6]), data[6]
		data = data[7:]
		if key > len(data) || uint64(length) > uint64(len(data)-key) || erase > 1 {
			return nil, ErrInput
		}
		value := int(length) // 先与实际有界缓冲比较, 避免 32 位平台长度转换溢出.
		entry := Change{Version: change.Version, Key: string(data[:key]), Value: data[key : key+value], Erase: erase == 1}
		if !validChange(entry) || seen[entry.Key] {
			return nil, ErrInput
		}
		seen[entry.Key] = true
		entries = append(entries, entry)
		data = data[key+value:]
	}
	if len(data) != 0 {
		return nil, ErrInput
	}
	return entries, nil
}
