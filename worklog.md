# Astra Worklog

## Explicit C++ protocol names (2026-09-15)

Removed wire, orbit and probe namespace aliases from 12 handwritten C++ service, test and probe files.
All protocol references use full proto::astra::v1, proto::orbit::v1 or proto::astra::bench::v1 names.
Verified changes are only alias expansion plus clang-format output; schemas, generated code and other previously tested sources remain unchanged.
Synced Linux and passed Debug regression with benchmark targets built: 10 CTests, 6 RPC cases and 13 process cases.
Generation comparison and formatting passed; test services exited and owned resources were cleaned.
This source-only edit did not rerun Release or sanitizers. No downloads, commit or push.
Linux log: build/protocol-names/debug.log. Naming rule is recorded in astra/CONTRIBUTING.md.

## Full v1 regression recheck (2026-09-15)

Rebuilt current Linux Debug, Release, ASan/UBSan and TSan project targets using existing dependencies.
Each profile passed 10 CTests, 6 TLS/RPC cases and 13 process cases; no sanitizer diagnostics.
Windows/Linux Supervisor and generator gates, Linux Go race, Python 44 tests and Admin 53 tests/typecheck/build passed.
Verified 338 cross-host source files, unchanged frozen sources/dependencies, binary hashes and final service cleanup.
No production code changes or new downloads. No long-running campaign, commit or push.
See [recheck report](testkit/results/protocol-v1-recheck-20260915.md); prior evidence below remains historical.

## Orbit / Astra / Comet v1 boundary

Consolidated the active schemas as orbit.proto, astra.proto and comet.proto. C++ handwritten types use astra;
Member/Role belong only to proto.orbit.v1. All node senders, receivers and benchmark probes use major 1,
with the single signing domain proto.orbit.v1.admission + NUL. Comet remains an empty SDK boundary.
Windows Go checks, Linux Go race, generator checks, Debug CTest/RPC/process regression and v1 reply validation passed.
154 service/tool files match both hosts; 704 frozen files and 3420 installed dependency files remain unchanged.
No Release/sanitizer rerun, new download, third-party rebuild, indefinite test, commit or push.
See [v1 contract and evidence](protocol-v1.md). Earlier entries retain their historical names and versions.

## Astra naming and source-directory migration (2026-09-15)

Moved the active C++ service from cluster-cpp to astra, with matching C++ namespace, CMake options,
protobuf packages, admission domain, Admin identity, documentation and service test paths.
Outer checkout directories, Git remote and repository-derived Go imports remain unchanged.
Legacy Verdandi SDKs and 704 frozen files remain unchanged; all 28 MessageIDs retain their numbers.
Linux now tests from the project root using migrated build/astra trees and existing dependency prefixes.
Debug and Release each passed 10 CTests, 6 RPC cases and 13 process cases. Both Go checks and Linux race,
protocol checks, Python harnesses and 53 Admin tests passed. Sanitizer caches were reconfigured, not rerun.
No downloads, third-party rebuild, indefinite test, commit or push. Details: [Astra migration](astra-migration.md).
Historical paths in entries below refer to the source directory before this move.
## SyncStore line review and bounded performance comparison (2026-09-15)

Applied the maintainer's binary-search/reserve idea, Ranges member projection, lock-external record preparation,
checked capacity and clearer storage ownership comments. Fixed the max-expiry sentinel regression and removed
the redundant entry deleted flag. Measurements retained node version matching and upper-bound snapshot reserve;
immediate erasure, versionless empty-node reclamation, exact live counting and forced append_range were not retained.
Final Store unit/fault executables passed four profiles; 76 write and 7 read injected failures were verified.
The new sentinel test fails against aadcbb4 and passes on the selected implementation. Added a bounded standalone
benchmark and CMake target, detailed audit and structured results. No line coverage or whole-service rerun claimed.
Report: [review](cluster-cpp/sync-store-performance-review-20260915.md).

## Sync foundation cleanup: validation complete (2026-09-15)

Prepared fixes after `2bf279f`: private storage ownership, allocation-safe batches, bounded version
progress, explicit control-stream overload, real C++/Go generated protocol files, and dedicated
store/fault-injection test targets. See [review notes](cluster-cpp/sync-foundation-review-20260914.md).

The maintainer explicitly paused compilation and tests until further notice. Only source edits,
formatting and protocol generation were performed; no test result is claimed for this revision.
The maintainer subsequently resumed complete validation. No leftover endurance processes were found;
current validation uses an isolated Ubuntu source copy and bounded runs only. Working changes remain uncommitted.

Validation finished successfully: four C++ profiles each passed 10 CTests, 6 RPC cases and 13 process
cases; Supervisor checks passed on both platforms, including Linux race and two bounded fuzz runs.
Core-only tests, Python harness/build tests and protocol generator gates also passed. Store injection
covered 76 allocation failures. The bounded 4-Star/2-Planet run completed 9 fault cycles in 65.555 seconds,
then 15 seconds of steady operation, with cleanup verified. 198 source hashes match the VM copy.
Only two Python harness files required formatting; no implementation fix was needed during testing.
Final process inspection found no leftover test process. No commit/push or new dependencies.
Evidence: [validation report](testkit/results/cluster-sync-foundation-20260915.json).

## Current service decision (2026-09-12)

C++ Star/Planet and Go Supervisor are the only active service implementations. The old Rust service is retired; its reports are historical. Shared code uses `cluster`, own identity fields use `id`, and Supervisor issues signed opaque string identities. Current protocol is v6; the [identity contract](cluster/identity-contract.md) supersedes older UUID/client-generation and Rust-comparison descriptions below. Accepted code-review corrections must be implemented, not left as documentation-only proposals. Rust SDKs and the standalone protocol generator remain separate.

## Admission simplification (2026-09-12)

The maintainer authorized single-RPC admission simplification. Protocol v6 removes Challenge,
startup Ticket and client CAS baselines. Supervisor owns durable request deduplication and member
epoch assignment; Star retains local signed-Hello validation and local session lifetime fencing.
Committed superseded requests stay rejected across Supervisor restarts. Fresh requests replace in
transaction commit order, not physical process-start order. Bounded durable request history is
controlled by --max-startups; no eviction or silent reset. See [current identity contract](cluster/identity-contract.md).
Planet business development is paused; preserve and regress its existing shared connection path.
Next business work targets direct Star/first SDK, but this turn only implements admission.
This supersedes the v5 ticket/CAS descriptions below; previous test results retain their own scope.
Implementation and bounded regression are complete. See [admission verification](cluster-cpp/admission-simplification-20260912.md).

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

Last updated: 2026-09-12

- Star/Planet target: C++26 / GCC 16.2.0 on Linux first, Supervisor remains Go.
  The maintainer authorized implementation in the separate `cluster-cpp/` directory.
  The [skeleton plan](cluster/cpp26-skeleton-design.md) now describes its actual layout;
  the default Linux entry selects C++. The Rust service is retired. Earlier comparison results are historical;
  protocol v6 uses a single Register RPC, Supervisor-issued opaque id and one signed admission credential.
  See [current admission evidence](cluster-cpp/admission-simplification-20260912.md).

- Project: Verdandi, a language-neutral distributed coordination protocol and
  SDK ecosystem.
- Repository: public `git@github.com:eosforge/verdandi.git`.
- Local path: `D:\projects\verdandi`.
- Git state: backup commit `3e074fa` on `alpha` was pushed with the maintainer's
  explicit authorization. The 2026-09-12 C++ qualification and maintenance changes are working
  tree changes, not a new commit or push. Dependency permissions remain specific
  to the approved packages and project-local locations; no new download occurred
  in this resumed qualification.
- Release state: the maintainer selected `0.1.0` as the current
  non-production Alpha version for distributed development and controlled
  service integration. Its source and documentation are published only on the
  public `alpha` branch; no version tag, GitHub Release, or language package is
  published.
- Current SDK version: `0.1.0`; stable target SDK version: `1.0.0`; intended
  stable protocol version: `1.0`. The `0.x` protocol remains experimental.
- Initial backend modes: Redis Standalone and Redis Sentinel.
- Alpha backend baseline: Redis Open Source 8.0.0 or later in a qualified Redis
  8 line, using Hash field TTL for Registry membership.
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
  Standalone regression and Windows/Linux x64 two-promotion Sentinel TLS
  regressions pass; remaining C# release
  gates are platform/RID packaging, NativeAOT/trimming, live mutual TLS, direct
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
- Go Selector now has an optional `verdandi-refgen` callback-only reference
  facade. Generated read views contain copied scalar values and opaque
  read-only slice wrappers rather than Attr/Data pointers; generated Editors
  stage Data fields under the existing transaction token. `WithOne`/`WithAny`
  return no detached values and commit only edits attached to final selected
  values. The ordinary detached APIs remain unchanged.
- The coding standard now requires language-native SDK implementations rather
  than source-shape symmetry. Go operation Contexts remain explicit parameters
  and are not stored in long-lived Clients; Rust uses owner-held hierarchical
  Tokio cancellation tokens. New stable language/runtime features are reviewed
  against the declared minimum version and measured before custom abstractions
  or compatibility changes are accepted. During the current maintainer-review
  phase, production source uses detailed Chinese declaration/block comments;
  test source is explicitly excluded and release readiness triggers conversion
  back to concise standard English.
- Windows x64 and Linux x64 now use `sdk/cpp/build.ps1` and
  `sdk/cpp/build.sh` as the normal C++/C ABI/Legacy developer entry points.
  They detect but never install toolchains, isolate all generated content under
  ignored repository-level `build/`, support verified offline dependency
  caches, and emit detailed standard-English console output while retaining
  temporary Chinese source comments for maintainer review. Go, Rust, and C#
  remain outside these native build scripts; C# only loads a shared DLL/SO.
- Go Registration and Catalog now share the root `Encoder.Encode() (Fields,
  error)` and `Decoder.Decode(Fields) error` contracts. Encoders transfer one
  complete field map to Verdandi; raw `Fields` deep-clones caller storage. No
  legacy codec aliases remain because the SDK is unreleased.
- The current Lua/Go/Rust/C++ source review preserves the specialized generated Lua
  hot paths, removes redundant SDK allocation and state-copying work, moves
  Go's raw Registration compatibility surface into tests, and fixes empty-value
  field comparison. A clean isolated Redis 8.8 regression passes all 13 suites
  with 4,579 processed commands and no background-thread exception.
- Accepted coordination scope includes paginated service discovery and
  persistent Catalog KV synchronization. Generic Campaign/Leader election is
  explicitly excluded from every target, including `1.0.0`. Redis Sentinel
  primary failover remains backend recovery and does not expose application
  leadership.
- Existing Registration Version fields, Register/Update behavior, and their
  qualification remain unchanged. Version is application metadata and has no
  built-in election semantics.
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
  The exact public freeze subsequently passed a 43,213.948-Redis-second
  Registration/Selector campaign with 21,600,000 Updates, 3,707,005 selection
  transactions, and all 214 planned faults, plus a separate
  43,201.858-Redis-second Catalog campaign with 5,932,160 attempts and all 113
  planned faults. Both ended at `DBSIZE=0` and removed their exact fixtures.
  Current Catalog qualification includes Lua/Go/Rust functional,
  reconnect, checkpoint, decoder, exact-base writer contention, WSL/Linux race,
  and cross-language interoperability. C++ passes strict static/shared GCC,
  C11 and C++11/14/17 C-ABI consumers, clang-tidy, ASan/UBSan, authenticated
  Standalone native/C-ABI integration, and isolated plain/TLS Sentinel
  startup/integration smoke. The same compiled core passes two TLS promotions
  through C ABI v1 and C# on Windows and Linux, while a direct native-API
  two-promotion harness, live mutual TLS, and automated packaging remain open.
  No service or Node ceiling is encoded; each Catalog
  Path is one bounded complete value.
- License: MIT. The `0.1.0` preparation, frozen-source endurance evidence,
  multilingual Alpha hardening, and the 2026-09-03 optimization review are
  public on `alpha`.

## 3. Active Work

### P1: Re-audit current source before approved dependency-backed qualification

Outcome: repeat line-level review of the current working tree for unexpected
bugs, simplification opportunities, and cross-language contract mismatches;
then qualify it with approved Go/Rust/C++ dependencies locally and on Ubuntu.

- Record a new source inventory, hashes, review ranges, and findings without
  replacing the immutable first audit or repair evidence.
- Use project command entry points and ignored `build/` caches. Download only
  approved SDK dependencies, keeping existing lockfiles/checksums and recording
  acquisition outcomes. No global environment changes or implicit tool installs.
- Do not start runtime tests until the fresh static review is complete.
  Check each issue for propagation across Go, Rust, C++, C ABI/Legacy, C#,
  shared Lua, and test/build tooling; separate intentional language idioms.
- The initial Python A22/A23 deferral was superseded by the later explicit
  approvals. Their ownership fixes are implemented in the unified runner;
  current evidence belongs to its completed qualification item below.
- On Ubuntu, inspect current memory/available tools before dependency
  preparation, keep build parallelism bounded, and synchronize only source.
- Preparation completed on 2026-09-08: approved Go/Rust toolchains and their
  locked VM dependencies are project-local; GCC/G++, CMake, and pkg-config are
  installed from Ubuntu official sources. C++ archives are hash-verified on
  both hosts, and prebuilt OpenSSL layouts are prepared without source builds.
  The initial source-only VM snapshot contains 708 files. Fresh findings and
  evidence remain in `code-reaudit-20260907.md`; static review completed on
  2026-09-08 before any fresh SDK build or runtime test.
- Current review coverage includes all owned SDK production implementations,
  C++ build tooling, C# project files, and all Go/Rust/C++/C# SDK test sources.
  The report records 28 source-confirmed findings and concrete gaps in the
  existing regression assertions. All testkit sources and configuration
  vectors are also reviewed. Both Lua generators passed their static --check;
  325 manually reviewed source files and 32 generated Lua copies have a
  pre-repair hash inventory in build/reaudit-20260907. Proceed with repairs,
  formatting, bounded builds, and targeted cross-language qualification.
  No fresh runtime pass is implied by this coverage.
- Repairs now cover B01-B28 plus the confirmed C++ synchronization deadline
  issue R05/B29. See `code-reaudit-fixes-20260908.md` for the exact verification
  boundary. The current source passes Windows Go unit/Redis tests, Ubuntu Go
  unit/Redis race tests, Rust library (Windows 84 / Ubuntu 85) and 10 Redis
  integration tests on each platform, Windows Clippy, and C++ shared Debug
  builds plus 16/16 CTest targets on both platforms. Windows net8.0/net10.0
  C# offline/Redis tests pass, including parent lease GC ordering. Go/Rust
  bidirectional Registration/Catalog interoperability also passes.
- Added real Redis regressions for C++ natural expiry, lowered write-policy
  reads, optional metadata, delete-then-Patch, and concurrent Find/create/two
  Close calls; Rust retained closed handles can reopen the checkpoint; Go/Rust
  public empty Data updates preserve revision/timestamp and reject after Close.
  PowerShell restores absence of VSLANG as well as its value and console
  encoding. Source-only VM deltas are hash-checked before application. VM
  builds used one job; available memory stayed around 6.5 GiB with zero swap.
- Remaining qualification is explicit in the repair table: selected precise
  cancellation/ACL/malformed-server/fence-timeout injections and hours-long
  endurance are not marked passed. The newer unified-runner evidence in Completed Work
  covers Python A22/A23, Linux C# and its executed Sentinel/TLS scenarios.
  Previous-source endurance evidence must not be inherited by this tree.

### P1: Finish qualification of the 2026-09-07 audit fixes

Outcome: qualify the 23 applied source fixes against complete SDK builds and
the remaining deterministic lifecycle and Redis integration regressions. The
original audit remains an immutable pre-fix snapshot; current evidence is in
[`code-fixes-20260907.md`](D:/projects/verdandi/code-fixes-20260907.md) and its
[`structured result`](D:/projects/verdandi/testkit/results/code-fixes-20260907.json).

- Local C++ tests, isolated actual-source Go/Rust regressions, C# compilation,
  and the real shared-Lua field-limit regression have the evidence recorded
  below. These checks do not establish complete SDK qualification.
- The 2026-09-08 work item above completed fresh review, approved dependency
  preparation, complete Windows/Ubuntu SDK builds and short Redis regression.
  Refer to its separate evidence for the current source; do not rewrite the
  earlier 2026-09-07 result as if it included those later checks.
- A05, A11, A12, A13 and R03 still require their complete asynchronous lifecycle
  or cancellation verification; other per-finding gaps remain in the report.
- The later approved unified runner implements the A22/A23 Python fixture
  fixes. Its ownership regressions and live cleanup evidence supersede the
  original deferral without rewriting the older audit result.

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
  keys, the withdrawn election proposal, scalable registry synchronization, Catalog
  KV, lease math, ACK transitions, stable errors, resource limits, Command
  deferral, the Redis qualification baseline, and executable artifacts.
- Reviewed the current Hermes Redis Service, Primary, and KV design sources.
  Verdandi adopts the discovery-relevant subscribe-before-snapshot, revision
  recovery, and immutable local-view invariants, while replacing the Hermes 100-instance atomic
  snapshot boundary with pagination, per-Registration event coalescing, and a
  subscribed-connection PING/PONG fence. Catalog uses Hash/ZSET/Read recovery
  with an explicit tombstone floor and full-operation Pub/Sub.
- Maintainer direction fixes persistent Catalog KV synchronization, no maximum
  service or Node count, and explicit exclusion of generic Leader election.
  Catalog is one complete Value.
- Maintainer direction also fixes Redis 8 Hash-field Registry membership and Catalog
  Replace/Patch/Delete with strict Patch bases, complete deletion, and no TTL.
- Maintainer direction calls one Node's leased record a `Registration`, calls
  the Zone/Type collection a `Registry`, and stores every Registration
  in its own Redis Hash with key TTL and typed partial field updates.
- Maintainer direction rejects a universal CDDL/deterministic-CBOR envelope for
  ordinary Redis state. SDKs retain the known fields they require; Redis ACLs,
  field contracts, and protocol-owned Lua define supported mutation behavior.
- The 2026-09-01 maintainer direction supersedes the original start-at-1.0
  choice: the implemented SDK preview is `0.1.0`, without production or stable
  compatibility promises. Stable `1.0.0` remains reserved for the complete
  supported release contract. The intended first stable protocol is `1.0`;
  capability negotiation remains absent.
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
  reset the revision.
- Redis key names, data types, and meanings are forward-compatible and
  unversioned. Compatible evolution adds optional fields or new keys.
- Maintainer direction rejects end-to-end signatures for desired state,
  commands, Catalog, Registration, and ACKs. Redis authentication/ACLs define
  write permission; hashes remain only for content integrity, and ACL-authorized raw
  mutation is outside the protocol guarantee.
- Zone is application-supplied SDK configuration validated as 1 through
  32 case-sensitive ASCII letters. The SDK generates each Registration UUID as
  exactly 32 lowercase hexadecimal characters.
- Type IDs use the accepted common ASCII form. Catalog uses
  one bounded Part/ID Path; no opaque business-key token remains.
- A Registration is one Hash exposed as `Meta`, `Attr`, and `Data`. Meta is
  exactly `@uuid`, `@revision`, `@timestamp`, `@ttl`, and `@version`; immutable
  Attr uses `.name`; mutable fixed-structure Data is unprefixed and independently
  patchable. Registration revision is a content version; Attr and TTL are
  immutable for the UUID lifetime. Catalog live Hashes reserve revision,
  Replace revision, shape, and encoded bytes around opaque application fields.
- Registry membership uses Redis 8 per-field TTL. The
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
  scan/PING/event, Catalog LWW/mirror, manifest, lease, ACK,
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

Outcome: extend the completed Client/Register/Selector/Catalog slice so the
supported SDKs also complete observed-load synchronization, desired state, and
acknowledgements against one Standalone protocol.

Acceptance criteria:

- Keep the completed Catalog Hash/ZSET/Pub/Sub and checkpoint qualification passing as
  later Publisher capabilities are added.
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
- For C#, retain the qualified Windows/Linux Redis/Sentinel TLS matrix and add
  qualified per-RID native packages, NativeAOT/trimming checks, live mutual
  TLS, performance, cross-language peers, and endurance before production
  release claims. Windows DLL offline loading, concurrent disposal/finalizer
  pressure, the independent Linux x64 Standalone gate, and both platform
  server-authenticated TLS two-promotion Sentinel gates are complete. macOS is
  intentionally unsupported.

## 5. Blockers and Open Decisions

- The 2026-09-07 fixes have incomplete full-SDK qualification because required
  dependencies are absent locally and downloads remain disallowed. Python
  A22/A23 are explicitly deferred; lack of Black does not authorize installing
  it. The Ubuntu VM recovered after the maintainer disabled Dynamic Memory;
  the subsequent real Lua regression passed, without a Linux SDK build.
- Add byte-exact cross-language vectors for application-owned typed Attr/Data
  codecs as real consumers define their field encodings. The direct Go and Rust
  conversion contracts, raw Fields compatibility, fixed-structure top-level
  patching, and key/value event envelopes are accepted.
- Qualification, not a protocol redesign, remains for sustained maximum-size
  Register/reset fan-out above eight subscribers, sustained driver-ingress
  pressure, and connection-generation RedisClock under delayed responses and
  clock steps.
- Add application-owned Catalog codec vectors as consumers select concrete
  scalar, array, map, or schema types. The raw Value contract deliberately does
  not impose one codec.

Accepted engineering qualification gates, not maintainer decision blockers:

- Validate Registry subscribe/scan/PING buffers and Catalog ZSET pages, floor,
  checkpoint, timeout, and complete-value limits under the accepted workload.

## 6. Completed Work

### 2026-09-14: Minimize the C++ connection skeleton for review

- Replaced HexId with concrete Principal and removed DialTarget. Policy::due now returns one
  optional Member, preserving Runtime dial pacing and pending ownership. Removed the generic
  admission Call, consumed completed calls on both outcomes, and consolidated redundant closing/upstream flags.
- Removed the already unusable benchmark command and dispatch. Preserved Chinese ownership comments,
  protocol v6, Supervisor behavior, existing role semantics and all prior tests. Planet business remains paused.
- Added checks for single-target retry ownership, stale failure after replacement, digest encoding and
  first cancellation reason. Windows formatting and six build-entry tests passed.
- Linux Debug, Release, ASan/UBSan and TSan each passed eight CTest suites, six RPC scenarios,
  thirteen process scenarios and generation checks. Release also completed 61.507 seconds and nine
  fault/recovery cycles with four Stars and two Planets. All owned resources were cleaned.
- Verified 264 validation input hashes against the VM. Review patch and the original working-file snapshot
  remain in build/cluster-minimal-20260914; existing migration/Admin changes are not counted as this cleanup.
- No new dependencies, global changes, business implementation, commit/push or long endurance restart.
  See [review guide](cluster-cpp/minimal-review-20260914.md) and [results](testkit/results/cluster-minimal-20260914.json).

### 2026-09-12: Simplify Supervisor admission and Star login

- Protocol v6 replaces Challenge/Register with one Register. Removed startup tickets, the second
  signature domain and client expected_epoch CAS; Star validates one signed admission locally.
- Supervisor owns atomic request deduplication and member epoch assignment. Committed old requests
  remain rejected after replacement and reopen; fresh starts use transaction commit order.
  The bounded --max-startups index is not evicted, and current retries work at capacity.
- Windows Go/Proto checks and 7 Python harness tests passed. Linux Go race passed after separating
  instrumentation timeout from credential semantics; production timeout and KDF cost are unchanged.
- C++ Debug, Release, ASan/UBSan and TSan each passed 8 CTest suites, 6 real RPC scenarios and
  13 process scenarios. Final Debug reused the final Go binary. 85 selected source hashes match
  between local and isolated VM validation; owned resources were cleaned and no test service remained.
- No new dependencies, business implementation, commit/push or endurance restart. Planet business
  work remains paused; its shared admission and connection behavior remain covered.
- Evidence: [review and limits](cluster-cpp/admission-simplification-20260912.md) and
  [machine-readable results](testkit/results/cluster-admission-20260912.json), including earlier failures.

### 2026-09-12: Implement Supervisor-issued ids and retire Rust service checks

- Active service roles and APIs use Star/Planet and `id`; shared C++ code is `cluster-cpp/` and `verdandi::cluster`. Admin uses Star/Planet and `stars`, retaining `starId` only for a planet's owner reference.
- Supervisor issues signed startup tickets. Retries retain the issued opaque string id and first CAS baseline; committed Hello credentials have a separate v5 signature domain. Client UUID generation and format validation are removed.
- Removed Rust service selection and mixed service regression. Deprecated Rust sources and historical reports are retained. The old v4 benchmark is rejected explicitly because its Rust receiver cannot qualify v5.
- Verified Windows Go quality checks and protocol generation, Linux Go race, eight C++ CTest suites, six TLS/RPC scenarios, thirteen real process scenarios, thirteen Python harness tests, and 53 Admin tests with type check, boundaries and production build. See [migration verification](cluster-cpp/identity-migration-20260912.md).
- No new dependencies were installed. Validation used an isolated VM source/build directory and new databases; the existing endurance run and its binaries were not replaced.

### 2026-09-12: Complete expanded connection endurance and manual cleanup

- User authorized 8 Star / 16 Planet plus Go Supervisor on Ubuntu, two hours
  of repeated failures and recovery followed by uninterrupted steady operation
  until manual stop. No upper-layer implementation is part of this run.
- Reuse qualified Release binaries and project-local tools, without downloads.
  `cluster-cpp/test_endurance.py` reuses Host ownership, adds phase transitions,
  fresh-status checks, per-Star Planet ownership checks and rotating samples.
- Windows 37 shared harness checks passed; Linux 36 passed with one Windows
  platform skip. Expanded-topology short preview and actual STOP/SIGTERM/
  unexpected-child-exit controls passed. Binary hashes match maintenance evidence.
- Fault qualification completed and the later steady run was manually stopped
  with cleanup verified. Details: [endurance](cluster-cpp/endurance-20260912.md).
- First attempt PID 310386 failed after 357.517 seconds / 41 cycles because the
  Python checker dereferenced a legitimate null Planet upstream. Cleanup passed;
  `build/peer-cpp/endurance-20260912/status.json` remains immutable failure evidence.
- The new regression fails on the original null case and passes after normalizing
  an absent/null upstream to a pending topology. Scale uses the same expression,
  with an existing mesh short-circuit guard; it was normalized consistently.
  C++ and Rust emit null by contract; Go and SDK paths do not consume this field.
  Shared Python suites pass 38 Windows / 37 Linux plus one platform skip.
- User clarified that failures require investigation and repair within existing
  authorization, then continued testing, not stopping work to wait for permission.
  Preserve each failed attempt, use a new output directory and restart the full
  two-hour clock after a fix. Current retry output is `endurance-20260912-r2`.
  Second attempt PID 314941 started at 2026-09-12 12:32:15 Asia/Shanghai.
  Expanded preview passed 43.200 fault seconds / 5 cycles and 12.655 steady
  seconds including cleanup. All four scale cases through 16 Stars / 32 Planets
  passed, as did STOP/SIGTERM and traceback-preserving abnormal-child cleanup.
  Heartbeat `star-planet` resumed every five minutes; stop and
  pause it only after a user stop or a concrete unresolved external blocker.
- Second attempt completed 7205.739 seconds / 833 fault cycles and entered steady
  operation at 2026-09-12 14:32:51.079 Asia/Shanghai. All three recovery checks
  counted 833 completions. Snapshot evidence is
  `testkit/results/peer-cpp-endurance-fault-20260912-r2.json`; binary hashes match
  maintenance qualification. All 25 steady process PIDs match their baseline.
  The final user stop completed at 2026-09-12 16:30:17.775 Asia/Shanghai.
  Steady observation lasted 7046.696 seconds including exit cleanup; total
  controller runtime was 14282.776 seconds. All 25 service PIDs and the controller
  are absent, ports are reusable, and the actual owned directory was removed.
  Heartbeat star-planet is PAUSED. Final evidence is
  `testkit/results/peer-cpp-endurance-final-20260912-r2.json`.
  This evidence refers to pre-rename peer-cpp binaries and original runtime paths.
  Last sampled RSS was 438.5 MiB, 3.297 MiB above the steady baseline, with
  unchanged PIDs/threads and two fewer FDs. No further test is running.

### 2026-09-12: Record Supervisor ownership of opaque PeerId generation

- Recorded the maintainer's accepted identity direction in the C++ skeleton design:
  Supervisor issues process IDs and owns their format; Star/Planet authenticate the
  signed admission envelope and do not depend on UUIDv4, hexadecimal or 16-byte IDs.
- Confirmed the maintainer's choice of Protobuf `string peer_id` and owned C++
  `std::string`; retained generic UTF-8/length bounds and exact comparison without
  case folding, trimming or normalization. Replaced the earlier bytes recommendation.
- Kept format evolution distinct from changing an already issued process identity,
  message-size limits, member binding/epoch checks and registration retry correlation.
- Linked the direction from current connection rules, the C++ README and project memory.
  Current client UUID generation remains explicitly documented as implementation history;
  Schema, generated source, runtime behavior and test results were not changed.

### 2026-09-12: Document C++ responsibilities, configuration and enum contracts

- Recorded functional source headers, per-field configuration explanations, individual enum
  comments, declaration contracts and implementation-block reasoning in `coding.md` and
  `cluster-cpp/CONTRIBUTING.md`, with links from the C++ entry and project memory.
- Applied functional headers to the 36 handwritten C++ source/header files; documented all
  28 enum elements, production configuration/CLI fields, function ownership and important
  validation, callback, replacement, retry and shutdown boundaries. Root licenses, required
  third-party notices and generated sources were not modified.
- Used the existing clang-format 22.1.3 after source writes. Compared all 36 files against a
  saved pre-edit working-copy baseline: non-comment token sequences and enum order are unchanged;
  clang-format dry-run, targeted configuration/enum coverage and diff whitespace checks passed.
  These are source-level checks, not a new Linux C++ build or runtime qualification run.
- The subsequent proposal to let Supervisor issue signed opaque process IDs remains a design
  discussion; this comment pass retains the current client-generated UUID and admission behavior.

### 2026-09-12: Record per-Key reconciliation and streaming direction

- Added [the synchronization design](cluster/key-stream-sync-design.md) covering publisher-owned
  versions across Star redirection, per-Key bounded history, complete-value fallback,
  authenticated reconciliation, live handoff, deletion evidence and stale-replica handling.
- Distinguished accepted direction from unresolved writer recovery, history coverage,
  lease/binding metadata, tombstone reclamation and storage confirmation rules.
- Linked the design from Peer/Core, Galaxy, the C++ entry and project memory, while retaining
  the existing Redis SDK contracts and connection-only implementation boundary.
- Documentation-only change; no runtime code, dependencies, service processes, commits or
  publication changed. Verification is limited to Markdown structure, local links and diff checks;
  the listed behavioral scenarios are future acceptance work, not tests executed this turn.

### 2026-09-12: Organize and qualify C++ skeleton maintenance boundaries

- Retained common/star/planet. Extracted private Signals/Wakeup/Logger from
  Runtime, separated admission and dial advancement, and documented ownership.
  Admission cannot move while gRPC borrows its request fields; process guards
  cannot be copied. Session error classification no longer imports the
  Supervisor adapter or constructs temporary Error strings just to read a code.
- Removed the unused dial flag and three temporary Planet duplicate sets;
  bounded candidate comparisons reuse inplace_vector. Consolidated six test
  assertions with source_location and preserved Release checking.
- Fixed empty-CTest success in Peer and the C++ SDK test entry, rejected ignored
  diagnostic combinations, and removed empty runtime search-path entries in
  both C++ preparation/build and the shared C#/Python native harness.
  Reviewed matching Rust/Go paths; no corresponding CMake cache or path append.
- Final Debug, Release, ASan/UBSan and full TSan each pass eight CTests, eight
  real RPC/interoperability cases and thirteen process cases. Default Release
  entry also passes Go vet/unit/race and protocol checks. Four Stars/two Planets
  complete 61.531 seconds and nine fault cycles with owned resource cleanup.
- Build-entry tests pass six cases on both hosts; shared Python tests pass 32
  on Windows and 31 plus one platform skip on Linux; Windows SDK build-policy
  tests pass 12. Four public headers compile independently; four invalid native
  configurations fail with intended diagnostics. Both installed executables
  and license files pass review without a development RPATH.
- Three comment punctuation lines were normalized after behavior testing;
  all four profiles were rebuilt and both executable hashes remain identical
  to their respective qualified outputs. No new download, global configuration,
  database test, commit or push. Earlier hour/performance reports are preserved.
- Evidence: [maintenance report](cluster-cpp/maintenance-20260912.md),
  [machine result](testkit/results/peer-cpp-maintenance-20260912.json), and
  [maintainer guide](cluster-cpp/CONTRIBUTING.md).

### 2026-09-12: Complete C++26 connection-skeleton qualification and default entry

- Resumed after the authorized backup commit 3e074fa. Added seven compiler
  negative cases, a positive metadata baseline and isolated contract/STL probes.
  The contracts-disabled Release passes six CTest groups, eight RPC/mixed groups
  and thirteen process groups. Default contracts remain enforce.
- Qualified 2/4/8/16 Stars with 0/1/8/32 Planets, including real Supervisor
  admission, mesh convergence, forced upstream failure, restart and cleanup.
  Separate allocation builds count successful C++ new requests only. The largest
  ordinary topology uses about 875 MiB aggregate sampled idle RSS.
- Added an isolated C++ callback push fixture and shared Rust state validation,
  without changing production messages. The final five-round matrix completed
  225 trials: 194 full passes and 31 explicit capacity rejections, all confined
  to pressure scenarios. All ordinary cases passed. The separate allocation
  comparison passed 40/40; Event-shell reuse saves about 5% of counted new calls
  but does not justify Arena or a new pool. C++ CPU/RSS and tail latency exceed
  this Rust implementation; no language-intrinsic performance advantage is claimed.
- Audited cross-language propagation: protect the benchmark receive object during
  reentrant dispatch; handle late-publication rejection in the shared Rust
  receiver; correct benchmark window metadata; stop injecting the instrumented
  dependency lib directory into unrelated tools. The latter had loaded TSan
  zlib into rustc/LLVM. Services retain static instrumented dependencies.
- Final ASan/UBSan and full TSan each pass six CTest groups and twelve push cases;
  TSan smoke explicitly uses 200 source updates/s, not performance measurement.
  Full TSan additionally passes eight RPC/mixed and thirteen process groups.
  No broad suppressions were added. Current clang-tidy cannot parse reflection,
  and the report retains that analyzer limitation and historical failed trials.
- The normal Release Star/Planet hashes remain identical to the earlier
  3,603.207-second / 510-cycle accepted fault soak. Production source did not
  change this turn; the one-hour run was not repeated or relabeled.
- Default Linux service checks and regression now select C++; Rust is explicit
  through --implementation=rust or -Implementation rust. Windows C++ selection
  fails early instead of silently falling back. The default Linux entry passes
  generated-source checks, six CTest groups, Go vet/tests/race, eight RPC/mixed
  groups, thirteen process groups and owned cleanup. Python passes 31 on Windows
  and 30 with one platform skip on Linux; changed Rust probe passes 16 tests and
  Clippy on both platforms. No SDK/database regression was triggered.
- M0-M4 connection scope is qualified with documented limits. Business data
  replication, persistence, SDK binding, Admin integration and production
  capacity/deployment qualification remain later work. No new dependency was
  downloaded, no global setting changed, and no new commit/push was made.
- Evidence: [detailed report](cluster-cpp/qualification-20260912.md) and
  [machine-readable results](testkit/results/peer-cpp-foundation-20260912.json).
  The immutable 2026-09-11 results preserve the earlier pause and sanitizer history.

### 2026-09-11: Document the C++26 Star/Planet skeleton target

- Incorporated all seven follow-up decisions: logical session identity and
  generations, transport reuse/rebuild, variable HTTP2 windows, public gRPC
  resource/lifecycle APIs, no extra Register predecode scan for this internal
  service, framework I/O workers with a fixed application loop, and measured
  optimization after bounded reuse. Updated qualification and regression rules
  together; ordinary authentication and correctness checks remain required.
- Added [the source-version lock](cluster/cpp26-dependencies.md) using official
  stable release/tag metadata: gRPC 1.84.0, Protobuf 36.1 and supporting releases,
  plus gRPC's exact BoringSSL gitlink. Recorded differences from upstream bundled
  revisions; selected versions are not a claim of verified build compatibility.
- Follow-up TLS design uses the selected gRPC release's pinned BoringSSL for
  transport and private identity crypto, replacing the initial OpenSSL reuse
  recommendation. No download/build occurred; existing Redis SDK policy is
  unchanged. BoringSSL API/ABI changes require a coordinated rebuild and tests.
- Added [the detailed plan](cluster/cpp26-skeleton-design.md): current-vs-target
  scope, C++26 feature applications, common/star/planet structure, callback and
  lock ownership, v4 parity, gRPC capability gates, dependency/generation policy,
  staged acceptance, tests and performance/complexity measurements.
- Read the existing protocol/configuration/identity/decoder and current service
  rules; checked official GCC, gRPC and Protobuf documentation. Documented the
  stale Supervisor-frame comment in peer_transport.proto for later source work.
- Linked the plan from service documentation and updated the future language
  decision while preserving historical Rust implementation and test facts.
- Documentation only. No production/test source or .proto changed, no package
  downloads, service migration, compilation or runtime tests in this task.
  Verification is limited to document consistency, local links and diff review.

### 2026-09-11: Harden and simplify the Star/Planet/Supervisor foundation

- Common owns Rust process launch, CLI/runtime/exit handling and RPC status
  classification. Role-specific loops remain separate. Go admission and
  membership share canonical identity validation; both languages consume one
  public fixture. Legacy Go frame code is test-only. Member commits reuse an
  already validated owned snapshot, removing a second complete read/decode.
- Fixed extreme Rust timer/channel capacities, transient gRPC errors being
  treated as permanent candidate failures, local Supervisor certificate
  validation, canceled KDF admission and invalid UTF-8 account provisioning.
  Rust topology and Go persistence both reject role/address changes within a
  principal slot. Related Redis SDK paths were reviewed read-only; no SDK
  implementation or SDK test scope was added.
- Tests cover real TLS/HTTP2 I/O ownership, a second RPC on one socket,
  listener/Close races, canceled waits, stale leases, input budgets, slot
  replacement, persistence recovery and snapshot independence. Native gates
  passed 53 Rust tests, 44 ordinary Go top-level tests plus two fuzz seed
  entrypoints, four generator and 14 probe tests on each host. Python passed
  28 on Windows and 27 with one platform skip on Linux. Linux Go race passed;
  Windows race was not run. Finite Go fuzz passed on both hosts.
- Process regressions expanded from 11 to 13 groups. Windows/Ubuntu/mixed
  deployments all passed, followed by requested 120/120/300-second fault
  loops with 17/18/42 restarts. Both hosts have no remaining owned service
  processes or temporary test directories; ports were reusable. Ubuntu's
  final boot-wide OOM kill counter was zero.
- One-key tests include Python harness unit tests. Check scripts accept
  explicit bounded fuzz duration. Final Go/Rust line wrapping was verified
  to change only whitespace; Go scanner tokens also match. Both source
  hashes are preserved and final native builds refreshed. No dependency
  downloads, version changes, global configuration changes, commits or pushes.
- Supplemental Linux Black invocation could not run because that module is
  absent. Existing Windows Black checked the identical Python source; Linux
  Python unit tests then ran independently. No tool was installed.
- Evidence: [detailed hardening report](cluster/service-hardening-20260911.md)
  and [machine-readable results](testkit/results/service-hardening-20260911.json).
  Existing gRPC migration and historical performance reports are preserved.
  Business replication/persistence, SDK Bind, bearer lifecycle and production
  capacity/endurance qualification remain outside this service skeleton.

### 2026-09-11: Complete gRPC service migration and account admission validation

- Rust Star/Planet now use Tonic bidirectional streams; Go Supervisor uses
  gRPC Challenge/Register and account/password login. One account can admit
  multiple independent nodes. Signed bearer credentials replace TLS exporter
  and process signing keys; TLS 1.3, role/scope checks and CAS epochs remain.
- Generated Rust/Go RPC sources are stored with source. Approved grpc-go
  1.83.2, protoc-gen-go-grpc 1.6.2 and necessary dependencies are cached only
  inside both projects. Production custom frame readers/writers were removed.
- Fixed ownership of slow pre-TLS sockets during Go shutdown; checked Rust's
  cancellation path. Preserved pre-allocation Member limits and restored own
  certificate validation after removing mutual TLS. Adjusted race test budgets
  after an initial Linux timing failure without relaxing production deadlines.
- Both hosts passed 36 Rust service tests, 32 top-level Go tests, four generator
  tests, 14 probe tests, static checks and Release builds. Python: Windows 24
  passed, Linux 23 passed/one platform skip. Linux Go race passed; Windows race
  was not run without a configured cgo environment.
- Windows, Ubuntu and direct mixed-host deployments each passed all 11 real
  process scenarios and a requested 60-second fault loop with nine restarts.
  Owned processes, temporary directories and listening ports were recovered.
  Ubuntu's final boot-wide OOM kill counter was zero. No firewall/global changes.
- Protocol v4 requires coordinated upgrade and a new member database path;
  old data is preserved. Bearer expiry/revocation, business replication, SDK
  migration and endurance/capacity qualification remain outside this result.
- Evidence: [detailed report](cluster/grpc-validation-20260911.md) and
  [machine-readable results](testkit/results/service-grpc-20260911.json).

### 2026-09-10: Complete the conditional gRPC push evaluation and detailed report

- The isolated `testkit/transport` workspace retains existing locked versions
  and adds 63 approved package versions in both project Cargo caches. Production
  manifests and protocols are unchanged; no SDK tests or Go gRPC downloads.
- The shared publisher exercises real string-key caches, Registry counts from
  100 to 100,000, 64/256/1024 B Catalog updates, 1/4/16 receiving sessions,
  sparse application progress, burst scheduling and a 200 ms receiver pause.
  Final v3 uses up to 16 ready TCP frames per flushed batch; earlier echo,
  per-frame-flush and no-flush results are excluded from its conclusions.
- Finished 22 cases × two transports × five alternating rounds on each of
  Windows, Ubuntu and Ubuntu-publisher/Windows-receiver sessions. Counts are
  216/220, 220/220 and 214/220: 650 completed, 10 failed, 73,200,050 delivered
  updates in completed samples. TCP completed 327/330 and gRPC 323/330.
  Source maps and platform binary hashes match between environments.
- Ordinary/Registry scenarios and Catalog rates up to 20,000/s all completed.
  Linux high-load throughput is similar, but gRPC update P99 and process CPU
  seconds are higher. Four Windows failures have receiver-lag diagnostics;
  six cross-host failures retain client errors but lack detailed server logs,
  so their root causes remain unresolved. No failures were replaced by reruns.
- The maintainer's no-slowdown condition is not established; production stays
  on TCP + Protobuf. This is a conditional evaluation outcome, not permanent
  exclusion of gRPC. Full migration, capacity/SLA certification and long-term
  recovery validation remain outside the completed experiment.
- Fixed production common TLS tail flushing inside the existing write deadline
  and checked propagation across both roles, Go Supervisor and SDK transports.
  Both platforms pass 39 production Rust tests and 14 prototype tests plus
  Clippy; Windows Python has 24 passing tests, Linux 23 plus one Windows-only skip.
  Atomic report replacement now tolerates brief Windows readers with a bounded
  retry; cleanup tests isolate their unrelated memory precondition.
- Final process/temporary-directory checks are clean on both hosts. Removed
  one ownership-verified stale Windows pre-experiment directory. Ubuntu's OOM
  kill count did not increase; minor system swap activity is documented.
- Evidence: [detailed report](cluster/grpc-benchmark-results.md),
  [TLS flush audit](cluster/tls-flush-audit.md), the three
  `testkit/results/transport-push-*-20260910-v3.json` matrices and
  `testkit/results/transport-cleanup-20260910-v3.json`.

### 2026-09-10: Star / Planet connection foundation and cross-platform validation

- Organized Rust into `cluster/common`, `cluster/star`, `cluster/planet`, preserving
  `peer` for Star and adding `planet`. Shared protocol and session code is not
  duplicated between roles; no new dependency downloads were needed.
- Protocol v3 binds role/group into admission, preserves Star full-list
  semantics and adds MessageID 10 for at most eight Planet candidates.
  Planet keeps one upstream with local-group preference and offline failover;
  Star mesh capacity and counts exclude Planets.
- Fixed authenticated stable-window timing, candidate starvation and strict
  legacy member-row decoding. Reviewed propagation across Rust roles, Go
  Supervisor, Python harness and existing Redis SDK/binding boundaries.
- Both hosts passed offline generation, formatting, static checks, Rust tests
  (37 plus four generator tests), Go tests, docs and release builds; Linux
  also passed Go race with one build job. Windows, Linux and mixed-host real
  processes each passed regression and a 60-second, ten-cycle fault loop;
  final decoder changes were followed by all three regressions again.
  Owned processes, ports and temporary resources were cleaned up.
- Updated [connection rules](cluster/connection-rules.md), project memory and
  service documentation. Evidence and limitations:
  [validation](cluster/star-planet-validation-20260910.md),
  [JSON](testkit/results/star-planet-foundation-20260910.json).
- Business cache/replication, persistence, SDK binding and Registry re-registration
  remain unimplemented and are not claimed by this connection foundation.

### 2026-09-10: Record full Planet replicas, selectable storage and grouped Star candidates

- Recorded GQ-001 through GQ-005: running Planet failover with existing valid
  authorization during Supervisor outages; full authorized Galaxy caches;
  memory/disk modes for Stars and Planets; mixed Star modes; new processes
  waiting for Supervisor authentication even after a previously joined deployment restarts.
- Replaced demand-driven upstream subscriptions and memory-only Relay proposals.
  Full replica data remains separate from managed active publications that are
  re-registered on failover. Planet disks are not authoritative durable voters.
- Flagged mixed-mode durable participant and write-confirmation rules for design;
  memory acceptance cannot masquerade as persistence. Retained disk recovery
  for persistent deployments without promising recovery of lost all-memory state.
- Recorded GQ-006/GQ-007: group Stars by actual region or similar properties;
  prefer the local group and allow authorized cross-group failover on failure.
  Grouping does not partition Galaxy replication; offline fallback still needs
  previously learned candidates and valid authorization.
- Reconciled the shared architecture, service design entry points, Admin document
  pointers and project memory. Documentation only; no dependencies downloaded,
  runtime source changed or business recovery test evidence claimed.
- Validation: existing Admin Prettier passed; checked nine Markdown files for
  whitespace/conflict markers and 82 relative file links outside code examples.
  Git diff whitespace checks passed; runtime tests are not applicable to this revision.

### 2026-09-10: Review Galaxy documents and raise the first decision batch

- Cross-checked the shared Galaxy design, Peer owner/replication draft,
  Supervisor availability contract and durable project decisions.
- Identified superseded fixed-Star routing and owner-equals-sender text as
  document reconciliation work, not questions to ask the maintainer again.
- Raised GQ-001 through GQ-003: Planet failover with Supervisor offline,
  demand-scoped versus full authorized data caching, and memory versus disk cache.
  These were unanswered when raised and are resolved by the newer decision entry above.
- Deferred implementation-level epoch issuer, indexing and scheduling choices
  until the product behavior is clear. No runtime code, dependencies or tests changed.

### 2026-09-10: Accept re-registration at the new Star with propagated invalidation

- The maintainer confirmed that a Planet changing Star registers its managed
  data at the destination, which propagates new affiliation and invalidates old
  affiliation. Recovery need not first contact the unavailable source Star.
- Updated the shared architecture and Peer design to supersede fixed Star ingress,
  while preserving business identities, logical writers and confirmed payloads.
- Recorded versioned replacement as the recommended implementation shape, avoiding
  unordered unconditional delete/register messages. Partial registration batches,
  stale callbacks/leases and old-source recovery remain explicit validation cases.
- Planet-initiated recovery is now accepted; proof scope, generation authority and
  ACK semantics remain protocol questions. A read-only cached record is not a
  registration managed by that Planet. Catalog durable recovery is still required.
- Documentation-only revision; checked whitespace and local references. No runtime,
  generated protocol, dependency, system setting or test result changed.

### 2026-09-10: Clarify Star processing, Planet forwarding and Registry failover boundaries

- Recorded the maintainer's direction: Stars process Publisher/Registry requests
  and synchronize to other Stars and attached Planets; Planets forward those
  requests only to the currently bound Star while reusing storage/sync semantics.
- Planet authenticates with Supervisor but receives a subset of candidate Stars,
  not full mesh membership. Candidate selection and its authorization semantics
  remain recommendations, not a implemented partial member-list protocol.
- Reviewed Registration identity preservation, stale candidate fallback, lagging
  destination state, ambiguous replies and old expiry/unregister races. Added a
  review flow with authenticated rebind and lease-generation fencing; the issuing
  authority and Planet delegation remain open rather than introducing a global
  per-Registration ownership service without a decision.
- Identified the old fixed-owner conflict: routing back to A cannot recover
  service at B while A is unavailable. Kept this Registry design change separate
  from Catalog's authoritative persistence and controlled owner takeover.
- Linked the expanded shared architecture from Peer/Supervisor drafts and project
  memory. Read Chubby's sequencer discussion and etcd's ambiguous completion
  contract as references; neither system nor any new dependency was installed.
- Documentation-only work; checked local references and whitespace. Existing
  network tests do not constitute evidence for unimplemented Planet/Registry recovery.

### 2026-09-10: Validate collision-free planet motion during generation

- Following the maintainer's clarification, perform collision checks only while
  generating the orbits. Spatial paths may cross at different times; there is
  no per-frame collision steering or positional correction.
- Choose integer harmonics of a common repeat period and recompute semi-major
  axes to retain the Kepler constant. Screen deterministic inclination/phase
  candidates with synchronous swept segments, model bounds, a safety margin,
  and a conservative acceleration-based interpolation error envelope.
- Plan all available systems and entity types together, including stellar/core
  obstacles. Treat snapshot positions as layout hints and retain input ownership.
  Initial instance poses, picking and focus use planned orbits; larger layouts
  adjust peer focus distance. Wrap the shared clock by the checked period.
- Full Admin checks passed: 84 import boundaries, 53 tests, formatting, strict
  types and production build. Independent full-cycle sampling of the 144-body
  fixture found minimum surface clearance 0.346 at test radius 0.82. Tests also
  cover coincident hints, spatial path reuse, deterministic order and recurrence.
- Browser review verified overview, peer focus, planet selection/pause and focus
  without console warnings/errors. Existing LAN service remains on port 5173;
  no dependencies or GLB assets were downloaded or changed.

### 2026-09-10: Document the revised Galaxy structure and aggregation Relay role

- Reviewed Admin's GalaxyData/PeerStar/Planet types, demo snapshot, reference
  validation and black-hole rendering. The current UI puts business entities
  directly under Peer as planets and has no Relay or satellite layer.
- Recorded the maintainer's mapping: Galaxy contains Peer stars; Relay planets
  attach to stars; business satellites may bind either level; unavailable Peers
  become black holes. Cross-Galaxy Catalog wormholes remain unimplemented future work.
- The maintainer explicitly chose aggregation and cached downstream distribution
  for Relay. Documented shared-state ownership, permission-safe aggregation,
  bounded slow-consumer handling and invalidation/recovery requirements.
- At this stage, satellite granularity, single active upstream and memory-only
  caching were recommendations. Later entries confirm the Planet's single active
  Star and replace the cache proposal with full authorized, selectable-mode storage;
  Relay implementation language and packaging remain undecided.
- Added [galaxy-architecture.md](galaxy-architecture.md) and linked service,
  Peer/Supervisor and Admin design documents. Corrected stale current-memory
  statements that still described registration and process UUIDs as unimplemented.
- This is a design/documentation revision. No production source, executable,
  Protobuf, dependencies or system configuration changed. Checked Admin Markdown
  with its existing Prettier and checked document references and whitespace;
  runtime tests are not new evidence for this unimplemented structure.

### 2026-09-10: Enable a baseline stellar rotation speed

- Set the default available-star rotation to 2*pi/60 radians per second,
  one revolution per minute, in the existing runtime configuration.
  Per-peer speed overrides and stopping remain available and independent of count.
- Updated the existing rotation regression for quarter/full default revolutions
  and independent overrides. All Admin checks passed: 81 import boundaries,
  50 tests, formatting, strict types and production build.
- Browser review confirmed default rotation with planet orbits paused and no
  console warnings/errors. No dependency changes; the LAN service remains running.

### 2026-09-10: Scale stars by planet count and expose independent rotation speeds

- Added the requested piecewise linear size mapping: counts 10/30/60/120/180
  map to scales 0.5/1/1.5/2/3, clamped outside that range. Stellar surfaces,
  coronas and picking share the transform; peer positions, orbital coordinates
  and black-hole scale remain independent. Demo scales are 1.1/1.3/1.5.
- Added GalaxyController.setStarRotationSpeed(peerId, radiansPerSecond).
  Following the maintainer's follow-up, speed defaults to zero and no business
  rule derives it. Finite signed speeds support rotation, reversal and stopping;
  invalid inputs, unavailable peers and inactive controllers are rejected.
- Rotation uses the existing frame loop and only rotates the stellar model.
  Added coverage for count landmarks, scaled picking, shared source ownership,
  frame independence, stopped planet orbits, reversal and numerical bounds.
- All Admin checks passed: 80 import boundaries, 50 tests, formatting, strict
  types and production build. Browser review verified overview scale differences,
  peer focus and explicit rotation/stop commands without console warnings/errors.
  No dependencies or assets were downloaded; the LAN service remains on 5173.

### 2026-09-10: Accelerate black-hole flow and rebalance its emission

- Tripled the instance flow clock, including texture advection and bright-knot
  lifetimes, without increasing shear accumulated within each virtual cycle.
  Planet motion remains overview 5, peer 2, and selected planet 0.
- Broke distant broad bands into angular clumps with finer breakup and retained
  filtered filament energy. Raised hot inner emission toward warm white; restored
  a small amount of orange-red to the outer palette after maintainer feedback.
  Model dimensions, texture allocation and draw count remain unchanged.
- Updated clock regression assertions; final Admin checks passed all 80 import
  boundaries, 47 tests, formatting, strict types and the production build.
  Browser review covered near and distant views and distant flow playback with
  no shader warnings/errors; this is not a motion or performance qualification.
- Discussed node proportions without changing model scale. No installations,
  asset downloads or publication; the existing LAN service remains on port 5173.

### 2026-09-10: Improve distant black-hole light separation

- Replaced the distant broad-cloud emission fallback with sparse, irregular
  luminous streams baked into the existing plasma texture's unused B channel.
  Near-view filaments and temperature data remain in R/G. Rejected an initial
  regular-band variant that looked like concentric neon rings.
- Reduced extra distant derivative blur from 2 to 1.15 while retaining mipmaps,
  anisotropy and seam filtering; raised the emission coefficient from 1.9 to
  2.15 and concentrated distant energy in separated streams. No new textures,
  draw calls, GLB changes or full-screen effects were introduced.
- Fixed-view browser comparisons covered distances 48/96/192 at 12 degrees
  and a distant 45-degree view; shader compilation produced no warnings/errors.
  The background review window refreshed slowly, so these captures are not a
  continuous-motion flicker or GPU performance qualification.
- Full Admin checks passed: 80 import boundaries, 47 tests, formatting, strict
  types and production build. No installs, downloads, commits or publication;
  the existing LAN development service remains on port 5173.

### 2026-09-10: Add elliptic planet motion and view-scoped selection

- Replaced rigid shell rotation with deterministic inclined Kepler ellipses,
  preserving initial snapshot positions and placing the owning star at a focus.
  A bounded Newton solver produces faster periapsis and slower apoapsis motion.
  The maintainer's final time multipliers are overview 5, peer view 2, and
  planet selection 0. Selecting a planet pauses all orbital motion.
- Overview rays only test stars; a peer view additionally tests its own planets.
  Both selectPlanet and focusPlanet reject requests outside the owning peer view.
  Picking, selection decoration and camera focus read current instance matrices.
  Nine shared GLB batches remain; independent motion uploads about 9 KiB of
  matrices per active frame for 144 planets, with no uploads while paused.
- Added the requested subtle core shading to the black-hole optical material,
  explicitly as an artistic volume cue. The foreground disc still occludes it.
  Doubled disc and knot flow speed, retained broad cloud contrast and moving
  highlights in distant views, and kept one black-hole draw with existing assets.
- Full Admin checks passed: 80 import boundaries, 47 tests, formatting, strict
  types and production build. Browser checks covered scene load, peer focus,
  controller selection rejection/acceptance and paused canvas planet picking.
  The current remote overview showed about 32 FPS; this is not GPU capacity
  qualification. The moving-target click limitation and exact scope are recorded
  in admin/docs/verification.md. Development-only visual pages remain available.
- No downloads, installs, commits or publishing. The LAN development service
  remains on 0.0.0.0:5173.

### 2026-09-10: Correct black-hole emission and reduce its scene footprint

- Rebuilt the original black-hole GLB with an inner edge at three Schwarzschild
  radii, an outer radius of 24 and a 0.28 maximum half-height. Star and planet
  assets remain unchanged. The complete black-hole instance now uses a 0.75
  scene scale, including its picking proxy, at the maintainer's request.
- Emission uses the curved ray's conserved angular momentum for frequency shift
  and brightness. Both disc intersections share emission and occlusion rules.
  A periodic two-dimensional density field replaces repeated radial noise;
  unresolved image energy is integrated at initialization. Per-draw local camera
  uniforms retain shared resource ownership and independent instance transforms.
- Kept a development-only fixed-angle review page under
  `admin/tests/visual/black-hole.html`. An experimental curved-volume integration
  was rejected after visible banding and a significant frame-rate regression;
  final rendering retains thin-disc intersections and a four-sample local rim.
- `pnpm check` passed: formatting, 78 import boundaries, all 42 tests, strict
  type checking and the production build. The development galaxy loaded with
  no browser warnings or errors and about 32 FPS in the current remote browser.
  This is not hardware-GPU performance qualification or a complete interaction
  and failure-matrix rerun. Detailed evidence is in `admin/docs/verification.md`.
- No dependencies or third-party assets were downloaded. The LAN development
  service was restarted on `0.0.0.0:5173`; no changes were committed or published.

### 2026-09-10: Preserve resolved black-hole disc images at intermediate distances

- Reproduced the opaque upper arc at inclination -2 degrees and distance 96.
  Whole-node distance detail was replacing a resolved secondary image with
  fixed-radius emission, losing radial falloff and producing hard boundaries.
  The previous visual pass had omitted negative-inclination intermediate views.
- Secondary images now retain their actual disc intersection at every distance.
  Only images near the pixel footprint blend with the coverage approximation;
  emission is blended by coverage to keep transparent edges from picking up a
  white rim. Distance detail still reduces knots and texture detail, but no
  longer skips the secondary intersection lookup or replaces a broad arc.
- Fixed-time browser comparisons at 1280x720 covered -2 degrees at distances
  48, 96, 192 and 384, plus +2, 0 and +12 degrees at distance 96. The opaque arc
  no longer appeared; logs were clean and the model still used one draw.
  Perspective taper remains; this does not claim full volumetric relativistic
  transfer. The temporary comparison page was removed.
- Formatting, 77 import boundaries, 42 tests, strict types and production build
  passed. No GLB, dependency or downloaded asset changed. Production browser
  preview, the full interaction matrix and performance capacity were not
  rerun. The existing LAN development service remains on port 5173.

### 2026-09-10: Add finite black-hole rims, differential flow and distance detail

- Added a four-sample foreground rim with a finite elliptical cross-section,
  matching the original GLB's 0.16 half-height. Filter footprints follow ray
  sample spacing to prevent edge-on barcode shimmer. The black-hole GLB is
  1,217,688 bytes; star and planet hashes are unchanged.
- Replaced rigid model rotation with two continuously blended differential
  flow phases, three transient sheared bright knots and baked azimuthal density
  variation. Each visible instance binds its own clock immediately before its
  draw while retaining shared, scene-owned geometry, materials and textures.
- Projected shadow size smoothly reduces distant secondary-image and knot
  detail. Stabilized zero-inclination intersection selection against floating
  point noise, removing close-up edge speckles. Finite thickness only augments
  the local foreground; lensed images remain a thin-disc approximation.
- Verification passed formatting, 77 import boundaries, 42 tests, strict types
  and production build. Browser checks covered reference, both sides of the
  disc plane, exact edge-on, elevated, close and far views; the far view reached
  detail zero with one draw. Production checks covered model loading, isolated
  Orion, focus without details and rotation. Final browser logs were clean.
- Remote production overview and ordinary focus showed about 32 FPS; enlarged
  isolated views showed about 15-18 FPS. Hardware 60 FPS and capacity remain
  unqualified. The full browser failure matrix and planet details were not
  rerun. No dependencies or third-party assets were downloaded. Removed the
  temporary review page and stopped its production preview; LAN development
  remains on port 5173.

### 2026-09-10: Accelerate black-hole flow and preserve both images near the disc plane

- Increased the display flow rate from 0.025 to 0.075 radians per second,
  approximately 84 seconds per rotation, without additional draws or sampling.
- Restored secondary-intersection visibility continuously near the disc plane,
  where the first intersection can lie outside the emitting disc. This avoids
  a false one-sided flip when the ordered intersections exchange sides.
- Browser checks covered inclinations +2, +0.5, 0, -0.5, -2 and +12 degrees;
  logs were clean and the temporary review page was removed. The infinitely
  thin foreground disc still degenerates at exactly zero inclination; finite
  thickness is documented as future work, not claimed as implemented.
- Formatting, 74 import boundaries, 41 tests, strict types and production build
  passed. No dependency was added. Production preview, the complete browser
  failure matrix and hardware GPU capacity were not requalified in this pass;
  the existing LAN development service remains on port 5173.

### 2026-09-10: Unify Admin black-hole imaging to remove the protruding hemisphere

- Replaced the combination of rasterized core/disc surfaces and separate
  lensed arcs with one closed GLB optical volume. It composites two ordered
  disc intersections and captured-ray shadows; the hidden Core mesh remains
  pickable. Disc and Glow remain offline authoring geometry. The revised GLB
  is 1,217,692 bytes; star and planet hashes are unchanged.
- Extended the orbit grid across both sides of the critical impact, retained
  captured rays at the horizon after termination, and added finite-observer
  phase lookup. Corrected periodic texture derivatives and concentrated the
  secondary image brightness near the shadow to reduce its broad lower ring.
  Textures remain shared and scene-owned; no new dependency or downloaded
  asset was used. External scene occlusion uses a documented virtual depth,
  not curved-ray intersections with surrounding stars.
- Verification: final formatting, 74 import boundaries, 41 tests, strict types
  and production build passed. Isolated visual checks covered front, below,
  45-degree and top views plus distance-8 closeups; the temporary viewer was
  removed. Production checks covered topology isolation, hidden-core focus,
  no automatic details, rotation and double-click return. Final browser logs
  were clean. Remote FPS remained about 32; hardware capacity and the complete
  browser fault matrix were not requalified. LAN development remains on 5173.

### 2026-09-10: Refine the Admin black-hole model and render lensed disc images

- Reauthored the original GLB with a finer shadow core and continuous accretion
  disc. The black-hole asset is 1,217,704 bytes; star and planet hashes remain
  unchanged. Unavailable Orion still has no planets, links or status marker.
- Replaced preset light arcs with initialization-time RK4 light-orbit tables
  and disc intersections. The physical disc and lensed images share a filtered,
  original plasma texture; thin higher-order images use coverage filtering.
  Inclination blending removes duplicate disc images at elevated viewpoints.
  This is an art-directed hybrid, not a complete relativistic renderer.
- Textures and geometry remain scene-owned and shared among model instances.
  No dependency, external model, full-screen effect or extra frame loop was
  added. Updated authoring provenance, architecture and verification notes.
- Verification: 39 tests, 73 import boundaries, formatting, strict types and
  production build passed. Browser checks covered near zoom, multiple viewing
  angles, production asset loading, sidebar collapse, isolated unavailable
  topology, focus without details and double-click return, with no warnings or
  errors. The temporary isolated review page was removed. The remote browser
  showed about 32 FPS; hardware capacity and the full browser fault matrix were
  not requalified. The existing LAN development service remains on port 5173.

### 2026-09-10: Replace the Admin black-hole wire appearance with a continuous disc

- Responded to the maintainer's visual feedback by removing the eight luminous
  tube streams. Reauthored the GLB with a continuous disc and a closed ellipsoid
  Glow volume; added warm plasma texture, softened the lensed arc and adjusted
  the viewing inclination. Star and planet GLB hashes remain unchanged.
- Local glow uses six bounded density samples, core occlusion and separate
  color/optical-depth blending to avoid an overbright sRGB haze. Resources
  remain scene-owned, without new dependencies, external assets, additional
  frame loops or full-screen effects. The black-hole GLB is now 403,752 bytes,
  down from 578,952; this size reduction is not a GPU performance claim.
- Verification: all 36 tests, 67 import boundaries, formatting, strict types
  and production build passed. Browser checks covered focus, near zoom,
  oblique rotation and double-click return without warnings or errors.
  Production preview, the full browser failure matrix and hardware GPU
  capacity were not requalified during this appearance revision.

### 2026-09-10: Rebuild the Admin black hole around the accretion-disc reference

- Reauthored the local black-hole GLB with a thin volumetric disc, tapered
  spiral streams and a larger spherical optical shell. The opaque core,
  independent model poses and unavailable-node isolation remain in place;
  the star and planet model bytes are unchanged.
- Added view-dependent upper/lower light arcs and a narrow bright rim on the
  spherical shell. Disc brightness follows the viewing direction relative
  to its orbital flow. These are bounded analytic visual approximations,
  not general-relativistic ray tracing or fixed vertical ring geometry.
- Shared the disc material between the disc and streams; retained the
  existing resource scope and frame loop without external assets, new
  dependencies, full-screen effects or per-frame uniform allocations.
- Verification: `pnpm check` passed 67 import boundaries, all 36 tests,
  strict types and production build. The GLB regression also checks that
  the optical axis remains aligned with the rotating disc. Browser checks
  covered focus, near-view zoom, oblique rotation and double-click return,
  with no warnings or errors. Large-fleet GPU performance and the full
  browser failure matrix were not requalified in this visual revision.

### 2026-09-10: Complete direct Windows/Ubuntu service qualification

- The maintainer explicitly approved temporary inbound rules for the already
  built Peer and Supervisor, restricted to local 192.168.0.25 and remote
  192.168.0.119. The administrator helper created exactly two owned rules;
  no existing rules were edited and the firewall was not disabled.
- Ran the final service binaries with two Peers on each host. All six real-process
  regression groups passed, including full mesh, Supervisor outage/recovery,
  durable membership, Peer normal/forced restart, invalid identities and cleanup.
- The subsequent 60.000-second fault window completed 13 restart cycles;
  minimum available memory across both hosts was 6753 MiB. Total regression,
  fault loop and cleanup time was 75.955 seconds. This short run is not a
  production endurance or large-cluster capacity qualification.
- Port rebind assertions passed. Independent final checks found no owned service
  processes or temporary test directories on either host. Ubuntu had 6747 MiB
  available memory and zero swap usage at the final check.
- Both temporary rules were removed; an elevated ActiveStore query confirmed
  no matching rule remained. `build/testkit/firewall-ab5d4456.json` records
  `removed`, an empty remaining-rules list and no error. The helper exited.
- Evidence: `build/testkit/results/services-1789007545576233300.json`,
  `build/testkit/mixed-services-final.log`, and the updated
  [validation report](service-admission-validation-20260910.md).
  The earlier failed network attempt remains recorded as failed. No dependencies
  were downloaded, and no commit or push was performed during this follow-up.

### 2026-09-10: Implement authenticated service membership and native qualification

- Checked generated Rust/Go protocol source into each service source directory.
  `proto/generator` owns its lockfile and append-only MessageID tests; normal
  builds do not invoke protoc. Both service checks detect generator drift.
- Integrated TLS 1.3 mutual authentication, certificate cluster/IP authorization,
  signed admission returned with the complete transactional member list, and
  per-process Ed25519 proof bound to the current TLS exporter and Hello.
- Added bbolt member persistence, idempotent registration and fixed-baseline CAS
  replacement. Peer installs the validated list atomically, then establishes
  two directed sessions per pair. Discover and periodic topology queries are gone.
  Initialized processes retain offline membership and reconnect without Supervisor.
- Reused or downloaded only specifically approved Go modules into both project
  caches: protobuf 1.36.12, bbolt 1.4.3, x/sys 0.48.0, x/sync 0.10.0,
  testify 1.10.0, go-cmp 0.7.0 and necessary indirect dependencies. No global
  configuration or installation was changed.
- Completed functionality and tests, reviewed ownership and simplified code,
  then ran both native full gates. Each passes 29 Rust tests, 24 Go top-level
  groups and four generator tests, plus formatting, Clippy/vet, module verification,
  generated-source comparison, documentation and Release builds. Linux Go race passes.
- Added one-key real-process regression and configurable soak entry points with
  owned PID/Job/process-group cleanup. Both platforms pass concurrent mesh,
  Supervisor outage/recovery, durable restart, normal/forced Peer replacement,
  invalid credentials, actual exit signals and port reuse. Final runtimes were
  14.312 seconds on Windows and 10.504 seconds on Ubuntu, excluding build gates.
- Native 60-second fault windows pass: Windows 10 cycles/63.084 seconds and
  Ubuntu eight cycles/60.739 seconds. The final Protobuf predecode count/phase
  guards were added afterward and passed fresh full gates and process regression;
  those earlier fault windows are not claimed as rerun on the final guards.
- Audited corresponding Go/Rust paths for each issue, including address canonicality,
  error echo, message allocation, lifecycle fencing and fixture key formats.
  Final independent checks found no owned test processes or temporary directories;
  Ubuntu had 6768 MiB available memory and zero swap usage.
- Direct mixed-host qualification was initially blocked and is completed in the newer entry above. Business
  Catalog/Registry replication, Peer authoritative recovery, certificate rotation,
  member retirement and Supervisor HA remain outside this network slice.
- Evidence: [validation report](service-admission-validation-20260910.md).
  No commit or push was performed.

### 2026-09-10: Use GLB celestial models and isolate unavailable Admin nodes

- Replaced runtime sphere/sprite construction with three original GLB assets
  for stars, planets and black holes. Added an offline authoring script using
  the existing Three.js exporter, with reproducible model files and asset
  provenance. Star surfaces and corona shells are meshes; the black hole has
  a solid core, horizon shell, volumetric disc and merged spiral streams.
- Removed Orion's planets and incident links from the demo. Runtime also
  suppresses stale entities, picking entries and links for any unavailable
  Peer; available-neighbor counts follow the same visibility rule. The demo
  now has four Peers, 144 planets in nine instance batches and three links.
- Load same-origin GLB assets before creating the canvas; cancellation,
  snapshot replacement and errors release resources through the scene scope.
  Each model type is loaded once per scene and shared across instances. No
  dependency installation, external model acquisition or extra RAF was added.
- Verification: `pnpm check` passed 67 import boundaries, all 36 tests, strict
  types and production build. Regression coverage uses the real GLB files for
  instancing, planet ray picking, independent black-hole poses, ownership,
  unavailable filtering, model errors and cancellation before/during loading.
  Browser checks verified black-hole and star focus, disc depth during rotation,
  double-click overview, and all three asset types in production preview with
  no warnings or errors. Moving-planet detail opening was not requalified in
  this browser pass; the GLB world-position/picking regression passed. Large
  fleet performance and the full browser failure matrix remain unqualified.

### 2026-09-09: Render the unavailable Admin peer as a black hole

- Replaced Orion's dim rock and red cross with an opaque black core, a
  camera-facing event-horizon rim and a tilted, slowly flowing accretion disc.
  Retained the three available stars, dim static entities, dashed incident
  links and existing cluster/planet selection contracts. Removed the X marker.
- Kept the effect in the Three.js runtime with owned ring geometries and
  materials, shared core geometry and the existing frame loop. The stylized
  shaders add no dependency, texture, full-screen postprocessing or ray tracing.
  Updated the detail icon, accessibility description and Admin documentation.
- Verification: `pnpm check` passed 62 import boundaries, all 34 tests, strict
  type checks and the production build. Browser checks covered overview,
  black-hole focus without automatic details, disc occlusion during rotation
  and Orion's Publisher details with 24 entities and two neighbors; no browser
  warnings or errors. This remote display still showed about 32 FPS; no hardware
  performance claim is made.

### 2026-09-09: Add an unavailable star to the Admin demo

- Added Orion as a fourth, unavailable Peer while retaining Atlas, Lyra and
  Vega as available with their original entities and links. Orion contributes
  24 retained entities and two explicit neighbor links; the demo now has
  168 planets in 12 instance batches.
- Added explicit display-only Peer availability and validation. Unavailable
  stars use a dim rock surface, no corona and a camera-facing red cross badge;
  incident links are dashed and retained planets are dimmed and stationary.
  Stars/planets remain selectable and focusable; details identify the unavailable
  owner and retained demo information. No backend availability is inferred.
- Validation: `pnpm check` passed with 60 import boundaries, 34 tests, strict
  Vue/TypeScript and production build. Regressions verify one additional
  unavailable Peer, retained picking, dashed edges and persistent orbit pause.
  Browser checks verified all four stars together, Orion focus and its planet
  details with the unavailable status, 24 entities and two neighbors.
- Scope: browser demo/presentation only, no Peer/Supervisor/SDK wire change,
  new dependency, installation, commit or publication.

### 2026-09-09: Smooth focused-star dragging in Admin

- Traced focused dragging through scene composition, CameraMotion,
  canvasInput and the installed Three.js OrbitControls source. OrbitControls
  applied damping in every pointer event as well as each render frame, making
  motion depend on event density; focus cancellation also waited for a
  five-pixel drag threshold.
- Added a frame-owned OrbitControls adapter: input accumulates rotation/pan,
  rendering consumes it once with a 90 ms time constant. Reduced rotation and
  pan sensitivity to 0.65/0.8 for closer control. Pointer press now cancels
  focus/zoom immediately; the threshold only classifies clicks. Starting focus
  or hiding the page clears residual drag without changing the current pose.
  No Three.js private fields or dependency changes were needed.
- Validation: `pnpm check` passed with 57 imports checked and 32 tests, including
  1-versus-16 input batches per frame, 30/60/120 FPS damping equivalence,
  focus handoff without recoil and sub-threshold pointer takeover. Strict
  Vue/TypeScript and production build passed. Browser smoke covered dragging
  during focus and after focus, with a stable stellar rotation center and no
  warnings/errors. This improves motion consistency, not the previously
  measured remote/software-rendering refresh limit.
- Propagation: shared camera input applies to stars, planets and overview;
  this is browser presentation only and does not change Peer, Supervisor or
  SDK protocol behavior. No install, commit or publication.

### 2026-09-09: Establish the Admin frontend foundation

- Reorganized `admin/` into app composition/layout/styles and a galaxy feature
  with readonly model, injected demo snapshot, Vue UI/composable and isolated
  Three.js runtime. Removed the former monolithic scene and mixed data/view
  entry points; retained the three stars, 144 sphere instances, quiet FPS-only
  overview, smooth camera controls and planet details.
- Extracted camera motion, canvas input, frame scheduling, scene objects and
  resource ownership. Stable IDs replace object-identity lookup; explicit links
  replace inferred complete graphs. One selection owner now clears both scene
  and details on overview. Dragging back to the pointer origin no longer counts
  as a click.
- Added initialization rollback, reverse/idempotent cleanup, stale async-load
  fencing, snapshot replacement and a context-loss/render-error retry path.
  The scene releases listeners, observers, RAF, instance/GPU resources and
  canvas/context on disposal. Vue receives no per-frame Three.js objects.
- Added strict indexed/optional-property/erasable-syntax checks, LF/editor
  conventions, architecture import checks, Node 24 built-in tests and the
  unified `pnpm check` gate. Added Admin architecture, contributor and browser
  verification documents; linked the stable decisions from root conventions.
- Validation: Node 24.21.0 / pnpm 12.3.4 `pnpm check` passed: formatting,
  54 imports checked, 28 regressions, Vue/TypeScript and Vite production build.
  Browser checks covered focus/zoom/details/list selection/double-click/sidebar;
  detail-click smoke temporarily froze orbit to avoid automation latency, then
  restored the production orbit path before the complete check. Real Three.js
  tests cover rotating-world-position picking and selected-system pause.
  A temporary UI harness verified real WebGL context loss/retry, unmount/remount,
  empty/invalid/replacement snapshots, and zero/one canvas cleanup/recovery;
  the harness was removed. Production preview also loaded the star field and
  split renderer with no warnings/errors or development frame-count attribute.
  Remaining browser fault-injection gaps are recorded
  in `admin/docs/verification.md`; this is not hardware or capacity qualification.
- Scope/propagation: fixes are confined to browser presentation and lifecycle;
  no matching SDK/wire behavior was changed. Supervisor remains disconnected
  from the demo. No new dependency, install, upgrade, commit, push or publication;
  concurrent Peer/Supervisor/SDK changes were preserved.

### 2026-09-09: Normalize Peer and Supervisor service foundations

- Split the Rust executable into a thin `main` and private process composition,
  CLI, signal and JSON logging modules; retained the existing concrete network
  library. Renamed the binary to `peer`, added equals-style arguments, version,
  bounded runtime workers and shutdown, and consistent 0/1/2 exit codes. Split
  Go management handlers from app-owned listener/shutdown responsibility.
- Removed stdin/EOF termination and the detached reporter/blocking stdin task.
  Added cancellation-safe `Peer::wait` so root network failure reaches the
  process; broken log output also cleans up without println panic. Verified
  Supervisor's analogous Serve-result observation with a new regression.
- Expanded Chinese/ASCII ownership, configuration and block comments; enabled
  missing public Rust documentation and broken-doc-link denial. Added root
  contributor guidance, service ownership/release gates and dependency metadata.
- Ordinary Peer commands now default offline. Protocol IDs remain automatic,
  but stale source locks fail normal builds; only explicit generation may write
  them. Frozen one-command PowerShell/Bash checks force generation permission
  off, bound jobs to 2 by default, and fail rather than silently skip tools.
- Checked corresponding Go/Rust/C++/C# production stdin and generation paths.
  Go generation is explicit, C++ embeds into binary output, and no SDK wire
  semantics changed. `peer.proto` and `message-ids.lock` remain byte-identical.
- Specifically approved downloads: signal-hook-registry 1.4.8 into project
  Cargo caches, plus official Rust 1.98.1 rustfmt/Clippy components into Ubuntu's
  project toolchain after SHA-256 checks. Reused existing serde_json dependencies.
  No global settings, toolchain upgrades, other package downloads, commit or push.
- Both native service-check entry points pass. Peer has 35 passing tests per
  platform; Supervisor has 8 top-level test groups plus subcases. Windows
  format/Clippy/docs/vet/tests/release build and Linux equivalents plus Go race
  pass. Final targeted tests cover RAII cleanup and unexpected listener failure.
- Linux real executables pass version/help/error codes, closed stdin survival,
  JSON startup/shutdown logs, SIGTERM exit 0 and released ports. Peer broken
  stdout exits 1 without panic/hang. Windows console-signal injection and race
  execution remain unverified; all test processes/resources were cleaned up.
- Registration, process UUID/Hello migration, legacy Discover replacement,
  durable Catalog authority, TLS/authentication and capacity/endurance proof
  remain explicit separate production gates, not placeholder implementations.

### 2026-09-09: Identify remote-session refresh and software rendering limits

- Ran a temporary browser diagnostic with six seconds of requestAnimationFrame
  alone, followed by six seconds with the complete galaxy. Baseline measured
  32.22 callbacks/s; the full scene measured 25.53 callbacks/s. This separates
  the existing refresh ceiling from additional scene rendering cost.
- The test browser reported ANGLE on Microsoft Basic Render Driver, device
  0x0000008C, Direct3D11: a software renderer. Windows reported the remote display
  at 32 Hz, NVIDIA hardware using Microsoft Basic Display Adapter, a Xeon
  E5-2680 v4 (14 cores/28 threads), and 31.8 GiB RAM. These observations do not
  establish performance on a hardware-accelerated client browser.
- Removed the temporary diagnostic after recording results. No production code,
  driver, remote-session settings or dependencies changed; no download/install.

### 2026-09-09: Use netutil for Supervisor connection admission

- With explicit maintainer approval, downloaded `golang.org/x/net v0.59.0`
  from the official Go module proxy with checksum-database verification into
  `build/deps/go/pkg/mod`. Pinned the module and `go.sum`; no toolchain or
  other new module was downloaded. The imported `netutil` package only needs
  standard-library packages.
- Replaced the private `listener.go` with `netutil.LimitListener`. Kept the
  full-capacity close regression and added a real HTTP test that verifies an
  idle connection holds capacity and closing it resumes a queued request.
  Existing drain, forced cancellation and port-release tests still apply.
- Windows: offline module tidy check, vet, shuffled tests and native build
  passed. Ubuntu: copied only Supervisor source and the verified module cache,
  then passed offline tidy, vet, shuffled race tests and native build with
  concurrency limited to 2. All test-owned connections and servers closed.
- Updated dependency preparation commands and architecture memory. Ordinary
  wrappers remain offline/read-only with project-local child-process caches;
  no global configuration changes. chi/gnet remain unselected. This is a
  Supervisor implementation simplification, not a shared SDK/protocol bug fix;
  Rust/Proto and other language implementations did not change.

### 2026-09-09: Smooth wheel zoom and target 60 FPS

- Changed the shared animation scheduler from 30 to a 60 FPS target, requested
  the high-performance adapter and capped render pixel ratio at 1.5. The FPS
  display continues reporting actual rendered frames; the verification browser
  currently reports about 32 FPS, so sustained 60 FPS is not marked verified.
- Intercepted wheel input before OrbitControls' immediate dolly. Wheel events
  accumulate a bounded target distance; the frame loop applies time-based smooth
  approach around the cursor anchor. Dragging, focus changes and page hiding
  cancel pending zoom. Middle-button dragging remains pan, not dolly.
- Browser forward/reverse zoom preserved the cursor anchor and returned without
  warnings/errors. Type checking, production build and formatting passed.
- The user confirmed remote desktop/streaming and explicitly deferred the dark
  rectangles. The earlier opaque-canvas adjustment did not solve that report;
  neither a renderer root cause nor video compression is confirmed. No further
  artifact fix is claimed. No dependencies installed, commits or pushes.

### 2026-09-09: Animate planetary shells and simplify the overview

- Added slow shell rotation in nine instanced batches with no per-frame instance
  buffer upload. Targeted 30 FPS for ambient motion, display-rate scheduling for
  interaction/damping/camera travel, and cancellation while the page is hidden.
  Selected planets pause their own system; picking and focus use rotated world
  coordinates. Resuming avoids accumulated hidden-time orbit jumps.
- Mapped middle-button drag to pan, retaining left-button rotation and wheel
  zoom. Changed the canvas to an opaque WebGL background, removed CSS gradients,
  discarded empty corona texels and disabled transparent decoration depth writes
  to address the supplied screenshot's rectangular compositing artifacts.
- Removed overview titles, labels, counters, cards, legend and instructions.
  Only the live FPS number remains at its upper right; planet details remain
  available on selection. Removed unused HUD CSS and label projection work.
- Browser sampling measured 1103 rendered frames over 36.744 seconds (30.02 FPS).
  The final visible counter reads 30. Verified rotation with a clean background,
  moving-planet picking and focus alignment. Type check, production build and
  formatting passed. No dependency changes/downloads; no large-scale benchmark.

### 2026-09-09: Keep planets spherical during direct zoom

- Removed the overview Points representation, whose default square sprites grew
  when zooming without selecting a star. All 144 planets now use the same shared
  sphere models in three instanced batches, independently of selection state.
- Verified direct wheel zoom from overview without selecting a star: nearby and
  neighboring-system planets remained spherical. Browser warnings/errors were
  empty; type checking, production build and formatting passed. No downloads.

### 2026-09-09: Improve star surfaces and add simple planet models

- Replaced the flat star grain shader with seamless three-dimensional turbulence,
  hot/cool surface detail, limb darkening and a shared corona with a transparent
  center. No animated time uniform or continuous rendering was introduced.
- Added simple rocky planets with shallow craters, shared color/bump texture,
  520-triangle sphere geometry and deterministic instance rotations. Preserved
  purple Registry, teal Subscriber and gold Publisher colors and instancing.
- All geometry and textures are generated locally with existing dependencies.
  Browser close-up inspection, planet picking and camera focus passed; no WebGL
  warnings/errors. Frame count stayed at 146 across 23 seconds idle. Type check,
  production build and formatting passed; large-scale performance remains untested.

### 2026-09-09: Implement the independent Go Supervisor skeleton

- Added the standard-library-only `supervisor/` Go module, command entry point,
  validated configuration, JSON logs, management `/healthz`, bounded listener
  and graceful/forced HTTP shutdown. Comments use Chinese with ASCII punctuation.
- Added project-local offline Go wrappers and exact build/run/test documentation.
  Management HTTP is explicitly separate from the future Peer registration
  endpoint; no fake membership, topology readiness or Catalog success responses.
- Windows: gofmt/160-column/comment checks, go vet, tests, build and executable
  help/invalid-configuration exit codes passed. The tests cover real HTTP,
  occupied ports, capacity-blocked accept cancellation, active-request draining,
  shutdown deadlines and port release. Windows race execution was unavailable
  with current cgo disabled; no compiler was installed.
- Ubuntu 192.168.0.119: synced only the new module, reused existing Go 1.27.1
  and GCC, and passed vet, race tests and native build with concurrency limited
  to 2. Actual binary startup with closed stdin, health, SIGTERM exit 0 and port
  release passed. All test processes/connections were cleaned up.
- No downloads, databases, Rust/Proto/SDK source changes, commit or push. Peer
  registration, durable member storage, process replacement and Catalog/UI
  integration remain unimplemented pending their separate protocol work.

### 2026-09-09: Smooth star navigation and broaden double-click return

- Replaced planet-only canvas double-click gating with scene-container handling
  for both focused stars and planets, including HTML star labels. Star selection
  now leaves details closed; planet details overlay the unchanged canvas.
- Camera travel now interpolates for 1.2 seconds with gradual acceleration and
  deceleration, preserving the current viewing direction for star approach.
  Removed the immediate-position branch; only dragging or scrolling interrupts
  travel, not a stationary pointer press.
- Browser checks observed intermediate and final camera views, no details on
  star selection, working planet picking, and canvas dimensions of 1060 x 619
  before and after opening details. Double-clicking a focused star or its label
  returned to overview. Browser warnings/errors were empty; production build,
  type check and formatting passed. No dependency changes or downloads.

### 2026-09-09: Return from a selected planet with a double-click

- Added canvas double-click navigation from a selected planet to the global
  overview, clearing details and restoring the overview camera. Preserved the
  selection at the start of the click sequence so double-clicking a star also
  returns correctly. Added a contextual gesture hint and listener cleanup.
- Verified browser double-clicks on a star and scene background, single-click
  planet picking and selection retention during dragging. Type check, production
  build and Prettier passed; browser errors were empty. No dependencies added.

### 2026-09-09: Add the three-star Supervisor UI demo

- Added Three.js with explicitly approved dependencies to `admin/`. Atlas, Lyra
  and Vega expose 36, 48 and 60 demo entities across Registry, Subscriber and
  Publisher. Planets occupy three-dimensional spherical shells; removed orbit
  guides and decorative background stars following user feedback.
- Added star selection, entity details, camera focus, overview reset and a dark
  collapsible sidebar. Peer links and all entities remain local demo data.
- Used instanced planets, shared resources, on-demand rendering and separate
  engine chunks. The browser frame counter stayed unchanged across a 42-second
  idle observation; this does not qualify large-scale performance.
- Verified type checking, production build, browser star/planet selection and
  camera focus. Final visual inspection confirmed the simplified background and
  spherical layout; browser warnings/errors were empty. No commit or push.

### 2026-09-09: Record simplified Peer CLI and process UUID identity

- Recorded `peer --listen=... --super=... --cluster=...` as the target interface,
  with `--name=value`, no manually configured ID and no new Zone flag. Peer ID
  is generated per process, retained across reconnects and changed on restart.
- Reviewed propagation into Hello/boot ID, durable owner references, same-address
  restarts, stale members, durable replica accounting and the optional join-order
  proposal. Marked stable-ID recovery assumptions superseded and dependent
  protocol choices unresolved; preserved the authoritative persistence goal.
- Updated Core, product and Supervisor designs, connection comparison, README
  pointers and durable decisions. Current CLI/network code remains documented
  as implemented, distinct from the new target. No source, schema or dependency
  changes, downloads or runtime tests. Documentation references and whitespace
  checked; no commit or push.

### 2026-09-09: Confirm Go Supervisor and Rust Peer

- Recorded the independent Supervisor service language as Go and retained Rust
  for Peer. Updated Supervisor/Peer designs, the README and durable decisions.
- Kept membership/management/Catalog publishing separate from Peer replication
  and authoritative persistence; the internal Rust connection supervisor stays
  in Peer. Storage engines and remaining protocol details are still undecided.
- Documentation whitespace checked. No backend scaffold, source changes,
  downloads or runtime tests were needed for this language decision.

### 2026-09-09: Clarify durable Peer authority and Supervisor Catalog publishing

- Recorded the final explicit choice: Peer retains authoritative Catalog on disk
  and recovers after whole-group restart with its Publisher offline. Superseded
  the intermediate memory-only direction; preserved existing A-001/A-002.
- Documented Supervisor as an ordinary authorized Catalog Publisher alongside
  its membership/observer roles, without a second Catalog authority. Distinguished
  durable acceptance, eventual replica convergence and SDK delivery.
- Added recovery boundaries for Publisher cache loss, tombstones, disk failures
  and Supervisor-offline bootstrap using persisted identity/known membership.
  Core memory tests remain a separate phase and cannot qualify disk recovery.
- Updated the design, Core/README pointers and durable decision record. Storage
  engine, Supervisor language and connection-count decisions remain unfrozen.
- Documentation references and whitespace checked. No source or Proto changes,
  downloads, runtime tests, commit or push were performed for this decision.

### 2026-09-09: Add sidebar icons and folding

- Added inline SVG branding/Home icons and an accessible footer toggle using
  existing Vue/Naive UI dependencies. The sidebar folds from 220 to 64 pixels,
  retaining icon navigation while the main content stays blank.
- Formatting, type checking and production build passed. Browser interaction
  verified both toggle directions, measured both widths and found no warnings/errors.

### 2026-09-09: Add the admin sidebar

- Added a 220-pixel Naive UI sidebar with Verdandi/Supervisor branding and
  one selected Home item; the main content remains blank. No new dependencies.
- Prettier, vue-tsc and the production build passed. The running development
  page displays the sidebar correctly with no browser warnings or errors.

### 2026-09-09: Add the minimal Supervisor admin homepage

- Added the independent `admin/` Vue 3, TypeScript, Vite and Naive UI project
  with one blank homepage. No router, shared state store or backend calls.
- The maintainer explicitly authorized its seven declared frontend packages
  and transitive dependencies. Installed through pnpm 12.3.4 with fnm-managed
  Node 24.21.0, using `D:\Program Data\pnpm\store` and `cache`; preserved
  the generated lockfile and ignored `node_modules/` and `dist/`.
- Prettier formatting, vue-tsc type checking and Vite production build passed.
  Browser verification of the production preview showed a blank white page,
  the expected document title and main region, and no console warnings/errors.
- No backend or Peer protocol changes, commit or push were performed by this task.

### 2026-09-09: Compare one full-duplex Peer connection with two mirrored connections

- Added `cluster/connection-model-review-20260909.md` and linked it from the Core
  and Supervisor designs. Compared exact socket counts, join/reconnect behavior,
  directional ownership, control/data queueing, duplicate arbitration and failure
  scope against TCP, Tokio and Erlang primary documentation and local code.
- Recommended considering one full-duplex socket with separate incoming/outgoing
  replication modules; this is a proposal, not a replacement of accepted CORE-002.
  Identified arbitration and bounded concurrent read/write as required design work.
- Clarified that first contact depends on the newcomer in both connection models
  when neither Supervisor push nor Peer introductions exist. Reverse dialing
  cannot discover a newcomer that has not yet contacted that old peer. First
  contact, later reconnect ownership and business request direction are separate.
- Refined the recommendation to fixed later-registered-to-earlier-registered
  dialing and reconnecting, using a persistent Supervisor-assigned join order.
  Documented stable order on retry/restart, filtering newer entries from a retry
  snapshot, directional reachability and remaining same-initiator stale sessions.
  This is still a design recommendation, not an implemented or accepted switch.
- No Rust/Proto edits, downloads or benchmark results. Documentation whitespace
  and new local references checked; existing dual-connection behavior unchanged.

### 2026-09-09: Simplify joining to Supervisor registration and a complete Peer list

- Recorded the final clarified flow: register and receive a complete list,
  actively connect, and let old peers learn the newcomer from Hello and create
  the reverse connection. A newcomer with a complete list can finish joining
  after Supervisor fails; one without it waits.
- Removed Peer list reconciliation, Supervisor list-push/ACK machinery and an
  independent admission-ticket step from the target design. Kept business sync,
  connection validation, bounded dialing and production identity questions separate.
- Updated Core/full design, Supervisor/UI design, README and protocol status;
  preserved superseded decisions in codex.md and labeled current Discover as
  implemented behavior pending replacement. Documented ordered concurrent
  registration, durable/idempotent replies, partial-list failure and restart limits.
- Documentation only; no Rust/Proto source changes or dependency downloads.
  Checked changed documentation and new local references; no new runtime test
  claim, commit or push.

### 2026-09-09: Record the accepted discovery/Supervisor split and propose topology UI

- Recorded pairwise join/reconnect discovery, sequential normal joins, and an
  independent observer for alerts and bounded exceptional connection repair.
  Kept current periodic Discover explicitly labeled as existing implementation.
- Added `cluster/supervisor-design.md` covering expected-node coverage, stale and
  incomplete observations, two directed TCP connections per pair, shared graph
  data, repair limits, and a proposed browser 3D/2D/table interface.
- Distinguished connection health from future data convergence, and documented
  the recovery dependency when Supervisor is the only discovery repair fallback.
- Documentation only; no Rust/Proto changes, dependency downloads, Supervisor
  implementation or new runtime-test claim. Checked touched documentation for
  whitespace and newly added local link targets. No commit or push was created.

### 2026-09-09: Integrate Peer Protobuf, keepalive and bounded topology discovery

- Downloaded the explicitly approved official protoc 36.1 Windows/Linux ZIPs into each project's build area and verified their published SHA-256 digests. Added pinned Prost/prost-build 0.14.4 with project-local Cargo caches; no global environment or installation changed.
- Followed the maintainer's corrected wire layout: big-endian uint16 MessageID, uint32 payload length, then the concrete Protobuf payload. Removed CoreEnvelope and automatically generated stable IDs from an append-only `proto/message-ids.lock`; existing IDs survive additions, removals and reordering. Applied the maintainer's four-space Proto clang-format configuration.
- Implemented cancellable Hello, reusable bounded buffers, automatic Ping/Pong with exact request matching, absolute write/response deadlines, safe fragmented reads, timeout reconnect and complete task cleanup. Fixed the previous capped-backoff jitter and canceled-shutdown handle ownership defects.
- Added bounded DiscoverRequest/DiscoverResponse pagination, direct verification of learned IDs, reverse connections, short-lock topology ownership, duplicate-address/ID suppression and generation protection. Periodic jittered queries supplement stale lists without deleting live members; candidate caps, request budgets and a shared dial cadence bound discovery amplification.
- Passed all 31 tests on both Windows and the native Ubuntu VM, including three-node full mesh, late-node discovery, stale and duplicate lists, paging, flood limits, old sessions, shutdown cancellation and port release. Windows rustfmt/strict Clippy and both Release builds/help paths passed. No long-duration or 64-node capacity claim is made.
- Rechecked the analogous Go/Rust Selector retry and PONG paths, C++ shared retry, and C# native delegation. The Peer framing has no other-language implementation to patch, and no existing Redis SDK contract changed. Database SDK regressions were not rerun for this Rust Peer-only change.
- Updated Peer/protocol design documents and recorded exact scope in `cluster/protobuf-network-validation-20260909.md`. State replication, SDK Bind, production authentication and daemon process lifecycle remain future work. No commit or push was created.

### 2026-09-08: Complete unified regression/soak entries and owned-resource recovery

- Added `testkit/run.py` with regression and configurable soak modes, thin
  PowerShell/Bash entries, project-only caches/scratch/configuration, verified
  source delta synchronization, and persistent JSON/Markdown reports.
- Completed Python A22/A23 fixture ownership fixes, bounded logs/statistics,
  workload readiness before fault timing, timeout/cancellation cleanup,
  stale-run recovery and protection of foreign resources. The approved Python
  dependencies are in both project environments; Ubuntu's approved .NET 10
  SDK, .NET 8 runtime and NuGet references are project-local.
- Fixed the C++ Pub/Sub failed-result access that caused native abort dialogs;
  deliberately denied subscriptions now return unavailable/NOPERM and preserve
  root use. Reviewed Go/Rust equivalents and the C ABI/Legacy/C# shared core.
  Fixed Catalog post-promotion command readiness and Sentinel fixture timing.
- Final regression `c57fedac5de9e92f`: Windows 26/26 and Ubuntu 26/26 pass,
  including 23 framework checks and 16 CTest cases per platform, Go race on
  Ubuntu, .NET 8/10, cross-language peers and plain/TLS Sentinel.
- Final soak `e4df1e6a6b3db9f8`: 20/20 stages per platform; four Go workloads
  each passed a 210-second Redis-time floor and six fault injections, with
  zero residual keys. Windows measured zero durations are retained; update
  sample counts are complete. This does not qualify hours of endurance.
- Both campaigns used the same 424-file source fingerprint:
  `947bf7925e4a9a510988452098b56e8be3fd938f162cc2a8c4eae47ba2420632`.
  All 27 Python source files pass Black and Python 3.10 syntax checks.
- Actual fixture-creator death, PowerShell 5.1 launcher death and conflicting
  VM source edits have separate passing recovery/preservation checks. Final
  inventories found no owned containers, fixture/scratch directories, pending
  manifests or test processes; the audit Redis container was preserved.
  Ubuntu ended with 6,662 MiB available and zero swap use.
- Evidence: `testkit/results/unified-test-runner-20260908.json`,
  `test-runner-review-20260908.md`, and the ignored campaign logs. Earlier
  failures remain recorded. Dedicated Rust/C++/C# continuous workloads, mTLS,
  direct C++ two-promotion peers, packaging/AOT and precise per-finding release
  gates remain explicit; no commit or push was made.

### 2026-09-08: Unify native build policy with thin PowerShell/Bash entries

- Replaced the duplicated C++ entry implementations (1,042 PowerShell and
  859 Bash lines) with `build.py` (386), `build_support.py` (199), and thin
  PowerShell/Bash entries (50/21). The 656-line total includes comments and
  blanks and excludes the preexisting shared SDK runner; that runner grew
  from 40 to 45 lines for Unicode output handling.
- Kept existing profiles, linkage choices, cache dimensions, OpenSSL
  system/vcpkg/prebuilt-cache ordering, offline archive behavior and external
  tool/OpenSSL preparation. CMake still owns targets and dependency builds.
  Go/Rust retain native cache wrappers and C# retains dotnet.
- Selected the user's existing uv-managed Python 3.14.7 on Windows; Ubuntu
  uses its existing Python 3.14.4. Standard-library build/test code needs no
  Python packages. With explicit approval, installed Black 26.5.1 and its
  dependencies only in `build/tools/python-build`, using project pip/Black
  caches; pinned the formatter in `sdk/cpp/requirements-dev.txt`.
- Migrated the resolver regressions to 12 shared tests, including both
  platform policies, malformed CLI input, failed configure short-circuiting,
  child environment/cwd, argument forwarding and grandchild timeout cleanup.
  Both platforms passed; PowerShell 5.1/7 and Bash passed real adapter checks,
  including spaces, Unicode and trailing backslashes. Existing CMake early
  install guards and package-boundary fixtures passed on both platforms.
- The Unicode regression exposed the shared PowerShell stdout decoding
  problem. Fixed it once for Go/Rust/C++ callers and verified Go cache paths,
  Cargo offline metadata, and restoration of the original console encoding,
  environment and cwd on both successful and failed C++ entry calls.
- Windows/MSVC and Ubuntu/GCC each completed offline Debug shared configure,
  build and 16/16 CTest cases, including live Redis/C ABI/Legacy tests. Native
  builds used one job and the existing isolated Redis container. Black,
  Python 3.10 syntax parsing and `git diff --check` passed. Linux test logs
  were retrieved and all six changed build/test source hashes matched.
- Evidence: `testkit/results/native-build-unification-20260908.json` and
  `build/script-unification-20260908/`. This qualifies the tested Debug
  configurations; Release/check, real Ninja/Clang builds and the older
  deferred Python Redis/Sentinel fixture work are not claimed here.

### 2026-09-07: Route Go/Rust caches through project-owned command entry points

- Added `sdk/go/go.ps1`/`go.sh`, `sdk/rust/cargo.ps1`/`cargo.sh`, and a shared
  PowerShell subprocess launcher. Cache paths remain under the ignored root
  `build/`: Go modules in `deps/go/pkg/mod`, Go build/test cache in `cache/go`,
  Cargo dependencies in `deps/cargo`, and Rust output in `rust/target`.
- All settings are passed to the tool subprocess. The parent terminal's
  environment and working directory, persistent user/machine settings, existing
  toolchains, and shared caches remain unchanged. The final implementation has
  no environment-activation scripts. Existing C++ cache rules remain intact.
- Verified actual Go cache paths, offline `verdandi-refgen` tests, Cargo
  toolchain reuse, and offline/no-dependency Cargo metadata targeting the
  project directory. Windows PowerShell 5.1 and PowerShell 7.6.5 probes cover
  argument boundaries (spaces, empty values, quotes, and backslashes), stdout/
  stderr, nonzero exit status, and unchanged parent/user/machine settings.
  JSON output also works through PowerShell pipelines. PowerShell files retain
  UTF-8 BOMs for Windows PowerShell 5.1, matching the existing C++ entry point.
- Bash syntax and actual Go/Cargo invocations passed under Windows Git Bash,
  including nonzero status, rejected sourcing, and unchanged caller state.
  Native Ubuntu execution was not repeated for this cache-only change.
  `git check-ignore` confirms all cache locations are excluded. No dependency
  download, tool installation, commit, or push occurred.
- Plain `go`/`cargo` and IDE invocations bypass the project entry points and
  retain their normal cache behavior. Existing Python harnesses still invoke
  those plain commands; their migration remains deferred with Python changes.
  Entry points choose cache locations and do not themselves authorize downloads.

### 2026-09-07: Apply audited fixes and run available regressions

- Applied source changes for A01-A21 and A24-A25, and corrected the Rust
  Subscriber completion publication order described by R03. Cross-language
  propagation and pending validation are recorded for every finding. A22/A23
  Python source remains unchanged at the maintainer's instruction.
- The independent C++ Release project passes 7/7 CTest targets, including real
  SQLite query/fault handling, actual queue/pending/deadline implementations,
  public-template transaction rollback, the Legacy C ABI fixture, and CMake
  embedding/DLL-copy regressions. Actual catalog.cpp/selector.cpp and the C11
  C ABI Redis test compile; this does not qualify full native linking.
- Actual-source isolated Go Read, final-capacity and JSON boundary regressions
  pass; copied production/test files match repository SHA-256. The complete
  refgen package passes. Rust's actual error.rs UTF-8 regression passes as an
  independent rustc test. Complete Go/Rust builds remain dependency-blocked.
- C# library and test projects compile for net8.0 and net10.0 with zero warnings
  or errors, using installed targeting packs and an offline configuration with
  empty package sources. Native-dependent runtime tests remain pending.
- After the maintainer disabled Hyper-V Dynamic Memory and restarted Ubuntu,
  real Redis rejected the 65,537th Map field atomically, accepted the legal
  boundary/overwrite, and returned a readable full value. Final PING is PONG,
  DBSIZE is zero, only owned keys were cleaned, and the SSH tunnel was closed.
- Formatting and generated Catalog Lua freshness pass. No software or
  dependencies were downloaded in this repair work; no commit or push occurred.
- Relative to the audit snapshot, runtime source grows by 151 physical lines
  and tests by 974. Duplicate retry, guard and decode paths were simplified;
  no unsupported net code-size or performance improvement is claimed.

### 2026-09-06: Require externally prepared OpenSSL on Windows and Linux

- The native wrappers now try system packages, local vcpkg installed triplets,
  and extracted `build/deps/openssl/<platform>/x64` development packages.
  A vcpkg executable alone no longer passes OpenSSL diagnostics. Missing or
  incompatible candidates stop with package requirements and external-build
  guidance; the agent downloaded/installed no software or dependencies.
- Both SDK/probe CMake entry points disable manifest installation before
  `project()`, including attempts to pass `VCPKG_MANIFEST_INSTALL=ON`.
  Wrappers consume an existing install tree, with optional
  `VCPKG_INSTALLED_DIR`, and no longer configure per-build vcpkg installs,
  downloads, binary-cache restores, or app-local tool/deployment actions.
- System/vcpkg/cache probes now use the selected Debug/Release profile.
  Explicit package roots reject mixed-source headers/libraries. SQLite,
  yyjson, and Boost keep existing source compilation; their downloads still
  require specific prior approval. Build guidance and durable decisions match
  this OpenSSL-only scope.
- Fixed a related Bash failure path: when a probe function was called as an
  `if` condition, implicit `errexit` could be suppressed, allowing a failed
  configure to proceed to a successful build of an old tree. Explicit exit
  checks now prevent that false success.
- **Bug propagation review:** The vcpkg availability/implicit-install problem
  affected both PowerShell and Bash plus direct native CMake. C ABI, Legacy,
  and C# share that native configuration and inherit its guard, including the
  C# Python Sentinel helper's direct CMake call. PowerShell already checked
  configure exit status before building and was unaffected by the Bash
  false-success bug. Go uses its own modules/TLS path; Rust selects rustls in
  `Cargo.toml`; neither uses these native probes/vcpkg. Lua has no host build
  resolver. Python testkit process helpers check subprocess status or raise;
  no equivalent native configure/build probe was found. These are source
  reviews, not new language-runtime qualification results.
- **Verification:** PowerShell AST parsing, Git Bash `bash -n`, and
  `git diff --check` passed. The standard-library-only
  `sdk/cpp/tests/dependency_policy_test.py` passed 18 focused scenarios: six
  provider scenarios and one failed-configure regression per shell, two early
  CMake install guards, and two package-boundary scenarios. Test doubles do not
  represent actual OpenSSL binaries. Generated artifacts are under ignored
  `build/dependency-policy-test-20260906`.
- **Real-host evidence/limits:** Windows `build.ps1 doctor -Offline` passed
  the C++23 Debug compile/link probe, then exited 1 with the expected missing
  OpenSSL development-package guidance. No full SDK/Redis tests ran. Bash
  control-flow tests ran with installed Git Bash on Windows; no native Linux
  environment or complete OpenSSL/vcpkg development package was available.
  Black was unavailable and not installed; Python syntax and regression
  execution were checked with the existing bundled Python.

### 2026-09-05: Audit array bug propagation and fix Subscriber notification ordering

- Cross-language review found a second C++ ordering defect: the notification
  field decoder applied lexical order to Array Replace, rejecting valid
  numeric sequences at index 10 and triggering generation recovery. C ABI,
  Legacy, and C# inherit both native defects. The reviewed Go/Rust/Lua paths
  distinguish the required orders; Python test inputs preserve list order.
- Extracted the production MessagePack cursor and field reader into a private
  standard-library module. Array Replace now validates consecutive canonical
  numeric indices; Value/Map Replace and Array/Map Patch remain lexical.
- The original field reader fails eight checks in the new regression; the
  corrected reader passes strict MSVC Release and Debug /RTC1 runs. Both
  standalone CMake Release CTest targets pass. Newly available local
  clang-format 22.1.3 formats the eleven changed C++ files; no tool was installed.
- Added the maintainer's mandatory every-fix language propagation review to
  coding.md and codex.md. The detailed per-language review and testing gaps
  are in optimization-review-20260905.md; current source fingerprints are in
  testkit/results/catalog-array-propagation-20260905.json.
- These are source and production field-reader checks, not full SDK,
  Subscriber-envelope/recovery, Redis, binding, or Go/Rust runtime qualification.
  No dependency download, package restore, commit, or push occurred.

### 2026-09-05: Fix C++ Catalog array ordering and remove validation scratch allocation

- Reproduced the original C++ validator accepting ten Array entries but
  rejecting eleven and one hundred because it treated map lexical order as
  numeric index order. Publisher, Subscriber, and checkpoint validation share
  the affected helper; C ABI/Legacy/C# reach the same native core.
- Isolated pure Catalog validation and Replace argument encoding in
  `catalog_value.cpp`. Array completeness now follows canonical unique indices
  in `[0,N)`, while Replace writes final argument slots in numeric order.
  Successful value/Patch validation allocates no scratch vector, and Publisher
  iteration no longer repeats name-based map searches.
- Added direct shape, UTF-8, 4 MiB, 65,536-field, malformed-index, binary and
  ownership tests, plus a dependency-free CMake test project and optional
  microbenchmark. Extended the live native test to Array Replace/Patch and
  checkpoint recovery; it was compiled but not executed in this task.
- MSVC strict Release/Debug tests, the standalone CMake/CTest gate, affected
  Publisher/C ABI/integration translation-unit checks, and both Lua freshness
  checks pass. Seven alternating Windows benchmark pairs show successful
  validation allocations falling from one to zero; the 512-field Map median
  changes from 17.056 to 9.715 microseconds. These are local function measurements.
- No dependencies were downloaded. Complete SDK/link/binding/live/Linux
  qualification and clang-format/clang-tidy remain unexecuted because the
  required local tools/dependencies are absent. An attempted Subscriber
  translation-unit check stopped at missing `openssl/evp.h`.
- Review: `optimization-review-20260905.md`; raw benchmark evidence:
  `testkit/results/optimization-offline-20260905.json`. No commit or push.

### 2026-09-03: Re-optimize hot parsers and re-audit the complete Alpha tree

- Profiled the Go Catalog 512-field Replace decoder on WSL/Linux and removed
  its redundant post-decode name slice, sort, and validation traversal. The
  bounded decoder now validates structure, order, field contracts, capacity,
  and encoded bytes during the ownership copy while preserving structural
  error precedence.
- Added shared zero-conversion canonical unsigned-decimal primitives for
  strings and byte slices. Catalog revisions/config values, Registration
  records/configuration, and Redis fixed-width scalar decoding now use the
  same sign, leading-zero, overflow, and upper-bound rules.
- Ten-sample Linux `benchstat` comparisons reduced Catalog Replace decoding
  from 85.87 to 27.20 microseconds (-68.33%), 91,485 to 82,008 B/op, and eight
  to six allocations. Redis int64/uint64 decoding improved by 42.86%/45.11%,
  from 32 to 8 B/op and from two to one allocation. Registration stored-record
  parsing improved by 2.99% and removed two allocations.
- Added direct exact/+1 capacity, Value/Map/Patch shape, UTF-8/reserved-name,
  canonical integer, fixed-width overflow, and shared JSON configuration fuzz
  coverage. The configuration, Catalog, and Registration fuzz targets each
  passed a 15-second run.
- Fixed the Windows native doctor under Windows PowerShell 5.1: an expected
  system OpenSSL probe failure is now captured and judged by exit status, so
  `auto` can fall back to the existing vcpkg installation. PowerShell 5.1 and
  7.6 real doctor runs passed; system-only mode retained its expected detailed
  failure.
- Go format/module/vet/all-package/ten-shuffle/Linux-race and targeted 100-run
  boundaries passed. Rust stable and 1.85 MSRV tests passed. Linux/Windows C++
  Release, GCC ASan/UBSan/leak, clang-format, clang-tidy, C ABI/Legacy, C#
  net8/net10 Release/offline, Lua generator, Go generator, and Python syntax
  checks passed.
- The remote `192.168.0.90` was unreachable by SSH and ICMP. The Standalone
  harness exited before fixture creation, so this working tree has no new live
  Redis or endurance qualification and left no remote fixture.
- Detailed analysis is in `optimization-review-20260903.md`; the structured
  result is `testkit/results/optimization-regression-20260903.json`. No commit
  or push was performed.

### 2026-09-02: Add reproducible Windows and Linux native build entry points

- Added C++-owned PowerShell and Bash entry points with explicit `doctor`,
  `configure`, `build`, `test`, and `all` stages; dev/check/release profiles;
  static/shared linkage; system/auto/managed dependency policy; bounded
  parallelism; offline operation; and dry-run plans.
- Added exact existing-toolchain discovery and compile/link probes. Windows
  selects the generator matching the installed Visual Studio major and finds
  vcpkg through explicit settings, environment/PATH, bounded common locations,
  or Visual Studio. Linux selects native GCC/Clang and Ninja/Make and never
  consumes a Windows `vcpkg.exe` through WSL. Neither script installs tools.
- Kept OpenSSL 3.0+ system/vcpkg-owned. Added a pinned vcpkg manifest and
  checksum-locked Boost 1.92, SQLite 3.53.4, and yyjson source fallbacks with
  bounded downloads, verified shared archives, isolated extraction/object
  trees, and network-forbidden offline configuration.
- Standardized all script-owned help, selected-environment diagnostics,
  commands, warnings, errors, elapsed-time results, and completion summaries in
  detailed English. Windows requests English MSBuild output and Linux uses the
  C locale. Both script sources retain detailed Chinese parameter,
  function, lifecycle, and non-obvious-block comments for current review.
- Added `sdk/cpp/BUILD.md`, C++23/OpenSSL probe sources, deterministic output
  layout documentation, and an atomic non-secret `build/environment.json`
  diagnostic manifest. Shared builds print and record the exact DLL/SO path;
  C# compiles independently and loads that file from its environment, RID
  native directory, or application directory. Generated content is ignored at
  repository level.
- Passed PowerShell 5.1/current parser and execution, Bash syntax/help/dry-run,
  Windows and Linux native doctor probes, cold offline configuration, online/
  offline cache switching, invalid-vcpkg and missing-cache negative cases,
  and Windows/Linux Release C++/C ABI/Legacy tests. Each native run reported
  six passes and three endpoint-owned skips. After removing the unnecessary
  managed orchestration, the final Windows shared library separately passed
  the existing C# net8/net10 tests; native scripts neither detect nor invoke
  .NET.
- Recorded the machine-readable matrix in
  `testkit/results/native-build-entry-20260902.json`. No commit or push was
  performed.

### 2026-09-02: Add the generated Go Selector reference API

- Added `ReferenceSelector.WithOne/WithAny`, callback-scoped Candidates,
  Candidate/Selection handles, token-fenced Editors, and delayed read-only
  slice wrappers on top of the existing Selector operation gate, synchronized
  view, field-granular overlay, and remote reconciliation.
- Preserved `One`, `Any`, `Find`, and `Snapshot` as the detached safe surface.
  The new path builds no complete legacy Candidate slice, returns no detached
  result, and encodes only edited final selections. Unselected edits and every
  callback/context/foreign/duplicate/encoding/shape/limit failure roll back
  atomically.
- Added `cmd/verdandi-refgen`. It reads application Attr/Data structs and emits
  only strongly typed read accessors, setters, slice cloning, aliases, and a
  wrapper constructor. It generates no wire codec, Redis logic, or business
  policy, rejects field forms whose alias safety cannot be proven, and checks
  the committed compile fixture byte-for-byte with `-check`.
- Added unit coverage for selected-only commit, all rollback paths, stale and
  foreign values, token wrap, unavailable/closed/nil boundaries, panic cleanup,
  mutable slice ownership, atomic multi-selection failure, concurrent
  serialization, generator rejection/check mode, and generated public API
  compilation. Added a random-Zone live Redis 8.8 integration covering service
  discovery, local `Power++`, remote correction, and final zero owned keys.
- Passed `go generate ./...`, all Go tests shuffled ten times, `go vet ./...`,
  complete WSL/Linux `go test -race ./...`, and 100 shuffled targeted reference
  race repetitions. The isolated live reference integration passed on Redis
  8.8 without global flush or Catalog operations.
- Ten one-second WSL/Linux samples over 500 candidates measured ordinary versus
  reference `One` medians of 11.460 versus 10.178 microseconds with allocations
  reduced from 28 to four. Eight-of-500 `Any` medians were 13.875 versus 5.587
  microseconds, with the reference path at zero steady-state allocations. The
  reference operations intentionally do not construct detached return values.
- Updated API, SDK, coding, decision, README, and durable project documents. No
  commit or push was performed.

### 2026-09-02: Withdraw generic Campaign and Leader election

- Removed generic Campaign readiness, Leader election, distributed locking,
  ownership terms, election keys/actions/roles, and Sentinel fencing adapters
  from every project and release target, including `1.0.0`.
- Kept Redis Sentinel primary discovery and failover as supported transport
  recovery. It does not expose an application Leader API or an exclusivity
  guarantee.
- Kept Registration `@version` as application-defined metadata with no built-in
  election semantics.
- Updated the current architecture, protocol, release, SDK, decision, and work
  documents. Historical entries remain only as superseded design history.
- This was a documentation and comment correction. No runtime behavior was
  changed, no test campaign was required, and no commit or push was performed.

### 2026-09-01: Re-audit and regress the complete multilingual Alpha tree

- Reviewed the current Lua, Go, Rust, C++23, C ABI/Legacy, C#, configuration,
  and testkit production surfaces under their language-native ownership rules.
  Unchanged Go and Lua hot paths were retained where current measurement did
  not justify speculative rewrites.
- Made the C++ reactor runtime independently shared so last-reference release
  on the I/O thread cannot leave `io_context::run()` referring to a destroyed
  Driver implementation. Command connections that exceed the completion
  fallback are now removed from the pool before cancellation and can never be
  reused while a handler remains pending.
- Made Rust Catalog internal Mutex/RwLock poison recovery consistent for
  no-callback exception-safe critical sections, added a deliberate poison
  regression, and made checkpoint restoration publish complete view/byte state
  before its cursor.
- Removed avoidable C# Fields construction objects and copies, made raw Key
  writes borrow and pin their synchronous Span directly, and added a live empty-
  value regression. The independent harness now selects the exact host native
  runtime rather than inheriting an accidental library path.
- Made Windows C++ shared builds copy transitive runtime DLLs next to the target,
  so clean CTest and managed consumers do not depend on a preconfigured PATH.
- Passed final Go format/vet/unit/Linux-race gates; Rust format, strict Clippy,
  warning-denied rustdoc, current tests and Rust 1.85 check; C++ GCC/MSVC static/
  shared, C++11/14/17, format, clang-tidy and ASan/UBSan gates; and C# net8/net10
  format/analyzer/zero-warning Release gates.
- Passed the complete isolated Redis 8.8 Standalone matrix, C# independent
  Standalone matrix, direct C++ Windows/Linux TLS smokes, C# Windows/Linux TLS
  two-promotion matrices, Go/Rust Linux TLS two-promotion matrix, and Catalog
  two-promotion matrix. All structured results report `pass`; the Catalog run
  ended at revision 10 with zero keys.
- Recorded current Linux Go benchmark ranges and the detailed scores,
  strengths, deductions, and release boundary in
  `optimization-review-20260901.md`; the machine summary is
  `testkit/results/optimization-regression-20260901.json`.
- The changed working tree does not inherit the earlier frozen source's
  twelve-hour qualification. No commit or push was performed.

### 2026-09-01: Complete Windows and Linux Sentinel TLS qualification

- Extended the isolated Go/Rust, direct C++23, and C# Sentinel harnesses with
  explicit `win-x64`/`linux-x64` client-runtime selection while retaining the
  same remote Redis 8.8 fixture, private CA, fixed `verdandi.test` identity,
  separate Redis/Sentinel ACL users, and run-owned cleanup.
- Qualified native Windows x64 Go and Rust through the complete two-promotion
  matrix: wrong identity rejection, acknowledged-write-loss repair,
  `SCRIPT FLUSH`, total Sentinel loss, primary loss, recovery, UUID
  preservation, and Selector generations `1 -> 2 -> 3` all passed.
- Qualified WSL/Ubuntu 24.04 Linux x64 Go 1.27 and Rust 1.98 through the same
  complete matrix. Go was already present; with explicit maintainer approval,
  Rust was installed for the WSL user through the official minimal rustup
  profile. No system-wide package or Windows toolchain was installed.
- Qualified direct C++23 root/Registration/Selector/Catalog/checkpoint TLS
  integration on both MSVC shared Release and GCC shared Release. Both runtimes
  rejected the wrong certificate identity and ended at `DBSIZE=0`; the direct
  C++23 two-promotion campaign remains a separate open gate.
- Qualified C# net8.0/net10.0 self-contained peers on Windows x64 and Linux x64
  through the full two-promotion TLS matrix. The Windows run loaded the
  generated MSVC DLL plus its yyjson/OpenSSL runtime dependencies; both
  platforms ended at `DBSIZE=0`.
- Hardened cross-runtime orchestration by shell-quoting WSL environment values,
  mapping the CA path, isolating the Linux Rust target cache, and resolving the
  Windows yyjson DLL directory. The remote-Sentinel Go leak gate ignores only
  named standard-library DNS resolver frames left by canceled go-redis
  discovery; SDK-owned goroutines remain fully checked.
- Wrote six platform-qualified result files under `testkit/results/` and
  retained the three unsuffixed Linux files only as historical evidence.
  A final read-only host audit found no labeled containers or networks, no
  `verdandi-sentinel-it-*` directory, and no listener on the six fixture ports.
  Created no commit and performed no push.

### 2026-09-01: Qualify the existing Windows toolchain and exclude macOS

- Confirmed the existing machine provides Visual Studio Community 2026, MSVC
  19.51, Windows SDK 26100, CMake 4.4, VS Ninja/LLVM, Go 1.27, Rust 1.98,
  .NET 10, and vcpkg OpenSSL 3.6.0. No toolchain or dependency was installed.
- Added explicit MSVC UTF-8 compilation and a Windows 10 minimum so temporary
  Chinese source comments and Boost.Asio platform selection remain deterministic
  under `/W4 /WX /permissive-`.
- Fixed one production local-shadow warning, one compile-time unreachable path,
  one test shadow, and Windows-only CRT warnings without weakening production
  diagnostics.
- Built and tested x64 static Debug and shared Release. Both CTest matrices
  accepted 9/9 tests; the Release DLL exports `verdandi_c_abi_version` and
  `verdandi_c_has_capability`. Windows .NET 8 and .NET 10 directly loaded the
  generated DLL and passed offline configuration/capability tests.
- Reran Linux GCC Debug, shared Release, ASan/UBSan, and format gates after the
  portability changes. At this checkpoint live Windows Redis/Sentinel TLS and
  automated packaging remained open; the later cross-platform TLS entry above
  closes the former. macOS is explicitly unsupported and is not a release gate.
- Created no commit and performed no push.

### 2026-09-09: Start the Rust Peer network skeleton

- Added the independent `peer` crate with project-local Cargo wrappers and a locked Rust 1.85-compatible dependency graph. Tokio 1.53.1 and tokio-util 0.7.19 were resolved entirely from the existing project cache; no package was downloaded.
- Named the executable target `verdandi` while retaining the `verdandi-peer` package and `verdandi_peer` library. Built the Windows Release executable offline, verified its help output, and passed strict Clippy and all three TCP tests with the renamed target.
- Corrected the Peer Bash wrapper to enter its crate directory before forwarding Cargo arguments, verified Bash syntax and actual Cargo metadata under Git Bash, and confirmed its project-local cache paths. This is not a claim of a native Ubuntu build.
- Added `proto/peer.proto` for server-side CoreEnvelope, Hello and finite ProtocolError messages, with Chinese comments, ASCII punctuation and a Proto clang-format configuration. Formatting passed using the existing Visual Studio tool. Protoc and Prost were not available in PATH/project caches; descriptor compilation, generated Rust types and runtime migration remain pending.
- Recorded the Protobuf/FlatBuffers/Cap'n Proto tradeoffs in `proto/serialization-review-20260909.md`. Protobuf remains an engineering candidate without project-specific comparative benchmarks; no additional tools or libraries were downloaded.
- Implemented bounded TCP listening, fixed-seed outbound supervisors, dial and Hello deadlines, per-process boot IDs, monotonic connection generations, per-Peer jittered exponential reconnect, global dial admission, bounded inbound sessions, diagnostic events, and joined shutdown.
- Added a 4-byte-length-prefixed, 512-byte-bounded internal Hello that rejects protocol-major, cluster, peer-ID, self-connection, advertise, and frame-limit violations before a connection becomes verified. This bootstrap encoding is explicitly not the frozen Protobuf protocol.
- Expanded the Rust module, API, field, and logical-block documentation in detailed Chinese with ASCII punctuation. The comments now explain ownership moves, `Arc`, child cancellation, `JoinSet`, semaphore permits, `select!`, nested `Result`, checked decoding, retry arithmetic, and the test flow; recorded the punctuation rule in `coding.md`.
- Passed offline all-target compilation, strict Clippy with warnings denied, three real-localhost network tests covering connection, cluster rejection, delayed seed recovery and listener release, plus the executable help-path smoke check.
- Automatic discovery, mirrored-session registry replacement, heartbeat, Protobuf, StateStore, OwnerPush, ReplicaRepair and SDK behavior remain outside this skeleton.
- Created no commit and performed no push.

### 2026-09-01: Implement fixed-identity Sentinel TLS and C ABI capability discovery

- Replaced the C++ Sentinel+TLS rejection with a fixed certificate-identity
  contract shared by Go, Rust, C++23, C ABI, Legacy, and C#. TLS-enabled
  Sentinel requires non-empty `server_name`, and every Sentinel/data-node
  certificate must contain that same identity.
- Kept full certificate-chain, validity, handshake-signature, and identity
  verification. C++ reapplies DNS SNI from `SSL_CTX` at every OpenSSL handshake
  so Boost.Redis discovery/reconnect streams inherit it. Rust delegates normal
  validation to WebPKI with the configured identity and disables Fred's
  address-derived Sentinel SNI; SNI virtual-host routing is outside that path.
- Added optional private-CA TLS mode to the isolated Sentinel fixture. Its leaf
  SAN contains only `verdandi.test`, never the announced IP; Sentinel, Redis,
  and replication links all use TLS.
- Go and Rust rejected a deliberately wrong identity and passed two promotions,
  acknowledged-write-loss repair, `SCRIPT FLUSH`, total Sentinel loss and
  recovery with UUID preservation and Selector generations `1 -> 2 -> 3`.
- C++ shared Release rejected the wrong identity and passed Root,
  Registration, Selector, Catalog and checkpoint integration. C# net8.0 and
  net10.0 then passed the full two-promotion TLS matrix through the same core;
  every fixture cleaned its keys, containers, directories and ports.
- Added `verdandi_c_has_capability` plus C++11 Legacy `has_capability` and C#
  `Runtime.Supports`. Seven string capabilities are currently published;
  known, unknown, empty and invalid inputs are covered. The shared library now
  exports 90 `verdandi_*` symbols.
- Updated the schema, shared conformance corpus, API/configuration docs, review,
  and machine results. Automated native/NuGet RID packaging is explicitly
  deferred; no commit or push was performed.

### 2026-09-01: Normalize and harden every configuration boundary

- Reduced root Redis reconnect to one portable fixed delay while preserving
  independent Selector/Catalog business-recovery backoff and the no-command-
  retry rule.
- Normalized endpoint, Unicode, strict JSON shape, required-field, numeric,
  TLS, bounded-file and path semantics across Go, Rust and C++23.
- Kept each native API idiomatic: Go topology/TLS structs, Rust Duration/PathBuf
  with isolated Fred URL mapping, and direct C++23 chrono/filesystem values.
- Added a C ABI offline configuration validator and used it from C# after
  strict UTF-16 and 1-MiB preflight, avoiding a second managed DTO/validator.
- Expanded the shared configuration corpus to 41 semantic and six raw cases.
- Passed Go test/vet/race, Rust current/MSRV tests and strict Clippy, C++ static/
  shared/sanitizer tests plus format/tidy, and C# net8/net10 build and Linux
  offline execution. External Redis tests were intentionally not run in this
  focused pass and remain recorded as skipped, not passed.
- Recorded the full audit, scores and limitations in
  `configuration-review-20260901.md` and the structured evidence in
  `testkit/results/configuration-normalization-20260901.json`.
- Created no commit and performed no push.

### 2026-09-01: Prepare the bounded 0.1.0 Alpha release identity

- Adopted `0.1.0` as the first non-production Alpha line for distributed SDK
  development and controlled service integration. This does not claim stable
  API, ABI, wire compatibility, availability, or production readiness.
- At that checkpoint, stable `1.0.0` was reserved for qualified Leader election,
  standard English production-source comments, and the complete acceptance
  matrix. The Leader portion was superseded by the 2026-09-02 scope decision;
  the comment and remaining acceptance gates still apply.
- Migrated active repository URLs, Go import/module paths, schema identity, and
  language package metadata to `github.com/eosforge/verdandi`; set Rust, CMake,
  C#, and local Go peer dependencies to `0.1.0`.
- Added `release-0.1.0.md` and retained the exact final Registration/Selector
  and Catalog twelve-hour result JSON files. Both frozen campaigns passed more
  than 43,200 Redis seconds, every planned fault and post-check, 1,441
  monotonic samples without a sampling failure, and final owned-key cleanup.
- Reverified the metadata-only preparation with Go module verification,
  formatting, vet, SDK and peer tests; Rust formatting, locked offline tests,
  and strict Clippy; generated Lua checks; C++23/C ABI/C++11/14/17 offline
  tests; C# .NET 8/10 formatting, zero-warning Release builds, and offline
  tests; JSON parsing; repository-reference checks; and `git diff --check`.
- Authorized this preparation only for the public `alpha` source branch. It
  deliberately creates no release tag, GitHub Release, or language-package
  publication.

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
  The 2026-09-01 fixed-identity TLS entry supersedes only the TLS gate.
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

### 2026-08-24: Decouple Campaign and election Version from Registration (superseded)

- Superseded on 2026-09-02 when generic Campaign/Leader election was withdrawn
  from every release target. The following bullets are historical only.

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

### 2026-08-24: Freeze SDK-driven strict Leader policy (superseded)

- Superseded on 2026-09-02 when generic Campaign/Leader election was withdrawn
  from every release target. The following bullets are historical only.

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
  checkpoint and is superseded by the 2026-08-31 entry; Leader was outside this
  completed scope and was withdrawn on 2026-09-02.
- Created no commit and performed no push.
