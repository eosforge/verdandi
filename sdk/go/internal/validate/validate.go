// Package validate 集中保存 Go SDK 各领域共享的低层配置校验原语。
//
// 此包位于 internal 下，不扩大 SDK 的公开 API；调用方仍负责选择字段默认值、范围和稳定错误名。
package validate

import "time"

// UintDecimal 解析不超过 maximum 的规范无符号十进制整数。
// allowZero 只控制唯一零形式 "0"；符号、空输入、前导零、非数字和溢出均被拒绝。
func UintDecimal(source string, maximum uint64, allowZero bool) (uint64, bool) {
	if len(source) == 0 {
		return 0, false
	}
	if source[0] == '0' {
		return 0, allowZero && len(source) == 1
	}
	if source[0] < '1' || source[0] > '9' {
		return 0, false
	}

	var value uint64
	for index := range len(source) {
		character := source[index]
		if character < '0' || character > '9' {
			return 0, false
		}
		digit := uint64(character - '0')
		if digit > maximum || value > (maximum-digit)/10 {
			return 0, false
		}
		value = value*10 + digit
	}
	return value, true
}

// UintDecimalBytes 是 []byte 热路径使用的同一规范解析规则。
// 保留具体切片循环可让编译器生成无泛型字典开销的代码，并避免调用方转换为 string。
func UintDecimalBytes(source []byte, maximum uint64, allowZero bool) (uint64, bool) {
	if len(source) == 0 {
		return 0, false
	}
	if source[0] == '0' {
		return 0, allowZero && len(source) == 1
	}
	if source[0] < '1' || source[0] > '9' {
		return 0, false
	}

	var value uint64
	for _, character := range source {
		if character < '0' || character > '9' {
			return 0, false
		}
		digit := uint64(character - '0')
		if digit > maximum || value > (maximum-digit)/10 {
			return 0, false
		}
		value = value*10 + digit
	}
	return value, true
}

// Duration 展开零值默认，并要求持续时间是闭区间内的整毫秒值。
func Duration(value, fallback, minimum, maximum time.Duration) (time.Duration, bool) {
	if value == 0 {
		value = fallback
	}
	if value < minimum || value > maximum || value%time.Millisecond != 0 {
		return 0, false
	}
	return value, true
}

// OptionalDuration 区分 nil 默认值和显式零值，并要求持续时间是闭区间内的整毫秒值。
func OptionalDuration(value *time.Duration, fallback, minimum, maximum time.Duration) (time.Duration, bool) {
	if value == nil {
		return fallback, true
	}
	if *value < minimum || *value > maximum || *value%time.Millisecond != 0 {
		return 0, false
	}
	return *value, true
}

// OptionalInt 区分 nil 默认值与显式零值，并检查闭区间范围。
func OptionalInt(value *int, fallback, minimum, maximum int) (int, bool) {
	if value == nil {
		return fallback, true
	}
	if *value < minimum || *value > maximum {
		return 0, false
	}
	return *value, true
}

// Zone 校验 1 至 32 字节、只含 ASCII 字母的 Zone。
func Zone(value string) bool {
	if len(value) == 0 || len(value) > 32 {
		return false
	}
	for index := range len(value) {
		character := value[index]
		if (character < 'a' || character > 'z') && (character < 'A' || character > 'Z') {
			return false
		}
	}
	return true
}
