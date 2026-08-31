# Registration Package Migration Review

Date: 2026-08-26
Status: implemented and qualified in the uncommitted working tree
Scope: Go Registration/Selector child package, Rust `registration` module,
owned Lua embeddings, tests, harnesses, documentation, and public API exposure

Historical note: the package move and domain ownership remain current, but the
Go internal bridge described below was superseded on 2026-08-28. Current Go
Registration/Catalog code consumes the root Client's borrowed `Redis()`,
permanent `Done()`, and immutable `Timeout()` capabilities directly.
The root retains close ownership and raw operations remain outside Verdandi
invariants.

## 1. Outcome

Registration and Selector no longer live in either SDK root namespace.

| Layer | Final ownership |
| --- | --- |
| Go public API and implementation | `sdk/go/registration/` |
| Go embedded Registration Lua | `sdk/go/registration/internal/protocol/` |
| Go root-to-child transport bridge | `sdk/go/internal/registrationaccess/` |
| Rust public API and implementation | `sdk/rust/src/registration/` |
| Rust script invocation and embedded Lua | `sdk/rust/src/registration/script.rs` and sibling Lua files |
| Canonical maintained/generated Lua | `lua/src/registration/` and `lua/registration/` |
| Design and review documents | `registration/` |

The SDK roots retain only shared connection, configuration, field-codec, and
error concepts. Neither root re-exports Registration, Selector, Candidate, or
their snapshots. No compatibility alias was added because SDK `1.0.0` has not
been published.

Scenario-oriented testkit directories remain at the testkit root. Standalone,
soak, Sentinel, Lua, and interoperability harnesses deliberately share Redis
fixtures or cross-domain checks; duplicating them under a hollow
`testkit/registration` directory would create two owners for the same fixture.

## 2. Public API result

Go callers use:

```go
handle, err := registration.NewRegistration[Attr, Data](client, options)
err = handle.Register(ctx, attr, data)
err = handle.Update(ctx, nextData)
err = handle.Unregister(ctx)

selector, err := registration.NewSelector[Attr, Data](ctx, client, options)
candidate, ok, err := selector.One(ctx, policy)
candidates, err := selector.Any(ctx, policy)
```

The application names only `Attr` and `Data`. Their pointer decoder constraints
are inferred. `verdandi.Fields` implements the same generic boundary, so raw
field users do not receive a second Raw API.

Rust follows native associated-constructor and borrowing conventions:

```rust
let registration = Registration::<Attr, Data>::new(&client, options)?;
registration.register(&attr, &data).await?;
registration.update(&next_data).await?;
registration.unregister().await?;

let selector = Selector::<Attr, Data>::new(&client, options).await?;
let selected = selector.one(Duration::from_millis(10), policy).await?;
let selected = selector.any(Duration::from_millis(10), policy).await?;
```

The authoritative signatures, callback rules, local mutation transaction, raw
`Fields` use, errors, and lifecycle are in [`api.md`](api.md).

## 3. Internal boundary

### 3.1 Go

Go cannot let the root package import its child package without creating an
import cycle. The internal `registrationaccess` package therefore binds a root
`*verdandi.Client` identity to a capability-limited runtime:

- Redis driver and Client owner context;
- immutable local Selector settings;
- current Zone Registration limits;
- child admission and configuration-refresh leases;
- asynchronous error reporting and closing state; and
- the Registration script preload callback.

Application code cannot import this bridge because Go's `internal` rule blocks
it. The bridge does not expose a Redis driver or an internal method on the
public Client. One `sync.Once` state per Client shares immutable runtime data,
while every published Registration still owns its independent bounded queue,
worker, desired/confirmed state, and renewal timer.

`NewRegistration` remains entirely local: it allocates the UUID and handle but
does not look up a runtime, touch Redis, admit a Client child, or start a
goroutine. `Register` performs the first runtime lookup and publication.
`Client.Close` unbinds before cancellation, preventing construction of new
active children while existing admitted children observe and join shutdown.

### 3.2 Rust

The Rust module owns its core, Selector, clock, deadline index, event decoder,
pending-change accumulator, script request/reply code, and four Lua embeddings.
The root crate contains only the shared Client and common types.

Typed Registration now stores `Arc<ClientInner>` rather than cloning the public
`Client` handle. The public Client maintains a separate handle count; therefore
dropping the last public handle still starts joined shutdown even when a
Registration retains internal state. A real Redis integration test covers this
case.

## 4. Behavior preserved

The move changes source ownership, not the wire/storage contract:

- four exact `register`, `update`, `renew`, and `unregister` Lua programs;
- one process UUID per Registration lifetime;
- immutable Attr/TTL and fixed Data field-name structure;
- Update coalescing in admission order and lease refresh on real Update;
- renew-only timestamp/TTL changes without content revision changes;
- one Registration queue/worker/timer, never one fleet-wide queue;
- one persistent Selector listener and at most one temporary sync/repair task;
- subscribe-before-scan plus same-connection PING fencing;
- half-synchronized views returning `unavailable`;
- bounded one-change-per-UUID buffering and targeted revision repair;
- RedisClock expiry, retained non-selectable state, and explicit-unregister purge;
- raw `Fields` and strongly typed Attr/Data through the same public API; and
- EVALSHA operation selection with per-script `NOSCRIPT` reload.

The Lua generator now writes and checks the Go copy at
`sdk/go/registration/internal/protocol`. The two saved Go decoder regression
seeds moved with the fuzz target to
`sdk/go/registration/testdata/fuzz/FuzzDecodeRegistrationEvent`.

## 5. Test and harness corrections found during migration

The migration review found four test-infrastructure defects and corrected them
without weakening a gate:

1. The Lua generator still targeted the removed Go root embedding directory.
   Its output manifest now targets the child package, and Redis 8.8 functional
   verification passes.
2. The Go fuzz regression corpus remained under the root package and was no
   longer discovered. Both saved allocation-bomb cases now load from the child
   package; a fresh 30-second run completed 7,342,037 executions.
3. Standalone and soak harness commands selected root-package test names after
   those tests moved. Both now pass `./registration`; a final harness run proves
   that the command executes rather than silently matching zero tests.
4. Selecting all three ignored Rust load profiles let libtest overlap their
   endpoint-wide `CONFIG RESETSTAT` windows. A test-local asynchronous mutex now
   serializes only those profiles. The previously failing all-profile command
   passes without requiring `--test-threads=1`.

One 30-second fault run was deliberately retained as failed evidence. All six
injected faults and cleanup passed, but only one natural expiry cycle completed
and the unchanged gate requires two. The gate was not relaxed; the replacement
90-second run completed three expiry cycles and passed.

## 6. Verification

### 6.1 Static, unit, race, fuzz, and API

| Check | Result |
| --- | --- |
| Go `gofmt`, `go test ./...`, `go vet ./...` | PASS |
| Go Linux race, shuffled, ten repetitions | PASS |
| Go external-package generic inference and raw `Fields` construction | PASS |
| Go 30-second decoder fuzz | PASS, 7,342,037 executions |
| Registration Lua deterministic generation | PASS |
| Registration Lua contract on Redis 8.8.0 | PASS |
| Rust formatter | PASS |
| Rust all-target tests | PASS, 37 library tests plus non-Registration targets |
| Rust Clippy, all targets/features, warnings denied | PASS |
| Rust rustdoc | PASS |
| Go and Rust Sentinel peers as external consumers | PASS |
| Root public namespaces contain no Registration/Selector re-export | PASS |

### 6.2 Current-source fault gate

The authoritative current-source result is
[`registration-package-migration-final-90s-20260826.json`](../testkit/results/registration-package-migration-final-90s-20260826.json),
with flushed samples in the sibling JSONL file. Its 88-file SHA-256 fingerprint
is `3af6405f80a4ef3af123f4ad143b4714bb0a94c50190ac023ce6356ecf346758`.

| Measurement | Result |
| --- | ---: |
| Redis | 8.8.0, isolated AOF everysec container |
| Population | 500 Registrations, 8 Selectors |
| Updates | 45,000 in 90 seconds |
| Update p50 / p95 / p99 | 0.668 / 1.096 / 1.415 ms |
| Update retries | 1 during injected faults |
| Selection transactions | 7,152 |
| Selection p99 | 1.156 ms |
| Natural-expiry cycles / records | 3 / 384 |
| Churn cycles / records | 9 / 144 |
| Final revision / generation | 91 / 3 |
| Go goroutines | 2 -> 528 -> 2 |
| Unexpected asynchronous errors | 0 |
| Redis memory stable-window change | -3,784 bytes |
| Evictions / rejected connections | 0 / 0 |
| Final database size | 0 |

All six scheduled faults passed: script flush, Pub/Sub connection kill,
three-second Redis pause, ordinary connection kill, AOF restart, and a second
script flush. Canonical Lua, Rust raw convergence, and Rust typed
Registration/Selector ran again after the fault phase and passed.

The longer 150-second package run remains useful evidence for the same logic:
75,000 updates, two update retries, 11,939 selection transactions, three expiry
cycles, five churn cycles, generation three, zero unexpected errors, stable
memory, and an empty final database. The final Rust module-file relocation came
after that run, so the 90-second result is the exact final-source fingerprint.

### 6.3 Release Rust load

The final release build used 500 Registrations, eight Selectors, ten update
seconds, ten renewal seconds, and a 5,000-record synchronization profile:

| Measurement | Result |
| --- | ---: |
| Update cadence | 500.0/s |
| Update operation p50 / p95 / p99 | 0.767 / 1.140 / 1.423 ms |
| Automatic renew executions | 471.7/s |
| 5,000 registrations published | 123.902 ms |
| 5,000-record Selector sync | 132.885 ms |
| 5,000 registrations removed | 58.816 ms |
| Redis memory for config, Registry, and 500 records | 144,419 bytes |
| Tokio tasks | 2 -> 519 -> 1 |

The renewal result is within the test's 80%-120% timing gate. It is lower than
500/s because timer cadence, test boundaries, and shutdown exclude renewals
that fall outside the exact ten-second measurement interval.

### 6.4 Sentinel

The result is
[`registration-package-migration-sentinel-20260826.json`](../testkit/results/registration-package-migration-sentinel-20260826.json).
The isolated three-Redis/three-Sentinel run completed in 33.42 seconds:

- initial primary 16381;
- acknowledged-write-loss primary 16382;
- final recovered primary 16383;
- Go and Rust Selector generations `1 -> 2 -> 3`;
- both process UUIDs preserved;
- `SCRIPT FLUSH`, all-Sentinel loss, primary loss without a resolver, Sentinel
  restart, second promotion, and cross-language convergence passed.

The first complete retry timed out while Sentinel still reported the excluded
16382 primary. Its topology cleaned completely. The immediately repeated full
matrix passed; this operational flake is recorded rather than hidden.

## 7. Performance review

Five-sample WSL/Linux Go benchmarks on an i7-13700F show no package-boundary
regression in the measured hot paths:

| Hot path | Result | Allocations |
| --- | ---: | ---: |
| Decode Registration event | 843.3-873.4 ns | 12 |
| Merge 32 pending Updates | 5.821-5.901 us | 1 |
| Validate default maximum record | 1.143-1.162 us | 0 |
| RedisClock upper bound | 23.11-23.34 ns | 0 |
| Apply visible Update | 1.336-1.399 us | 5 |
| No-repair pending drain | 18.98-22.46 ns | 0 |
| Typed `One` over 500 candidates | 20.980-21.352 us | 43 |

The Go bridge is used only when a Registration is first published or a
Selector is created. Update, Renew, local `One`/`Any`, event decoding, and view
application do not perform a bridge-map lookup.

## 8. Score

Overall current implementation score: **9.8/10**. This is an engineering and
evidence assessment, not a release or production certification.

| Area | Score | Reason |
| --- | ---: | --- |
| Registration Lua | 10.0 | unchanged accepted atomic glue, exact generation, Redis 8 verification |
| Go SDK | 9.7 | narrow child API, local construction, preserved ownership, extensive race/fuzz/fault evidence |
| Rust SDK | 9.7 | idiomatic module, static field values, corrected Client-handle lifecycle, strict lint/live evidence |
| Selector synchronization | 9.8 | bounded recovery, fail-closed views, retained TTL, three observed generations |
| Package maintainability | 9.8 | domain-owned sources/scripts/tests/docs with no root compatibility surface |
| Test evidence | 9.9 | exact fingerprint, unit/race/fuzz/load/AOF/Sentinel/cleanup evidence and retained failed gates |

## 9. Principal strengths

- Public discovery is immediate: users import one domain package/module rather
  than finding Registration and Catalog declarations mixed at the root.
- Strong types and raw fields use one implementation, so there is no second
  behavior surface to drift.
- Construction, readiness publication, worker ownership, and shutdown remain
  explicit and independently testable.
- Go's internal bridge exposes capabilities, not the Redis driver, and adds no
  lookup to steady-state Update/Renew/selection paths.
- Rust preserves language-native ownership and fixes the subtle distinction
  between public Client handles and internal lifetime references.
- Script source, generated copies, fuzz corpus, package tests, load tests, and
  documentation now move together with their owner.
- Failed infrastructure or qualification attempts are retained and explained;
  no gate was weakened to turn a failure into a pass.

## 10. Remaining weaknesses and trade-offs

- Go documentation necessarily displays two inferred private pointer type
  parameters on constructors. Callers still spell only `[Attr, Data]`; removing
  those parameters would trade static decoder dispatch for runtime assertions.
- The Go root-to-child bridge uses an internal process map keyed by Client
  identity. `Client.Close` removes it deterministically. Forgetting Close can
  retain that entry, although the same mistake already retains Redis resources.
- Typed Go `Selector.One` over 500 candidates still costs 43 allocations. It is
  fast at about 21 us, but future profiling may justify reusing more transaction
  scratch state.
- Selection callbacks remain serialized and cooperatively bounded. Verdandi
  can reject a result returned after its context expires but cannot forcibly
  terminate application callback code.
- Local load mutation is a prediction, not a distributed reservation. Multiple
  Selector processes converge only after remote reports.
- Pub/Sub remains non-durable and Sentinel replication remains asynchronous;
  repair and same-process republish are required after loss.
- The exact post-migration fingerprint has a 90-second fault gate. Earlier
  two-hour evidence covers the same production state machines before the
  source-package relocation; a new two-hour run would raise relocation-specific
  endurance confidence but is not needed to prove a path-only module move.
- The combined Registration/Catalog interoperability peers are temporarily
  waiting for the parallel Catalog child-API migration. Registration's
  bidirectional cross-language path passed through the Sentinel peers.

## 11. Evidence files

- [`registration-package-migration-20260826.json`](../testkit/results/registration-package-migration-20260826.json): compact aggregate.
- [`registration-package-migration-final-90s-20260826.json`](../testkit/results/registration-package-migration-final-90s-20260826.json): exact final-source fault gate.
- [`registration-package-migration-final-90s-20260826-samples.jsonl`](../testkit/results/registration-package-migration-final-90s-20260826-samples.jsonl): flushed Redis samples.
- [`registration-package-migration-soak-150s-20260826.json`](../testkit/results/registration-package-migration-soak-150s-20260826.json): longer package-run evidence before the final path-only Rust script move.
- [`registration-package-migration-sentinel-20260826.json`](../testkit/results/registration-package-migration-sentinel-20260826.json): complete failover matrix.
- [`registration-package-migration-final-30s-expiry-gate-failed-20260826.json`](../testkit/results/registration-package-migration-final-30s-expiry-gate-failed-20260826.json): retained rejected short gate.

No commit, tag, package publication, or push was created.
