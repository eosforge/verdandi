//go:build integration && load

package registration

import (
	"context"
	"fmt"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

const qualificationRegistrations = 500

func TestRegistrationSelectorQualificationLoad(t *testing.T) {
	redisURL := requireRedisURL(t)
	rounds := qualificationRounds(t)
	fanout := qualificationFanout(t)
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	goroutineBaseline := runtime.NumGoroutine()
	raw := redis.NewClient(options)
	t.Cleanup(func() { _ = raw.Close() })
	zone := integrationZone(t)
	t.Cleanup(func() { cleanupZone(t, raw, zone) })
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(rounds)*time.Second+3*time.Minute)
	defer cancel()
	client, err := openTestRegistrationClient(t, ctx, verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
		Timeout: 5 * time.Second,
	}, Config{
		Zone:                    zone,
		SelectorPageSize:        64,
		SelectorEventBuffer:     8192,
		SelectorPublishInterval: new(time.Millisecond),
		ClockRefresh:            time.Second,
	})
	if err != nil {
		t.Fatal(err)
	}

	registrations := make([]*RawRegistration, qualificationRegistrations)
	registerLatencies := make([]time.Duration, qualificationRegistrations)
	registerStarted := time.Now()
	runBounded(t, qualificationRegistrations, 64, func(index int) error {
		started := time.Now()
		registration, err := registerRaw(ctx, client, RegistrationConfig{
			Type:          "proxy",
			TTL:           6 * time.Hour,
			RenewInterval: 2 * time.Hour,
			Version:       1,
			Attr:          Fields{"role": []byte("worker")},
			Data:          Fields{"load": []byte("0")},
		})
		registerLatencies[index] = time.Since(started)
		registrations[index] = registration
		return err
	})
	registerElapsed := time.Since(registerStarted)

	selectStarted := time.Now()
	selector, err := selectRaw(ctx, client, SelectorConfig{Type: "proxy"})
	if err != nil {
		t.Fatal(err)
	}
	selectorSync := time.Since(selectStarted)
	if snapshot, snapshotErr := selector.Snapshot(); snapshotErr != nil || !snapshot.Synchronized || len(snapshot.Records) != qualificationRegistrations {
		t.Fatalf("initial paginated snapshot synchronized=%v records=%d", snapshot.Synchronized, len(snapshot.Records))
	}
	selectors := make([]*RawSelector, 1, fanout)
	selectors[0] = selector
	for len(selectors) < fanout {
		next, selectErr := selectRaw(ctx, client, SelectorConfig{Type: "proxy"})
		if selectErr != nil {
			t.Fatal(selectErr)
		}
		selectors = append(selectors, next)
	}
	activeGoroutines := runtime.NumGoroutine()
	minimumGoroutines := goroutineBaseline + qualificationRegistrations
	maximumGoroutines := minimumGoroutines + fanout*16 + 64
	if activeGoroutines < minimumGoroutines || activeGoroutines > maximumGoroutines {
		t.Fatalf(
			"unexpected one-goroutine-per-Registration topology: baseline=%d active=%d expected=%d..=%d",
			goroutineBaseline,
			activeGoroutines,
			minimumGoroutines,
			maximumGoroutines,
		)
	}
	if err := raw.ConfigResetStat(ctx).Err(); err != nil {
		t.Fatal(err)
	}

	updateLatencies, scheduleLags, updatesElapsed, err := runCadencedUpdates(ctx, registrations, rounds)
	if err != nil {
		t.Fatal(err)
	}
	for _, current := range selectors {
		waitForSnapshot(t, current, qualificationRegistrations, uint64(rounds+1))
	}
	evalshaCalls, evalshaMicroseconds := redisCommandStat(t, ctx, raw, "cmdstat_evalsha")
	keyMemory := redisKeyMemory(t, ctx, raw, zone, registrations)

	closeStarted := time.Now()
	runBounded(t, qualificationRegistrations, 64, func(index int) error {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer closeCancel()
		return registrations[index].Close(closeCtx)
	})
	closeElapsed := time.Since(closeStarted)
	for _, current := range selectors {
		waitForSnapshot(t, current, 0, 0)
		selectorCloseCtx, selectorCloseCancel := context.WithTimeout(context.Background(), 30*time.Second)
		if err := current.Close(selectorCloseCtx); err != nil {
			selectorCloseCancel()
			t.Fatal(err)
		}
		selectorCloseCancel()
	}
	finalGoroutines := waitGoroutineCeiling(goroutineBaseline+8, 5*time.Second)
	if finalGoroutines > goroutineBaseline+8 {
		t.Fatalf(
			"goroutines did not return near baseline: baseline=%d final=%d",
			goroutineBaseline,
			finalGoroutines,
		)
	}

	t.Logf("register: count=%d elapsed=%s rate=%.1f/s %s", qualificationRegistrations, registerElapsed,
		float64(qualificationRegistrations)/registerElapsed.Seconds(), latencySummary(registerLatencies))
	t.Logf("selector initial HSCAN sync: records=%d page_size=64 first_elapsed=%s subscriber_fanout=%d", qualificationRegistrations, selectorSync, fanout)
	completedRate := float64(len(updateLatencies)) / updatesElapsed.Seconds()
	if completedRate < qualificationRegistrations*0.98 {
		t.Fatalf("sustained update completion rate %.1f/s fell below 98%% of the offered 500/s", completedRate)
	}
	zeroLatencies := 0
	for _, latency := range updateLatencies {
		if latency == 0 {
			zeroLatencies++
		}
	}
	if zeroLatencies != 0 {
		t.Fatalf("sustained update measurements contain %d zero-duration observations; use a higher-resolution client clock", zeroLatencies)
	}
	if percentile(updateLatencies, 99) > time.Second || percentile(scheduleLags, 99) > time.Second {
		t.Fatalf("sustained update p99 exceeded one second: operation=%s schedule_lag=%s", percentile(updateLatencies, 99), percentile(scheduleLags, 99))
	}
	t.Logf(
		"update: count=%d cadence=%d/s duration=%s completed_rate=%.1f/s operation_%s schedule_lag_%s",
		len(updateLatencies),
		qualificationRegistrations,
		updatesElapsed,
		completedRate,
		latencySummary(updateLatencies),
		latencySummary(scheduleLags),
	)
	t.Logf("Redis EVALSHA during updates: calls=%d total=%dus average=%.2fus", evalshaCalls, evalshaMicroseconds,
		float64(evalshaMicroseconds)/float64(evalshaCalls))
	t.Logf("graceful unregister: count=%d elapsed=%s rate=%.1f/s", qualificationRegistrations, closeElapsed,
		float64(qualificationRegistrations)/closeElapsed.Seconds())
	t.Logf("Redis MEMORY USAGE for config, Registry, and 500 Registration keys: %d bytes", keyMemory)
	t.Logf("Go goroutines one-per-Registration: baseline=%d active=%d final=%d", goroutineBaseline, activeGoroutines, finalGoroutines)
}

func waitGoroutineCeiling(ceiling int, timeout time.Duration) int {
	deadline := time.Now().Add(timeout)
	for {
		count := runtime.NumGoroutine()
		if count <= ceiling || time.Now().After(deadline) {
			return count
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestRegistrationSelectorRenewalLoad(t *testing.T) {
	redisURL := requireRedisURL(t)
	seconds := qualificationRounds(t)
	if seconds < 5 {
		seconds = 5
	}
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	t.Cleanup(func() { _ = raw.Close() })
	zone := integrationZone(t)
	t.Cleanup(func() { cleanupZone(t, raw, zone) })
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(seconds)*time.Second+3*time.Minute)
	defer cancel()
	client, err := openTestRegistrationClient(t, ctx, verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
		Timeout: 5 * time.Second,
	}, Config{
		Zone:                    zone,
		SelectorPageSize:        64,
		SelectorEventBuffer:     8192,
		SelectorPublishInterval: new(time.Millisecond),
		ClockRefresh:            time.Second,
	})
	if err != nil {
		t.Fatal(err)
	}

	registrations := make([]*RawRegistration, qualificationRegistrations)
	runBounded(t, qualificationRegistrations, 64, func(index int) error {
		registration, err := registerRaw(ctx, client, RegistrationConfig{
			Type:          "renew",
			TTL:           3 * time.Second,
			RenewInterval: time.Second,
			Version:       1,
			Attr:          Fields{"role": []byte("worker")},
			Data:          Fields{"load": []byte("0")},
		})
		registrations[index] = registration
		return err
	})
	selector, err := selectRaw(ctx, client, SelectorConfig{Type: "renew"})
	if err != nil {
		t.Fatal(err)
	}
	waitForSnapshot(t, selector, qualificationRegistrations, 1)
	if err := raw.ConfigResetStat(ctx).Err(); err != nil {
		t.Fatal(err)
	}
	started := time.Now()
	timer := time.NewTimer(time.Duration(seconds) * time.Second)
	select {
	case <-timer.C:
	case <-ctx.Done():
		stopTimer(timer)
		t.Fatal(ctx.Err())
	}
	elapsed := time.Since(started)
	waitForSnapshot(t, selector, qualificationRegistrations, 1)
	evalshaCalls, evalshaMicroseconds := redisCommandStat(t, ctx, raw, "cmdstat_evalsha")
	rate := float64(evalshaCalls) / elapsed.Seconds()
	if rate < qualificationRegistrations*0.8 || rate > qualificationRegistrations*1.2 {
		t.Fatalf("automatic renewal rate %.1f/s is outside 80%%..120%% of 500/s", rate)
	}
	for _, registration := range registrations {
		select {
		case err := <-registration.Errors():
			if err != nil {
				t.Fatalf("automatic renewal error: %v", err)
			}
		default:
		}
	}
	t.Logf("renew: live=%d duration=%s calls=%d rate=%.1f/s Redis_EVALSHA_average=%.2fus", qualificationRegistrations,
		elapsed, evalshaCalls, rate, float64(evalshaMicroseconds)/float64(evalshaCalls))

	runBounded(t, qualificationRegistrations, 64, func(index int) error {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer closeCancel()
		return registrations[index].Close(closeCtx)
	})
	waitForSnapshot(t, selector, 0, 0)
	closeCtx, closeCancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer closeCancel()
	if err := selector.Close(closeCtx); err != nil {
		t.Fatal(err)
	}
}

func TestRegistrationSelectorScaleRecovery(t *testing.T) {
	redisURL := requireRedisURL(t)
	count := qualificationScale(t)
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	t.Cleanup(func() { _ = raw.Close() })
	zone := integrationZone(t)
	t.Cleanup(func() { cleanupZone(t, raw, zone) })
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Minute)
	defer cancel()
	client, err := openTestRegistrationClient(t, ctx, verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
		Timeout: 5 * time.Second,
	}, Config{
		Zone:                    zone,
		SelectorPageSize:        64,
		SelectorEventBuffer:     65536,
		SelectorPublishInterval: new(time.Millisecond),
		ClockRefresh:            time.Second,
	})
	if err != nil {
		t.Fatal(err)
	}

	registrations := make([]*RawRegistration, count)
	registerStarted := time.Now()
	runBounded(t, count, 64, func(index int) error {
		registration, registerErr := registerRaw(ctx, client, RegistrationConfig{
			Type:          "scale",
			TTL:           6 * time.Hour,
			RenewInterval: 2 * time.Hour,
			Version:       1,
			Attr:          Fields{"role": []byte("worker")},
			Data:          Fields{"load": []byte(strconv.Itoa(index % 10))},
		})
		registrations[index] = registration
		return registerErr
	})
	registerElapsed := time.Since(registerStarted)
	selectStarted := time.Now()
	selector, err := selectRaw(ctx, client, SelectorConfig{Type: "scale"})
	if err != nil {
		t.Fatal(err)
	}
	waitForSnapshot(t, selector, count, 1)
	selectElapsed := time.Since(selectStarted)

	closeStarted := time.Now()
	runBounded(t, count, 64, func(index int) error {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer closeCancel()
		return registrations[index].Close(closeCtx)
	})
	waitForSnapshot(t, selector, 0, 0)
	closeElapsed := time.Since(closeStarted)
	selectorCloseCtx, selectorCloseCancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer selectorCloseCancel()
	if err := selector.Close(selectorCloseCtx); err != nil {
		t.Fatal(err)
	}
	t.Logf(
		"scale recovery: registrations=%d register_elapsed=%s register_rate=%.1f/s HSCAN_page_size=64 sync_elapsed=%s unregister_elapsed=%s unregister_rate=%.1f/s",
		count,
		registerElapsed,
		float64(count)/registerElapsed.Seconds(),
		selectElapsed,
		closeElapsed,
		float64(count)/closeElapsed.Seconds(),
	)
}

func qualificationRounds(t *testing.T) int {
	t.Helper()
	value := strings.TrimSpace(os.Getenv("VERDANDI_LOAD_SECONDS"))
	if value == "" {
		return 10
	}
	seconds, err := strconv.Atoi(value)
	if err != nil || seconds < 1 || seconds > 3600 {
		t.Fatalf("VERDANDI_LOAD_SECONDS = %q, want 1..3600", value)
	}
	return seconds
}

func qualificationFanout(t *testing.T) int {
	t.Helper()
	value := strings.TrimSpace(os.Getenv("VERDANDI_SELECTOR_FANOUT"))
	if value == "" {
		return 1
	}
	fanout, err := strconv.Atoi(value)
	if err != nil || fanout < 1 || fanout > 64 {
		t.Fatalf("VERDANDI_SELECTOR_FANOUT = %q, want 1..64", value)
	}
	return fanout
}

func qualificationScale(t *testing.T) int {
	t.Helper()
	value := strings.TrimSpace(os.Getenv("VERDANDI_SCALE_REGISTRATIONS"))
	if value == "" {
		return 5000
	}
	count, err := strconv.Atoi(value)
	if err != nil || count < 1 || count > 100000 {
		t.Fatalf("VERDANDI_SCALE_REGISTRATIONS = %q, want 1..100000", value)
	}
	return count
}

func runCadencedUpdates(ctx context.Context, registrations []*RawRegistration, rounds int) ([]time.Duration, []time.Duration, time.Duration, error) {
	count := len(registrations) * rounds
	latencies := make([]time.Duration, count)
	lags := make([]time.Duration, count)
	errors := make(chan error, len(registrations))
	start := time.Now().Add(100 * time.Millisecond)
	var wait sync.WaitGroup
	for index, registration := range registrations {
		wait.Add(1)
		go func(index int, registration *RawRegistration) {
			defer wait.Done()
			for round := 0; round < rounds; round++ {
				target := start.Add(time.Duration(round)*time.Second + time.Duration(index)*time.Second/time.Duration(len(registrations)))
				delay := time.Until(target)
				if delay > 0 {
					timer := time.NewTimer(delay)
					select {
					case <-timer.C:
					case <-ctx.Done():
						stopTimer(timer)
						errors <- ctx.Err()
						return
					}
				}
				started := time.Now()
				slot := round*len(registrations) + index
				if started.After(target) {
					lags[slot] = started.Sub(target)
				}
				if err := registration.Update(ctx, Update{Data: Fields{"load": []byte(strconv.Itoa(round + 1))}}); err != nil {
					errors <- err
					return
				}
				latencies[slot] = time.Since(started)
			}
			errors <- nil
		}(index, registration)
	}
	wait.Wait()
	elapsed := time.Since(start)
	for range registrations {
		if err := <-errors; err != nil {
			return nil, nil, elapsed, err
		}
	}
	return latencies, lags, elapsed, nil
}

func requireRedisURL(t *testing.T) string {
	t.Helper()
	value := strings.TrimSpace(os.Getenv("VERDANDI_REDIS_URL"))
	if value == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	return value
}

func runBounded(t *testing.T, count int, concurrency int, operation func(int) error) {
	t.Helper()
	semaphore := make(chan struct{}, concurrency)
	errors := make(chan error, count)
	var wait sync.WaitGroup
	for index := 0; index < count; index++ {
		wait.Add(1)
		go func(index int) {
			defer wait.Done()
			semaphore <- struct{}{}
			err := operation(index)
			<-semaphore
			errors <- err
		}(index)
	}
	wait.Wait()
	close(errors)
	for err := range errors {
		if err != nil {
			t.Fatal(err)
		}
	}
}

func waitForSnapshot(t *testing.T, selector *RawSelector, count int, revision uint64) {
	t.Helper()
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		snapshot, snapshotErr := selector.Snapshot()
		if snapshotErr == nil && snapshot.Synchronized && len(snapshot.Records) == count {
			matches := true
			for _, record := range snapshot.Records {
				if revision != 0 && record.Meta.Revision != revision {
					matches = false
					break
				}
			}
			if matches {
				return
			}
		}
		time.Sleep(5 * time.Millisecond)
	}
	snapshot, _ := selector.Snapshot()
	t.Fatalf("snapshot did not converge: records=%d revision=%d actual=%#v", count, revision, snapshot)
}

func latencySummary(values []time.Duration) string {
	ordered := append([]time.Duration(nil), values...)
	sort.Slice(ordered, func(left int, right int) bool { return ordered[left] < ordered[right] })
	percentile := func(value int) time.Duration {
		index := (len(ordered)*value + 99) / 100
		if index > 0 {
			index--
		}
		return ordered[index]
	}
	return fmt.Sprintf("p50=%s p95=%s p99=%s max=%s", percentile(50), percentile(95), percentile(99), ordered[len(ordered)-1])
}

func percentile(values []time.Duration, value int) time.Duration {
	ordered := append([]time.Duration(nil), values...)
	sort.Slice(ordered, func(left int, right int) bool { return ordered[left] < ordered[right] })
	index := (len(ordered)*value + 99) / 100
	if index > 0 {
		index--
	}
	return ordered[index]
}

func redisKeyMemory(t *testing.T, ctx context.Context, raw *redis.Client, zone string, registrations []*RawRegistration) int64 {
	t.Helper()
	keys := []string{configKey(zone), registryKey(zone, "proxy")}
	for _, registration := range registrations {
		keys = append(keys, registrationKey(zone, "proxy", registration.UUID()))
	}
	var total int64
	for _, key := range keys {
		bytes, err := raw.MemoryUsage(ctx, key).Result()
		if err != nil {
			t.Fatal(err)
		}
		total += bytes
	}
	return total
}

func redisCommandStat(t *testing.T, ctx context.Context, raw *redis.Client, command string) (int64, int64) {
	t.Helper()
	info, err := raw.Info(ctx, "commandstats").Result()
	if err != nil {
		t.Fatal(err)
	}
	for _, line := range strings.Split(info, "\n") {
		line = strings.TrimSpace(line)
		prefix := command + ":calls="
		if !strings.HasPrefix(line, prefix) {
			continue
		}
		values := strings.Split(strings.TrimPrefix(line, prefix), ",")
		if len(values) < 2 {
			t.Fatalf("malformed Redis commandstat: %q", line)
		}
		calls, callsErr := strconv.ParseInt(values[0], 10, 64)
		microseconds, usecErr := strconv.ParseInt(strings.TrimPrefix(values[1], "usec="), 10, 64)
		if callsErr != nil || usecErr != nil || calls <= 0 {
			t.Fatalf("malformed Redis commandstat: %q", line)
		}
		return calls, microseconds
	}
	t.Fatalf("Redis INFO commandstats omitted %s", command)
	return 0, 0
}
