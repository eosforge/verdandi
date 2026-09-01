package configuration

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

func TestLoadSharedExample(t *testing.T) {
	t.Parallel()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}
	config, err := LoadFile(filepath.Join(filepath.Dir(file), "..", "..", "..", "configuration.example.json"))
	if err != nil {
		t.Fatal(err)
	}
	redis, err := config.RedisConfig()
	if err != nil {
		t.Fatal(err)
	}
	if redis.Standalone == nil || redis.Standalone.Address != "127.0.0.1:6379" || redis.Timeout != 2*time.Second {
		t.Fatalf("unexpected Redis config: %#v", redis)
	}
	registration, err := config.RegistrationConfig()
	if err != nil {
		t.Fatal(err)
	}
	if registration == nil || registration.Zone != "Alpha" || registration.Policy.DataMaxFields != 32 {
		t.Fatalf("unexpected Registration config: %#v", registration)
	}
	catalog, err := config.CatalogConfig()
	if err != nil {
		t.Fatal(err)
	}
	if catalog == nil || catalog.Zone != "Alpha" || catalog.MaxRecordBytes != 512*1024 {
		t.Fatalf("unexpected Catalog config: %#v", catalog)
	}
}

func TestParseJSONStrictness(t *testing.T) {
	t.Parallel()
	tests := []struct {
		name   string
		source string
		field  string
	}{
		{name: "unknown", source: `{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"future":true}}`, field: "json"},
		{name: "duplicate", source: `{"version":"v1","version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"]}}`, field: "json"},
		{name: "trailing", source: `{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"]}} {}`, field: "json"},
		{name: "null", source: `{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"timeout_ms":null}}`, field: "json"},
		{name: "version", source: `{"version":"v2","redis":{"mode":"standalone","addresses":["localhost:6379"]}}`, field: "version"},
		{
			name:   "explicit zero timeout",
			source: `{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"timeout_ms":0}}`,
			field:  "redis.timeout_ms",
		},
		{name: "unbracketed IPv6", source: `{"version":"v1","redis":{"mode":"standalone","addresses":["::1:6379"]}}`, field: "redis.addresses"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			t.Parallel()
			_, err := ParseJSON([]byte(test.source))
			var actual *verdandi.Error
			if !errors.As(err, &actual) || actual.Code != verdandi.CodeInvalid || actual.Field != test.field {
				t.Fatalf("got %v, want invalid field %q", err, test.field)
			}
		})
	}
}

func TestJSONDefaultsAndExplicitZero(t *testing.T) {
	t.Parallel()
	config, err := ParseJSON([]byte(`{
		"version":"v1",
		"redis":{"mode":"standalone","addresses":["localhost:6379"],"reconnect":{"jitter_percent":0}},
		"registration":{"zone":"Alpha","selector":{"view_publish_interval_ms":0,"max_retained_bytes":0,"clock_uncertainty_ms":0}}
	}`))
	if err != nil {
		t.Fatal(err)
	}
	redis, err := config.RedisConfig()
	if err != nil {
		t.Fatal(err)
	}
	if redis.Reconnect.JitterPercent == nil || *redis.Reconnect.JitterPercent != 0 {
		t.Fatalf("explicit jitter zero was not preserved: %#v", redis.Reconnect.JitterPercent)
	}
	if redis.Timeout != 2*time.Second || redis.Pool.MinConnections != 1 || redis.Pool.MaxConnections != 4 {
		t.Fatalf("JSON defaults were not materialized: %#v", redis)
	}
	registration, err := config.RegistrationConfig()
	if err != nil {
		t.Fatal(err)
	}
	if registration == nil || registration.SelectorPublishInterval == nil || *registration.SelectorPublishInterval != 0 {
		t.Fatalf("explicit publish zero was not preserved: %#v", registration)
	}
	if registration.BufferCapacity != 8 || registration.MinimumRenewInterval != 100*time.Millisecond {
		t.Fatalf("Registration defaults were not materialized: %#v", registration)
	}
	if registration.SelectorRetainedBytes == nil || *registration.SelectorRetainedBytes != 0 {
		t.Fatalf("explicit retained zero was not preserved: %#v", registration.SelectorRetainedBytes)
	}
	if registration.ClockUncertainty == nil || *registration.ClockUncertainty != 0 {
		t.Fatalf("explicit clock zero was not preserved: %#v", registration.ClockUncertainty)
	}
}

func TestSentinelTopology(t *testing.T) {
	t.Parallel()
	config, err := ParseJSON([]byte(`{
		"version":"v1",
		"redis":{
			"mode":"sentinel",
			"addresses":["sentinel-a:26379","[::1]:26379"],
			"master_name":"primary",
			"auth":{"username":"data","password":"secret"},
			"sentinel_auth":{"username":"sentinel","password":"secret"}
		}
	}`))
	if err != nil {
		t.Fatal(err)
	}
	redis, err := config.RedisConfig()
	if err != nil {
		t.Fatal(err)
	}
	if redis.Sentinel == nil || len(redis.Sentinel.Addresses) != 2 || redis.Sentinel.MasterName != "primary" {
		t.Fatalf("unexpected Sentinel config: %#v", redis.Sentinel)
	}
}

func TestLoadJSONLimitsInput(t *testing.T) {
	t.Parallel()
	if _, err := LoadJSON(nil); !verdandi.IsCode(err, verdandi.CodeInvalid) {
		t.Fatalf("nil reader: %v", err)
	}
	oversized := strings.Repeat(" ", maximumJSONBytes+1)
	if _, err := LoadJSON(bytes.NewBufferString(oversized)); !verdandi.IsCode(err, verdandi.CodeCapacity) {
		t.Fatalf("oversized reader: %v", err)
	}
}

func TestTLSObjectBuildsPrivateRootsClientCertificateAndSNI(t *testing.T) {
	t.Parallel()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}
	fixture := filepath.Join(filepath.Dir(file), "..", "..", "..", "testkit", "tls")
	config := Config{
		Version: "v1",
		Redis: Redis{
			Mode:      "standalone",
			Addresses: []string{"127.0.0.1:6379"},
			TLS: TLS{
				Enabled:     true,
				SystemRoots: new(false),
				ServerName:  "redis.test",
				CAFile:      filepath.Join(fixture, "certificate.pem"),
				CertFile:    filepath.Join(fixture, "certificate.pem"),
				KeyFile:     filepath.Join(fixture, "private-key.pem"),
			},
		},
	}
	native, err := config.RedisConfig()
	if err != nil {
		t.Fatal(err)
	}
	if native.Standalone == nil || native.Standalone.TLS == nil {
		t.Fatal("TLS config was not created")
	}
	tlsConfig := native.Standalone.TLS
	if tlsConfig.ServerName != "redis.test" || tlsConfig.RootCAs == nil || len(tlsConfig.Certificates) != 1 {
		t.Fatalf("unexpected TLS config: %#v", tlsConfig)
	}
}

func TestParseJSONDefersTLSFileIO(t *testing.T) {
	t.Parallel()
	source, err := json.Marshal(Config{
		Version: "v1",
		Redis: Redis{
			Mode:      "standalone",
			Addresses: []string{"127.0.0.1:6379"},
			TLS: TLS{
				Enabled:     true,
				SystemRoots: new(false),
				CAFile:      filepath.Join(t.TempDir(), "missing.pem"),
			},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	config, err := ParseJSON(source)
	if err != nil {
		t.Fatalf("ParseJSON performed certificate I/O: %v", err)
	}
	_, err = config.RedisConfig()
	var actual *verdandi.Error
	if !errors.As(err, &actual) || actual.Code != verdandi.CodeUnavailable || actual.Field != "redis.tls.ca_file" {
		t.Fatalf("got %v, want unavailable redis.tls.ca_file", err)
	}
}

func TestTLSObjectRejectsInvalidRelationships(t *testing.T) {
	t.Parallel()
	tests := []struct {
		name   string
		source string
		field  string
	}{
		{
			name:   "legacy boolean",
			source: `{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"tls":true}}`,
			field:  "json",
		},
		{
			name:   "disabled custom field",
			source: `{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"tls":{"server_name":"redis.test"}}}`,
			field:  "redis.tls",
		},
		{
			name:   "empty trust set",
			source: `{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"tls":{"enabled":true,"system_roots":false}}}`,
			field:  "redis.tls.ca_file",
		},
		{
			name:   "certificate without key",
			source: `{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"tls":{"enabled":true,"cert_file":"client.pem"}}}`,
			field:  "redis.tls.cert_file",
		},
		{
			name: "sentinel fixed server name",
			source: `{"version":"v1","redis":{"mode":"sentinel","addresses":["localhost:26379"],"master_name":"primary",` +
				`"tls":{"enabled":true,"server_name":"redis.test"}}}`,
			field: "redis.tls.server_name",
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			t.Parallel()
			_, err := ParseJSON([]byte(test.source))
			var actual *verdandi.Error
			if !errors.As(err, &actual) || actual.Code != verdandi.CodeInvalid || actual.Field != test.field {
				t.Fatalf("got %v, want invalid field %q", err, test.field)
			}
		})
	}
}

func TestTLSFileReadIsBounded(t *testing.T) {
	t.Parallel()
	path := filepath.Join(t.TempDir(), "oversized.pem")
	if err := os.WriteFile(path, bytes.Repeat([]byte{'x'}, maximumTLSFileBytes+1), 0o600); err != nil {
		t.Fatal(err)
	}
	config := Config{
		Redis: Redis{
			Mode:      "standalone",
			Addresses: []string{"127.0.0.1:6379"},
			TLS: TLS{
				Enabled:     true,
				SystemRoots: new(false),
				CAFile:      path,
			},
		},
	}
	_, err := config.RedisConfig()
	var actual *verdandi.Error
	if !errors.As(err, &actual) || actual.Code != verdandi.CodeCapacity || actual.Field != "redis.tls.ca_file" {
		t.Fatalf("got %v, want capacity redis.tls.ca_file", err)
	}
}
