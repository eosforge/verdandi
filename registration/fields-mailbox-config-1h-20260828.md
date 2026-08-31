# Registration/Selector Fields Mailbox One-Hour Qualification

Date: 2026-08-28

The generated configuration layer mentioned below was removed on 2026-08-29.
This report remains frozen historical qualification evidence and does not
describe the current configuration maintenance model.

## Result

The accepted one-hour qualification passed. The frozen 104-file
Registration/Selector qualification scope completed the full 3,600-second
workload, all 16 injected standalone faults, pre- and post-workload Lua/Rust
convergence checks, and the post-workload three-Redis/three-Sentinel matrix.
The owned standalone database was empty at completion, and every remote test
container, port, and temporary directory was removed.

This result supersedes the earlier same-day attempt that completed its Go
workload but was rejected when a stale Rust test configuration failed the
post-workload gate. Rejected evidence remains on disk and is not counted as a
pass.

## Frozen qualification identity

| Item | Accepted value |
| --- | --- |
| Run ID | `b6af4e4f` |
| Source files | 104 |
| Source SHA-256 | `38448c747230a72eb4d0b1a4ea838b83467a2b8d66d366909bbb1b73b6dd8f77` |
| Workload duration | 3,600 seconds |
| Measured Redis TIME | 3,608.788 seconds |
| Complete suite elapsed time | 3,650.336 seconds |
| Redis | Open Source 8.8.0 |
| Persistence | AOF `everysec` on an owned bind directory |
| Registrations | 500 |
| Selectors | 8 |
| Result | PASS |

The fingerprint covers the generated and source Lua programs, root Go/Rust
transport surfaces used by Registration, both Registration/Selector SDK
implementations and tests, the Sentinel peers and harness, and the soak
harness. The exact path list is embedded in the accepted JSON result.

## Corrections made before the accepted run

### Keep the qualification gate aligned with generated configuration

The shared configuration DSL requires
`selector.clock_refresh_interval_ms >= 1000`. Several old integration and peer
fixtures still requested 100 ms. The first formal workload therefore ran for
one hour successfully but its Rust post-check rejected the invalid fixture
configuration. All affected Go/Rust Registration integration, Sentinel peer,
and interoperability peer fixtures now use one second.

The soak harness now runs the canonical Lua contract, Rust raw convergence,
and Rust typed Registration/Selector checks both before and after the long
workload. An invalid qualification configuration can no longer consume an hour
before first being detected. The harness also requires an empty owned database
at the end.

### Bound Go Pub/Sub reconnect time as part of Selector fail-closed behavior

Sentinel preflight exposed that go-redis `PubSub.ReceiveTimeout` bounds the
socket read, but a read error can enter a synchronous driver reconnect using
the supplied context. Passing the Selector's long-lived owner context therefore
allowed driver backoff to delay the point at which the local view became
unavailable.

The Go Selector now derives a per-receive context with the same calculated
receive deadline. That bound covers both the read and driver reconnect work.
When connectivity is lost, the persistent Selector goroutine regains control
on time and can fail the view closed. This adds no goroutine and does not change
the accepted one persistent listener plus at most one temporary synchronization
goroutine ownership model.

### Qualification-source accuracy

The source fingerprint no longer names a removed Go file and now automatically
includes root Go production files plus the Rust generated configuration,
identifier, and Redis modules used by the tested path. The accepted scope
contains 104 files.

## Workload results

The workload scheduled exactly 500 logical Registration Updates per second for
3,600 seconds. It completed all 1,800,000 scheduled Updates. The approximately
487.993 Updates per elapsed wall second value obtained by dividing by the
3,688.581-second update-drain interval includes injected stalls and final
drain; it does not mean requests were omitted. Similarly, Redis instantaneous
operations per second includes Lua, Hash, Pub/Sub, Selector, monitoring, and
recovery commands and must not be read as a Registration-update rate.

### Registration Update

| Measurement | Result |
| --- | ---: |
| Completed Updates | 1,800,000 |
| Retries | 4 |
| Average latency | 0.627669 ms |
| p50 latency | 0.604057 ms |
| p95 latency | 0.841814 ms |
| p99 latency | 1.135046 ms |
| Maximum latency | 3.125363 s |
| p50 schedule lag | 0.486463 ms |
| p95 schedule lag | 1.026495 ms |
| p99 schedule lag | 1.114831 ms |
| Maximum schedule lag | 3.123875 s |

The two approximately 3.12-second maxima coincide with deliberate three-second
Redis pauses. They are fault-recovery observations, not steady-state tail
latency.

### Selector and lifecycle work

| Measurement | Result |
| --- | ---: |
| Selection transactions | 294,982 |
| Selection mutations | 324,483 |
| Selection retries | 90 |
| Average selection latency | 0.210125 ms |
| p50 selection latency | 0.182851 ms |
| p95 selection latency | 0.317371 ms |
| p99 selection latency | 0.915748 ms |
| Maximum selection latency | 5.402946 ms |
| Expiry cycles / Registrations | 12 / 1,536 |
| Churn cycles / Registrations | 13 / 208 |
| Peak retained records | 33 |
| Expected asynchronous fault errors | 85 |
| Unexpected asynchronous errors | 0 |
| Final content revision | 3,601 |
| Final connection generation | 7 |

## Injected standalone faults

All 16 scheduled faults passed:

| Fault | Scheduled seconds | Count | Result |
| --- | --- | ---: | --- |
| Redis `SCRIPT FLUSH` | 300, 600, 900, 1500, 2100, 2400, 3300 | 7 | PASS |
| Kill Pub/Sub clients | 750, 1350, 1950, 2550, 3150 | 5 | PASS |
| Pause Redis for 3 seconds | 1200, 3000 | 2 | PASS |
| Restart Redis with AOF | 1800 | 1 | PASS |
| Kill ordinary clients | 2700 | 1 | PASS |

The workload recovered from script-cache loss, Pub/Sub generation loss,
ordinary connection loss, bounded server stalls, and a persisted Redis restart
without an unexpected asynchronous error.

## Process and Redis resource evidence

| Measurement | Result |
| --- | ---: |
| Go goroutines, initial / peak / final | 2 / 530 / 2 |
| Go heap bytes, initial / peak / final | 624,288 / 97,597,488 / 33,637,208 |
| Peak Go heap objects | 349,027 |
| Process samples | 125 |
| Redis samples / stable samples | 122 / 119 |
| Early / late Redis memory median | 2,547,384 / 2,371,712 bytes |
| Redis median memory growth | -175,672 bytes |
| Allowed memory-growth gate | 2,097,152 bytes |
| Redis used-memory peak | 3,019,296 bytes |
| Redis RSS peak | 25,202,688 bytes |
| Connected-client peak | 16 |
| Blocked-client peak | 0 |
| Instantaneous operation peak | 4,665/s |
| Evictions / rejected connections | 0 / 0 |
| Redis sample failures | 0 |
| Final owned database size | 0 |

Returning from 530 goroutines to two is strong lifecycle evidence. The final
heap value is an ordinary runtime sample without a forced full GC, so this run
does not claim that it is a standalone proof of zero live-object retention.
The negative Redis median-memory trend passes the configured leak gate. The
high peak fragmentation ratio in the raw result is expected to be noisy at
this very small Redis dataset and is not used as an acceptance gate.

## Cross-language and Sentinel evidence

Before and after the one-hour workload, all of these checks passed:

- canonical generated Registration Lua and Selector bootstrap contract;
- Rust raw Registration/Selector convergence on Redis 8.8;
- Rust typed Registration and transactional Selector behavior.

The post-workload Sentinel suite then passed in 32.734 seconds, with its fault
scenario taking 28.811 seconds. It used three Redis nodes, three Sentinel
nodes, and separate ACL credentials. The elected primary moved
`16381 -> 16383 -> 16382`; both Go and Rust Selector connection generations
moved `1 -> 2 -> 3`; both Registration UUIDs survived recovery. Both SDK views
became explicitly unavailable during total Sentinel loss, then converged after
Sentinel restart and the second promotion. The suite also covered an
acknowledged-write-loss fence, full-state republish, script reload, Pub/Sub
recovery, and cross-language observation.

The Windows Rust linker emitted its localized import-library creation message.
It is a compiler warning independent of strict source linting and did not
change any test outcome.

Final short regression after documentation passed the deterministic
configuration and Registration-Lua generation checks, all Go packages, and
Rust all-target/all-feature tests (52 library plus four endpoint-free external
tests; Redis-dependent cases remained intentionally ignored in this local
command because the accepted campaign had already exercised their isolated
endpoints). The modified Go interoperability peer also passed `go test`; the
Rust interoperability peer passed `cargo check`.

## Rejected evidence retained for audit

Run `1119eacd` completed its 3,600-second Go workload but is deliberately
recorded as failed. Its post-workload Rust raw convergence check rejected the
stale 100 ms clock-refresh fixture as invalid under the generated 1,000 ms
minimum. Its JSON and samples are preserved with the
`postcheck-failed` suffix. They are useful failure evidence but must not be
combined with the accepted metrics.

Short qualification before the accepted run also included a fully passing
90-second smoke campaign (45,000 Updates, 7,406 selections, three expiry
cycles, nine churn cycles, six faults, and an empty database) and a passing
Sentinel preflight. Deliberately rejected shorter trials established that the
harness enforces a minimum lifecycle interval and at least two lifecycle
cycles.

## Assessment

### Lua: 10.0/10 for its accepted scope

Strengths:

- operation-specific generated programs keep the deployed entry points small;
- fixed positional inputs and direct Redis primitives minimize glue overhead;
- atomic register, update, renew, and unregister behavior passed canonical,
  fault, restart, and cross-language checks;
- script-cache flush recovery passed seven times during the accepted hour.

This score is scoped to Lua's intended atomic glue responsibility. SDKs own
validation, codecs, policy, and view interpretation; moving those back into Lua
would increase coupling rather than improve this layer.

### Go: 9.8/10

Strengths:

- one single-slot Fields mailbox merges pending work without retaining caller
  structs, while one worker owns Redis mutation and renewal state;
- the Selector's read/reconnect deadline now gives fail-closed behavior a
  bounded owner-loop handoff even when go-redis reconnects synchronously;
- the accepted hour met the complete 500-Update/s schedule with low p99 Update
  and selection latency and no unexpected asynchronous errors;
- goroutines returned from 530 to two after all Registration and Selector
  shutdown work.

Remaining deductions:

- Registration and Selector lifecycle state machines are necessarily complex
  and remain maintenance-sensitive despite ownership comments and tests;
- typed snapshot publication still has allocation costs at scale;
- the non-forced-GC final heap sample is not allocator-level leak proof.

### Rust: 9.7/10

Strengths:

- Rust follows native Tokio cancellation, task ownership, permit retention,
  and shared immutable payload patterns rather than copying the Go shape;
- raw and typed Redis convergence passed before and after the hour;
- the Rust peer passed both Sentinel promotions, total-loss unavailability,
  UUID preservation, and cross-language recovery;
- strict build/test evidence and both updated interoperability peer compile
  checks pass.

Remaining deductions:

- the sustained 3,600-second load generator in this campaign is Go; Rust was
  qualified around it before/after and through Sentinel, not subjected to a
  separate hour-long sustained load in this result;
- current evidence does not include an allocator-counted statistical Rust
  benchmark campaign;
- the Rust Selector module remains large because it owns synchronization,
  retention, selection, and recovery state.

### Qualification evidence: 9.9/10

The accepted campaign freezes its source list, runs cross-language gates on
both sides of the workload, injects 16 faults, verifies an empty database,
executes two Sentinel promotions, records resource samples, and cleans its
remote fixture. The remaining 0.1 reflects environmental breadth: this is one
Redis 8.8 host/topology and not a multi-OS, multi-kernel, repeated statistical
campaign.

## Evidence files

- Accepted result: `testkit/results/registration-fields-mailbox-config-1h-20260828.json`
- Accepted samples: `testkit/results/registration-fields-mailbox-config-1h-20260828-samples.jsonl`
- Accepted Sentinel result: `testkit/results/registration-fields-mailbox-config-sentinel-20260828.json`
- Sentinel preflight: `testkit/results/registration-fields-mailbox-config-sentinel-preflight-20260828.json`
- Passing 90-second smoke result: `testkit/results/registration-fields-mailbox-config-smoke-20260828.json`
- Rejected first-hour result: `testkit/results/registration-fields-mailbox-config-1h-20260828-postcheck-failed.json`
- Rejected first-hour samples: `testkit/results/registration-fields-mailbox-config-1h-20260828-postcheck-failed-samples.jsonl`

No commit or push was created.
