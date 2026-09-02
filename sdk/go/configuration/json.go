package configuration

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"unicode/utf8"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

const maximumJSONBytes = 1024 * 1024

// LoadJSON 从 reader 读取并严格解析一份 JSON 配置。
// 读取量硬限制为 1 MiB 加一个探测字节，超限时不会继续消费输入。
func LoadJSON(reader io.Reader) (Config, error) {
	if reader == nil {
		return Config{}, configError(verdandi.CodeInvalid, "reader", nil)
	}
	source, err := io.ReadAll(io.LimitReader(reader, maximumJSONBytes+1))
	if err != nil {
		return Config{}, configError(verdandi.CodeUnavailable, "json", err)
	}
	return ParseJSON(source)
}

// LoadFile 打开并解析 path 指向的 JSON 配置文件；文件句柄在返回前关闭。
func LoadFile(path string) (Config, error) {
	if path == "" {
		return Config{}, configError(verdandi.CodeInvalid, "path", nil)
	}
	file, err := os.Open(path)
	if err != nil {
		return Config{}, configError(verdandi.CodeUnavailable, "path", err)
	}
	defer file.Close()
	return LoadJSON(file)
}

// ParseJSON 严格解析一份不超过 1 MiB 的 UTF-8 JSON 配置并验证所有已启用领域。
// 未知字段、重复字段、尾随 JSON 值和不支持的版本都会返回 Invalid。
func ParseJSON(source []byte) (Config, error) {
	if len(source) > maximumJSONBytes {
		return Config{}, configError(verdandi.CodeCapacity, "json", nil)
	}
	if err := validateJSONEncoding(source); err != nil {
		return Config{}, configError(verdandi.CodeInvalid, "json", err)
	}
	if err := rejectDuplicateFields(source); err != nil {
		return Config{}, configError(verdandi.CodeInvalid, "json", err)
	}
	decoder := json.NewDecoder(bytes.NewReader(source))
	decoder.DisallowUnknownFields()
	var config Config
	if err := decoder.Decode(&config); err != nil {
		return Config{}, configError(verdandi.CodeInvalid, "json", err)
	}
	if err := requireJSONEnd(decoder); err != nil {
		return Config{}, configError(verdandi.CodeInvalid, "json", err)
	}
	if config.Version != "v1" {
		return Config{}, configError(verdandi.CodeInvalid, "version", nil)
	}
	if err := config.check(); err != nil {
		return Config{}, err
	}
	return config, nil
}

// validateJSONEncoding 拒绝非法 UTF-8 及不成对的 UTF-16 转义，避免 encoding/json 静默替换为 U+FFFD。
func validateJSONEncoding(source []byte) error {
	if !utf8.Valid(source) {
		return errors.New("JSON is not valid UTF-8")
	}
	for index, inString := 0, false; index < len(source); index++ {
		switch source[index] {
		case '"':
			inString = !inString
		case '-':
			if !inString && index+1 < len(source) && source[index+1] == '0' && (index+2 == len(source) || jsonValueDelimiter(source[index+2])) {
				return errors.New("negative zero is not a canonical JSON integer")
			}
		case '\\':
			if !inString {
				continue
			}
			index++
			if index >= len(source) {
				return errors.New("incomplete JSON escape")
			}
			if source[index] != 'u' {
				continue
			}
			value, ok := hexQuad(source, index+1)
			if !ok {
				return errors.New("invalid JSON Unicode escape")
			}
			index += 4
			switch {
			case value >= 0xD800 && value <= 0xDBFF:
				if index+6 >= len(source) || source[index+1] != '\\' || source[index+2] != 'u' {
					return errors.New("unpaired high surrogate")
				}
				low, valid := hexQuad(source, index+3)
				if !valid || low < 0xDC00 || low > 0xDFFF {
					return errors.New("unpaired high surrogate")
				}
				index += 6
			case value >= 0xDC00 && value <= 0xDFFF:
				return errors.New("unpaired low surrogate")
			}
		}
	}
	return nil
}

// jsonValueDelimiter 判断一个字节能否合法结束整数 token，用于精确识别独立的 -0 而不误判负数前缀。
func jsonValueDelimiter(value byte) bool {
	switch value {
	case ' ', '\t', '\n', '\r', ',', ']', '}':
		return true
	default:
		return false
	}
}

// hexQuad 解析 `u` 后的四个十六进制数字，不接受缺失或非 ASCII 十六进制字符。
func hexQuad(source []byte, offset int) (uint16, bool) {
	if offset+4 > len(source) {
		return 0, false
	}
	var value uint16
	for _, character := range source[offset : offset+4] {
		value <<= 4
		switch {
		case character >= '0' && character <= '9':
			value |= uint16(character - '0')
		case character >= 'a' && character <= 'f':
			value |= uint16(character-'a') + 10
		case character >= 'A' && character <= 'F':
			value |= uint16(character-'A') + 10
		default:
			return 0, false
		}
	}
	return value, true
}

// requireJSONEnd 要求第一份配置后只剩空白，拒绝拼接的第二个 JSON 值。
func requireJSONEnd(decoder *json.Decoder) error {
	var trailing any
	err := decoder.Decode(&trailing)
	if errors.Is(err, io.EOF) {
		return nil
	}
	if err != nil {
		return err
	}
	return errors.New("trailing JSON value")
}

// rejectDuplicateFields 单遍检查所有对象键，避免 Go 的最后字段获胜行为与 Rust Serde 不一致。
func rejectDuplicateFields(source []byte) error {
	decoder := json.NewDecoder(bytes.NewReader(source))
	decoder.UseNumber()
	if err := scanJSONValue(decoder); err != nil {
		return err
	}
	return requireJSONEnd(decoder)
}

// scanJSONValue 递归消费一个 JSON 值，并在每个对象作用域内拒绝重复字段名。
func scanJSONValue(decoder *json.Decoder) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	if token == nil {
		return errors.New("null is not allowed")
	}
	delimiter, container := token.(json.Delim)
	if !container {
		return nil
	}
	switch delimiter {
	case '{':
		fields := make(map[string]struct{})
		for decoder.More() {
			nameToken, err := decoder.Token()
			if err != nil {
				return err
			}
			name, ok := nameToken.(string)
			if !ok {
				return errors.New("object field is not a string")
			}
			if _, exists := fields[name]; exists {
				return fmt.Errorf("duplicate field %q", name)
			}
			fields[name] = struct{}{}
			if err := scanJSONValue(decoder); err != nil {
				return err
			}
		}
	case '[':
		for decoder.More() {
			if err := scanJSONValue(decoder); err != nil {
				return err
			}
		}
	default:
		return errors.New("unexpected JSON delimiter")
	}
	_, err = decoder.Token()
	return err
}

// configError 构造配置层稳定错误；cause 只用于有界诊断，不参与程序判断。
func configError(code verdandi.Code, field string, cause error) error {
	return &verdandi.Error{Code: code, Field: field, Cause: cause}
}
