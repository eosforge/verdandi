package main

import "testing"

// Polaris 与 Astrolabe 都使用相同的重复参数拒绝规则, 不只修补一个命令入口.
func TestParse(t *testing.T) {
	base := []string{"--listen=127.0.0.1:9553", "--super=127.0.0.1:9554", "--galaxy=test"}
	if config, err := parse(base); err != nil || config.advertise != config.listen || config.initialize {
		t.Fatal("valid Polaris configuration rejected")
	}
	for _, extra := range [][]string{{"--listen=127.0.0.1:9555"}, {"--init", "--init=false"}, {"--snapshot-bytes=1"}, {"--unknown=1"}, {"positional"}} {
		if _, err := parse(append(append([]string{}, base...), extra...)); err == nil {
			t.Fatalf("invalid options accepted: %v", extra)
		}
	}
}
