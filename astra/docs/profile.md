# C++ Probes and Clock Diagnostics

Implementation: [profile.hpp](../common/include/astra/profile.hpp); offline analysis: [profile.py](../tools/profile.py). Probes locate costs; performance rankings use production builds with probes compiled out. Builds/tests/microbenchmarks/real workloads require current authorization; results belong in [validation.md](validation.md).

## Build and collection

ASTRA_PROFILE defaults OFF. Disabled macros eliminate arguments, timing, TLS, recording state, and diagnostic members directly, without relying on optimizer removal of runtime branches. Public SDK APIs, protocols, lock lifetimes, and scheduling budgets stay unchanged. Instrument handwritten C++ only, not generated/third-party code. Standalone Comet uses the same switch; diagnostic static SDKs retain probes, releases use OFF.

With authorization, build separately without overwriting baselines/downloading tools:

```bash
bash build.sh build --profile release --probes
```

Set environment only for tested children, never user/system-wide. Create a distinct absolute directory beneath project build/ for each run before starting Star/Comet/Pulsar:

| Variable | Default | Boundary |
| --- | --- | --- |
| ASTRA_PROFILE_DIR | Unset: no recording | Existing absolute directory, per-process <pid>.profile; reject existing names |
| ASTRA_PROFILE_GROUPS | all | Comma-separated groups below; reject empty values/items, unknowns, duplicates, whitespace, or all mixed with names |
| ASTRA_PROFILE_SAMPLE | 64 | Approximate 1/N per-thread outer selected-entry sampling, N=1..65536; descendants inherit decision |
| ASTRA_PROFILE_MIB | 64 | Fixed 1..512 MiB/process file, no growth |
| ASTRA_PROFILE_CPU | 0 | 1 adds thread CPU timing; compare separately from wall-only |
| ASTRA_PROFILE_SECONDS | 0 | From first process probe initialization: 0 capacity-only, 1..86400 maximum collection seconds |

| Group | Scope |
| --- | --- |
| clock | Pulsar kernel reference, Ping/Pong, Star four-timestamp filter/continuous-model publication; excludes hot Clock::now |
| storage | Native state/history/projection/pages and otherwise unclassified Star internals |
| rpc | Catalog/Ephemeris services and Gateway |
| replication | Exchange/Dispatch/Landing/Receiver/Intake/Restore, domain replicas, peer sessions |
| watch | Downstream/Readout/Broadcast/Edition/Pagination preparation/paging/fanout |
| sdk | Comet writers/subscriptions/callbacks/scheduling |
| core | Runtime/common/unclassified sites, including hot Clock::now |
| test | Probe fixture profile.test.* sites |

Each site has one compile-time group. clock,replication selects only those. Unselected parents do not enter TLS stacks; selected children attach to the nearest selected parent or form new sampled roots. Calls still execute across groups; partial recording is not a complete call tree.

Enabled binaries without a directory still pay entry-check cost, unlike compile-time OFF. Invalid configuration/mapping emits fixed diagnostics and stops recording while business continues; fixtures must verify expected files, exits, and completeness. Record no error text, addresses, Keys, UUIDs, payloads, or credentials. No postinitialization fork-inherited collection, runtime environment changes, or probes inside async signal handlers.

Deploy diagnostics only to selected instances with constrained groups/sample/capacity/time. No hot switching, online reading, or remote-control API exists. Expiry stops new records while reserved spans finish; mappings live until exit rather than unmapping addresses used by threads. Collection duration neither terminates in-flight calls nor restarts business/another round. Newly added capabilities require applicable evidence before claiming production overhead targets.

## Probe overhead

- Hash names at compile time. Hot paths write fixed 80-byte records, without formatting, sorting, Protobuf conversion, or heap allocation.
- Unselected groups avoid TLS stacks/diagnostic clocks. Selected unsampled children maintain stacks without timing; configured duration still requires an entry monotonic read. Synchronous children inherit the nearest selected root decision.
- Per-thread xorshift32 uses three shift/XOR sets without devices/shared atomics. It avoids fixed-every-N phase lock against work/wait loops; N=1 records all. It is not cryptographic randomness.
- Reserve 64 slots/thread at a time, 64-byte-aligned blocks, avoiding an atomic per record. Up to 63 unused final slots/thread count against capacity.
- Initialize by preallocating/mapping a fixed disk file. No collector thread, global hot lock, or synchronous flush. mmap still costs first faults, RSS, page cache, and kernel writeback; budget disk/memory.
- Synchronous spans read monotonic time twice; CPU mode adds two CLOCK_THREAD_CPUTIME_ID calls and is separately enabled. Query thread ID at most once/thread.
- Counters read existing O(1), side-effect-free values, never extra ByteSizeLong/directory/tree scans. Compile-time OFF removes argument evaluation; runtime group filtering does not. COW charges page objects, not full malloc/control-block/allocator traffic.
- Capacity never expands. Record directly rejected count missed and stop new collection process-wide, avoiding repeated rejection atomics. missed is not all lost events. Exhaustion/clock/nesting-error reports cannot support normal attribution.
- Duration checks occur at selected entries, values, and async consumption, adding monotonic reads; after any thread sees expiry, others check the stop flag. Capacity/time may truncate later sites in a tree while existing spans finish with valid ancestry; absence at the window edge is not absent execution.

Zero perturbation is not promised. Stack work, layout, and branches can affect caches even for unsampled trees. Choose sample rates by paired measurement rather than asserting fixed nanosecond cost or QPS gains.

## Instrumented boundaries

| Path | Observations |
| --- | --- |
| Comet writing | Publishing/Beaming, completion, poll, renewals, Core scheduling |
| Comet reads | Page callbacks, ready-to-consume, projection accept, Table clone/edit/finish, notification poll |
| Star RPC | Catalog Publish/Renew, Ephemeris Create/Update/Renew/Remove, Watch admission |
| Native state | Candidates, Scene/Origin commits/replay, reads/expiry, local export, remote preparation/install |
| Concurrency | Initial lock, shared-read fallback to maintenance, source waits, outside-domain preparation/reacquisition |
| Replication | Runtime stages, Session receive/consume, Data::prepare, synchronous begin_write, Exchange receive/ACK/repair, Dispatch pages |
| Downstream | Notifications, freeze, advance/pump, synchronous StartWrite submit, Broadcast cache/encoding/page bytes |
| Other C++ | Almanac/Library/Receiver, Pulsar Ping/Pong/admission, Star calibration/Clock publication |

Parent context distinguishes template domains; no dynamic per-Key labels. Some overloads share names, so retain parents when aggregating. Generate site inventory statically without tests:

```bash
python3 -B tools/profile.py --inventory build/results/profile-sites.json
```

Do not time every line/hash/recursive node: probe cost could rival useful work. These C++ probes do not directly cover gRPC/TLS internals, kernel scheduling, Go services, or frozen components. No sample means unobserved, not free. Select appropriate profilers for remaining costs; missing-tool downloads need separate permission.

## Analysis and interpretation

Parse only after owning processes exit normally. Header watermarks count reservations, not completions; no online reads. Empty slots may be unfinished spans from abnormal termination. Analyzer complete means format/capacity/completed-record validity only; fixtures independently preserve exit/cleanup/source/binary SHA-256. Current-workspace site inventories do not prove binary provenance.

```bash
python3 -B tools/profile.py build/results/profile-run --output build/results/profile-report.json
# Optional timeline export can exceed binary size; export after measurement.
python3 -B tools/profile.py build/results/profile-run --output build/results/profile-report.json --trace build/results/profile-trace.json
```

Fixed Linux little-endian 64-bit format: 128-byte header/80-byte records. Writer v2 adds groups/duration and signed values; analyzer also accepts v1 without rewriting evidence. Validate version/capacity/event types/ancestry/threads/time containment; reject truncation, unknown sites, missing parents. Nonzero missed/errors means incomplete/nonzero exit; missing/bad files cannot be ignored. Summarize per process and parent site: observations, mean, P50/P95/P99/P99.9, min/max/sum. Below 10,000 samples mark tails insufficient.

- wall_ns includes synchronous children; own_ns subtracts instrumented direct-child intervals but retains uninstrumented work, preemption, and some overhead. Neither equals CPU; parent/child totals cannot be added as request duration.
- CPU covers the current worker only; stacks do not cross async callbacks. Report ready-to-consume and peer/downstream StartWrite-to-OnWriteDone separately, not subtracted from a function's CPU. Write intervals include gRPC scheduling/callback queues and failures, not pure wire time/peer visibility.
- Counters are observations within sampled trees. Shared decisions correlate tree samples; multiplying by sample rate is not exact total reconstruction, and site count is not functional coverage.
- Same-host steady_clock aligns process timelines, not hosts. No cross-node request correlation IDs were added; nearby timestamps do not prove end-to-end decomposition. Business benchmarks still measure end-to-end latency.

## Clock-offset diagnosis

Prefer clock-only collection on Pulsar/three Stars. Authorized child environments may use GROUPS=clock, SAMPLE=1, CPU=0, MIB=32, SECONDS=1800 with an existing absolute project ASTRA_PROFILE_DIR. This is an example, not execution evidence; inspect actual capacity coverage. Do not change NTP, thresholds, or business clocks for attractive results.

| Prefix | Meaning/units |
| --- | --- |
| clock.kernel.* | status=adjtimex result, flags=timex flags; maxerror_us/esterror_us microseconds; signed offset × offset_unit_ns gives ns; elapsed_ns BOOTTIME, unix_ns wall Unix ns, window_ns brackets query/time |
| clock.source.* | available=raw sample, accepted=continuous clock accepted; exception separately marks time/calibration/Provider failure |
| clock.server.* | Pulse rejection, local ready/uncertainty_ns without trusted observation, successful response error budget |
| clock.filter.* | Four timestamps, precision, roundtrip/processing, estimated network RTT/combined error in ns; t0/t3 Star BOOTTIME, t1/t2 Pulsar Unix |
| clock.model.* | Initialization, input error, observation-consumption age, target/model Unix, signed debt_ns, slew_ppm; positive debt needs acceleration, negative deceleration |
| clock.client.* | Received count in eight probes, final gRPC code, content validation, filter/model acceptance; RPC success separate from synchronization acceptance |

Rejections are fixed integers, not formatted errors. rejected=0 means only that layer accepted; later layers may reject.

| Prefix | Nonzero rejected |
| --- | --- |
| clock.kernel | 1 query/unsynchronized/invalid error; 2 invalid sample window/Unix coordinate; 3 raw error/residual offset over limit; 4 combined error over limit |
| clock.server | 1 no trusted receive clock; 2 changing quality/reversed window/unavailable precision; 3 response preparation exception; 4 invalid request coordinate |
| clock.filter | 1 invalid coordinates/precision/quality; 2 roundtrip/processing over 200 ms; 3 processing exceeds roundtrip plus quantization tolerance; 4 coordinate mapping overflow |
| clock.model | 1 invalid/duplicate/reversed observation or failed model; 2 aged observation/extrapolation overflow; 3 first-anchor error over 500 ms; 4 local advancement failure |

Kernel records follow the bracketing window; Star T0/T3 remain adjacent to actual send/receive. Presend Pulse records affect RTT and model records extend publication locks; no zero-perturbation claim. clock excludes frequent business reads, avoiding lease-path diagnostic floods.

After process exit/evidence collection export raw clock values:

```bash
python3 -B tools/profile.py build/results/profile-clock --output build/results/profile-clock-report.json --clock build/results/profile-clock.jsonl
```

JSONL preserves PID/TID, diagnostic monotonic observed_ns, parent span/scope, field/value. Correlate by (process, span). Thread blocks make file order nonglobal; sort timelines by observed_ns. Same-host monotonic axes align; cross-host causality needs independent alignment, not sorting alone.

Investigate in order:

1. Linearly growing maxerror_us with stable offset/esterror_us and kernel rejection 3/4: inspect Chrony effective updates/conservative error growth. maxerror is a bound, not measured wall skew; smaller esterror cannot replace it.
2. Significant neighboring Δunix_ns − Δelapsed_ns changes: inspect wall-versus-BOOTTIME movement, sample windows, suspension, Chrony. This is not absolute error against independent time and cannot exclude joint drift.
3. Stable kernel quality with elapsed_ns/rtt_ns/model age_ns spikes: inspect scheduling/network/consumption. processing_ns separates server work; full RTT is not network-only.
4. model.initialized=0/rejection 3 differs from initialized-node debt/quality. Smooth catch-up after calibration is not missing first anchor. t1−t0 includes Unix/BOOTTIME epoch difference, never physical skew.

## Verification method

cpp_profile checks enabled/disabled binaries: nested trees, idempotent early finish, exception unwinding, four threads, cross-thread timestamps, disabled argument elision/no probe strings, missing directories, invalid configuration, CPU, sampling, alternating-phase coverage, capacity. Python cases cover bad headers/records/parents/empty slots/statistics.

Added cases cover single/multiple groups, unselected parents, invalid settings, duration, signed/int64 bounds, v1/v2, and clock JSONL. As recorded on 2026-09-27, implementation/cases/static review were complete but these additions had not been built/run or newly sampled, and Chrony was unchanged. Later execution status belongs in [validation](validation.md); this historical boundary is not current authorization.

With current permission, build ordinary/probe Release separately and regress, then star_profile_on_test bench / star_profile_off_test bench, checking equal-work checksums, sample counts, and loop wall cost. Microbenchmarks do not replace paired three-Star runs:

1. Compile OFF / enabled-not-recording / wall 1:64 / wall 1:256; compare CPU mode separately from rankings.
2. Equal resources, all Stars writing/replicating, multiple subscribers; Catalog 2048 B and Ephemeris separately. Same source/optimization/contracts/settings, interleaved paired order, at least four pairs.
3. Throughput, receipt/visibility P99/P99.9, CPU, peak RSS, disk/capacity, samples/loss, warmup/formal windows. Ordinary end-to-end results are baseline.
4. Provisional lightweight targets: about ≤5% throughput and ≤10% P99 impact, considering paired variability. Otherwise lower sampling/narrow groups and report honestly. Noise-sized changes are not certain benefits.

Old all-group measurements do not validate later filters/duration/clock changes. Targets remain unproven for all loads. Keep results/variation/direction in validation; reruns require permission.

## Weak page caching and deadline indexing

[Broadcast](../star/src/broadcast.hpp) retains weak_ptr per page. Nonoverlapping consumers can rebuild: a lifetime tradeoff, not an unprotected cache race. Shared Protobuf objects do not guarantee shared final gRPC wire encoding.

Downstream returns encoded charges after write completion; Edition held accounts frozen sources only. Replacing weak with strong references would retain bytes after accounting release, including rejected candidates. Strong caching needs its own budgets/eviction/rejection/cancellation semantics; Limits is not an RSS cap.

rebuilds() and star.broadcast.rebuild/rebuilt_bytes distinguish first construction from successful reconstruction after release. Weak control blocks distinguish empty/expired slots; failed construction is not success. rebuilt_bytes is wire length, not heap size/CPU savings. See validation for executed-case scope.

SDK deadline indexing remains a measurement candidate. [Core](../comet/cpp/src/core.cpp) uses steady_clock; [Agenda](../star/src/agenda.hpp) uses Unix/10 ms ticks. Transplanting it must handle infinity, rounding, long pauses, cross-thread unlink/destruction, and lifetime. Agenda::next is next tick; advance still cascades/processes expiry, not O(1) overall. First measure directory_items/ready_items/polled_items to establish scan cost.

<a id="performance"></a>

## Evidence boundaries for optimization

Attribute candidates under real workloads, changing one core mechanism at a time:

| Scope | Measure first | Preserve |
| --- | --- | --- |
| SDK | Directory/ready scans, callback queues, version queries, encoding/workspace | Prompt cancellation, stale completion isolation, no failure replay, budgets/lifetimes |
| Downstream | Exact/full-Scope fanout, pending collection/progress/pages/index locks | Continuous cursors, atomic installs, per-stream backpressure/bounded slow readers |
| Dynamic domains | Source preparation/final locks, large TTL steps/recovery peaks | Continuous ACK, watermarks/ownership, stable old views/full rollback |
| RPC/replication | prepare/StartWrite sync cost, packets, in-flight waits, coverage scans | ACK complete prefixes; transport batching is not transactions |
| Polaris | WAL/checkpoints/read pools/snapshot cache/interleaved Scopes/history trims | Durable-before-ACK, Scope +1, FULL durability |
| Astrolabe | Large/slow directory freshness, worker reuse/backoff | Bounded concurrency, stale-instance rejection, explicit stale/unknown |
| Build/capacity | Deferred Planet defaults, static dependencies, allocation/cache working sets | Link declarations are not actual artifact contents; no implicit upgrades |

Astrolabe removed a separate 64-node cap, but four workers/two-second deadlines mean a fully slow sweep can take ceil(N/4) × 2s; a five-second ticker does not promise directory-wide freshness. Mesh n(n−1)/2 streams/fanout remain despite shared pages.

Bidi writes, Arena/PMR, strong caches, RCU/Actor, container replacement, LTO/PGO need independent evidence. Unary already has failure/cancellation contracts; streaming adds correlation/backpressure/recovery. atomic shared_ptr need not be lock-free, Arena need not eliminate every allocation, ByteBuffer is not end-to-end zero-copy.

Fix source, three-Star local writes, total resources/application load; report throughput/tails/CPU/RSS/FD/threads/replication/control costs. Short same-VM results do not imply physical-machine scale; old APIs do not prove new gains. Validation indexes data/raw evidence.

## Further acceptance

Existing cases cover native state, faults, RPC, persistence, three-Star propagation, and SDK lifecycles; see [acceptance](comet.md). Systematic cross-machine partitions/reconnects, large sources, real sleep/clock steps, power/disk-full combinations, browser/GPU, and long resource evidence remain.

Prioritize affected regression, then select random models, malformed-page/sequence properties, and allocation profiling by problem. Fixed interleavings do not prove all concurrency; no coverage percentages without collection. Follow [C++ coding](coding.md#cpp)/[authorization](../AGENTS.md); document cleanup starts no tests/builds.
