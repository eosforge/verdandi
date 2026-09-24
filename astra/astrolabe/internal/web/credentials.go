package web

import (
	"encoding/base64"
	"net/http"
	"strconv"

	"github.com/eosforge/verdandi/astra/internal/generated/comet"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"google.golang.org/protobuf/proto"
)

// credentials 与所有管理写入共用浏览器登录/Origin 防护, 内部版本仍由唯一 Polaris 严格检查.
// response/request 为当前 HTTP 交换; 凭据存于内部 __auth/comet 范围, 版本乐观并发由 Polaris 裁决.
func (server *Server) credentials(response http.ResponseWriter, request *http.Request) {
	// 凭据固定存于内部范围, 不接受调用方指定范围.
	scope := &comet.Scope{Sector: []byte("__auth"), Spectrum: []byte("comet")}
	if request.URL.RawQuery != "" {
		// 查询串无意义, 直接拒绝, 不进入方法分发.
		problem(response, http.StatusBadRequest, "input", "unapplied")
		return
	}
	if request.Method == http.MethodGet {
		server.load(response, request, scope) // 返回 APIKEY 与 redacted, 不返回 SECRET.
		return
	}
	if request.Method != http.MethodPost {
		problem(response, http.StatusMethodNotAllowed, "method", "unapplied")
		return
	}
	// 输入三选一: 设置密钥 (secret) 或删除 (erase=true), version 必填用于乐观并发.
	var input struct {
		Key     string  `json:"key"`
		Secret  *string `json:"secret"`
		Erase   *bool   `json:"erase"`
		Version string  `json:"version"`
	}
	if body(response, request, 8<<10, &input) != nil || !text(input.Key, 128) || (input.Secret == nil) == (input.Erase == nil) || input.Erase != nil && !*input.Erase {
		problem(response, http.StatusBadRequest, "input", "unapplied")
		return
	}
	// 版本必须为规范十进制非零 uint64, 变体写法拒绝, 防止版本比较歧义.
	version, err := strconv.ParseUint(input.Version, 10, 64)
	if err != nil || version == 0 || strconv.FormatUint(version, 10) != input.Version {
		problem(response, http.StatusBadRequest, "version", "unapplied")
		return
	}
	change := &comet.AlmanacChange{Key: input.Key}
	if input.Secret != nil {
		// 密钥经标准 base64 严格解码, 长度 1..4096 字节, 编码为凭据消息后内存立即清零.
		secret, err := base64.StdEncoding.Strict().DecodeString(*input.Secret)
		if err != nil || len(secret) == 0 || len(secret) > 4096 {
			problem(response, http.StatusBadRequest, "input", "unapplied")
			return
		}
		value, err := proto.Marshal(&orbit.Credential{Secret: secret})
		clear(secret)
		if err != nil {
			problem(response, http.StatusInternalServerError, "encoding", "unapplied")
			return
		}
		change.Action = &comet.AlmanacChange_Value{Value: value}
	} else {
		// 删除分支构造擦除动作, 无载荷.
		change.Action = &comet.AlmanacChange_Erase{Erase: &comet.Empty{}}
	}
	server.submit(response, request, scope, change, version)
}
