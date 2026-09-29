# Performance measurement of the current implementation

[English](README.md) | [简体中文](README_CN.md)

`native.cpp`, `almanac.cpp`, and `comet.cpp` measure the current three domains and native Comet C++. `store.cpp` and `push.cpp` are older Store/isolated transport probes, not substitutes for current system results. Actual data/conclusions live only in [validation](../docs/validation.md#performance); this page specifies reproduction.

## Build and execution

Explicit authorization for the current tests is required. Use existing project tools/dependencies only; missing inputs fail without download/install. Use Release; Sanitizer results are not production performance. Run scenarios sequentially, without overlapping compilation, other tests, or another load run.

```bash
bash build.sh configure --profile release --benchmarks
cmake --build build/cmake/release --target star_native_bench star_almanac_bench comet_bench --parallel 1
python3 -B bench/run.py --binaries build/cmake/release --native --cases bench/native.json --output build/results/performance/native
python3 -B bench/run.py --binaries build/cmake/release --almanac --cases bench/native.json --output build/results/performance/almanac
python3 -B bench/run.py --binaries build/cmake/release --cases bench/network.json --output build/results/performance/network
python3 -B bench/run.py --binaries build/cmake/release --cases bench/auth.json --output build/results/performance/auth
python3 -B bench/run.py --binaries build/cmake/release --cases bench/concurrency.json --output build/results/performance/concurrency
```

`star_restore_bench <records-per-remote> <scope-count> <0-or-1>` diagnoses six local writers competing with two remote full recoveries; 0 is the no-recovery control, 1 enables concurrent recovery. `4096 1 1` versus `4096 64 1` keeps total records fixed and changes Scope distribution only. It uses real Catalog/Origin/Scene with fixed business time, without networking, Pulsar, or SDK. Its result is not three-Star system throughput. Ordinary CTest does not run performance probes.

Choose build parallelism from actual resources; the single-job example does not override adaptive project policy. Network cases also require current `star`, `pulsar`, `polaris`, and `astrolabe` artifacts built through the unified entry beforehand; runners do not build. Output directories must not exist. `--repeat=1..5`, default three, selects finite rounds without resident background jobs.

Polaris uses real SQLite files, production WAL/FULL, and prefilled history. Creation/prefill are outside commit timing. From the Astra root, using the project's offline Linux Go environment:

```bash
python3 -B - <<'PY'
import subprocess
from tools import build
env = build.environment(jobs=build.resources()[0])
subprocess.run([str(build.CACHE / 'tools/go-1.27.1/bin/go'), '-C', str(build.ROOT), 'test', './polaris/internal/storage', '-run', '^$', '-bench', '^BenchmarkCommit$', '-benchtime=2s', '-count=3', '-benchmem'], env=env, check=True)
PY
```

## Measurement semantics

- Native microbenchmarks time 20,000 ordinary operations individually and report throughput/nearest-rank p50/p95/p99/p99.9. Record preparation is outside the window. Catalog/Ephemeris use preallocated immutable bodies; Almanac's owning API includes body/key copies. These are not directly comparable pure-algorithm costs across domains. Full recovery has five samples/round, Create count equals records, and a long pause occurs once/round; emphasize duration, not stable tail distributions.
- Clocks/sampling cost time, without estimated subtraction. Submicrosecond values show local magnitude, not equivalent precision guarantees. Native domains use fixed valid business time to exclude TTL expiry. Agenda separately measures empty catch-up after one hour/day/week, without actually suspending the OS.
- `comet.commit` runs from call start to actual write receipt. `comet.visible` runs from the same start until every participating Subscriber/Observer callback confirms correct version/body. Callbacks point-query the supplied View, not a shared reader through reverse polling. Each writer has its own condition variable. Visible latency includes Star replication/push, SDK projection install, application notification, and thread wakeup, not just gRPC transport.
- `rate=0` is closed-loop with 1/8/16/32 writers, at most one request in flight per writer, waiting for all observers before the next write. Completion rate is end-to-end throughput for this workload, **not maximum standalone Star QPS**. Watchers count logical streams, not TCP connections. Each target Star shares one subscribing Client; producers also share one Client.
- `rate>0` assigns planned send times from a common origin; latency includes client delay before initiation. Each writer still has one in-flight request. Backlog is neither server rejection nor requests already received, and this is not an unbounded open-loop packet generator. Exceeding total time budget fails; do not discard backlog to improve tails.
- Network samples create all records, await complete real subscriptions, then allow 200 ms for handshake drain before the default five-second window. Initialization/normal exit are excluded from network throughput; process CPU/RSS traces include initialization. Every write/visibility check must succeed; failure invalidates the sample rather than being skipped. Storage/authentication/replication use real implementations.
- TLS toggles only public business endpoints; internal admission/TLS remain real. Authentication cases install random test credentials through Astrolabe → Polaris, then log in with Comet, never injecting Session into Star memory. Password/SECRET files stay in this run's private temporary directory and are removed afterward.

## Resources and evidence

The runner records artifact SHA-256, CPU/affinity, kernel, actual memory, paging counters, raw quantiles, and per-process RSS/thread/CPU traces. Each network sample starts its own Pulsar, Polaris, required Astrolabe, and 1/2/4 Stars. Completion/failure stops only owned process groups and removes temporary databases; logs/results remain under the chosen `build/` directory. No system settings, dynamic-memory configuration, or global environment are changed.

`stable_memory=false` means total memory changed or system paging counters increased during the sample. Keep raw results but exclude them from stable-sample medians. This flag does not prove freedom from host scheduling, other workloads, frequency changes, or cold-cache effects. RSS sampling is approximately 20 ms/200 ms, not allocation-level auditing. Summed process RSS double-counts shared pages and is not physical resident memory. Report existing swap separately from new paging.

Clients and Stars on one VM establish real protocol behavior/relative scaling cost, not cross-machine networking, production capacity, long-term memory stability, or p99.99 SLA. Uncovered [B01–B14 acceptance branches](../docs/comet.md#performance-and-scale-matrix) remain open after finite benchmarks pass.
