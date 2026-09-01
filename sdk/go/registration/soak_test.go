//go:build integration && load && soak

package registration

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

const (
	soakRegistrationCount = 500
	soakExpiryBurst       = 128
	soakSelectionRate     = 10
)

type soakAttr struct {
	Role  string
	Shard int
}

func (value soakAttr) Encode() (Fields, error) {
	return Fields{
		"role":  []byte(value.Role),
		"shard": strconv.AppendInt(nil, int64(value.Shard), 10),
	}, nil
}

func (value *soakAttr) Decode(source Fields) error {
	if value == nil || len(source) != 2 {
		return fmt.Errorf("invalid soak Attr field count")
	}
	role, roleOK := source["role"]
	shard, shardOK := source["shard"]
	if !roleOK || !shardOK {
		return fmt.Errorf("invalid soak Attr fields")
	}
	decodedShard, err := strconv.Atoi(string(shard))
	if err != nil {
		return fmt.Errorf("decode soak Attr shard: %w", err)
	}
	*value = soakAttr{Role: string(role), Shard: decodedShard}
	return nil
}

type soakData struct {
	Load    int
	Power   int64
	Payload string
}

func (value soakData) Encode() (Fields, error) {
	return Fields{
		"load":    strconv.AppendInt(nil, int64(value.Load), 10),
		"power":   strconv.AppendInt(nil, value.Power, 10),
		"payload": []byte(value.Payload),
	}, nil
}

func (value *soakData) Decode(source Fields) error {
	if value == nil || len(source) != 3 {
		return fmt.Errorf("invalid soak Data field count")
	}
	load, loadOK := source["load"]
	power, powerOK := source["power"]
	payload, payloadOK := source["payload"]
	if !loadOK || !powerOK || !payloadOK {
		return fmt.Errorf("invalid soak Data fields")
	}
	decodedLoad, err := strconv.Atoi(string(load))
	if err != nil {
		return fmt.Errorf("decode soak Data load: %w", err)
	}
	decodedPower, err := strconv.ParseInt(string(power), 10, 64)
	if err != nil {
		return fmt.Errorf("decode soak Data power: %w", err)
	}
	*value = soakData{
		Load:    decodedLoad,
		Power:   decodedPower,
		Payload: string(payload),
	}
	return nil
}

type soakDurationStats struct {
	Count       int           `json:"count"`
	P50         time.Duration `json:"p50_nanoseconds"`
	P95         time.Duration `json:"p95_nanoseconds"`
	P99         time.Duration `json:"p99_nanoseconds"`
	Maximum     time.Duration `json:"maximum_nanoseconds"`
	Average     time.Duration `json:"average_nanoseconds"`
	ZeroSamples int           `json:"zero_samples"`
}

type soakProcessStats struct {
	InitialGoroutines int    `json:"initial_goroutines"`
	PeakGoroutines    int    `json:"peak_goroutines"`
	FinalGoroutines   int    `json:"final_goroutines"`
	InitialHeapBytes  uint64 `json:"initial_heap_bytes"`
	PeakHeapBytes     uint64 `json:"peak_heap_bytes"`
	FinalHeapBytes    uint64 `json:"final_heap_bytes"`
	PeakHeapObjects   uint64 `json:"peak_heap_objects"`
	Samples           int    `json:"samples"`
}

type soakResult struct {
	DurationSeconds       int               `json:"duration_seconds"`
	UpdateElapsed         time.Duration     `json:"update_elapsed_nanoseconds"`
	Registrations         int               `json:"registrations"`
	Selectors             int               `json:"selectors"`
	Updates               int               `json:"updates"`
	UpdateRetries         int64             `json:"update_retries"`
	UpdateLatency         soakDurationStats `json:"update_latency"`
	ScheduleLag           soakDurationStats `json:"schedule_lag"`
	SelectionTransactions int64             `json:"selection_transactions"`
	SelectionMutations    int64             `json:"selection_mutations"`
	SelectionRetries      int64             `json:"selection_retries"`
	SelectionLatency      soakDurationStats `json:"selection_latency"`
	ExpiryCycles          int64             `json:"expiry_cycles"`
	ExpiryRegistrations   int64             `json:"expiry_registrations"`
	PeakRetainedRecords   int64             `json:"peak_retained_records"`
	ChurnCycles           int64             `json:"churn_cycles"`
	ChurnRegistrations    int64             `json:"churn_registrations"`
	ExpectedAsyncErrors   int64             `json:"expected_async_errors"`
	UnexpectedAsyncErrors []string          `json:"unexpected_async_errors"`
	FinalRevision         uint64            `json:"final_revision"`
	FinalGeneration       uint64            `json:"final_generation"`
	Process               soakProcessStats  `json:"process"`
}

type soakSelectionWorkerResult struct {
	latencies    []time.Duration
	transactions int64
	mutations    int64
	retries      int64
	err          error
}

type soakSelectionResult struct {
	latencies    []time.Duration
	transactions int64
	mutations    int64
	retries      int64
	perSelector  []int64
}

type soakUpdateResult struct {
	latencies    []time.Duration
	scheduleLags []time.Duration
	retries      int64
	elapsed      time.Duration
	err          error
}

type soakErrorCollector struct {
	expected   atomic.Int64
	mu         sync.Mutex
	unexpected []string
}

type soakLifecycleStats struct {
	expiryCycles        atomic.Int64
	expiryRegistrations atomic.Int64
	peakRetained        atomic.Int64
	churnCycles         atomic.Int64
	churnRegistrations  atomic.Int64
}

type soakRuntimeMonitor struct {
	mu     sync.Mutex
	result soakProcessStats
}

func TestRegistrationSelectorSoak(t *testing.T) {
	redisURL := requireRedisURL(t)
	duration := soakDuration(t)
	lifecycleInterval := soakLifecycleInterval(t)
	fanout := qualificationFanout(t)
	if fanout < 2 {
		t.Fatal("soak qualification requires at least two Selectors")
	}

	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	baselineGoroutines := runtime.NumGoroutine()
	var baselineMemory runtime.MemStats
	runtime.ReadMemStats(&baselineMemory)
	raw := redis.NewClient(options)
	zone := integrationZone(t)
	// Redis TIME controls the workload floor. The local deadline only bounds a
	// stuck run and therefore leaves headroom for virtualized clock drift.
	ctx, cancel := context.WithTimeout(
		context.Background(),
		time.Duration(duration+3_600)*time.Second,
	)
	defer cancel()

	retainedBytes := 8 * 1024
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
	client, err := Open(ctx, transport, Config{
		Zone:                    zone,
		SelectorPageSize:        64,
		SelectorEventBuffer:     65_536,
		SelectorEventBytes:      64 * 1024 * 1024,
		SelectorMaxBytes:        256 * 1024 * 1024,
		SelectorRetainedBytes:   &retainedBytes,
		SelectorPublishInterval: new(time.Millisecond),
		SelectorSyncTimeout:     30 * time.Second,
		ClockRefresh:            time.Second,
	})
	if err != nil {
		_ = transport.Close()
		_ = raw.Close()
		t.Fatal(err)
	}

	closed := false
	t.Cleanup(func() {
		if !closed {
			closeCtx, closeCancel := context.WithTimeout(context.Background(), time.Minute)
			_ = client.Close(closeCtx)
			_ = transport.Close()
			closeCancel()
			cleanupZone(t, raw, zone)
			_ = raw.Close()
		}
	})

	payload := strings.Repeat("x", 128)
	registrations := make([]*Registration[soakAttr, soakData], soakRegistrationCount)
	runBounded(t, soakRegistrationCount, 64, func(index int) error {
		registration, registerErr := client.Registration[soakAttr, soakData](RegistrationOptions{
			Type:          "soak",
			TTL:           30 * time.Second,
			RenewInterval: 10 * time.Second,
			Version:       1,
		})
		if registerErr != nil {
			return registerErr
		}
		registrations[index] = registration
		return registration.Register(
			ctx,
			soakAttr{Role: "worker", Shard: index % 16},
			soakData{Payload: payload},
		)
	})

	selectors := make([]*Selector[soakAttr, soakData], 0, fanout)
	for range fanout {
		selector, selectErr := client.Selector[soakAttr, soakData](ctx, SelectorOptions{Type: "soak"})
		if selectErr != nil {
			t.Fatal(selectErr)
		}
		selectors = append(selectors, selector)
	}
	expirySelector, err := selectRaw(ctx, client, SelectorConfig{Type: "expiry"})
	if err != nil {
		t.Fatal(err)
	}
	churnSelector, err := selectRaw(ctx, client, SelectorConfig{Type: "churn"})
	if err != nil {
		t.Fatal(err)
	}
	for _, selector := range selectors {
		waitForTypedSnapshot(t, ctx, selector, soakRegistrationCount, 1)
	}
	// Pace complete update rounds against the same Redis clock used by the
	// external qualification gate, so a fast VM clock cannot shorten the run.
	redisStart, err := readSoakRedisTime(ctx, raw)
	if err != nil {
		t.Fatal(err)
	}

	errorCollector := &soakErrorCollector{}
	var errorWatchers sync.WaitGroup
	watchSoakErrors(&errorWatchers, "client", client.Errors(), errorCollector)

	runtimeMonitor := newSoakRuntimeMonitor(baselineGoroutines, baselineMemory.HeapAlloc)
	runtimeStop := make(chan struct{})
	runtimeDone := make(chan struct{})
	go runtimeMonitor.run(runtimeStop, runtimeDone)

	lifecycleCtx, lifecycleCancel := context.WithCancel(ctx)
	lifecycleStats := &soakLifecycleStats{}
	lifecycleErrors := make(chan error, 2)
	go func() {
		lifecycleErrors <- runSoakExpiryCycles(
			lifecycleCtx,
			client,
			expirySelector,
			lifecycleInterval,
			lifecycleStats,
		)
	}()
	go func() {
		lifecycleErrors <- runSoakChurnCycles(
			lifecycleCtx,
			client,
			churnSelector,
			lifecycleInterval,
			lifecycleStats,
		)
	}()

	runCtx, stopRun := context.WithCancel(ctx)
	defer stopRun()
	selectionDone := make(chan struct {
		result soakSelectionResult
		err    error
	}, 1)
	go func() {
		result, selectionErr := runSoakSelections(runCtx, selectors, duration)
		selectionDone <- struct {
			result soakSelectionResult
			err    error
		}{result: result, err: selectionErr}
	}()
	updateDone := make(chan soakUpdateResult, 1)
	go func() {
		latencies, scheduleLags, retries, elapsed, updateErr := runSoakCadencedUpdates(
			runCtx,
			raw,
			redisStart,
			registrations,
			duration,
			payload,
		)
		updateDone <- soakUpdateResult{
			latencies:    latencies,
			scheduleLags: scheduleLags,
			retries:      retries,
			elapsed:      elapsed,
			err:          updateErr,
		}
	}()

	var updates soakUpdateResult
	var selection struct {
		result soakSelectionResult
		err    error
	}
	var runErr error
	for completed := 0; completed < 2; completed++ {
		select {
		case updates = <-updateDone:
			if updates.err != nil {
				if runErr == nil {
					runErr = fmt.Errorf("Registration update loop: %w", updates.err)
				}
			}
			stopRun()
		case selection = <-selectionDone:
			if selection.err != nil {
				if runErr == nil {
					runErr = fmt.Errorf("Selector policy loop: %w", selection.err)
				}
				stopRun()
			}
		}
	}
	lifecycleCancel()
	for range 2 {
		if lifecycleErr := <-lifecycleErrors; lifecycleErr != nil {
			t.Errorf("lifecycle soak: %v", lifecycleErr)
		}
	}
	if runErr != nil {
		t.Fatal(runErr)
	}
	if updates.elapsed < time.Duration(duration)*time.Second*99/100 {
		t.Fatalf("soak elapsed %s is shorter than 99%% of %s", updates.elapsed, time.Duration(duration)*time.Second)
	}

	finalRevision := uint64(duration + 1)
	var finalGeneration uint64
	for index, selector := range selectors {
		waitForTypedSnapshot(t, ctx, selector, soakRegistrationCount, finalRevision)
		snapshot, snapshotErr := selector.Snapshot(ctx)
		if snapshotErr != nil {
			t.Fatal(snapshotErr)
		}
		if snapshot.Generation > finalGeneration {
			finalGeneration = snapshot.Generation
		}
		var predictedPower int64
		for _, candidate := range snapshot.Candidates {
			if candidate.Data == nil || candidate.Data.Load != duration || candidate.Data.Payload != payload {
				t.Fatalf("final data mismatch for %s: %#v", candidate.Meta.UUID, candidate.Data)
			}
			predictedPower += candidate.Data.Power
		}
		if predictedPower != selection.result.perSelector[index] {
			t.Fatalf(
				"selector[%d] predicted power=%d, committed mutations=%d",
				index,
				predictedPower,
				selection.result.perSelector[index],
			)
		}
	}

	updateStats := summarizeSoakDurations(updates.latencies)
	lagStats := summarizeSoakDurations(updates.scheduleLags)
	if updateStats.ZeroSamples != 0 {
		t.Errorf("update latency contains %d zero samples", updateStats.ZeroSamples)
	}
	if updateStats.P99 > time.Second {
		t.Errorf("update p99 exceeded one second: %s", updateStats.P99)
	}
	lagLimit := time.Second
	if duration < 600 {
		// Short preflights deliberately compress a three-second Redis pause and
		// restart into less than ten minutes, so more than one percent of their
		// scheduling samples must cross the production p99 boundary.
		lagLimit = 5 * time.Second
	}
	if lagStats.P99 > lagLimit {
		t.Errorf("schedule-lag p99 exceeded %s: %s", lagLimit, lagStats.P99)
	}
	selectionStats := summarizeSoakDurations(selection.result.latencies)
	expectedSelectionTransactions := int64(fanout * duration * soakSelectionRate)
	if selection.result.transactions < expectedSelectionTransactions*95/100 {
		t.Errorf(
			"selection transactions=%d, want at least 95%% of %d",
			selection.result.transactions,
			expectedSelectionTransactions,
		)
	}
	if selectionStats.P99 > 250*time.Millisecond {
		t.Errorf("selection p99 exceeded 250ms: %s", selectionStats.P99)
	}
	minimumLifecycleCycles := int64(2)
	if duration >= 3_600 {
		minimumLifecycleCycles = 10
	}
	if lifecycleStats.expiryCycles.Load() < minimumLifecycleCycles {
		t.Errorf("expiry cycles=%d, want at least %d", lifecycleStats.expiryCycles.Load(), minimumLifecycleCycles)
	}
	if lifecycleStats.churnCycles.Load() < minimumLifecycleCycles {
		t.Errorf("churn cycles=%d, want at least %d", lifecycleStats.churnCycles.Load(), minimumLifecycleCycles)
	}

	runBounded(t, soakRegistrationCount, 64, func(index int) error {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer closeCancel()
		return registrations[index].Close(closeCtx)
	})
	for _, selector := range selectors {
		waitForTypedSnapshot(t, context.Background(), selector, 0, 0)
	}
	waitForSnapshot(t, expirySelector, 0, 0)
	waitForSnapshot(t, churnSelector, 0, 0)

	closeCtx, closeCancel := context.WithTimeout(context.Background(), time.Minute)
	for _, selector := range selectors {
		if closeErr := selector.Close(closeCtx); closeErr != nil {
			t.Error(closeErr)
		}
	}
	if closeErr := expirySelector.Close(closeCtx); closeErr != nil {
		t.Error(closeErr)
	}
	if closeErr := churnSelector.Close(closeCtx); closeErr != nil {
		t.Error(closeErr)
	}
	if closeErr := client.Close(closeCtx); closeErr != nil {
		t.Error(closeErr)
	}
	if closeErr := transport.Close(); closeErr != nil {
		t.Error(closeErr)
	}
	closeCancel()
	errorWatchers.Wait()
	cleanupZone(t, raw, zone)
	if err := raw.Close(); err != nil {
		t.Error(err)
	}
	closed = true

	close(runtimeStop)
	<-runtimeDone
	runtime.GC()
	time.Sleep(500 * time.Millisecond)
	runtime.GC()
	process := runtimeMonitor.finish()
	// Production owns exactly one synchronization goroutine per Registration.
	// The 500-Registration qualification is intentionally atypical for one
	// process, but its ceiling still catches duplicate workers or leaks while
	// leaving headroom for Redis transport, test producers, and Selector sync.
	if process.PeakGoroutines > process.InitialGoroutines+soakRegistrationCount+128 {
		t.Errorf(
			"goroutine peak exceeded one-worker-per-Registration topology: initial=%d peak=%d",
			process.InitialGoroutines,
			process.PeakGoroutines,
		)
	}
	if process.PeakGoroutines < process.InitialGoroutines+soakRegistrationCount {
		t.Errorf(
			"goroutine peak did not account for every Registration worker: initial=%d peak=%d",
			process.InitialGoroutines,
			process.PeakGoroutines,
		)
	}
	if process.FinalGoroutines > process.InitialGoroutines+8 {
		t.Errorf(
			"goroutines did not return near baseline: initial=%d final=%d",
			process.InitialGoroutines,
			process.FinalGoroutines,
		)
	}

	result := soakResult{
		DurationSeconds:       duration,
		UpdateElapsed:         updates.elapsed,
		Registrations:         soakRegistrationCount,
		Selectors:             fanout,
		Updates:               len(updates.latencies),
		UpdateRetries:         updates.retries,
		UpdateLatency:         updateStats,
		ScheduleLag:           lagStats,
		SelectionTransactions: selection.result.transactions,
		SelectionMutations:    selection.result.mutations,
		SelectionRetries:      selection.result.retries,
		SelectionLatency:      selectionStats,
		ExpiryCycles:          lifecycleStats.expiryCycles.Load(),
		ExpiryRegistrations:   lifecycleStats.expiryRegistrations.Load(),
		PeakRetainedRecords:   lifecycleStats.peakRetained.Load(),
		ChurnCycles:           lifecycleStats.churnCycles.Load(),
		ChurnRegistrations:    lifecycleStats.churnRegistrations.Load(),
		ExpectedAsyncErrors:   errorCollector.expected.Load(),
		UnexpectedAsyncErrors: errorCollector.snapshotUnexpected(),
		FinalRevision:         finalRevision,
		FinalGeneration:       finalGeneration,
		Process:               process,
	}
	encoded, marshalErr := json.Marshal(result)
	if marshalErr != nil {
		t.Fatal(marshalErr)
	}
	t.Logf("SOAK_RESULT %s", encoded)
	if len(result.UnexpectedAsyncErrors) != 0 {
		t.Fatalf("unexpected asynchronous errors: %v", result.UnexpectedAsyncErrors)
	}
}

func soakDuration(t *testing.T) int {
	t.Helper()
	value := strings.TrimSpace(os.Getenv("VERDANDI_SOAK_SECONDS"))
	if value == "" {
		return 120
	}
	seconds, err := strconv.Atoi(value)
	if err != nil || seconds < 30 || seconds > 86_400 {
		t.Fatalf("VERDANDI_SOAK_SECONDS = %q, want 30..86400", value)
	}
	return seconds
}

func soakLifecycleInterval(t *testing.T) time.Duration {
	t.Helper()
	value := strings.TrimSpace(os.Getenv("VERDANDI_SOAK_LIFECYCLE_INTERVAL"))
	if value == "" {
		return 5 * time.Minute
	}
	interval, err := time.ParseDuration(value)
	if err != nil || interval < 10*time.Second || interval > time.Hour {
		t.Fatalf("VERDANDI_SOAK_LIFECYCLE_INTERVAL = %q, want 10s..1h", value)
	}
	return interval
}

func runSoakSelections(
	ctx context.Context,
	selectors []*Selector[soakAttr, soakData],
	seconds int,
) (soakSelectionResult, error) {
	attempts := seconds * soakSelectionRate
	workers := make([]soakSelectionWorkerResult, len(selectors))
	start := time.Now().Add(250 * time.Millisecond)
	var wait sync.WaitGroup
	for index, selector := range selectors {
		wait.Add(1)
		go func(index int, selector *Selector[soakAttr, soakData]) {
			defer wait.Done()
			worker := &workers[index]
			worker.latencies = make([]time.Duration, 0, attempts+attempts/10)
			for attempt := 0; ; attempt++ {
				target := start.Add(time.Duration(attempt) * time.Second / soakSelectionRate)
				if err := waitUntil(ctx, target); err != nil {
					if errors.Is(ctx.Err(), context.Canceled) {
						return
					}
					worker.err = err
					return
				}
				started := time.Now()
				operation, cancel := context.WithTimeout(ctx, 250*time.Millisecond)
				mutations, err := runSoakSelection(operation, selector, attempt)
				cancel()
				if err != nil {
					if errors.Is(ctx.Err(), context.Canceled) {
						return
					}
					if isSoakTransient(err) {
						worker.retries++
						continue
					}
					worker.err = err
					return
				}
				worker.latencies = append(worker.latencies, time.Since(started))
				worker.transactions++
				worker.mutations += int64(mutations)
			}
		}(index, selector)
	}
	wait.Wait()

	result := soakSelectionResult{perSelector: make([]int64, len(workers))}
	for index, worker := range workers {
		if worker.err != nil {
			return soakSelectionResult{}, fmt.Errorf("selector[%d] policy loop: %w", index, worker.err)
		}
		result.latencies = append(result.latencies, worker.latencies...)
		result.transactions += worker.transactions
		result.mutations += worker.mutations
		result.retries += worker.retries
		result.perSelector[index] = worker.mutations
	}
	if len(result.latencies) == 0 {
		return soakSelectionResult{}, fmt.Errorf("no Selector policy transaction succeeded")
	}
	return result, nil
}

func runSoakSelection(
	ctx context.Context,
	selector *Selector[soakAttr, soakData],
	attempt int,
) (int, error) {
	if attempt%soakSelectionRate != soakSelectionRate-1 {
		_, selected, err := selector.One(
			ctx,
			func(candidates Candidates[soakAttr, soakData]) (Candidate[soakAttr, soakData], bool, error) {
				first, _ := lowestSoakCandidates(candidates)
				if first < 0 {
					return Candidate[soakAttr, soakData]{}, false, nil
				}
				if err := candidates.Mutate(first, incrementSoakPower); err != nil {
					return Candidate[soakAttr, soakData]{}, false, err
				}
				return candidates[first], true, nil
			},
		)
		if err != nil {
			return 0, err
		}
		if !selected {
			return 0, protocolError(CodeUnavailable, "selector", 0)
		}
		return 1, nil
	}

	selected, err := selector.Any(
		ctx,
		func(candidates Candidates[soakAttr, soakData]) ([]Candidate[soakAttr, soakData], error) {
			first, second := lowestSoakCandidates(candidates)
			if first < 0 || second < 0 {
				return nil, nil
			}
			if err := candidates.Mutate(first, incrementSoakPower); err != nil {
				return nil, err
			}
			if err := candidates.Mutate(second, incrementSoakPower); err != nil {
				return nil, err
			}
			return []Candidate[soakAttr, soakData]{candidates[first], candidates[second]}, nil
		},
	)
	if err != nil {
		return 0, err
	}
	if len(selected) != 2 {
		return 0, protocolError(CodeUnavailable, "selector", 0)
	}
	return 2, nil
}

func lowestSoakCandidates(candidates Candidates[soakAttr, soakData]) (int, int) {
	first, second := -1, -1
	for index := range candidates {
		if first < 0 || candidates[index].Data.Power < candidates[first].Data.Power {
			second = first
			first = index
			continue
		}
		if second < 0 || candidates[index].Data.Power < candidates[second].Data.Power {
			second = index
		}
	}
	return first, second
}

func incrementSoakPower(data *soakData) error {
	data.Power++
	return nil
}

func runSoakCadencedUpdates(
	ctx context.Context,
	raw *redis.Client,
	redisStart time.Time,
	registrations []*Registration[soakAttr, soakData],
	seconds int,
	payload string,
) ([]time.Duration, []time.Duration, int64, time.Duration, error) {
	count := len(registrations) * seconds
	latencies := make([]time.Duration, count)
	lags := make([]time.Duration, count)
	runStarted := time.Now()
	var retries atomic.Int64
	for round := range seconds {
		redisTarget := redisStart.Add(time.Duration(round) * time.Second)
		if err := waitForSoakRedisTime(ctx, raw, redisTarget); err != nil {
			return nil, nil, retries.Load(), time.Since(runStarted), err
		}
		roundStart := time.Now()
		for index, registration := range registrations {
			target := roundStart.Add(
				time.Duration(index) * time.Second / time.Duration(len(registrations)),
			)
			if err := waitUntil(ctx, target); err != nil {
				return nil, nil, retries.Load(), time.Since(runStarted), err
			}
			operationStarted := time.Now()
			slot := round*len(registrations) + index
			if operationStarted.After(target) {
				lags[slot] = operationStarted.Sub(target)
			}
			if err := updateSoakRegistration(
				ctx,
				registration,
				round+1,
				payload,
				&retries,
			); err != nil {
				return nil, nil, retries.Load(), time.Since(runStarted), err
			}
			latencies[slot] = time.Since(operationStarted)
		}
	}
	if err := waitForSoakRedisTime(
		ctx,
		raw,
		redisStart.Add(time.Duration(seconds)*time.Second),
	); err != nil {
		return nil, nil, retries.Load(), time.Since(runStarted), err
	}
	return latencies, lags, retries.Load(), time.Since(runStarted), nil
}

func waitForSoakRedisTime(ctx context.Context, raw *redis.Client, target time.Time) error {
	for {
		current, err := readSoakRedisTime(ctx, raw)
		if err != nil {
			return err
		}
		remaining := target.Sub(current)
		if remaining <= 0 {
			return nil
		}
		if remaining > 100*time.Millisecond {
			remaining = 100 * time.Millisecond
		}
		if err := waitForInterval(ctx, remaining); err != nil {
			return err
		}
	}
}

func readSoakRedisTime(ctx context.Context, raw *redis.Client) (time.Time, error) {
	deadline := time.Now().Add(45 * time.Second)
	for {
		operation, cancel := context.WithTimeout(ctx, 5*time.Second)
		current, err := raw.Time(operation).Result()
		cancel()
		if err == nil {
			return current, nil
		}
		if ctx.Err() != nil {
			return time.Time{}, ctx.Err()
		}
		if time.Now().After(deadline) {
			return time.Time{}, fmt.Errorf("read Redis time: %w", err)
		}
		if err := waitForRetry(ctx); err != nil {
			return time.Time{}, err
		}
	}
}

func updateSoakRegistration(
	ctx context.Context,
	registration *Registration[soakAttr, soakData],
	value int,
	payload string,
	retries *atomic.Int64,
) error {
	deadline := time.Now().Add(45 * time.Second)
	for {
		operation, cancel := context.WithTimeout(ctx, 5*time.Second)
		err := registration.Update(operation, soakData{Load: value, Payload: payload})
		cancel()
		if err == nil {
			return nil
		}
		if !isSoakTransient(err) {
			return err
		}
		retries.Add(1)
		if IsCode(err, CodeAmbiguous) {
			if renewErr := renewSoakRegistration(ctx, registration, deadline, retries); renewErr == nil {
				return nil
			} else if !isSoakTransient(renewErr) {
				return renewErr
			}
		}
		if time.Now().After(deadline) {
			return err
		}
		if err := waitForRetry(ctx); err != nil {
			return err
		}
	}
}

func renewSoakRegistration(
	ctx context.Context,
	registration *Registration[soakAttr, soakData],
	deadline time.Time,
	retries *atomic.Int64,
) error {
	for {
		operation, cancel := context.WithTimeout(ctx, 5*time.Second)
		err := registration.Renew(operation)
		cancel()
		if err == nil {
			return nil
		}
		if !isSoakTransient(err) || time.Now().After(deadline) {
			return err
		}
		retries.Add(1)
		if err := waitForRetry(ctx); err != nil {
			return err
		}
	}
}

func runSoakExpiryCycles(
	ctx context.Context,
	client *Client,
	selector *RawSelector,
	interval time.Duration,
	stats *soakLifecycleStats,
) error {
	cycle := 0
	for {
		if err := runSoakExpiryCycle(ctx, client, selector, cycle, stats); err != nil {
			if errors.Is(err, context.Canceled) {
				return nil
			}
			return err
		}
		cycle++
		if err := waitForInterval(ctx, interval); err != nil {
			return nil
		}
	}
}

func runSoakExpiryCycle(
	ctx context.Context,
	client *Client,
	selector *RawSelector,
	cycle int,
	stats *soakLifecycleStats,
) error {
	// Keep the synthetic lease longer than the three-second injected pause so
	// the Selector can observe at least one authoritative active state before
	// the test asks it to validate retained expiry.
	const ttl = 10_000
	payload := []byte(strings.Repeat("e", 128))
	for index := range soakExpiryBurst {
		uuid := fmt.Sprintf("%032x", uint64(cycle+1)<<16|uint64(index+1))
		_, err := callSoakRegistration(
			ctx,
			client,
			registrationScriptRegister,
			"expiry",
			uuid,
			registerArguments(uuid, 1, ttl, 1, Fields{"class": []byte("ephemeral")}, Fields{"payload": payload}),
		)
		if err != nil {
			return fmt.Errorf("expiry Register: %w", err)
		}
	}
	if err := waitForSoakSnapshot(ctx, selector, soakExpiryBurst, false); err != nil {
		return err
	}
	if err := waitForSoakSnapshot(ctx, selector, 0, true); err != nil {
		return err
	}
	snapshot, err := selector.Snapshot()
	if err != nil {
		return err
	}
	retained := int64(len(snapshot.Retained))
	if retained == 0 || retained >= soakExpiryBurst {
		return fmt.Errorf("retained bound inactive: retained=%d burst=%d", retained, soakExpiryBurst)
	}
	for {
		previous := stats.peakRetained.Load()
		if retained <= previous || stats.peakRetained.CompareAndSwap(previous, retained) {
			break
		}
	}
	if err := waitForSoakRetainedEmpty(ctx, selector); err != nil {
		return err
	}
	stats.expiryCycles.Add(1)
	stats.expiryRegistrations.Add(soakExpiryBurst)
	return nil
}

func runSoakChurnCycles(
	ctx context.Context,
	client *Client,
	selector *RawSelector,
	interval time.Duration,
	stats *soakLifecycleStats,
) error {
	cycle := 0
	for {
		if err := runSoakChurnCycle(ctx, client, selector, cycle, stats); err != nil {
			if errors.Is(err, context.Canceled) {
				return nil
			}
			return err
		}
		cycle++
		if err := waitForInterval(ctx, interval); err != nil {
			return nil
		}
	}
}

func runSoakChurnCycle(
	ctx context.Context,
	client *Client,
	selector *RawSelector,
	cycle int,
	stats *soakLifecycleStats,
) error {
	const count = 16
	uuids := make([]string, count)
	defer func() {
		cleanupCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		for _, uuid := range uuids {
			if uuid != "" {
				_, _ = callRawRegistration(
					cleanupCtx,
					client,
					registrationScriptUnregister,
					"churn",
					uuid,
					unregisterArguments(uuid),
				)
			}
		}
	}()
	for index := range count {
		uuid := fmt.Sprintf("%032x", uint64(1)<<63|uint64(cycle+1)<<16|uint64(index+1))
		uuids[index] = uuid
		_, err := callSoakRegistration(
			ctx,
			client,
			registrationScriptRegister,
			"churn",
			uuid,
			registerArguments(uuid, 1, 30_000, 1, Fields{"class": []byte("churn")}, Fields{"value": []byte("registered")}),
		)
		if err != nil {
			return fmt.Errorf("churn Register: %w", err)
		}
	}
	if err := waitForSoakSnapshot(ctx, selector, count, false); err != nil {
		return err
	}
	for _, uuid := range uuids {
		_, err := callSoakRegistration(
			ctx,
			client,
			registrationScriptUpdate,
			"churn",
			uuid,
			updateArguments(uuid, 2, false, 1, Fields{"value": []byte("updated")}),
		)
		if IsCode(err, CodeMissing) || IsCode(err, CodeTransition) {
			_, err = callSoakRegistration(
				ctx,
				client,
				registrationScriptRegister,
				"churn",
				uuid,
				registerArguments(uuid, 2, 30_000, 1, Fields{"class": []byte("churn")}, Fields{"value": []byte("updated")}),
			)
		}
		if err != nil {
			return fmt.Errorf("churn Update: %w", err)
		}
	}
	if err := waitForSoakRevision(ctx, selector, count, 2); err != nil {
		return err
	}
	for _, uuid := range uuids {
		_, err := callSoakRegistration(
			ctx,
			client,
			registrationScriptUnregister,
			"churn",
			uuid,
			unregisterArguments(uuid),
		)
		if err != nil {
			return fmt.Errorf("churn Unregister: %w", err)
		}
	}
	if err := waitForSoakSnapshot(ctx, selector, 0, false); err != nil {
		return err
	}
	snapshot, err := selector.Snapshot()
	if err != nil {
		return err
	}
	if retained := len(snapshot.Retained); retained != 0 {
		return fmt.Errorf("explicit Unregister retained %d records", retained)
	}
	stats.churnCycles.Add(1)
	stats.churnRegistrations.Add(count)
	return nil
}

func callSoakRegistration(
	ctx context.Context,
	client *Client,
	kind registrationScriptKind,
	typeName string,
	uuid string,
	arguments []any,
) (registrationReply, error) {
	deadline := time.Now().Add(45 * time.Second)
	for {
		operation, cancel := context.WithTimeout(ctx, 5*time.Second)
		result, err := callRawRegistration(operation, client, kind, typeName, uuid, arguments)
		cancel()
		if err == nil {
			return result, nil
		}
		if !isSoakTransient(err) || time.Now().After(deadline) {
			return registrationReply{}, err
		}
		if err := waitForRetry(ctx); err != nil {
			return registrationReply{}, err
		}
	}
}

func waitForTypedSnapshot(
	t *testing.T,
	ctx context.Context,
	selector *Selector[soakAttr, soakData],
	count int,
	revision uint64,
) {
	t.Helper()
	deadline := time.Now().Add(45 * time.Second)
	for time.Now().Before(deadline) {
		operation, cancel := context.WithTimeout(ctx, 5*time.Second)
		snapshot, err := selector.Snapshot(operation)
		cancel()
		if err == nil && snapshot.Synchronized && len(snapshot.Candidates) == count {
			matched := true
			if revision != 0 {
				for _, candidate := range snapshot.Candidates {
					if candidate.Meta.Revision != revision {
						matched = false
						break
					}
				}
			}
			if matched {
				return
			}
		} else if err != nil && !isSoakTransient(err) {
			t.Fatalf("typed snapshot: %v", err)
		}
		if err := waitForRetry(ctx); err != nil {
			t.Fatal(err)
		}
	}
	t.Fatalf("typed snapshot did not converge to count=%d revision=%d", count, revision)
}

func waitForSoakSnapshot(ctx context.Context, selector *RawSelector, count int, requireRetained bool) error {
	deadline := time.Now().Add(45 * time.Second)
	for time.Now().Before(deadline) {
		snapshot, snapshotErr := selector.Snapshot()
		if snapshotErr == nil && snapshot.Synchronized && len(snapshot.Records) == count && (!requireRetained || len(snapshot.Retained) > 0) {
			return nil
		}
		if err := waitForRetry(ctx); err != nil {
			return err
		}
	}
	snapshot, _ := selector.Snapshot()
	return fmt.Errorf("snapshot did not converge to active=%d retained_required=%v: %#v", count, requireRetained, snapshot)
}

func waitForSoakRevision(ctx context.Context, selector *RawSelector, count int, revision uint64) error {
	deadline := time.Now().Add(45 * time.Second)
	for time.Now().Before(deadline) {
		snapshot, snapshotErr := selector.Snapshot()
		if snapshotErr == nil && snapshot.Synchronized && len(snapshot.Records) == count {
			matched := true
			for _, record := range snapshot.Records {
				if record.Meta.Revision != revision {
					matched = false
					break
				}
			}
			if matched {
				return nil
			}
		}
		if err := waitForRetry(ctx); err != nil {
			return err
		}
	}
	return fmt.Errorf("snapshot did not converge to count=%d revision=%d", count, revision)
}

func waitForSoakRetainedEmpty(ctx context.Context, selector *RawSelector) error {
	deadline := time.Now().Add(45 * time.Second)
	for time.Now().Before(deadline) {
		snapshot, snapshotErr := selector.Snapshot()
		if snapshotErr == nil && snapshot.Synchronized && len(snapshot.Records) == 0 && len(snapshot.Retained) == 0 {
			return nil
		}
		if err := waitForRetry(ctx); err != nil {
			return err
		}
	}
	snapshot, _ := selector.Snapshot()
	return fmt.Errorf("retained view did not drain: %#v", snapshot)
}

func waitUntil(ctx context.Context, target time.Time) error {
	delay := time.Until(target)
	if delay <= 0 {
		return nil
	}
	timer := time.NewTimer(delay)
	defer stopTimer(timer)
	select {
	case <-timer.C:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func waitForInterval(ctx context.Context, interval time.Duration) error {
	timer := time.NewTimer(interval)
	defer stopTimer(timer)
	select {
	case <-timer.C:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func waitForRetry(ctx context.Context) error {
	timer := time.NewTimer(100 * time.Millisecond)
	defer stopTimer(timer)
	select {
	case <-timer.C:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func isSoakTransient(err error) bool {
	return IsCode(err, CodeAmbiguous) ||
		IsCode(err, CodeUnavailable) ||
		IsCode(err, CodeDeadline)
}

func watchSoakErrors(
	wait *sync.WaitGroup,
	owner string,
	stream <-chan error,
	collector *soakErrorCollector,
) {
	wait.Add(1)
	go func() {
		defer wait.Done()
		for err := range stream {
			if err == nil || errors.Is(err, context.Canceled) {
				continue
			}
			if isSoakTransient(err) {
				collector.expected.Add(1)
				continue
			}
			collector.addUnexpected(fmt.Sprintf("%s: %v", owner, err))
		}
	}()
}

func (collector *soakErrorCollector) addUnexpected(value string) {
	collector.mu.Lock()
	defer collector.mu.Unlock()
	if len(collector.unexpected) < 32 {
		collector.unexpected = append(collector.unexpected, value)
	}
}

func (collector *soakErrorCollector) snapshotUnexpected() []string {
	collector.mu.Lock()
	defer collector.mu.Unlock()
	if len(collector.unexpected) == 0 {
		return []string{}
	}
	return append([]string(nil), collector.unexpected...)
}

func newSoakRuntimeMonitor(initialGoroutines int, initialHeap uint64) *soakRuntimeMonitor {
	return &soakRuntimeMonitor{result: soakProcessStats{
		InitialGoroutines: initialGoroutines,
		PeakGoroutines:    initialGoroutines,
		InitialHeapBytes:  initialHeap,
		PeakHeapBytes:     initialHeap,
	}}
}

func (monitor *soakRuntimeMonitor) run(stop <-chan struct{}, done chan<- struct{}) {
	defer close(done)
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()
	monitor.sample()
	for {
		select {
		case <-ticker.C:
			monitor.sample()
		case <-stop:
			monitor.sample()
			return
		}
	}
}

func (monitor *soakRuntimeMonitor) sample() {
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	goroutines := runtime.NumGoroutine()
	monitor.mu.Lock()
	defer monitor.mu.Unlock()
	monitor.result.Samples++
	if goroutines > monitor.result.PeakGoroutines {
		monitor.result.PeakGoroutines = goroutines
	}
	if memory.HeapAlloc > monitor.result.PeakHeapBytes {
		monitor.result.PeakHeapBytes = memory.HeapAlloc
	}
	if memory.HeapObjects > monitor.result.PeakHeapObjects {
		monitor.result.PeakHeapObjects = memory.HeapObjects
	}
}

func (monitor *soakRuntimeMonitor) finish() soakProcessStats {
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	monitor.mu.Lock()
	defer monitor.mu.Unlock()
	monitor.result.FinalGoroutines = runtime.NumGoroutine()
	monitor.result.FinalHeapBytes = memory.HeapAlloc
	return monitor.result
}

func summarizeSoakDurations(values []time.Duration) soakDurationStats {
	var total time.Duration
	zero := 0
	for _, value := range values {
		total += value
		if value == 0 {
			zero++
		}
	}
	sort.Slice(values, func(left int, right int) bool { return values[left] < values[right] })
	percentile := func(value int) time.Duration {
		index := (len(values)*value + 99) / 100
		if index > 0 {
			index--
		}
		return values[index]
	}
	return soakDurationStats{
		Count:       len(values),
		P50:         percentile(50),
		P95:         percentile(95),
		P99:         percentile(99),
		Maximum:     values[len(values)-1],
		Average:     total / time.Duration(len(values)),
		ZeroSamples: zero,
	}
}
