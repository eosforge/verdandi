# Verdandi Optimization and Regression Review

Date: 2026-08-30

## 1. Outcome

The current Go, Rust, Lua, configuration, and shared testkit implementation has
been consolidated, rebuilt, and qualified without adding C++ or Leader. C++ is
explicitly deferred until the Redis driver, codec boundary, asynchronous
runtime, and public ownership model are selected. Leader remains a separate
future module.

The accepted implementation has these principal changes:

- Catalog no longer uses an external Path lock. Its executable surface is four
  generated Lua scripts: Read, Replace, Patch, and Delete.
- Replace and Delete are one atomic Redis-primary operation. Patch projects the
  requested fields without a lock and commits only if Lua still observes the
  exact base revision.
- Go and Rust Catalog Publisher values are stateless views of a Catalog Client;
  they no longer own a lock token, retry timer, or independent close lifecycle.
- At this 2026-08-30 checkpoint a Go Catalog Subscriber owned exactly two
  long-lived goroutines: one Pub/Sub reader and one synchronization/repair
  worker. The 2026-08-31 design supersedes that lifetime detail with one
  persistent listener plus at most one temporary synchronization task.
- Go Registration and Catalog clients share one internal lifecycle gate and
  one set of validation primitives. Rust shares one crate-private Activity and
  RAII Guard while retaining Tokio cancellation and awaited shutdown.
- Both SDKs consume the same configuration and binary Catalog-event conformance
  corpus.
- Optional bbolt/redb Catalog checkpoints remain. They are disposable,
  monotonic recovery accelerators rather than an audit log or authority.

No commit or push was created.

## 2. Catalog concurrency after lock removal

### Replace and Delete

Each request executes one Lua script. Redis serializes scripts on the current
primary, so the last script executed is the current state. There is no SDK-side
lease, acquisition deadline, token cleanup, or second release round trip.

### Patch

Patch remains an exact-base operation:

1. the SDK reads only the fields needed to project and validate the result;
2. the SDK sends the base revision, projected byte count, and changed fields;
3. Lua rechecks the current base revision before making any mutation;
4. one writer from a same-base race can succeed; every later writer receives
   `stale` and does not partially mutate the record.

The Redis 8.8 contention regression starts two concurrent Patch requests at
base revision 1. It requires exactly one success, exactly one stale result,
final revision 2, and one complete, internally consistent value. The test
passes.

A response lost after Redis accepted a mutation remains an ambiguous outcome.
The caller must read or synchronize authoritative state before deciding whether
to retry. This is a distributed-request property, not something an SDK lock can
eliminate.

## 3. Consolidation and maintainability

### Go

- `internal/lifecycle.Gate` centralizes operation admission, cancellation,
  child-worker tracking, and close waiting for Registration and Catalog.
- `internal/validate` centralizes exact duration, optional integer, optional
  duration, and Zone validation without exporting policy constants.
- Catalog Publisher contains no mutex or local state and every operation is
  admitted by its owning Catalog Client.
- Subscriber termination is completed by the last of its two workers. There is
  no watcher whose only purpose is to wait for the other workers.

### Rust

- crate-private `Activity` centralizes closing admission, active-operation
  accounting, notification, cancellation, and public-handle accounting;
  `Guard` releases admission through RAII.
- Registration and Catalog preserve their language-native asynchronous close
  behavior without duplicating poisoned mutex, active count, idle notification,
  and handle-count state.
- strict Catalog Read decoding was moved to a private included source fragment.
  It remains in the same module namespace, so the split does not broaden any
  implementation visibility.

### Shared protocol evidence

`testkit/conformance/v1` now contains:

- binary MessagePack Replace, Patch, and Delete events, including binary field
  values;
- JSON configuration acceptance cases for Standalone and explicit supported
  zero values;
- shared rejection cases for unsupported versions, Cluster mode, invalid zero
  timeout, and the removed `catalog.lock` object.

Go and Rust execute the same vectors instead of maintaining equivalent-looking
but independent fixtures.

## 4. Current physical source inventory

These counts are physical lines, include documentation comments and embedded
generated source, and exclude Go `_test.go` files and Rust test directories.
They are a maintenance indicator, not a complexity or performance score.

| Scope | Physical lines | Largest current file |
| --- | ---: | --- |
| Go production source | 11,856 | `registration/selector_core.go`, 1,413 |
| Rust production source | 10,592 | `registration/selector.rs`, 2,035 |
| Canonical Lua fragments | 864 | `catalog/actions/read.lua.inc`, 147 |

The optimization deliberately reduced ownership paths and duplicate state, not
comments or validation. A mechanical split of the largest Selector files would
not improve runtime and can obscure tightly coupled invariants. They should be
split later only at proven state-machine boundaries, with exact behavior and
benchmark comparison.

## 5. Regression matrix

| Gate | Result | Relevant evidence |
| --- | --- | --- |
| Registration and Catalog generated-source checks | pass | generated SDK copies match canonical Lua |
| Shared JSON syntax and conformance files | pass | schema, example, and both v1 corpora parse |
| Go unit packages and `go vet` | pass | all packages |
| Go WSL/Linux race detector | pass | all packages after final source split |
| Rust all targets/all features | pass | 71 library tests, 4 offline external tests; endpoint-only tests separately exercised below |
| Rust strict Clippy | pass | all targets/features with `-D warnings` |
| Rust rustdoc | pass | all features with warnings denied |
| Rust minimum toolchain | pass | `cargo +1.85.0 check --all-features` |
| Redis 8.8 functional matrix | pass | Lua, Go, Rust, root Redis API, and Go/Rust live interoperability |
| Registration Sentinel matrix | pass | two promotions, Go/Rust generation `1 -> 2 -> 3` |
| Catalog Sentinel matrix | pass | two promotions, final revision 10, final key count 0 |
| Catalog 60-second stable workload | pass | 8,448 accepted mutations, post-soak Lua/Rust/interop checks |

Structured evidence:

- `testkit/results/optimization-functional-20260830.json`
- `testkit/results/optimization-sentinel-registration-20260830.json`
- `testkit/results/optimization-sentinel-catalog-20260830.json`
- `testkit/results/optimization-catalog-60s-20260830.json`
- `testkit/results/optimization-catalog-60s-20260830-samples.jsonl`

The functional fixture used authenticated, isolated Redis 8.8.0 and finished
with an empty database. It covered both Lua contracts, standalone SDK behavior,
Linux race detection, exact-base contention, the 4-MiB Catalog ceiling,
`2^53-1`, script-cache recovery, root Redis APIs, and Go-to-Rust plus
Rust-to-Go Registration/Catalog interoperability. Redis processed 4,305
commands and peaked at 6,797,160 bytes of used memory.

The Registration Sentinel fixture preserved both SDK UUIDs through acknowledged
write loss and two promotions. It also covered minority Sentinel loss, stale
Sentinel configuration, all-Sentinel loss, primary loss while discovery was
unavailable, script flush, recovery, and cross-language convergence.

The Catalog Sentinel fixture completed Go/Rust writes around script flush and
two promotions, reached revision 10, deleted the final value, and left no key.

## 6. Catalog stable-workload result

The post-change Catalog source fingerprint is
`14b4ca0895ded150e3f04dd82ab607e1e9d937fbffa36cd660064a5b028edac3`
over the 70 files selected by the Catalog harness.

| Measurement | Result |
| --- | ---: |
| Redis duration floor | 60 seconds passed |
| Target rate | 128 mutations/second |
| Accepted attempts | 8,448 |
| Patch / Replace / Delete | 8,208 / 192 / 48 |
| Transient errors | 0 |
| Stale retries | 0 |
| Unexpected asynchronous errors | 0 |
| Mutation average | 1.245 ms |
| Mutation p50 / p95 / p99 bucket ceilings | 1.049 / 4.194 / 4.194 ms |
| Mutation maximum | 7.906 ms |
| Go goroutines initial / peak / final | 9 / 10 / 2 |
| Redis stable-window memory growth | 28,656 bytes |
| Evictions / rejected connections / blocked clients | 0 / 0 / 0 |
| Final database key count | 0 |

The run used Redis AOF `everysec`, 16 Catalog paths, 256 fields, two
Subscribers, and one persistent checkpoint owner. It then reran the Catalog Lua
contract, Rust checkpoint/convergence tests, and Go/Rust Catalog
interoperability against the same accepted source.

The first attempted workload exposed a stale test-only shutdown reference left
after Publisher became stateless. The `soak` build tag was not part of ordinary
unit compilation. The obsolete variable use was removed, the tagged package was
compiled independently, and the complete workload above was rerun from zero.
The failed attempt is not counted as evidence.

## 7. Scores

The scores assess the accepted Alpha scope. Deliberately unsupported Cluster,
deferred Leader, and the then-deferred C++ SDK are not treated as implementation
defects in this 2026-08-30 review. The C++ status is superseded by the
2026-08-31 implementation review.

| Area | Score | Main deduction |
| --- | ---: | --- |
| Lua atomic layer | **9.8/10** | no injected late Redis runtime/OOM rollback campaign; Redis scripts are atomic against interleaving but do not transactionally undo a command that succeeded before a later runtime failure |
| Go SDK | **9.6/10** | large Selector state-machine files and verbose external-to-native configuration conversion remain |
| Rust SDK | **9.5/10** | larger Selector module, no Loom/Miri campaign, and no allocator-counted statistical benchmark equivalent to Go |
| Shared protocol/configuration | **9.5/10** | two-language corpus is strong but still small and does not yet prove a third implementation |
| Test and qualification system | **9.7/10** | broad real-Redis/Sentinel/race coverage, but the exact lock-free Catalog fingerprint has a 60-second rather than multi-hour continuous campaign |
| Overall accepted scope | **9.6/10** | release-quality direction with explicit remaining maintainability and endurance work |

These scores are not line-count scores. The implementation is intentionally
strict at untrusted data and asynchronous ownership boundaries; removing those
checks to reduce source size would lower the score.

## 8. Strengths

### Lua

- Lua remains atomic glue rather than an application-schema validator.
- Four Catalog and four Registration scripts are operation-specific and loaded
  by SHA; source fragments are canonical and generated copies are byte-checked.
- Request controls use fixed positional ABIs while dynamic fields stay named.
- Current Redis state, revision ordering, timestamps, TTL, membership, and
  publication are changed in one Redis execution.
- Real Redis tests cover script-cache flush, maximum safe revision, exact-base
  contention, TTL, tombstones, and 4-MiB records.

### Go

- Public APIs remain strongly typed while raw Fields stay available.
- One Registration owns one merge mailbox and writer; one Selector owns one
  listener plus at most one temporary synchronization worker.
- Catalog Publisher is allocation-light and stateless; Subscriber has exactly
  two owned workers.
- Lifecycle admission is centralized and cancellation callbacks run outside
  the gate lock.
- Race detection, real Redis integration, Sentinel, and cross-language tests
  exercise the same production ownership model.

### Rust

- RAII guards make operation-admission release structural rather than
  convention-based.
- CancellationToken and awaited close fit Tokio/Fred instead of copying Go's
  Context/channel shape.
- Strict parsing rejects non-canonical numbers, duplicate controls, malformed
  MessagePack, unsafe revisions, and oversized values before publication.
- Rust 1.85 compatibility, all-feature Clippy, and warning-denied rustdoc pass.
- Raw and derived typed data remain first-class without exposing Fred through
  domain APIs.

### Cross-language design

- Redis key and event shapes do not depend on either SDK's native types.
- Both SDKs load one strict versioned JSON configuration shape, then convert to
  language-native runtime structures.
- Shared event/configuration vectors reduce silent protocol drift.
- Local checkpoints are optional accelerators; Redis remains authoritative and
  audit storage remains a separate concern.

## 9. Remaining weaknesses and recommended direction

1. **Large Selector implementation files.** Split only around stable ownership
   boundaries such as event admission, full synchronization, publication, and
   selection transactions. Require unchanged race, load, and allocation
   evidence for each split.
2. **Duplicated configuration mapping knowledge.** Go and Rust intentionally
   retain native validation because omission and explicit zero differ. Extend
   shared conformance vectors for every field and relationship; do not restore
   a generator until it can preserve language-native types and error paths
   without becoming a second source of truth.
3. **Catalog exact-source endurance is short.** Before publishing `1.0.0`, run
   the same lock-free fingerprint for at least two hours with AOF restarts,
   disconnects, script flushes, Subscriber churn, and Sentinel promotion. The
   older interrupted eight-hour campaign used the superseded lock protocol and
   cannot qualify this source.
4. **Rust concurrency-model testing is conventional.** Add focused Loom tests
   if Activity or publication synchronization becomes more complex; use Miri
   for unsafe additions. The current crate contains no justification to add
   unsafe code merely for a test target.
5. **Cross-language confidence currently has only Go and Rust.** A later C++
   SDK should begin by consuming the existing configuration and binary event
   corpus. Driver/runtime selection must precede public API design.
6. **Ambiguous writes remain possible after connection loss.** Keep the current
   read/synchronize-before-retry contract explicit. Adding a client request ID
   or durable replay log would be a separate protocol feature, not a local SDK
   optimization.
7. **Redis script rollback is not transactional after a late runtime error.**
   Continue validating all deterministic failure conditions before the first
   write and add controlled OOM/failure-injection qualification before a
   production claim.

## 10. Disposition

The implementation is suitable for continued Alpha review. It is not yet a
formal `1.0.0` release because the exact current Catalog source still needs a
long fault-injected endurance campaign and Leader is outside this completed
scope. No source version, package, tag, commit, or remote branch was published.
