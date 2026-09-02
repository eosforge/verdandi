package verdandi

import (
	"bytes"
	"crypto/tls"
	"errors"
	"testing"
	"time"
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
		{name: "invalid endpoint", config: Config{Standalone: &Standalone{Address: "127.0.0.1"}}, code: CodeInvalid},
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
				runtime.poolIdle != 10*time.Second || runtime.reconnectDelay != 100*time.Millisecond {
				t.Fatalf("runtime defaults = %#v", runtime)
			}
			if (runtime.standalone == nil) == (runtime.sentinel == nil) {
				t.Fatalf("runtime topology = %#v", runtime)
			}
		})
	}
}

func TestNormalizeTransportConfigAcceptsBoundaries(t *testing.T) {
	t.Parallel()
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
			Delay: 10 * time.Millisecond,
		},
	}
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
			Delay: 30 * time.Second,
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
		{name: "standalone address without port", field: "standalone.address", config: func() Config {
			return Config{Standalone: &Standalone{Address: "localhost"}}
		}},
		{name: "standalone port out of range", field: "standalone.address", config: func() Config {
			return Config{Standalone: &Standalone{Address: "localhost:65536"}}
		}},
		{name: "standalone port leading plus", field: "standalone.address", config: func() Config {
			return Config{Standalone: &Standalone{Address: "localhost:+6379"}}
		}},
		{name: "standalone host whitespace", field: "standalone.address", config: func() Config {
			return Config{Standalone: &Standalone{Address: "bad host:6379"}}
		}},
		{name: "standalone address invalid UTF-8", field: "standalone.address", config: func() Config {
			return Config{Standalone: &Standalone{Address: string([]byte{0xff}) + ":6379"}}
		}},
		{name: "sentinel addresses missing", field: "sentinel.addresses", config: func() Config {
			return Config{Sentinel: &Sentinel{MasterName: "primary"}}
		}},
		{name: "sentinel address empty", field: "sentinel.addresses", config: func() Config {
			return Config{Sentinel: &Sentinel{Addresses: []string{" "}, MasterName: "primary"}}
		}},
		{name: "sentinel address malformed", field: "sentinel.addresses", config: func() Config {
			return Config{Sentinel: &Sentinel{Addresses: []string{"localhost"}, MasterName: "primary"}}
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
		{name: "reconnect delay", field: "reconnect.delay", config: func() Config {
			config := standalone()
			config.Reconnect.Delay = 9 * time.Millisecond
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
		{name: "insecure TLS", field: "tls.insecure_skip_verify", config: func() Config {
			config := standalone()
			config.Standalone.TLS = &tls.Config{InsecureSkipVerify: true} //nolint:gosec // 测试拒绝路径。
			return config
		}},
		{name: "obsolete TLS", field: "tls.min_version", config: func() Config {
			config := standalone()
			config.Standalone.TLS = &tls.Config{MinVersion: tls.VersionTLS11}
			return config
		}},
		{name: "reversed TLS version range", field: "tls.min_version", config: func() Config {
			config := standalone()
			config.Standalone.TLS = &tls.Config{MinVersion: tls.VersionTLS13, MaxVersion: tls.VersionTLS12}
			return config
		}},
		{name: "sentinel missing fixed TLS identity", field: "tls.server_name", config: func() Config {
			return Config{Sentinel: &Sentinel{Addresses: []string{"127.0.0.1:26379"}, MasterName: "primary", TLS: &tls.Config{}}}
		}},
		{name: "standalone SNI whitespace", field: "tls.server_name", config: func() Config {
			config := standalone()
			config.Standalone.TLS = &tls.Config{ServerName: "redis test"}
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

func TestNormalizeTransportConfigCopiesMutableInputs(t *testing.T) {
	t.Parallel()
	addresses := []string{"127.0.0.1:26379"}
	tlsConfig := &tls.Config{RootCAs: nil, ServerName: "redis.test"}
	runtime, err := (Config{Sentinel: &Sentinel{Addresses: addresses, MasterName: "primary", TLS: tlsConfig}}).normalize()
	if err != nil {
		t.Fatal(err)
	}
	addresses[0] = "127.0.0.1:1"
	tlsConfig.ServerName = "mutated"
	if runtime.sentinel.Addresses[0] != "127.0.0.1:26379" || runtime.sentinel.TLS.ServerName != "redis.test" || runtime.sentinel.TLS.MinVersion != tls.VersionTLS12 {
		t.Fatalf("runtime retained caller aliases: %#v", runtime.sentinel)
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
