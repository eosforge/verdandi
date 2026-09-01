package configuration

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"

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
