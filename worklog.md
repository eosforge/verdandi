# Verdandi Worklog

## 1. Purpose and Update Rules

This is the authoritative record of changing repository state and engineering
work. Read [`codex.md`](codex.md) first. Stable architecture belongs in
`codex.md`; version acceptance belongs in `alpha.md`; protocol mechanics belong
in `protocol.md`; coding rules belong in `coding.md`.

Maintain this file with the work it describes:

- Put only genuinely started work under **Active Work**.
- Give each active item a concrete outcome and verifiable acceptance criteria.
- Keep ordered future work under **Planned Work**.
- Record unresolved decisions and blockers explicitly.
- Move completed work to **Completed Work**, newest first, with verification.
- Do not use this file as a raw chat transcript.
- Never state that uncommitted work exists in Git history or on GitHub.

## 2. Current Snapshot

Last updated: 2026-08-31

- Project: Verdandi, a language-neutral distributed coordination protocol and
  SDK ecosystem.
- Repository: public `git@github.com:LaconisIves/verdandi.git`.
- Local path: `D:\laconis\verdandi`.
- Git state: the commit containing this entry is the first complete `alpha`
  source freeze and is pushed to the public `alpha` branch for review.
- Release state: one immutable review commit exists; no tag, package, stable
  protocol version, or release exists.
- Target SDK version: `1.0.0`; target protocol version: `1.0`.
- Initial backend modes: Redis Standalone and Redis Sentinel.
- Alpha backend baseline: Redis Open Source 8.0.0 or later in a qualified Redis
  8 line, using Hash field TTL for Campaign readiness.
- Explicitly unsupported with no scheduled support horizon: Redis Cluster;
  multi-primary merging is also outside Alpha.
- Implemented native SDKs: Go, Rust, and C++23, while protocol and repository
  remain open to future languages. C ABI v1 exposes the same compiled C++23
  core to C11 and C++11/14/17 callers. A header-only `verdandi::legacy` facade
  provides lower-standard C++ RAII and typed APIs while forwarding every
  operation through that ABI; it owns no duplicate runtime or state machine.
  A managed C# facade now targets .NET 8/10 through the same ABI with private
  source-generated P/Invoke, SafeHandle ownership, strong field types,
  transactional Selector policies, and typed Catalog APIs; it likewise owns no
  duplicate runtime or state machine. Its independent .NET 8/10 Linux x64 ACL
  Standalone and two-promotion Sentinel regressions pass; remaining C# release
  gates are platform/RID packaging, NativeAOT/trimming, TLS, direct
  cross-language peers, performance, and endurance. Concurrent root disposal
  and forced finalizer cleanup now have direct Standalone regressions.
  The C++ driver and codec/runtime model are fixed for Alpha; release
  qualification remains incomplete.
- Implemented Register/Selector slice: four operation-specific Lua programs
  generated from shared fragments with fixed positional request controls,
  fully inlined hot-path glue, direct writes, Redis 8 `HSETEX`, and the exact Hash-field expiry
  ceiling; Redis-owned live policy refresh, application-owned Go/Rust/C++ strong
  field types, Registration-scoped shared configuration refresh,
  transactional local selection prediction, retained
  non-selectable recovery, header reuse, and Go/Rust lifecycle/synchronization
  are qualified against authenticated standalone Redis 8.8 and a three-node
  Redis/three-Sentinel fault topology with independent ACLs.
- Register/Selector production review now uses immutable internal field-value
  ownership, cached exact record sizes, lazy repair allocation, and Rust `Arc`
  payload reuse. Paired WSL/Linux Go measurements reduce Update/Renew state
  application by 68.32%/59.90%, their allocations from 37 to five, and the
  ordinary pending drain to zero allocations without changing public APIs or
  Lua executable logic. Temporary Chinese maintenance comments regenerate the
  four Registration scripts to 14,112 bytes and new SHAs. The current Selector additionally reuses immutable-view
  identity, stores ordered shared record references, and uses reusable `Any`
  selection marks; 500-record view publication and typed `One` improved by
  48.83% and 40.02% in ten-sample Linux comparisons.
- Registration execution is isolated per published Registration: one
  single-slot Fields merge mailbox, one capacity-one wake signal, one
  long-lived worker, one desired/confirmed state, and one renewal timer for that
  UUID. A small admission semaphore defaults to eight result waiters (range
  1..256), while the mailbox retains only the latest pending Version and value
  per changed Data field. Other Registrations share neither mailbox nor worker.
  Each Selector owns one persistent Pub/Sub/state task and at most one temporary
  full-synchronization or targeted-repair task.
- Numeric defaults, ranges, zero semantics, and relationship checks now live in
  methods on their owning Go/Rust/C++ configuration structures. The abandoned VDL,
  generator, and parallel rule modules are removed, and no exported constants
  surface is added. `configuration.md` remains the hand-maintained cross-language
  review table. Root Redis, Registration/Selector, and Catalog keep separate
  native configs; Catalog records default to 512 KiB with a configurable 4 MiB
  ceiling.
- Current Registration/Selector optimization fingerprint
  `2d3235af5a7a63049e4ba63c3a4fe2a933cd71ce829d753dbdfd9f1a89c8100b`
  passes Linux microbench/race, the complete short Redis 8.8 functional matrix,
  and a two-promotion Sentinel matrix. The accepted 2026-08-28 one-hour run has
  its own older frozen fingerprint and remains historical evidence.
- Registration and Selector now live in the public `registration` child
  package/module/namespace in all SDKs. One root Client owns the Redis transport. In Go,
  that root is a thin concrete go-redis wrapper; in Rust it is a thin private
  Fred transport capability with awaited driver shutdown. Neither root owns
  Zone, child admission, or joined worker shutdown; Registration and Catalog
  independently own Zone and lifecycle. Go child domains directly borrow
  `Client.Redis()`, observe `Done()`, and inherit `Timeout()`; the root
  alone closes the driver, while raw operations are an ACL-controlled escape
  hatch outside Verdandi invariants. Rust keeps Fred private. Root re-exports are absent; Registration design, review,
  production sources, embedded scripts, and package tests all follow the domain
  boundary.
- Implemented Catalog slice: four generated Read/Replace/Patch/Delete Lua
  programs, Go/Rust/C++ Catalog child packages on the shared transport, stateless Publisher, complete
  in-memory Subscriber, stable Entry, per-load generic typing, Hash/ZSET/Pub/Sub
  repair, bounded streaming event decoders, and optional monotonic
  bbolt/redb/SQLite checkpoints pass current unit and Redis 8.8 integration
  tests. Every Catalog Subscriber now owns one persistent listener and at most
  one temporary full/scope synchronization and repair task; the temporary slot
  drains coalesced requests and exits while idle.
- The Go SDK now requires Go 1.27. Generic methods own typed Registration,
  Selector, and Entry loading; static generic codec function instances remove
  retained closures. Registration construction is 240 B/three allocations in
  the current smoke benchmark, down from 288 B/five allocations.
- The coding standard now requires language-native SDK implementations rather
  than source-shape symmetry. Go operation Contexts remain explicit parameters
  and are not stored in long-lived Clients; Rust uses owner-held hierarchical
  Tokio cancellation tokens. New stable language/runtime features are reviewed
  against the declared minimum version and measured before custom abstractions
  or compatibility changes are accepted. During the current maintainer-review
  phase, production source uses detailed Chinese declaration/block comments;
  test source is explicitly excluded and release readiness triggers conversion
  back to concise standard English.
- Go Registration and Catalog now share the root `Encoder.Encode() (Fields,
  error)` and `Decoder.Decode(Fields) error` contracts. Encoders transfer one
  complete field map to Verdandi; raw `Fields` deep-clones caller storage. No
  legacy codec aliases remain because the SDK is unreleased.
- The current Lua/Go/Rust/C++ source review preserves the specialized generated Lua
  hot paths, removes redundant SDK allocation and state-copying work, moves
  Go's raw Registration compatibility surface into tests, and fixes empty-value
  field comparison. A clean isolated Redis 8.8 regression passes all 13 suites
  with 4,579 processed commands and no background-thread exception.
- Accepted coordination scope now includes paginated service discovery,
  strict zero-or-one Leader election, and persistent Catalog KV
  synchronization. Campaign readiness token and immutable Version are
  independent from Registration; Sentinel Leader activation requires a
  deployment-provided durable fence.
- Campaign/Leader remains unimplemented. Existing Registration Version fields,
  Register/Update behavior, and their qualification remain unchanged and are
  outside the Leader task.
- Registration scale history includes separate five-minute profiles with 500
  live Registrations renewing or updating once per second, the earlier raw-core
  7,263.649-second/3,750,000-Update fault soak, the direct typed 7,608.409-
  second/4,000,000-Update fault soak, and a superseded Client-coordinator
  7,388.601-Redis-second/4,000,000-Update run. The corrected per-Registration
  design passes current functional, race, interoperability, 500-Registration,
  5,000-record, 30-second load, 210-second six-fault, and the current exact-
  source 7,759.124-Redis-second/4,000,000-Update/34-fault gate. The corrected
  automatic two-promotion Sentinel matrix waits for the surviving replica to
  converge before total outage and passes both SDK generations `1 -> 2 -> 3`.
  Current Catalog qualification includes Lua/Go/Rust functional,
  reconnect, checkpoint, decoder, exact-base writer contention, WSL/Linux race,
  and cross-language interoperability. C++ passes strict static/shared GCC,
  C11 and C++11/14/17 C-ABI consumers, clang-tidy, ASan/UBSan, authenticated
  Standalone native/C-ABI integration, and an isolated plain Sentinel
  startup/integration smoke. The same compiled core passes two promotions
  through C ABI v1 and C#, while a direct native-API two-promotion harness,
  Windows DLL/MSVC, and TLS remain open. No service, Node, or Campaign ceiling is encoded; each Catalog
  Path is one bounded complete value.
- License: MIT. The source-freeze commit is public; later review edits and
  endurance evidence remain separate until explicitly committed.

## 3. Active Work

### P0: Review and freeze the project foundation

Outcome: obtain maintainer approval for the independent repository boundaries,
Alpha requirements, architecture, protocol direction, coding rules, and branch
policy before implementation.

Current review documents:

- `README.md`
- `codex.md`
- `alpha.md`
- `architecture.md`
- `protocol.md`
- `coding.md`
- `decisions.md`
- `worklog.md`

Remaining acceptance:

- Approve or amend the Alpha outcome and staged delivery order.
- Complete maintainer code/document review of the frozen snapshot. The first
  source-freeze commit and push were explicitly authorized on 2026-08-31; this
  does not authorize later commits or pushes.

Current progress:

- `decisions.md` separates accepted maintainer directions from unresolved
  recommendations; accepted items are copied into their owning documents.

### P1: Freeze the language-neutral protocol and trust contract

Outcome: produce a reviewable canonical contract before selecting SDK
dependencies or implementing network clients.

Acceptance criteria:

- Freeze Redis-native field contracts, scalar encodings,
  and unknown optional field behavior.
- Freeze remaining identifier alphabets, revision, and target rules.
- Freeze Redis Zone/key encoding and role ownership.
- Freeze Registry pages, membership indexes, PING fencing, and Registration-UUID-fenced
  mutation rules without a service-count ceiling.
- Freeze Catalog raw Value fields, independent LWW Patch, explicit tombstone,
  Hash/ZSET/Read floor recovery, optional checkpoint, and Subscriber state rules.
- Freeze Campaign readiness, Leader term, version retirement, local validity,
  and exact-token release rules.
- Freeze configuration chunk, manifest, and current-pointer schemas.
- Freeze the Redis ACL trust boundary and supported raw-write behavior.
- Define the ACK transition table and stable string error taxonomy.
- Define lease/clock-skew calculations and exact resource-limit units.
- Create adversarial byte vectors and Lua input/output vectors.
- Validate that future SDK languages can implement the contract without
  copying Go- or Rust-specific concepts.

Current progress:

- Drafted PRT-001 through PRT-015 in `decisions.md`, covering Redis-native field
  contracts, versioning, identifier encoding, ACL trust boundaries, Redis
  keys, version-aware Leader terms, scalable registry synchronization, Catalog
  KV, lease math, ACK transitions, stable errors, resource limits, Command
  deferral, the Redis qualification baseline, and executable artifacts.
- Reviewed the current Hermes Redis Service, Primary, and KV design sources.
  Verdandi now adopts their subscribe-before-snapshot, revision recovery,
  immutable local view, readiness/ownership token, version retirement, and
  exact-release invariants, while replacing the Hermes 100-instance atomic
  snapshot boundary with pagination, per-Registration event coalescing, and a
  subscribed-connection PING/PONG fence. Catalog uses Hash/ZSET/Read recovery
  with an explicit tombstone floor and full-operation Pub/Sub.
- Maintainer direction now fixes three architectural points: include Leader
  election, include Catalog KV synchronization, and encode no maximum service,
  Node, or Campaign count. Catalog is one complete Value.
- Maintainer direction now also fixes Redis 8 Hash-field readiness and Catalog
  Replace/Patch/Delete with strict Patch bases, complete deletion, and no TTL.
- Maintainer direction calls one Node's leased record a `Registration`, calls
  the Zone/Type collection a `Registry`, and stores every Registration
  in its own Redis Hash with key TTL and typed partial field updates.
- Maintainer direction rejects a universal CDDL/deterministic-CBOR envelope for
  ordinary Redis state. SDKs retain the known fields they require; Redis ACLs,
  field contracts, and protocol-owned Lua define supported mutation behavior.
- Maintainer direction sets both first SDKs to `1.0.0`. Source version metadata
  remains fixed before formal publication; no mutable `1.0.0` artifact or tag
  may be published during development. The first protocol is `1.0`; every
  behavior is mandatory and protocol capability negotiation is absent.
- Maintainer direction replaces stable `node_id` plus `generation_id` with one
  SDK-generated UUID per process start. Registrations use
  `verdandi:registration:<zone>:<type>:<uuid>`; crashes expire by TTL and graceful
  shutdown removes the exact UUID through the atomic Registry mutation.
- The SDK constructs and mutates all protocol Redis keys. Applications use
  typed APIs and do not receive Redis clients or perform raw Hash updates.
- Publisher, desired-state, and Catalog data carry no write-authority term.
  Every changed shared-state mutation advances its Redis-owned scope revision;
  Catalog publishes the complete operation and retains authoritative field and
  delete revision indexes. Publisher restart does not
  reset the revision. Generic election remains independent from publication.
- Redis key names, data types, and meanings are forward-compatible and
  unversioned. Compatible evolution adds optional fields or new keys.
- Maintainer direction rejects end-to-end signatures for desired state,
  commands, Catalog, Registration, and ACKs. Redis authentication/ACLs define
  write permission; hashes remain only for content integrity, and ACL-authorized raw
  mutation is outside the protocol guarantee.
- Zone is application-supplied SDK configuration validated as 1 through
  32 case-sensitive ASCII letters. The SDK generates each Registration UUID as
  exactly 32 lowercase hexadecimal characters.
- Type and election-domain IDs use the accepted common ASCII form. Catalog uses
  one bounded Part/ID Path; no opaque business-key token remains.
- A Registration is one Hash exposed as `Meta`, `Attr`, and `Data`. Meta is
  exactly `@uuid`, `@revision`, `@timestamp`, `@ttl`, and `@version`; immutable
  Attr uses `.name`; mutable fixed-structure Data is unprefixed and independently
  patchable. Registration revision is a content version; Attr and TTL are
  immutable for the UUID lifetime. Catalog live Hashes reserve revision,
  Replace revision, shape, and encoded bytes around opaque application fields.
- Each Campaign owns a fresh private readiness token and positive safe-integer election
  Version independently from Registration. Every SDK uses the same numeric
  comparison; Redis validates exact Campaign readiness/ownership and equal
  versions are first-successful-claim wins. Changing Version requires a new
  Campaign; there is no version revision or comparator contract ID.
- Registry membership and Campaign readiness use Redis 8 per-field TTL. The
  separate version/expiry index proposals are removed.
- Registration SDK operations, Lua mutations, and Registry Pub/Sub use aligned
  `register`, `update`, `renew`, and `unregister` string kinds. Register is
  complete, Update carries a Version change and/or Data patch and advances content
  revision, Renew changes only Redis timestamp and lease expiry, and Unregister
  is terminal for the UUID. Selector bootstrap subscribes first, performs a paginated
  current-state scan, and fences it with payload-bearing PING/PONG on the same
  subscribed connection. Per-UUID gaps use targeted fetch plus another PING;
  disconnects restart the generation. Catalog retains its separate Redis-owned
  revision and barrier synchronization. Redis stores no mutation history.
- Catalog permits multiple ACL-authorized writers and bounded atomic
  Replace/Patch/Delete batches. Later execution on the current Redis primary
  wins for overlapping data; protocol `1.0` does not require CAS.
- Registry Hash and channel are both `verdandi:registry:<zone>:<type>`. Its UUID
  fields store per-Registration content revision and use matching field TTL;
  Renew refreshes field expiry without changing that value. Lua derives absolute
  expiry from Redis timestamp plus immutable TTL; `@expire` is not stored.
- TTL expiry or fenced absence removes a Registration from selection but may
  retain its payload until `timestamp + 2*ttl` under an independent byte budget.
  Explicit Unregister purges it immediately; Close drains prior writes, sends
  only on the current healthy generation, and never reuses that UUID.
- Desired targets are typed partition, service-within-partition, or exact
  Registration scopes. Multiple Publishers use Redis-revision last-write-wins.
- Exact Leader release emits one latency-only wake. One domain has zero or one
  application-active Leader; uncertainty immediately closes admission and
  handoff may leave the domain without a Leader. Standalone uses its configured
  Redis primary as term authority. Sentinel requires a deployment-provided
  durable fence after Redis claim and before callback admission; missing or
  failed fencing remains unavailable.
- Catalog snapshots expose synchronized health, revision, tomb version, floor,
  deleted state, and the complete Value. Last-known data is labeled
  unsynchronized; optional local storage is a disposable checkpoint.
- Go and Rust expose `Catalog[T]` over that raw Mirror. Go binds an external
  `FieldCodec[T]`; Rust statically dispatches `CatalogValue` and caches `Arc<T>`.
  Both own deterministic complete-Value differencing and bounded publication.
- Catalog unit, integration, WSL/Linux race, minimum-toolchain, and live
  cross-language suites pass against isolated Redis 8.8.0 fixtures.
- Command delivery is deferred beyond SDK `1.0.0`.
- ACL provisioning is by role and Zone, not per Registration UUID.
- Redis persistence is the current Publisher-state source. The core SDK does
  not manage metrics, history, or audit; a separate future synchronizer module
  may provide them.
- The ACK/error contract will use generated stable string names from one
  language-neutral table rather than numeric wire values copied as a convention.
- Go uses `go-redis/v9`. Rust uses the qualified `fred` line for the implemented
  Standalone and Sentinel slice, with `redis-rs` retained only as a fallback if
  later work finds a blocker. Registration Meta,
  key/value MessagePack event envelopes, RedisClock, one-pending-change-per-UUID,
  and the writer/Selector state machines are documented. Standalone and
  Sentinel payload-bearing PONG, bounded per-UUID pending coalescing, and
  cross-language recovery are qualified. Go Attr/Data values directly implement
  `Encoder`/`Decoder`; Rust values implement `FieldValue`. Raw
  `Fields` follows the same generic API. Registration business-logic generation
  and Schema objects are not SDK responsibilities.
- The first executable protocol and SDK slice now exists: generated
  `lua/registration/{register,update,renew,unregister}.lua` programs implement
  the four Registration lifecycle actions from shared fragments; Go and Rust
  independently implement Client, Register, and Selector; Selector
  remains a bounded direct-command algorithm rather than an all-record Lua
  snapshot. The implemented slice supports and is fault-qualified for
  Standalone and Sentinel. Every other coordination subsystem remains
  unimplemented.
- Client bootstrap fills six active Registration record limits plus
  `configuration_refresh_ms` in the
  non-expiring `verdandi:config:<zone>` Hash with defaults 16 Attr fields, 32
  Data fields, 64-byte names, 128-byte Attr/Data values, 16-KiB records, and a
  30-second refresh. An administrative backend may later change them atomically.
  Clients retain a last-valid snapshot, use its refresh interval with jitter,
  or refresh explicitly. Registration workers validate complete projected
  state in the SDK; steady-state Lua Update reads neither configuration nor the
  complete Hash.
- Registration desired state plus confirmation status and Selector
  active/retained views are explicitly bounded process memory, not local
  persistence. SDKs write no Registration UUID, content, replay log, database,
  or WAL to disk; restart creates a new UUID and old state expires by Redis TTL.

### P2: Establish the shared conformance testkit

Outcome: make protocol correctness executable before SDK behavior can diverge.

Acceptance criteria:

- Add valid and invalid Redis field/scalar vectors.
- Add exact hashes, Registration identity, revision, Registry
  scan/PING/event, Catalog LWW/mirror, Leader term, manifest, lease, ACK,
  and error vectors.
- Add real Redis Standalone fixtures for Lua, TTL, and Pub/Sub loss.
- Add a reproducible Sentinel primary/replica/failover harness.
- Define how every SDK consumes the same fixtures without rewriting them.

Current progress:

- Added a Python-driven Redis 8 fixture for Registration replies, exact
  MessagePack events, key and field TTL, revision transitions, idempotency,
  natural expiry, script reload, Update hot-path command accounting, and the
  initial Selector `HSCAN`/pipeline/PING bootstrap. It uses raw oversized input
  to prove Lua does not duplicate the SDK's schema/capacity boundary.
- Added deterministic shared-fragment generation for four operation-specific
  Registration scripts and byte-identical Go/Rust copies. The fixture
  independently flushes/reloads every operation SHA.
- Added an isolated paired specialization benchmark that reconstructs the
  former combined shape from the same fragments, alternates order, and records
  Redis command-stat and wall-throughput evidence without subscribers.
- The fixture uses a unique Zone, removes only its own keys, and never flushes
  the selected database. It resets command statistics and clears Redis's script
  cache for two isolated scenarios and therefore requires an isolated test
  endpoint.
- Added independent Go and Rust unit/integration suites, one live Go/Rust
  producer-consumer harness, a saved Go fuzz regression corpus, 500-Registration
  sustained update/renewal profiles, eight-way fan-out, 5,000-record scale,
  retained recovery, generated Go typed-codec tests, per-UUID coalescing
  stress/benchmarks, and empty-Redis disconnect/recovery qualification.
- Added a reproducible three-node Redis 8.8/three-Sentinel harness with separate
  Redis and Sentinel ACL credentials. It covers a stale/minority Sentinel,
  promotion, forced acknowledged-write loss, same-UUID full-state republish,
  subscription-generation recovery, `SCRIPT FLUSH`, complete Sentinel loss,
  primary loss without resolution, Sentinel restart, second promotion, and
  Go/Rust convergence.
- Detailed API, reproduction, performance, discovered-failure, and limitation
  records now live in `sdk.md`, `testkit/README.md`, and `test-results.md`.

## 4. Planned Work

### P3: Complete remaining Standalone coordination in the first SDKs

Outcome: extend the completed Client/Register/Selector slice so Go and Rust also
implement the remaining Publisher, Campaign/Leader, desired state,
load policy, and acknowledgements against one Standalone protocol.

Acceptance criteria:

- Keep the completed Catalog Hash/ZSET/Pub/Sub and checkpoint qualification passing as
  later Publisher capabilities are added.
- Concurrent Campaigns produce one exact Leader term, version handoff does not
  overlap callbacks, and stale tokens cannot renew or release a later term.
- Sentinel Campaigns never invoke application callbacks without acquiring the
  same durable fence, and primary-generation changes produce zero overlapping
  application-active terms.
- Cross-language tests extend beyond Registration/Selector to every new data
  class.

### P5: Implement desired configuration

Outcome: deliver immutable opaque snapshots with atomic consumer activation and
acknowledgement convergence. Command delivery is deferred beyond SDK `1.0.0`.

Acceptance criteria:

- Target, time, size, hash, and revision failures
  are rejected before activation.
- Consumer callbacks expose no partially activated state.
- ACK states and stable errors are identical across SDKs.

### P6: Integrate an initial consumer

Outcome: qualify Bifrost as a consumer without moving Bifrost business contracts
into Verdandi.

Acceptance criteria:

- Controller uses Publisher, Proxy uses Node, and Dispatcher uses Node plus
  Selector.
- Bifrost route configuration remains opaque bytes to Verdandi.
- No Verdandi package imports a Bifrost implementation or schema.
- Bifrost activation, eligibility, drain ordering, and configuration leases
  pass integration tests.

### P7: Capacity and fault qualification

Outcome: publish reproducible evidence for operational guidance without a
hard-coded scale promise.

Acceptance criteria:

- Retain the completed separate 500-live-Registration Renew/Update profiles,
  eight-Selector fan-out, 5,000-record recovery, and Sentinel matrix as the
  Registration baseline. Add the separate 10-Catalog-mutations/s and large-
  Value capacity profiles to the completed Catalog functional baseline.
- Run reconnect storms, Redis primary loss, Sentinel loss, Publisher restart,
  malformed publication, and long-duration churn.
- Record hardware, topology, versions, payloads, cadence, connections, CPU,
  memory, allocations, latency, failover, and recovery duration.
- Derive recommended partition and limit defaults from evidence.
- Restart the isolated Catalog 24-hour interval from zero. The prior run
  `95384fc9` ended as `interrupted` after 26,765.765 seconds; its partial result
  remains at `testkit/results/catalog-soak-24h-20260825.json`, and its dedicated
  port `36440` is closed. It is evidence of the elapsed interval only, not a
  completed 24-hour qualification.

### P8: Add an optional statistics and audit synchronizer

Outcome: provide a separate service/module only after its storage, retention,
and audit requirements are defined; do not make it a core SDK dependency.

Acceptance criteria:

- Consume Verdandi state/change subscriptions and persist an independently
  owned history.
- Own metric names, audit schema, storage backend, retention, backpressure, and
  gap behavior.
- Record an explicit history gap when the source's available recovery window is
  exceeded. Catalog provides only bounded replay above its floor, not an audit
  journal.

### P9: Qualify C# and add future SDK languages beyond C++23

Outcome: complete the implemented C# binding's release matrix and add another
SDK only after a real consumer need exists.

Acceptance criteria:

- Define idiomatic package/API, formatter, lint, test, concurrency, and release
  rules for the language.
- Pass the complete protocol vector corpus.
- Pass production/consumption tests with at least one existing SDK.
- Add no new protocol behavior solely to imitate a language-specific library.
- For C#, retain qualified per-RID native packages, Windows/macOS coverage,
  NativeAOT/trimming checks, TLS, performance, cross-language peers, and
  endurance before release claims. Concurrent disposal/finalizer pressure and the
  independent Linux x64 Standalone and two-promotion Sentinel gates are
  complete.

## 5. Blockers and Open Decisions

- Add byte-exact cross-language vectors for application-owned typed Attr/Data
  codecs as real consumers define their field encodings. The direct Go and Rust
  conversion contracts, raw Fields compatibility, fixed-structure top-level
  patching, and key/value event envelopes are accepted.
- Qualification, not a protocol redesign, remains for sustained maximum-size
  Register/reset fan-out above eight subscribers, sustained driver-ingress
  pressure, and connection-generation RedisClock under delayed responses and
  clock steps.
- Freeze the cross-language mandatory Sentinel fence-adapter API and qualify at
  least one durable advisory-lock implementation. The strict zero-or-one policy
  itself is accepted.
- Add application-owned Catalog codec vectors as consumers select concrete
  scalar, array, map, or schema types. The raw Value contract deliberately does
  not impose one codec.

Accepted engineering qualification gates, not maintainer decision blockers:

- Validate Registry subscribe/scan/PING buffers and Catalog ZSET pages, floor,
  checkpoint, timeout, and complete-value limits under the accepted workload.
- Establish and pass p95/p99 latency, failover-time, recovery-time, and resource
  objectives for the accepted capacity workload.

## 6. Completed Work

### 2026-08-31: Freeze the complete Alpha review tree and start endurance qualification

- Audited the complete Lua, Go, Rust, C++23/C ABI/C++11 Legacy, and C# source
  tree with each language's formatter, strict compiler/linter, ownership model,
  and declared minimum toolchain rather than forcing one implementation shape
  across languages.
- Added exact Go validation-boundary tests, shared C++ JSON-conformance vectors,
  strict warnings for every project-owned C/C++ target, C# concurrent-disposal
  and finalizer-pressure tests, and a Rust `Ordering` cleanup required by the
  declared 1.85 MSRV. Generated Lua and public protocol behavior are unchanged.
- Passed Go module verification, vet, ten shuffled runs, WSL/Linux race, and
  60-second Registration/Catalog fuzz campaigns; Rust current and 1.85 tests
  plus strict Clippy; C++ format/tidy/static/shared/sanitizer/C/C++11/14/17
  gates; and C# .NET 8/10 zero-warning builds and offline tests.
- Passed the complete Redis 8.8 Standalone matrix with Lua, Go, Rust,
  interoperability, 500 live Registrations, eight Selectors, 60,000 Updates per
  language at 500/s, independent Renew profiles, and 5,000-record recovery.
- Passed Registration and Catalog two-promotion Sentinel matrices, the direct
  C++ Sentinel smoke, and independent C# Standalone and two-promotion Sentinel
  matrices. Every accepted fixture reported final cleanup.
- Recorded exact scope, edge coverage, result artifacts, benchmark ranges, and
  remaining release gates in `freeze-20260831.md`. The commit containing that
  report is the immutable source identity for two isolated twelve-hour
  Registration and Catalog campaigns; those results are post-freeze evidence,
  not content retroactively added to the source commit.

### 2026-08-31: Complete independent C# regression and harden the Release boundary

- Established that every language owns an independently executable regression;
  an aggregate all-language run is no longer required for language-local
  acceptance. Shared fixtures and vectors remain reusable, while
  cross-language interoperability stays a separate compatibility gate.
- Expanded the dependency-free C# test executable across Result/Fields scalar
  boundaries, typed and malformed codecs, C ABI layout, raw Key/Hash,
  Registration lifecycle and exact limits, concurrent Registration/Selector
  calls, Selector rollback/stale/duplicate choices, Catalog stale Patch and
  exact 4 MiB boundaries, and safe terminal behavior after parent disposal.
- Added C#-owned Standalone and Sentinel harnesses. Standalone builds/analyzes
  .NET 8/10, publishes both as self-contained Linux x64 applications, verifies
  explicit and application-directory native loading against separate
  ACL-protected Redis 8.8 fixtures, leaves `DBSIZE=0`, and passed in 26.801
  seconds.
- The C# Sentinel matrix kept both managed targets alive through acknowledged
  write loss, desired-state repair, `SCRIPT FLUSH`, all-Sentinel loss, primary
  loss, unavailable views, restart, and two promotions. Masters moved
  `16381 -> 16383 -> 16382`; both Selector generations advanced
  `1 -> 2 -> 3`; final cleanup passed in 59.041 seconds.
- Initial second-promotion attempts correctly exposed a fixture conflict:
  waiting for the C++ transport-backed managed view to become unavailable under
  a one-second Sentinel `down-after` made the only survivor ineligible, and
  Sentinel logged `no-good-slave`. The C#-owned topology now uses five seconds,
  preserving the unavailable-state assertion without invalidating its recovery
  candidate. An unchanged Go/Rust control still passed its separate topology;
  it is diagnostic evidence, not part of C# acceptance.
- C#'s shared Release build exposed GCC's optimization-only
  `maybe-uninitialized` diagnostic around two optional Catalog shape values.
  Split presence validation from concrete enum extraction without changing the
  protocol. C++ separately passed Debug, shared Release, ASan/UBSan, all nine
  CTest entries per preset with endpoint cases explicitly skipped locally,
  clang-format, clang-tidy, and a live shared-Release C++ Sentinel integration.
- Added machine-readable results under `testkit/results`, updated the C# review
  to **9.3/10**, and left platform binaries, NuGet/RID packaging,
  NativeAOT/trimming, TLS, direct C# cross-language peers, performance,
  concurrent disposal/finalizer pressure, and soak as explicit release gates.
  Created no commit and performed no push.

### 2026-08-31: Add the managed C# facade over C ABI v1

- This initial focused-functional checkpoint is superseded by the independent
  Standalone/Sentinel completion entry above; its implementation history
  remains accurate.
- Added `sdk/csharp` with a pinned .NET 10 toolchain, C# 14, .NET 8/10 library
  targets, warnings-as-errors, nullable analysis, formatter/analyzer rules, and
  a dependency-free executable test project. No NuGet package or native binary
  was added to the repository.
- Implemented stable Result/Error types, immutable continuous Fields, canonical
  Boolean/Int64/UInt64 codecs, static generic `IFieldValue<TSelf>`, root
  Client/Key/Hash, Registration Client and delayed Registration lifecycle,
  Selector One/Any/local mutation/snapshot, and Catalog
  Client/Publisher/Subscriber/stable Entry/typed load.
- Kept C ABI v1 private behind source-generated `LibraryImport`. Dedicated
  SafeHandles own every opaque allocation; finalizer-backed parent leases
  preserve native child-before-parent release order. The loader supports an
  explicit path, NuGet RID layout, application directory, and normal OS search,
  and checks ABI v1 before opening Redis.
- Made borrowed Selector Candidates a `ref struct` and fenced opaque Choice
  values with a process-wide transaction identity. Managed callback and Codec
  exceptions are converted to fixed stable errors before returning through C;
  no fake `Task.Run` async API was added.
- Passed .NET 8/10 Release builds with zero warnings, formatter/analyzer
  verification, and offline Result/Fields/scalar/64-bit-layout tests. Published
  the test runner as self-contained Linux x64 and passed focused Redis 8.8
  Key/Hash, delayed Register, Update/version/content/Renew/Unregister, Selector
  prediction/stale-choice/duplicate-choice/callback rollback/snapshot, and
  Catalog Replace/Patch/Delete/Entry convergence with final key cleanup.
- Repeated the live test through both explicit `VERDANDI_NATIVE_LIBRARY` and
  application-directory native discovery. This is short functional evidence,
  not Windows/macOS, NuGet, NativeAOT, Sentinel/TLS, performance, concurrency,
  or soak qualification.
- Added the detailed C# API/build/deployment guide and the initial **9.0/10**
  managed-scope review. The completed independent regression above raises the
  current score to **9.3/10**. Created no commit and performed no push.

### 2026-08-31: Add the C++11 RAII and typed facade over C ABI v1

- Added the header-only `verdandi::legacy` CMake target and umbrella header.
  C++11/14/17 callers now have owning errors, result/optional values, chrono
  durations, raw Fields, schema codecs, root Key/Hash, typed Registration,
  transactional Selector One/Any/snapshot, and typed Catalog APIs.
- Kept C ABI v1 as the only compatibility boundary. Legacy handles retain
  their ancestors through shared state and release paired C handles through
  move-only RAII. The facade contains no Redis transport, retry, clock,
  synchronization, recovery, worker, or checkpoint implementation.
- Added strict-warning C++11, C++14, and C++17 offline consumer tests plus a
  C++11 Redis integration covering Key/Hash, Registration
  publish/update/version/content/renew/close, Selector local prediction and
  detached views, and Catalog replace/patch/subscriber/load/delete.
- Added independent C++11 translation units for every Legacy component header
  and the umbrella header so accidental transitive-include dependencies fail
  during the normal build.
- Static and shared GCC builds pass all nine CTest entries against Redis 8.8.
  ASan/UBSan/leak with halt-on-error, clang-format, and split-standard
  clang-tidy also pass. No long-duration or new Sentinel test was run.
- Added [`sdk/cpp/LEGACY.md`](sdk/cpp/LEGACY.md) and updated the C ABI, SDK,
  decision, test, and C++ review records. The facade is source-compatible, not
  a promised C++ binary ABI. No commit or push was created.

### 2026-08-31: Add source-buildable C ABI v1 for lower C++ standards

- Added one opaque C ABI v1 to the existing compiled C++23 runtime. It covers
  strict JSON root Client and Key/Hash access, Registration, transactional
  Selector One/Any and snapshots, Catalog Publisher/Subscriber/Entry, owned
  results, bounded string diagnostics, and explicit lifecycle release.
- Added `verdandi::c` as a `LINK_ONLY` CMake interface over the same runtime.
  Native `verdandi::verdandi` remains C++23; C11 and C++11/14/17 consumers do
  not inherit that compile feature. Both static and shared source builds are
  supported, while the source toolchain still must compile the core as C++23.
- Kept strict v1 JSON as the only cross-language configuration carrier and raw
  binary Fields as the value boundary. No STL type, exception, template,
  application struct, driver type, or allocator ownership crosses the ABI.
- Added C11 offline/live tests and independent C++11, C++14, and C++17 consumer
  targets. The live C matrix covers root commands, Registration, Selector local
  mutation/selection/snapshot, Catalog publication/subscription/Entry, and
  exact cleanup.
- Found shared fallback SQLite was not position-independent, enabled PIC on the
  private static dependency, and restored per-build FetchContent output
  isolation so static/shared/sanitizer dependencies cannot rewrite one another.
  Rebuilt the shared runtime and verified 88 exported C symbols plus dynamic
  lower-standard linkage.
- Passed strict static and shared GCC builds, all seven offline test entries
  with the two Redis tests correctly skipped without an address, native and C
  ABI Redis 8.8 live tests, ASan/UBSan/leak offline and live tests,
  clang-format, and project-owned clang-tidy. No long-duration test was run.
- Documented build constraints, ownership, errors, callbacks, evolution,
  strengths, deductions, and the revised **9.3/10** C++ score. Created no commit
  and performed no push.

### 2026-08-31: Expand C++23 boundaries and remove Selector hot-path churn

- Replaced C++ strict-JSON hand-written field chains with non-type-template
  field bindings and variadic short-circuit folds. Binding-local seen bits keep
  duplicate rejection strict without allocating a name vector; unknown and
  required-field behavior is unchanged.
- Replaced Selector `std::function` projectors with two plain function pointers,
  reused immutable typed Attr/Data projections for copyable selection and
  mutation, reused generation-tagged `Any` duplicate marks, and consolidated
  Schema traversal plus application-exception translation into inline
  higher-order helpers. Non-copyable structured values retain their decode
  fallback.
- Rejected signed negative zero as a non-canonical field scalar and added its
  regression case. Removed three obsolete moves exposed when the projector pair
  became trivially copyable.
- Reduced the 32-file C++ production inventory from 9,191 to 9,049 lines while
  retaining one compiled Redis/lifecycle core. Updated the API guide, detailed
  engineering review, scores, limitations, and test evidence.
- Passed strict GCC, C++ unit/live Redis 8.8, clang-format, clang-tidy,
  ASan/UBSan/leak offline and live checks; uncached Go tests and vet; Rust
  tests, formatting, strict Clippy and rustdoc; and both generated-Lua identity
  checks. Per maintainer direction, no long-duration test was run.
- Revised the C++ SDK score to **9.2/10**. Created no commit and performed no
  push.

### 2026-08-31: Implement C++23 and make Catalog synchronization tasks temporary

- Changed Go and Rust Catalog Subscribers from a permanently retained reader
  plus repair pair to exactly one persistent Pub/Sub listener and at most one
  temporary full/scope synchronization and repair task. Pending repair requests
  coalesce into the occupied slot; it drains work and exits when idle.
- Implemented the same lifecycle in the new C++23 SDK together with the root
  Redis Client, strict v1 JSON configuration, raw Key/Hash commands,
  Registration, typed/raw Selector, Catalog Publisher/Subscriber, stable Entry,
  bounded MessagePack recovery, and optional transactional SQLite checkpoint.
- Selected one compiled implementation using Boost.Redis 1.92, yyjson 0.12,
  OpenSSL, and SQLite 3.37 or newer with a locked 3.53.4 fallback. Compile-time
  schema descriptors and policy templates remain at the strong-type boundary;
  Redis and lifecycle state machines are not header-only or repeated per type.
- Supported Redis 8 Standalone, ACLs, Standalone TLS, and plain Sentinel;
  rejected Cluster and Sentinel+TLS explicitly. A possible legacy opaque C ABI
  remains deferred and no duplicate C++11/14/17 SDK was added.
- Hardened subscription startup so Redis error replies become bounded Verdandi
  diagnostics instead of exceptions escaping the reactor. Made driver,
  subscription, Registration, Selector, and Catalog shutdown deterministic and
  exception-safe.
- Passed strict GCC builds, unit and authenticated Standalone integration,
  clang-format, high-signal clang-tidy, ASan/UBSan/leak checks, and an isolated
  ACL-protected three-data-node/three-Sentinel smoke on Redis 8.8.0. The smoke
  completed in 3.424 seconds and left its database and labeled containers
  empty. Go package tests and Rust all-target/strict-Clippy tests also passed.
- Recorded the implementation, build/API guide, exact limitations, **9.1/10**
  C++ score, and release gates in
  [`sdk/cpp/README.md`](sdk/cpp/README.md),
  [`cpp-review-20260831.md`](cpp-review-20260831.md), and
  [`test-results.md`](test-results.md).
- Created no commit and performed no push.

### 2026-08-29: Align Go/Rust configuration and bound Catalog lock waits

- Audited root Redis, Registration/Selector, and Catalog configuration in both
  SDKs. Validation errors now identify the exact rejected field, default and
  range comments are colocated with their owning types, and Rust Catalog
  configuration has its own module instead of being embedded in the client.
- Aligned explicit-zero semantics: Go now distinguishes omitted versus zero
  Selector publication interval and RedisClock uncertainty, while Catalog's
  zero aggregate-view limit is a direct scalar in both languages. Defaults and
  closed ranges have named table-driven tests on both sides.
- Added a positive configurable Catalog Path-lock acquisition timeout: 30
  seconds by default, 100 milliseconds to 1 hour. It bounds the whole lock
  contention loop; each Redis attempt remains bounded by the root command
  timeout. Persistent background recovery remains lifecycle-owned and does not
  turn one foreground call into an unbounded wait.
- Passed Go formatting, all-package tests, vet, and WSL/Linux race tests; Rust
  formatting, 58 library tests plus four endpoint-aware external tests, strict
  Clippy, and warning-denied rustdoc; and all six Go/Rust interoperability,
  Sentinel, and Catalog peer builds. Endpoint-dependent cases were not enabled
  in this focused configuration review. Detailed scores and deductions are in
  [`configuration-review-20260829.md`](configuration-review-20260829.md). No
  commit or push was created.

### 2026-08-29: Make configuration defaults and checks locally reviewable

- Added detailed Chinese field comments to the root Redis,
  Registration/Selector, RegistrationLimits, Zone, and Catalog configuration
  structures in both Go and Rust. Every configurable field now records its
  default, accepted range, and zero/nil behavior where applicable.
- Documented every private Go `runtimeConfig` and Rust `RuntimeConfig` field
  with its effective meaning, expanded default, and range. Split compound
  validation expressions into commented topology, timeout, capacity, view,
  RedisClock, recovery, lock, and path groups without changing accepted values
  or public APIs.
- Passed Go formatting, all-package tests, and vet; Rust formatting, all-target
  and all-feature tests, strict Clippy, and warning-denied rustdoc. Endpoint-
  dependent Redis/Sentinel tests remained explicitly ignored in this short
  review run. No commit or push was created.

### 2026-08-29: Remove configuration code generation and localize validation

- Removed the configuration VDL, Python generator, and six generated-rule Go
  and Rust modules. No configuration generation or generated-rule build edge
  remains.
- Moved zero/default expansion and every numeric/relationship check into methods
  on the owning root Redis, Registration/Selector, Catalog, RegistrationLimits,
  or private Zone configuration structures. No exported rule constants were
  introduced.
- Kept the native Go/Rust public configuration shapes and existing values
  unchanged, updated the cross-language reference and API/decision documents,
  and passed all endpoint-free Go and Rust tests. No commit or push was created.

### 2026-08-29: Reduce typed Selector copies and stale transaction retention

- Reused Go Encoder-owned Attr/Data fields when detaching selected values,
  rather than discarding them and deep-copying immutable internal Fields again.
  `One`/`Any` now finish result decoding before overlay commit, so a return-value
  Decoder failure cannot publish local prediction state.
- Shared SDK-owned immutable overlay base/staged maps, used map-only copies for
  reconciliation, cleared commit scratch, and cleared only removed tails of
  reused transaction/Candidate arrays when a view shrinks. Added direct tests
  for decode rollback and stale-reference release.
- Changed Rust staged/committed remote overlay baselines from copied `Fields`
  to `Arc<Fields>`. Reworked Registration event duplicate tracking into a
  seven-bit reserved-field mask, application maps, and a lazily allocated set
  only for unknown controls; consuming MessagePack strings removes redundant
  clones while preserving hostile-input bounds and all duplicate rules.
- Audited Lua without changing it. A Go experiment that saved one 8-byte event-
  kind allocation but slowed decoding was measured, rejected, and reverted.
- Ten-sample Go 1.27 Linux comparison reduced `One(500)` from 3,882 B/43 allocs
  to 2,226 B/28 allocs and `Any(8/500)` from 14,723 B/154 allocs to 8,067 B/97
  allocs. Conservative immediate latency changes were -3.20% and -16.90%.
- Passed deterministic generation, Python compilation, Go all-package/vet/WSL
  race, Rust 52 library plus four endpoint-free external tests, strict Clippy,
  and rustdoc. An isolated Redis 8.8 matrix passed 14 suites and 4,770 commands;
  a separate Sentinel matrix passed `16381 -> 16382 -> 16383` and Go/Rust
  generations `1 -> 2 -> 3`. All owned ports closed.
- Current 104-file fingerprint is
  `2d3235af5a7a63049e4ba63c3a4fe2a933cd71ce829d753dbdfd9f1a89c8100b`.
  The prior one-hour result is retained as historical baseline rather than
  relabelled. Detailed evidence and scoring are in
  [`registration/selector-optimization-review-20260829.md`](registration/selector-optimization-review-20260829.md).
  No commit or push was created.

### 2026-08-28: Pass the Fields-mailbox one-hour fault qualification

- Corrected stale 100 ms Selector RedisClock fixtures to the generated one-
  second minimum across Go/Rust integration and Sentinel/interoperability
  peers. Added Lua, Rust raw, and Rust typed convergence gates before and after
  the long workload, final database-empty enforcement, and an accurate 104-file
  source fingerprint to the soak harness.
- Bounded each Go Selector Pub/Sub receive with a derived context. The deadline
  now covers both socket read and synchronous go-redis reconnect work, so driver
  backoff cannot prevent the owner loop from publishing an unavailable view
  during total Sentinel loss. The change adds no goroutine.
- Passed a 90-second qualification smoke run and a separate Sentinel preflight,
  then passed the accepted one-hour run `b6af4e4f` on Redis 8.8.0 with AOF
  `everysec`. The frozen fingerprint is
  `38448c747230a72eb4d0b1a4ea838b83467a2b8d66d366909bbb1b73b6dd8f77`.
- Completed all 1,800,000 scheduled Updates across 500 Registrations and eight
  Selectors. Update latency was 0.604057/0.841814/1.135046 ms at p50/p95/p99;
  294,982 selection transactions were 0.182851/0.317371/0.915748 ms. All 16
  injected standalone faults passed with zero unexpected asynchronous errors.
- Go goroutines returned `2 -> 530 -> 2`; Redis late median memory was 175,672
  bytes below its early median; there were no evictions, rejected connections,
  monitoring failures, or remaining keys. Pre/post Lua and Rust checks passed.
- The post-soak Sentinel matrix passed primary movement
  `16381 -> 16383 -> 16382`, Go/Rust generation movement `1 -> 2 -> 3`, total-
  loss unavailable views, UUID preservation, and cross-language convergence.
  All owned ports, containers, and remote temporary directories were removed.
- Final short regression passed deterministic configuration/Lua generation,
  all Go packages, 52 Rust library plus four endpoint-free external tests, and
  compilation checks for both modified interoperability peers.
- Preserved the first one-hour attempt as explicitly rejected evidence: its Go
  workload completed, but the new post-check exposed the stale Rust 100 ms
  configuration. Detailed accepted/rejected evidence and scoring are in
  [`registration/fields-mailbox-config-1h-20260828.md`](registration/fields-mailbox-config-1h-20260828.md).
  No commit or push was created.

### 2026-08-28: Implement the single-slot Registration Fields mailbox and shared configuration rules

- Replaced each Go/Rust Registration's request-object queue with one directly
  merged Fields mailbox, one capacity-one wake signal, and one small admission
  semaphore. Later pending Version/Data-field values overwrite earlier values;
  all Update waiters in a taken batch share its Redis outcome and revision.
  Renew stores no Fields, shares a successful effective Update's TTL refresh,
  and runs independently after an Update no-op or failure.
- Encoded typed Data before mailbox ownership and detached raw Fields, so no
  caller struct survives into the worker. Go callers can cancel before
  admission and wait for the actual outcome after admission. Rust keeps each
  semaphore permit with mailbox work after a receiver Future is dropped, so
  cancellation cannot silently enlarge the admitted set.
- Added `registration.buffer_capacity` with default 8 and range 1..256. Added
  serialization-neutral `schema/config.vdl`, its deterministic generator, and
  package-local rules for Redis transport/pool/reconnect, Registration/Selector
  local behavior, Redis-backed initial Registration policy, and Catalog
  synchronization/recovery/lock/capacity settings. Runtime formats remain
  application-selected rather than fixed to JSON or another carrier.
- This generated configuration layer was removed on 2026-08-29; this entry is
  retained only as historical evidence for the accepted one-hour run.
- Wired Go go-redis and Rust Fred pools and connection recovery while disabling
  automatic business-command replay. Catalog complete records now default to
  512 KiB and permit a configured 4 MiB ceiling; Subscriber aggregate encoded
  bytes may be limited independently.
- Fixed generated Rust zero-minimum validators so strict Clippy sees no
  constant false/true comparisons. Changed generated-default conversions to
  non-panicking saturation followed by normal validation. Corrected a Go driver
  options test so it does not create a real minimum-idle background connection
  merely to inspect settings.
- Passed deterministic generation check; Go format, all-package tests, vet, and
  WSL/Linux race tests; Rust format, all-target/all-feature strict Clippy,
  rustdoc, 52 library tests, and four endpoint-free integration/API tests. Redis/Sentinel integration
  cases remained explicitly skipped because this short review run did not set
  their isolated endpoint variables. No long campaign was started.
- Updated current protocol, architecture, Alpha, SDK, Registration/Catalog API,
  configuration, decision, test-result, and worklog documentation. Historical
  256-queue evidence remains labelled as superseded rather than rewritten. No
  commit or push was created.

### 2026-08-28: Use the single-word root timeout name during focused review

- Standardized the ordinary root Redis command budget as `timeout`: Go exposes
  `Config.Timeout` and `Client.Timeout()`, while Rust exposes `Config::timeout`
  and keeps its accessor crate-private. Invalid configuration identifies field
  `timeout`; domain-specific names such as `sync_timeout` remain qualified.
- Added no compatibility alias because version 1.0.0 is unpublished. Necessary
  test and peer references changed only with the public identifier; test logic
  and comments were not mechanically rewritten.
- Inlined both Go and Rust root Clients' one-use, one-command `PING` bootstrap
  and both Catalog Clients' one-use, one-command Lua load. Registration retains
  its bootstrap because it owns a real ordered invariant: Redis 8 validation,
  Zone policy installation/read, runtime snapshot publication, and script
  loading.
- Passed Go formatting, all-package tests and vet; Rust formatting, 52 library
  tests, four endpoint-free integration/API tests, strict all-target Clippy and
  rustdoc; and Registration/Catalog Lua generation checks.
- A disposable Redis 8.8 functional run passed 14 Lua, Go, WSL/Linux race,
  Rust, root API, Catalog, and cross-language suites with 4,751 commands and
  exact cleanup. Its 88-file source fingerprint is
  `05874222ecae71f6469039e89f6b745a58402cea53171e2fc74d470a7641e867`.
- The subsequent Go-focused review caught and removed the matching Go wrappers.
  Root and Catalog affected-package tests pass. The current fingerprint is
  `bfb9396852fbf66d86f6a0d19fef35b7c5ba5a78e6098ef215366e4ef7747bc7`;
  it is deliberately not presented as having rerun the prior live matrix.
- One Sentinel review run passed both SDK integration cases, first promotion,
  UUID preservation, generation `1 -> 2`, and correct unavailable views during
  total Sentinel loss. It was not accepted as a complete pass because Sentinel
  continued to report excluded primary `16383` during the bounded second-
  promotion window. No SDK assertion failed before that gate; all six ports,
  labelled containers, and remote directories were absent after cleanup.
- Per maintainer direction, subsequent review stays focused on Go, mirrors only
  confirmed semantics into idiomatic Rust, and uses short affected-package
  tests. Full Redis/Sentinel/race/soak qualification waits until all review is
  complete. Created no commit and performed no push.

### 2026-08-28: Expose the direct Go transport capability and complete the production-comment review

- Replaced Go's private `internal/clientaccess` bridge with three root Client
  capabilities: borrowed `Redis() *redis.Client`, permanent `Done()`, and
  immutable `Timeout()`. Registration and Catalog reuse the same pool
  directly. Root Close remains the sole driver owner; raw commands are an
  explicit ACL-controlled escape hatch outside Verdandi limits, error mapping,
  and multi-key invariants.
- Kept request Contexts at Go operation/worker boundaries rather than storing
  them in long-lived objects. Registration and Selector no longer need
  shutdown-only watcher goroutines. Domain Clients retain their own cancel
  functions and joined shutdown. Fixed typed Selector commit so every staged
  candidate decodes before any prediction overlay is published; a later decode
  failure now rolls back the complete transaction.
- Removed Rust Registration/Catalog shutdown watcher tasks. Hierarchical
  `CancellationToken`s now directly fence admission, explicit Close performs
  the join under one async gate, and Drop remains signal-only. Registration
  `is_registered()` now also observes its unique worker terminal state. Kept
  bounded Unregister cleanup independent of domain cancellation so explicit
  close retains graceful cleanup semantics.
- Added detailed Chinese declaration comments to all handwritten production Go
  and Rust functions/fields and documented nontrivial ownership, concurrency,
  capacity, synchronization, and recovery blocks. Tests were not translated or
  mechanically cleaned. Only API-required adaptations and the atomic Selector
  rollback regression changed test behavior. Recorded the temporary Chinese-
  comment rule and the release gate back to concise standard English.
- Added Chinese KEYS/ARGV and atomic-phase comments to maintained Registration
  Lua fragments and generator templates, then regenerated all canonical, Go,
  and Rust copies. Exact-copy checks pass. The four files are now 4,480, 4,975,
  3,314, and 1,343 bytes (14,112 total); only comments/source SHAs changed, not
  executable statements or the steady EVALSHA path.
- Passed Go formatting, unit tests, vet, Go 1.27 WSL/Linux race, Rust formatting,
  52 library tests plus endpoint-free integration/API tests, strict all-target
  Clippy, generator freshness, generator syntax, and Black 26.5.1 on the edited
  generator. The known Windows Rust import-library linker message remains
  informational.
- Passed 17 isolated Redis 8.8 Registration/root suites with 48,120 commands,
  3,707,896-byte Redis peak memory, zero final keys, and exact container cleanup.
  Go/Rust each sustained 500.0 Updates/s for 500 live Registrations; 1,000-record
  Selector sync completed in 24.305/33.636 ms. General Catalog suites were not
  run; the existing shared interop peer performed only its bounded Catalog
  sanity exchange inside the disposable isolated container.
- The qualified Sentinel run first correctly entered unavailable state while
  the topology kept reporting excluded primary 16383. After exact cleanup, an
  unchanged 33.094-second rerun passed acknowledged-write loss, SCRIPT FLUSH,
  total Sentinel loss, two promotions, UUID preservation, and Go/Rust Selector
  generations `1 -> 2 -> 3`.
- The load and qualified Sentinel evidence used 88-file fingerprint
  `bf703e372f259100a2332533c15d299eb22db43b411880afb2b19abce29987c8`.
  Subsequent production-comment completion, signature wrapping, and generated
  derive documentation did not change executable behavior. The exact frozen
  fingerprint is
  `a5323e162ef7778b4cb19847f56214ece0dd8e0634a7180e872df8db8e586739`.
  It passed 12 isolated Redis 8.8 Registration/root suites, 2,264 commands,
  2,632,832-byte peak memory, zero final keys, and exact container cleanup.
- Two frozen-fingerprint Sentinel attempts passed SDK integration, first
  promotion and recovery, total-Sentinel-loss unavailable state, and cleanup,
  but were not accepted as complete passes: restarted Sentinel retained the
  excluded primary 16383 and then 16382 through the bounded second-promotion
  window. No source or test change was made between attempts; all six ports
  were verified closed afterward. Detailed evidence is in `test-results.md`
  and the two `registration-direct-root-*-20260828.json` files. No long
  campaign, commit, tag, release, or push was performed.
- Follow-up ownership review found that Rust still retained a private
  `Client -> Owner -> Arc<Transport>` capability chain. Removed both `Owner` and
  `Transport`: root `Client` now directly owns `Arc<Inner>`, and Registration
  and Catalog retain a clone of the same Client. The private Fred driver,
  timeout, shutdown and Subscriber factory remain methods on that root type.
  No public API or test source changed.
- Rust format, all-target/all-feature compile, 52 library tests, four
  endpoint-free integration/API tests, strict Clippy, and rustdoc pass. Eight
  existing live Redis 8.8 Registration/root tests passed with 548 commands,
  zero final keys, and exact container/port cleanup.
- The new 88-file fingerprint is
  `e709ae4ce1149377c2276e41e053c7b264f64cacda13da29b85559261dd628f9`.
  An intermediate Sentinel run reached the expected unavailable state but was
  rejected when restarted Sentinel retained excluded primary 16382. The exact
  final-source rerun passed in 40.327 seconds with master sequence
  `16381 -> 16382 -> 16383`, stable Go/Rust UUIDs, Selector generations
  `1 -> 2 -> 3`, `SCRIPT FLUSH`, total Sentinel loss/restart, and cross-language
  convergence. All six ports, labelled containers and remote directories were
  absent after cleanup. No test modification, commit, tag, release, or push was
  made.

### 2026-08-28: Standardize language-native implementation choices

- Made `coding.md` explicitly separate cross-language protocol and observable
  behavior from language-specific implementation shape. SDKs must use their
  own language's ownership, cancellation, concurrency, generic, error, and
  resource-management facilities.
- Required review of features available in both the declared minimum toolchain
  and the current stable toolchain before adding custom abstractions. Raising a
  minimum version remains an explicit compatibility decision; performance
  claims require language-native measurement rather than visual source parity.
- Clarified Go's official Context discipline: pass operation Contexts
  explicitly, do not store them in long-lived structs, scope every CancelFunc,
  and avoid shutdown-only watcher goroutines. Clarified Rust's Tokio discipline:
  retain CancellationToken at owner boundaries, derive one-way child tokens,
  use timeout/select for commands, and still await owned tasks.
- This was a documentation and design-rule update only. No SDK source, test,
  commit, or remote state was changed.

### 2026-08-28: Align the Rust root Client ownership boundary

- Reduced the Rust root Client to a public-owner handle over one private Fred
  transport. The transport retains connection construction, timeout, shutdown,
  bounded Key/Hash command, and dedicated Subscriber creation only. Removed
  root admission mutex/counters, child lifetime guards, joined root shutdown,
  manual public-handle counting, and domain `Deref` access to root internals.
- Moved required Zone to `registration::Config::new(zone)` and
  `catalog::Config::new(zone)`; root `Config::new(endpoint)` is connectivity-
  only. Root bootstrap now performs only `PING`; Registration performs the
  Redis 8 `HELLO` check before policy and Lua bootstrap.
- Kept Rust-native lifecycle semantics: explicit `close().await` broadcasts
  loss and awaits Fred `quit()` without joining domains. Dropping the last
  public root handle signals shutdown and schedules best-effort close when a
  Tokio runtime exists. Domains retain and join only their own workers.
- Updated all Rust SDK tests and standalone/Sentinel/interop peers. Added HELLO
  response validation and a real Redis test proving two Registration Zones can
  share one transport, root-first close is bounded, later child construction is
  rejected, and both domains still close independently.
- Rust format, 52 unit tests, all targets, strict Clippy, rustdoc, six live
  Registration cases, two Catalog cases, two root Redis cases, and Go/Rust
  Registration/Catalog interoperability pass on an isolated authenticated Redis
  8.8 fixture. It processed 4,750 commands, peaked at 2,945,264 bytes, ended
  empty, and removed its exact container. Evidence is in
  `testkit/results/rust-transport-refactor-functional-20260828.json`.
- The first Sentinel run reached its second promotion but retained the excluded
  old primary. After exact cleanup, an unchanged repeat completed in 29.914
  seconds with both SDK generations `1 -> 2 -> 3`, UUID preservation, two
  promotions, and cross-language convergence. Evidence is in
  `testkit/results/rust-transport-refactor-sentinel-20260828.json`.
- Ran no long load/soak campaign. Created no commit and performed no push.

### 2026-08-28: Simplify the Go root Client ownership boundary

- Reduced root `verdandi.Client` to one concrete `*redis.Client`, connectivity
  configuration, ordinary operation timeout, bounded Key/Hash helpers and
  descriptor cache, an atomic closed state, and an idempotent close broadcast.
  Removed the global client-access map, root child admission/WaitGroup,
  lifecycle Context storage, `redis.UniversalClient`, and `INFO server` parsing.
- Moved required Zone identity to `registration.Config` and `catalog.Config`.
  Registration now performs its Redis 8 validation through `HELLO`; root Open
  performs only bounded `PING`. Registration and Catalog initially received
  the driver, close signal, and timeout through a client-owned internal
  capability and owned their independently joined shutdown. The later direct-
  root-capability entry supersedes only that access bridge.
- Changed Go root shutdown to `Close() error`. It immediately signals transport
  loss and closes go-redis without waiting for domain Clients. Definite
  `redis.ErrClosed` results map to stable `closed`; ordinary root methods retain
  both concise and `*Context` variants.
- Updated all Go tests, peers, examples, current architecture/API documents,
  decision history, and test evidence. Added invalid-Zone unit cases, close
  broadcast/idempotence coverage, and a live two-Zone shared-transport/root-first
  shutdown regression.
- Go format, three shuffled unit repetitions, vet, all-tag compile, and all
  three peer test/vet gates pass. An isolated Redis 8.8 matrix passed 13 Lua,
  Go, WSL/Linux race, Rust, and interoperability suites with 4,655 commands and
  2,974,568-byte peak memory. Registration and Catalog Sentinel matrices passed
  two promotions each; both cleaned their owned fixtures and Catalog ended with
  zero keys. Evidence is in
  `testkit/results/go-thin-client-functional-20260828.json`.
- Ran no long load/soak campaign. Created no commit and performed no push.

### 2026-08-27: Review and compact Lua, Go, and Rust production source

- Audited every canonical Catalog and Registration Lua fragment. Kept the
  operation-specific generated programs unchanged: their apparent duplication
  avoids extra Lua calls on high-cardinality hot paths, while bounded loops
  avoid Redis Lua stack limits. Both generators reproduce the committed script
  bytes exactly.
- Removed avoidable Go encoding clones, repeated Catalog field-clone logic,
  redundant error joining, version-only Data copies, and duplicate Selector
  update bodies. Raw Registration compatibility helpers now live exclusively
  in test source. Fixed `fieldsEqual` so a missing field cannot equal a present
  empty field, and added a direct regression.
- Removed avoidable Rust intermediate maps, key vectors, value clones, nonce
  formatting, duplicate key validation, and desired-state commit bodies.
  Reused one checked Selector update transition without weakening the separate
  live and pending revision preconditions.
- Windows Go test/vet, WSL/Linux Go race and all-tag compile gates, stable Rust
  format/Clippy/test/doc, Rust 1.85 tests, both decoder fuzz targets, Python
  compilation, and the 160-column source check pass. The paired Linux
  microbenchmarks retain identical allocation counts and show no defensible
  regression in the measured Catalog or Registration paths.
- An isolated randomly labelled Redis 8.8 fixture passed 13 Lua, Go, Rust, and
  cross-language suites: 4,579 commands, 3,050,104-byte peak Redis memory, and
  zero background-thread exceptions. The regression harness now decodes child
  output explicitly as UTF-8 and rejects stale Rust filters that execute zero
  matching tests. No long test, shared Registration fixture, commit, or push
  was used.

### 2026-08-27: Separate Rust tests from production source

- Moved all 19 embedded Rust test modules, including test-only helper types and
  script-source tables, into `sdk/rust/tests/internal`. Production modules now
  retain only minimal conditional path hooks, preserving private white-box
  access without widening the crate API.
- Kept the five existing integration and public-API test files under
  `sdk/rust/tests`. The resulting tree contains 7,808 production-source lines
  and 2,788 test-source lines, of which 1,148 are private white-box tests.
- Corrected two Rust 1.85-incompatible `let` chains discovered by the minimum-
  version gate without changing behavior. Rust 1.85 and the current stable
  toolchain both pass all 53 endpoint-free tests; Clippy passes with warnings
  denied. Eleven opt-in Redis/Sentinel/load tests remain ignored because no
  endpoint was configured. No Redis, long-running test, commit, or push was
  performed.

### 2026-08-27: Standardize Go Encoder and Decoder

- Replaced the unreleased `FieldEncoder`/`FieldDecoder` and
  `VerdandiEncode`/`VerdandiDecode` surface with the shared root `Encoder` and
  `Decoder` capability interfaces. Registration Attr/Data and Catalog
  Publisher/Entry now use the same contract.
- Changed encoding to return one complete `Fields` representation. Ownership of
  the returned map and byte slices transfers to Verdandi; decoding still
  receives a detached complete map and replaces the receiver. Removed the
  obsolete `value.go` split by keeping the contracts with `Fields` in
  `field.go`.
- Made raw `Fields` a safe first-class value through deep-cloning `Encode` and
  detached-input `Decode`. Removed Registration's raw-type special case and the
  redundant Catalog post-encode clone.
- Added direct detachment and nil-decoder regressions. Passed Go formatting,
  vet, ordinary tests, integration and load/soak compile-only gates, and Go
  1.27 WSL/Linux race tests. No Redis, long-running test, commit, or push was
  performed.

### 2026-08-27: Implement typed root Key and Hash commands

- Added Go `Ping`, `Key`, and `Hash` command groups with concise/Context method
  pairs, root admission and shutdown cancellation, fixed operation timeouts,
  stable validation, detached raw values, and conservative ambiguous-write
  handling. Hash struct descriptors are immutable and cached per concrete T.
- Froze the shared scalar contract: bytes, strings, `0`/`1` booleans,
  canonical fixed-width integers, and application-owned Go Binary/Text or Rust
  value traits. Machine-width integers, floats, pointers, and automatic
  JSON/Serde remain outside the contract.
- Added Rust `KeyCommands`, one-use `with_ttl` writes, `HashCommands`, the manual
  `HashValue` trait, and the separate `verdandi-derive` procedural-macro crate.
  Derived named structs support exact names, `redis(name = "...")`, `skip`, and
  missing-field defaults without reflection or dynamic dispatch.
- Enforced matching key, field-count, field-name, individual-value, and
  aggregate Hash ceilings. HSET results are intentionally discarded, HDEL and
  HLEN counts remain visible, and single-key DEL/EXISTS return bool.
- Passed Go root unit tests, Rust's 49 library tests plus two Catalog and two
  external Redis-API tests, workspace formatting, strict Clippy, and isolated
  Go/Rust Redis 8.8 integration including empty values, missing projections,
  TTL, HSET/HDEL, and WRONGTYPE classification. The randomly labelled remote
  container was removed by the owning fixture. No long test, Registration
  integration, commit, or push was performed.

### 2026-08-27: Adopt Go 1.27 and shared transport clients

- Raised the Go SDK and all Go peer modules to `go 1.27.0`. Replaced the
  unreleased package-level typed constructors with concrete generic methods:
  Registration and Selector belong to `registration.Client`, while typed
  Catalog loading belongs to `Entry`.
- Kept one root Go/Rust Client as the Redis transport and attached Registration
  and Catalog child Clients to it. Domain clients own their scripts, policy,
  workers, diagnostics, and optional persistence without duplicating the pool.
- Applied promoted embedded-field keys to explicit normalized transport
  construction, ran all four Go 1.27 modernizers cleanly, and used generalized
  generic-function assignment to replace retained Selector codec closures with
  static function instances.
- Removed the two Registration codec closures entirely. The constructor smoke
  benchmark changed from 288 B/five allocations to 240 B/three allocations.
  Alternating single-core tests found no significant hot-path difference from
  Go 1.27's automatic size-specialized allocator, so no speculative allocator-
  driven rewrite was made.
- Passed Go format, unit, vet, all integration/load/soak compile gates, and six
  Go/Rust peer builds. Rust passed 43 unit plus two non-Redis Catalog tests,
  strict format, Clippy, and documentation. Upgraded WSL to Go 1.27.0 and passed
  Linux formatting, shuffled unit, vet, all-tag compilation, three Go peer, and
  race gates. Native Windows race remains unavailable because CGO and a C
  compiler are absent.
- Updated current API, architecture, Alpha, decisions, test evidence, and
  onboarding documents. Per maintainer instruction, no live Redis or long test
  was started. Created no commit and performed no push.

### 2026-08-27: Adopt 160-column formatting for edited source

- Set 160 columns as the repository limit for newly written or materially
  edited handwritten source and exposed it through `.editorconfig`.
- Required the owning language formatter immediately after every source write;
  formatter-selected earlier breaks remain authoritative, while generated
  byte-exact artifacts and indivisible protocol literals are exempt.
- Added crate-local Rustfmt and Python Black configuration at 160 columns, then
  reformatted the repository's Go, Rust, and Python source after the maintainer
  explicitly authorized Registration reflow. No long-duration test was started.

### 2026-08-27: Pass complete Registration regression and exact two-hour gate

- Ran deterministic Lua generation, Python harness compilation, Go ten-repeat
  shuffled unit/vet and WSL/Linux race gates, two separate 30-second fuzz gates,
  Rust's 28 Registration tests, all test-target compilation, format, strict
  Clippy, rustdoc, live Redis 8 integration, and cross-language Sentinel.
- Reproduced two Sentinel second-promotion timeouts before changing source. The
  SDKs correctly failed closed, while Sentinel retained whichever promoted
  primary had just been killed. Added a bounded state gate requiring the sole
  surviving Redis replica to report a connected `ROLE` and appear through
  `SENTINEL REPLICAS` on all three Sentinels before total outage. The corrected
  matrix passed in 34.503 seconds with both generations `1 -> 2 -> 3`.
- Retained a rejected 7,200-second workload: all 3,600,000 Updates and 34 faults
  passed, but measured Redis server time was only 6,984.007 seconds and correctly
  failed the 7,200-second floor.
- The formal isolated run used 8,000 workload seconds and a 7,200-Redis-second
  floor. It measured 7,759.124 Redis seconds, completed 4,000,000 Updates and
  639,743 selection transactions, passed 34/34 faults, 25 expiry and 27 churn
  cycles, reported zero unexpected asynchronous errors, stable Redis memory,
  final revision 8,001/generation 15, goroutines `2 -> 528 -> 2`, and `DBSIZE=0`.
- Canonical Lua, Rust raw convergence, and Rust typed Registration/Selector
  passed after the workload. The main worktree remained on the exact 88-file
  fingerprint `feb767345b8b09323d53dea9c3ead5427be21ece7de668c55e9577eedf5173b0`.
- Confirmed the owned container and directory were absent and port 16440 was
  closed. Left the independent port-36440 test untouched. Detailed evidence is
  in `registration/full-regression-2h-20260827.md`. No commit or push was made.

### 2026-08-26: Optimize and review current Registration/Selector hot paths

- Kept Registration Lua byte-identical after another responsibility and
  hot-path audit; the four positional scripts remain atomic glue and the SDKs
  remain responsible for validation and decoding.
- Made Go and Rust selection transactions skip overlay reconciliation for an
  identical immutable view, changed ordered views from copied UUIDs plus map
  lookups to shared record references, and replaced per-call `Any` duplicate
  sets with reusable generation marks. Added ordering, identity, duplicate, and
  token-wrap regressions.
- On Go 1.26.4 WSL/Linux, ten-sample `benchstat` comparisons reduced 500-record
  view publication from 105.68 to 54.08 us (-48.83%, p=0.000) and typed `One`
  from 26.54 to 15.92 us (-40.02%, p=0.000). Publication bytes/op fell 11.50%.
- Passed deterministic Lua generation, Go unit/vet, ten shuffled Linux race
  repetitions, a 30-second/7,658,786-execution fuzz run, Rust's 28 Registration
  unit tests, format/strict-Clippy/rustdoc, and targeted optimization regressions.
- The exact 88-file fingerprint
  `79e5a4aae09bd01005becca57848087bcc17a39405b0ea5966e440fbbc39ba5d`
  passed a 90-second Redis 8.8 AOF fault gate with 500 Registrations, eight
  Selectors, 45,000 Updates, no Update retry, six injected faults, zero
  unexpected errors, stable memory, and an empty final database. The complete
  two-promotion Sentinel matrix preserved both SDK UUIDs and advanced both
  Selector generations `1 -> 2 -> 3`.
- Confirmed cleanup left zero owned containers, no owned soak directory, and all
  seven Redis/Sentinel ports closed. Recorded separate scoped scores of Lua
  10.0/10, Go 9.8/10, and Rust 9.7/10 with detailed strengths and deductions in
  `registration/optimization-review-20260826.md`.
- Did not modify or execute Catalog work and created no commit or push.

### 2026-08-26: Rebuild Catalog around Replace/Patch/Delete and Subscriber

- Replaced the former Hash/Stream Mirror protocol with one
  `verdandi:catalog:<zone>:<part>:<id>` Value/Array/Map Hash, Zone-global and
  per-field revisions, live/deleted ZSET indexes, full-operation Pub/Sub, and
  bounded tombstone floor.
- Generated strict Acquire, Release, Read, Replace, Patch, and Delete Lua copies
  for standalone, Go, and Rust. Lua remains atomic glue; SDKs own codecs,
  validation, Patch projection, and type semantics.
- Added independent Go `catalog` and Rust `verdandi::catalog` APIs with
  Publisher, one-reader/one-repair-worker Subscriber, stable Entry, generic
  per-call Load, reconnect/field repair, and disposable bbolt/redb persistence.
  Removed the old root Catalog public API and Stream feature.
- Made checkpoint entries and cursors monotonic across same-scope Subscribers;
  added bounded streaming MessagePack decoders that reject malicious declared
  sizes before allocation.
- Rebuilt Catalog integration, interoperability peer calls, sequential Redis
  benchmark, and interruptible 24-hour-capable endurance harness without
  invoking Registration tests. The direct 30-second preflight accepted all 960
  scheduled operations and converged a fresh final Subscriber.
- Replaced canonical README, protocol, architecture, SDK, Lua, decision, API,
  optimization, and testkit Catalog descriptions. Historical worklog and old
  result artifacts remain explicitly superseded evidence.
- Remaining maintainer decisions are independent Catalog Client ownership,
  fixed tombstone retention defaults, and whether to keep the internal
  65,536-field defensive ceiling in addition to the 512 KiB public byte limit.
  Later 2026-08-28 work resolved shared-root transport ownership and raised the
  configurable record ceiling to 4 MiB; this bullet remains historical scope.

### 2026-08-26: Migrate Registration and Selector into domain-owned SDK packages

- Moved the Go public API, implementation, package tests, embedded scripts, and
  saved fuzz corpus into `sdk/go/registration`. Added an internal capability-
  limited root-Client bridge without exporting Redis or re-exporting domain
  declarations from the root package.
- Moved the Rust public API, implementation, Selector, clock/deadline/event/
  pending helpers, Registration script transport, and Lua embeddings into
  `sdk/rust/src/registration`. Typed Registration now retains `Arc<ClientInner>`
  without incrementing the public Client-handle count, so dropping the final
  public handle still begins joined shutdown.
- Kept strongly typed Attr/Data and raw `Fields` on one generic API. Preserved
  delayed readiness publication, one queue/worker/timer per Registration, one
  persistent plus one temporary Selector task, Update coalescing, renew reset,
  fail-closed partial synchronization, retained TTL, and all four wire
  operations.
- Corrected the Registration Lua generator's Go target, moved the saved fuzz
  corpus with its package, updated standalone/load/soak selectors, rebuilt the
  source fingerprint around the child packages, and serialized Rust load
  profiles that share endpoint-wide Redis command statistics.
- Passed Go full tests/vet, Linux race with ten shuffled repetitions, 30-second
  fuzz with 7,342,037 executions, Lua generation and Redis 8.8 behavior, Rust
  format/all-target tests/Clippy/rustdoc, real standalone integration, current
  Rust release load, and external Sentinel peers.
- The exact final 88-file fingerprint passed a 90-second AOF fault gate with
  500 Registrations, eight Selectors, 45,000 Updates, six injected faults,
  generation three, zero unexpected errors, stable Redis memory, and final
  `DBSIZE=0`. A complete three-Redis/three-Sentinel matrix preserved both UUIDs
  and advanced both Selector generations `1 -> 2 -> 3`.
- Retained a 30-second rejected expiry-gate result and recorded one transient
  Sentinel topology timeout before the successful complete rerun. Detailed
  evidence and the 9.8/10 assessment are in
  `registration/package-migration-20260826.md` and
  `testkit/results/registration-package-migration-20260826.json`.
- Created no commit, tag, package publication, or push.

### 2026-08-26: Give Registration an explicit child-package boundary

- Added `registration/api.md` as the current Go/Rust public API and source-
  ownership proposal, parallel to `catalog/api.md`.
- Selected the noun `registration` for the public child package/module;
  `register` remains the readiness operation. Kept Selector in the same child
  because both APIs share Attr/Data, revision, lease, retained-view, and
  Registry synchronization contracts.
- Moved the Registration API, concurrency review, production review, two
  endurance reports, and Lua optimization review from the repository root into
  `registration/`. Updated repository-relative links without moving result
  artifacts or shared qualification harnesses.
- Recorded the source migration as pending until the public proposal is frozen.
  No implementation, wire format, test result, commit, or push changed.

### 2026-08-26: Align Client workers with Registration and Selector ownership

- Removed the eager configuration-refresh worker from Go and Rust Client open.
  Register still refreshes synchronously before validation. The first
  successfully published Registration now starts one Client-shared poller,
  concurrent Registrations reference-count it, and the last Registration
  cancels and joins it. A constructed but unpublished typed Registration starts
  no work; explicit refresh remains available with no Registration.
- Removed Selector's unnecessary immediate Registration-policy refresh.
  Confirmed and retained its existing connection-generation RedisClock behavior:
  full synchronization samples Redis `TIME`, the one persistent listener task
  recalibrates at `ClockRefresh`, and reconnect performs a new calibration. No
  second Selector clock task or goroutine was added.
- Added a Go reference-lifecycle unit regression and expanded Go/Rust live
  configuration integration to prove that a Selector repeatedly executes
  `TIME` without adopting changed Registration limits, that Register loads and
  polls current limits, that invalid policy retains the last valid snapshot,
  that the last Registration stops polling, and that explicit refresh still
  works afterward.
- Passed Go formatting, unit tests, vet, tagged compilation, and WSL/Linux race
  integration; Rust formatting, 44 unit tests, all targets, and strict Clippy;
  plus the isolated Redis 8.8 Lua, Go/Rust lifecycle, Catalog, shutdown, and
  interoperability suite. The accepted fixture used port 16421, left no keys,
  removed its owned container, and left the port closed. Evidence is
  `testkit/results/client-worker-lifecycle-20260826.json`.
- Repeated the isolated three-Redis/three-Sentinel recovery matrix. An initial
  attempt exhausted the 60-second second-promotion window while Sentinel kept
  reporting the excluded dead primary; all owned resources were still cleaned.
  The unchanged-source rerun passed in 31.813 seconds with two promotions,
  stable UUIDs, and Go/Rust Selector generations `1 -> 2 -> 3`. Evidence is
  `testkit/results/client-worker-lifecycle-sentinel-20260826.json`.
- Updated the public, architecture, SDK, decision, project-memory, test, and
  worklog documents. Created no commit and performed no push.

### 2026-08-26: Initialize the shared Catalog Stream Hub lazily

- Removed eager Catalog Hub startup from Go and Rust Client construction.
  A Client without a Catalog Mirror owns no Catalog Hub; the first Mirror
  initializes the one Client-shared blocking Stream reader. This change left
  the then-current configuration lifecycle untouched; the later entry above
  supersedes it with a Registration-scoped configuration poller.
- Serialized concurrent first-Mirror initialization without holding a mutex
  across Redis I/O. Go shares one explicit in-flight result and permits a later
  retry after failure; Rust uses cancellation-safe asynchronous one-time
  initialization. Every Mirror retains the resulting Hub handle, and Client
  shutdown waits for it only when it was actually created.
- Added live Go assertions that Client open and Catalog Patch do not create the
  Hub, plus concurrent first-Mirror and later-Mirror sharing coverage in both
  SDKs. One rejected preflight exposed an omitted test-fixture cleanup entry;
  the test was corrected and the complete suite rerun.
- Passed Go formatting, unit tests, vet, and WSL/Linux race detection; Rust
  formatting, 44 unit tests, all targets, strict Clippy, and rustdoc; and the
  final isolated Redis 8.8 Lua, Go/Rust integration, lifecycle, concurrent
  Mirror initialization, and interoperability suite. The accepted fixture used
  port 16420, ended with no Redis keys, removed its owned container, and left
  the port closed. Structured evidence is
  `testkit/results/catalog-hub-lazy-20260826.json`.
- Updated the public, architecture, SDK, API, decision, project-memory, test,
  and worklog documents. Created no commit and performed no push.

### 2026-08-26: Correct Registration ownership and usable Selector views

Historical status: the per-Registration sole-writer ownership and Selector
topology remain current; the 256-request queue/FIFO detail below was superseded
by the 2026-08-28 single-slot Fields mailbox entry above.

- Replaced the superseded Client-wide Registration coordinator with one
  independent 256-entry bounded queue, one synchronization worker/task, one
  desired/confirmed state, and one renewal timer per published Registration in
  both Go and Rust. Production processes are expected to own few
  Registrations; the 500/5,000 cases remain stress and pagination workloads.
- Implemented ordered coalescing of consecutive Update calls. Invalid calls are
  isolated; last Version/Data-field wins; non-Update operations are FIFO
  barriers; valid calls absorbed into one Redis write share its revision and
  outcome; and a folded state equal to confirmed state performs no Redis I/O.
- Made every confirmed real Update reset the next Renew deadline. Preserved
  liveness under no-op/invalid floods by following admitted work with Renew when
  the timer remains due.
- Kept every Selector at one persistent listener/state-machine worker and at
  most one temporary full-sync/targeted-repair worker. Targeted repair now marks
  the public view unavailable immediately. Raw and typed Snapshot, Find,
  FindRetained, One, and Any return explicit `unavailable` while half-synchronized.
- Added deterministic queue merge/order/no-op regressions, Update-versus-Renew
  timing coverage, raw/typed half-sync gates, and numeric runtime topology
  checks. The first live integration run exposed an empty-batch Go fast-path
  panic; fixed it and added a focused regression before accepting results.
- Passed Go/Rust static and unit gates, WSL/Linux race integration, canonical
  Redis 8.8 Lua contracts, standalone lifecycle, live interoperability, a
  30-second 500-Update/s load, 5,000-record sync, and a 210-second six-fault
  preflight. Observed Go goroutines `2 -> 513 -> 4` and Rust Tokio tasks
  `5 -> 521 -> 1/2` with 500 live Registrations. Reducing the queue to 256 and
  removing a redundant typed-Update copy cut observed comparable peak Go heap
  70.41-78.13% to a 35,014,568-47,374,384-byte range. A final-source independent Sentinel preflight passed two
  promotions and both Selector generations `1 -> 2 -> 3`.
  The final two-hour run and its automatic Sentinel tail later passed as
  recorded below.
- Recorded the corrected invariants, final 9.8/10 assessment, evidence,
  and trade-offs in `registration/concurrency-review-20260826.md`. All tests use
  isolated owned Redis resources and do not touch Catalog. Created no commit or
  push.
- A frozen-source audit caught that Go validated the initial Register reply but
  not later successful Update/Renew revision and timestamp values. Added one
  shared success validator, classified post-write `corrupt` with `ambiguous` as
  uncertain in both SDKs, and added a live Redis regression that injects false
  success replies and proves complete desired-state recovery. Rejected and
  cleaned the in-progress pre-fix endurance run, reran static, race, functional,
  load, six-fault, and two-promotion Sentinel gates, and froze fingerprint
  `c7bef517173b9c298e41b6dac272e78736b317c017bbe70ba838185960bdf63a`.
- Passed the authoritative run on that frozen 58-file fingerprint. Redis `TIME`
  measured 7,866.527 seconds; 500 typed Registrations completed 4,000,000
  Updates with seven retries and p50/p95/p99 0.649/1.044/1.427 ms. Eight
  Selectors completed 639,704 policy transactions and 703,672 local mutations;
  final revision/generation were 8,001/15.
- Passed all 34 standalone faults, 25 natural-expiry cycles over 3,200 records,
  27 explicit churn cycles over 432 records, canonical Lua, and Rust raw/typed
  convergence. There were 212 expected transient diagnostics, zero unexpected
  asynchronous errors, Go goroutines returned `2 -> 529 -> 2`, stable Redis
  memory decreased 55,840 bytes, and final `DBSIZE=0`.
- Passed the automatic 39.690-second three-Redis/three-Sentinel tail with two
  promotions, acknowledged-write loss, total Sentinel loss/restart, full-state
  republish, stable Go/Rust UUIDs, and both Selector generations
  `1 -> 2 -> 3`. Verified that every owned container, directory, key, and port
  was removed without using or changing Catalog resources.
- Explained the Go test-process 179,485,736-byte heap peak: exact percentile
  instrumentation retains 64,000,000 bytes for Update latency/schedule lag plus
  about 5.1 MiB of selection durations. The 71,985,416-byte final heap matches
  that test-only storage; shorter equal-topology preflights remain the comparable
  SDK memory evidence. Recorded O(operation count) duration retention as a
  harness limitation.
- Re-ran final Go formatting, unit, vet, tagged compilation, Registration Lua
  generation, and Rust formatting, 44 unit tests/all targets/features, strict
  Clippy, and rustdoc. Updated README, Codex memory, Alpha status, test guide,
  structured test results, and the detailed concurrency review. Created no
  commit and performed no push.

### 2026-08-26: Historical superseded Client-level Registration coordinator

- Replaced per-Registration Go workers and Rust tasks with one lazily created
  coordinator per Client. It is the sole Registration Redis writer, serializes
  the bounded request stream, keeps independent desired/confirmed state per
  UUID, and schedules all leases with one indexed minimum heap and one timer.
  Individual Registration handles create no worker.
- Rebuilt each Selector around one persistent Pub/Sub listener/state machine and
  one optional temporary synchronization task shared by full snapshot and
  targeted repair. The listener alone receives events and owns mutable state;
  temporary work is cancelled and joined, and targeted repair stays in the
  current generation.
- Found and fixed Rust lazy-deadline accumulation under long-TTL churn with an
  indexed heap and a 10,000-cycle removal regression. Added bounded,
  coalesced reclamation when implicit `Drop` cannot enter a saturated FIFO, and
  preserved explicit Close ordering. Corrected targeted repair so it no longer
  falsely advances Selector generation.
- Removed 500 test-owned producer goroutines and 500 per-Registration diagnostic
  watchers from the endurance workload. Added a process regression gate at
  baseline plus 128, retained eight Selector policy loops, and observed Go
  goroutines `2 -> 30 -> 2` with 500 live Registrations.
- Passed Go formatting/unit/vet/WSL race, Rust formatting/43 unit tests/all
  targets/strict Clippy/rustdoc, deterministic Lua generation, Redis 8.8
  functional integration, Go/Rust interoperability, and both-language load
  gates. At 500 Updates/s, Go p99 was 1.308 ms and Rust p99 was 2.984 ms.
- Passed the authoritative current-source endurance run with fingerprint
  `24da63ef5d057bf6b2410cbd5e35f491421e8512fb5bdb5b180d0253cfd3b601`:
  7,388.601 Redis seconds, 4,000,000 Updates, 639,722 selection transactions,
  703,698 local mutations, all 34 faults, zero unexpected asynchronous errors,
  28,300 bytes stable Redis-memory growth, and final `DBSIZE=0`.
- Passed canonical Lua and raw/typed Rust post-checks, then the 90.444-second
  three-Redis/three-Sentinel matrix. Go and Rust Selector generations both
  advanced `1 -> 2 -> 3` through acknowledged-write loss, all-Sentinel outage,
  Sentinel restart, and two primary promotions.
- Recorded the topology, exact evidence, 9.8/10 score, strengths, weaknesses,
  rejected pre-fix run, and reproduction paths in
  `registration/concurrency-review-20260826.md`, `test-results.md`, SDK/protocol
  documents, and the testkit guide.
- Used only isolated Registration/Selector fixtures, confirmed all owned Redis
  and Sentinel ports closed, did not use the Catalog test endpoint, and created
  no commit or push.

### 2026-08-25: Optimize and production-qualify direct typed Registration/Selector

- Profiled the complete Go 500-candidate typed Selector transaction on
  WSL/Linux. Removing a redundant clone of the application encoder's owned
  destination reduced the comparable ten-sample median by 4.57%, bytes by
  24.21%, and allocations by 20.37%. Final `b.Loop` reference is 19.945-21.726
  us/op, 3,881 B/op, and 43 allocs/op.
- Converted the principal 500-Registration/eight-Selector soak workload and
  basic Go/Rust Sentinel integration paths to the public direct typed APIs.
  Added real `One`/`Any` transactions, local `Power` mutation, field-granular
  remote correction, exact final prediction checks, fail-fast cancellation,
  and typed Rust post-checks. Raw compatibility remains covered by lifecycle
  sub-workloads.
- Expanded the qualification source fingerprint to 58 Registration/Selector,
  Lua, Go, Rust, soak, and Sentinel files. Added credential-safe harness errors
  and durable structured failure evidence.
- Passed formatting, static analysis, shuffled Go tests, WSL/Linux race, a
  30-second/7,920,411-execution Registration fuzz run, Rust strict Clippy,
  42 Rust unit tests, rustdoc, Lua generation, a 150-second typed fault
  preflight, and a typed Go/Rust Sentinel preflight.
- Rejected the first formal attempt because Redis `TIME` measured only
  7,138.759 seconds against the 7,200-second floor after a WSL clock jump,
  despite the Go workload itself passing. Preserved that failure instead of
  mislabelling it as a two-hour pass.
- Passed the margin-adjusted authoritative run: 7,608.409 Redis seconds,
  4,000,000 typed Updates, 639,713 selection transactions, 703,688 committed
  local mutations, 34/34 standalone faults, zero unexpected asynchronous
  errors, bounded Redis memory, and final goroutines 2 after a 1,553 recovery
  peak. Lua, Rust raw/typed convergence, and the complete two-promotion
  Sentinel matrix passed afterward; final `DBSIZE` was zero.
- Recorded exact results, performance evidence, 9.8/10 overall assessment,
  strengths, and weaknesses in `registration/typed-soak-20260825.md`,
  `registration/production-review-20260825.md`, and `test-results.md`.
- Used isolated port `36443`, did not touch the Catalog endpoint, cleaned every
  run-owned resource, and created no commit or push.

### 2026-08-25: Replace SDK code generation with direct typed APIs

- Replaced the Go Registration `Schema` and `verdandi-codegen` design with
  application-owned `FieldEncoder`/`FieldDecoder`; Rust now exposes the
  equivalent `FieldValue` trait. Raw `Fields` implements the same generic API
  in both languages.
- Added local-only typed Registration construction with a fresh stable UUID and
  explicit delayed Register readiness. Complete typed Data updates keep their
  fixed field shape while emitting only changed Redis Hash fields.
- Added generic Go and Rust Selectors with synchronous `One` and `Any` policy
  transactions, borrowed immutable views, explicit staged mutation, complete
  rollback, detached results, and no Redis I/O on selection paths.
- Added field-granular local prediction reconciliation: Renew and unrelated
  remote changes preserve predicted Data fields, while an authoritative remote
  change to the same field corrects it.
- Retained the existing raw Registration/Selector entry points as compatibility
  aliases over the same lifecycle core; removed the obsolete Go generator and
  its generated-schema fixtures.
- Added unit, rollback, cancellation, aliasing, reconciliation, live Redis 8.8,
  and 500-candidate policy benchmark coverage. Detailed public contracts are in
  `registration/api.md`; final verification is recorded in
  `test-results.md`.
- Created no commit and performed no push.

### 2026-08-29: Expand cross-language TLS and finalize Catalog lock retention

- Replaced the v1 JSON `redis.tls` boolean with one strict object containing
  enablement, system-root selection, Standalone SNI override, private PEM CA
  bundle, and paired PEM client certificate/private-key paths.
- Kept JSON parsing free of certificate I/O. Go performs bounded PEM loading
  while converting to its native `*tls.Config`; Rust added a language-native
  `TlsConfig` and performs bounded PEM loading while constructing the Fred
  rustls connector before network initialization. Every PEM file is capped at
  1 MiB, TLS requires peer verification and TLS 1.2+, and no insecure bypass is
  exposed.
- Restricted fixed `server_name` to Standalone in both SDKs. Fred cannot
  propagate a fixed SNI override to a new primary discovered through Sentinel,
  so accepting it only in Go would create a false cross-language contract.
- Added a public, test-only self-signed PEM fixture and paired tests for valid
  private roots, mTLS parsing, SNI injection, deferred file I/O, legacy boolean
  rejection, empty trust sets, missing key pairs, and Sentinel SNI rejection.
- Finalized retention of the external token-fenced Catalog Path lock. Catalog
  Publisher is expected to be a single writer or one of a small number of
  nearby writers, so contention is exceptional and the acquire round trip is
  accepted to preserve SDK-owned Patch projection. TTL, finite acquisition
  timeout, atomic success release, and best-effort unconfirmed release remain.
- Updated the canonical schema, complete example, configuration/API/SDK
  references, decisions, review, and test evidence. The short regression passed
  Go test/vet, Rust 68 library plus four offline external tests, strict Clippy,
  warning-denied rustdoc, both Lua generator checks, and schema/example JSON
  syntax parsing. Twelve endpoint-dependent Rust tests remained explicitly
  ignored; no live TLS topology qualification is claimed. No commit or push was
  created.

### 2026-08-25: Optimize and review Lua/Go/Rust Register and Selector

- Profiled Go Selector Update/Renew and pending-event application on WSL/Linux.
  The original allocation profile attributed 85.97% of allocated objects to
  `bytes.Clone`; after safe ownership transfer, complete record-size scans were
  the remaining dominant avoidable work.
- Made internal field bytes immutable and shareable while retaining detached
  public values. Go now shallow-copies record maps, lazily creates repair work,
  takes ownership of decoded Register fields, resets successful-sync retry
  backoff, and maintains an exact cached record size. Rust moves Update values,
  shares immutable Data through `Arc`, and maintains the same size invariant.
- Added Go benchmarks and boundary regressions for version preservation,
  decimal-width size changes, projected oversize rejection, Update, Renew, and
  the no-repair pending drain. Added Rust detached-record alias coverage.
- Ten-sample paired Go results on Linux/amd64 improved Update from 4.103 to
  1.300 us/op, Renew from 3.083 to 1.236 us/op, and the no-repair drain from
  38.25 to 19.18 ns/op. Update/Renew fell from 6,984 bytes and 37 allocations
  to 2,888 bytes and five allocations; the ordinary drain reached zero bytes
  and zero allocations.
- Kept the fully line-audited Lua bytes unchanged. Generator checks and exact
  Go/Rust embedded hashes pass; speculative literal/local/helper changes have
  no stronger evidence than the accepted scripts.
- Passed Go unit/shuffle/static/format/Linux-race checks, Rust unit/format/
  Clippy/rustdoc checks, and the post-change isolated Redis 8.8 Registration
  contract, Go/Rust integration, lifecycle, ceiling, and live interoperability
  matrix.
- Did not start competing long/load/fuzz/Sentinel/MSRV reruns after the Catalog
  24-hour test became active. A just-started 30-second compressed smoke was
  stopped, its separate port was closed, and its p99 failure is retained only
  as explicitly non-qualifying diagnostic evidence.
- Recorded the detailed review, measurements, scores, trade-offs, completed
  checks, and isolation-deferred gates in
  `registration/production-review-20260825.md` and
  `testkit/results/registration-production-review-20260825.json`.
- Created no commit and performed no push.

### 2026-08-25: Optimize Catalog Lua and pass the bounded fault gate

- Built a 19-scenario, 21-pair Redis 8.8 comparison against the cached original
  Patch/Delete/Compact SHAs with alternating order and command-stat timing.
- Promoted only stable changes: present-state `HMGET` type validation, direct
  one-field comparison, allocation-free contiguous multi-delete lookup,
  trailing-9 decimal revision increment, and action-local argument bindings.
- Improved every candidate median, from 1.69% through 35.09%, while preserving
  exact `MAX_INT64`, corruption, tombstone, replay-floor, binary, and no-op
  contracts. Canonical, Go, and Rust generated scripts remain byte-identical.
- Added an interruptible AOF endurance harness with Redis/runtime sampling,
  source fingerprinting, structured heartbeats, fault injection, exact fixture
  cleanup, and durable partial results on console interruption.
- A steady run found a real same-Catalog Go bbolt replacement race. Added a
  deterministic 32,768-field interleaving regression, serialized complete
  replacement sequences only per Catalog with a draining keyed lock, and added
  explicit missing-bucket checks. Windows repetition and WSL/Linux race count
  10 pass; Rust's one-transaction redb replacement is not affected.
- Passed the corrected 120-second production-parameter gate: 15,360 Patch
  operations, 60 Delete/restore/Compact cycles, 122 complete Mirror checks,
  all injected faults, zero async errors, p99 2 ms, maximum Stream length 241,
  bounded Redis memory, runtime cleanup, final `DBSIZE=0`, and fresh Lua/Rust
  post-checks.
- Retained the authoritative machine evidence in
  `testkit/results/lua-catalog-line-final-20260825.json`,
  `testkit/results/catalog-soak-2m-steady-20260825.json`, and
  `testkit/results/catalog-soak-interrupt-rehearsal-20260825.json`.
- Created no commit and performed no push.

### 2026-08-25: Pass the two-hour Registration/Selector endurance qualification

- Added a reproducible authenticated Redis 8.8 AOF soak harness for 500 live
  Registration writers and eight Selectors. It records every update and
  scheduling latency, samples Redis every 30 seconds, runs parallel natural-
  expiry/retained and explicit-churn cycles, checks final exact convergence,
  and gates Go goroutine return and stable Redis memory.
- Injected and passed 34 faults: 14 script-cache flushes, 11 complete Pub/Sub
  connection kills, four three-second Redis pauses, three AOF restarts, and two
  ordinary-connection kills. The run completed 3,750,000 Updates, 25 expiry
  cycles covering 3,200 records, and 25 explicit churn cycles covering 400
  records with zero unexpected asynchronous errors.
- Added an independent Redis `TIME` duration gate after an earlier WSL run
  exposed a 210-second cross-clock discrepancy. The authoritative run scheduled
  7,500 Go seconds and measured 7,263.649 Redis-server seconds against a hard
  7,200-second floor.
- Final update p50/p95/p99 were 0.759/1.463/2.386 ms. Stable Redis memory grew
  634,476 bytes against a 2 MiB gate; evictions, rejected connections, and final
  database size were zero. Go goroutines returned from a 1,541 peak to the
  initial two.
- Ran from an isolated source copy whose 32-file fingerprint matched the
  working tree before and after qualification, preventing concurrent Catalog
  edits from invalidating the executable. Canonical Lua, Rust convergence, and
  the complete two-promotion Go/Rust Sentinel matrix passed afterward.
- Preserved the standalone checkpoint before a report-only absolute-path bug,
  fixed the formatter, reran Sentinel, and finalized machine-readable JSON plus
  243 raw JSONL samples. Detailed design, results, scores, strengths, and gaps
  are in `registration/soak-20260825.md` and `test-results.md`.
- Verified all run-owned Redis/Sentinel containers, directories, and ports were
  absent. Created no commit, tag, release, or push.

### 2026-08-25: Unify Catalog Stream reads and gap recovery across SDKs

- Replaced per-Mirror blocking reads with one Client-level Catalog Stream Hub
  in both Go and Rust. One dedicated connection issues a dynamic multi-key
  `XREAD`; Lua writes, header reads, `XRANGE`, `HSCAN`, and checkpoint work stay
  on the ordinary Client command path.
- Added per-subscription revision tracking and one-event local acknowledgement.
  A slow Mirror therefore holds at most one decoded large event and cannot
  block unrelated Catalog streams or ordinary writes.
- A reconnect/trim gap now detaches only the affected Mirror. Its existing
  worker marks the retained snapshot unsynchronized, prefers exact `XRANGE`
  replay when tomb/floor permit, and falls back to complete `HSCAN` plus catch-up
  only when the delta is unavailable. Redis consumer groups remain excluded.
- Corrected the wholly missing-tail case: a later Hash header now triggers full
  comparison when exact `XRANGE` is empty instead of retrying a permanent
  corruption forever. The Mirror emits one bounded corruption diagnostic,
  starts continuity recovery immediately, and reserves retry backoff for real
  transport failures. Revision regression does not masquerade as corruption.
- Reused the Go Hub's 1,000-Mirror position map and work slices. The isolated
  Windows position-planning median improved from about 87.56 to 26.66 us/op
  (`-69.6%`), from 108,760 bytes/op and 20 allocations/op to zero and zero.
- Added Go and Rust unit coverage for multi-Mirror stream coalescing,
  acknowledgement advancement, continuity gaps, and isolation. Passed 41 Rust
  unit tests and the complete Go suite locally.
- Passed the final isolated Redis 8.8.0 functional matrix on port 36434 after
  injecting a real `1 -> 3` Stream hole into both SDK integration paths: 10 Lua,
  Go, Rust, Linux-race, lifecycle, and interop suites, 3,378 commands, and
  4,118,720 bytes peak Redis memory. Both Mirrors recovered through the full
  comparison fallback. The harness removed its exact fixture and did not touch
  the fixed Register/Sentinel environment.
- Repeated the final-code matrix on fresh port 36436 with both the middle hole
  and a Hash-only revision `3 -> 4`: all 10 suites passed, including WSL/Linux
  race and Go/Rust live interoperability, across 3,669 Redis commands with
  4,119,392 bytes peak memory. Both Mirrors converged to revision 4, the harness
  removed its exact fixture, and the port was confirmed closed.
- Created no commit and performed no push.

### 2026-08-24: Complete Rust Catalog<T> parity

- Added public Rust `Catalog<T>`, `CatalogValue`, and
  `TypedCatalogSnapshot<T>`. External structures own static raw-field
  encode/decode rules; no reflection, dynamic codec, or type information enters
  Redis, Stream events, or redb.
- Added serialized typed Publish/Delete/Compact operations over the existing raw
  Mirror. Complete Values are deterministically diffed and split under the same
  field/byte limits as Go, and each changed call waits for local observation of
  its final Redis revision.
- Cached each decoded revision as `Arc<T>`. Repeated Snapshot and floor-only
  Compact reuse the same allocation; deleted/absent state is `None`, while a
  live empty external type remains `Some(Arc<T>)`.
- The multi-Mirror live test exposed blocking `XREAD` head-of-line blocking on
  the shared Fred command connection. The initial per-Mirror reader fix was
  later superseded by the 2026-08-25 cross-language Client-level Stream Hub.
- Added unit coverage for deterministic diff splitting, complete/single-mutation
  capacity, and non-`Default` typed snapshots. Expanded real-Redis integration
  across cached snapshots, no-op publication, Compact, Delete, non-empty and
  empty resurrection, post-close reads, and rejected post-close writes.
- Passed 39 Rust unit tests, all targets, denied-warning Clippy, rustdoc, Rust
  1.85, and the isolated Redis 8.8 functional/interop matrix on port 36432. The
  successful fixture processed 3,187 commands, left an empty database, removed
  its exact container, and did not use an existing Register endpoint.
- Created no commit and performed no push.

### 2026-08-24: Production-review and optimize Catalog across Lua, Go, and Rust

- Hardened the three Lua operations around empty-Value resurrection, exact
  metadata relations, orphan/corrupt key rejection, and 1,024-field Redis
  command chunks. A 4,097-field contract case proves large SDK batches do not
  depend on Lua `unpack` or one unbounded Redis argument vector.
- Added strict bounded Go and Rust MessagePack event decoders that reject
  declared allocation bombs before expansion. Go fuzzed the Catalog decoder for
  60 seconds and 20,405,860 executions without failure.
- Removed duplicate Go event-value cloning and Rust intermediate event arrays.
  Paired Linux medians improved Go Patch apply by 14.3% and the 1,000-field
  decoder by 12.0%, with decoder memory reduced by 28.6%.
- Made bbolt/redb checkpoints monotonic across multiple Mirrors, idempotent
  under concurrent persistence, and strict about transition/header validity.
  Redis remains authoritative and local persistence remains default-off.
- Corrected Rust blocking-XREAD null handling, relative checkpoint filenames,
  diagnostics shutdown, and last-snapshot preservation after close. The later
  Rust `Catalog<T>` follow-up keeps this wire contract raw.
- Passed generated-source checks, Go shuffled repetition/vet/Go 1.24/race/fuzz,
  Rust all-target/formatter/Clippy/rustdoc/Rust 1.85 checks, Lua 8.8
  corruption and 4,097-field cases, checkpoint restart, and live bidirectional
  Go/Rust interoperability.
- Passed the final isolated 14-suite Redis 8.8.0 matrix: 482,079 commands,
  5,291,816 bytes peak memory, Go and Rust 120-second update/renewal loads, and
  5,000-record recovery. The harness verified an empty database and removed
  only its random fixture; no existing Register test endpoint was used.
- Recorded the initial 9.1/10 Catalog engineering score; the later Rust typed
  parity follow-up supersedes its typed-Rust limitation.
- Created no commit and performed no push.

### 2026-08-24: Decouple Campaign and election Version from Registration

- Audited the inherited Hermes Primary dependency. Hermes needs a live service
  registration because its Primary is a routable service snapshot; Verdandi's
  generic Leader does not, and readiness already proves claimant liveness.
- Moved election identity and immutable Version to a fresh Campaign lifetime.
  The readiness token is its internal identity; no public Campaign ID is added.
  Campaign can run with or without Registration; changing priority requires a
  fully closed Campaign and a new readiness-token/Version pair.
- Left Registration Version, Register, Update, and re-registration semantics
  unchanged. Leader does not read or react to any Registration mutation.
- Retained SDK-driven ready-view comparison, bounded atomic Redis Claim,
  exact-token ownership, strict zero-or-one application activation, and the
  mandatory Sentinel fence plus post-fence Redis-term confirmation.
- Updated the protocol, architecture, Alpha, decision, SDK, public introduction,
  and canonical onboarding documents. Campaign implementation and
  qualification remain future work.
- Created no commit and performed no push.

### 2026-08-24: Implement and qualify Catalog Value synchronization

- Replaced the stale multi-record/TTL/Pub/Sub-barrier design with one raw Hash
  Value plus a bounded revision-ordered Stream. Accepted `MAX_INT64`, Redis-
  execution-order LWW, explicit whole-Value tombstones, and monotonic replay
  floors.
- Added three deterministically generated Lua programs and byte-identical Go and
  Rust embeddings. Lua owns only atomic Hash/Stream/revision glue; SDK Publisher
  code owns external typing, validation, differencing, ordering, and splitting.
- Added Go and Rust Patch/Delete/Compact APIs, complete in-memory Mirrors,
  synchronized last-known snapshots, full Hash fallback, and joined recovery
  workers.
- Added default-off disposable bbolt and redb checkpoints. Redis remains
  authoritative; stale/corrupt/mismatched local state never writes back.
- Passed canonical generation, unit/static checks, isolated Redis 8.8.0 Lua and
  SDK integration, checkpoint restart, Delete/recreate/compact, exact
  `9223372036854775807`, WSL/Linux race, and Go/Rust interoperability. The test
  fixture verified empty state and removed only its random container.
- Created no commit and performed no push.

### 2026-08-24: Freeze SDK-driven strict Leader policy

- **Partially superseded:** strict zero-or-one activation, SDK-driven selection,
  and Sentinel fencing remain accepted. The Registration Version conclusions
  below were removed from the current contract scope; existing behavior remains
  unchanged for later independent work.

- Made Registration Meta `@version` immutable for one process-start UUID and
  removed dynamic version mutation and `version_revision` from the target
  protocol. A different election version now requires a new Registration UUID.
- Audited same-UUID Register recovery and fixed it as immutable-state
  restoration: an existing Version/TTL/Attr mismatch is `immutable`, while an
  absent Hash is restored with the original values retained by the live SDK
  process. Re-registration cannot change election priority.
- Fixed Leader policy to zero or one application-active term. The SDK owns
  ready-view synchronization, numeric comparison, Claim decisions, retirement,
  renewal, invalidation, and callback lifetime; Redis owns only atomic
  readiness/current-owner validation, exact-token mutation, and lease state.
- Removed the proposed availability mode that admitted short work through
  Sentinel overlap. Standalone uses its configured Redis primary; Sentinel
  requires an SDK-invoked deployment fence after Redis claim and before
  application callback admission. Failure to fence leaves the domain without a
  Leader.
- The later scope correction restored Registration documentation to the current
  Go/Rust and Lua behavior. No Registration source correction belongs to the
  Leader task.
- Created no commit and performed no push.

### 2026-08-24: Complete the second Registration Lua line audit

- Removed every one-call-site state, clock, deadline, expiry, and generic error
  helper from successful Lua execution. Fixed errors are literal named arrays,
  so no production script repeatedly evaluates `#reply`; Register/Update use one
  explicit next-write event index.
- Bound `ARGV` only in Register/Update, where a dynamic tail rereads it, and
  specialized Renew/Unregister bindings. Lua string literals such as
  `"@revision"` remain compiled constants rather than per-invocation locals.
  A first/recovery Register skips redundant `DEL` when both known prior Meta
  fields are absent; valid existing records still receive complete replacement.
- Measured and rejected modulo TIME truncation, removal of the repeated
  `tonumber` local, implicit arithmetic request conversion, absent-state
  short-circuit conversion, a local `KEYS` table, and caching the two Update
  version-presence comparisons as `has_version`. The last candidate was -0.23%
  for one-field Update and -0.16% for 32-field Update by median server time;
  each rejected candidate failed paired direction/win-count consistency and
  was removed.
- The final generated sizes are Register 3,542, Update 3,955, Renew 2,771, and
  Unregister 1,010 bytes: 11,278 total, 23.61% below the preceding 14,763-byte
  positional set. Canonical/Go/Rust copies are byte-identical.
- In 21 same-Redis alternating pairs against the preceding production SHAs,
  server-time medians improved by 9.03% for small Register (21/21), 7.65% for
  default-maximum Register (20/21), 6.66% for one-field Update (21/21), 6.18%
  for versioned Update (21/21), 7.12% for 31-field Update (21/21), and 7.25%
  for Renew (21/21). A corrected canonical 32-field current-source benchmark
  passes at 19.74 microseconds. Unregister remained neutral at 3.72 microseconds.
- Passed the canonical Redis 8.8 Lua contract, generation freshness, Go
  shuffled/unit/`vet`, WSL real-Redis race, 60-second/25,420,541-execution fuzz,
  Rust 31-unit/all-target/formatter/Clippy/rustdoc, both SDK integrations, and
  bidirectional interoperability.
- Repeated four five-minute 500-writer/eight-Selector phases. Go/Rust Update
  each completed 150,000 at 500.0/s; Renew completed 149,597 at 498.7/s and
  148,610 at 495.4/s. Five-thousand-record synchronization completed in 56.489
  ms and 322.066 ms. The final two-promotion Sentinel matrix passed in 152.842
  seconds with UUID preservation and generations `[1,2,3]` in both SDKs.
- Retained machine-readable accepted/rejected microbenchmarks and final
  standalone/Sentinel JSON, updated project documentation, and verified no
  remote test containers, temporary directories, or dedicated-port listeners
  remain. Created no commit, tag, release, or push.

### 2026-08-24: Promote and fully qualify positional Registration Lua

- Accepted all four measured optimization decisions. Fixed SDK-to-Lua control
  values now occupy operation-specific slots without repeated control names;
  dynamic Attr/Data remain canonical field/value pairs. Replies and Pub/Sub
  events remain named alternating arrays. A future incompatible request layout
  requires a new script/SHA.
- Removed the generic Lua request Hash and payload write table, passed Register
  and Update tails directly from `ARGV`, used multiple-return state reads,
  numeric Redis-generated time/deadline arguments, inlined fixed success and
  publication blocks, and replaced membership `HSET` plus `HPEXPIREAT` with one
  Redis 8 `HSETEX PXAT`. Updated generator headers with each exact ABI.
- Corrected the Redis 8 Hash-field absolute-expiry boundary to `2^46-1`
  milliseconds in Lua, Go, Rust, and fetched/event deadline admission. Exact
  ceiling, ceiling-plus-one, rollback, key TTL, and membership field-TTL tests
  pass. The four canonical scripts total 14,763 UTF-8 bytes versus 19,948
  before this promotion.
- Preserved the pre-promotion eleven-pair Register evidence: the final candidate
  improved Redis server time by 28.68 percent for 2 Attr/2 Data and 25.40
  percent for default 16 Attr/32 Data, positive in every pair. Fresh production
  medians are 10.15 microseconds small Register, 39.26 default-maximum Register,
  10.28 one-field Update, 10.46 version-plus-Data Update, and 9.68 Renew.
- Passed canonical Redis 8.8 Lua contracts; Go generation, unit, shuffled,
  `vet`, WSL/Linux real-Redis race, and 60-second/19,697,832-execution fuzz;
  Rust 31-unit/all-target/formatter/Clippy/rustdoc checks; Go/Rust standalone
  integration and interoperability; and raw SDK-bypass boundary tests.
- Repeated all four formal five-minute phases with 500 live writers and eight
  Selectors. Go/Rust Update each sustained 150,000 operations at 500.0/s;
  Renew sustained 149,637 at 498.8/s and 148,542 at 495.1/s. Final 5,000-record
  synchronization was 55.065 ms in Go and 89.750 ms in Rust.
- Repeated the final three-Redis/three-Sentinel matrix in 48.281 seconds. Both SDKs
  retained UUIDs and converged through acknowledged-write loss, two promotions,
  `SCRIPT FLUSH`, minority stale Sentinel state, complete Sentinel loss, and
  resolver recovery. Results are in the final `*-positional-20260824.json`
  files plus the two production Lua benchmark JSON files.
- Verified the final database was empty and no Verdandi test containers,
  temporary directories, or listeners remained on ports 16381-16383 or
  26381-26383. Updated protocol, architecture, Alpha, SDK, decisions, Lua,
  testkit, optimization, result, README, Codex, and worklog documentation.
  Created no commit, tag, release, or push.

### 2026-08-24: Measure Register Lua line-optimization candidates

- Added an executable, test-only candidate builder for isolated and cumulative
  Register Lua rewrites. Eleven alternating Redis 8.8 trials cover both a
  2-Attr/2-Data record and the default 16-Attr/32-Data record with 128-byte
  values; every candidate verifies stored state, membership, matched expiry,
  reply, publication path, and cleanup.
- Measured the final fixed-header/direct-HSET/numeric/inlined/`HSETEX` candidate
  at 10.19 versus 14.23 microseconds for the small shape and 38.70 versus 51.83
  microseconds for the default-maximum shape. Paired server-time improvement
  was 28.68% and 25.40%, positive in all eleven trials. Wall throughput remains
  secondary because its maximum-payload direction disagreed with server time.
- Discovered and reproduced Redis 8's distinct Hash-field absolute-expiry
  ceiling: `2^46-1` milliseconds is accepted, while the larger Lua safe-integer
  maximum used by the current generic deadline check is rejected. Documented
  the required correctness fix, exact v1 positional-ABI tradeoff, candidate
  matrix, promotion gates, and steady-state Update/Renew follow-up in
  `registration/lua-optimization.md`.
- Left all canonical and embedded production Lua unchanged. Created no commit,
  tag, release, or push.

### 2026-08-24: Generate specialized Registration Lua glue

- Replaced the single generated Registration executable with `register`,
  `update`, `renew`, and `unregister` programs. Common Redis-state, clock,
  reply, and publication behavior remains maintained once as reviewed fragments
  under `lua/src/registration`; one explicit manifest and deterministic
  generator produce the canonical files plus byte-identical Go/Rust copies.
- Kept one selected `EVALSHA` per mutation. The selected SHA is the operation
  dispatch; readable request `&kind` is not reparsed in Lua. Selector still
  owns no Lua snapshot.
- Changed Go and Rust Clients to load four SHAs, dispatch by operation, and
  reload only the selected script after `NOSCRIPT`. Rust heap-pins concurrent
  script-load futures to keep bootstrap stack use bounded.
- Extended the real-Redis fixture with separate cache-loss/reload cases for all
  four scripts and a raw oversized-field bypass that demonstrates the SDK/Lua
  validation boundary. Added a paired isolated Redis 8.8 specialization
  benchmark and machine-readable results.
- Repeated the complete isolated functional/race/interoperability suite, the
  three-Redis/three-Sentinel two-promotion matrix, and all four formal
  five-minute Go/Rust Update/Renew phases with eight Selectors. Update sustained
  500.0/s in both languages; Renew sustained 498.8/s in Go and 495.1/s in Rust;
  both subsequently converged and cleaned 5,000 Registrations.
- Removed duplicate Lua request/schema/capacity validation. The SDK remains the
  sole owner of those checks; Lua now retains only Redis-state-dependent atomic
  conditions and writes. Four generated bodies fell from 44,133 to 19,948 UTF-8
  source bytes. Against a test-only minimal combined reconstruction, eleven
  paired trials measured Update at 15.67 versus 15.66 microseconds and Renew at
  14.31 versus 14.51 microseconds: the split is effectively runtime-neutral and
  exists for maintenance boundaries.
- Updated protocol, architecture, Alpha, SDK, coding, decision, testkit, and
  maintainer documentation. Repeated the post-boundary functional suite, four
  formal five-minute phases, and two-promotion Sentinel matrix. Verified the
  final comparison database had zero keys, removed only
  `verdandi-lua-glue-final-20260824`, and confirmed the functional, long-load,
  Sentinel, and benchmark resources and dedicated ports were absent. Created
  no commit, tag, package release, or push.

### 2026-08-24: Complete, optimize, and long-qualify Registration/Selector

- Clarified the Registration durability boundary throughout the contracts and
  SDK documentation: writer recovery state is volatile process memory only,
  process restart never restores an old UUID, Selector views are non-durable,
  and historical persistence remains external to the core SDK.
- Added Redis-owned `configuration_refresh_ms` with a 30-second default,
  1-second through 24-hour range, plus/minus-ten-percent jitter, immediate
  refresh, last-valid fallback, and live Go/Rust integration coverage.
- Implemented independent non-selectable retained views in Go and Rust. Natural
  expiry or fenced absence retains payload for one additional TTL; explicit
  Unregister purges it; valid same-UUID state reactivates it. Defaults are 64
  MiB, zero disables, 1 GiB maximum, earliest-deadline eviction.
- Added same-revision `HMGET @revision @timestamp` reconciliation so active or
  retained content avoids `HGETALL`; changed revisions still fetch and validate
  complete records under the subscribed PING/PONG proof.
- Added Go generic typed Registration/Selector/Snapshot/retained APIs plus
  deterministic `verdandi-codegen` output for tagged flat primitive/byte
  structs. Generated codecs use canonical big-endian/scalar bytes, one shared
  capacity-limited output slab, defensive cloning, and no reflection on encode
  or decode hot paths. Typed integration verifies patch-only writes and cached
  projections.
- Replaced the generic Go event decoder with a bounded flat MessagePack reader.
  Paired median improved by 41.72 percent, bytes by 45.89 percent, and
  allocations from 26 to 12. Added all-width, invalid-container, impossible-
  length, saved-corpus, and 60-second/15,570,690-execution fuzz coverage.
- Corrected the load generator to 500 continuously live writers evenly updating
  once per second. Go and Rust release each completed 150,000 updates at 500.0/s
  for five minutes with eight Selectors, then separate five-minute 500-writer
  renewal phases at 498.7/s and 495.0/s while revision stayed one. The earlier
  617.6-621.4/s burst calculation is explicitly superseded.
- Passed 5,000-record paginated synchronization (Go 60.3 ms, Rust 77.8 ms),
  maximum 128-Attr/128-Data record recovery, authenticated standalone Redis
  integration, Go real-Redis Linux race, Go 1.24, Rust 1.85, Clippy/vet/docs,
  generated-fixture freshness, bidirectional interop, and the full two-
  promotion Sentinel matrix.
- Added isolated standalone and Sentinel JSON result output. Formal raw evidence
  is under `testkit/results/`; exact environment, metrics, score, strengths,
  weaknesses, and remaining gaps are in `test-results.md`.
- Cleaned only exact test resources. Created no commit, tag, package release, or
  push.

### 2026-08-23: Optimize and fault-qualify Selector and Sentinel recovery

- Replaced scan-time raw-event retention in both SDKs with a reader-side,
  bounded one-logical-change-per-UUID accumulator. Contiguous Updates merge by
  top-level field, Renew raises timestamp only, Register replaces complete
  pending state, Unregister is terminal, and gaps become targeted repair.
  Entry and encoded-byte ceilings are transactional; overflow abandons the
  generation instead of publishing incomplete state.
- Made the Go common merge path take ownership of decoded event fields and
  mutate pending state in place, while retaining a transactional copy only near
  the byte ceiling. The Linux 32-Update benchmark improved from approximately
  11.1 microseconds, 13,168 bytes, and 97 allocations to 5.64-5.69
  microseconds, 112 bytes, and one allocation. Go and Rust each passed a
  10,000-event same-UUID burst test while retaining one pending entry.
- Hardened both MessagePack decoders against impossible declared string/binary
  lengths before a generic value decoder can allocate or consume the body. The
  second saved Go fuzz regression declared a 3,334,915,782-byte `bin32` value
  in an 11-byte input. A final 30-second Go run passed 12,930,707 executions;
  Rust checks every MessagePack marker with truncated suffixes.
- Completed Go and Rust Sentinel adapters and transition recovery. A stale
  promoted-state response triggers complete same-UUID Register republish;
  Selectors abandon the old connection generation and repeat
  subscribe/scan/PING recovery.
- Added and passed an isolated Redis 8.8 three-node/three-Sentinel harness with
  separate Redis and Sentinel ACLs. It exercised minority stale Sentinel state,
  forced acknowledged-write loss, two promotions, same-UUID republish,
  `SCRIPT FLUSH`, all-Sentinel loss, primary loss without resolution, restart,
  and Go/Rust cross-language convergence in 39.9 seconds.
- Re-ran standalone Lua, Go, Rust, interoperability, Go Linux race, minimum Go
  1.24 and Rust 1.85, Go vet, Rust formatter/Clippy/rustdoc, shuffled stress,
  500-Registration loads, and exact script-copy verification. Detailed commands,
  measurements, scoring, strengths, and remaining risks are in
  `test-results.md` and `testkit/README.md`.
- Removed only generated coverage/profile/test-binary and Python-cache outputs.
  Verified the standalone Redis database was empty, removed only
  `verdandi-it-standalone-20260823a`, and confirmed no Verdandi test labels,
  Sentinel temporary directories, or dedicated-port listeners remained on the
  remote Docker host. Created no commit and performed no push.

### 2026-08-23: Implement and qualify the initial Go/Rust Register and Selector

- Added the Go module and Rust crate at source version `1.0.0`, with independent
  Client, Registration, Selector, RedisClock, MessagePack, deadline-index,
  reconnect, error, and lifecycle implementations. Public APIs expose no Redis
  driver types.
- Implemented fresh 32-hex process UUIDs, serialized content revision, immutable
  Attr/TTL/fixed Data names, patch-only Update, no-op suppression, automatic and
  explicit Renew, full-register missing/ambiguous recovery, graceful terminal
  unregister, immutable local Selector views, paginated HSCAN/pipelined HGETALL,
  subscription PING fencing, targeted gap repair, and local TTL expiry.
- Changed Zone capacity from a process-pinned administrative prerequisite to
  Redis-backed defaults that Client bootstrap fills with `HSETNX`. An
  authorized backend can change related fields atomically; Clients poll or
  refresh explicitly and retain the last complete valid snapshot. Added live
  Go/Rust tests for atomic policy adoption, invalid-policy fallback, lowered-
  policy write rejection, legacy discovery, and restoration.
- Added independent default and protocol ceilings for Attr/Data: defaults are
  16 and 32 fields with 128-byte individual values; protocol ceilings are 128
  and 128 fields with 16-KiB values. Complete defaults/ceilings are 16/64 KiB.
- Found and fixed a Go cancellation-classification edge after write admission:
  the serialized writer now returns its confirmed or ambiguous outcome instead
  of allowing the outer Context to hide it as a simple deadline.
- Fuzzing found a 119-byte MessagePack `map32` allocation bomb in the Go generic
  decoder. Replaced both SDK event paths with bounded flat-envelope parsing,
  retained the failing corpus, reduced its Go regression from about 17.6 seconds
  to 0.00 seconds, and passed the then-current 10-second, 1,729,936-execution
  fuzz run. The later 30-second result and second regression are recorded in the
  newer completion entry above.
- Passed the canonical Lua fixture, Go/Rust Redis 8.8 integration suites,
  Go Linux race detector, Go vet, Rust formatter/Clippy/rustdoc, and live
  Go-to-Rust plus Rust-to-Go binary Pub/Sub interoperability.
- Stopped and recreated the isolated Redis container empty while each SDK peer
  remained alive. Go recovered. The first Rust attempt exposed missing `fred`
  reconnect policy; explicit bounded infinite reconnect was added and the
  repeated Rust fault test recovered the same UUID/revision successfully.
- Re-ran 500-Registration one-update-per-second profiles. Final end-to-end
  results, Redis EVALSHA timing, exact key memory, environment, commands, Lua
  9.2/10 assessment, and known limitations are recorded in `test-results.md`.
- Added `sdk.md`, `testkit/README.md`, and `.gitignore`; updated the protocol,
  architecture, decisions, Alpha, README, Codex memory, Lua guide, and this
  worklog to match the implemented behavior.
- Verified the isolated Redis database had zero keys, stopped only
  `verdandi-sdk-20260823`, and confirmed Docker had removed that container.
- Created no commit and performed no push.

### 2026-08-23: Move steady-state Registration capacity checks to the SDK

- Initially fixed the shared Registration field/count/byte limits as one
  Administrator-provisioned, Client-pinned `verdandi:config:<zone>` Hash. The
  later implementation entry above supersedes only that lifecycle with
  SDK-seeded, administratively mutable, last-valid snapshots; the Redis shape
  and local hot-path capacity ownership remain.
- Defined exact SDK-side complete-record accounting, including a 16-byte
  upper-bound reservation for the Redis-generated `@timestamp`. Capacity failure
  occurs before desired state, revision, or Redis are changed.
- Removed the full Registration `HGETALL` and projected-record reconstruction
  from a new-revision Lua Update. Full Register still inspects its own record for
  immutable/full-state comparison, obsolete-Data removal, and protocol-ceiling
  enforcement.
- Extended the isolated fixture with Redis command-stat accounting and passed
  the complete Registration/Selector suite against Redis 8.8.0. The Update case
  observed zero `HGETALL` calls.
- Ran an indicative remote Redis 8.8.0 Docker microbenchmark with persistence
  disabled on a four-vCPU allocation of an Intel i7-13700F host, 500
  Registrations, 20 contiguous Update rounds (10,000 operations), one changed
  byte, pipelined per round, and no subscribers. Server `EVALSHA` averages were
  21.58 microseconds for a 100-byte/seven-field record, 21.11 microseconds for a
  15,943-byte/13-field record, 22.32 microseconds for a 63,263-byte/13-field
  record, and 25.36 microseconds for a 63,341-byte/133-field record. The
  corresponding p99 values were 84.479, 72.191, 77.311, and 135.167
  microseconds; all four profiles observed zero `HGETALL` calls. These are
  implementation smoke measurements, not Alpha capacity qualification, and do
  not measure Pub/Sub subscriber fan-out.
- Removed the remote test container and all test keys. Created no commit and
  performed no push.

### 2026-08-22: Implement the initial Registration Lua protocol slice

- Initially added one shared `registration.lua` script for atomic `register`,
  `update`, `renew`, and `unregister` actions with canonical string statuses, Redis-time
  timestamps, overflow-safe deadlines, matched Registration/Registry expiry,
  MessagePack events, revision checks, immutable Attr/TTL enforcement,
  per-invocation ceilings, and complete-Register validation.
- The 2026-08-24 completion entry supersedes only this executable packaging;
  the recorded wire behavior and one-execution atomic invariant remain.
- Kept Selector free of a Registry-wide Lua snapshot. Documented and exercised
  subscribe acknowledgement, paginated `HSCAN`, pipelined header/full reads,
  event coalescing, and the subscribed-connection PING/PONG fence.
- Passed the retained integration fixture against an isolated Redis 8.8.0
  Docker container, including maximum safe integers, natural expiry, corrupt
  membership detection/recovery, and `NOSCRIPT` reload.
- Ran indicative same-host Redis 8.8.0 Docker microbenchmarks with persistence
  disabled, one Registration, 16 clients, and pipeline depth 8: 20,000 Renew
  executions completed at about 68,259 requests/second and 10,000 equal full
  Register executions at about 38,911 requests/second. The host was not
  characterized; these are implementation smoke measurements, not the Alpha
  capacity qualification.
- Created no commit and performed no push.

### 2026-08-21: Create the independent Verdandi working copy

- Created the public GitHub repository `LaconisIves/verdandi` through the
  maintainer's GitHub account.
- Cloned the confirmed-empty remote to `D:\laconis\verdandi`.
- Switched the empty local working copy to an unborn `alpha` work line.
- Performed no commit and no push.

### 2026-08-21: Establish the initial documentation system

- Defined `codex.md` as self-contained project memory for new sessions.
- Defined `alpha.md` as version `1.0.0` requirements and acceptance.
- Defined `architecture.md` and `protocol.md` as detailed system and protocol
  drafts.
- Defined `coding.md` as a language-neutral standard with Go, Rust, schema, Lua,
  comment-form, and comment-density rules.
- Defined this worklog for current and future execution state.
- Recorded Go and Rust as first SDKs rather than permanent language limits.
- Preserved the language-neutral repository root and local-only `alpha` policy.
- Left every file uncommitted for maintainer review.

### 2026-08-29: Add strict shared JSON configuration and re-audit Catalog locking

- Superseded only the earlier “no fixed configuration carrier” decision with a
  versioned v1 JSON boundary. Added the canonical schema and a complete default
  example with required Redis and optional Registration/Selector and Catalog
  objects; Cluster remains unsupported.
- Added Go `configuration` and Rust `configuration` loaders. Both cap input at
  1 MiB, reject unknown/duplicate fields, null, trailing values and invalid
  topology/ranges, distinguish omission from explicit zero, materialize the
  same defaults, and convert into language-native configs before Redis or
  checkpoint I/O; SDK-017 later added deferred bounded TLS-file loading.
- Added public read-only `Check/check` methods to the Go/Rust native root,
  Registration and Catalog Config types without restoring generated config
  code or exported defaults/range constants.
- Audited the Catalog lease chain. Confirmed mutation Lua already deletes its
  exact token atomically on success, so Go and Rust now skip the redundant
  release Lua on confirmed-success paths. Projection/unconfirmed error paths
  retain token-fenced best-effort release and TTL remains orphan recovery.
- Kept the finite lock acquisition deadline. It is a one-shot foreground bound
  on an unfair `SET NX` retry loop, not a lock recycler or persistent Catalog
  timer. The later SDK-017 decision retains the entire external lock for the
  intended single/few nearby Publisher workload.
- Found and corrected Catalog Lua's stale 512-KiB hard ceiling to the documented
  4-MiB protocol ceiling, regenerated all standalone and Go/Rust embedded
  scripts, and added a live boundary regression.
- Passed Go test/vet, Rust 63 library plus four offline external tests,
  format/strict-Clippy/warning-denied rustdoc, generated-script checks, JSON
  syntax checks, and the unique-Zone Redis 8.8 Catalog protocol suite. The live
  test accepted a value beyond 512 KiB, rejected beyond 4 MiB, cleaned its lock,
  preserved the old value, and left zero owned keys.
- Rewrote `configuration-review-20260829.md` with the timer/TTL distinction,
  network-roundtrip analysis, then-current scores, TLS/schema/TTL relation
  limitations, and lock-retention versus lock-removal options; SDK-017 later
  resolves the TLS and lock choices.
- Created no commit and performed no push.

### 2026-08-30: Remove Catalog locking and consolidate the Go/Rust implementation

- Superseded the Catalog-lock portion of SDK-017. Removed the Acquire/Release
  source fragments, generated executables and embedded scripts; removed the
  `:@lock` key, token/TTL arguments, retry loops, lock configuration, reply
  statuses, schema fields, examples, and lock-specific tests.
- Catalog now has four generated scripts. Replace and Delete are one atomic Lua
  call. Patch retains SDK-owned HMGET projection and commits only when Lua still
  sees the exact base; a new Redis 8.8 two-writer race proves one success, one
  stale result, revision 2, and one intact final value.
- Made Go and Rust Publisher lightweight stateless views of Catalog Client and
  removed their independent Close operations. Catalog Client remains the
  admission owner. At this historical checkpoint Go Catalog Subscriber had
  exactly two long-lived owned workers: reader and repair; the 2026-08-31 entry
  supersedes that worker-lifetime detail.
- Added Go internal configuration-validation and lifecycle packages to remove
  duplicate duration/optional/Zone checks and Registration/Catalog activity
  gates. Added a crate-private Rust Activity/RAII Guard implementation shared
  by Registration and Catalog while retaining native CancellationToken and
  async-close behavior.
- Added `testkit/conformance/v1`: both languages now consume identical binary
  Catalog MessagePack events and JSON acceptance/error vectors, including
  explicit supported zero values, rejected Cluster mode, and rejection of the
  removed `catalog.lock` object.
- Preserved optional bbolt/redb Catalog checkpoints, incremental revision/floor
  recovery, Registration/Selector behavior, and all public data/codec shapes.
  Leader and C++ were unimplemented at this historical checkpoint; the C++
  status is superseded by the 2026-08-31 entry above.
- Passed generated-script and JSON checks, all Go packages, WSL/Linux Go race,
  71 Rust library tests plus external offline tests, Redis 8.8 Lua and SDK
  integration, root Redis commands, exact-base contention, Go/Rust live
  Registration/Catalog interoperability, and final empty-database cleanup.
  The structured functional result is
  `testkit/results/optimization-functional-20260830.json`.
- Passed Rust all-target/all-feature strict Clippy, warning-denied rustdoc, and
  the declared Rust 1.85 minimum-toolchain all-feature check. Split strict
  Catalog Read decoding into a private included fragment without expanding
  implementation visibility.
- Passed independent Registration and Catalog Sentinel matrices. Registration
  preserved both UUIDs and advanced Go/Rust Selector generations `1 -> 2 -> 3`
  through two promotions; Catalog reached revision 10 through two promotions,
  deleted its final value, and left zero keys.
- The first Catalog soak-tag compilation found a stale test-only shutdown
  variable after Publisher became stateless. Removed it, independently compiled
  the tagged package, and reran the workload from zero; the failed attempt is
  not accepted evidence.
- Passed the exact 70-file lock-free Catalog fingerprint for 60 Redis seconds
  with AOF everysec: 8,448 accepted mutations, zero transient error, stale
  retry, or unexpected asynchronous error, 28,656 bytes of stable-window Redis
  memory growth, goroutines `9 -> 10 -> 2`, successful Lua/Rust/interop
  post-checks, and final `DBSIZE=0`.
- Recorded the current physical source inventory, detailed regression evidence,
  scores, strengths, deductions, and follow-up direction in
  `optimization-review-20260830.md`. C++ was deferred at this historical
  checkpoint and is superseded by the 2026-08-31 entry; Leader remains outside
  this completed scope.
- Created no commit and performed no push.
