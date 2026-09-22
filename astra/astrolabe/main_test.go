package main

import "testing"

// 冷配置测试不读取证书/账号, 不建立网络或批准新的依赖获取.
func TestParse(t *testing.T) {
	base := []string{"--listen=127.0.0.1:8553", "--super=127.0.0.1:8554", "--galaxy=test", "--account=account.json", "--public=https://admin.example"}
	if config, err := parse(base); err != nil || config.advertise != config.listen || config.group != "default" {
		t.Fatal("valid deployment rejected")
	}
	for _, extra := range [][]string{
		{"--listen=127.0.0.1:8555"},
		{"--sessions=0"},
		{"--origins=*"},
		{"--origins=https://admin.example"},
		{"--origins=https://other.example,https://other.example"},
		{"--http=true", "--http=false"},
		{"positional"},
	} {
		if _, err := parse(append(append([]string{}, base...), extra...)); err == nil {
			t.Fatalf("invalid options accepted: %v", extra)
		}
	}
	if _, err := parse([]string{"--listen=192.168.0.5:8553", "--super=127.0.0.1:8554", "--galaxy=test", "--account=account.json", "--public=http://admin.example", "--http"}); err == nil {
		t.Fatal("non-loopback plaintext management accepted")
	}
	if _, err := parse(append(append([]string{}, base...), "--http", "--crosssite")); err != nil {
		t.Fatal("explicit HTTPS reverse-proxy deployment rejected")
	}
}
