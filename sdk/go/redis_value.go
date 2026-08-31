package verdandi

import (
	"bytes"
	"encoding"
	"reflect"
	"strconv"
)

// 以下反射类型在进程内固定，集中缓存可避免每次编解码重复构造接口类型。
var (
	binaryMarshalerType   = reflect.TypeFor[encoding.BinaryMarshaler]()
	binaryUnmarshalerType = reflect.TypeFor[encoding.BinaryUnmarshaler]()
	textMarshalerType     = reflect.TypeFor[encoding.TextMarshaler]()
	textUnmarshalerType   = reflect.TypeFor[encoding.TextUnmarshaler]()
)

// encodeRedisValue 校验泛型 T 的编码契约，并把 value 编码为独立字节切片。
// field 仅用于稳定错误定位；不写入编码结果。
func encodeRedisValue[T any](value T, field string) ([]byte, error) {
	typeOfValue := reflect.TypeFor[T]()
	if !supportsRedisEncode(typeOfValue) {
		return nil, protocolError(CodeContract, field, 0)
	}
	return encodeRedisReflectValue(reflect.ValueOf(&value).Elem(), field)
}

// decodeRedisValue 校验泛型 T 的解码契约，并从 source 构造一个完整的新值。
// 失败时返回 T 的零值，绝不暴露部分解码结果。
func decodeRedisValue[T any](source []byte, field string) (T, error) {
	var value T
	typeOfValue := reflect.TypeFor[T]()
	if !supportsRedisDecode(typeOfValue) {
		return value, protocolError(CodeContract, field, 0)
	}
	if len(source) > maxRedisValueBytes {
		return value, protocolError(CodeCapacity, field, 0)
	}
	if err := decodeRedisReflectValue(reflect.ValueOf(&value).Elem(), source, field); err != nil {
		var zero T
		return zero, err
	}
	return value, nil
}

// supportsRedisEncode 判断 valueType 是否可由内置标量规则或指针接收者的标准编码接口编码。
func supportsRedisEncode(valueType reflect.Type) bool {
	if valueType == nil || valueType.Kind() == reflect.Pointer {
		return false
	}
	pointerType := reflect.PointerTo(valueType)
	if pointerType.Implements(binaryMarshalerType) || pointerType.Implements(textMarshalerType) {
		return true
	}
	return supportedRedisScalarKind(valueType)
}

// supportsRedisDecode 判断 valueType 是否可由内置标量规则或指针接收者的标准解码接口解码。
func supportsRedisDecode(valueType reflect.Type) bool {
	if valueType == nil || valueType.Kind() == reflect.Pointer {
		return false
	}
	pointerType := reflect.PointerTo(valueType)
	if pointerType.Implements(binaryUnmarshalerType) || pointerType.Implements(textUnmarshalerType) {
		return true
	}
	return supportedRedisScalarKind(valueType)
}

// supportedRedisScalarKind 判断 valueType 是否属于跨平台宽度固定的内置 Redis 标量集合。
// int、uint、uintptr、浮点数和非字节切片均被有意排除。
func supportedRedisScalarKind(valueType reflect.Type) bool {
	switch valueType.Kind() {
	case reflect.Bool, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
		reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.String:
		return true
	case reflect.Slice:
		return valueType.Elem().Kind() == reflect.Uint8
	default:
		return false
	}
}

// encodeRedisReflectValue 按二进制接口、文本接口、内置标量的优先级编码 value。
// 返回字节始终受 maxRedisValueBytes 限制，并与应用编码器的缓冲区解除别名。
func encodeRedisReflectValue(value reflect.Value, field string) ([]byte, error) {
	// 标准接口允许应用定义稳定结构；二进制编码优先于文本编码。
	address := value.Addr().Interface()
	if marshaler, ok := address.(encoding.BinaryMarshaler); ok {
		encoded, err := marshaler.MarshalBinary()
		return finishRedisEncoding(encoded, field, err)
	}
	if marshaler, ok := address.(encoding.TextMarshaler); ok {
		encoded, err := marshaler.MarshalText()
		return finishRedisEncoding(encoded, field, err)
	}

	// 内置类型使用规范化形式，保证不同 SDK 或重复写入得到相同字节。
	var encoded []byte
	switch value.Kind() {
	case reflect.Bool:
		if value.Bool() {
			encoded = []byte{'1'}
		} else {
			encoded = []byte{'0'}
		}
	case reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		encoded = strconv.AppendInt(nil, value.Int(), 10)
	case reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		encoded = strconv.AppendUint(nil, value.Uint(), 10)
	case reflect.String:
		encoded = []byte(value.String())
	case reflect.Slice:
		encoded = bytes.Clone(value.Bytes())
	default:
		return nil, protocolError(CodeContract, field, 0)
	}
	if len(encoded) > maxRedisValueBytes {
		return nil, protocolError(CodeCapacity, field, 0)
	}
	return encoded, nil
}

// finishRedisEncoding 统一处理应用编码器错误、容量检查和输出缓冲区脱离。
func finishRedisEncoding(encoded []byte, field string, err error) ([]byte, error) {
	if err != nil {
		return nil, &Error{Code: CodeContract, Field: field, Cause: err}
	}
	if len(encoded) > maxRedisValueBytes {
		return nil, protocolError(CodeCapacity, field, 0)
	}
	return bytes.Clone(encoded), nil
}

// decodeRedisReflectValue 按二进制接口、文本接口、内置标量的优先级替换 value。
// field 用于损坏或契约错误定位；source 的所有权不会转移给应用解码器之外的状态。
func decodeRedisReflectValue(value reflect.Value, source []byte, field string) error {
	// 应用标准接口优先，可为自定义类型提供稳定的外部表示。
	address := value.Addr().Interface()
	if unmarshaler, ok := address.(encoding.BinaryUnmarshaler); ok {
		if err := unmarshaler.UnmarshalBinary(source); err != nil {
			return &Error{Code: CodeCorrupt, Field: field, Cause: err}
		}
		return nil
	}
	if unmarshaler, ok := address.(encoding.TextUnmarshaler); ok {
		if err := unmarshaler.UnmarshalText(source); err != nil {
			return &Error{Code: CodeCorrupt, Field: field, Cause: err}
		}
		return nil
	}

	// 数值仅接受重新格式化后完全相同的规范十进制文本，拒绝空白、加号和前导零。
	switch value.Kind() {
	case reflect.Bool:
		switch string(source) {
		case "0":
			value.SetBool(false)
		case "1":
			value.SetBool(true)
		default:
			return protocolError(CodeCorrupt, field, 0)
		}
	case reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		text := string(source)
		decoded, err := strconv.ParseInt(text, 10, value.Type().Bits())
		if err != nil || strconv.FormatInt(decoded, 10) != text {
			return protocolError(CodeCorrupt, field, 0)
		}
		value.SetInt(decoded)
	case reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		text := string(source)
		decoded, err := strconv.ParseUint(text, 10, value.Type().Bits())
		if err != nil || strconv.FormatUint(decoded, 10) != text {
			return protocolError(CodeCorrupt, field, 0)
		}
		value.SetUint(decoded)
	case reflect.String:
		value.SetString(string(source))
	case reflect.Slice:
		value.SetBytes(bytes.Clone(source))
	default:
		return protocolError(CodeContract, field, 0)
	}
	return nil
}
