package main

import (
	"bytes"
	"context"
	"testing"
)

func TestCLIValidationAndCanceledStartup(t *testing.T) {
	for _, tc := range []struct {
		name string
		args []string
		code int
	}{
		{"help", []string{"--help"}, 0},
		{"version", []string{"--version"}, 0},
		{"canceled", []string{"--listen=127.0.0.1:0", "--log-level=DEBUG"}, 0},
		{"unknown", []string{"--super=127.0.0.1:7442"}, 2},
		{"address", []string{"--listen=invalid"}, 2},
		{"shutdown", []string{"--shutdown-timeout=0s"}, 2},
		{"capacity", []string{"--max-connections=0"}, 2},
		{"log-level", []string{"--log-level=invalid"}, 2},
		{"positional", []string{"unexpected"}, 2},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			cancel()
			var stdout, stderr bytes.Buffer
			if code := run(ctx, tc.args, &stdout, &stderr); code != tc.code {
				t.Fatalf("exit = %d, want %d; stderr: %s", code, tc.code, stderr.String())
			}
		})
	}
}
