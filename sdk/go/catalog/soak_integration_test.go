//go:build integration && load && soak

package catalog

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"math/bits"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

const catalogSoakFieldBytes = 256

type catalogSoakHistogram struct {
	count   uint64
	sum     uint64
	maximum uint64
	buckets [65]uint64
}

type catalogSoakDurationStats struct {
	Count   uint64        `json:"count"`
	P50     time.Duration `json:"p50_nanoseconds"`
	P95     time.Duration `json:"p95_nanoseconds"`
	P99     time.Duration `json:"p99_nanoseconds"`
	Maximum time.Duration `json:"maximum_nanoseconds"`
	Average time.Duration `json:"average_nanoseconds"`
}

type catalogSoakStats struct {
	mu sync.Mutex

	attempts        uint64
	patches         uint64
	replaces        uint64
	deletes         uint64
	transient       uint64
	stale           uint64
	convergence     uint64
	maximumRevision uint64
	latency         catalogSoakHistogram
	scheduleLag     catalogSoakHistogram
}

type catalogSoakErrorCollector struct {
	expected atomic.Uint64
	mu       sync.Mutex
	values   []string
}

type catalogSoakProcess struct {
	InitialGoroutines int    `json:"initial_goroutines"`
	PeakGoroutines    int    `json:"peak_goroutines"`
	FinalGoroutines   int    `json:"final_goroutines"`
	InitialHeapBytes  uint64 `json:"initial_heap_bytes"`
	PeakHeapBytes     uint64 `json:"peak_heap_bytes"`
	FinalHeapBytes    uint64 `json:"final_heap_bytes"`
	PeakHeapObjects   uint64 `json:"peak_heap_objects"`
	Samples           uint64 `json:"samples"`
}

type catalogSoakProcessMonitor struct {
	mu     sync.Mutex
	result catalogSoakProcess
}

type catalogSoakResult struct {
	DurationSeconds       int                      `json:"duration_seconds"`
	Catalogs              int                      `json:"catalogs"`
	Fields                int                      `json:"fields"`
	Subscribers           int                      `json:"subscribers"`
	PersistentSubscribers int                      `json:"persistent_subscribers"`
	TargetRate            int                      `json:"target_operations_per_second"`
	Attempts              uint64                   `json:"attempts"`
	Patches               uint64                   `json:"patches"`
	Replaces              uint64                   `json:"replaces"`
	Deletes               uint64                   `json:"deletes"`
	TransientErrors       uint64                   `json:"transient_errors"`
	StaleRetries          uint64                   `json:"stale_retries"`
	ExpectedAsyncErrors   uint64                   `json:"expected_async_errors"`
	UnexpectedAsyncErrors []string                 `json:"unexpected_async_errors"`
	ConvergenceChecks     uint64                   `json:"convergence_checks"`
	MaximumRevision       uint64                   `json:"maximum_revision"`
	MutationLatency       catalogSoakDurationStats `json:"mutation_latency"`
	ScheduleLag           catalogSoakDurationStats `json:"schedule_lag"`
	Process               catalogSoakProcess       `json:"process"`
}

func TestCatalogSoak(t *testing.T) {
	redisURL := strings.TrimSpace(os.Getenv("VERDANDI_REDIS_URL"))
	if redisURL == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	duration := catalogSoakInteger(t, "VERDANDI_SOAK_SECONDS", 86_400, 30, 86_400)
	catalogs := catalogSoakInteger(t, "VERDANDI_CATALOG_SOAK_CATALOGS", 16, 1, 64)
	fields := catalogSoakInteger(t, "VERDANDI_CATALOG_SOAK_FIELDS", 256, 1, 1_024)
	subscriberCount := catalogSoakInteger(t, "VERDANDI_CATALOG_SUBSCRIBER_FANOUT", 2, 1, 8)
	persistentSubscriberCount := catalogSoakInteger(t, "VERDANDI_CATALOG_PERSISTENT_SUBSCRIBERS", 1, 0, subscriberCount)
	rate := catalogSoakInteger(t, "VERDANDI_CATALOG_SOAK_RATE", 128, 1, 2_000)
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	zone := catalogSoakZone(t)
	// Redis TIME is the qualification clock. A VM-local deadline can expire
	// early when its monotonic clock runs faster than the Redis host clock.
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	transport, err := verdandi.Open(ctx, verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
		Timeout: 5 * time.Second,
	})
	if err != nil {
		_ = raw.Close()
		t.Fatal(err)
	}
	config := Config{Zone: zone, SyncTimeout: 45 * time.Second}
	client, err := Open(ctx, transport, config)
	if err != nil {
		_ = transport.Close()
		_ = raw.Close()
		t.Fatal(err)
	}
	clients := []*Client{client}
	clientsClosed := false
	t.Cleanup(func() {
		if !clientsClosed {
			for _, catalogClient := range clients {
				closeCtx, closeCancel := context.WithTimeout(context.Background(), time.Minute)
				_ = catalogClient.Close(closeCtx)
				closeCancel()
			}
			_ = transport.Close()
		}
		catalogSoakCleanup(t, raw, zone)
		_ = raw.Close()
	})
	var persistentClient *Client
	if persistentSubscriberCount != 0 {
		persistentConfig := config
		persistentConfig.LocalStorePath = filepath.Join(t.TempDir(), "catalog.db")
		persistentClient, err = Open(ctx, transport, persistentConfig)
		if err != nil {
			t.Fatal(err)
		}
		clients = append(clients, persistentClient)
	}

	publisher, err := client.Publisher()
	if err != nil {
		t.Fatal(err)
	}
	paths := make([]Path, catalogs)
	revisions := make([]uint64, catalogs)
	statuses := make([]Status, catalogs)
	for index := range paths {
		paths[index], err = NewPath("soak", fmt.Sprintf("item-%03d", index))
		if err != nil {
			t.Fatal(err)
		}
		operation, operationCancel := context.WithTimeout(ctx, 30*time.Second)
		result, replaceErr := publisher.Replace(
			operation,
			paths[index],
			Map,
			catalogSoakValue(fields, uint64(index+1)),
		)
		operationCancel()
		if replaceErr != nil {
			t.Fatalf("initialize %s: %v", paths[index].ID(), replaceErr)
		}
		revisions[index] = result.Revision
		statuses[index] = StatusPresent
	}

	subscribers := make([]*Subscriber, subscriberCount)
	for index := range subscribers {
		subscriberClient := catalogSoakSubscriberClient(index, persistentSubscriberCount, client, persistentClient)
		subscribers[index], err = subscriberClient.Subscriber(ctx, Subscription{Parts: []string{"soak"}})
		if err != nil {
			t.Fatalf("subscriber[%d]: %v", index, err)
		}
	}
	if err := catalogSoakWaitConverged(ctx, subscribers, paths, fields, time.Minute); err != nil {
		t.Fatal(err)
	}

	collector := &catalogSoakErrorCollector{}
	var watchers sync.WaitGroup
	for index, subscriber := range subscribers {
		catalogSoakWatchErrors(&watchers, fmt.Sprintf("subscriber[%d]", index), subscriber.Errors(), collector)
	}
	stats := &catalogSoakStats{}
	process := catalogSoakInitialProcess()
	monitorCtx, stopMonitor := context.WithCancel(ctx)
	monitorDone := make(chan struct{})
	go process.run(monitorCtx, monitorDone)

	runCtx, stopRun := context.WithCancel(ctx)
	heartbeatDone := make(chan struct{})
	go catalogSoakHeartbeats(t, runCtx, stats, collector, process, heartbeatDone)
	started := time.Now()
	if err := catalogSoakMutate(
		runCtx,
		raw,
		publisher,
		subscribers[0],
		paths,
		revisions,
		statuses,
		fields,
		rate,
		time.Duration(duration)*time.Second,
		stats,
	); err != nil {
		stopRun()
		<-heartbeatDone
		stopMonitor()
		<-monitorDone
		t.Fatal(err)
	}
	stopRun()
	<-heartbeatDone

	if elapsed := time.Since(started); elapsed < time.Duration(duration)*time.Second*99/100 {
		t.Fatalf("soak elapsed %s is shorter than 99%% of %s", elapsed, time.Duration(duration)*time.Second)
	}
	fresh, err := client.Subscriber(ctx, Subscription{Parts: []string{"soak"}})
	if err != nil {
		t.Fatal(err)
	}
	subscribers = append(subscribers, fresh)
	catalogSoakWatchErrors(&watchers, "subscriber[fresh]", fresh.Errors(), collector)
	if err := catalogSoakWaitConverged(ctx, subscribers, paths, fields, time.Minute); err != nil {
		t.Fatal(err)
	}
	stats.mu.Lock()
	stats.convergence++
	stats.mu.Unlock()

	for _, subscriber := range subscribers {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), time.Minute)
		if err := subscriber.Close(closeCtx); err != nil {
			closeCancel()
			t.Fatal(err)
		}
		closeCancel()
	}
	watchers.Wait()
	for index, catalogClient := range clients {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), time.Minute)
		if err := catalogClient.Close(closeCtx); err != nil {
			closeCancel()
			t.Fatalf("client[%d]: %v", index, err)
		}
		closeCancel()
	}
	if err := transport.Close(); err != nil {
		t.Fatal(err)
	}
	clientsClosed = true
	stopMonitor()
	<-monitorDone
	runtime.GC()
	processResult := process.finish()

	result := catalogSoakResult{
		DurationSeconds:       duration,
		Catalogs:              catalogs,
		Fields:                fields,
		Subscribers:           subscriberCount,
		PersistentSubscribers: persistentSubscriberCount,
		TargetRate:            rate,
		ExpectedAsyncErrors:   collector.expected.Load(),
		UnexpectedAsyncErrors: collector.snapshot(),
		Process:               processResult,
	}
	stats.mu.Lock()
	result.Attempts = stats.attempts
	result.Patches = stats.patches
	result.Replaces = stats.replaces
	result.Deletes = stats.deletes
	result.TransientErrors = stats.transient
	result.StaleRetries = stats.stale
	result.ConvergenceChecks = stats.convergence
	result.MaximumRevision = stats.maximumRevision
	result.MutationLatency = stats.latency.snapshot()
	result.ScheduleLag = stats.scheduleLag.snapshot()
	stats.mu.Unlock()
	if len(result.UnexpectedAsyncErrors) != 0 {
		t.Fatalf("unexpected asynchronous errors: %v", result.UnexpectedAsyncErrors)
	}
	if result.Patches+result.Replaces+result.Deletes == 0 {
		t.Fatal("Catalog soak accepted no mutations")
	}
	encoded, err := json.Marshal(result)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("CATALOG_SOAK_RESULT %s", encoded)
}

func catalogSoakSubscriberClient(index int, persistentSubscribers int, memoryClient *Client, persistentClient *Client) *Client {
	if index < persistentSubscribers {
		return persistentClient
	}
	return memoryClient
}

func catalogSoakMutate(
	ctx context.Context,
	clock *redis.Client,
	publisher *Publisher,
	subscriber *Subscriber,
	paths []Path,
	revisions []uint64,
	statuses []Status,
	fields int,
	rate int,
	duration time.Duration,
	stats *catalogSoakStats,
) error {
	serverStarted, err := catalogSoakRedisTime(ctx, clock)
	if err != nil {
		return fmt.Errorf("read initial Redis time: %w", err)
	}
	started := time.Now()
	interval := time.Second / time.Duration(rate)
	for sequence := uint64(0); ; sequence++ {
		// Redis is the qualification clock because VM monotonic clocks may drift.
		if sequence%uint64(rate) == 0 {
			serverNow, timeErr := catalogSoakRedisTime(ctx, clock)
			if timeErr != nil {
				return fmt.Errorf("read Redis time: %w", timeErr)
			}
			if serverNow.Sub(serverStarted) >= duration {
				return nil
			}
		}
		target := started.Add(time.Duration(sequence) * interval)
		if delay := time.Until(target); delay > 0 {
			timer := time.NewTimer(delay)
			select {
			case <-timer.C:
			case <-ctx.Done():
				if !timer.Stop() {
					<-timer.C
				}
				return ctx.Err()
			}
		}
		lag := time.Since(target)
		pathIndex := int(sequence % uint64(len(paths)))
		path := paths[pathIndex]
		entry := subscriber.Find(path)
		if entry == nil {
			return fmt.Errorf("missing local Entry for %s", path.ID())
		}
		operation, cancel := context.WithTimeout(ctx, 10*time.Second)
		operationStarted := time.Now()
		var result Result
		var err error
		cycle := sequence / uint64(len(paths))
		kind := cycle % 256
		switch kind {
		case 0:
			result, err = publisher.Delete(operation, path)
		case 1:
			result, err = publisher.Replace(operation, path, Map, catalogSoakValue(fields, sequence+1))
		case 2, 64, 128, 192:
			result, err = publisher.Replace(operation, path, Map, catalogSoakValue(fields, sequence+1))
		default:
			if revisions[pathIndex] == 0 && entry.Status() == StatusPresent {
				revisions[pathIndex] = entry.Revision()
				statuses[pathIndex] = StatusPresent
			}
			if statuses[pathIndex] != StatusPresent || revisions[pathIndex] == 0 {
				cancel()
				catalogSoakRecord(stats, lag, 0, 0, verdandi.CodeStale)
				continue
			}
			field := fmt.Sprintf("field-%04d", sequence%uint64(fields))
			result, err = publisher.Patch(operation, path, Patch{
				BaseRevision: revisions[pathIndex],
				Set:          verdandi.Fields{field: catalogSoakPayload(sequence + 1)},
			})
		}
		latency := time.Since(operationStarted)
		cancel()
		if err != nil && !catalogSoakTransient(err) && !verdandi.IsCode(err, verdandi.CodeStale) {
			return fmt.Errorf("mutate %s: %w", path.ID(), err)
		}
		code := verdandi.Code("")
		if err != nil {
			switch {
			case verdandi.IsCode(err, verdandi.CodeStale):
				code = verdandi.CodeStale
			default:
				code = verdandi.CodeUnavailable
			}
		}
		catalogSoakRecord(stats, lag, latency, result.Revision, code)
		if err == nil {
			revisions[pathIndex] = result.Revision
			stats.mu.Lock()
			switch kind {
			case 0:
				stats.deletes++
				statuses[pathIndex] = StatusDeleted
			case 1, 2, 64, 128, 192:
				stats.replaces++
				statuses[pathIndex] = StatusPresent
			default:
				stats.patches++
			}
			stats.mu.Unlock()
		} else {
			revisions[pathIndex] = 0
			statuses[pathIndex] = StatusSynchronizing
		}
	}
}

func catalogSoakRecord(
	stats *catalogSoakStats,
	lag time.Duration,
	latency time.Duration,
	revision uint64,
	code verdandi.Code,
) {
	stats.mu.Lock()
	defer stats.mu.Unlock()
	stats.attempts++
	stats.scheduleLag.observe(lag)
	if latency > 0 {
		stats.latency.observe(latency)
	}
	if revision > stats.maximumRevision {
		stats.maximumRevision = revision
	}
	switch code {
	case verdandi.CodeStale:
		stats.stale++
	case "":
	default:
		stats.transient++
	}
}

func catalogSoakRedisTime(ctx context.Context, client *redis.Client) (time.Time, error) {
	deadline := time.Now().Add(30 * time.Second)
	var lastErr error
	for {
		operation, cancel := context.WithTimeout(ctx, 5*time.Second)
		value, err := client.Time(operation).Result()
		cancel()
		if err == nil {
			return value, nil
		}
		lastErr = err
		if time.Now().After(deadline) {
			return time.Time{}, lastErr
		}
		timer := time.NewTimer(100 * time.Millisecond)
		select {
		case <-timer.C:
		case <-ctx.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return time.Time{}, ctx.Err()
		}
	}
}

func catalogSoakWaitConverged(
	ctx context.Context,
	subscribers []*Subscriber,
	paths []Path,
	fieldCount int,
	timeout time.Duration,
) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		converged := true
		for _, path := range paths {
			var expected Snapshot[verdandi.Fields]
			for index, subscriber := range subscribers {
				entry := subscriber.Find(path)
				if entry == nil || !entry.Synchronized() {
					converged = false
					break
				}
				snapshot, err := entry.Load[verdandi.Fields]()
				if err != nil {
					return fmt.Errorf("load %s: %w", path.ID(), err)
				}
				if snapshot.Status == StatusPresent {
					if snapshot.Value == nil || len(*snapshot.Value) != fieldCount {
						return fmt.Errorf("invalid complete value for %s", path.ID())
					}
					for name, value := range *snapshot.Value {
						if !strings.HasPrefix(name, "field-") || len(value) != catalogSoakFieldBytes {
							return fmt.Errorf("invalid field %q for %s", name, path.ID())
						}
					}
				}
				if index == 0 {
					expected = snapshot
				} else if !catalogSoakSnapshotsEqual(expected, snapshot) {
					converged = false
					break
				}
			}
			if !converged {
				break
			}
		}
		if converged {
			return nil
		}
		timer := time.NewTimer(25 * time.Millisecond)
		select {
		case <-timer.C:
		case <-ctx.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return ctx.Err()
		}
	}
	return errors.New("Catalog Subscribers did not converge")
}

func catalogSoakSnapshotsEqual(left, right Snapshot[verdandi.Fields]) bool {
	if left.Revision != right.Revision || left.Status != right.Status ||
		left.Synchronized != right.Synchronized || (left.Value == nil) != (right.Value == nil) {
		return false
	}
	if left.Value == nil {
		return true
	}
	if len(*left.Value) != len(*right.Value) {
		return false
	}
	for name, value := range *left.Value {
		if !bytes.Equal(value, (*right.Value)[name]) {
			return false
		}
	}
	return true
}

func catalogSoakValue(fields int, generation uint64) verdandi.Fields {
	value := make(verdandi.Fields, fields)
	for index := range fields {
		value[fmt.Sprintf("field-%04d", index)] = catalogSoakPayload(generation + uint64(index))
	}
	return value
}

func catalogSoakPayload(generation uint64) []byte {
	payload := bytes.Repeat([]byte{'x'}, catalogSoakFieldBytes)
	copy(payload, fmt.Sprintf("%020d", generation))
	return payload
}

func catalogSoakTransient(err error) bool {
	return verdandi.IsCode(err, verdandi.CodeAmbiguous) ||
		verdandi.IsCode(err, verdandi.CodeUnavailable) ||
		verdandi.IsCode(err, verdandi.CodeDeadline)
}

func catalogSoakWatchErrors(
	wait *sync.WaitGroup,
	owner string,
	errorsStream <-chan error,
	collector *catalogSoakErrorCollector,
) {
	wait.Add(1)
	go func() {
		defer wait.Done()
		for err := range errorsStream {
			if err == nil || errors.Is(err, context.Canceled) {
				continue
			}
			if catalogSoakTransient(err) {
				collector.expected.Add(1)
				continue
			}
			collector.mu.Lock()
			if len(collector.values) < 32 {
				collector.values = append(collector.values, owner+": "+err.Error())
			}
			collector.mu.Unlock()
		}
	}()
}

func (collector *catalogSoakErrorCollector) snapshot() []string {
	collector.mu.Lock()
	defer collector.mu.Unlock()
	return append([]string(nil), collector.values...)
}

func catalogSoakHeartbeats(
	t *testing.T,
	ctx context.Context,
	stats *catalogSoakStats,
	collector *catalogSoakErrorCollector,
	process *catalogSoakProcessMonitor,
	done chan<- struct{},
) {
	defer close(done)
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			stats.mu.Lock()
			value := map[string]any{
				"attempts":              stats.attempts,
				"accepted":              stats.patches + stats.replaces + stats.deletes,
				"transient_errors":      stats.transient,
				"stale_retries":         stats.stale,
				"maximum_revision":      stats.maximumRevision,
				"mutation_p95_ns":       stats.latency.quantile(0.95),
				"expected_async_errors": collector.expected.Load(),
			}
			stats.mu.Unlock()
			value["goroutines"] = runtime.NumGoroutine()
			processSnapshot := process.snapshot()
			value["peak_heap_bytes"] = processSnapshot.PeakHeapBytes
			encoded, err := json.Marshal(value)
			if err == nil {
				t.Logf("CATALOG_SOAK_HEARTBEAT %s", encoded)
			}
		case <-ctx.Done():
			return
		}
	}
}

func (histogram *catalogSoakHistogram) observe(value time.Duration) {
	if value < 0 {
		value = 0
	}
	nanoseconds := uint64(value)
	bucket := 0
	if nanoseconds != 0 {
		bucket = bits.Len64(nanoseconds)
	}
	histogram.buckets[bucket]++
	histogram.count++
	histogram.sum += nanoseconds
	if nanoseconds > histogram.maximum {
		histogram.maximum = nanoseconds
	}
}

func (histogram catalogSoakHistogram) quantile(value float64) time.Duration {
	if histogram.count == 0 {
		return 0
	}
	target := uint64(float64(histogram.count-1)*value) + 1
	var observed uint64
	for bucket, count := range histogram.buckets {
		observed += count
		if observed >= target {
			if bucket == 0 {
				return 0
			}
			if bucket == 64 {
				return time.Duration(histogram.maximum)
			}
			return time.Duration((uint64(1) << bucket) - 1)
		}
	}
	return time.Duration(histogram.maximum)
}

func (histogram catalogSoakHistogram) snapshot() catalogSoakDurationStats {
	result := catalogSoakDurationStats{
		Count:   histogram.count,
		P50:     histogram.quantile(0.50),
		P95:     histogram.quantile(0.95),
		P99:     histogram.quantile(0.99),
		Maximum: time.Duration(histogram.maximum),
	}
	if histogram.count != 0 {
		result.Average = time.Duration(histogram.sum / histogram.count)
	}
	return result
}

func catalogSoakInitialProcess() *catalogSoakProcessMonitor {
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	return &catalogSoakProcessMonitor{
		result: catalogSoakProcess{
			InitialGoroutines: runtime.NumGoroutine(),
			PeakGoroutines:    runtime.NumGoroutine(),
			InitialHeapBytes:  memory.HeapAlloc,
			PeakHeapBytes:     memory.HeapAlloc,
			PeakHeapObjects:   memory.HeapObjects,
		},
	}
}

func (process *catalogSoakProcessMonitor) run(ctx context.Context, done chan<- struct{}) {
	defer close(done)
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			var memory runtime.MemStats
			runtime.ReadMemStats(&memory)
			goroutines := runtime.NumGoroutine()
			process.mu.Lock()
			if goroutines > process.result.PeakGoroutines {
				process.result.PeakGoroutines = goroutines
			}
			if memory.HeapAlloc > process.result.PeakHeapBytes {
				process.result.PeakHeapBytes = memory.HeapAlloc
			}
			if memory.HeapObjects > process.result.PeakHeapObjects {
				process.result.PeakHeapObjects = memory.HeapObjects
			}
			process.result.Samples++
			process.mu.Unlock()
		case <-ctx.Done():
			return
		}
	}
}

func (process *catalogSoakProcessMonitor) snapshot() catalogSoakProcess {
	process.mu.Lock()
	defer process.mu.Unlock()
	return process.result
}

func (process *catalogSoakProcessMonitor) finish() catalogSoakProcess {
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	process.mu.Lock()
	defer process.mu.Unlock()
	process.result.FinalGoroutines = runtime.NumGoroutine()
	process.result.FinalHeapBytes = memory.HeapAlloc
	if process.result.FinalGoroutines > process.result.PeakGoroutines {
		process.result.PeakGoroutines = process.result.FinalGoroutines
	}
	if process.result.FinalHeapBytes > process.result.PeakHeapBytes {
		process.result.PeakHeapBytes = process.result.FinalHeapBytes
	}
	if memory.HeapObjects > process.result.PeakHeapObjects {
		process.result.PeakHeapObjects = memory.HeapObjects
	}
	return process.result
}

func catalogSoakInteger(t *testing.T, name string, fallback, minimum, maximum int) int {
	t.Helper()
	text := strings.TrimSpace(os.Getenv(name))
	if text == "" {
		return fallback
	}
	value, err := strconv.Atoi(text)
	if err != nil || value < minimum || value > maximum {
		t.Fatalf("%s must be %d..%d", name, minimum, maximum)
	}
	return value
}

func catalogSoakZone(t *testing.T) string {
	t.Helper()
	random := make([]byte, 12)
	if _, err := rand.Read(random); err != nil {
		t.Fatal(err)
	}
	for index := range random {
		random[index] = 'a' + random[index]%26
	}
	return "CatalogSoak" + string(random)
}

func catalogSoakCleanup(t *testing.T, client *redis.Client, zone string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	var cursor uint64
	for {
		keys, next, err := client.Scan(ctx, cursor, zonePrefix(zone)+"*", 512).Result()
		if err != nil {
			t.Error(err)
			return
		}
		if len(keys) != 0 {
			if err := client.Unlink(ctx, keys...).Err(); err != nil {
				t.Error(err)
				return
			}
		}
		cursor = next
		if cursor == 0 {
			return
		}
	}
}
