package admission

import (
	"bytes"
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"unicode/utf8"
)

// WriteAccount 从有界 stdin JSON 读取一次账号配置, 只向输出写加盐摘要.
// 密码不进入命令行参数或日志, 本函数不修改账号文件, 由部署者合并到 accounts.json.
func WriteAccount(input io.Reader, output io.Writer) error {
	var login struct {
		Username string   `json:"username"`
		Password string   `json:"password"`
		Roles    []string `json:"roles"`
	}
	data, err := io.ReadAll(io.LimitReader(input, 4097))
	if err != nil || len(data) > 4096 || !utf8.Valid(data) {
		return errors.New("invalid account input")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&login); err != nil {
		return errors.New("invalid account input")
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF || len(login.Password) < 1 || len(login.Password) > 1024 || !validAccount(login.Username, login.Roles) {
		return errors.New("invalid account input")
	}
	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		return err
	}
	hash, err := pbkdf2.Key(sha256.New, login.Password, salt, passwordIterations, 32)
	if err != nil {
		return err
	}
	account := Account{Username: login.Username, Salt: hex.EncodeToString(salt), Hash: hex.EncodeToString(hash), Roles: login.Roles}
	encoder := json.NewEncoder(output)
	encoder.SetIndent("", "    ")
	return encoder.Encode(account)
}
