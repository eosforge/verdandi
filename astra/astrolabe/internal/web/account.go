// Package web 提供有界的管理 HTTP 接口, 浏览器身份与 Pulsar 节点身份完全分开.
package web

import (
	"bytes"
	"crypto/pbkdf2"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"os"
	"unicode/utf8"

	"github.com/eosforge/verdandi/astra/internal/admission"
)

// Account 只包含部署指定的单个管理账号摘要, 不保存原始密码或可登录的默认账号.
type Account struct {
	username string
	salt     [16]byte
	hash     [32]byte
}

var errInput = errors.New("invalid management configuration or input")

// object 接受一层 JSON 对象, 所有当前 HTTP 输入均为标量; 拒绝重复、未知字段和尾随文档.
func object(data []byte, target any) error {
	if !utf8.Valid(data) {
		return errInput
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	if token, err := decoder.Token(); err != nil || token != json.Delim('{') {
		return errInput
	}
	seen := make(map[string]bool)
	for decoder.More() {
		token, err := decoder.Token()
		name, ok := token.(string)
		if err != nil || !ok || seen[name] || len(seen) >= 16 {
			return errInput
		}
		seen[name] = true
		var value json.RawMessage
		if decoder.Decode(&value) != nil || len(value) == 0 || value[0] == '{' || value[0] == '[' {
			return errInput
		}
	}
	if token, err := decoder.Token(); err != nil || token != json.Delim('}') {
		return errInput
	}
	if _, err := decoder.Token(); err != io.EOF {
		return errInput
	}
	decoder = json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return errInput
	}
	return nil
}

// LoadAccount 有界读取普通文件, 不生成默认账号、KDF 参数或下载密码工具.
func LoadAccount(path string) (*Account, error) {
	if info, err := os.Lstat(path); err != nil || !info.Mode().IsRegular() {
		return nil, errInput
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, errInput
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		return nil, errInput
	}
	data, err := io.ReadAll(io.LimitReader(file, 4097))
	if err != nil || len(data) > 4096 {
		return nil, errInput
	}
	var input struct {
		Username string `json:"username"`
		Salt     string `json:"salt"`
		Hash     string `json:"hash"`
	}
	if object(data, &input) != nil || !admission.Name(input.Username) {
		return nil, errInput
	}
	salt, one := hex.DecodeString(input.Salt)
	hash, two := hex.DecodeString(input.Hash)
	if one != nil || two != nil || len(salt) != 16 || len(hash) != 32 {
		return nil, errInput
	}
	return &Account{username: input.Username, salt: [16]byte(salt), hash: [32]byte(hash)}, nil
}

// verify 与 Pulsar 同为 PBKDF2-HMAC-SHA256/600000/32, 错误用户名也执行完整派生.
func (account *Account) verify(username, password string) bool {
	derived, err := pbkdf2.Key(sha256.New, password, account.salt[:], 600000, len(account.hash))
	defer clear(derived)
	return err == nil && subtle.ConstantTimeCompare(derived, account.hash[:]) == 1 && username == account.username
}
