package catalog

import (
	"strings"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

type eventKind uint8

const (
	eventReplace eventKind = iota + 1
	eventPatch
	eventDelete
)

type catalogEvent struct {
	kind         eventKind
	path         Path
	revision     uint64
	baseRevision uint64
	valueKind    Kind
	encodedBytes int
	fields       verdandi.Fields
}

type eventDecoder struct {
	payload string
	offset  int
}

// decodeEvent 解码并校验单条 Catalog Pub/Sub 通知。
// payload 是 Redis 返回的 MessagePack 原始消息；expectedPath 是订阅者绑定的目标，
// 用于拒绝投递到错误频道或成员的消息；maximumBytes 限制解码后完整值的逻辑大小。
// 返回值只会包含通过协议版本、目标、revision、字段顺序和容量校验的完整事件。
func decodeEvent(payload string, expectedPath Path, maximumBytes int) (catalogEvent, error) {
	// 先按最坏编码开销拒绝异常大消息，避免在解析恶意负载前占用过多 CPU。
	if len(payload) > maximumEventPayload(maximumBytes) {
		return catalogEvent{}, newError(verdandi.CodeCapacity, "notification", 0, nil)
	}
	decoder := eventDecoder{payload: payload}
	count, ok := decoder.arrayLength()
	if !ok || count < 4 {
		return catalogEvent{}, newError(verdandi.CodeCorrupt, "notification", 0, nil)
	}
	protocol, ok := decoder.text()
	if !ok || protocol != "v1" {
		return catalogEvent{}, newError(verdandi.CodeProtocol, "protocol", 0, nil)
	}
	operation, ok := decoder.text()
	if !ok {
		return catalogEvent{}, newError(verdandi.CodeCorrupt, "operation", 0, nil)
	}
	member, ok := decoder.text()
	if !ok || !expectedPath.matchesMember(member) {
		return catalogEvent{}, newError(verdandi.CodeTarget, "path", 0, nil)
	}

	// 公共头部确认后再按操作类型解析固定 ABI，任何分支都必须恰好消费全部元素。
	event := catalogEvent{path: expectedPath}
	var err error
	switch operation {
	case "replace":
		if count != 7 {
			return catalogEvent{}, newError(verdandi.CodeCorrupt, "notification", 0, nil)
		}
		event.kind = eventReplace
		event.revision, err = decodeEventRevision(&decoder, "@revision")
		if err != nil {
			return catalogEvent{}, err
		}
		kindText, kindOK := decoder.text()
		event.valueKind, kindOK = parseKind(kindText)
		if !kindOK {
			return catalogEvent{}, newError(verdandi.CodeCorrupt, "@kind", event.revision, nil)
		}
		bytesText, bytesOK := decoder.text()
		if !bytesOK {
			return catalogEvent{}, newError(
				verdandi.CodeCorrupt, "@encoded_bytes", event.revision, nil,
			)
		}
		event.encodedBytes, err = parseInteger(bytesText, "@encoded_bytes", maximumBytes)
		if err != nil {
			return catalogEvent{}, err
		}
		event.fields, err = decodeEventFields(&decoder, event.valueKind == Array)
		if err != nil {
			return catalogEvent{}, err
		}
		_, actual, validateErr := validateValue(event.valueKind, event.fields, maximumBytes)
		if validateErr != nil || actual != event.encodedBytes {
			return catalogEvent{}, newError(
				verdandi.CodeCorrupt, "@encoded_bytes", event.revision, validateErr,
			)
		}
	case "patch":
		if count != 8 {
			return catalogEvent{}, newError(verdandi.CodeCorrupt, "notification", 0, nil)
		}
		event.kind = eventPatch
		event.baseRevision, err = decodeEventRevision(&decoder, "@base_revision")
		if err != nil {
			return catalogEvent{}, err
		}
		event.revision, err = decodeEventRevision(&decoder, "@revision")
		if err != nil || event.revision <= event.baseRevision {
			return catalogEvent{}, newError(
				verdandi.CodeCorrupt, "@revision", event.revision, err,
			)
		}
		kindText, kindOK := decoder.text()
		event.valueKind, kindOK = parseKind(kindText)
		if !kindOK || event.valueKind == Value {
			return catalogEvent{}, newError(verdandi.CodeCorrupt, "@kind", event.revision, nil)
		}
		bytesText, bytesOK := decoder.text()
		if !bytesOK {
			return catalogEvent{}, newError(
				verdandi.CodeCorrupt, "@encoded_bytes", event.revision, nil,
			)
		}
		event.encodedBytes, err = parseInteger(bytesText, "@encoded_bytes", maximumBytes)
		if err != nil {
			return catalogEvent{}, err
		}
		event.fields, err = decodeEventFields(&decoder, false)
		if err != nil {
			return catalogEvent{}, err
		}
		if _, err := validatePatchFields(event.fields, maximumBytes); err != nil {
			return catalogEvent{}, err
		}
	case "delete":
		if count != 4 {
			return catalogEvent{}, newError(verdandi.CodeCorrupt, "notification", 0, nil)
		}
		event.kind = eventDelete
		event.revision, err = decodeEventRevision(&decoder, "@revision")
		if err != nil {
			return catalogEvent{}, err
		}
	default:
		return catalogEvent{}, newError(verdandi.CodeProtocol, "operation", 0, nil)
	}
	if !decoder.done() {
		return catalogEvent{}, newError(verdandi.CodeCorrupt, "notification", 0, nil)
	}
	return event, nil
}

// maximumEventPayload 计算通知的保守线长上限。
// maximumBytes 是值内容上限；返回值额外容纳字段名称、MessagePack 容器头和协议元数据。
func maximumEventPayload(maximumBytes int) int {
	fields := maximumBytes
	if fields > maximumFields {
		fields = maximumFields
	}
	return maximumBytes + fields*10 + 1024
}

// decodeEventRevision 从 decoder 读取一个正 revision。
// field 仅用于精确标记错误来源；成功时返回协议允许范围内的版本号。
func decodeEventRevision(decoder *eventDecoder, field string) (uint64, error) {
	text, ok := decoder.text()
	if !ok {
		return 0, newError(verdandi.CodeCorrupt, field, 0, nil)
	}
	revision, err := parseRevision(text, false)
	if err != nil {
		return 0, newError(verdandi.CodeCorrupt, field, 0, err)
	}
	return revision, nil
}

// decodeEventFields 解码按名称/值交替排列的字段数组。
// arrayReplace 为 true 时要求名称严格为连续数组下标；否则要求字段名严格递增。
// 返回的字段名和值由本次事件私有存储承载，值切片容量被截断到长度，调用者追加时
// 不会覆盖相邻字段；任何结构、顺序或数量错误都会使整条事件失败。
func decodeEventFields(decoder *eventDecoder, arrayReplace bool) (verdandi.Fields, error) {
	count, ok := decoder.arrayLength()
	if !ok || count%2 != 0 || count/2 > maximumFields {
		return nil, newError(verdandi.CodeCorrupt, "fields", 0, nil)
	}
	fieldCount := int(count / 2)
	remaining := len(decoder.payload) - decoder.offset
	nameCapacity := fieldCount * 16
	if nameCapacity > remaining {
		nameCapacity = remaining
	}
	// 字段名和值分别写入共享连续存储；暴露的值切片把容量截断到自身长度，
	// 从而兼顾低分配和字段间的追加隔离。
	var names strings.Builder
	names.Grow(nameCapacity)
	valueStorage := make([]byte, 0, remaining)
	fields := make(verdandi.Fields, fieldCount)
	previous := ""
	for expectedIndex := range fieldCount {
		name, nameOK := decoder.text()
		value, valueOK := decoder.rawBytes()
		if !nameOK || !valueOK || name == "" {
			return nil, newError(verdandi.CodeCorrupt, "fields", 0, nil)
		}
		if arrayReplace {
			index, canonical := arrayFieldIndex(name, fieldCount)
			if !canonical || index != expectedIndex {
				return nil, newError(verdandi.CodeCorrupt, name, 0, nil)
			}
		} else if previous != "" && previous >= name {
			return nil, newError(verdandi.CodeCorrupt, name, 0, nil)
		}
		previous = name
		nameOffset := names.Len()
		names.WriteString(name)
		nameStorage := names.String()
		valueOffset := len(valueStorage)
		valueStorage = append(valueStorage, value...)
		valueEnd := len(valueStorage)
		fields[nameStorage[nameOffset:]] = valueStorage[valueOffset:valueEnd:valueEnd]
	}
	return fields, nil
}

// done 报告 decoder 是否已经精确消费整个 payload。
func (decoder *eventDecoder) done() bool {
	return decoder.offset == len(decoder.payload)
}

// arrayLength 读取 MessagePack 数组头并返回元素数量。
// 返回 false 表示输入耗尽、长度头不完整或下一个值不是数组。
func (decoder *eventDecoder) arrayLength() (uint64, bool) {
	code, ok := decoder.code()
	if !ok {
		return 0, false
	}
	switch {
	case code >= 0x90 && code <= 0x9f:
		return uint64(code & 0x0f), true
	case code == 0xdc:
		value, ok := decoder.take(2)
		if !ok {
			return 0, false
		}
		return uint64(value[0])<<8 | uint64(value[1]), true
	case code == 0xdd:
		value, ok := decoder.take(4)
		if !ok {
			return 0, false
		}
		return uint64(value[0])<<24 | uint64(value[1])<<16 |
			uint64(value[2])<<8 | uint64(value[3]), true
	default:
		return 0, false
	}
}

// text 读取一个 MessagePack 字符串或二进制值并返回其零拷贝字符串视图。
func (decoder *eventDecoder) text() (string, bool) {
	return decoder.rawBytes()
}

// rawBytes 读取 MessagePack 字符串或二进制载荷。
// 返回的字符串直接引用 payload；调用方不得假定其生命周期长于 decoder.payload。
func (decoder *eventDecoder) rawBytes() (string, bool) {
	code, ok := decoder.code()
	if !ok {
		return "", false
	}
	var length int
	switch {
	case code >= 0xa0 && code <= 0xbf:
		length = int(code & 0x1f)
	case code == 0xc4 || code == 0xd9:
		value, ok := decoder.take(1)
		if !ok {
			return "", false
		}
		length = int(value[0])
	case code == 0xc5 || code == 0xda:
		value, ok := decoder.take(2)
		if !ok {
			return "", false
		}
		length = int(value[0])<<8 | int(value[1])
	case code == 0xc6 || code == 0xdb:
		value, ok := decoder.take(4)
		if !ok {
			return "", false
		}
		encodedLength := uint64(value[0])<<24 | uint64(value[1])<<16 |
			uint64(value[2])<<8 | uint64(value[3])
		if encodedLength > uint64(^uint(0)>>1) {
			return "", false
		}
		length = int(encodedLength)
	default:
		return "", false
	}
	return decoder.take(length)
}

// code 读取当前位置的一个 MessagePack 类型字节并推进 offset。
func (decoder *eventDecoder) code() (byte, bool) {
	if decoder.offset >= len(decoder.payload) {
		return 0, false
	}
	value := decoder.payload[decoder.offset]
	decoder.offset++
	return value, true
}

// take 从当前位置取得 length 字节的零拷贝字符串视图并推进 offset。
// length 为负数或超过剩余负载时返回 false，且不推进位置。
func (decoder *eventDecoder) take(length int) (string, bool) {
	remaining := len(decoder.payload) - decoder.offset
	if length < 0 || length > remaining {
		return "", false
	}
	end := decoder.offset + length
	value := decoder.payload[decoder.offset:end]
	decoder.offset = end
	return value, true
}

// eventString 把 go-redis 可能返回的 string 或 []byte 统一为字符串。
// 其他类型返回 false，避免用格式化转换掩盖协议损坏。
func eventString(value any) (string, bool) {
	switch value := value.(type) {
	case string:
		return value, true
	case []byte:
		return string(value), true
	default:
		return "", false
	}
}

// eventBytes 把 go-redis 可能返回的 string 或 []byte 复制为调用方独占字节切片。
// 复制用于隔离驱动缓冲区；其他类型返回 false。
func eventBytes(value any) ([]byte, bool) {
	switch value := value.(type) {
	case string:
		return []byte(value), true
	case []byte:
		return append([]byte(nil), value...), true
	default:
		return nil, false
	}
}
