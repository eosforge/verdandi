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
	username string   // 管理用户名, 登录时必须精确匹配.
	salt     [16]byte // 密码盐, 16 字节随机值, 与摘要一起存于账号文件.
	hash     [32]byte // PBKDF2 派生摘要, 32 字节, 从不保存原始密码.
}

var errInput = errors.New("invalid management configuration or input")

// object 接受一层 JSON 对象, 所有当前 HTTP 输入均为标量; 拒绝重复、未知字段和尾随文档.
// data 为请求正文; target 为解码目标, 仅标量字段会被填充.
func object(data []byte, target any) error {
	// 先校验完整 UTF-8, 非法编码直接拒绝, 不进入解析器.
	if !utf8.Valid(data) {
		return errInput
	}
	// 第一遍只做形状检查: 顶层对象、键唯一且不超 16 个、值全部为标量 (禁嵌套对象/数组).
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
	// 第二遍严格解码: 未知字段直接失败, 形状已在上一步保证.
	decoder = json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return errInput
	}
	return nil
}

// LoadAccount 有界读取普通文件, 不生成默认账号、KDF 参数或下载密码工具.
// path 为账号文件路径; 返回账号摘要, 文件非法或超限时返回错误.
func LoadAccount(path string) (*Account, error) {
	// 只接受普通文件, 拒绝目录、链接等特殊类型, 缩小符号链接攻击面.
	if info, err := os.Lstat(path); err != nil || !info.Mode().IsRegular() {
		return nil, errInput
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, errInput
	}
	defer file.Close()
	// 打开后再确认一次类型, 防止检查与使用之间的竞态替换.
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		return nil, errInput
	}
	// 限 4 KiB 读取, 账号文件只含三段短十六进制, 超限即拒绝.
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
	// 盐与摘要必须为十六进制且长度精确 (16/32 字节), 否则拒绝.
	salt, one := hex.DecodeString(input.Salt)
	hash, two := hex.DecodeString(input.Hash)
	if one != nil || two != nil || len(salt) != 16 || len(hash) != 32 {
		return nil, errInput
	}
	return &Account{username: input.Username, salt: [16]byte(salt), hash: [32]byte(hash)}, nil
}

// verify 与 Pulsar 同为 PBKDF2-HMAC-SHA256/600000/32, 错误用户名也执行完整派生.
// username/password 为待验证凭据; 用户名错误同样走完整 KDF, 不泄露账号存在性.
func (account *Account) verify(username, password string) bool {
	derived, err := pbkdf2.Key(sha256.New, password, account.salt[:], 600000, len(account.hash))
	defer clear(derived)
	return err == nil && subtle.ConstantTimeCompare(derived, account.hash[:]) == 1 && username == account.username
}
