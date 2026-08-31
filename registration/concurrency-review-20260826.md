# Registration and Selector Concurrency Review — 2026-08-26

This review records the corrected Go and Rust ownership model. It supersedes
the earlier same-day Client-level Registration coordinator design.

Historical status: its per-Registration ownership, sole-writer worker, renewal
timer, and Selector task topology remain valid. The 256-request queue and FIFO
barrier representation were superseded on 2026-08-28 by the single-slot Fields
merge mailbox recorded in `codex.md`, `registration/api.md`, and `worklog.md`.
Metrics below remain evidence for the exact historical source only.

## 1. Accepted outcome

The frozen 2026-08-26 source used these exact ownership rules:

- every published Registration owns exactly one bounded request queue, one
  long-lived synchronization worker, and one renewal timer;
- a process with `N` published Registrations therefore owns `N` such queues and
  workers; production is expected to hold only a small number, while the
  500- and 5,000-Registration cases are qualification workloads;
- every Selector owns exactly one long-lived Pub/Sub listener/state-machine
  worker and, only while synchronizing, at most one temporary full-sync or
  targeted-repair worker;
- a successful content Update resets that Registration's next automatic Renew;
- a half-synchronized Selector view is unavailable, not a readable stale
  selection view; and
- first-version `One` and `Any` policy evaluation remains O(number of active
  candidates). A complete detached Snapshot is deliberately a heavy O(N)
  operation.

Registration and Selector do not share a queue or mutable state. They share
only the Client's Redis transport, last-valid Zone configuration, shutdown
owner, and diagnostic sink.

## 2. Registration topology

```text
Client
  |
  +-- Registration A
  |     +-- one 256-request bounded queue
  |     +-- one desired/confirmed state
  |     +-- one synchronization worker
  |     +-- one jittered renewal timer
  |
  +-- Registration B
        +-- one independent queue/state/worker/timer
```

`NewRegistration` is local. It allocates a fresh process UUID, validates local
options, performs no Redis I/O, and starts no worker. This preserves the
readiness boundary: the service can finish listening before `Register`
publishes it.

The first `Register` creates that Registration's queue and worker. The worker
performs the complete revision-1 Redis Register before `Register` returns.
After publication, the same worker is the only owner allowed to mutate that
UUID's desired state, confirmation state, revision, timestamp, and renewal
schedule. Update, explicit Renew, recovery Register, and Unregister can never
race each other for the same UUID.

The queue capacity is 256 admitted requests per Registration. A full queue
applies ordinary caller-context backpressure; it does not create another
worker or an unbounded spill queue. Reducing the old 4,096 capacity is important
because capacity is now per Registration rather than per Client.

## 3. Update admission and coalescing

Callers may submit Updates concurrently. Queue admission is serialized, giving
each accepted request a precise FIFO position. Once admitted, the Registration
worker owns the operation even if the caller's waiting context is later
cancelled. Go returns the confirmed or ambiguous worker result. Rust future
cancellation may discard the receiver, but the admitted state transition still
finishes in the worker.

The worker treats a non-Update request as an ordering barrier. Consecutive
queued Updates before that barrier are folded in admission order:

1. each Update is validated independently against the fixed Data field set and
   current Zone limits;
2. an invalid Update fails only its own caller and does not poison the batch;
3. later values replace earlier values for the same Data field or Version;
4. the final desired state is compared with the last worker-owned state;
5. an exact final no-op returns success without Redis I/O or revision change;
6. otherwise one partial Redis Update advances revision once, obtains one new
   Redis timestamp, and publishes one Pub/Sub `update` event; and
7. every valid request absorbed by that batch receives the same final Redis
   outcome.

This intentionally makes superseded intermediate values unobservable. A
successful queued Update means its desired-state transition was accepted into
the ordered batch; it does not promise a distinct revision or Pub/Sub event for
that individual call. Explicit Renew requests are also coalesced when
contiguous, but never cross an Update barrier.

The ordinary no-backlog path avoids allocating a batch container in both
languages. Input Fields are still defensively owned at the API boundary.

## 4. Renewal scheduling

Each Registration worker owns one timer. Zero configuration selects TTL/3;
the accepted explicit interval is 100 milliseconds through TTL/3, and every
scheduled interval receives bounded ±10% jitter.

A successful real Update refreshes Redis `@timestamp` and field TTLs, so the
worker resets the next automatic Renew from Update completion. If an Update is
already queued when the old renewal deadline becomes ready, the worker handles
the Update first. A confirmed write makes the now-redundant Renew disappear.
An invalid or exact no-op Update does not refresh Redis liveness; if its renewal
deadline is already due, the worker performs Renew instead of allowing no-op
traffic to starve the lease.

Renew never advances content revision. Transport ambiguity marks the local
state uncertain. The next Update or Renew sends one complete Register with the
same UUID and desired revision, making recovery safe whether the prior Redis
write committed or not.

## 5. Close and process loss

Explicit Unregister closes admission, drains requests already admitted to that
Registration in FIFO order, sends terminal Unregister only for confirmed
healthy state, and joins its worker. Repeated close calls share the terminal
result. Client Close cancels every child and waits for all Registration and
Selector workers before closing Redis connections.

If the process disappears, its volatile desired state, queue, and worker also
disappear. Redis Hash-field TTL removes the old Registration. No Registration
UUID, Data, replay log, local database, or WAL is persisted by the SDK.

## 6. Selector topology and synchronization

```text
Selector
  |
  +-- persistent listener/state-machine worker
        +-- owns Pub/Sub receive and mutable local state
        +-- owns expiry and retained deadlines
        +-- starts zero or one temporary synchronization worker
              +-- full HSCAN/fetch/PING fence, or
              +-- targeted UUID repair/PING fence
```

The listener subscribes before starting a full scan. The temporary worker
builds a candidate state and requests a PING on that same subscribed
connection. The listener remains the sole Pub/Sub receiver, coalesces events
arriving during the scan, recognizes the matching PONG, and alone installs the
result. Full synchronization advances generation once; targeted repair stays
within the existing generation. Full and targeted work share one optional task
slot and cannot overlap.

When a revision gap starts targeted repair, or when the subscription generation
is lost, the published view is immediately marked unavailable. Raw and typed
`Snapshot`, `Find`, `FindRetained`, `One`, and `Any` return the explicit
`unavailable` code until the next fence succeeds. Retained payload may remain
inside the private recovery state, but it is not exposed through a half-synced
public view.

The listener never runs application policy. `One` and `Any` execute the
callback synchronously under the Selector transaction gate, borrow the current
immutable view, and commit only explicitly staged local Data mutations. The
first release intentionally scans all active candidates. A globally exact
capacity reservation remains outside Verdandi; higher remote report frequency
and smaller prediction steps only bound approximation error.

## 7. Language-specific realization

Go uses one buffered channel and goroutine per published Registration. Atomic
revision/timestamp fields provide lock-free observation; a short admission
mutex orders queue insertion against Close. Typed lifecycle serialization is
limited to Register and Unregister, so concurrent typed Updates can reach the
coalescer.

Rust uses one Tokio MPSC queue and task per published Registration. An async
admission mutex orders send against Close, oneshot channels return individual
outcomes, `OnceLock` publishes immutable typed shape, and atomics expose
terminal, revision, and timestamp state. `Drop` signals the same worker; callers
requiring ordered completion and the Unregister result use explicit
`unregister().await`.

Both implementations use the same desired-state, ambiguity-recovery, batch,
renewal-reset, and half-synchronized-unavailable rules. Their APIs follow each
language's ownership and cancellation conventions rather than imitating each
other syntactically.

## 8. Enforced invariants

| Invariant | Enforcement |
| --- | --- |
| One queue per Registration | queue is allocated inside successful Registration publication, never on Client |
| One Registration writer | exactly one worker owns mutable state and calls Registration Lua |
| Ordered Close | admission gate closes before the worker drains already admitted requests |
| Bounded backlog | 256 entries per Registration; caller context bounds pre-admission waiting |
| Batch order | only consecutive Updates/Renews coalesce; the other kind is a FIFO barrier |
| One renewal timer | timer is worker-local and reset by confirmed Update/Renew |
| One persistent Selector worker | one listener spawn at Selector construction |
| At most one temporary Selector worker | one optional synchronization handle/result slot |
| No half-sync selection | every public active/retained/policy view checks synchronized state |
| Targeted repair keeps generation | only successful full synchronization advances generation |
| Joined shutdown | Registration and Selector completion is acknowledged before Client transport close |

## 9. Final verification

The current-source preflight passed:

- Go unit tests, `go vet`, tagged build, and WSL/Linux race integration;
- Rust 44 unit tests, formatting, Clippy with warnings denied, and documentation;
- canonical Redis 8.8 Lua contracts and generated-script identity;
- Go and Rust standalone lifecycle, configuration refresh, recovery, typed API,
  and live cross-language interoperability;
- 30-second Go and Rust loads at 500 Updates/s with eight Selectors; Update p99
  was 1.324 ms for Go and 1.585 ms for Rust;
- 5,000-Registration paginated synchronization and cleanup in 64.76 ms for Go
  and 113.52 ms for Rust;
- numeric worker regression evidence: Go `2 -> 513 -> 4` goroutines and Rust
  `5 -> 521 -> 1/2` Tokio tasks around 500 live Registrations; and
- a final-source 210-second fault run with script loss, Pub/Sub loss,
  ordinary-connection loss, a three-second Redis pause, AOF restart,
  expiry/retained cycles, and churn. It completed 105,000 Updates, reported no
  unexpected asynchronous errors, returned Go goroutines `2 -> 529 -> 2`, and
  left `DBSIZE=0`;
- reducing the deliberately per-Registration queue from 4,096 to 256, then
  removing the redundant typed-Update byte copy, cut observed comparable
  500-Registration preflight peak heap from 160,098,688 bytes to a
  35,014,568-47,374,384-byte range, a 70.41-78.13% reduction; and
- the final-source independent three-Redis/three-Sentinel preflight completed
  in 63.970 seconds and passed two promotions,
  acknowledged-write loss, all-Sentinel loss/restart, Go/Rust UUID preservation,
  and Selector generations `1 -> 2 -> 3`.

The final-source fingerprint is
`c7bef517173b9c298e41b6dac272e78736b317c017bbe70ba838185960bdf63a`.

Before starting the accepted endurance interval, a frozen-source audit found
that Go checked the initial Register reply but did not check the revision and
timestamp in later successful Update/Renew replies. The pre-fix endurance run
was rejected and its owned fixture was removed. Go now requires every success
reply to contain the exact expected positive revision and a nonzero timestamp;
Go and Rust both classify a post-dispatch corrupt reply like an ambiguous write,
retain the complete desired state, and repair with a full Register on the next
Update or Renew. Focused unit and live Redis regressions prove this recovery.

The replacement authoritative run passed on the frozen fingerprint above:

- Redis `TIME` measured **7,866.527 seconds**, above the hard 7,200-second
  qualification floor. The 8,000-second Go workload margin prevents a client or
  WSL clock jump from being mistaken for Redis endurance time.
- 500 typed Registrations completed **4,000,000 Updates** with seven expected
  retries. Update p50/p95/p99 were **0.649/1.044/1.427 ms**. Final revision was
  exactly 8,001.
- Eight Selectors completed **639,704** `One`/`Any` transactions and 703,672
  committed local prediction mutations. Selection p99 was **0.845 ms** and
  final generation was 15.
- All **34/34** standalone faults passed: 14 script-cache flushes, 11 Pub/Sub
  connection kills, four three-second Redis pauses, three AOF restarts, and two
  ordinary-connection kills.
- Natural expiry completed 25 cycles over 3,200 Registrations; explicit churn
  completed 27 cycles over 432 Registrations. The run observed 212 expected
  transient diagnostics and **zero unexpected asynchronous errors**.
- Go goroutines returned `2 -> 529 -> 2`. Redis stable-memory medians moved from
  2,441,152 to 2,385,312 bytes, a decrease of 55,840 bytes; there were zero
  evictions, zero rejected connections, no sample failures, and final
  `DBSIZE=0`.
- Canonical Lua generation/contract checks and Rust raw plus typed standalone
  convergence passed after the soak.
- The automatic three-Redis/three-Sentinel tail passed in 39.690 seconds. It
  covered two promotions, acknowledged-write loss, total Sentinel loss and
  restart, full-state republish, cross-language convergence, stable Go/Rust
  UUIDs, and Selector generations `1 -> 2 -> 3` in both SDKs.

The Go test process reported a 179,485,736-byte heap peak and a 71,985,416-byte
final heap. This is not an SDK steady-state measurement: exact percentile
instrumentation deliberately retains two 4,000,000-element `time.Duration`
arrays for Update latency and schedule lag (64,000,000 bytes), plus 639,704
selection durations (about 5.1 MiB). Those measurement arrays explain the
final heap; the shorter equal-topology preflights remain the comparable SDK
resource evidence. The harness's O(operation count) exact-duration storage is
therefore recorded as a test-tool limitation.

Structured evidence is in
[`registration-per-instance-soak-owned-update-2h-20260826.json`](../testkit/results/registration-per-instance-soak-owned-update-2h-20260826.json),
its
[`sample stream`](../testkit/results/registration-per-instance-soak-owned-update-2h-20260826-samples.jsonl),
and the
[`Sentinel result`](../testkit/results/registration-per-instance-sentinel-owned-update-2h-20260826.json).
Historical coordinator-era results remain protocol and Lua evidence, but they
are not ownership evidence for this design.

## 10. Strengths, weaknesses, and final score

Strengths:

- failure and backpressure are isolated per Registration rather than coupling
  unrelated UUIDs behind one Client writer;
- concurrent Update bursts collapse without losing FIFO last-write-wins intent;
- Update naturally replaces redundant Renew work and resets lease scheduling;
- task ownership is simple enough to audit from one Registration worker and
  two Selector spawn sites;
- half-synchronized data cannot accidentally enter selection policy; and
- Go and Rust have both code-level and numeric runtime ownership checks.

Trade-offs:

- resource ownership is O(number of Registrations) per process. This is the
  deliberate price of isolation and is suitable only because normal processes
  hold few Registrations;
- a queue entry retains caller-owned encoded update content until processed;
  the 256-entry bound prevents unbounded growth but does not make pathological
  bursts free;
- multiple valid calls absorbed into one batch share one revision and outcome,
  so intermediate values are intentionally not observable;
- full synchronization and detached Snapshot remain O(N) in time and memory;
- first-version policy callbacks scan O(N), and local Power prediction is not a
  distributed reservation; and
- the exact-percentile endurance harness retains O(operation count) timing
  samples. This is test-only memory, but a bounded histogram would make the
  harness itself more scalable; and
- managed Redis, TLS, deliberate clock steps, and multi-day qualification remain
  outside current evidence.

| Area | Score | Reason |
| --- | ---: | --- |
| Registration Lua atomic glue | 10.0/10 | minimal accepted responsibility, deterministic generated scripts, positional ABI, line-audited hot paths, and complete contract coverage |
| Go Registration/Selector | 9.8/10 | corrected per-instance ownership, typed/raw APIs, reply validation, race/load/fault evidence, and complete two-hour cleanup |
| Rust Registration/Selector | 9.6/10 | idiomatic matching ownership and failure semantics with strict lint/live/Sentinel coverage; lacks a Rust-native long-duration allocator/profile campaign |
| Synchronization and recovery | 9.8/10 | unavailable half-sync boundary, bounded coalescing, fenced repair, retained TTL, and repeated Standalone/Sentinel recovery |
| Operational evidence | 9.9/10 | frozen source, Redis-time gate, four million Updates, 34 faults, post-checks, two promotions, structured samples, and verified cleanup |
| Maintainability | 9.7/10 | narrow public API and explicit ownership; large state-machine files and O(operation count) soak instrumentation remain improvement targets |

Overall reviewed Registration/Selector slice: **9.8/10**. Lua's 10.0 is scoped
to its deliberately small approved glue responsibility, not a claim that the
whole distributed system is perfect. The overall slice remains below 10
because managed Redis/TLS, deliberate host-clock steps, multi-day operation,
Rust-native long-duration profiling, and globally exact load reservation are
outside the present evidence.
