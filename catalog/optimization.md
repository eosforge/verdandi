# Catalog v1 production review

Status: implementation and qualification record frozen on 2026-08-27. The
2026-08-28 shared-root transport and generated configurable 512-KiB-default/
4-MiB-maximum record limit supersede the corresponding policy discussion below.
The 2026-08-30 SDK-018 lock removal additionally supersedes this frozen
document's Acquire/Release, Path-lease, Publisher-Close, and six-script claims.
Current behavior is documented in [`api.md`](api.md).
This
document supersedes every Catalog Hash/Stream, Mirror, Compact, and
`Catalog<T>` optimization note. The exact public and Redis contracts are in
[`api.md`](api.md).

## 1. Result

Catalog now implements one coherent model in Lua, Go, and Rust:

- Redis stores one bounded Value, Array, or Map at
  `verdandi:catalog:<zone>:<part>:<id>`;
- Publisher exposes atomic Replace, strict Patch, and Delete;
- Lua is only the atomic state/revision/notification glue;
- Pub/Sub carries the complete accepted operation but is never authoritative;
- Subscriber holds complete raw values in memory and repairs from Redis after
  loss, reconnect, or an inapplicable Patch;
- each path has a stable Entry, and callers choose a different external type on
  every `Load<T>` if needed; and
- optional bbolt/redb/SQLite persistence is a disposable restart checkpoint, not a
  disk-backed working set or reverse synchronization source.

There is no Redis Stream, Mirror, Compact operation, segmented Replace, field
delete, Array hole, or SDK-managed large-value reference in this version.

## 2. Atomic and bounded Lua

Six generated scripts share reviewed fragments and are copied byte-for-byte to
the standalone Lua output and all SDKs:

| Script | Responsibility |
| --- | --- |
| Acquire | validate optional base revision and obtain a token-fenced TTL lease |
| Replace | commit one complete shape, global/field revisions, indexes, and full event |
| Patch | revalidate the exact base and apply only additions/overwrites |
| Delete | remove live data, create a fresh tombstone, prune bounded history, publish |
| Read | return absent/deleted, full replacement state, or exact newer fields |
| Release | remove only the caller's still-matching lease |

All control integers use canonical decimal strings. Revision equality and
ordering stay in that representation instead of repeatedly converting through
Lua numbers; only bounded byte counts need numeric conversion. Sorted-set
scores remain within `1..=2^53-1`, where Redis can represent every revision
exactly. Every script validates its exact `KEYS`/`ARGV` ABI and relevant Redis
key types before mutation. Mutation events are packed by Lua from the state
being committed, so an SDK cannot publish bytes that disagree with Redis.

Array Patch now uses one `HGET` per changed index for both existence and old
size, rather than an `HEXISTS` followed by `HGET`. Delete prunes expired
tombstones first, then only the actual count excess. One call removes at most
256 records. A single `ZMSCORE` obtains all revisions selected for eviction;
batched `ZREM` then removes them. `@floor_revision` advances only to the highest
tombstone revision really evicted.

Lua cannot roll a script back after an exceptional Redis runtime failure such
as out-of-memory. Production Redis therefore needs `maxmemory-policy noeviction`
and enough headroom for the largest accepted Replace plus indexes and Pub/Sub
payload. All predictable contract/capacity failures are validated before the
first write.

## 3. Subscriber recovery

One Subscriber uses one dedicated Pub/Sub connection and one persistent
listener task for all of its exact channels and patterns. Initial alignment and
later repair share at most one temporary task; requests coalesce while it is
active and it exits when idle. Local routing is by the actual channel.
Redundant exact subscriptions are removed when Part or Zone coverage already
includes them.

The safe synchronization order is subscribe, authoritative read, subscribed
PING/PONG fence, metadata recheck, then publish the aligned local state.
Replace and Delete can apply directly. Patch applies only when the Entry's
revision equals `base_revision`; otherwise that path is repaired with the Read
script. A reconnect or malformed/missing notification requests authoritative
repair. Pub/Sub's at-most-once delivery is therefore a latency optimization,
not a correctness dependency.

Broad recovery uses the Zone live/deleted ZSETs. A checkpoint cursor within the
retained floor reads only members with newer scores. A zero/ahead/below-floor
cursor performs a complete index scan. The per-field revision ZSET lets Read
return only fields newer than the local revision unless a later Replace makes a
complete read necessary.

Go and Rust cap concurrent path reads at 32. Notification decoders stream over
MessagePack, validate declared sizes before durable allocation, reject trailing
data, and enforce numeric Array order or lexical Map/Patch order. Go consumes
the Pub/Sub string directly and stores names and values in shared immutable
arenas with capacity-limited value slices. Rust borrows transient header text
from the payload and stores complete fields behind `Arc`, so status-only state
transitions do not clone the complete value. Rust event decoding is now a
separate module instead of being embedded in the Subscriber state machine.

## 4. Measured performance

### 4.1 Go local hot paths

The checked-in Go benchmarks exercise a 512-element Array and a 512-field Map
Replace notification. Ten Windows/amd64 samples before and after the cleanup
show the following median for Array validation:

| Array validation | Time | Heap bytes | Allocations |
| --- | ---: | ---: | ---: |
| Before | 60.6 us | 21,580 B/op | 826 allocs/op |
| Current | 9.54 us | 9,472 B/op | 1 alloc/op |

This is an 84% median-time reduction, 56% fewer allocated bytes, and removal of
825 allocations per validation. The implementation parses canonical decimal
indexes directly instead of sorting strings and rebuilding every index with
`strconv.Itoa`.

For the 512-field notification, durable decode allocations fell from 1,030 to
8. The current decoder reports 91,485 B/op. The former production path also
needed one complete Pub/Sub string-to-byte copy; a forced-copy measurement was
107,864 B/op and 1,031 allocations/op. Direct decoder timing was noisy and
overlapped across runs (roughly 72--101 us current versus 78--90 us for the old
decoder before its required payload copy), so this review claims an allocation
and GC-pressure improvement, not a proven single-core latency improvement.

### 4.2 Redis 8.8 line latency

The current benchmark artifact is
[`catalog-v1-benchmark-20260826.json`](../testkit/results/catalog-v1-benchmark-20260826.json).
It ran 2,000 sequential operations per ordinary scenario against a shared Redis
8.8 endpoint over the local network. Values below are end-to-end wall latency;
`server us/op` comes from Redis command statistics.

| Scenario | Throughput | p50 | p95 | Server us/op |
| --- | ---: | ---: | ---: | ---: |
| Small Replace | 1,863.8/s | 502.1 us | 784.5 us | 74.86 |
| One-field Patch, including size projection | 1,288.2/s | 702.2 us | 1,238.0 us | 76.00 |
| Full Read, 64 fields | 2,336.4/s | 413.8 us | 631.6 us | 70.09 |
| One-field delta Read | 4,005.9/s | 236.0 us | 379.3 us | 27.96 |
| Unchanged Read | 3,909.0/s | 242.7 us | 381.4 us | 23.83 |
| Delete/tombstone refresh | 1,846.6/s | 511.2 us | 783.8 us | 65.15 |
| Replace, 512 fields | 271.1/s | 3,559.8 us | 5,131.6 us | 1,628.64 |

The endpoint had variable concurrent load: an earlier same-day sample was
uniformly 30--50% faster, including scenarios unaffected by this cleanup. The
artifact is therefore a current shared-host latency snapshot, not an isolated
before/after comparison or a concurrent capacity claim. The Lua improvements
above are deterministic command/count reductions; an isolated Redis campaign
is still required to quantify their latency effect.

The expected hot path is small Replace for compact frequently changing values,
or strict Patch plus delta Read for larger Maps/Arrays. Full notification
payloads intentionally trade network bytes for immediate local application and
simple recovery logic.

## 5. Verification completed

Current-source checks completed against the new protocol only:

- deterministic generator check and Redis 8.8 Lua contract;
- strict ABI, type/corruption, tombstone floor, Pub/Sub, and maximum revision
  Lua cases;
- Go unit/property tests, ten shuffled repetitions, goleak shutdown gate, vet,
  60-second decoder fuzzing, WSL/Linux race detection with authenticated Redis,
  remote integration, benchmark, and Catalog-only endurance tests;
- Rust formatter, eight Catalog unit/property tests, strict clippy/rustdoc, two
  authenticated Redis integration tests, bounded decoder checks, shared-state,
  redb monotonicity, and forced Pub/Sub reconnect tests;
- C++ strict GCC, clang-tidy, ASan/UBSan/leak, authenticated Redis 8.8
  Standalone integration, SQLite restart, and isolated plain-Sentinel smoke;
- Value, Array, Map, Replace, strict Patch, Delete, stable Entry, generic typed
  load, field-level repair, full/delta restart alignment, and local checkpoint
  coverage in both SDKs;
- standalone Go/Rust interoperability across six revisions, with both
  languages publishing Replace/Patch/Delete and binary fields;
- a three-node Redis 8.8 Sentinel matrix with two primary promotions, script
  cache loss, final revision/content convergence, and complete cleanup; and
- a new interruptible fault/endurance harness that fingerprints only Catalog
  sources and never invokes Registration tests.

The formal isolated Redis 8.8 AOF run passed on the frozen 72-file fingerprint
`bb994a871b4d9c4679e8ecf800cba1c6d4f37df8ab25a67dfc8d76d161e68cd1`.
Redis `TIME` measured 7,201.265 seconds, independently of the faster WSL clock:

- 960,000 attempts produced 937,468 Patch, 18,768 Replace, and 3,760 Delete
  commits; 959,996 attempts committed directly;
- one transient result and three stale retries occurred only at the deliberate
  AOF restart boundary, then authoritative revision repair converged every
  Subscriber; no unexpected asynchronous error was reported;
- all 18 scheduled faults passed: seven script-cache flushes, four Pub/Sub
  disconnects, four ordinary-connection disconnects, two three-second pauses,
  and one AOF restart;
- mutation p50/p95/p99 were below 2.10/4.20/8.39 ms. The 3.160-second maximum
  covered an injected outage rather than steady-state service time;
- schedule-lag p95/p99 stayed below 2.10 ms; its 4.365-second maximum covered
  injected pause/restart recovery and was subsequently drained;
- stable-window Redis memory grew by only 19,560 bytes, with zero eviction,
  rejected connection, or blocked client; Go goroutines went from 11 through
  a peak of 14 to 2, and heap fell from a 9.62 MB peak to 0.91 MB at shutdown;
  and
- a fresh Subscriber matched every complete value, final `DBSIZE` was zero,
  and the post-run Lua, Rust, and Go/Rust interoperability checks all passed.

This historical two-hour run placed both Subscribers on the same persistent
Client, so both applied every checkpoint update. Its functional and recovery
result remains valid, but its local-disk pressure is intentionally not a
representative deployment measurement. The corrected harness defaults to one
persistent Subscriber and keeps the remaining fanout in memory.

The current Go integration coverage is 74.5% of statements. Fuzzing, property
tests, race detection, and fault injection materially cover behavior that this
single percentage does not express, but unexecuted branches remain a known
test-depth gap. The two-hour run is not a substitute for a future 24-hour
campaign or a full-duration Rust load driver.

## 6. Endurance operation

`testkit/catalog/soak_test.py` can own an isolated Redis 8 AOF fixture, sample
Redis and process state, inject script-cache loss, command-connection loss,
pause, and restart, and preserve structured heartbeats on interruption. It
uses a random Catalog Zone and removes only that fixture/Zone.

```text
python -B testkit/catalog/soak_test.py \
  --duration-seconds 120 --sample-seconds 10 \
  --result-file testkit/results/catalog-soak-2m.json

python -B testkit/catalog/soak_test.py \
  --duration-seconds 7200 --minimum-redis-seconds 7200 --sample-seconds 30 \
  --subscriber-fanout 2 --persistent-subscribers 1 \
  --result-file testkit/results/catalog-soak-2h.json \
  --sample-file testkit/results/catalog-soak-2h-samples.jsonl
```

The production-sized default is 16 Catalogs, 256 fields of 256 bytes, two
Subscribers, one persistent Subscriber, and 128 mutations/s. Additional
Subscribers are memory-only so fanout tests do not multiply synchronous local
checkpoint writes. `--persistent-subscribers` accepts zero through the total
fanout; one retains persistence/restart coverage without turning disk pressure
into the dominant workload. The Go controller keeps the last accepted revision
per path, matching the real Publisher contract; it uses Subscriber state only
to recover after an ambiguous operation. The driver uses Redis `TIME` as its
workload deadline, and the outer harness independently rejects a run whose
Redis-observed interval is shorter than
`--minimum-redis-seconds`. This prevents WSL or VM clock drift from qualifying
a short run. A 24-hour campaign uses the same command with both duration values
set to `86400`.

## 7. Assessment

| Component | Score | Reason |
| --- | ---: | --- |
| Lua/Redis contract | 9.8/10 | compact atomic glue, strict ABI/corruption checks, fewer hot-path calls, and exact bounded history; Redis runtime-failure and global-revision costs remain |
| Go SDK | 9.8/10 | narrow generic API, lock-free reads, bounded decoder/fuzz, Linux race evidence, fault recovery, and monotonic bbolt recovery |
| Rust SDK | 9.7/10 | generic static `FieldValue`, ArcSwap plus shared field state, borrowed protocol parsing, one-reader topology, redb recovery, and clean clippy/rustdoc |
| Qualification confidence | 9.7/10 | strict two-hour Redis-clock run, 18 injected faults, Linux race/fuzz, cross-language interop, and two-promotion Sentinel evidence; 24-hour and full-duration Rust load remain |
| Overall current Catalog | 9.7/10 | coherent production design with strong bounded-failure evidence and explicit remaining deployment limits |

### Lua/Redis strengths

- Replace, Patch, and Delete commit data, revisions, indexes, tombstones, and
  notifications atomically under a token-fenced lease.
- Exact ABI/type/canonical checks reject predictable corruption before the
  first mutation, and events are built from committed state.
- Array Patch no longer performs a redundant existence lookup; tombstone
  revision lookup and removal are batched.
- Generated standalone, Go-embedded, and Rust-embedded scripts are verified
  byte-for-byte from shared fragments.

### Lua/Redis costs

- Redis cannot roll back writes already performed if a script hits an
  exceptional runtime failure such as OOM; deployment headroom and
  `noeviction` remain operational requirements.
- Every mutation contends on one Zone-global revision and publishes a complete
  operation payload. An extremely hot Zone will reach one Redis core first.
- Delete performs bounded retention maintenance, so its tail latency depends
  on the configured eviction batch.
- Redis Cluster is intentionally unsupported because one atomic mutation spans
  several Zone keys.

### Go strengths

- `Entry` identity is stable and reads publish immutable states through atomic
  pointers; callers can choose a different `Load[T]` type on every read.
- One persistent Pub/Sub listener and at most one temporary sync/repair task
  serve all normalized subscriptions; repair reads are bounded and pipelined
  without one goroutine per path, and the temporary task exits while idle.
- Canonical Array validation is allocation-light, and the streaming decoder
  reduces a 512-field notification from about 1,030 durable allocations to 8.
- Optional bbolt state/cursor writes are monotonic, disposable, and disabled
  after the first reported persistence failure.

### Go costs

- An accepted Patch clones the complete current field map before publishing a
  new immutable state; cost is proportional to the Catalog's field count and
  bytes even when one field changes.
- Optional bbolt persistence is deliberately synchronous to preserve ordering;
  a slow local disk can delay notification application and repair.
- The decoder may reserve the remaining bounded notification payload as an
  arena before trailing-byte rejection. This is bounded by the configured
  maximum but should be included in memory sizing.
- Statement coverage is 74.5%; the long fault campaign and fuzz/race gates add
  depth, but several defensive error branches are not directly counted as
  executed by the coverage profile.

### Rust strengths

- `Entry::load<T>` provides the same external-type flexibility with static
  `FieldValue` checking and no erased user value in the cache.
- `ArcSwap<RawState>` keeps reads lock-free; `Arc<Fields>` makes status-only
  transitions O(1) in Catalog bytes instead of deep-cloning the value.
- The event parser borrows transient payload text, validates bounds before
  durable allocation, and is isolated from the Subscriber state machine.
- redb work runs off the async executor, checkpoints are monotonic, and the
  complete Catalog surface passes clippy with warnings denied plus rustdoc with
  warnings denied.

### Rust costs

- `BTreeMap` gives deterministic order but costs O(log n) lookup/insertion, and
  a Patch still clones the current field collection for immutable publication.
- Full payload values must still be copied into owned storage; `Arc` removes
  redundant state/status copies, not the accepted mutation copy.
- Every Subscriber owns a dedicated Fred Pub/Sub connection in addition to
  shared command connectivity. This is the intended non-blocking topology but
  must be included in connection budgets.
- Rust has no Go-style race detector; confidence comes from ownership, focused
  reconnect tests, Clippy, and integration/endurance behavior.
- Rust passed post-soak Redis convergence and live cross-language/Sentinel
  recovery, but the 7,201-second mutation driver itself was Go; equal-duration
  Rust-specific load remains a separate qualification item.

### C++23 strengths

- Public Catalog types contain no Redis-driver or SQLite types; one compiled
  core avoids repeating state machines for every loaded application type.
- Stable Entries atomically publish immutable states, while `entry::load<T>()`
  accepts either a compile-time Schema type or raw Fields without Redis I/O.
- One persistent listener plus at most one temporary sync/repair task provides
  the reviewed bounded topology and returns to one task while idle.
- SQLite checkpoints are transactional, monotonic, normalized-scope keyed, and
  remain incapable of overwriting Redis.
- Strict GCC, clang-tidy, ASan/UBSan/leak, authenticated Standalone, and plain
  Sentinel startup/integration gates pass.

### C++23 costs

- C++ has not yet run the full two-promotion Catalog Sentinel campaign or a
  Catalog-specific long soak/performance benchmark.
- Sentinel+TLS is rejected until dynamic target discovery can preserve secure
  hostname verification; only Standalone TLS and plain Sentinel are currently
  represented.
- The present build matrix is Linux GCC and lacks install/export packaging,
  MSVC/Clang/macOS consumption evidence, and a legacy C ABI.
- A Patch still constructs the complete immutable post-patch state, so a
  one-field update remains proportional to the full local value.

Across the protocol and all SDK layers, the principal intentional costs remain one full
Pub/Sub payload per mutation, a complete working set in every Subscriber,
projection plus multiple Redis round trips for Patch, and a full index scan when a
checkpoint falls below the tombstone floor.

## 8. Current policy disposition

These choices do not change Replace/Patch/Delete semantics:

1. Resolved 2026-08-28: Catalog shares the thin root Redis transport but owns
   its independent Zone, scripts, workers, limits, and checkpoint lifecycle.
2. Accept the hidden tombstone defaults of 24 hours, 1,000,000 retained keys,
   and 256 maximum evictions per Delete, or choose different fixed values.
3. The 65,536-field defensive ceiling remains while encoded record bytes
   default to 512 KiB and may be configured through 4 MiB. Removing the count
   ceiling would permit a much larger
   worst-case Lua table and MessagePack frame even though application bytes are
   still bounded.
