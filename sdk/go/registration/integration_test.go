//go:build integration

package registration

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
	"github.com/vmihailenco/msgpack/v5"
)

func TestRegistrationAndSelectorIntegration(t *testing.T) {
	redisURL := os.Getenv("VERDANDI_REDIS_URL")
	if redisURL == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	t.Cleanup(func() { _ = raw.Close() })
	zone := integrationZone(t)
	transportConfig := verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
	}
	config := Config{Zone: zone, SelectorPublishInterval: new(time.Millisecond), SelectorMaxBytes: 2048, ClockRefresh: time.Second}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	client, err := openTestRegistrationClient(t, ctx, transportConfig, config)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer closeCancel()
		_ = client.Close(closeCtx)
		cleanupZone(t, raw, zone)
	})

	assertDefaultConfiguration(t, ctx, raw, zone)
	selector, err := selectRaw(ctx, client, SelectorConfig{Type: "proxy"})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer closeCancel()
		_ = selector.Close(closeCtx)
	})
	waitForIntegrationSnapshot(t, selector, func(snapshot Snapshot) bool {
		return snapshot.Synchronized && len(snapshot.Records) == 0
	})

	registration, err := registerRaw(ctx, client, RegistrationConfig{
		Type:          "proxy",
		TTL:           3 * time.Second,
		RenewInterval: 500 * time.Millisecond,
		Version:       1,
		Attr: Fields{
			"build":  []byte("2026.08.23"),
			"region": []byte("cn-east"),
		},
		Data: Fields{
			"address": []byte("10.0.0.1:8080"),
			"load":    []byte("0"),
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(registration.UUID()) != 32 || registration.Revision() != 1 {
		t.Fatalf("unexpected Registration identity/revision: %q/%d", registration.UUID(), registration.Revision())
	}
	record := waitForRecord(t, selector, registration.UUID(), func(record Record) bool {
		return record.Meta.Revision == 1 && string(record.Data["load"]) == "0"
	})
	if string(record.Attr["region"]) != "cn-east" {
		t.Fatalf("Attr region = %q", record.Attr["region"])
	}

	if err := registration.Update(ctx, Update{Data: Fields{"load": []byte("1")}}); err != nil {
		t.Fatal(err)
	}
	waitForRecord(t, selector, registration.UUID(), func(record Record) bool {
		return record.Meta.Revision == 2 && string(record.Data["load"]) == "1"
	})
	if err := registration.Update(ctx, Update{Data: Fields{"load": []byte("1")}}); err != nil {
		t.Fatalf("no-op Update error = %v", err)
	}
	if revision := registration.Revision(); revision != 2 {
		t.Fatalf("no-op Update revision = %d, want 2", revision)
	}

	if err := raw.HSet(ctx, configKey(zone), "registration_data_max_field_value_bytes", "2").Err(); err != nil {
		t.Fatal(err)
	}
	if err := client.RefreshConfiguration(ctx); err != nil {
		t.Fatal(err)
	}
	if err := registration.Update(ctx, Update{Data: Fields{"load": []byte("123")}}); !IsCode(err, CodeCapacity) {
		t.Fatalf("Update above live configuration error = %v", err)
	}
	if revision := registration.Revision(); revision != 2 {
		t.Fatalf("rejected Update revision = %d, want 2", revision)
	}
	legacySelector, err := selectRaw(ctx, client, SelectorConfig{Type: "proxy"})
	if err != nil {
		t.Fatalf("Selector could not load a record accepted before a lower limit: %v", err)
	}
	waitForRecord(t, legacySelector, registration.UUID(), func(record Record) bool {
		return record.Meta.Revision == 2 && string(record.Data["address"]) == "10.0.0.1:8080"
	})
	legacyCloseCtx, legacyCloseCancel := context.WithTimeout(context.Background(), 5*time.Second)
	if err := legacySelector.Close(legacyCloseCtx); err != nil {
		legacyCloseCancel()
		t.Fatal(err)
	}
	legacyCloseCancel()
	if err := raw.HSet(ctx, configKey(zone), "registration_data_max_field_value_bytes", "128").Err(); err != nil {
		t.Fatal(err)
	}
	if err := client.RefreshConfiguration(ctx); err != nil {
		t.Fatal(err)
	}

	if err := raw.ScriptFlush(ctx).Err(); err != nil {
		t.Fatal(err)
	}
	if err := registration.Update(ctx, Update{Data: Fields{"load": []byte("2")}}); err != nil {
		t.Fatalf("Update after SCRIPT FLUSH error = %v", err)
	}
	waitForRecord(t, selector, registration.UUID(), func(record Record) bool {
		return record.Meta.Revision == 3 && string(record.Data["load"]) == "2"
	})

	beforeRenew := registration.Revision()
	if err := registration.Renew(ctx); err != nil {
		t.Fatal(err)
	}
	if registration.Revision() != beforeRenew {
		t.Fatalf("Renew changed revision from %d to %d", beforeRenew, registration.Revision())
	}

	testConcurrentRegistrationUpdates(t, ctx, registration, selector)
	testSelectorGapRepair(t, ctx, raw, client, registration, selector)
	testSelectorMissingRepairCapacity(t, ctx, raw, client, selector)
	testSelectorNaturalExpiry(t, ctx, client, selector)

	closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer closeCancel()
	if err := registration.Close(closeCtx); err != nil {
		t.Fatal(err)
	}
	if err := registration.Close(closeCtx); err != nil {
		t.Fatalf("repeated Registration Close error = %v", err)
	}
	waitForAbsent(t, selector, registration.UUID())
	waitForIntegrationRetained(t, selector, registration.UUID(), func(_ RetainedRecord, found bool) bool {
		return !found
	})
	if raw.Exists(ctx, registrationKey(zone, "proxy", registration.UUID())).Val() != 0 {
		t.Fatal("Registration key remains after graceful Close")
	}
	if err := selector.Close(closeCtx); err != nil {
		t.Fatalf("Selector Close error = %v", err)
	}
	if err := selector.Close(closeCtx); err != nil {
		t.Fatalf("repeated Selector Close error = %v", err)
	}
	if err := client.Close(closeCtx); err != nil {
		t.Fatalf("Client Close error = %v", err)
	}
	if err := client.Close(closeCtx); err != nil {
		t.Fatalf("repeated Client Close error = %v", err)
	}
}

func TestRegistrationCoalescesFieldsMailboxIntegration(t *testing.T) {
	ctx, raw, client, zone := openRegistrationIntegration(t, 20*time.Second)
	registration, err := registerRaw(ctx, client, RegistrationConfig{
		Type:          "coalesce",
		TTL:           6 * time.Hour,
		RenewInterval: 2 * time.Hour,
		Version:       1,
		Data:          Fields{"load": []byte("initial")},
	})
	if err != nil {
		t.Fatal(err)
	}

	hook := &registrationCommandBlockHook{
		entered: make(chan struct{}),
		release: make(chan struct{}),
	}
	requireRegistrationRuntime(t, client).redis.AddHook(hook)
	defer hook.unblock()
	const updates = 8
	results := make(chan error, updates)
	go func() {
		results <- registration.Update(ctx, Update{Data: Fields{"load": []byte("load-00")}})
	}()
	select {
	case <-hook.entered:
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	for index := 1; index < updates; index++ {
		value := []byte(fmt.Sprintf("load-%02d", index))
		go func() {
			results <- registration.Update(ctx, Update{Data: Fields{"load": value}})
		}()
		waitRegistrationPendingUpdates(t, registration, index)
	}
	hook.unblock()
	for range updates {
		if err := <-results; err != nil {
			t.Fatalf("queued Update error = %v", err)
		}
	}
	if revision := registration.Revision(); revision != 3 {
		t.Fatalf("coalesced revision = %d, want 3", revision)
	}
	stored, err := raw.HGet(ctx, registrationKey(zone, "coalesce", registration.UUID()), "load").Bytes()
	if err != nil {
		t.Fatal(err)
	}
	want := fmt.Sprintf("load-%02d", updates-1)
	if string(stored) != want {
		t.Fatalf("coalesced final Data = %q, want %q", stored, want)
	}
}

func waitRegistrationPendingUpdates(t *testing.T, registration *RawRegistration, count int) {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		registration.bufferMu.Lock()
		pending := len(registration.pendingUpdates)
		registration.bufferMu.Unlock()
		if pending == count {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("pending updates did not reach %d", count)
}

func TestRegistrationUpdateResetsAutomaticRenewIntegration(t *testing.T) {
	ctx, _, client, _ := openRegistrationIntegration(t, 15*time.Second)
	registration, err := registerRaw(ctx, client, RegistrationConfig{
		Type:          "renewreset",
		TTL:           9 * time.Second,
		RenewInterval: 3 * time.Second,
		Version:       1,
		Data:          Fields{"load": []byte("0")},
	})
	if err != nil {
		t.Fatal(err)
	}

	time.Sleep(1800 * time.Millisecond)
	if err := registration.Update(ctx, Update{Data: Fields{"load": []byte("1")}}); err != nil {
		t.Fatal(err)
	}
	updatedTimestamp := registration.Timestamp()
	time.Sleep(2 * time.Second)
	if timestamp := registration.Timestamp(); timestamp != updatedTimestamp {
		t.Fatalf("automatic Renew used the pre-Update schedule: update=%d renewed=%d", updatedTimestamp, timestamp)
	}
	deadline := time.Now().Add(4 * time.Second)
	for time.Now().Before(deadline) {
		if registration.Timestamp() > updatedTimestamp {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("automatic Renew did not resume from the Update-reset schedule")
}

func TestRegistrationCorruptSuccessReplyRecoversCompleteDesiredState(t *testing.T) {
	ctx, raw, client, zone := openRegistrationIntegration(t, 15*time.Second)
	registration, err := registerRaw(ctx, client, RegistrationConfig{
		Type:          "replyrecovery",
		TTL:           6 * time.Hour,
		RenewInterval: 2 * time.Hour,
		Version:       1,
		Data:          Fields{"load": []byte("0")},
	})
	if err != nil {
		t.Fatal(err)
	}

	originalUpdate := protocolScripts.update
	protocolScripts.update = redis.NewScript(`return {"&result", "ok", "@revision", 99, "@timestamp", 1}`)
	err = registration.Update(ctx, Update{Data: Fields{"load": []byte("1")}})
	protocolScripts.update = originalUpdate
	if !IsCode(err, CodeCorrupt) {
		t.Fatalf("Update with corrupt success reply error = %v", err)
	}
	if revision := registration.Revision(); revision != 2 {
		t.Fatalf("desired revision after corrupt Update reply = %d, want 2", revision)
	}
	if stored := raw.HGet(ctx, registrationKey(zone, "replyrecovery", registration.UUID()), "@revision").Val(); stored != "1" {
		t.Fatalf("fake Update unexpectedly changed stored revision to %q", stored)
	}
	if err := registration.Renew(ctx); err != nil {
		t.Fatalf("full-state recovery after corrupt Update reply = %v", err)
	}
	values, err := raw.HMGet(
		ctx,
		registrationKey(zone, "replyrecovery", registration.UUID()),
		"@revision",
		"load",
	).Result()
	if err != nil {
		t.Fatal(err)
	}
	if len(values) != 2 || values[0] != "2" || values[1] != "1" {
		t.Fatalf("recovered Registration state = %#v", values)
	}

	originalRenew := protocolScripts.renew
	protocolScripts.renew = redis.NewScript(`return {"&result", "ok", "@revision", 2, "@timestamp", 0}`)
	err = registration.Renew(ctx)
	protocolScripts.renew = originalRenew
	if !IsCode(err, CodeCorrupt) {
		t.Fatalf("Renew with corrupt success reply error = %v", err)
	}
	if err := registration.Renew(ctx); err != nil {
		t.Fatalf("full-state recovery after corrupt Renew reply = %v", err)
	}
}

func TestTypedRegistrationAndTransactionalSelectorIntegration(t *testing.T) {
	redisURL := os.Getenv("VERDANDI_REDIS_URL")
	if redisURL == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	t.Cleanup(func() { _ = raw.Close() })
	zone := integrationZone(t)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	client, err := openTestRegistrationClient(t, ctx, verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
	}, Config{Zone: zone, SelectorPublishInterval: new(time.Millisecond), SelectorMaxBytes: 1 << 20, ClockRefresh: time.Second})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer closeCancel()
		_ = client.Close(closeCtx)
		cleanupZone(t, raw, zone)
	})

	registration, err := client.Registration[apiAttr, apiData](RegistrationOptions{
		Type:          "proxy",
		TTL:           3 * time.Second,
		RenewInterval: 500 * time.Millisecond,
		Version:       1,
	})
	if err != nil {
		t.Fatal(err)
	}
	if registration.Registered() || registration.Revision() != 0 || len(registration.UUID()) != 32 {
		t.Fatalf("unexpected local Registration state: registered=%v revision=%d uuid=%q",
			registration.Registered(), registration.Revision(), registration.UUID())
	}
	selector, err := client.Selector[apiAttr, apiData](ctx, SelectorOptions{Type: "proxy"})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer closeCancel()
		_ = selector.Close(closeCtx)
	})
	if err := registration.Register(ctx, apiAttr{Region: []byte("east")}, apiData{Power: 1}); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer closeCancel()
		_ = registration.Unregister(closeCtx)
	})

	deadline := time.Now().Add(2 * time.Second)
	for {
		candidate, exists, findErr := selector.Find(ctx, registration.UUID())
		if findErr != nil && !IsCode(findErr, CodeUnavailable) {
			t.Fatal(findErr)
		}
		if findErr == nil && exists && candidate.Meta.Revision == 1 && candidate.Data.Power == 1 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("typed Selector did not observe Registration")
		}
		time.Sleep(10 * time.Millisecond)
	}

	selected, exists, err := selector.One(ctx, func(candidates Candidates[apiAttr, apiData]) (Candidate[apiAttr, apiData], bool, error) {
		if len(candidates) != 1 {
			return Candidate[apiAttr, apiData]{}, false, fmt.Errorf("candidate count %d", len(candidates))
		}
		if mutateErr := candidates.Mutate(0, func(data *apiData) error {
			data.Power++
			return nil
		}); mutateErr != nil {
			return Candidate[apiAttr, apiData]{}, false, mutateErr
		}
		return candidates[0], true, nil
	})
	if err != nil || !exists || selected.Data.Power != 2 {
		t.Fatalf("typed One = %#v, %v, %v", selected, exists, err)
	}

	if err := registration.UpdateContent(ctx, 2, apiData{Power: 8}); err != nil {
		t.Fatal(err)
	}
	deadline = time.Now().Add(2 * time.Second)
	for {
		candidate, exists, findErr := selector.Find(ctx, registration.UUID())
		if findErr != nil && !IsCode(findErr, CodeUnavailable) {
			t.Fatal(findErr)
		}
		if findErr == nil && exists && candidate.Meta.Revision == 2 && candidate.Meta.Version == 2 && candidate.Data.Power == 8 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("typed Selector did not reconcile remote Update")
		}
		time.Sleep(10 * time.Millisecond)
	}

	if err := registration.Unregister(ctx); err != nil {
		t.Fatal(err)
	}
}

func testSelectorMissingRepairCapacity(t *testing.T, ctx context.Context, raw *redis.Client, client *Client, selector *RawSelector) {
	t.Helper()
	runtime := requireRegistrationRuntime(t, client)
	beforeSnapshot := waitForIntegrationSnapshot(t, selector, func(snapshot Snapshot) bool {
		return snapshot.Synchronized
	})
	before := beforeSnapshot.Generation
	registry := registryKey(runtime.config.Zone, "proxy")
	for index := range 20 {
		uuid := fmt.Sprintf("%032x", 0x1000+index)
		if _, err := callRawRegistration(ctx, client, registrationScriptRegister, "proxy", uuid, registerArguments(
			uuid, 1, 10_000, 1, nil, Fields{"load": []byte("temporary")},
		)); err != nil {
			t.Fatal(err)
		}
		waitForRecord(t, selector, uuid, func(record Record) bool { return record.Meta.Revision == 1 })
		if err := raw.Del(ctx, registrationKey(runtime.config.Zone, "proxy", uuid)).Err(); err != nil {
			t.Fatal(err)
		}
		payload, err := msgpack.Marshal([]any{
			"&protocol", "v1", "&kind", "update", "@uuid", uuid,
			"@revision", int64(3), "@timestamp", time.Now().UnixMilli(),
			"load", []byte("gap"),
		})
		if err != nil {
			t.Fatal(err)
		}
		if err := raw.Publish(ctx, registry, payload).Err(); err != nil {
			t.Fatal(err)
		}
		waitForAbsent(t, selector, uuid)
		if err := raw.HDel(ctx, registry, uuid).Err(); err != nil {
			t.Fatal(err)
		}
	}
	waitForIntegrationSnapshot(t, selector, func(snapshot Snapshot) bool {
		return snapshot.Generation == before && snapshot.Synchronized
	})
}

func TestZoneConfigurationRefreshIntegration(t *testing.T) {
	redisURL := os.Getenv("VERDANDI_REDIS_URL")
	if redisURL == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	t.Cleanup(func() { _ = raw.Close() })
	zone := integrationZone(t)
	transportConfig := verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
	}
	config := Config{Zone: zone, ClockRefresh: time.Second}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := raw.HSet(ctx, configKey(zone), map[string]any{
		"registration_attr_max_fields": "8",
		"registration_data_max_fields": "24",
		"configuration_refresh_ms":     "1000",
	}).Err(); err != nil {
		t.Fatal(err)
	}
	client, err := openTestRegistrationClient(t, ctx, transportConfig, config)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer closeCancel()
		_ = client.Close(closeCtx)
		cleanupZone(t, raw, zone)
	})
	if limits := client.RegistrationLimits(); limits.AttrMaxFields != 8 || limits.DataMaxFields != 24 ||
		limits.AttrValueMaxBytes != 128 || limits.DataValueMaxBytes != 128 ||
		limits.ConfigurationRefresh != time.Second {
		t.Fatalf("bootstrap did not preserve backend values and fill missing defaults: %#v", limits)
	}

	// A Selector owns RedisClock calibration but does not keep Registration
	// deployment policy fresh.
	if err := raw.HSet(ctx, configKey(zone), map[string]any{
		"registration_attr_max_fields": "9",
		"registration_data_max_fields": "25",
		"configuration_refresh_ms":     "1000",
	}).Err(); err != nil {
		t.Fatal(err)
	}
	selector, err := selectRaw(ctx, client, SelectorConfig{Type: "configwatch"})
	if err != nil {
		t.Fatal(err)
	}
	beforeTime := integrationCommandCalls(t, ctx, raw, "cmdstat_time")
	time.Sleep(1200 * time.Millisecond)
	afterTime := integrationCommandCalls(t, ctx, raw, "cmdstat_time")
	if afterTime <= beforeTime {
		t.Fatalf("Selector RedisClock did not recalibrate: TIME calls %d -> %d", beforeTime, afterTime)
	}
	if limits := client.RegistrationLimits(); limits.AttrMaxFields != 8 || limits.DataMaxFields != 24 {
		t.Fatalf("Selector unexpectedly refreshed Registration policy: %#v", limits)
	}
	if err := selector.Close(ctx); err != nil {
		t.Fatal(err)
	}

	registration, err := registerRaw(ctx, client, RegistrationConfig{
		Type: "configwatch", TTL: 3 * time.Second, RenewInterval: time.Second,
		Version: 1, Data: Fields{"load": []byte("0")},
	})
	if err != nil {
		t.Fatal(err)
	}
	if limits := client.RegistrationLimits(); limits.AttrMaxFields != 9 || limits.DataMaxFields != 25 ||
		limits.ConfigurationRefresh != time.Second {
		t.Fatalf("Registration did not load current policy before publication: %#v", limits)
	}

	// A malformed or above-ceiling refresh is reported and cannot replace the
	// last complete valid local snapshot.
	if err := raw.HSet(ctx, configKey(zone), "registration_attr_max_fields", "129").Err(); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-client.Errors():
		if !IsCode(err, CodeCapacity) {
			t.Fatalf("configuration refresh error = %v, want capacity", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for invalid configuration diagnostic")
	}
	if limits := client.RegistrationLimits(); limits.AttrMaxFields != 9 || limits.DataMaxFields != 25 {
		t.Fatalf("invalid refresh replaced last valid limits: %#v", limits)
	}

	defaults := defaultZoneConfig()
	if err := raw.HSet(ctx, configKey(zone), map[string]any{
		"registration_attr_max_fields": defaults.attrMaxFields,
		"registration_data_max_fields": defaults.dataMaxFields,
	}).Err(); err != nil {
		t.Fatal(err)
	}
	waitForLimits(t, client, func(limits RegistrationLimits) bool {
		return limits.AttrMaxFields == defaults.attrMaxFields && limits.DataMaxFields == defaults.dataMaxFields
	})
	if err := registration.Close(ctx); err != nil {
		t.Fatal(err)
	}

	// The last Registration stops automatic refresh. Explicit refresh remains
	// available while no Registration is published.
	if err := raw.HSet(ctx, configKey(zone), map[string]any{
		"registration_attr_max_fields": "7",
		"registration_data_max_fields": "23",
	}).Err(); err != nil {
		t.Fatal(err)
	}
	time.Sleep(1200 * time.Millisecond)
	if limits := client.RegistrationLimits(); limits.AttrMaxFields != defaults.attrMaxFields ||
		limits.DataMaxFields != defaults.dataMaxFields {
		t.Fatalf("configuration refreshed without a Registration: %#v", limits)
	}
	if err := client.RefreshConfiguration(ctx); err != nil {
		t.Fatal(err)
	}
	if limits := client.RegistrationLimits(); limits.AttrMaxFields != 7 || limits.DataMaxFields != 23 {
		t.Fatalf("explicit configuration refresh did not publish current policy: %#v", limits)
	}
}

func integrationCommandCalls(t *testing.T, ctx context.Context, raw *redis.Client, command string) int64 {
	t.Helper()
	info, err := raw.Info(ctx, "commandstats").Result()
	if err != nil {
		t.Fatal(err)
	}
	prefix := command + ":calls="
	for _, line := range strings.Split(info, "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, prefix) {
			continue
		}
		calls, parseErr := strconv.ParseInt(strings.SplitN(strings.TrimPrefix(line, prefix), ",", 2)[0], 10, 64)
		if parseErr != nil {
			t.Fatalf("malformed Redis commandstat %q: %v", line, parseErr)
		}
		return calls
	}
	t.Fatalf("Redis INFO commandstats omitted %s", command)
	return 0
}

func TestProtocolCeilingRegistrationRecoveryIntegration(t *testing.T) {
	redisURL := os.Getenv("VERDANDI_REDIS_URL")
	if redisURL == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	t.Cleanup(func() { _ = raw.Close() })
	zone := integrationZone(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	configured := protocolZoneConfig()
	configured.attrValueMaxBytes = 256
	configured.dataValueMaxBytes = 256
	values := configured.values()
	fields := make(map[string]any, len(zoneConfigFields))
	for index, name := range zoneConfigFields {
		fields[name] = values[index]
	}
	if err := raw.HSet(ctx, configKey(zone), fields).Err(); err != nil {
		t.Fatal(err)
	}
	client, err := openTestRegistrationClient(t, ctx, verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
	}, Config{Zone: zone, SelectorPageSize: 32, SelectorPublishInterval: new(time.Millisecond)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer closeCancel()
		_ = client.Close(closeCtx)
		cleanupZone(t, raw, zone)
	})
	selector, err := selectRaw(ctx, client, SelectorConfig{Type: "maximum"})
	if err != nil {
		t.Fatal(err)
	}
	attr := make(Fields, protocolAttrFields)
	data := make(Fields, protocolDataFields)
	for index := range protocolAttrFields {
		attr[fmt.Sprintf("a%03d", index)] = bytes.Repeat([]byte{'a'}, 240)
	}
	for index := range protocolDataFields {
		data[fmt.Sprintf("d%03d", index)] = bytes.Repeat([]byte{'d'}, 240)
	}
	recordBytes := registrationSize("00000000000000000000000000000000", 1, 30_000, 1, attr, data)
	if recordBytes > protocolRecordBytes || recordBytes < 60*1024 {
		t.Fatalf("maximum fixture record bytes = %d, want 60 KiB..64 KiB", recordBytes)
	}
	registration, err := registerRaw(ctx, client, RegistrationConfig{
		Type:          "maximum",
		TTL:           30 * time.Second,
		RenewInterval: 10 * time.Second,
		Version:       1,
		Attr:          attr,
		Data:          data,
	})
	if err != nil {
		t.Fatal(err)
	}
	waitForRecord(t, selector, registration.UUID(), func(record Record) bool {
		return record.Meta.Revision == 1 && len(record.Attr) == protocolAttrFields && len(record.Data) == protocolDataFields
	})
	if err := raw.Del(ctx, registrationKey(zone, "maximum", registration.UUID())).Err(); err != nil {
		t.Fatal(err)
	}
	changed := bytes.Repeat([]byte{'x'}, 240)
	if err := registration.Update(ctx, Update{Data: Fields{"d000": changed}}); err != nil {
		t.Fatal(err)
	}
	waitForRecord(t, selector, registration.UUID(), func(record Record) bool {
		return record.Meta.Revision == 2 && bytes.Equal(record.Data["d000"], changed) && len(record.Data) == protocolDataFields
	})
	t.Logf("protocol-ceiling complete Register recovery: stored_bytes=%d fields=%d", recordBytes, len(attr)+len(data))
	closeCtx, closeCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer closeCancel()
	if err := registration.Close(closeCtx); err != nil {
		t.Fatal(err)
	}
	if err := selector.Close(closeCtx); err != nil {
		t.Fatal(err)
	}
}

func testConcurrentRegistrationUpdates(t *testing.T, ctx context.Context, registration *RawRegistration, selector *RawSelector) {
	t.Helper()
	const updates = 24
	startRevision := registration.Revision()
	var wait sync.WaitGroup
	errors := make(chan error, updates)
	for index := range updates {
		wait.Add(1)
		go func() {
			defer wait.Done()
			errors <- registration.Update(ctx, Update{Data: Fields{"load": []byte(fmt.Sprintf("load-%02d", index))}})
		}()
	}
	wait.Wait()
	close(errors)
	for err := range errors {
		if err != nil {
			t.Fatalf("concurrent Update error = %v", err)
		}
	}
	if revision := registration.Revision(); revision <= startRevision || revision > startRevision+updates {
		t.Fatalf("coalesced concurrent Update revision = %d, want (%d, %d]", revision, startRevision, startRevision+updates)
	}
	waitForRecord(t, selector, registration.UUID(), func(record Record) bool {
		return record.Meta.Revision == registration.Revision()
	})
}

func testSelectorGapRepair(t *testing.T, ctx context.Context, raw *redis.Client, client *Client, registration *RawRegistration, selector *RawSelector) {
	t.Helper()
	runtime := requireRegistrationRuntime(t, client)
	beforeSnapshot := waitForIntegrationSnapshot(t, selector, func(snapshot Snapshot) bool {
		return snapshot.Synchronized
	})
	before := beforeSnapshot.Generation
	payload, err := msgpack.Marshal([]any{
		"&protocol", "v1",
		"&kind", "update",
		"@uuid", registration.UUID(),
		"@revision", int64(registration.Revision() + 2),
		"@timestamp", time.Now().UnixMilli(),
		"load", []byte("gap"),
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := raw.Publish(ctx, registryKey(runtime.config.Zone, "proxy"), payload).Err(); err != nil {
		t.Fatal(err)
	}
	waitForRecord(t, selector, registration.UUID(), func(record Record) bool {
		return record.Meta.Revision == registration.Revision() && string(record.Data["load"]) != "gap"
	})
	waitForIntegrationSnapshot(t, selector, func(snapshot Snapshot) bool {
		return snapshot.Synchronized && snapshot.Generation == before
	})
}

func testSelectorNaturalExpiry(t *testing.T, ctx context.Context, client *Client, selector *RawSelector) {
	t.Helper()
	uuid := "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	limits := requireRegistrationRuntime(t, client).limits()
	if err := validateRecord(uuid, 1, 300, 1, nil, Fields{"load": []byte("ephemeral")}, limits); err != nil {
		t.Fatal(err)
	}
	if _, err := callRawRegistration(ctx, client, registrationScriptRegister, "proxy", uuid,
		registerArguments(uuid, 1, 300, 1, nil, Fields{"load": []byte("ephemeral")})); err != nil {
		t.Fatal(err)
	}
	waitForRecord(t, selector, uuid, func(record Record) bool { return string(record.Data["load"]) == "ephemeral" })
	waitForAbsent(t, selector, uuid)
	waitForIntegrationRetained(t, selector, uuid, func(retained RetainedRecord, found bool) bool {
		return found && string(retained.Record.Data["load"]) == "ephemeral"
	})
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if _, found, err := selector.FindRetained(uuid); err == nil && !found {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	snapshot, _ := selector.Snapshot()
	t.Fatalf("retained record outlived its second TTL: %#v", snapshot)
}

func waitForRecord(t *testing.T, selector *RawSelector, uuid string, predicate func(Record) bool) Record {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if record, ok, err := selector.Find(uuid); err == nil && ok && predicate(record) {
			return record
		} else if err != nil && !IsCode(err, CodeUnavailable) {
			t.Fatal(err)
		}
		time.Sleep(5 * time.Millisecond)
	}
	snapshot, _ := selector.Snapshot()
	t.Fatalf("timed out waiting for Registration %s; snapshot=%#v", uuid, snapshot)
	return Record{}
}

func waitForIntegrationSnapshot(t *testing.T, selector *RawSelector, predicate func(Snapshot) bool) Snapshot {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		snapshot, err := selector.Snapshot()
		if err == nil && predicate(snapshot) {
			return snapshot
		}
		if err != nil && !IsCode(err, CodeUnavailable) {
			t.Fatal(err)
		}
		time.Sleep(5 * time.Millisecond)
	}
	snapshot, _ := selector.Snapshot()
	t.Fatalf("timed out waiting for synchronized Selector snapshot: %#v", snapshot)
	return Snapshot{}
}

func waitForIntegrationRetained(t *testing.T, selector *RawSelector, uuid string, predicate func(RetainedRecord, bool) bool) RetainedRecord {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		retained, found, err := selector.FindRetained(uuid)
		if err == nil && predicate(retained, found) {
			return retained
		}
		if err != nil && !IsCode(err, CodeUnavailable) {
			t.Fatal(err)
		}
		time.Sleep(5 * time.Millisecond)
	}
	snapshot, _ := selector.Snapshot()
	t.Fatalf("timed out waiting for retained Selector state for %s: %#v", uuid, snapshot)
	return RetainedRecord{}
}

func waitForAbsent(t *testing.T, selector *RawSelector, uuid string) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if _, ok, err := selector.Find(uuid); err == nil && !ok {
			return
		} else if err != nil && !IsCode(err, CodeUnavailable) {
			t.Fatal(err)
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for absent Registration %s", uuid)
}

func waitForLimits(t *testing.T, client *Client, predicate func(RegistrationLimits) bool) RegistrationLimits {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		limits := client.RegistrationLimits()
		if predicate(limits) {
			return limits
		}
		time.Sleep(5 * time.Millisecond)
	}
	limits := client.RegistrationLimits()
	t.Fatalf("timed out waiting for Zone configuration; limits=%#v", limits)
	return RegistrationLimits{}
}

func assertDefaultConfiguration(t *testing.T, ctx context.Context, raw *redis.Client, zone string) {
	t.Helper()
	actual, err := raw.HMGet(ctx, configKey(zone), zoneConfigFields[:]...).Result()
	if err != nil {
		t.Fatal(err)
	}
	expected := defaultZoneConfig().values()
	for index := range expected {
		if fmt.Sprint(actual[index]) != fmt.Sprint(expected[index]) {
			t.Fatalf("configuration %s = %v, want %v", zoneConfigFields[index], actual[index], expected[index])
		}
	}
}

type registrationCommandBlockHook struct {
	once    sync.Once
	entered chan struct{}
	release chan struct{}
}

func (hook *registrationCommandBlockHook) DialHook(next redis.DialHook) redis.DialHook {
	return next
}

func (hook *registrationCommandBlockHook) ProcessHook(next redis.ProcessHook) redis.ProcessHook {
	return func(ctx context.Context, command redis.Cmder) error {
		block := false
		if command.Name() == "evalsha" {
			hook.once.Do(func() {
				block = true
				close(hook.entered)
			})
		}
		if block {
			select {
			case <-hook.release:
			case <-ctx.Done():
				return ctx.Err()
			}
		}
		return next(ctx, command)
	}
}

func (hook *registrationCommandBlockHook) ProcessPipelineHook(next redis.ProcessPipelineHook) redis.ProcessPipelineHook {
	return next
}

func (hook *registrationCommandBlockHook) unblock() {
	select {
	case <-hook.release:
	default:
		close(hook.release)
	}
}

func openRegistrationIntegration(t *testing.T, timeout time.Duration) (context.Context, *redis.Client, *Client, string) {
	t.Helper()
	redisURL := os.Getenv("VERDANDI_REDIS_URL")
	if redisURL == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	zone := integrationZone(t)
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	client, err := openTestRegistrationClient(t, ctx, verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
		Timeout: 5 * time.Second,
	}, Config{Zone: zone, ClockRefresh: time.Second})
	if err != nil {
		cancel()
		_ = raw.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		_ = client.Close(closeCtx)
		closeCancel()
		cleanupZone(t, raw, zone)
		_ = raw.Close()
		cancel()
	})
	return ctx, raw, client, zone
}
