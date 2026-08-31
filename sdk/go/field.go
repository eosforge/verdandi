package verdandi

import "bytes"

// Fields 表示一个完整的顶层结构，字段值均为协议不解析的字节。
// 零长度字节切片是有效值而不是删除操作；字段是否存在由 map 键决定。
type Fields map[string][]byte

// Encoder 把应用值编码成完整的顶层 Fields。
// 返回后 map 与所有字节切片的所有权转移给 Verdandi；实现不得继续保留并修改这些缓冲区。
type Encoder interface {
	// Encode 返回完整字段集合；错误表示应用编码失败，Verdandi 不会执行 Redis 写入。
	Encode() (Fields, error)
}

// Decoder 用完整的顶层 Fields 替换接收者内容。
// Verdandi 传入的是脱离内部状态的副本，因此实现可在返回后继续持有 source。
type Decoder interface {
	// Decode 解码完整字段集合；失败时接收者是否保持原值由具体实现自行保证。
	Decode(source Fields) error
}

// Encode 深拷贝原始 Fields，使其可直接作为 Encoder 使用。
// 返回值不与接收者的 map 或字节切片共享可变内存。
func (fields Fields) Encode() (Fields, error) {
	return cloneFields(fields), nil
}

// Decode 接管 source 并用它替换接收者，使 *Fields 可直接作为 Decoder 使用。
// source 已由调用方脱离内部状态；nil 接收者返回 invalid 错误。
func (fields *Fields) Decode(source Fields) error {
	if fields == nil {
		return protocolError(CodeInvalid, "decoder", 0)
	}
	*fields = source
	return nil
}

// cloneFields 深拷贝 source 的 map 和每个字段值。
// 空输入返回 nil，避免为没有字段的结构分配 map。
func cloneFields(source Fields) Fields {
	if len(source) == 0 {
		return nil
	}
	result := make(Fields, len(source))
	for name, value := range source {
		result[name] = bytes.Clone(value)
	}
	return result
}
