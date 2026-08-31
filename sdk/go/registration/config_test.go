package registration

import (
	"bytes"
	"context"
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

func TestNormalizeRegistrationConfig(t *testing.T) {
	t.Parallel()
	runtime, err := (Config{Zone: "Alpha"}).normalize(2 * time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if runtime.registrationBuffer != 8 || runtime.registrationErrorBuffer != 16 ||
		runtime.minimumRenewInterval != 100*time.Millisecond || runtime.renewJitterPercent != 10 ||
		runtime.policyRefreshJitter != 10 || runtime.selectorPageSize != 256 ||
		runtime.selectorEventBuffer != 4096 || runtime.selectorEventBytes != 64*1024*1024 ||
		runtime.selectorPublishInterval != 10*time.Millisecond || runtime.selectorSyncTimeout != 30*time.Second ||
		runtime.selectorMaxBytes != 256*1024*1024 || runtime.selectorRetainedBytes != 64*1024*1024 ||
		runtime.clockRefresh != 30*time.Second || runtime.clockUncertainty != time.Millisecond ||
		runtime.selectorErrorBuffer != 16 || runtime.selectorRecoveryInitial != 100*time.Millisecond ||
		runtime.selectorRecoveryMax != 5*time.Second || runtime.selectorRecoveryFactor != 2 ||
		runtime.selectorRecoveryJitter != 50 {
		t.Fatalf("unexpected defaults: %#v", runtime)
	}
	zeroInt := 0
	zeroDuration := time.Duration(0)
	runtime, err = (Config{
		Zone:                          "Alpha",
		RenewJitterPercent:            &zeroInt,
		PolicyRefreshJitterPercent:    &zeroInt,
		SelectorPublishInterval:       &zeroDuration,
		SelectorRetainedBytes:         &zeroInt,
		ClockUncertainty:              &zeroDuration,
		SelectorRecoveryJitterPercent: &zeroInt,
	}).normalize(2 * time.Second)
	if err != nil || runtime.renewJitterPercent != 0 || runtime.policyRefreshJitter != 0 ||
		runtime.selectorPublishInterval != 0 || runtime.selectorRetainedBytes != 0 ||
		runtime.clockUncertainty != 0 || runtime.selectorRecoveryJitter != 0 {
		t.Fatalf("explicit zero options = %#v, %v", runtime, err)
	}
}

func TestNormalizeRegistrationConfigReportsExactInvalidField(t *testing.T) {
	t.Parallel()
	integer := func(value int) *int { return &value }
	duration := func(value time.Duration) *time.Duration { return &value }
	tests := []struct {
		name    string
		field   string
		timeout time.Duration
		mutate  func(*Config)
	}{
		{name: "zone", field: "zone", mutate: func(config *Config) { config.Zone = "a:bad" }},
		{name: "root timeout", field: "timeout", timeout: -time.Second},
		{name: "registration buffer", field: "registration.buffer_capacity", mutate: func(config *Config) {
			config.BufferCapacity = 257
		}},
		{name: "registration error buffer", field: "registration.error_buffer_capacity", mutate: func(config *Config) {
			config.ErrorBufferCapacity = 1025
		}},
		{name: "minimum renew interval", field: "registration.min_renew_interval", mutate: func(config *Config) {
			config.MinimumRenewInterval = 9 * time.Millisecond
		}},
		{name: "renew jitter", field: "registration.renew_jitter_percent", mutate: func(config *Config) {
			config.RenewJitterPercent = integer(51)
		}},
		{name: "policy refresh jitter", field: "registration.policy_refresh_jitter_percent", mutate: func(config *Config) {
			config.PolicyRefreshJitterPercent = integer(-1)
		}},
		{name: "policy", field: "registration.policy", mutate: func(config *Config) {
			config.Policy.AttrMaxFields = 129
		}},
		{name: "selector scan page", field: "selector.scan_page_size", mutate: func(config *Config) {
			config.SelectorPageSize = 1025
		}},
		{name: "selector pending entries", field: "selector.max_pending_entries", mutate: func(config *Config) {
			config.SelectorEventBuffer = 65_537
		}},
		{name: "selector pending bytes", field: "selector.max_pending_bytes", mutate: func(config *Config) {
			config.SelectorEventBytes = 1024*1024*1024 + 1
		}},
		{name: "selector publish interval", field: "selector.view_publish_interval", mutate: func(config *Config) {
			config.SelectorPublishInterval = duration(time.Second + time.Millisecond)
		}},
		{name: "selector publish precision", field: "selector.view_publish_interval", mutate: func(config *Config) {
			config.SelectorPublishInterval = duration(time.Nanosecond)
		}},
		{name: "selector sync timeout", field: "selector.sync_timeout", mutate: func(config *Config) {
			config.SelectorSyncTimeout = 99 * time.Millisecond
		}},
		{name: "selector active bytes", field: "selector.max_active_bytes", mutate: func(config *Config) {
			config.SelectorMaxBytes = -1
		}},
		{name: "selector retained bytes", field: "selector.max_retained_bytes", mutate: func(config *Config) {
			config.SelectorRetainedBytes = integer(-1)
		}},
		{name: "clock refresh", field: "selector.clock_refresh_interval", mutate: func(config *Config) {
			config.ClockRefresh = time.Second - time.Millisecond
		}},
		{name: "clock uncertainty", field: "selector.clock_uncertainty", mutate: func(config *Config) {
			config.ClockUncertainty = duration(time.Second + time.Millisecond)
		}},
		{name: "selector error buffer", field: "selector.error_buffer_capacity", mutate: func(config *Config) {
			config.SelectorErrorBufferCapacity = 1025
		}},
		{name: "selector recovery initial", field: "selector.recovery.initial_delay", mutate: func(config *Config) {
			config.SelectorRecoveryInitialDelay = 9 * time.Millisecond
		}},
		{name: "selector recovery maximum", field: "selector.recovery.max_delay", mutate: func(config *Config) {
			config.SelectorRecoveryMaxDelay = 99 * time.Millisecond
		}},
		{name: "selector recovery multiplier", field: "selector.recovery.multiplier", mutate: func(config *Config) {
			config.SelectorRecoveryMultiplier = 9
		}},
		{name: "selector recovery jitter", field: "selector.recovery.jitter_percent", mutate: func(config *Config) {
			config.SelectorRecoveryJitterPercent = integer(51)
		}},
		{name: "selector recovery relation", field: "selector.recovery.initial_delay", mutate: func(config *Config) {
			config.SelectorRecoveryInitialDelay = 2 * time.Second
			config.SelectorRecoveryMaxDelay = time.Second
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
	if _, err := Open(context.Background(), nil, Config{Zone: "Alpha"}); !IsCode(err, CodeClosed) {
		t.Fatalf("Open with nil transport = %v, want closed", err)
	}
}

func TestParseZoneConfigAndRecordLimits(t *testing.T) {
	t.Parallel()
	defaults := defaultZoneConfig()
	parsed, err := parseZoneConfig(defaults.values())
	if err != nil || parsed != defaults {
		t.Fatalf("parseZoneConfig() = %#v, %v", parsed, err)
	}
	attr := make(Fields, 16)
	data := make(Fields, 32)
	for index := range 16 {
		attr[string(rune('a'+index))] = bytes.Repeat([]byte{'a'}, 128)
	}
	for index := range 32 {
		data["d"+string(rune('A'+index))] = bytes.Repeat([]byte{'d'}, 128)
	}
	if err := validateRecord("0123456789abcdef0123456789abcdef", 1, 15_000, 1, attr, data, defaults); err != nil {
		t.Fatal(err)
	}
}
