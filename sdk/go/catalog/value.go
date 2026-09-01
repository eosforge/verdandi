package catalog

import (
	"bytes"
	"reflect"
	"sort"
	"strings"
	"unicode/utf8"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

// Kind 标识 Catalog 值的规范顶层表示。
type Kind uint8

const (
	// Value 恰好包含一个名为 value 的应用字段。
	Value Kind = iota + 1
	// Array 是从 0 开始、字段名为规范十进制索引的连续序列。
	Array
	// Map 是应用命名字段集合。
	Map
)

// text 返回 Kind 的稳定协议字符串；未知值返回空字符串。
func (kind Kind) text() string {
	switch kind {
	case Value:
		return "value"
	case Array:
		return "array"
	case Map:
		return "map"
	default:
		return ""
	}
}

// parseKind 把稳定协议字符串解析为 Kind。
func parseKind(value string) (Kind, bool) {
	switch value {
	case "value":
		return Value, true
	case "array":
		return Array, true
	case "map":
		return Map, true
	default:
		return 0, false
	}
}

// encodeValue 调用应用 Encoder，并拒绝 nil 接口或带类型的 nil 值。
func encodeValue(value verdandi.Encoder) (verdandi.Fields, error) {
	if value == nil || nilInterface(value) {
		return nil, newError(verdandi.CodeInvalid, "value", 0, nil)
	}
	fields, err := value.Encode()
	if err != nil {
		return nil, newError(verdandi.CodeInvalid, "value", 0, err)
	}
	return fields, nil
}

// nilInterface 判断动态值是否属于可为 nil 的种类且当前为 nil。
func nilInterface(value any) bool {
	reflected := reflect.ValueOf(value)
	switch reflected.Kind() {
	case reflect.Chan, reflect.Func, reflect.Interface, reflect.Map,
		reflect.Pointer, reflect.Slice:
		return reflected.IsNil()
	default:
		return false
	}
}

// validateValue 校验 kind 对应的完整字段形状、名称、连续性和总编码字节。
// 返回确定字段顺序、完整大小和错误；fields 的字节所有权不会在此复制。
func validateValue(kind Kind, fields verdandi.Fields, maximumBytes int) ([]string, int, error) {
	if kind.text() == "" {
		return nil, 0, newError(verdandi.CodeInvalid, "kind", 0, nil)
	}
	if len(fields) > maximumFields {
		return nil, 0, newError(verdandi.CodeCapacity, "fields", 0, nil)
	}
	if kind == Value {
		field, exists := fields["value"]
		if len(fields) != 1 || !exists {
			return nil, 0, newError(verdandi.CodeContract, "value", 0, nil)
		}
		size := len("value") + len(field)
		if size > maximumBytes {
			return nil, 0, newError(verdandi.CodeCapacity, "value", 0, nil)
		}
		return []string{"value"}, size, nil
	}
	if kind == Array {
		names := make([]string, len(fields))
		size := 0
		for name, field := range fields {
			index, ok := arrayFieldIndex(name, len(fields))
			if !ok || names[index] != "" {
				return nil, 0, newError(verdandi.CodeContract, "array", 0, nil)
			}
			if len(name) > maximumBytes-size ||
				len(field) > maximumBytes-size-len(name) {
				return nil, 0, newError(verdandi.CodeCapacity, "value", 0, nil)
			}
			names[index] = name
			size += len(name) + len(field)
		}
		return names, size, nil
	}
	names := sortedNames(fields)
	size := 0
	for _, name := range names {
		field := fields[name]
		if !validFieldName(name) {
			return nil, 0, newError(verdandi.CodeInvalid, name, 0, nil)
		}
		if len(name) > maximumBytes-size || len(field) > maximumBytes-size-len(name) {
			return nil, 0, newError(verdandi.CodeCapacity, "value", 0, nil)
		}
		size += len(name) + len(field)
	}
	return names, size, nil
}

// arrayFieldIndex 解析无前导零的规范数组索引，并要求索引落在 [0,count) 内。
func arrayFieldIndex(name string, count int) (int, bool) {
	if count == 0 || name == "" || name[0] == '0' && name != "0" {
		return 0, false
	}
	value := 0
	for index := range len(name) {
		digit := int(name[index] - '0')
		if digit < 0 || digit > 9 || value > (count-1-digit)/10 {
			return 0, false
		}
		value = value*10 + digit
	}
	return value, value < count
}

// validatePatchFields 校验非空 Patch 的字段名、数量和增量字节上限，并返回排序名称。
func validatePatchFields(fields verdandi.Fields, maximumBytes int) ([]string, error) {
	if len(fields) == 0 {
		return nil, newError(verdandi.CodeInvalid, "patch", 0, nil)
	}
	if len(fields) > maximumFields {
		return nil, newError(verdandi.CodeCapacity, "fields", 0, nil)
	}
	names := sortedNames(fields)
	size := 0
	for _, name := range names {
		value := fields[name]
		if !validFieldName(name) {
			return nil, newError(verdandi.CodeInvalid, name, 0, nil)
		}
		if len(name) > maximumBytes-size || len(value) > maximumBytes-size-len(name) {
			return nil, newError(verdandi.CodeCapacity, "patch", 0, nil)
		}
		size += len(name) + len(value)
	}
	return names, nil
}

// validFieldName 要求非空 UTF-8，且不得使用 @ 保留前缀。
func validFieldName(name string) bool {
	return name != "" && utf8.ValidString(name) && !strings.HasPrefix(name, "@")
}

// sortedNames 返回字段 map 的字节字典序名称切片。
func sortedNames(fields verdandi.Fields) []string {
	names := make([]string, 0, len(fields))
	for name := range fields {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// cloneFields 深拷贝字段 map 和全部值缓冲区；空输入返回 nil。
func cloneFields(source verdandi.Fields) verdandi.Fields {
	if len(source) == 0 {
		return nil
	}
	result := make(verdandi.Fields, len(source))
	for name, value := range source {
		result[name] = bytes.Clone(value)
	}
	return result
}
