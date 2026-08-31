# Direct Typed Registration/Selector Production Qualification

Date: 2026-08-25

> Historical evidence: this run predates the Client-level Registration
> coordinator and the one-listener/one-temporary-task Selector design. It remains
> valid protocol and fault history, but no longer qualifies current worker
> ownership. See
> [`concurrency-review-20260826.md`](concurrency-review-20260826.md).

## 1. Outcome

The current Go/Rust direct typed Registration and transactional Selector
implementation passed the complete standalone Redis and Sentinel qualification.
The authoritative standalone interval is measured with Redis `TIME`, not the
client or WSL clock.

| Result | Value |
| --- | ---: |
| Overall status | PASS |
| Redis version | 8.8.0 |
| Redis-authoritative elapsed time | 7,608.409 s |
| External harness elapsed time | 7,668.969 s |
| Live Registrations | 500 |
| Selectors | 8 |
| Typed Updates | 4,000,000 |
| Selection transactions | 639,713 |
| Committed local mutations | 703,688 |
| Injected standalone faults | 34/34 passed |
| Unexpected asynchronous errors | 0 |
| Final revision | 8,001 |
| Final Selector generation | 15 |
| Final Redis `DBSIZE` | 0 |

The executable source fingerprint covered 58 Registration/Selector Lua, Go,
Rust, soak, and Sentinel files:

```text
3f7e21dc1c925cd71cd4dd3f74400a70dc4a042ab0f9572d7bd9197d4006e1c7
```

No Catalog implementation or Catalog test endpoint was used. The standalone
fixture used isolated port `36443`; every run-owned standalone and Sentinel
resource was removed afterward. No commit, tag, release, or push was created.

## 2. What was qualified

The main load uses the public direct typed APIs rather than only the raw
`Fields` compatibility layer:

- 500 local `Registration[Attr, Data]` values are constructed with stable
  process-lifetime UUIDs;
- `Register` is delayed until the simulated service is ready;
- each Registration supplies immutable typed Attr and complete typed Data;
- every typed Update re-encodes complete Data while the SDK emits only changed
  Redis Hash fields;
- eight typed Selectors receive the same remote state;
- Selector policy traffic exercises both `One` and `Any`;
- each successful transaction stages local `Power` increments and commits them
  atomically to that Selector's prediction overlay;
- later remote Updates change `Load` without overwriting locally predicted
  `Power`, proving field-granular reconciliation;
- final typed snapshots verify exact remote fields and the exact sum of every
  Selector's committed local mutations; and
- raw compatibility paths remain covered by the expiry/retained and explicit
  churn sub-workloads.

The workload also repeatedly creates Registrations that expire naturally,
enter the non-selectable retained view, reactivate, and expire again. A separate
loop performs explicit Register/Unregister churn, which must purge rather than
retain records.

## 3. Progressive verification before the long run

The production interval was preceded by the following current-source gates:

| Gate | Result |
| --- | --- |
| Go formatting and `go vet` | PASS |
| Go unit tests, five shuffled repetitions | PASS |
| Go integration/load/soak tagged compilation | PASS |
| Go WSL/Linux race detector | PASS |
| Registration event-decoder fuzz, 30 s | PASS, 7,920,411 executions |
| Rust formatter | PASS |
| Rust Clippy, all targets/features, warnings denied | PASS |
| Rust unit and all-target tests | PASS, 42 unit tests |
| Rust documentation tests | PASS |
| Registration and Catalog Lua generation checks | PASS |
| 150 s typed standalone fault preflight | PASS |
| Go/Rust typed three-node Sentinel preflight | PASS |

The 150-second preflight completed 75,000 typed Updates, 11,927 selection
transactions, 13,117 local mutations, five expiry cycles, five explicit churn
cycles, and all six compressed faults. Its result is retained separately and is
not substituted for the production interval.

## 4. Standalone fault matrix

| Fault | Count | Result |
| --- | ---: | --- |
| `SCRIPT FLUSH` | 14 | PASS; operation-specific SHAs reloaded |
| Kill all Verdandi Pub/Sub connections | 11 | PASS; snapshot/revision repair converged |
| Pause Redis for three seconds | 4 | PASS; retries and scheduling recovered |
| Restart Redis with AOF | 3 | PASS; state restored and clients reconciled |
| Kill ordinary command connections | 2 | PASS; pools rebuilt and traffic resumed |

There were 612 expected asynchronous availability diagnostics in injected fault
windows and no unexpected asynchronous error. There were 2,348 Update retries
and 287 selection-transaction retries. Retries are part of the recovery
contract; no timeout was interpreted as exactly-once failure.

Natural expiry completed 27 cycles covering 3,456 temporary Registrations.
Explicit churn completed 27 cycles covering 432 temporary Registrations. The
retained view peaked at 34 records and returned to its expected bound.

## 5. Latency and scheduling

All latency distributions include injected fault windows.

| Metric | p50 | p95 | p99 | Maximum | Average |
| --- | ---: | ---: | ---: | ---: | ---: |
| Typed Update | 0.665 ms | 0.933 ms | 1.239 ms | 3.299 s | 2.218 ms |
| Update schedule lag | 0.569 ms | 1.107 ms | 1.215 ms | 2.300 s | 1.834 ms |
| Selector `One`/`Any` transaction | 0.223 ms | 0.381 ms | 0.623 ms | 14.280 ms | 0.250 ms |

The maximum Update and scheduling values cover the deliberate three-second
Redis pauses and recovery windows. The p99 Update and schedule-lag gates remain
far below one second without removing fault samples.

The Go workload completed exactly 4,000,000 scheduled updates over 8,000 Go
clock seconds. WSL's monotonic clock jumped forward near the end, so the Redis-
authoritative interval saw about 525.8 Updates/s on average rather than exactly
500. This exceeds the target load but is not presented as a precise rate
measurement. Redis `TIME` is the only basis for the two-hour claim.

## 6. Resource behavior

| Metric | Result |
| --- | ---: |
| Stable Redis samples | 253 |
| Early median Redis memory | 2,503,272 B |
| Late median Redis memory | 3,060,000 B |
| Stable memory growth | 556,728 B |
| Memory-growth gate | 2,097,152 B |
| Redis used-memory peak | 10,560,992 B |
| Redis RSS peak | 28,835,840 B |
| Connected-client peak | 252 |
| Blocked-client peak | 0 |
| Evictions | 0 |
| Rejected connections | 0 |
| Go goroutines, initial / peak / final | 2 / 1,553 / 2 |
| Go heap, initial / peak / final | 611,976 / 206,672,352 / 72,640,024 B |

Redis memory remained bounded and passed the 2 MiB stable-growth gate. The
large transient Go goroutine and heap peaks are reproducible during a paused
Redis recovery: 500 independently scheduled owners and eight Selectors become
runnable together. Closing the clients returns goroutines exactly to baseline,
so this is bounded recovery fan-out rather than a goroutine leak. It remains a
real capacity cost and is included in the module's weaknesses.

Redis command connections similarly expand to roughly 250 during a pause and
remain at the connection-pool high-water mark. Killing ordinary connections or
restarting Redis reduces the count before demand grows it again. This is not
unbounded growth, but deployments should explicitly size SDK pools instead of
accepting runtime-dependent defaults blindly.

## 7. Sentinel and cross-language recovery

After standalone cleanup, a fresh three-Redis/three-Sentinel topology passed:

- separate command and Sentinel ACL credentials;
- Go and Rust typed Registration/Selector integration tests;
- minority Sentinel loss and stale Sentinel configuration;
- one deliberately acknowledged write lost with the old primary;
- full desired-state republish by the still-live owners;
- Pub/Sub generation recovery;
- `SCRIPT FLUSH` reload;
- all Sentinels unavailable;
- primary loss while every Sentinel is unavailable;
- Sentinel restart and a second promotion; and
- Go/Rust cross-language convergence.

Both SDKs preserved their Registration UUIDs. Both Selectors observed
generations `1 -> 2 -> 3`. This proves the accepted repair behavior; it does not
turn asynchronous Sentinel replication into consensus or durable history.

## 8. The rejected first attempt

The first formal typed attempt is intentionally preserved as failed evidence:

```text
Redis server elapsed time did not reach the qualification floor:
7138.759s < 7200s
```

Its Go workload passed 7,502 seconds and 3,750,000 Updates, but WSL advanced its
clock by roughly six minutes relative to Redis and the Windows harness. The
Redis-time gate correctly rejected the false two-hour claim. The accepted rerun
raised the client budget to 8,000 seconds while retaining the same hard 7,200-
second Redis floor. WSL jumped again, but the rerun had already accumulated
7,608.409 authoritative Redis seconds.

This is a qualification-environment weakness, not a RedisClock protocol
failure. Future WSL runs must retain a substantial client-time margin and must
never remove the Redis `TIME` floor.

## 9. Selector micro-optimization

The typed Go 500-candidate benchmark initially cloned the destination passed to
the application's `FieldEncoder`, despite the codec contract already forbidding
retention of that buffer. Returning the owned destination directly removes one
redundant copy without weakening public detachment.

Ten-sample WSL/Linux comparison:

| Metric | Before | After | Change |
| --- | ---: | ---: | ---: |
| Median transaction | 21.51 us | 20.53 us | -4.57% |
| Bytes/op | 5,121 B | 3,881 B | -24.21% |
| Allocations/op | 54 | 43 | -20.37% |

`benchstat` reported `p=0.001` for latency and `p=0.000` for bytes and
allocations (`n=10`). The final `b.Loop` reference ranges from 19.945 to 21.726
us/op with a 21.313 us median, 3,881 B/op, and 43 allocations/op. The policy
scans all 500 candidates, stages `Power++`, commits the overlay, and returns a
detached typed result; it performs no Redis I/O.

The remaining CPU profile is dominated by building the borrowed transaction
view. Eliminating that O(N) work would require a materially different indexing
or callback contract and was not hidden behind an unsafe cache optimization.

## 10. Engineering score

These are implementation and evidence scores, not a security certification or
a `1.0.0` release decision.

| Area | Score | Basis |
| --- | ---: | --- |
| Registration Lua atomic glue | 10.0/10 | accepted responsibility is minimal, generated, deterministic, line-audited, and fully regressed |
| Go Registration/Selector | 9.8/10 | direct generics, delayed readiness, measured optimization, race/fuzz/live/long evidence |
| Rust Registration/Selector | 9.6/10 | matching typed lifecycle and transactional API, `Arc` reuse, strict Clippy/live/Sentinel evidence; no dedicated allocator benchmark |
| Synchronization and recovery | 9.8/10 | revisioned authoritative repair, retained TTL, field-granular prediction, repeated standalone/Sentinel recovery |
| Testing and operational evidence | 9.9/10 | 7,608 s Redis-time interval, 4M Updates, fault matrix, source fingerprint, final cleanup and cross-language post-checks |
| Maintainability/open-source readiness | 9.8/10 | narrow public APIs, raw compatibility, generated Lua, explicit ownership and executable evidence |

Overall Registration/Selector slice: **9.8/10**.

Principal strengths:

- application code uses direct strong types without SDK code generation;
- delayed `Register` preserves the service-readiness boundary;
- complete typed Data remains convenient while Redis writes stay field-
  proportional;
- `One`/`Any` callbacks see borrowed state, stage mutations transactionally,
  and fully roll back on errors or invalid results;
- revision/timestamp/TTL and Pub/Sub responsibilities remain explicit;
- Lua stays a small atomic storage adapter rather than a duplicate validator;
- live state, retained state, retry work, decoders, and payload sizes are all
  bounded; and
- machine-readable results make the score independently auditable.

Principal weaknesses and trade-offs:

- Redis Pub/Sub is only a wake-up hint. Reconnects and gaps still require Redis
  reads and sometimes a complete Registration reload.
- A paused backend releases a large bounded recovery wave: 1,553 goroutines,
  about 207 MB Go heap, and roughly 250 Redis connections in this workload.
- Each Selector transaction builds an O(N) borrowed candidate view; the
  500-candidate Go path still allocates 3,881 bytes and 43 objects.
- A slow callback serializes that Selector. The SDK can reject a callback after
  its deadline but cannot safely abort arbitrary synchronous user code.
- Local `Power` prediction is process-local and may diverge across a large
  fleet. More frequent reports and step probing reduce error; exact global load
  balancing requires a different distributed coordination mechanism.
- Typed wire bytes are application-owned. Exact cross-language Attr/Data codec
  vectors must be supplied by each real schema rather than inferred by
  Verdandi.
- Rust lacks a dedicated Criterion/allocator suite comparable to the Go
  benchmark evidence.
- Sentinel may lose an acknowledged write. A live owner repairs desired state;
  a vanished owner cannot, and Redis is not an audit log.
- WSL clock behavior required an external Redis-time floor and larger client
  margin. A client-only duration is not acceptable qualification evidence.
- TLS, managed Redis, WAN loss, deliberate Redis clock steps, larger Selector
  fan-out, sustained maximum-size payloads, and multi-day Registration operation
  remain outside this test matrix.

These limits explain the 9.8 rather than 10.0 overall score. The Lua glue earns
10.0 within its deliberately narrow responsibility; the complete distributed
module retains unavoidable deployment and recovery trade-offs.

## 11. Evidence

- Accepted result:
  `testkit/results/registration-typed-soak-2h-pass-20260825.json`
- Accepted Redis samples:
  `testkit/results/registration-typed-soak-2h-pass-20260825-samples.jsonl`
- Sentinel result:
  `testkit/results/sentinel-typed-soak-pass-20260825.json`
- Rejected clock-short attempt:
  `testkit/results/registration-typed-soak-2h-20260825.json`
- Passing typed preflight:
  `testkit/results/registration-typed-soak-preflight-pass-20260825.json`

Reproduction:

```powershell
$env:VERDANDI_TEST_SSH_PASSWORD = "<temporary test-host password>"
python -B testkit/soak/soak_test.py --host <redis-host> --ssh-user <user> --port 36443 --duration-seconds 8000 --minimum-redis-seconds 7200 --selector-fanout 8 --sample-seconds 30 --lifecycle-interval 5m --run-sentinel --result-file testkit/results/registration-typed-soak-2h.json --sentinel-result-name sentinel-typed-soak.json
```
