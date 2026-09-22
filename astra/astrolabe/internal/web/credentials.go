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
func (server *Server) credentials(response http.ResponseWriter, request *http.Request) {
	scope := &comet.Scope{Sector: "__auth", Spectrum: "comet"}
	if request.URL.RawQuery != "" {
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
	version, err := strconv.ParseUint(input.Version, 10, 64)
	if err != nil || version == 0 || strconv.FormatUint(version, 10) != input.Version {
		problem(response, http.StatusBadRequest, "version", "unapplied")
		return
	}
	change := &comet.AlmanacChange{Key: input.Key}
	if input.Secret != nil {
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
		change.Action = &comet.AlmanacChange_Erase{Erase: &comet.Empty{}}
	}
	server.submit(response, request, scope, change, version)
}
