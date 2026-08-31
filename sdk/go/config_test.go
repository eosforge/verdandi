package verdandi

import (
	"bytes"
	"context"
	"errors"
	"testing"
	"time"

	redis "github.com/redis/go-redis/v9"
)

func requireInvalidConfigField(t *testing.T, err error, field string) {
	t.Helper()
	var actual *Error
	if !errors.As(err, &actual) {
		t.Fatalf("error = %v, want *Error", err)
	}
	if actual.Code != CodeInvalid || actual.Field != field {
		t.Fatalf("error = %#v, want invalid field %q", actual, field)
	}
}

func TestNormalizeTransportConfig(t *testing.T) {
	t.Parallel()
	tests := []struct {
		name   string
		config Config
		code   Code
	}{
		{name: "standalone", config: Config{Standalone: &Standalone{Address: "127.0.0.1:6379"}}},
		{name: "sentinel", config: Config{Sentinel: &Sentinel{Addresses: []string{"127.0.0.1:26379"}, MasterName: "primary"}}},
		{name: "missing topology", config: Config{}, code: CodeInvalid},
		{
			name: "two topologies",
			config: Config{
				Standalone: &Standalone{Address: "127.0.0.1:6379"},
				Sentinel:   &Sentinel{Addresses: []string{"127.0.0.1:26379"}, MasterName: "primary"},
			},
			code: CodeInvalid,
		},
		{
			name: "invalid timeout",
			config: Config{
				Standalone: &Standalone{Address: "127.0.0.1:6379"},
				Timeout:    -time.Second,
			},
			code: CodeInvalid,
		},
		{
			name: "database above shared range",
			config: Config{
				Standalone: &Standalone{Address: "127.0.0.1:6379", Database: 256},
			},
			code: CodeInvalid,
		},
		{
			name: "pool minimum above maximum",
			config: Config{
				Standalone: &Standalone{Address: "127.0.0.1:6379"},
				Pool:       PoolConfig{MinConnections: 5, MaxConnections: 4},
			},
			code: CodeInvalid,
		},
		{
			name: "reconnect minimum above maximum",
			config: Config{
				Standalone: &Standalone{Address: "127.0.0.1:6379"},
				Reconnect: ReconnectConfig{
					InitialDelay: 2 * time.Second,
					MaxDelay:     time.Second,
				},
			},
			code: CodeInvalid,
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			runtime, err := test.config.normalize()
			if test.code != "" {
				if !IsCode(err, test.code) {
					t.Fatalf("Config.normalize() error = %v, want code %q", err, test.code)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if runtime.timeout != 2*time.Second {
				t.Fatalf("timeout = %v", runtime.timeout)
			}
			if runtime.connectTimeout != 5*time.Second || runtime.poolMin != 1 || runtime.poolMax != 4 ||
				runtime.poolIdle != 10*time.Second || runtime.reconnectInitial != 100*time.Millisecond ||
				runtime.reconnectMax != 5*time.Second || runtime.reconnectFactor != 2 || runtime.reconnectJitter != 10 {
				t.Fatalf("runtime defaults = %#v", runtime)
			}
			if runtime.Standalone != test.config.Standalone || runtime.Sentinel != test.config.Sentinel ||
				runtime.Timeout != test.config.Timeout {
				t.Fatalf("runtime config did not retain the validated transport: %#v", runtime.Config)
			}
		})
	}
}

func TestNormalizeTransportConfigAcceptsBoundaries(t *testing.T) {
	t.Parallel()
	zero := 0
	minimum := Config{
		Standalone:     &Standalone{Address: "127.0.0.1:6379"},
		Timeout:        10 * time.Millisecond,
		ConnectTimeout: 20 * time.Millisecond,
		Pool: PoolConfig{
			MinConnections: 1,
			MaxConnections: 1,
			IdleTimeout:    time.Second,
		},
		Reconnect: ReconnectConfig{
			InitialDelay:  10 * time.Millisecond,
			MaxDelay:      100 * time.Millisecond,
			Multiplier:    1,
			JitterPercent: &zero,
		},
	}
	maximumJitter := 50
	maximum := Config{
		Standalone:     &Standalone{Address: "127.0.0.1:6379", Database: 255},
		Timeout:        15 * time.Second,
		ConnectTimeout: 30 * time.Second,
		Pool: PoolConfig{
			MinConnections: 1024,
			MaxConnections: 1024,
			IdleTimeout:    time.Hour,
		},
		Reconnect: ReconnectConfig{
			InitialDelay:  5 * time.Second,
			MaxDelay:      30 * time.Second,
			Multiplier:    8,
			JitterPercent: &maximumJitter,
		},
	}
	for name, config := range map[string]Config{"minimum": minimum, "maximum": maximum} {
		t.Run(name, func(t *testing.T) {
			if _, err := config.normalize(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestNormalizeTransportConfigReportsExactInvalidField(t *testing.T) {
	t.Parallel()
	standalone := func() Config {
		return Config{Standalone: &Standalone{Address: "127.0.0.1:6379"}}
	}
	integer := func(value int) *int { return &value }
	tests := []struct {
		name   string
		field  string
		config func() Config
	}{
		{name: "missing topology", field: "topology", config: func() Config { return Config{} }},
		{name: "two topologies", field: "topology", config: func() Config {
			config := standalone()
			config.Sentinel = &Sentinel{Addresses: []string{"127.0.0.1:26379"}, MasterName: "primary"}
			return config
		}},
		{name: "standalone address", field: "standalone.address", config: func() Config {
			return Config{Standalone: &Standalone{}}
		}},
		{name: "sentinel addresses missing", field: "sentinel.addresses", config: func() Config {
			return Config{Sentinel: &Sentinel{MasterName: "primary"}}
		}},
		{name: "sentinel address empty", field: "sentinel.addresses", config: func() Config {
			return Config{Sentinel: &Sentinel{Addresses: []string{" "}, MasterName: "primary"}}
		}},
		{name: "sentinel master", field: "sentinel.master_name", config: func() Config {
			return Config{Sentinel: &Sentinel{Addresses: []string{"127.0.0.1:26379"}}}
		}},
		{name: "timeout below range", field: "timeout", config: func() Config {
			config := standalone()
			config.Timeout = 9 * time.Millisecond
			return config
		}},
		{name: "timeout above range", field: "timeout", config: func() Config {
			config := standalone()
			config.Timeout = 15*time.Second + time.Millisecond
			return config
		}},
		{name: "timeout fractional millisecond", field: "timeout", config: func() Config {
			config := standalone()
			config.Timeout = 10*time.Millisecond + time.Nanosecond
			return config
		}},
		{name: "connect timeout", field: "connect_timeout", config: func() Config {
			config := standalone()
			config.ConnectTimeout = 19 * time.Millisecond
			return config
		}},
		{name: "pool idle timeout", field: "pool.idle_timeout", config: func() Config {
			config := standalone()
			config.Pool.IdleTimeout = time.Second - time.Millisecond
			return config
		}},
		{name: "reconnect initial delay", field: "reconnect.initial_delay", config: func() Config {
			config := standalone()
			config.Reconnect.InitialDelay = 9 * time.Millisecond
			return config
		}},
		{name: "reconnect max delay", field: "reconnect.max_delay", config: func() Config {
			config := standalone()
			config.Reconnect.MaxDelay = 99 * time.Millisecond
			return config
		}},
		{name: "reconnect jitter below range", field: "reconnect.jitter_percent", config: func() Config {
			config := standalone()
			config.Reconnect.JitterPercent = integer(-1)
			return config
		}},
		{name: "reconnect jitter above range", field: "reconnect.jitter_percent", config: func() Config {
			config := standalone()
			config.Reconnect.JitterPercent = integer(51)
			return config
		}},
		{name: "database below range", field: "database", config: func() Config {
			config := standalone()
			config.Standalone.Database = -1
			return config
		}},
		{name: "database above range", field: "database", config: func() Config {
			config := standalone()
			config.Standalone.Database = 256
			return config
		}},
		{name: "pool minimum", field: "pool.min_connections", config: func() Config {
			config := standalone()
			config.Pool.MinConnections = -1
			return config
		}},
		{name: "pool maximum", field: "pool.max_connections", config: func() Config {
			config := standalone()
			config.Pool.MaxConnections = 1025
			return config
		}},
		{name: "pool relation", field: "pool.min_connections", config: func() Config {
			config := standalone()
			config.Pool.MinConnections = 5
			config.Pool.MaxConnections = 4
			return config
		}},
		{name: "reconnect multiplier", field: "reconnect.multiplier", config: func() Config {
			config := standalone()
			config.Reconnect.Multiplier = 9
			return config
		}},
		{name: "reconnect relation", field: "reconnect.initial_delay", config: func() Config {
			config := standalone()
			config.Reconnect.InitialDelay = 2 * time.Second
			config.Reconnect.MaxDelay = time.Second
			return config
		}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			_, err := test.config().normalize()
			requireInvalidConfigField(t, err, test.field)
		})
	}
}

func TestReconnectDelayIsBounded(t *testing.T) {
	t.Parallel()
	for attempt := range 64 {
		delay := reconnectDelay(attempt, 100*time.Millisecond, 5*time.Second, 2, 0)
		if delay < 100*time.Millisecond || delay > 5*time.Second {
			t.Fatalf("attempt %d delay = %v", attempt, delay)
		}
	}
}

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
		options.MinIdleConns != 0 || options.MaxActiveConns != 4 || options.ConnMaxIdleTime != 10*time.Second {
		t.Fatalf("unexpected driver options: %#v", options)
	}
}

func TestFieldsAreDefensivelyCopied(t *testing.T) {
	t.Parallel()
	source := Fields{"load": {1, 2, 3}}
	copy := cloneFields(source)
	source["load"][0] = 9
	source["new"] = []byte("value")
	if !bytes.Equal(copy["load"], []byte{1, 2, 3}) || len(copy) != 1 {
		t.Fatalf("cloneFields() retained caller aliases: %#v", copy)
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
