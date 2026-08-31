package registration

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

const (
	protocolVersion = "v1"

	protocolAttrFields      = 128
	protocolDataFields      = 128
	protocolFieldNameBytes  = 64
	protocolFieldValueBytes = 16 * 1024
	protocolRecordBytes     = 64 * 1024

	maxSafeInteger                   uint64 = 1<<53 - 1
	maxHashFieldExpireAtMilliseconds uint64 = 1<<46 - 1
)

// fields 和 code 让领域实现使用根包的统一协议类型，同时保持内部签名简洁。
type fields = verdandi.Fields
type code = verdandi.Code

const (
	codeInvalid     = verdandi.CodeInvalid
	codeProtocol    = verdandi.CodeProtocol
	codeContract    = verdandi.CodeContract
	codeTarget      = verdandi.CodeTarget
	codeCapacity    = verdandi.CodeCapacity
	codeMissing     = verdandi.CodeMissing
	codeStale       = verdandi.CodeStale
	codeTransition  = verdandi.CodeTransition
	codeImmutable   = verdandi.CodeImmutable
	codeCorrupt     = verdandi.CodeCorrupt
	codeUnavailable = verdandi.CodeUnavailable
	codeDeadline    = verdandi.CodeDeadline
	codeAmbiguous   = verdandi.CodeAmbiguous
	codeClosed      = verdandi.CodeClosed
)

// fieldPointer 约束 T 的指针必须实现完整字段解码；调用泛型 API 时用户只需显式提供 T。
type fieldPointer[T any] interface {
	*T
	verdandi.Decoder
}

// protocolError 构造 Registration 领域的稳定错误，field/revision 为可选协议上下文。
func protocolError(code verdandi.Code, field string, revision uint64) error {
	return &verdandi.Error{Code: code, Field: field, Revision: revision}
}

// isCode 判断错误链是否包含指定 Verdandi 结果类别。
func isCode(err error, code verdandi.Code) bool {
	return verdandi.IsCode(err, code)
}

// wrapError 用稳定 code 包装 cause；cause 为 nil 时直接返回 nil。
func wrapError(code verdandi.Code, cause error) error {
	if cause == nil {
		return nil
	}
	return &verdandi.Error{Code: code, Cause: cause}
}

// wrapContext 把标准 Context 结束原因映射为 deadline、closed 或 unavailable。
func wrapContext(err error) error {
	if errors.Is(err, context.DeadlineExceeded) {
		return wrapError(verdandi.CodeDeadline, err)
	}
	if errors.Is(err, context.Canceled) {
		return wrapError(verdandi.CodeClosed, err)
	}
	return wrapError(verdandi.CodeUnavailable, err)
}

// wrapDriver 把 go-redis 失败映射为稳定类别，并保留原始错误链。
// code 表示响应未知时应采用的读/写语义；确定关闭和 Context 状态会覆盖它。
func wrapDriver(code verdandi.Code, err error) error {
	if err == nil {
		return nil
	}
	if errors.Is(err, redis.ErrClosed) {
		return &verdandi.Error{Code: verdandi.CodeClosed, Cause: fmt.Errorf("redis operation: %w", err)}
	}
	if code == verdandi.CodeAmbiguous {
		return &verdandi.Error{Code: code, Cause: fmt.Errorf("redis operation: %w", err)}
	}
	if errors.Is(err, context.DeadlineExceeded) {
		code = verdandi.CodeDeadline
	} else if errors.Is(err, context.Canceled) {
		code = verdandi.CodeClosed
	}
	return &verdandi.Error{Code: code, Cause: fmt.Errorf("redis operation: %w", err)}
}

// cloneFields 深拷贝字段 map 和所有值缓冲区；空输入返回 nil 以避免分配。
func cloneFields(source fields) fields {
	if len(source) == 0 {
		return nil
	}
	result := make(fields, len(source))
	for name, value := range source {
		result[name] = bytes.Clone(value)
	}
	return result
}

// cloneFieldMap 只复制 map 所有权，并复用 SDK 内部已不可变的字段值。
// 只能用于值字节已经归 SDK 所有、后续不会原地修改的状态转换。
func cloneFieldMap(source fields) fields {
	if len(source) == 0 {
		return nil
	}
	result := make(fields, len(source))
	for name, value := range source {
		result[name] = value
	}
	return result
}

// sortedFieldNames 返回按字节字典序排序的字段名，用于生成确定的 Lua 参数顺序。
func sortedFieldNames(values fields) []string {
	names := make([]string, 0, len(values))
	for name := range values {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// fieldsEqual 比较两个字段集合的键存在性和字节值；缺失字段不等于存在的空值。
func fieldsEqual(left fields, right fields) bool {
	if len(left) != len(right) {
		return false
	}
	for name, value := range left {
		other, exists := right[name]
		if !exists || !bytes.Equal(value, other) {
			return false
		}
	}
	return true
}

// validType 校验 Registry Type：首字符为 ASCII 字母，其余可含字母、数字、下划线、点和连字符。
func validType(value string) bool {
	if len(value) == 0 || len(value) > 64 || !asciiLetter(value[0]) {
		return false
	}
	for index := 1; index < len(value); index++ {
		character := value[index]
		if !asciiLetter(character) && (character < '0' || character > '9') &&
			character != '_' && character != '.' && character != '-' {
			return false
		}
	}
	return true
}

// asciiLetter 判断一个字节是否为 ASCII 大小写字母。
func asciiLetter(value byte) bool {
	return value >= 'a' && value <= 'z' || value >= 'A' && value <= 'Z'
}

// validateFields 按 Zone 策略校验 Attr/Data 数量以及每个字段名和值。
func validateFields(attr fields, data fields, limits zoneConfig) error {
	if len(attr) > limits.attrMaxFields {
		return protocolError(verdandi.CodeCapacity, "attr", 0)
	}
	if len(data) > limits.dataMaxFields {
		return protocolError(verdandi.CodeCapacity, "data", 0)
	}
	for name, value := range attr {
		if err := validateApplicationField(name, value, limits.fieldNameMaxBytes, limits.attrValueMaxBytes); err != nil {
			return err
		}
	}
	for name, value := range data {
		if err := validateApplicationField(name, value, limits.fieldNameMaxBytes, limits.dataValueMaxBytes); err != nil {
			return err
		}
	}
	return nil
}

// protocolZoneConfig 返回协议硬上限，用于校验 Redis 事件和权威读取，而非管理员可调默认值。
func protocolZoneConfig() zoneConfig {
	return zoneConfig{
		attrMaxFields:        protocolAttrFields,
		dataMaxFields:        protocolDataFields,
		fieldNameMaxBytes:    protocolFieldNameBytes,
		attrValueMaxBytes:    protocolFieldValueBytes,
		dataValueMaxBytes:    protocolFieldValueBytes,
		recordMaxBytes:       protocolRecordBytes,
		configurationRefresh: 30 * time.Second,
	}
}

// validateApplicationField 校验应用字段名和值的编码和容量。
// 名称必须是非空 UTF-8，且不得占用 &、@ 或 . 保留前缀。
func validateApplicationField(name string, value []byte, nameLimit int, valueLimit int) error {
	if len(name) == 0 || len(name) > nameLimit || !utf8.ValidString(name) || strings.HasPrefix(name, "&") ||
		strings.HasPrefix(name, "@") || strings.HasPrefix(name, ".") {
		return protocolError(verdandi.CodeInvalid, name, 0)
	}
	if len(value) > valueLimit {
		return protocolError(verdandi.CodeCapacity, name, 0)
	}
	return nil
}

// registrationSize 计算 Registration Hash 的保守协议字节数。
// @timestamp 为 Redis 生成值，预留 16 个十进制字节以便在写入前完成容量检查。
func registrationSize(uuid string, revision uint64, ttlMilliseconds uint64, version uint64, attr fields, data fields) int {
	size := len("@uuid") + len(uuid)
	size += len("@revision") + decimalDigits(revision)
	size += len("@timestamp") + 16
	size += len("@ttl") + decimalDigits(ttlMilliseconds)
	size += len("@version") + decimalDigits(version)
	for name, value := range attr {
		size += 1 + len(name) + len(value)
	}
	for name, value := range data {
		size += len(name) + len(value)
	}
	return size
}

// decimalDigits 返回无符号整数的十进制位数，不进行格式化分配。
func decimalDigits(value uint64) int {
	digits := 1
	for value >= 10 {
		value /= 10
		digits++
	}
	return digits
}

// validateRecord 校验完整 Registration 的版本、TTL、字段结构和总容量。
// uuid 的格式由创建/读取边界单独校验；此函数只把其字节计入容量。
func validateRecord(uuid string, revision uint64, ttlMilliseconds uint64, version uint64, attr fields, data fields, limits zoneConfig) error {
	if revision == 0 || revision > maxSafeInteger {
		return protocolError(verdandi.CodeInvalid, "@revision", 0)
	}
	if ttlMilliseconds == 0 || ttlMilliseconds > maxHashFieldExpireAtMilliseconds {
		return protocolError(verdandi.CodeInvalid, "@ttl", 0)
	}
	if version == 0 || version > maxSafeInteger {
		return protocolError(verdandi.CodeInvalid, "@version", 0)
	}
	if err := validateFields(attr, data, limits); err != nil {
		return err
	}
	if registrationSize(uuid, revision, ttlMilliseconds, version, attr, data) > limits.recordMaxBytes {
		return protocolError(verdandi.CodeCapacity, "registration", 0)
	}
	return nil
}

// durationMilliseconds 把正数、整毫秒精度的 Duration 转成 Hash-field TTL 协议整数。
// 超过 Redis 8 HPEXPIREAT 的 2^46-1 边界会被拒绝。
func durationMilliseconds(value time.Duration) (uint64, error) {
	if value <= 0 || value%time.Millisecond != 0 {
		return 0, protocolError(verdandi.CodeInvalid, "ttl", 0)
	}
	milliseconds := uint64(value / time.Millisecond)
	if milliseconds == 0 || milliseconds > maxHashFieldExpireAtMilliseconds {
		return 0, protocolError(verdandi.CodeInvalid, "ttl", 0)
	}
	return milliseconds, nil
}

// encodeFieldValue 调用应用 Encoder 并把失败定位到 field。
// 返回字段所有权按 Encoder 契约转移给 SDK，不额外复制。
func encodeFieldValue[T verdandi.Encoder](value T, field string) (fields, error) {
	encoded, err := value.Encode()
	if err != nil {
		return nil, &verdandi.Error{Code: verdandi.CodeInvalid, Field: field, Cause: err}
	}
	return encoded, nil
}

// decodeOwnedFieldValue 把一份已经脱离 SDK 内部状态的 source 直接移交给 Decoder。
// 调用后 source 不得再次使用；借此避免先前 Encoder 已生成独立字段时再做一次无意义深拷贝。
func decodeOwnedFieldValue[T any, P fieldPointer[T]](source fields, field string) (T, error) {
	var value T
	if err := P(&value).Decode(source); err != nil {
		return value, &verdandi.Error{Code: verdandi.CodeCorrupt, Field: field, Cause: err}
	}
	return value, nil
}

// decodeFieldValue 深拷贝 SDK 内部 source 后调用 T 指针的 Decoder，并把失败归类为 corrupt。
// 返回错误时 value 可能被应用解码器部分修改，但不会发布到 SDK 视图。
func decodeFieldValue[T any, P fieldPointer[T]](source fields, field string) (T, error) {
	return decodeOwnedFieldValue[T, P](cloneFields(source), field)
}

// encodeSelectorAttr 把类型化 Attr 编码错误统一定位为 attr。
func encodeSelectorAttr[T verdandi.Encoder](value T) (fields, error) {
	return encodeFieldValue(value, "attr")
}

// encodeSelectorData 把类型化 Data 编码错误统一定位为 data。
func encodeSelectorData[T verdandi.Encoder](value T) (fields, error) {
	return encodeFieldValue(value, "data")
}

// decodeSelectorAttr 从内部字段构造独立的类型化 Attr。
func decodeSelectorAttr[T any, P fieldPointer[T]](source fields) (T, error) {
	return decodeFieldValue[T, P](source, "attr")
}

// decodeSelectorData 从内部字段构造独立的类型化 Data。
func decodeSelectorData[T any, P fieldPointer[T]](source fields) (T, error) {
	return decodeFieldValue[T, P](source, "data")
}

// decodeOwnedSelectorAttr 接管一次选择校验刚生成的独立 Attr 字段，避免再次深拷贝。
func decodeOwnedSelectorAttr[T any, P fieldPointer[T]](source fields) (T, error) {
	return decodeOwnedFieldValue[T, P](source, "attr")
}

// decodeOwnedSelectorData 接管一次选择校验刚生成的独立 Data 字段，避免再次深拷贝。
func decodeOwnedSelectorData[T any, P fieldPointer[T]](source fields) (T, error) {
	return decodeOwnedFieldValue[T, P](source, "data")
}

// projectSelectorAttr 把解码后的 Attr 装箱到 Selector 内部不可变投影缓存。
func projectSelectorAttr[T any, P fieldPointer[T]](source fields) (any, error) {
	return decodeSelectorAttr[T, P](source)
}

// projectSelectorData 把解码后的 Data 装箱到 Selector 内部不可变投影缓存。
func projectSelectorData[T any, P fieldPointer[T]](source fields) (any, error) {
	return decodeSelectorData[T, P](source)
}

// sameFieldStructure 只比较字段名集合，不比较字段值。
func sameFieldStructure(left fields, right fields) bool {
	if len(left) != len(right) {
		return false
	}
	for name := range left {
		if _, exists := right[name]; !exists {
			return false
		}
	}
	return true
}
