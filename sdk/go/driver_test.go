package verdandi

import (
	"context"
	"testing"
	"time"

	redis "github.com/redis/go-redis/v9"
)

func TestRedisDriverOptionsDisableCommandRetries(t *testing.T) {
	t.Parallel()
	runtime, err := (Config{Standalone: &Standalone{Address: "127.0.0.1:6379"}}).normalize()
	if err != nil {
		t.Fatal(err)
	}
	// 此测试只检查 Options 映射；禁用最小空闲连接，避免 go-redis 构造时真实启动后台拨号。
	// 默认值 1 已由 TestNormalizeTransportConfig 独立验证。
	runtime.poolMin = 0
	client := newRedisClient(runtime)
	defer func() { _ = client.Close() }()
	options := client.Options()
	// go-redis 接收 -1 后会把“零次重试”规范化为 0；两个退避值也应同时归零。
	if options.MaxRetries != 0 || options.MinRetryBackoff != 0 || options.MaxRetryBackoff != 0 ||
		options.DialTimeout != 5*time.Second || options.PoolSize != 4 ||
		options.MinIdleConns != 0 || options.MaxActiveConns != 4 || options.ConnMaxIdleTime != 10*time.Second ||
		options.DialerRetryTimeout != 100*time.Millisecond || options.DialerRetryBackoff != nil {
		t.Fatalf("unexpected driver options: %#v", options)
	}
}

func TestWriteTransportErrorsRemainAmbiguous(t *testing.T) {
	t.Parallel()
	if err := wrapDriver(CodeAmbiguous, context.DeadlineExceeded); !IsCode(err, CodeAmbiguous) {
		t.Fatalf("write timeout error = %v, want ambiguous", err)
	}
	if err := wrapDriver(CodeUnavailable, context.DeadlineExceeded); !IsCode(err, CodeDeadline) {
		t.Fatalf("read timeout error = %v, want deadline", err)
	}
	if err := wrapDriver(CodeAmbiguous, redis.ErrClosed); !IsCode(err, CodeClosed) {
		t.Fatalf("closed write error = %v, want closed", err)
	}
}
