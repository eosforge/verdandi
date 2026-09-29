# Shared Redis / Astra application baseline

[English](README.md) | [简体中文](README_CN.md)

The frozen Redis C++ SDK and current Comet C++ use their public interfaces. Shared `workload.hpp` generates, schedules, verifies, and measures identical application load. Adapters run/link separately, without mixing OpenSSL/BoringSSL. Current measurements live only in [validation](../../docs/validation.md#baseline).

## API measurement boundaries

The Catalog adapter uses synchronous update, internal version queries, and no automatic renewal. See [validation](../../docs/validation.md) for build/workload execution; old performance samples do not qualify migrated APIs. Beacon Update renewal, same-ID recovery, and Observer `one(selector)` are implemented in the [SDK](../../comet/cpp/README.md); their new measurement scenarios and performance evidence must be distinguished from SDK implementation. Existing samples retain original API/source identity and cannot establish current performance.

Static Catalog background values must survive initialization, measurement, and drain. The adapter requires explicit TTL of at least `(seconds + 120)` seconds. Short-TTL keepalive belongs in Beacon or a separate expiry case, not hidden refresh traffic.

Keep concurrent writes at three Stars and identical application load, separately accounting for version preparation/repair, synchronous commit, installed visibility, and local selection. Silent Catalog periods do not renew; effective Beacon updates suppress beats. Report actual protocol/network/timer differences. The current adapter polls retained public compatibility views through `observer.select()`. Measuring new `one(selector)`/selection semantics requires a dedicated public-API scenario; do not use private storage or treat old traversal numbers as direct new-selection performance.

Build/workload checks and performance runs have separate records. Do not infer new gains from old measurements. Comet now requires C++23; older Linux C++26 samples retain their original build identity.

## Scope

- Share strict argument parsing, payload generation, complete-view acceptance, thread exception propagation, finite waits, and statistics. Adapters handle public API differences only, without private Stores or raw Redis writes as shortcuts.
- Do not change production protocol, history retention, TTL, acknowledgment, or recovery for benchmarks. Keep the old SDK frozen.
- After all writers finish, reread every subscription's current view and verify completeness, uniqueness, versions/bodies, and fixed Attr. Historical observed watermarks are insufficient. Nonempty background diagnostics fail the sample; timeout/failure never counts as successful throughput.
- Record definite workload failures and continue other predetermined cases, then exit nonzero. Environment, memory-budget, or cleanup failure stops immediately. No indefinite retries or replacement of failure evidence with a successful rerun.
- Independent `cpp_baseline_workload` verifies multiple groups/subscriptions/writers, both completion modes, numeric boundaries, background-error propagation, and rejection of “observed earlier but missing finally.” Real adapters additionally require service scenarios.

## Workload and statistics

`records` is the total, evenly distributed across `groups`. Each group has `fanout` independent Selector/Subscriber/Observer instances, totaling `groups * fanout`. Producer and consumer sides each share `clients` Clients, assigning records/subscriptions round-robin. `writers` threads own disjoint record sequences and cycle through every record, with one in-flight request per thread.

The standard matrix starts with **three Stars writing concurrently**. `stars` defaults to 3. Each producer Client has one fixed Star endpoint by round-robin index; consumers offset by one node. `clients` and `writers` must cover and be divisible by Star count; each Scope's `fanout` must be at least that count, guaranteeing every source writes and every Scope is verified on every node. Multiple endpoints/failover in one Client do not establish load distribution. One Scope measures three-source merge; multiple Scopes measure independent concurrent updates. Ephemeris updates/renews at its registration Star; Catalog sources use disjoint keys.

`cases.json` contains 36 standard three-Star configurations, including receipt cases for both dynamic domains with 24 writers/6 Clients and 24 writers/3 Clients. Catalog also has 2048-byte normal/high-fanout bodies in both completion modes, compared with 128-byte cases of identical topology. 2K means 2 KiB body, not RPC wire length. The probe supports at most 32 writers; parameter rejection is neither performance failure nor zero throughput. One/two-Star diagnostics require explicit `stars` and `diagnostic: true`; they diagnose serialization/protocol boundaries, not standard acceptance. Changed topology/parameters cannot yield a claimed code speedup against old single-Star numbers; rerun both versions with equal topology, parameters, and total resources.

The first sixteen Data bytes encode record number/incrementing version; the rest are fixed. Both adapters use identical body/Attr. The old Fields API wraps an extra `body` field; actual encoding/copy/decoding cost remains, without estimated subtraction.

| Mode | Next-write condition | Latency | Throughput interpretation |
| --- | --- | --- | --- |
| receipt | Server acknowledges this write | commit p50/p95/p99/p99.9 | Application commit completion with multiple subscriptions online |
| visible | Acknowledged and every subscription in the Scope observes this body | commit and visible | Full visibility completion, including sampling/slowest subscription |

Both modes finally verify all latest records. Receipt mode may coalesce intermediate updates and does not report unobserved per-update visibility. Since the old SDK lacks the new callback equivalent, both adapters use one sampling thread polling public views, default 1 ms. Visible latency includes sampling quantization, scan cost, and wakeup; **do not divide it directly by callback-based `bench/comet.cpp` latency**. `sampler.busy_seconds` accumulates scanning wall time across the entire probe lifecycle, not CPU; `/proc` separately measures process CPU.

`rate=0` is fixed-concurrency closed-loop. `rate>0` times from planned sends, including application queueing; exceeding bounded drain fails. Initialization is outside the measurement window. Throughput includes the final receipt/visibility wait; final consistency verification is separate. Five seconds per round is a finite baseline; p99.9 is observational, not a production SLA.

## Explicit differences

- Both use loopback, no business authentication/TLS, and memory-only business commits. Astra really runs Pulsar/Polaris/three Stars with internal TLS/admission/full replication. The frozen SDK uses one Redis with AOF/RDB disabled. This compares application-solution cost under fixed total VM resources, not engines with equal replication guarantees. Report node count and per-node/total resources; summed three-node throughput is not single-node acceleration.
- Old Selector `view_publish_interval=0` removes default 10 ms coalescing. Redis root pools cap at 32 per Client; Comet allows 128 readers per Client. Equal logical Client counts do not imply equal TCP/HTTP2 stream/thread counts; retain resource traces.
- `coalesced.json` is explicitly a single-Star diagnostic, restoring the old 10 ms Selector window for 256 registrations/8 Selectors in both modes. Do not mix it with the three-Star zero-coalescing matrix or interpret 10 ms as a visibility bound. Comet production configuration is unchanged.
- Old Registration updates refresh TTL. Historical Comet Data-update samples did not; current Beacon Update does renew TTL. Preserve that source/API distinction rather than treating old samples as current behavior. Old Catalog lacks dynamic Catalog leases. Short-TTL cases intentionally include real automatic-renewal differences and do not claim equal protocol work.
- Old Catalog Entry reads decode Fields; old Selector candidate transactions execute public projection steps. Comet traverses immutable Views. These are real application API costs, not pure bandwidth.

## Offline build and execution

Obtain authorization for the current build/tests first. Missing caches fail without automatic downloads. Use the same GCC 16.2, Release. The old SDK remains C++23; original Linux baseline artifacts used C++26. Rebuild/validate new Comet C++23 separately, without inheriting old artifact success. From the configured project environment:

```bash
cmake -S . -B build/cmake/release -DASTRA_BUILD_BENCHMARKS=ON
cmake --build build/cmake/release --target baseline_comet baseline_workload_test
cmake -S bench/baseline -B build/baseline-current/redis-release \
  -DASTRA_LEGACY_SDK=/absolute/path/to/verdandi/sdk \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_C_COMPILER="$PWD/build/tools/gcc-16.2.0/bin/gcc" \
  -DCMAKE_CXX_COMPILER="$PWD/build/tools/gcc-16.2.0/bin/g++" \
  -DVERDANDI_DOWNLOAD_CACHE="$PWD/build/deps/common/downloads/fetchcontent" \
  -DVERDANDI_OPENSSL_ROOT="$PWD/build/deps/openssl/linux/x64"
cmake --build build/baseline-current/redis-release --target baseline_redis
python3 -B bench/baseline/run.py \
  --binaries build/cmake/release \
  --legacy build/baseline-current/redis-release/baseline_redis \
  --cases bench/baseline/cases.json --repeat 3 \
  --output build/results/baseline-current/results
```

Choose compilation concurrency from actual available memory. Performance cases overlap neither builds nor other tests. Build Release services through the project entry first. Python uses the standard library/existing project helpers only. C++ follows `.clang-format`.

The runner uses cached `redis:8.8.0` only, with `--pull=never`, loopback listening, persistence disabled, and 512 MiB container limit. Existing Docker access is required; `--sudo` uses already granted `sudo -n`, without modifying global groups. Normal exit/errors/termination stop owned services and remove owned containers/temp databases. Externally managed Redis may be supplied through `--redis` and `--redis-pid`; its owner handles container cleanup. **Use only an instance exclusive to this run: every case executes FLUSHALL.**

Cases alternate execution order and default to three rounds. Raw results contain parameters, artifact digests, kernel/CPU, actual memory, paging, RSS/thread/CPU traces, and failure causes. Memory-growth/paging samples remain but are excluded from stable summaries. `python3 -B bench/baseline/report.py build/results/baseline-current/results/results.json` compares only same-case/same-round pairs where both sides are stable, reporting medians of round quantiles, throughput range, and valid count. Fewer than three pairs remain explicitly fewer, never a fabricated three-round stable conclusion.

Redis reuses this run's exclusive container across the matrix, clearing business data each case while allocator/script caches may remain; Stars restart per case. Compare sample CPU deltas, not cumulative Redis CPU or cross-case HWM as a case peak. RSS is sampled for this case, including initialization. Output directories must not exist. Same-host results establish relative regression baselines, not cross-machine capacity or every production workload.
