package catalog

import (
	"context"
	"errors"
	"testing"
	"time"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
)

func requireInvalidConfigField(t *testing.T, err error, field string) {
	t.Helper()
	var actual *verdandi.Error
	if !errors.As(err, &actual) {
		t.Fatalf("error = %v, want *verdandi.Error", err)
	}
	if actual.Code != verdandi.CodeInvalid || actual.Field != field {
		t.Fatalf("error = %#v, want invalid field %q", actual, field)
	}
}

func TestNormalizeCatalogConfig(t *testing.T) {
	t.Parallel()
	runtime, err := (Config{Zone: "Alpha"}).normalize(2 * time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if runtime.Zone != "Alpha" || runtime.timeout != 2*time.Second || runtime.syncTimeout != 30*time.Second ||
		runtime.scanPageSize != 256 || runtime.maxInflightReads != 32 || runtime.eventBuffer != 256 ||
		runtime.errorBuffer != 64 || runtime.maxViewBytes != 0 || runtime.maxRecordBytes != 512*1024 ||
		runtime.recoveryInitial != 250*time.Millisecond || runtime.recoveryMax != 5*time.Second ||
		runtime.recoveryFactor != 2 || runtime.recoveryJitter != 10 {
		t.Fatalf("unexpected defaults: %#v", runtime)
	}
	zero := 0
	runtime, err = (Config{Zone: "Alpha", RecoveryJitterPercent: &zero}).normalize(2 * time.Second)
	if err != nil || runtime.recoveryJitter != 0 {
		t.Fatalf("disabled recovery jitter = %#v, %v", runtime, err)
	}
}

func TestNormalizeCatalogConfigAcceptsBoundaries(t *testing.T) {
	t.Parallel()
	zero := 0
	minimum := Config{
		Zone:                  "Alpha",
		SyncTimeout:           100 * time.Millisecond,
		ScanPageSize:          1,
		MaxInflightReads:      1,
		EventBufferCapacity:   1,
		ErrorBufferCapacity:   1,
		MaxRecordBytes:        1,
		RecoveryInitialDelay:  10 * time.Millisecond,
		RecoveryMaxDelay:      100 * time.Millisecond,
		RecoveryMultiplier:    1,
		RecoveryJitterPercent: &zero,
	}
	maximumJitter := 50
	maximum := Config{
		Zone:                  "Alpha",
		SyncTimeout:           time.Hour,
		ScanPageSize:          4096,
		MaxInflightReads:      256,
		EventBufferCapacity:   65_536,
		ErrorBufferCapacity:   4096,
		MaxViewBytes:          64 * 1024 * 1024 * 1024,
		MaxRecordBytes:        maximumEncodedBytes,
		RecoveryInitialDelay:  5 * time.Second,
		RecoveryMaxDelay:      30 * time.Second,
		RecoveryMultiplier:    8,
		RecoveryJitterPercent: &maximumJitter,
	}
	for name, config := range map[string]Config{"minimum": minimum, "maximum": maximum} {
		t.Run(name, func(t *testing.T) {
			if _, err := config.normalize(2 * time.Second); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestNormalizeCatalogConfigReportsExactInvalidField(t *testing.T) {
	t.Parallel()
	integer := func(value int) *int { return &value }
	tests := []struct {
		name    string
		field   string
		timeout time.Duration
		mutate  func(*Config)
	}{
		{name: "zone", field: "zone", mutate: func(config *Config) { config.Zone = "a:bad" }},
		{name: "root timeout", field: "timeout", timeout: -time.Second},
		{name: "sync timeout", field: "catalog.sync_timeout", mutate: func(config *Config) {
			config.SyncTimeout = 99 * time.Millisecond
		}},
		{name: "scan page", field: "catalog.scan_page_size", mutate: func(config *Config) {
			config.ScanPageSize = 4097
		}},
		{name: "inflight reads", field: "catalog.max_inflight_reads", mutate: func(config *Config) {
			config.MaxInflightReads = 257
		}},
		{name: "event buffer", field: "catalog.event_buffer_capacity", mutate: func(config *Config) {
			config.EventBufferCapacity = 65_537
		}},
		{name: "error buffer", field: "catalog.error_buffer_capacity", mutate: func(config *Config) {
			config.ErrorBufferCapacity = 4097
		}},
		{name: "view bytes below range", field: "catalog.max_view_bytes", mutate: func(config *Config) {
			config.MaxViewBytes = -1
		}},
		{name: "view bytes above range", field: "catalog.max_view_bytes", mutate: func(config *Config) {
			config.MaxViewBytes = 64*1024*1024*1024 + 1
		}},
		{name: "record bytes", field: "catalog.max_record_bytes", mutate: func(config *Config) {
			config.MaxRecordBytes = maximumEncodedBytes + 1
		}},
		{name: "recovery initial", field: "catalog.recovery.initial_delay", mutate: func(config *Config) {
			config.RecoveryInitialDelay = 9 * time.Millisecond
		}},
		{name: "recovery maximum", field: "catalog.recovery.max_delay", mutate: func(config *Config) {
			config.RecoveryMaxDelay = 99 * time.Millisecond
		}},
		{name: "recovery multiplier", field: "catalog.recovery.multiplier", mutate: func(config *Config) {
			config.RecoveryMultiplier = 9
		}},
		{name: "recovery jitter", field: "catalog.recovery.jitter_percent", mutate: func(config *Config) {
			config.RecoveryJitterPercent = integer(51)
		}},
		{name: "recovery relation", field: "catalog.recovery.initial_delay", mutate: func(config *Config) {
			config.RecoveryInitialDelay = 2 * time.Second
			config.RecoveryMaxDelay = time.Second
		}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			config := Config{Zone: "Alpha"}
			if test.mutate != nil {
				test.mutate(&config)
			}
			timeout := test.timeout
			if timeout == 0 {
				timeout = 2 * time.Second
			}
			_, err := config.normalize(timeout)
			requireInvalidConfigField(t, err, test.field)
		})
	}
}

func TestOpenRejectsNilTransport(t *testing.T) {
	t.Parallel()
	if _, err := Open(context.Background(), nil, Config{Zone: "Alpha"}); !verdandi.IsCode(err, verdandi.CodeClosed) {
		t.Fatalf("Open with nil transport = %v, want closed", err)
	}
}
