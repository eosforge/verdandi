package web

import (
	"encoding/base64"
	"net/http"
	"strings"
	"testing"

	"github.com/eosforge/verdandi/astra/internal/generated/comet"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"github.com/eosforge/verdandi/astra/internal/generated/polaris"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// 浏览器只提交新 SECRET, 源数据由既有 Protobuf 生成器编码, 不要求前端编写内部协议.
func TestCredentials(t *testing.T) {
	backend := &backend{}
	server := server(t, backend)
	cookie := login(t, server)
	secret := base64.StdEncoding.EncodeToString([]byte("private-secret"))
	input := `{"key":"application","secret":"` + secret + `","version":"1"}`
	response := request(server, http.MethodPost, "/api/credentials", input, cookie)
	if response.Code != http.StatusOK || backend.calls.Load() != 1 || backend.request.Scope.Sector != "__auth" || backend.request.Scope.Spectrum != "comet" || backend.request.Version != 1 || backend.request.Change.Key != "application" {
		t.Fatal("credential did not use one authority commit")
	}
	var decoded orbit.Credential
	if proto.Unmarshal(backend.request.Change.GetValue(), &decoded) != nil || string(decoded.Secret) != "private-secret" {
		t.Fatal("credential encoding or ownership failed")
	}
	if strings.Contains(response.Body.String(), secret) || strings.Contains(response.Body.String(), "private-secret") {
		t.Fatal("credential echoed to browser")
	}
	for _, invalid := range []string{`{"key":"a","secret":"","version":"2"}`, `{"key":"a","secret":"!","version":"2"}`, `{"key":"a","erase":false,"version":"2"}`, `{"key":"a","erase":true,"version":"02"}`, `{"key":"a","erase":true,"secret":"AA==","version":"2"}`, `{"key":"a","erase":true,"version":2}`} {
		response = request(server, http.MethodPost, "/api/credentials", invalid, cookie)
		if response.Code != http.StatusBadRequest || backend.calls.Load() != 1 {
			t.Fatal("invalid credentials reached authority")
		}
	}
	backend.failure = status.Error(codes.Unavailable, "must-not-expose-secret")
	response = request(server, http.MethodPost, "/api/credentials", `{"key":"application","erase":true,"version":"2"}`, cookie)
	if response.Code != http.StatusServiceUnavailable || backend.calls.Load() != 2 || !strings.Contains(response.Body.String(), `"effect":"unknown"`) || strings.Contains(response.Body.String(), "must-not-expose") {
		t.Fatal("uncertain credential removal retried or misreported")
	}
}

// 专有入口和通用 Almanac 读取都执行相同脱敏, 不能换一个 URL 绕过现存 SECRET 保护.
func TestCredentialRead(t *testing.T) {
	encoded, err := proto.Marshal(&orbit.Credential{Secret: []byte("private-secret")})
	if err != nil {
		t.Fatal(err)
	}
	backend := &backend{pages: []*polaris.Snapshot{{Scope: &comet.Scope{Sector: "__auth", Spectrum: "comet"}, Version: proto.Uint64(3), Complete: true, Entries: []*comet.AlmanacChange{{Key: "application", Action: &comet.AlmanacChange_Value{Value: encoded}}}}}}
	server := server(t, backend)
	cookie := login(t, server)
	for _, path := range []string{"/api/credentials", "/api/almanac?sector=__auth&spectrum=comet"} {
		response := request(server, http.MethodGet, path, "", cookie)
		body := response.Body.String()
		if response.Code != http.StatusOK || !strings.Contains(body, `"key":"application"`) || !strings.Contains(body, `"redacted":true`) || !strings.Contains(body, `"complete":true`) || strings.Contains(body, `"value"`) || strings.Contains(body, base64.StdEncoding.EncodeToString(encoded)) {
			t.Fatal("credential read failed to redact complete snapshot")
		}
	}
	backend.pages[0].Entries[0].Action = &comet.AlmanacChange_Value{Value: []byte{0xff}}
	response := request(server, http.MethodGet, "/api/credentials", "", cookie)
	if response.Code != http.StatusBadGateway || strings.Contains(response.Body.String(), `"complete":true`) {
		t.Fatal("invalid first credential page claimed successful snapshot")
	}
}
