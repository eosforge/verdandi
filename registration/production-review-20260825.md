# Registration/Selector Production Optimization Review

Date: 2026-08-25

> Historical evidence: the protocol, Lua, API, and performance observations in
> this report remain useful, but its per-Registration worker and Selector task
> ownership was superseded on 2026-08-26. Use
> [`concurrency-review-20260826.md`](concurrency-review-20260826.md)
> for the current concurrency and resource assessment.

## 1. Result

The Lua, Go, and Rust Register/Selector implementation has completed a focused
production/open-source review. The accepted changes preserve the protocol and
public APIs while reducing local Selector update work, allocations, and copied
payload bytes. All unit, static, generator, documentation, Linux race, isolated
Redis 8.8 functional, direct typed two-hour, and Sentinel checks pass.

The Go WSL/Linux paired benchmark is the clearest measured result:

| Hot path | Before | After | Change |
| --- | ---: | ---: | ---: |
| apply one Registration Update | 4.103 us/op | 1.300 us/op | -68.32% |
| apply one Registration Renew | 3.083 us/op | 1.236 us/op | -59.90% |
| drain pending events without repair | 38.25 ns/op | 19.18 ns/op | -49.86% |
| Update/Renew bytes | 6,984 B/op | 2,888 B/op | -58.65% |
| Update/Renew allocations | 37 allocs/op | 5 allocs/op | -86.49% |
| no-repair pending drain | 48 B, 1 alloc/op | 0 B, 0 alloc/op | -100% |

The comparison used Go `1.26.4`, Linux/amd64 under WSL, an Intel i7-13700F,
ten samples per case, and a 750 ms benchmark interval. `benchstat` reported
`p=0.000`, `n=10` for all three latency changes. These are in-process state
application measurements, not Redis round-trip latency.

No commit, tag, release, or push was created.

## 2. Scope and isolation

This review changed only Registration/Selector implementation and tests plus
shared project reports. It did not change Catalog Lua, Catalog-specific Go or
Rust implementation, Catalog checkpoints, or Catalog test logic.

The other task's Catalog 24-hour run started after the initial implementation
changes were complete and was active on isolated Redis port `36440` during this
review. Its fingerprinted source scope is
`e1afff94990d79b91f9d89fc35f14f94a15bedbe70e859faa4fb2cc1695a5afd`
across 75 Catalog-Lua/Go-SDK/Rust-Catalog/harness files. No source in that
fingerprint was edited after the run started. This review therefore did not
start a competing WSL load, fuzz, Sentinel, Redis pause/restart, or remote
throughput campaign while the Catalog run was collecting production evidence.
The run later ended `interrupted` after 26,765.765 seconds and its port closed.
Only then did the isolated direct typed qualification run on port `36443`; that
sequencing preserves both source and runtime isolation.

## 3. Accepted implementation changes

### 3.1 Lua

The four generated operation-specific scripts remain byte-for-byte unchanged.
The existing implementation already reflects the completed line audit:

- SDKs own schema, field-name, value-size, and capacity validation;
- Lua owns only atomic Redis state transitions and Redis-state invariants;
- fixed positional arguments avoid key/value envelope parsing;
- Register, Update, Renew, and Unregister load independent cached SHAs;
- common fragments are source-maintained and generated bodies are never hand
  edited; and
- generator checks and embedded Go/Rust copies are byte exact.

Changing Lua solely to make this review appear active would discard stronger
same-Redis evidence. Literal caching, extra local aliases, generic helpers, and
additional Lua validation were already measured or reviewed and do not improve
the accepted hot paths. The production choice is therefore to retain the
qualified canonical bytes.

Canonical SHA-256 values:

| Operation | SHA-256 |
| --- | --- |
| Register | `3fe840d6ea01d9c0bdd6141af3d4f3a9ed8b31c68874dc76d6065de739b2739f` |
| Update | `aa908d08fa5f0811756d6e4b785d23e90931528eb7d2a167ce43cd772249e580` |
| Renew | `a58df74f4c371a57ece69b3a485c48008e0c51b594619ff15c3244f4608e2cbd` |
| Unregister | `67bdf6bfdcaf2fe5febc8e42eeed9bac47ba3928462f4d5ce45d965688f8d787` |

### 3.2 Go

The internal byte slices are immutable after decoding or caller detachment.
Selector records now exploit that invariant instead of cloning every field on
each Update, Renew, Register event, or same-revision view rebuild. Public API
boundaries still return detached values and never expose internal mutable byte
slices.

Accepted changes:

- shallow-copy immutable field maps inside state transitions;
- move already-detached Update values into desired Registration state instead
  of cloning them twice;
- transfer decoded Register event ownership directly into Selector state;
- allocate the repair map only when a repair is actually required;
- reset reconnect failure backoff after a successful synchronization;
- cache complete Registration byte size on the immutable internal record;
- update that cached size by exact revision/version/value deltas on patch;
- retain full record-size gates without rescanning every unchanged field; and
- add targeted benchmarks and regression tests for version preservation,
  decimal digit boundaries, projected oversize rejection, pending-event drain,
  Update, and Renew.

The size cache is an internal invariant, not trusted external metadata. The
bounded event decoder validates field names and values before state mutation;
all record construction and mutation paths update the cache, and tests verify
both a `9 -> 10` decimal-width transition and rejection without partial state
mutation.

### 3.3 Rust

Rust adopts the same ownership model without changing its public detached
record contract:

- desired Registration Update values are moved into the next state after Redis
  arguments are built, avoiding a second patch clone;
- internal Selector Data is stored in `Arc<Fields>` so Renew and metadata-only
  transitions share immutable payload state in O(1);
- public records still deep-clone their field values and cannot alias internal
  mutable state;
- complete record byte size is cached and maintained by exact deltas; and
- regression coverage proves detached records do not alias subsequent internal
  updates.

No shared Catalog Rust file was edited.

### 3.4 Files changed across the review and typed follow-up

- `sdk/go/field.go`
- `sdk/go/registration/registration_core.go`
- `sdk/go/registration/registration.go`
- `sdk/go/registration/selector_core.go`
- `sdk/go/registration/selector.go`
- `sdk/go/value.go`
- `sdk/go/registration/event.go`
- `sdk/go/registration/benchmark_test.go`
- `sdk/go/registration/selector_retained_test.go`
- `sdk/go/registration/soak_test.go`
- `sdk/go/registration/sentinel_integration_test.go`
- `sdk/rust/src/registration/mod.rs`
- `sdk/rust/src/registration/selector.rs`
- `sdk/rust/tests/integration.rs`
- `testkit/soak/soak_test.py`
- `registration/production-review-20260825.md`
- `registration/typed-soak-20260825.md`
- `testkit/results/registration-production-review-20260825.json`
- Registration-only sections in `codex.md`, `worklog.md`, and
  `test-results.md`

Catalog implementation files are deliberately absent from this list.

## 4. Verification completed on current source

| Check | Result |
| --- | --- |
| Registration Lua generator `--check` | PASS |
| canonical Lua vs Go/Rust embedded bytes | PASS, four exact hashes |
| Go `go test ./...` | PASS |
| Go randomized order, ten repetitions | PASS |
| Go targeted size/version regressions | PASS |
| Go `go vet ./...` | PASS |
| Go recursive formatting check | PASS |
| Go Linux/WSL race detector | PASS |
| Rust formatter | PASS |
| Rust Clippy, all targets/features, warnings denied | PASS |
| Rust unit/all-target tests | PASS, 42 unit tests; 9 external tests intentionally ignored without environment |
| Rust documentation build | PASS |
| isolated Redis 8.8.0 Registration Lua contract | PASS |
| isolated Go Redis integration | PASS |
| isolated Go Redis integration under Linux race | PASS |
| Rust Registration/Selector reconciliation | PASS |
| Rust live Zone policy refresh | PASS |
| Rust final-client cleanup | PASS |
| Rust 256-field/240-byte ceiling recovery | PASS |
| live Go/Rust binary Registration interoperability | PASS in both directions |

The Redis-backed checks ran against Redis Open Source `8.8.0` on isolated port
`36441` after the production source changes. The fixture processed 3,657 Redis
commands, verified the Registration Lua contract, Go integration in 10.569 s,
Go integration with the WSL/Linux race detector in 12.585 s, the four named
Rust Registration/lifecycle scenarios, and live cross-language interoperability.
Its final status is `pass`; the harness cleaned its own resources.

## 5. Optimization evidence and rejected changes

The first Go CPU/allocation profile showed repeated deep field cloning as the
dominant allocation source: `bytes.Clone` represented 85.97% of allocated
objects and `cloneFields` reached 97.28% cumulatively. After ownership-safe
shallow copies, full record-size calculation became a dominant remaining CPU
cost. Caching the immutable record size produced the final paired result above.

Two tempting changes were deliberately rejected:

- Reusing a persistent pending-event scratch buffer would save about 112 bytes
  per drain but could retain the last event payload, including a near-limit
  payload, for the entire idle lifetime of a Selector. The bounded short-lived
  allocation is the safer memory trade-off.
- Additional Lua locals/helpers/literal tables did not have evidence of a
  server-time win and can increase VM instructions or maintenance surface. The
  already qualified generated scripts remain smaller and clearer.

The later direct typed Selector profile found a separate redundant ownership
copy: `FieldEncoder` is forbidden from retaining its destination, yet the SDK
cloned that destination once more. Returning the already owned bytes reduced
the comparable 500-candidate transaction median from 21.51 to 20.53 us/op
(`-4.57%`), bytes from 5,121 to 3,881 (`-24.21%`), and allocations from 54 to 43
(`-20.37%`) over ten WSL/Linux samples. Final `b.Loop` samples have a 21.313 us
median. The remaining profile is dominated by the O(N) borrowed-view build and
scan, which requires an API/indexing decision rather than another ownership
micro-optimization.

## 6. Non-qualifying compressed smoke

A 30-second Registration fault smoke was launched on separate port `36520`
immediately before the Catalog 24-hour process became visible. It was stopped
and its fixture was removed as soon as the overlap was recognized. It completed
15,000 Updates with no unexpected asynchronous error, 41 retries, final
goroutine count two, and expected transient errors during injected faults.

It is not counted as a pass: compressing a three-second Redis pause and an AOF
restart into a 30-second window produced a 2.933 s p99 against the one-second
gate. That result describes an invalid abbreviated schedule, not a code
regression. The flushed diagnostic samples remain at
`testkit/results/registration-production-optimization-30s-20260825-samples.jsonl`.
Port `36520` was confirmed closed.

## 7. Follow-up qualification after isolation release

After the Catalog interval released its port and runtime, the direct typed
current source passed:

- Go formatting, `vet`, shuffled unit repetitions, tagged compilation, and
  WSL/Linux race;
- a 30-second Registration decoder fuzz run with 7,920,411 executions;
- Rust formatting, strict all-target/all-feature Clippy, 42 unit tests, and
  rustdoc;
- a 150-second typed fault preflight;
- a typed Go/Rust Sentinel preflight; and
- the complete Redis-time-gated standalone plus post-soak Sentinel campaign.

The accepted run measured 7,608.409 Redis seconds, completed 4,000,000 typed
Updates, 639,713 selection transactions, 703,688 local mutations, and all 34
faults. Update p99 was 1.239 ms and selection p99 was 0.623 ms. Redis stable
memory growth was 556,728 bytes, final `DBSIZE` was zero, and Go goroutines
returned from a 1,553 recovery peak to the initial two. Canonical Lua, Rust raw
and typed convergence, and the two-promotion Sentinel matrix passed afterward.
The exact record is
[`typed-soak-20260825.md`](typed-soak-20260825.md).

## 8. Scores

These are implementation and current evidence scores, not production,
security, or release certification.

| Area | Score | Assessment |
| --- | ---: | --- |
| Lua atomic glue | 10.0/10 | complete approved responsibility, deterministic generation, exact copies, line-audited hot paths |
| Go Register/Selector | 9.8/10 | direct typed API, measured speed/allocation reductions, detached safety, race/fuzz/long evidence |
| Rust Register/Selector | 9.6/10 | matching typed ownership and transactional behavior; lacks a dedicated allocator/criterion comparison |
| Synchronization and recovery | 9.8/10 | bounded coalescing, repair, PING fence, retry reset, retained view, repeated standalone/Sentinel recovery |
| Maintainability/open-source readiness | 9.8/10 | narrow APIs, generator-owned Lua, explicit invariants, static checks, focused regressions, detailed evidence |
| Current-source operational evidence | 9.9/10 | 7,608 Redis seconds, 4M typed Updates, 34 faults, source fingerprint, complete post-checks and cleanup |

Overall reviewed slice: **9.8/10**. Lua atomic glue is **10.0/10 within its
accepted narrow responsibility**; the whole distributed module remains below
10 because deployment topology, asynchronous failover, callback, and recovery-
resource trade-offs are outside Lua's control.

## 9. Principal strengths

- Clean responsibility split: SDK validation and parsing, Lua atomic Redis glue.
- No JSON parsing in Redis; flat binary fields remain directly patchable.
- Operation-specific cached scripts preserve atomicity without runtime routing.
- Public values are detached while immutable internal storage avoids redundant
  copies.
- Renew is content-revision neutral and now reuses immutable payload state.
- Pub/Sub is treated only as a wake-up hint; revisioned Redis state remains
  authoritative.
- Bounded pending work, bounded decoders, bounded retained state, and explicit
  repair prevent unbounded memory growth.
- Go performance changes are profile-driven and statistically compared rather
  than inferred from source appearance.
- Go and Rust behavior is verified against the same live Redis contract and in
  cross-language operation.

## 10. Principal weaknesses and trade-offs

- Redis Pub/Sub is non-durable; a reconnect or detected gap still requires
  header reads and sometimes a complete Registration reload.
- Go Update/Renew still allocate five objects and about 2.8 KiB in the measured
  internal fixture because a new immutable record/map is intentionally built.
- Cached byte size improves every event but makes centralized construction and
  mutation paths a correctness invariant. New paths must update the cache and
  its boundary tests.
- Rust lacks a production-style Criterion/allocator benchmark that quantifies
  its `Arc` and moved-value gains independently.
- Very high content-change rates still publish new immutable Selector views;
  Renew avoids that work, but large changing fleets require separate fan-out
  capacity evidence.
- The typed Go 500-candidate transaction still spends O(N) work building and
  scanning the borrowed view and measures 3,881 B/op with 43 allocations/op.
- A Redis pause can release a bounded recovery wave. The qualified workload
  peaked at 1,553 Go goroutines, about 207 MB heap, and 252 Redis clients before
  client shutdown returned goroutines exactly to baseline.
- A slow synchronous `One`/`Any` callback serializes that Selector. The SDK can
  reject the completed callback after its deadline but cannot abort arbitrary
  user code safely.
- Local load predictions remain process-local approximations; exact global load
  control requires a different distributed coordination mechanism.
- Timeout after an accepted Redis write remains ambiguous. Reconciliation,
  rather than an exactly-once promise, is the correct recovery contract.
- Sentinel can lose an acknowledged write during asynchronous replication; a
  live owner repairs its Registration, but a vanished process cannot.
- WSL moved its client clock forward by roughly six minutes near the end of
  both formal attempts. The Redis `TIME` floor prevented a false pass, but
  future WSL qualification still requires a substantial client-time margin.
- TLS, managed Redis, wider subscriber fan-out, deliberate Redis clock steps,
  sustained maximum-size payloads, and multi-day Registration operation remain
  outside current qualification.
- Four generated script SHAs increase artifact bookkeeping. Generator checks,
  exact embedded copies, and per-script `NOSCRIPT` recovery remain mandatory.
