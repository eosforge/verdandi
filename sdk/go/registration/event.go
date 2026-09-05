package registration

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"strings"
	"unicode/utf8"

	"github.com/eosforge/verdandi/sdk/go/internal/validate"
	"github.com/vmihailenco/msgpack/v5/msgpcode"
)

const maxRegistrationEventBytes = 128 * 1024

// registrationEvent 是 Registry Pub/Sub 消息解码后的拥有型表示。
// Attr/Data 字节已从输入 payload 脱离，可安全进入跨同步阶段的合并缓冲区。
type registrationEvent struct {
	kind       string
	uuid       string
	revision   uint64
	timestamp  uint64
	ttl        uint64
	version    uint64
	hasVersion bool
	attr       fields
	data       fields
}

// decodeRegistrationEvent 对一个受限 MessagePack 交替数组执行单遍解码和完整语义校验。
// payload 来自不可信 Pub/Sub；limits 是当前协议/Zone 上限。错误时不返回部分事件，也不进行无界分配。
func decodeRegistrationEvent(payload []byte, limits zoneConfig) (registrationEvent, error) {
	if len(payload) == 0 || len(payload) > maxRegistrationEventBytes {
		return registrationEvent{}, protocolError(codeCapacity, "event", 0)
	}
	decoder := eventDecoder{input: payload}
	elements, err := decoder.arrayLen()
	if err != nil || elements < 0 || elements%2 != 0 {
		return registrationEvent{}, protocolError(codeCorrupt, "event", 0)
	}
	if elements > (protocolAttrFields+protocolDataFields+7)*2 {
		return registrationEvent{}, protocolError(codeCapacity, "event", 0)
	}
	// 固定字段使用位图拒绝重复，未知控制字段可跳过标量以保留向前兼容性。
	const (
		seenProtocol uint8 = 1 << iota
		seenKind
		seenUUID
		seenRevision
		seenTimestamp
		seenTTL
		seenVersion
	)
	var seen uint8
	var unknown map[string]struct{}
	event := registrationEvent{}
	for index := 0; index < elements; index += 2 {
		nameBytes, decodeErr := decoder.bytes()
		if decodeErr != nil || len(nameBytes) == 0 || !utf8.Valid(nameBytes) {
			return registrationEvent{}, protocolError(codeCorrupt, "event", 0)
		}
		var marker uint8
		switch {
		case bytes.Equal(nameBytes, []byte("&protocol")):
			marker = seenProtocol
		case bytes.Equal(nameBytes, []byte("&kind")):
			marker = seenKind
		case bytes.Equal(nameBytes, []byte("@uuid")):
			marker = seenUUID
		case bytes.Equal(nameBytes, []byte("@revision")):
			marker = seenRevision
		case bytes.Equal(nameBytes, []byte("@timestamp")):
			marker = seenTimestamp
		case bytes.Equal(nameBytes, []byte("@ttl")):
			marker = seenTTL
		case bytes.Equal(nameBytes, []byte("@version")):
			marker = seenVersion
		}
		if marker != 0 {
			if seen&marker != 0 {
				return registrationEvent{}, protocolError(codeContract, string(nameBytes), 0)
			}
			seen |= marker
		}
		switch marker {
		case seenProtocol:
			protocol, valueErr := decoder.bytes()
			if valueErr != nil || !bytes.Equal(protocol, []byte(protocolVersion)) {
				return registrationEvent{}, protocolError(codeProtocol, "&protocol", 0)
			}
		case seenKind:
			kind, valueErr := decoder.bytes()
			if valueErr != nil {
				return registrationEvent{}, protocolError(codeCorrupt, "&kind", 0)
			}
			event.kind = string(kind)
		case seenUUID:
			uuid, valueErr := decoder.bytes()
			if valueErr != nil || !validUUIDBytes(uuid) {
				return registrationEvent{}, protocolError(codeInvalid, "@uuid", 0)
			}
			event.uuid = string(uuid)
		case seenRevision:
			event.revision, err = decoder.uint()
			if err != nil {
				return registrationEvent{}, protocolError(codeInvalid, "@revision", 0)
			}
		case seenTimestamp:
			event.timestamp, err = decoder.uint()
			if err != nil {
				return registrationEvent{}, protocolError(codeInvalid, "@timestamp", 0)
			}
		case seenTTL:
			event.ttl, err = decoder.uint()
			if err != nil {
				return registrationEvent{}, protocolError(codeInvalid, "@ttl", 0)
			}
		case seenVersion:
			event.version, err = decoder.uint()
			event.hasVersion = err == nil
			if err != nil {
				return registrationEvent{}, protocolError(codeInvalid, "@version", 0)
			}
		default:
			// 未知 &/@ 控制字段只允许有界标量；应用字段必须是字节并按前缀归入 Attr 或 Data。
			if nameBytes[0] == '&' || nameBytes[0] == '@' {
				name := string(nameBytes)
				if unknown == nil {
					unknown = make(map[string]struct{})
				}
				if _, exists := unknown[name]; exists {
					return registrationEvent{}, protocolError(codeContract, name, 0)
				}
				unknown[name] = struct{}{}
				if err := decoder.skipScalar(); err != nil {
					return registrationEvent{}, protocolError(codeCorrupt, name, 0)
				}
				continue
			}
			encoded, valueErr := decoder.bytes()
			if valueErr != nil {
				return registrationEvent{}, protocolError(codeCorrupt, string(nameBytes), 0)
			}
			if nameBytes[0] == '.' {
				field := string(nameBytes[1:])
				if err := validateApplicationField(field, encoded, limits.fieldNameMaxBytes, limits.attrValueMaxBytes); err != nil {
					return registrationEvent{}, err
				}
				if event.attr == nil {
					event.attr = make(fields)
				}
				if _, exists := event.attr[field]; exists {
					return registrationEvent{}, protocolError(codeContract, "."+field, 0)
				}
				event.attr[field] = bytes.Clone(encoded)
			} else {
				name := string(nameBytes)
				if err := validateApplicationField(name, encoded, limits.fieldNameMaxBytes, limits.dataValueMaxBytes); err != nil {
					return registrationEvent{}, err
				}
				if event.data == nil {
					event.data = make(fields)
				}
				if _, exists := event.data[name]; exists {
					return registrationEvent{}, protocolError(codeContract, name, 0)
				}
				event.data[name] = bytes.Clone(encoded)
			}
		}
	}
	if !decoder.done() {
		return registrationEvent{}, protocolError(codeCorrupt, "event", 0)
	}
	if seen&seenProtocol == 0 {
		return registrationEvent{}, protocolError(codeCorrupt, "&protocol", 0)
	}
	if seen&seenKind == 0 {
		return registrationEvent{}, protocolError(codeCorrupt, "&kind", 0)
	}
	if seen&seenUUID == 0 {
		return registrationEvent{}, protocolError(codeCorrupt, "@uuid", 0)
	}

	// 四种生命周期事件各自拥有严格字段集合，防止部分消息被误当作另一种操作。
	switch event.kind {
	case "register":
		if event.revision == 0 || event.timestamp == 0 || event.ttl == 0 || !event.hasVersion {
			return registrationEvent{}, protocolError(codeContract, "register", 0)
		}
		if err := validateRecord(event.uuid, event.revision, event.ttl, event.version, event.attr, event.data, limits); err != nil {
			return registrationEvent{}, err
		}
	case "update":
		if event.revision == 0 || event.timestamp == 0 || len(event.attr) != 0 || (!event.hasVersion && len(event.data) == 0) {
			return registrationEvent{}, protocolError(codeContract, "update", 0)
		}
	case "renew":
		if event.revision == 0 || event.timestamp == 0 || event.ttl != 0 || event.hasVersion || len(event.attr) != 0 || len(event.data) != 0 {
			return registrationEvent{}, protocolError(codeContract, "renew", 0)
		}
	case "unregister":
		if event.revision != 0 || event.timestamp != 0 || event.ttl != 0 || event.hasVersion || len(event.attr) != 0 || len(event.data) != 0 {
			return registrationEvent{}, protocolError(codeContract, "unregister", 0)
		}
	default:
		return registrationEvent{}, protocolError(codeInvalid, "&kind", 0)
	}
	return event, nil
}

// eventDecoder 是专为 Registry 平面事件设计的无反射、有界 MessagePack 游标。
// 它不支持容器嵌套，所有 take 都先验证剩余输入长度。
type eventDecoder struct {
	input  []byte
	offset int
}

// arrayLen 读取顶层数组长度，并在 array32 宣称超大长度时返回明确超限哨兵。
func (decoder *eventDecoder) arrayLen() (int, error) {
	code, err := decoder.byte()
	if err != nil {
		return 0, err
	}
	switch {
	case msgpcode.IsFixedArray(code):
		return int(code & msgpcode.FixedArrayMask), nil
	case code == msgpcode.Array16:
		value, takeErr := decoder.take(2)
		if takeErr != nil {
			return 0, takeErr
		}
		return int(binary.BigEndian.Uint16(value)), nil
	case code == msgpcode.Array32:
		value, takeErr := decoder.take(4)
		if takeErr != nil {
			return 0, takeErr
		}
		length := binary.BigEndian.Uint32(value)
		if uint64(length) > uint64(maxRegistrationEventBytes) {
			return maxRegistrationEventBytes + 1, nil
		}
		return int(length), nil
	default:
		return 0, fmt.Errorf("invalid event array")
	}
}

// bytes 读取一个 String/Binary 标量并返回指向输入的只读切片。
// 需要跨调用持有的上层字段会显式 Clone。
func (decoder *eventDecoder) bytes() ([]byte, error) {
	code, err := decoder.byte()
	if err != nil {
		return nil, err
	}
	length, err := decoder.byteLength(code)
	if err != nil || length > maxRegistrationEventBytes {
		return nil, fmt.Errorf("invalid event bytes")
	}
	return decoder.take(length)
}

// uint 读取任意 MessagePack 整数宽度，并要求结果为正且不超过 maxSafeInteger。
func (decoder *eventDecoder) uint() (uint64, error) {
	code, err := decoder.byte()
	if err != nil {
		return 0, err
	}
	var value uint64
	switch {
	case code <= msgpcode.PosFixedNumHigh:
		value = uint64(code)
	case code == msgpcode.Uint8:
		encoded, takeErr := decoder.take(1)
		if takeErr != nil {
			return 0, takeErr
		}
		value = uint64(encoded[0])
	case code == msgpcode.Uint16:
		encoded, takeErr := decoder.take(2)
		if takeErr != nil {
			return 0, takeErr
		}
		value = uint64(binary.BigEndian.Uint16(encoded))
	case code == msgpcode.Uint32:
		encoded, takeErr := decoder.take(4)
		if takeErr != nil {
			return 0, takeErr
		}
		value = uint64(binary.BigEndian.Uint32(encoded))
	case code == msgpcode.Uint64:
		encoded, takeErr := decoder.take(8)
		if takeErr != nil {
			return 0, takeErr
		}
		value = binary.BigEndian.Uint64(encoded)
	case code == msgpcode.Int8:
		encoded, takeErr := decoder.take(1)
		if takeErr != nil || int8(encoded[0]) <= 0 {
			return 0, fmt.Errorf("invalid event integer")
		}
		value = uint64(int8(encoded[0]))
	case code == msgpcode.Int16:
		encoded, takeErr := decoder.take(2)
		if takeErr != nil {
			return 0, takeErr
		}
		signed := int16(binary.BigEndian.Uint16(encoded))
		if signed <= 0 {
			return 0, fmt.Errorf("invalid event integer")
		}
		value = uint64(signed)
	case code == msgpcode.Int32:
		encoded, takeErr := decoder.take(4)
		if takeErr != nil {
			return 0, takeErr
		}
		signed := int32(binary.BigEndian.Uint32(encoded))
		if signed <= 0 {
			return 0, fmt.Errorf("invalid event integer")
		}
		value = uint64(signed)
	case code == msgpcode.Int64:
		encoded, takeErr := decoder.take(8)
		if takeErr != nil {
			return 0, takeErr
		}
		signed := int64(binary.BigEndian.Uint64(encoded))
		if signed <= 0 {
			return 0, fmt.Errorf("invalid event integer")
		}
		value = uint64(signed)
	default:
		return 0, fmt.Errorf("invalid event integer")
	}
	if value == 0 || value > maxSafeInteger {
		return 0, fmt.Errorf("invalid event integer")
	}
	return value, nil
}

// skipScalar 跳过一个未知控制字段的有界标量值。
// 数组、Map、扩展类型和声明超限的字节值会被拒绝，避免递归和分配攻击。
func (decoder *eventDecoder) skipScalar() error {
	code, err := decoder.byte()
	if err != nil {
		return err
	}
	switch {
	case code <= msgpcode.PosFixedNumHigh || code >= msgpcode.NegFixedNumLow ||
		code == msgpcode.Nil || code == msgpcode.False || code == msgpcode.True:
		return nil
	case msgpcode.IsString(code) || msgpcode.IsBin(code):
		length, lengthErr := decoder.byteLength(code)
		if lengthErr != nil || length > maxRegistrationEventBytes {
			return fmt.Errorf("invalid event scalar length")
		}
		_, err = decoder.take(length)
		return err
	case code == msgpcode.Float || code == msgpcode.Uint32 || code == msgpcode.Int32:
		_, err = decoder.take(4)
		return err
	case code == msgpcode.Double || code == msgpcode.Uint64 || code == msgpcode.Int64:
		_, err = decoder.take(8)
		return err
	case code == msgpcode.Uint8 || code == msgpcode.Int8:
		_, err = decoder.take(1)
		return err
	case code == msgpcode.Uint16 || code == msgpcode.Int16:
		_, err = decoder.take(2)
		return err
	default:
		return fmt.Errorf("invalid event scalar")
	}
}

// byteLength 根据 String/Binary marker 读取长度前缀，并在转换为 int 前检查协议上限。
func (decoder *eventDecoder) byteLength(code byte) (int, error) {
	if msgpcode.IsFixedString(code) {
		return int(code & msgpcode.FixedStrMask), nil
	}
	var width int
	switch code {
	case msgpcode.Str8, msgpcode.Bin8:
		width = 1
	case msgpcode.Str16, msgpcode.Bin16:
		width = 2
	case msgpcode.Str32, msgpcode.Bin32:
		width = 4
	default:
		return 0, fmt.Errorf("invalid event byte marker")
	}
	encoded, err := decoder.take(width)
	if err != nil {
		return 0, err
	}
	switch width {
	case 1:
		return int(encoded[0]), nil
	case 2:
		return int(binary.BigEndian.Uint16(encoded)), nil
	default:
		length := binary.BigEndian.Uint32(encoded)
		if uint64(length) > uint64(maxRegistrationEventBytes) {
			return maxRegistrationEventBytes + 1, nil
		}
		return int(length), nil
	}
}

// byte 读取一个字节并推进游标；输入耗尽时返回截断错误。
func (decoder *eventDecoder) byte() (byte, error) {
	if decoder.offset >= len(decoder.input) {
		return 0, fmt.Errorf("truncated event")
	}
	value := decoder.input[decoder.offset]
	decoder.offset++
	return value, nil
}

// take 返回接下来 length 个输入字节的只读窗口并推进游标。
// length 为负或超过剩余输入时不改变可观察结果并返回截断错误。
func (decoder *eventDecoder) take(length int) ([]byte, error) {
	if length < 0 || length > len(decoder.input)-decoder.offset {
		return nil, fmt.Errorf("truncated event")
	}
	start := decoder.offset
	decoder.offset += length
	return decoder.input[start:decoder.offset], nil
}

// done 报告顶层事件是否被精确消费，借此拒绝尾随垃圾字节。
func (decoder *eventDecoder) done() bool {
	return decoder.offset == len(decoder.input)
}

// validUUIDBytes 校验协议规定的 32 字节小写十六进制进程 UUID，不进行字符串分配。
func validUUIDBytes(value []byte) bool {
	if len(value) != 32 {
		return false
	}
	for _, character := range value {
		if (character < '0' || character > '9') && (character < 'a' || character > 'f') {
			return false
		}
	}
	return true
}

// validUUID 校验字符串形式的 32 字节小写十六进制进程 UUID。
func validUUID(value string) bool {
	if len(value) != 32 {
		return false
	}
	for index := 0; index < len(value); index++ {
		character := value[index]
		if (character < '0' || character > '9') && (character < 'a' || character > 'f') {
			return false
		}
	}
	return true
}

// parseStoredRecord 把 Redis HGETALL 结果解析为一条完整 Selector 记录。
// uuid 是索引期望身份；values 必须包含全部 Meta，且 Attr/Data 与 limits 一致。
func parseStoredRecord(uuid string, values map[string]string, limits zoneConfig) (*selectorRecord, error) {
	required := [...]string{"@uuid", "@revision", "@timestamp", "@ttl", "@version"}
	for _, field := range required {
		if _, exists := values[field]; !exists {
			return nil, protocolError(codeCorrupt, field, 0)
		}
	}
	if values["@uuid"] != uuid || !validUUID(uuid) {
		return nil, protocolError(codeTarget, "@uuid", 0)
	}
	revision, err := parseCanonicalUint(values["@revision"])
	if err != nil {
		return nil, protocolError(codeCorrupt, "@revision", 0)
	}
	timestamp, err := parseCanonicalUint(values["@timestamp"])
	if err != nil {
		return nil, protocolError(codeCorrupt, "@timestamp", 0)
	}
	ttl, err := parseCanonicalUint(values["@ttl"])
	if err != nil {
		return nil, protocolError(codeCorrupt, "@ttl", 0)
	}
	version, err := parseCanonicalUint(values["@version"])
	if err != nil {
		return nil, protocolError(codeCorrupt, "@version", 0)
	}
	// 保留字段不进入应用视图；点前缀字段去掉前缀后进入不可变 Attr，其余进入 Data。
	attr := make(fields)
	data := make(fields)
	for name, value := range values {
		if strings.HasPrefix(name, "@") {
			continue
		}
		if strings.HasPrefix(name, "&") {
			return nil, protocolError(codeCorrupt, name, 0)
		}
		if strings.HasPrefix(name, ".") {
			attr[strings.TrimPrefix(name, ".")] = []byte(value)
		} else {
			data[name] = []byte(value)
		}
	}
	if err := validateRecord(uuid, revision, ttl, version, attr, data, limits); err != nil {
		return nil, err
	}
	if timestamp > maxHashFieldExpireAtMilliseconds || ttl > maxHashFieldExpireAtMilliseconds-timestamp {
		return nil, protocolError(codeCorrupt, "@timestamp", 0)
	}
	return &selectorRecord{
		meta:     Meta{UUID: uuid, Revision: revision, Timestamp: timestamp, TTL: ttl, Version: version},
		attr:     attr,
		data:     data,
		deadline: timestamp + ttl,
		size:     registrationSize(uuid, revision, ttl, version, attr, data),
	}, nil
}

// parseCanonicalUint 解析规范十进制的正安全整数。
// 共享解析原语拒绝符号、零、前导零、非数字和安全整数上限外的值。
func parseCanonicalUint(value string) (uint64, error) {
	parsed, valid := validate.UintDecimal(value, maxSafeInteger, false)
	if !valid {
		return 0, fmt.Errorf("invalid positive safe integer")
	}
	return parsed, nil
}
