# Registration/Selector Complete Regression and Two-Hour Qualification

Completed: 2026-08-27

## 1. Result

The complete Registration/Selector regression passed on the exact 88-file
source fingerprint
`feb767345b8b09323d53dea9c3ead5427be21ece7de668c55e9577eedf5173b0`.
The formal endurance gate ran 8,000 seconds of workload and measured
7,759.124 seconds of Redis server time, exceeding the required 7,200-second
floor.

The run completed 4,000,000 typed Updates across 500 live Registrations while
eight Selectors performed 639,743 `One`/`Any` transactions. All 34 scheduled
faults passed, all post-run Lua/Rust checks passed, Redis memory was stable, the
final database was empty, and the Go process returned from 528 peak goroutines
to its initial two.

No Catalog source, data, container, or port was changed. The test used an
independent source snapshot, Redis container, AOF directory, and port 16440.
No commit or push was created.

## 2. Pre-endurance regression

The preflight passed:

- byte-identical Registration Lua generation and Python harness compilation;
- Go formatting, ten shuffled unit repetitions, vet, and ten shuffled
  WSL/Linux race repetitions;
- a 30-second Go event-decoder fuzz run with 8,063,945 executions and one new
  interesting cache input;
- Rust's 28 Registration unit tests, all test targets compiled, format check,
  strict Clippy, and rustdoc; and
- the complete three-Redis/three-Sentinel cross-language fault matrix.

The same static/unit/lint/documentation gates ran again after the endurance
test. The post-run race gate passed ten more shuffled repetitions. The post-run
30-second fuzz gate completed 6,179,348 executions, found four new interesting
cache inputs, and reported no failure.

## 3. Sentinel gate correction

Two unchanged-source Sentinel attempts reached the second-promotion phase but
timed out while Sentinel continued to report the just-killed promoted primary:
first port 16382, then port 16383. Go and Rust correctly became unavailable and
unsynchronized during both injected total outages. Every owned resource was
cleaned after each rejected attempt.

The failure followed whichever node had just been promoted, not one fixed Redis
node. The harness waited for all Sentinels to agree on the new primary but did
not wait for the sole surviving Redis replica to attach to that primary and
become known to all three Sentinels. Stopping all Sentinels immediately could
therefore persist a topology with no known promotable replica.

The harness now gates the total-outage phase on both:

- the surviving Redis node reporting `ROLE` as a connected replica of the
  promoted primary; and
- all three Sentinel instances reporting that replica through
  `SENTINEL REPLICAS`.

This is state-driven and bounded; it does not add an arbitrary sleep. After the
correction, the full matrix passed in 34.503 seconds:

| Stage | Redis port |
| --- | ---: |
| initial primary | 16381 |
| primary after acknowledged-write loss | 16383 |
| primary after total Sentinel outage | 16382 |

Go and Rust preserved their process UUIDs and both advanced Selector generation
`1 -> 2 -> 3`. Script-cache loss, acknowledged-write loss, full-state
republish, complete Sentinel loss, Sentinel restart, second promotion, and
cross-language convergence all passed.

## 4. Rejected 7,200-second workload attempt

The first endurance attempt deliberately remains failed evidence. It ran the Go
workload for 7,200 seconds and completed:

- 3,600,000 Updates;
- 575,731 selection transactions;
- 34/34 passed injected faults;
- zero unexpected asynchronous errors; and
- goroutines `2 -> 528 -> 2`.

However, its Redis server-time interval was only 6,984.007 seconds after the
three AOF restarts and test timing boundaries, below the explicit 7,200-second
qualification floor. The harness therefore rejected it. The correct formal
configuration, already used by the preceding production qualification, is an
8,000-second workload with a 7,200-second Redis floor.

This failure is recorded in
`testkit/results/registration-production-optimization-2h-floor-failed-20260826.json`
and its JSONL sample file. It is not counted as a passing two-hour gate.

## 5. Formal endurance result

### 5.1 Workload and latency

| Measurement | Result |
| --- | ---: |
| Workload duration | 8,000 seconds |
| Redis server-time floor | 7,200 seconds |
| Measured Redis server time | 7,759.124 seconds |
| Live Registrations / Selectors | 500 / 8 |
| Updates | 4,000,000 |
| Update retries | 4 |
| Update p50 / p95 / p99 | 0.672 / 1.037 / 1.404 ms |
| Update maximum | 3.292 seconds during injected faults |
| Selection transactions | 639,743 |
| Local selection mutations | 703,730 |
| Selection retries | 257 |
| Selection p50 / p95 / p99 | 0.218 / 0.415 / 0.671 ms |
| Selection maximum | 4.353 ms |
| Final Registry revision / Selector generation | 8,001 / 15 |

The four Update retries and 257 selection retries occurred inside expected
fault windows. The SDK reported 202 expected asynchronous errors and zero
unexpected asynchronous errors.

### 5.2 Lifecycle coverage

| Scenario | Result |
| --- | ---: |
| natural-expiry cycles / affected Registrations | 25 / 3,200 |
| churn cycles / affected Registrations | 27 / 432 |
| peak retained records | 33 |
| Redis expired keys / expired fields | 896 / 896 |

Every lifecycle expansion converged back to the 500 live Registration baseline.
The final cleanup removed every Registration, Registry, configuration, and test
key.

### 5.3 Fault coverage

All 34 scheduled faults passed:

- repeated `SCRIPT FLUSH` cache loss;
- repeated Pub/Sub connection kills;
- ordinary command-connection kills;
- four three-second Redis pauses; and
- three AOF-preserving Redis restarts.

Selectors published 15 generations over the run and converged after every
notification gap or reconnect.

### 5.4 Process and Redis resources

| Resource | Initial | Peak | Final / late |
| --- | ---: | ---: | ---: |
| Go goroutines | 2 | 528 | 2 |
| Go heap | 625,160 B | 178,315,192 B | 71,806,472 B |
| Redis used memory median | 2,399,208 B early | 3,699,672 B observed peak | 2,386,984 B late |
| Redis RSS | - | 25,530,368 B | 25,395,200 B |
| Redis clients | 1 before load | 22 | 1 after cleanup |

Redis stable-window memory changed by -12,224 bytes, well inside the 2 MiB
growth gate. Redis reported zero evictions, zero rejected connections, and zero
blocked clients. The Go heap final sample remains above process startup because
the runtime retains capacity after the 500-worker and decoded-view workload; it
is not treated as proof of a leak. Goroutine ownership returned exactly to two,
and Redis memory showed no positive endurance trend.

## 6. Post-run convergence and cleanup

The same live Redis fixture passed, after the 8,000-second workload:

- the canonical Lua atomic contract and generated-copy check;
- Rust raw Registration/Selector synchronization; and
- Rust typed Registration plus transactional Selector synchronization.

The final Redis database size was zero. The owned run was `8d84a2f3`; its
container and `/tmp/verdandi-soak-8d84a2f3` directory were absent and port 16440
was closed after the harness returned. An unrelated independently owned test on
port 36440 remained running and was not inspected beyond its name/port or
modified.

## 7. Assessment impact

The language scores remain:

| Implementation | Score | Two-hour impact |
| --- | ---: | --- |
| Lua | 10.0/10 | exact generated bytes passed 34 faults and the post-run Redis 8 contract |
| Go | 9.8/10 | 4M Updates, 640k selections, race/fuzz, stable goroutine ownership, and full cleanup increase endurance confidence |
| Rust | 9.7/10 | live post-run raw/typed convergence and Sentinel recovery passed, but the sustained writer workload remains Go-led and allocator benchmarks remain absent |

The Sentinel test fix improves qualification determinism, not the SDK score.
The rejected attempts also demonstrate the desired fail-closed behavior: a
stale Sentinel topology never became a false SDK success.

## 8. Evidence

- `testkit/results/registration-production-optimization-full-regression-20260827.json`
- `testkit/results/registration-production-optimization-final-2h-qualified-20260826.json`
- `testkit/results/registration-production-optimization-final-2h-qualified-20260826-samples.jsonl`
- `testkit/results/registration-production-optimization-2h-floor-failed-20260826.json`
- `testkit/results/registration-production-optimization-2h-floor-failed-20260826-samples.jsonl`
- `testkit/results/registration-production-optimization-full-regression-sentinel-20260826.json`
- `testkit/results/registration-production-optimization-full-regression-sentinel-failed-20260826.json`
