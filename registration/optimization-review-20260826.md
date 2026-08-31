# Registration/Selector Production Optimization Review

Date: 2026-08-26

Historical note: performance numbers and scores apply to the frozen fingerprint
below. Per-Registration sole-writer ownership remains current, but the
256-request queue was replaced on 2026-08-28 by a single-slot Fields merge
mailbox; that newer source requires its own future performance qualification.

## 1. Outcome and scope

This review optimized the current Registration-owned Go and Rust Selector hot
paths without changing the public API, Redis key layout, Lua ABI, lifecycle
semantics, or Catalog implementation. The frozen qualification fingerprint is
`feb767345b8b09323d53dea9c3ead5427be21ece7de668c55e9577eedf5173b0`
over 88 Registration/Selector and qualification source files.

The accepted changes are:

- skip overlay reconciliation when a transaction still holds the same immutable
  published Selector view;
- materialize the UUID ordering as shared record references instead of a second
  list of copied UUIDs followed by one Hash-map lookup per candidate;
- reuse transaction-local generation marks for `Any` duplicate detection instead
  of constructing a map or ordered set for every call; and
- add ordering, shared-record, duplicate-selection, and generation-wrap
  regressions around those invariants.

The four generated Lua programs were audited but deliberately left byte-for-byte
unchanged. They were already the accepted positional, operation-specific atomic
glue; moving SDK validation back into Lua or adding cached symbolic constants
would increase work on Redis's single execution thread without improving the
contract.

No Catalog source or test was changed or executed by this qualification. No
commit, tag, package publication, or push was created.

## 2. Accepted implementation changes

### 2.1 Go

`selectionTransaction` now remembers the immutable `selectorView` that produced
its overlays. Consecutive `One` or `Any` calls against the same view compare one
pointer and retain the already reconciled overlay map. A newly published view
still performs the complete UUID/revision reconciliation before the callback can
observe it, so the optimization cannot expose stale overlay state.

Published views now contain `orderedRecords []*selectorRecord` and
`orderedRetained []retainedSelectorRecord`. Publication sorts the shared record
pointers once. Selection and snapshots walk those arrays directly rather than
walking sorted UUID strings and looking every UUID up in the authoritative map.
The map remains the source for event application, repair, and identity lookup;
the ordered arrays are immutable read indexes over the same records.

`Any` now uses a lazily allocated `[]uint64` mark array indexed by candidate
position. One transaction token marks every selected position. Advancing the
token makes all previous marks logically empty in O(1); the extremely rare
`uint64` wrap clears the complete backing array before returning to token one.
This removes the per-call duplicate-detection map while preserving caller order,
duplicate rejection, retry semantics, and detached returned values.

### 2.2 Rust

Rust applies the same invariants using language-native ownership. `SelectorView`
stores ordered `Arc<SelectorRecord>` values and ordered retained records;
`materialize_view` constructs those indexes from the authoritative maps. The
change removes one owned UUID copy per sorted active record and one map lookup
per visited candidate while keeping the record payload shared.

`SelectionState` retains the last `Arc<SelectorView>` and uses `Arc::ptr_eq` to
skip reconciliation only when the immutable view is identical. Its reusable
`Vec<u64>` generation marks replace the per-call `BTreeSet` used by `Any`.
Token wrap explicitly clears the vector, so the optimization has a tested answer
even for a state that would require 2^64-1 successful generations in ordinary
execution.

### 2.3 Lua

Lua remains four specialized programs: Register, Update, Renew, and Unregister.
Each program performs only the Redis-owned atomic work: derive Redis time and the
deadline, mutate the Registration Hash and Registry membership, increment the
Registry revision where required, apply field expiry, publish the compact binary
event, and return the positional reply.

The SDK still owns field naming, field/count/byte limits, type encoding, and
reply/event decoding. Redis still performs the final `2^46-1` absolute-deadline
guard because the authoritative timestamp is available only inside the atomic
operation. This is the intended glue-layer boundary, not omitted validation.

## 3. Measured Go hot paths

The Selector comparison used Go 1.26.4 on WSL/Linux, an Intel i7-13700F,
`-benchmem`, ten one-second samples, and `benchstat`. Baseline and final samples
were taken in the same source review and environment.

| Benchmark | Baseline median | Final median | Change | Significance |
| --- | ---: | ---: | ---: | ---: |
| publish 500-record immutable view | 105.68 us | 54.08 us | -48.83% | p=0.000, n=10 |
| typed `One` over 500 candidates | 26.54 us | 15.92 us | -40.02% | p=0.000, n=10 |

Publication memory fell from 35,624 to 31,528 bytes/op (-11.50%, p=0.000).
Its six allocations/op did not change. Typed `One` remains 3,880 bytes/op and
43 allocations/op: the optimization removed candidate traversal and overlay
reconciliation work, but the strongly typed codec and detached result still
own their decoded values.

The new `Any` benchmark selected eight of 500 candidates in a final median of
19.69 us, 14,721 bytes/op, and 154 allocations/op. There is no clean pre-change
latency claim for this row because its initial sample was contaminated by host
load; only the final value and the structural removal of the duplicate set are
accepted evidence.

Five additional one-second final samples measured:

| Hot path | Final range | Bytes/op | Allocations/op |
| --- | ---: | ---: | ---: |
| decode one Registration event | 880.5-899.8 ns | 896 | 12 |
| coalesce 32 pending Updates | 5.950-6.014 us | 112 | 1 |
| drain one pending Update | 154.2-159.5 ns | 112 | 1 |
| validate default maximum record | 1.107-1.146 us | 0 | 0 |
| RedisClock upper-bound read | 23.37-23.67 ns | 0 | 0 |
| apply visible Update | 1.353-1.451 us | 2,888 | 5 |
| apply visible Renew | 1.243-1.453 us | 2,888 | 5 |
| no-repair pending check | 19.14-19.22 ns | 0 | 0 |

CPU profiles taken before the change identified repeated view reconciliation and
UUID map access as dominant cumulative work. The accepted optimization therefore
removes measured work rather than speculatively pooling general objects.

## 4. Lua performance evidence

The Lua bytes are unchanged from the accepted 21-pair Redis 8.8 line audit, so
the source-identical server-time measurements remain applicable:

| Operation | Payload | Redis execution time | Sequential throughput |
| --- | --- | ---: | ---: |
| Register | 2 Attr + 2 Data | 8.69 us | 49,877.36/s |
| Register | 16 Attr + 32 Data, 128 B each | 35.57 us | 7,364.20/s |
| Update | one Data field | 8.43 us | 49,803.82/s |
| Update | Version + one Data field | 8.73 us | 49,802.33/s |
| Update | 32 Data fields | 19.74 us | 15,813.84/s |
| Renew | timestamp/TTL only | 7.82 us | 60,092.47/s |
| Unregister | existing record | 3.72 us | 93,566.38/s |

At the target 500 one-field Updates/s, 8.43 us of Redis execution per Update is
about 4.22 ms of single-thread execution per wall-clock second, before unrelated
Redis commands. Network round-trip latency dominates the live client measurement
on the tested LAN.

## 5. Current-source verification

The exact 88-file fingerprint passed:

- deterministic Registration Lua generation and the Redis 8.8 atomic contract;
- Go unit tests and vet;
- Go Linux race detection with ten shuffled repetitions;
- a 30-second Go decoder fuzz run with 7,658,786 executions, seven newly
  interesting in-cache inputs, and the two reviewed persistent corpus cases;
- Rust's 28 Registration unit tests, format check, strict Clippy, and rustdoc;
- targeted ordering, pointer-sharing, duplicate-selection, and token-wrap
  regressions in both SDKs; and
- authenticated Redis 8.8 AOF and Sentinel failure qualification.

The 90-second AOF fault gate used 500 live Registrations and eight Selectors. It
completed 45,000 Updates with no Update retry, 7,137 selection transactions,
7,851 local mutations, three expiry cycles covering 384 Registrations, and nine
churn cycles covering 144 Registrations. Update p50/p95/p99 were
0.702/1.227/1.701 ms; selection p50/p95/p99 were 0.187/0.482/1.094 ms.

`SCRIPT FLUSH`, Pub/Sub connection loss, a three-second Redis pause, ordinary
connection loss, AOF restart, and a second `SCRIPT FLUSH` all passed. The final
Registry revision was 91, Selector generation was three, unexpected asynchronous
errors were zero, Redis stable-window memory changed by -170,648 bytes, there
were no evictions or rejected connections, and cleanup left `DBSIZE=0`.

The independent three-Redis/three-Sentinel matrix then passed acknowledged-write
loss, complete Sentinel outage, primary loss, Sentinel restart, a second
promotion, script-cache loss, and cross-language convergence. Go and Rust both
preserved their process UUID and advanced Selector generations `1 -> 2 -> 3`.

The exact fingerprint subsequently passed the formal two-hour qualification:
an 8,000-second workload, 7,759.124 measured Redis seconds against a 7,200-second
floor, 4,000,000 Updates, 639,743 selection transactions, 34/34 injected faults,
zero unexpected asynchronous errors, stable Redis memory, all Lua/Rust
post-checks, and final `DBSIZE=0`. The complete regression and the retained
rejected preflights are documented in
[`full-regression-2h-20260827.md`](full-regression-2h-20260827.md).

After the gates, the remote host had zero `verdandi.test` containers, the owned
soak directory was absent, and ports 16431, 16381-16383, and 26381-26383 were all
closed.

## 6. Separate engineering scores

These are scoped open-source engineering scores for the implemented
Registration/Selector slice, not a promise that Redis, the network, or an
application callback can never fail.

### 6.1 Lua: 10.0/10

| Area | Score | Assessment |
| --- | ---: | --- |
| atomic correctness | 2.0/2.0 | one operation-specific script owns every coupled Redis mutation and publish |
| hot-path efficiency | 2.0/2.0 | positional ABI, cached SHA execution, measured line audit, no redundant full reads or validation loops |
| protocol boundaries | 2.0/2.0 | Redis-only time/deadline work stays server-side; codecs and deployment limits stay SDK-side |
| maintainability | 2.0/2.0 | shared fragments generate four deterministic specialized scripts and byte-identical SDK copies |
| evidence | 2.0/2.0 | paired microbenchmarks, contract matrix, AOF faults, Sentinel faults, and copy verification |

Why a scoped 10 is justified: no known Lua defect or accepted responsibility is
left unimplemented, and every attempted remaining line optimization either made
the script slower, merely moved work, or weakened clarity/correctness. It does
not claim that Lua is universally ideal. The protocol intentionally requires
Redis 8 field TTL, EVALSHA still occupies Redis's execution thread, Pub/Sub is
non-durable, and a cold or flushed script cache requires one reload. Direct
clients granted write ACLs can also bypass SDK validation; ACL deployment and
client correctness are explicit system boundaries.

### 6.2 Go: 9.8/10

| Area | Score | Assessment |
| --- | ---: | --- |
| correctness | 2.0/2.0 | lifecycle, fail-closed synchronization, immutable views, retained TTL, and recovery invariants are covered |
| performance | 1.9/2.0 | measured view publication and `One` improved 48.83% and 40.02%; core merge/clock/validation paths remain small |
| concurrency | 2.0/2.0 | one queue/worker/timer per published Registration, bounded coalescing, one persistent plus one temporary Selector worker, race-clean |
| API and maintainability | 1.9/2.0 | strong typed/raw APIs share one core and preserve delayed readiness; internal bridge and large Selector core add complexity |
| evidence | 2.0/2.0 | unit, vet, race, fuzz, statistical benchmark, Redis 8, AOF, expiry/churn, cleanup, and Sentinel evidence |

Principal strengths:

- typed `Attr`/`Data` and raw `Fields` converge on one protocol implementation;
- Update accumulation is Registration-local and can coalesce without coupling
  unrelated Registrations;
- immutable published views make reads predictable and allow the identity fast
  path without weakening synchronization;
- selection callbacks borrow a stable view, while returned candidates are
  detached from SDK-owned mutable state; and
- current Linux measurements quantify the accepted optimizations rather than
  relying only on code inspection.

Remaining deductions:

- typed `One` still allocates 43 times and 3.88 KiB; typed `Any(8)` allocates 154
  times and 14.7 KiB because codec decoding and detached outputs require
  ownership;
- Go documentation exposes two inferred private pointer type parameters on the
  generic constructors even though callers write only `[Attr, Data]`;
- the root Client capability bridge uses an internal `sync.Map`; it is outside
  steady-state Update/Renew/selection but increases lifecycle complexity;
- `selector_core.go` remains a large state-machine file, so later protocol
  growth should split logical ownership rather than add more cross-cutting
  branches; and
- callback cancellation is cooperative: Verdandi rejects a late result but
  cannot forcibly terminate arbitrary user code.

### 6.3 Rust: 9.7/10

| Area | Score | Assessment |
| --- | ---: | --- |
| correctness | 2.0/2.0 | ownership, process UUID, reconciliation, retained state, and token-wrap behavior are explicit and tested |
| performance | 1.8/2.0 | ordered shared `Arc` records, identical-view skipping, and reusable marks remove structural work, but allocator deltas are not yet benchmarked |
| concurrency | 2.0/2.0 | Tokio task ownership matches the one-writer and persistent/temporary Selector topology; shutdown joins owned work |
| API and maintainability | 1.9/2.0 | borrowed `CandidateRef`, detached results, static field codecs, and no unsafe code are idiomatic; the Selector module is still large |
| evidence | 2.0/2.0 | unit, format, strict lint, documentation, live Redis, cross-language, AOF post-check, and two-promotion Sentinel evidence |

Principal strengths:

- `Arc::ptr_eq` gives an exact O(1) immutable-view identity test without exposing
  pointer mechanics in the public API;
- ordered `Arc<SelectorRecord>` values avoid a second owned UUID list and
  candidate lookup while preserving shared payload ownership;
- `CandidateRef` makes the callback's borrowed lifetime explicit and detached
  selected records make post-callback use safe;
- the per-Registration Tokio task owns admission order, renewal reset, and
  shutdown; and
- strict Clippy and absence of `unsafe` narrow the class of ownership defects.

Remaining deductions:

- there is no Criterion plus allocator-count benchmark comparable to Go's
  `benchstat` evidence, so the Rust hot-path improvement is structurally proven
  and behavior-tested but not assigned a percentage;
- cloning the authoritative `HashMap<String, Arc<SelectorRecord>>` during
  publication still clones its String keys even though record payloads are
  shared;
- `selector.rs` is 2,359 lines and combines synchronization, view publication,
  selection, retention, and tests; further feature growth should split private
  modules along those ownership boundaries; and
- current exact-fingerprint high-load writing is Go-led. Rust passed live typed
  convergence and Sentinel mutation/recovery, while its 500-Registration release
  load belongs to the immediately preceding source layout.

## 7. Shared trade-offs not charged to one language

- Pub/Sub is a low-latency hint, not a durable log. Gap detection and complete
  synchronization remain mandatory.
- Sentinel replication is asynchronous. An acknowledged write can disappear
  after promotion; a still-running Registration republishes complete state.
- local `power` mutation is a prediction, not a distributed reservation. Exact
  global admission requires a different coordination system.
- the retained view deliberately preserves non-explicitly deleted records for
  one additional TTL; this improves transient-outage behavior at a bounded
  memory and staleness cost.
- the exact-source two-hour gate is Go-writer-led. Rust passed the same live
  post-run raw/typed convergence and complete Sentinel recovery, but a separate
  Rust-led two-hour writer run would increase Rust-specific endurance evidence.

## 8. Evidence

- [`registration-production-optimization-review-20260826.json`](../testkit/results/registration-production-optimization-review-20260826.json)
- [`registration-production-optimization-final-90s-20260826.json`](../testkit/results/registration-production-optimization-final-90s-20260826.json)
- [`registration-production-optimization-final-90s-20260826-samples.jsonl`](../testkit/results/registration-production-optimization-final-90s-20260826-samples.jsonl)
- [`registration-production-optimization-sentinel-20260826.json`](../testkit/results/registration-production-optimization-sentinel-20260826.json)
- [`registration-production-optimization-final-2h-qualified-20260826.json`](../testkit/results/registration-production-optimization-final-2h-qualified-20260826.json)
- [`registration-production-optimization-full-regression-20260827.json`](../testkit/results/registration-production-optimization-full-regression-20260827.json)
- [`full-regression-2h-20260827.md`](full-regression-2h-20260827.md)
- [`lua-optimization.md`](lua-optimization.md)
