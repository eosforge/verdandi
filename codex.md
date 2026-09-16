# Astra Project Memory

## Tests require explicit approval (2026-09-16)

After implementation and cleanup, report changes and proposed test scope, then wait for explicit user approval before running tests or their prerequisite builds.
This applies on Windows and Linux, including unit/regression, sanitizer, benchmark and endurance runs. Writing tests and formatting source remain allowed.
A current explicit request to run tests is approval for its scope; earlier rounds are not standing authorization. See AGENTS.md.
The Wheel test run already in progress had completed when this instruction arrived; no further tests were launched.

## C++ protocol names are explicit

Use full generated protocol names in handwritten C++, including tests and isolated probes.
No wire/orbit/probe namespace aliases or using namespace directives for protocol packages.
Use proto::astra::v1, proto::orbit::v1, proto::comet::v1 and proto::astra::bench::v1 as applicable.
This is a source-readability change; schemas and generated code retain their existing definitions.

## Current v1 regression recheck (2026-09-15)

Debug, Release, ASan/UBSan and TSan each passed 10 CTests, 6 RPC cases and 13 process cases on Linux.
Both Supervisor/generator checks and Linux Go race passed; Windows Python 44 and Admin 53 tests plus build passed.
338 relevant files match across hosts; 704 frozen files and 3420 installed dependency files remain unchanged.
No production source fix, downloads, unbounded tests, commit or push. Test services are stopped.
See [current recheck evidence](testkit/results/protocol-v1-recheck-20260915.md).

## Current protocol ownership: v1

The latest maintainer decision supersedes all v6 and astra::cluster naming below.
Handwritten C++ uses astra and <astra/...>. Schemas are proto/orbit.proto (Supervisor),
proto/astra.proto (Star/Planet), and proto/comet.proto (future SDK, no messages or RPC yet).
Generated C++ namespaces are proto::orbit::v1, proto::astra::v1, proto::comet::v1.
Only protocol major 1 is accepted; signing domain is proto.orbit.v1.admission + NUL.
Member and Role are defined only in Orbit. No compatibility aliases or old-version handlers.
The old SDK remains frozen. See [current v1 contract](protocol-v1.md).

## Astra migration: current working paths (2026-09-15)

The active C++ service source is now `astra/`, with `astra::cluster` headers and namespace.
Use `bash astra/build.sh ...`; binaries and dependency entry are `build/astra` and `build/deps/astra`.
Go Supervisor, Admin and generated protocol use Astra; gRPC package is `astra.cluster.v1`, admission domain is
`astra-admission-v6` + NUL. All participating services must upgrade together and re-register.
The outer Windows/Linux directory and Git remote remain verdandi; Go module/import/go_package retain that repository path.
Legacy SDKs, Redis contracts, retired Rust service/transport experiments and historical evidence are frozen.
Linux current tests run at `/home/ubuntu/verdandi`, reusing migrated build trees and existing dependency installations.
Details and current validation: [Astra migration](astra-migration.md). Earlier entries below are historical;
paths beginning cluster-cpp now refer to astra. Store changes below were committed as `467dea7`.

## Store review after aadcbb4 (2026-09-15)

Reviewed Store implementation/tests line by line after the maintainer's upper_bound/reserve proposal.
Use Ranges projection/subrange, checked reserve and measured iterator insert; prepare put records before locking.
Entry derives deletion from null payload but keeps its version for allocation-cache reuse. Immediate node
erasure and exact snapshot pre-counting were rejected after benchmarks. Fixed the never-expire max sentinel.
Detailed Chinese comments now cover fields, locals, function contracts and blocks, with ASCII punctuation;
keep function signatures and opening braces together. Bounded Store tests passed in Debug/Release/ASan+UBSan/TSan
with 76 write and 7 read allocation failures. No fresh coverage percentage or network/SDK regression is claimed.
Review and reproducible benchmark: [Store performance review](cluster-cpp/sync-store-performance-review-20260915.md).
These changes remain uncommitted; no downloads or persistent background test.

## Sync foundation cleanup: validated (2026-09-15)

The maintainer requested fixes after WIP commit `2bf279f`, then explicitly required finishing
cleanup without compilation or tests and waiting for further notice. The maintainer has now
explicitly resumed complete validation. Stop any leftover endurance campaign, then run bounded tests only.
Store cleanup, dedicated fault-injection test sources and real C++/Go protocol generation are prepared.
SyncTransport remains an unregistered draft; runtime cursor checks and business synchronization are not implemented.
Fresh validation now passed on the cleanup: Debug/Release/ASan+UBSan/TSan each passed 10 CTests,
6 real RPC cases and 13 process cases. Both Supervisor platform checks, Linux race and bounded fuzz,
generator checks, 38 Python harness tests and 6 build-entry tests passed. The allocation sweep triggered
76 failures. A bounded 4-Star/2-Planet run completed 9 fault cycles and cleaned all owned resources.
198 selected source hashes match the VM copy. No indefinite test is running. No download, installation,
commit or push was performed. Review: [sync foundation](cluster-cpp/sync-foundation-review-20260914.md).

## Current service decision (2026-09-12)

C++ Star/Planet and Go Supervisor are the only active service implementations. The old Rust service is retired; its reports are historical. Shared code uses `cluster`, own identity fields use `id`, and Supervisor issues signed opaque string identities. Current protocol is v6; the [identity contract](cluster/identity-contract.md) supersedes older UUID/client-generation and Rust-comparison descriptions below. Accepted code-review corrections must be implemented, not left as documentation-only proposals. Rust SDKs and the standalone protocol generator remain separate.

## C++ skeleton cleanup (2026-09-14)

The maintainer requested a minimal, reviewable cluster-cpp connection foundation before business work.
Principal is a concrete 32-byte deployment digest; HexId and the DialTarget wrapper are removed.
Policy::due returns one optional Member, matching Runtime's single-dial pacing. Admission owns one
concrete Register call; completed calls are consumed on both success and failure. Closing is derived
from its first local reason and upstream presence from the owned member snapshot, without duplicate flags.
The retired benchmark command and --benchmark-smoke option are removed; isolated experiments remain separate.
Protocol v6, Supervisor behavior, existing Star/Planet connection semantics and Chinese ownership comments remain.
Review entry: [C++ cleanup](cluster-cpp/minimal-review-20260914.md). Planet business development remains paused.
Verified Windows formatting/build-entry tests, Linux Debug/Release/ASan/UBSan/TSan regressions and
a 61.507-second, nine-cycle fault loop with four Stars and two Planets. The 264 selected validation inputs
match the VM; owned resources were cleaned. This does not restart the previously stopped endurance campaign.

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
Implemented and verified: Windows Go/Proto gates, Linux Go race and C++ Debug/Release/ASan/UBSan/TSan
each with 8 CTest suites, 6 RPC scenarios and 13 process scenarios. All passed; owned test resources
were cleaned. See [admission verification](cluster-cpp/admission-simplification-20260912.md).

## 0. How to Use This File

This is the canonical onboarding document for maintainers and new AI-assisted
development sessions. Read it completely before modifying the repository. It
contains the stable project identity, accepted architectural constraints,
repository rules, and durable decisions needed to continue work without the
original conversation.

This file is intentionally self-contained. Use the following documents for
detail after reading it:

- [`alpha.md`](alpha.md) owns version `1.0.0` requirements and acceptance
  criteria.
- [`architecture.md`](architecture.md) owns system roles, topology, state
  ownership, failure behavior, and integration boundaries.
- [`protocol.md`](protocol.md) owns the draft wire/storage protocol, revision
  rules, Redis key ownership, and compatibility contract.
- [`coding.md`](coding.md) owns coding, API, documentation, and comment rules
  for every language implementation.
- [`decisions.md`](decisions.md) contains review-ready recommendations for
  unresolved foundation and protocol choices. Its proposals are non-normative
  until the maintainer explicitly accepts them into an owning contract.
- [`worklog.md`](worklog.md) owns current repository state, active work,
  planned work, blockers, and completed work.
- [`README.md`](README.md) is the public project introduction.
- [`freeze-20260831.md`](freeze-20260831.md) records the first complete Alpha
  source freeze, its regression evidence, and its remaining release gates.
- [`release-0.1.0.md`](release-0.1.0.md) owns the non-production `0.1.0`
  release scope, compatibility statement, and exact freeze evidence.

When documents disagree, stop implementation and reconcile them explicitly.
Stable decisions in this file take precedence over stale worklog text. The
maintainer's latest explicit instruction takes precedence over every document.

## 1. Project Identity

Verdandi is a reusable, language-neutral distributed coordination layer. It is
not a Bifrost submodule and does not belong to Hermes. It lives in its own
public GitHub repository:

```text
Local:  D:\projects\verdandi
Remote: git@github.com:eosforge/verdandi.git
```

Its responsibilities are deliberately narrow:

- discover independently identified service-process Registrations;
- synchronize leased observed state and coarse load information;
- synchronize persistent revisioned Catalog key/value namespaces;
- distribute versioned desired-configuration documents;
- record validation, activation, rejection, and expiry results;
- recover deterministic local state after missed notifications, reconnects,
  and Redis primary failover; and
- provide conformant SDKs without embedding application business semantics.

The initial active-state backend is Redis. The project name and language-
neutral domain APIs do not encode Redis because backend choice is an
implementation constraint, not the product identity. Go's root-only borrowed
`Redis()` capability is an explicit language integration escape hatch, not a
Verdandi domain or wire contract.

## 2. Stable Scope and Boundaries

Verdandi owns coordination mechanics, not consuming-application semantics.
Payloads such as Bifrost route tables are opaque ACL-authorized bytes to
Verdandi. Their schemas, validation, precedence rules, and business meaning
remain in the consuming application's own contracts.

Verdandi must not:

- import Bifrost, Hermes, or another consumer's schema or implementation;
- implement application routing, user authentication, or business data;
- select one universal load-balancing algorithm for every consumer;
- encode a fixed maximum number of services, Nodes, or Catalog Values into the
  protocol;
- perform Redis access on a request or connection selection hot path;
- present Pub/Sub delivery as durable or authoritative;
- claim consensus, exactly-once execution, or lossless asynchronous failover;
- expose a Redis-driver type through a language-neutral or domain SDK API;
  Go's reviewed root-only borrowed `*redis.Client` capability is the explicit
  exception and remains outside Verdandi protocol guarantees; or
- allow unbounded payloads, snapshots, queues, tasks, retries, or decoders.

## 3. Protocol Roles

### 3.1 Node

A Node represents one running service process. On every start the SDK creates
a fresh Registration UUID. The Node writes only its own Registration through
the aligned `register`, `update`, `renew`, and `unregister` lifecycle, plus its
load samples and acknowledgements. Renew changes only Redis timestamp and lease
expiry; it does not change the content revision.
Before Registration initialization completes, its Client fills missing
defaults, reads, and validates the Zone's shared Registration limits from
Redis. The last valid snapshot refreshes periodically or explicitly. The Node reads authorized desired
state and wake channels, validates application payloads through consumer-owned
callbacks, and republishes its complete state after reconnect or detected Redis
state loss. Each published Registration's sole worker keeps that UUID's bounded
desired-state cache and confirmation status only in process memory for this
purpose. It never persists a Registration
UUID, replay log, or Registration content to local disk. Process termination
discards that cache; a restart receives a new UUID and never restores the
previous process identity.

### 3.2 Publisher

A Publisher normally belongs to a controller or control service. It owns
desired state, Catalog mutations, configuration manifests, target revisions,
and convergence tracking. A wake notification is never treated
as proof that a target applied a publication.

### 3.3 Selector

A Selector subscribes before a paginated authoritative Registry load, buffers
at most one logical change per Registration, fences the completed scan with an
ordered PING/PONG on the subscribed connection, and atomically exposes an
immutable local view. It tracks per-Registration revisions and leases through
a connection-generation RedisClock. Filtering, weights,
locality, least-load policy, and optimistic reservation logic belong to the
consuming application.

### 3.4 Administrator

An Administrator provisions Zones, identities, Redis and
Sentinel credentials, ACL policy, and the non-expiring Zone capacity Hash.
Administrative
credentials must not be available to ordinary Nodes, Publishers, Selectors,
or Catalog Subscribers.

## 4. Communication and Deployment Model

Runtime components connect to Redis directly. A Controller using Publisher
does not maintain a dedicated control connection to every Proxy, Dispatcher,
or other Node.

```text
Administrator -> Publisher
                     |
                     v
             Redis current primary
               ^       ^       ^
               |       |       |
             Node    Node   Selector

Sentinel clients resolve the current Redis primary and then connect to Redis.
Sentinel is not an application proxy.
```

The same protocol supports:

- **Standalone:** one configured Redis primary, suitable for development and
  deployments that explicitly accept a single point of failure.
- **Sentinel:** a Sentinel-resolved Redis primary with replicas and separately
  configurable Redis and Sentinel authentication.

Redis Cluster is not part of version `1.0.0`. The required model depends on
small atomic pointer changes and predictable key ownership; future large
deployments scale through explicit application partitions or cells backed by
independent Standalone or Sentinel deployments.

## 5. Consistency Contract

Verdandi provides recoverable convergence, not general distributed consensus:

- One current Redis primary serializes each accepted atomic operation.
- Redis Pub/Sub is the Registry and Catalog normal at-most-once, non-durable
  incremental event path. A validated Catalog operation may update a local
  Entry; loss, reconnect, or a nonmatching Patch base invokes authoritative
  Hash/ZSET/Read recovery.
- Durable current keys, paginated indexes, buffered Pub/Sub events, immutable chunks,
  manifests, current pointers, Registration records, Catalog Hashes plus
  revision indexes, and acknowledgements are the recovery source.
- A subscriber subscribes before its initial authoritative read.
- Shared Publisher-owned publications carry one Redis-owned monotonic revision
  per target. Registration instead carries one SDK-owned per-UUID revision that
  survives Redis reconnect for the same process.
- A reconnect, revision gap, malformed
  notification, missing document, or incompatible schema forces an
  authoritative reload.
- Sentinel promotion can lose a recently acknowledged write because Redis
  replication is asynchronous. Nodes republish their leased Registrations.
  Publisher state has no second core-SDK durable source, so protocol `1.0`
  explicitly does not promise recovery of every lost acknowledged latest write.
- Existing application traffic must not depend on Redis remaining available.
  New selections and new activations obey explicit lease and stale-state
  policy.

Redis persistence is the protocol `1.0` source for current Publisher state. A
future external statistics/audit synchronizer may persist history, but the core
SDK does not aggregate metrics, manage history, or own an audit database.
Registration recovery is intentionally different from durable Publisher
history: only a still-running Node may use its volatile in-process cache to
republish the same UUID after Redis reconnect or failover. Once that process
ends, TTL cleanup owns the old Registration and a new process starts with a new
UUID.

## 6. Data Classes

Topology, observed load, Catalog KV, desired configuration, acknowledgements,
and commands remain distinct because they have different
owners, cadences, and failure semantics.

- **Zone Configuration:** one non-expiring Redis Hash at
  `verdandi:config:<zone>` containing the shared Registration limits. A Client
  fills missing common defaults at bootstrap, retains the last complete valid
  value set locally, and adopts valid administrative changes through periodic
  or explicit refresh.
- **Registration:** one Redis Hash at
  `verdandi:registration:<zone>:<type>:<uuid>`, with key TTL and public `Meta`,
  `Attr`, and `Data` views. Redis/Lua-managed Meta leaves use `@name`, immutable
  Attr leaves use `.name`, and mutable fixed-structure Data leaves are
  unprefixed. Every top-level leaf is stored independently so typed steady-state
  mutations update only selected Data fields.
- **Registry:** the Zone/Type collection of leased Registrations, represented
  by `verdandi:registry:<zone>:<type>` as both a paginated membership Hash with
  field TTL and a Pub/Sub channel. It has no global revision or mutation
  history. Selectors combine a subscribed paginated load, per-Registration
  content revisions, four aligned lifecycle events, and an ordered PING/PONG
  fence before publishing a synchronized local view.
- **Catalog Value:** one Publisher-owned raw Value, contiguous Array, or Map at
  `verdandi:catalog:<zone>:<part>:<id>`. Replace/Delete are Redis-order
  last-write-wins; Patch requires the exact current base. Live Hashes plus
  global/field revisions and live/deleted ZSETs are authoritative. Lua
  publishes the complete accepted operation; Subscribers repair any loss and
  expose synchronization health explicitly. Catalog has no TTL.
- **Observed Load:** bounded, debounced Registration field updates. A Node does
  not publish for every connection event. Selectors may maintain local pending
  reservations until Registration patches reconcile them.
- **Desired Configuration:** ACL-authorized, versioned, time-bounded, targeted opaque
  snapshots using the same chunks, manifest, and current-pointer pattern.
- **Acknowledgement:** a Node-owned record stating received, validated, active,
  rejected, expired, or drained state with a stable error code and bounded
  diagnostic.
- **Command:** deferred from SDK `1.0.0`; a future bounded imperative request is
  considered only when desired-state reconciliation cannot express a concrete
  approved operation.

Desired configuration uses complete snapshots in the first release. Registry
uses per-Registration record mutations; Catalog uses globally ordered
Replace/Patch/Delete over independently typed Path values.

## 7. Identity and Fencing

The protocol distinguishes:

- `uuid`: a fresh UUID generated by the SDK for every process start;
- `zone`: SDK-configured administrative/deployment isolation;
- `type`: the logical Registration class and Registry;
- Registration `revision`: an SDK-owned positive per-UUID content version that
  advances only when Data changes and is validated/stored by Lua; and
- shared `revision`: a Redis-owned monotonic value within a Catalog, desired
  target, or ACK scope.

A process may update, acknowledge, or delete only the key containing its exact
Registration UUID. A restart receives another key and never overwrites the
previous process. Publisher and Catalog updates carry no write-authority term.
Each accepted shared-state mutation advances its scope revision. Registration
`update` advances its owner's content revision, while `renew` retains revision
and advances only Redis timestamp. Reconnect or primary change forces
authoritative Selector reload while a live Node republishes complete `register`
state with the same process UUID and desired content revision.

## 8. Language and Package Model

Verdandi is protocol-first and may support any implementation language. Go and
Rust are the first required SDKs for `1.0.0`; they do not define the permanent
repository boundary.

The intended layout is:

```text
spec/                 public language-neutral protocol
schema/               language-neutral Redis field and artifact definitions
lua/                  protocol-owned Redis atomic operations
sdk/go/               Go module and implementation
sdk/rust/             Rust crate or workspace and implementation
sdk/<future>/         additional conformant SDKs
testkit/              vectors, conformance, and failover harnesses
```

Rules:

- The repository root is language-neutral. Do not place `go.mod`,
  `Cargo.toml`, .NET solution/toolchain pins, language toolchain pins, or
  IDE-specific project configuration at the root.
- Each SDK owns its dependencies, tool configuration, semantic version, and
  publication workflow under `sdk/<language>`.
- Protocol compatibility is versioned independently from SDK package versions.
- Native protocol SDKs implement independently and may not import one another.
  A language binding may instead target the reviewed C ABI when that choice is
  explicit, exposes idiomatic language ownership/API, and owns no second
  protocol state machine. C# is the first accepted instance.
- Shared artifacts are schemas, Lua programs, error codes, and conformance
  vectors, not cross-language implementation code.
- Adding a language requires an explicit SDK contract, standard formatting and
  test commands, packaging rules, and cross-language conformance evidence.

## 9. Security and Resource Rules

Production deployments require private network placement, TLS when Redis or
Sentinel traffic crosses host boundaries, separate least-privilege Redis and
Sentinel credentials, target and validity checks, Registration UUID fencing,
same-connection revision rollback protection, and Redis ACLs for every role. Redis ACLs are the
write-permission boundary; protocol-owned SDK/Lua actions are the supported
consistency boundary. A principal granted raw write access can violate stored
invariants and is outside Verdandi's guarantee.

The application supplies a case-sensitive Zone of 1 through 32 ASCII letters
through SDK configuration. The SDK generates each Registration UUID as
exactly 32 lowercase hexadecimal characters. ACL credentials are scoped by
role and Zone, not by individual UUID.

Pub/Sub messages contain no secrets and are never authoritative. Diagnostics
must not contain credentials, complete configuration, private
origins, or authorization material.

Every deployment configures explicit limits for record bytes, Catalog Patch and
complete-Value bytes, document bytes,
chunk bytes, entries and bytes per page, local snapshot memory, concurrent
fetches, queued events, retry concurrency, synchronization duration, PONG and
Catalog recovery timeouts, acknowledgement retention, and diagnostic volume.
Shared Registration field/count/byte limits live in
`verdandi:config:<zone>`; connectivity, timing, concurrency, and local-memory
budgets remain local SDK configuration. Exceeding a resource limit returns a
stable error and preserves the last valid state only within its lease policy.
These byte, page, and concurrency bounds do not create a protocol-wide
service-count limit.

There is no product-wide fixed Node limit. Qualification uses separate profiles
of 500 live Proxy Registrations, each renewing or updating once per second, plus
the earlier raw-core fault run covering 3,750,000 Updates across 7,263.649
Redis-server seconds and the direct typed run covering 4,000,000 Updates across
7,608.409 Redis-server seconds with eight transactional Selectors. Catalog
targets 10 mutations per second and recovery of the same population. This is
test evidence, not a permanent maximum.

## 10. Verification Contract

Mocks alone are insufficient. Protocol acceptance requires real Redis and
Sentinel integration tests covering TTL, Lua, paginated Registry loads,
subscribe/scan/PING reconciliation, Catalog Hash/ZSET/Read alignment,
tombstones, floors, checkpoints and strict-Patch/LWW conflicts, Pub/Sub loss,
reconnect, replication, failover, script-cache loss, acknowledged-write loss,
and self-healing republish.

All SDKs must pass the same corpus for Redis field maps/scalars, hashes,
revision transitions, per-start Registration identity, manifest validation,
lease calculations, stable errors, idempotency, and Lua results. Cross-language
tests must prove that each supported Publisher, Node, and Selector can consume
records produced by another supported SDK.

Capacity results must state hardware, Redis configuration, topology, payload
sizes, cadence, subscriber and connection counts, CPU, memory, allocation,
latency, failover duration, and recovery duration. Unsupported performance or
fleet-size claims are prohibited.

## 11. Bifrost Integration Boundary

Bifrost is an initial consumer, not Verdandi's owner. A typical mapping is:

- Bifrost Controller uses Publisher for desired configuration and Catalog KV.
- Bifrost Proxy uses Node for lifecycle, Registration, configuration, and ACK.
- Bifrost Dispatcher uses Node for its own lifecycle and Selector for eligible
  Proxy views.

Bifrost route snapshots remain in Bifrost contracts. Verdandi transports their
complete opaque bytes and metadata but never parses hosts, paths, origins,
listener policy, TLS policy, or routing precedence. Hermes may adopt Verdandi
later through the same public protocol without creating a Bifrost dependency.

## 12. Repository and Git Rules

- `main` is the public release branch.
- Among Markdown files, `main` currently permits only `README.md`. Internal
  Markdown documents such as `codex.md`, `alpha.md`, `architecture.md`,
  `protocol.md`, `coding.md`, `decisions.md`, and `worklog.md` live only on
  local `alpha` until the maintainer explicitly changes the publication
  policy.
- `LICENSE` is the public non-Markdown MIT license artifact.
- `alpha` is private-by-default working history and must not be pushed without
  an explicit current-task maintainer instruction. The 2026-08-31 first source
  freeze is the one explicitly authorized exception.
- A normal merge from `alpha` would leak internal Markdown into `main`.
  Release work must start from `main` and select only explicitly approved
  files or commits.
- Public protocol documentation will eventually require a maintainer decision:
  expand the Markdown allowlist, publish generated documentation, or place the
  necessary public contract in `README.md` and schemas. Do not silently weaken
  the current branch rule.
- AI-assisted work may edit and verify files, but it must not create, amend, or
  sign a Git commit unless the maintainer explicitly requests a commit in the
  current task.
- Never push any branch or tag without an explicit instruction. An explicit
  instruction to push `main` never authorizes pushing `alpha`.
- Inspect `git status` before and after work. Preserve all existing user
  changes and never discard, reset, or overwrite them without permission.
- Do not track `.idea`, local credentials, recovery codes, deployment private
  keys, environment files, build output, or editor state. A deliberately public
  self-signed test key may be tracked only beside a README that states it
  protects no deployed identity or data.

Because Git hooks and local configuration are not cloned, every new working
copy must recreate the local `alpha` push guard before using that branch. An
authorized push temporarily bypasses that guard only for the exact reviewed
commit and does not become standing permission for later pushes.

## 13. Current Repository Store::Snapshot

Historical v5 identity implementation (superseded by single-RPC v6 admission), 2026-09-12: Supervisor owns id issuance and generation format. All active own-identity fields use `id`, owned as `std::string` in C++. The signed startup ticket fixes the id, deployment binding and first CAS baseline; retries reuse it. Only committed registrations receive the separate v5 Hello admission signature. There is no client UUID generator or UUID format parser. Star/Planet are the only roles; shared code is `cluster-cpp/` / `verdandi::cluster`. The retired Rust service is outside current migration and checks. See [identity contract](cluster/identity-contract.md).

Source documentation convention, 2026-09-12: owned handwritten source headers
describe current functionality instead of repeating the repository license.
Configuration fields document meaning, units, defaults, bounds and interactions;
every enum element has its own comment, including private states and test scenarios.
Function declarations document caller contracts and implementation blocks explain
non-obvious transitions and ownership. The C++ application of this rule is recorded
in [cluster-cpp/CONTRIBUTING.md](cluster-cpp/CONTRIBUTING.md#文件职责与注释).
Root and required third-party license notices remain intact; generated files are
not edited by hand. This convention does not change the current identity protocol.

Peer synchronization direction, 2026-09-12: the maintainer selected
[per-Key reconciliation and streaming](cluster/key-stream-sync-design.md) for the future backend.
Publishers retain each Key's increasing version across Star redirection. Subscriber/Selector
authenticate, reconcile the authorized Key scope, replay available per-Key history or receive
that Key's complete current state, then continue on the same ordered gRPC stream.
SDK progress is per Key, not a receiving Star's log offset or a publisher-cursor vector.
Deleted and unknown remain distinct; writer restart, lease/binding ordering, deletion reclamation,
and mixed-storage confirmation still require protocol design. This is unimplemented and does
not alter the Redis SDK contracts below.

Peer target update, 2026-09-11: the maintainer selected C++26 for the future
Star/Planet implementation, keeping Supervisor in Go and temporarily excluding
MSVC from the new service target. The maintainer has now authorized implementation
in the separate `cluster-cpp/` directory; the runnable Rust/Tonic implementation remains
available as a comparison. C++ qualification is in progress, not yet complete.
Stage one retains the existing v4 admission/connection skeleton, without adding
business replication, storage or SDK migration. See
[C++26 skeleton design](cluster/cpp26-skeleton-design.md) for feature use, ownership,
locked source versions, gRPC qualification gates and acceptance tests.
The accepted C++ rules use logical session identity/generations, allow gRPC
transport reuse/rebuild, and do not freeze 64 KiB windows. Typed admission RPCs
keep byte and postdecode member limits without the earlier predecode scan.
GCC 16.2.0 is installed project-locally on Ubuntu and passed focused feature
probes; this is not evidence of a completed C++ service or full compiler qualification.

Peer development update, 2026-09-11: this is a separate backend track and does
not replace the existing Redis SDK contracts described below. Rust `peer`
is now a workspace with `common`, `star`, and `planet` crates. It implements
account-authorized Supervisor admission, gRPC protocol v4, process UUIDs, signed bearer Hello,
Ping/Pong, mirrored Star connections, single Planet upstream, bounded group
candidates, offline failover, retry and owned shutdown. Executables are `peer`
(Star) and `planet`; generated Rust is in `cluster/common/src/generated`.
The current contract is [`cluster/connection-rules.md`](cluster/connection-rules.md).
State replication, business persistence and SDK Bind remain unimplemented.
The isolated Rust gRPC push evaluation is complete: 660 v3 trials across
Windows, Ubuntu and cross-host sessions, with 650 completed and 10 failed.
This did not establish the maintainer's condition of no performance loss;
the initial conditional hold was subsequently superseded by the maintainer's explicit choice of gRPC. No second production backend is retained.
The historical TCP writer was fixed to flush within its deadline; v3 passed
39 production and 14 prototype tests on both platforms. Those are historical counts. See
[`cluster/grpc-benchmark-results.md`](cluster/grpc-benchmark-results.md) and
[`cluster/tls-flush-audit.md`](cluster/tls-flush-audit.md). Cross-host failure root
causes remain unresolved because remote detailed diagnostics were not retained.
Later on 2026-09-10 the maintainer explicitly chose gRPC for maintainability,
superseding the evaluation's conditional hold. The current implementation work
also replaces TLS-exporter/process-private-key proofs with account/password
Supervisor login and per-process signed bearer credentials. One account may
run multiple independent Star/Planet nodes. See
[`cluster/grpc-implementation.md`](cluster/grpc-implementation.md). The new service
foundation initially passed its own Windows, Ubuntu and mixed-host 11-case regressions
and 60-second fault loops, with nine restart cycles per deployment. Each host
passed 36 Rust service tests, 32 top-level Go tests, four generator tests and
14 probe tests; Linux Go race passed. See the independent
[v4 validation report](cluster/grpc-validation-20260911.md), not the old transport's qualification status.
The subsequent service hardening pass shares process launch and identity
validation, bounds all network timers/capacities, preserves transient gRPC
failure categories, validates local Supervisor certificates, and removes a
duplicate membership snapshot scan. Both hosts passed 53 Rust service tests,
44 ordinary Go top-level tests plus two fuzz seed entrypoints, four generator
tests and 14 probe tests. Python passed 28 on Windows and 27 with one platform
skip on Linux; Linux Go race passed. Both Go fuzz targets ran for a requested
10 seconds per host without failures. Native and mixed regressions now cover
13 groups; 120/120/300-second fault loops completed 17/18/42 restart cycles.
The final source only adds verified whitespace wrapping to the tested version;
both versions and fresh final builds are recorded separately. No dependencies
were downloaded or upgraded. See [hardening evidence](cluster/service-hardening-20260911.md).
The Galaxy design now selects full authorized state caches for Planets, shared
memory/disk storage modes for Stars and Planets, and mixed Star modes per Galaxy.
Running Planets may fail over using valid existing authorization during Supervisor
outages; new Star/Planet processes still wait for Supervisor admission.

The independent Go `supervisor/` module provides standard-library HTTP
management and a separate gRPC/TLS admission listener: validated CLI
configuration, JSON logging, bounded connections and graceful shutdown.
Its `/healthz` reports management health, not cluster readiness. Authenticated
registration and a transactional bbolt member table are implemented; Catalog
Publisher and live topology APIs remain unimplemented. Both native gates and
Windows/Ubuntu mixed-process regression and short fault loops pass. See
[`supervisor/README.md`](supervisor/README.md) for exact commands and boundaries.

The historical six-byte MessageID/length header was replaced by gRPC framing
in v4. Peer OpenSession carries generated SessionPacket messages; Supervisor
uses generated unary RPCs. `proto/message-ids.lock` retains old assignments
as history and does not dispatch v4 messages. Handwritten Rust/Proto comments use Chinese with
ASCII punctuation, and Proto uses the maintainer's four-space clang-format
configuration. Tools, dependencies and generated output remain project-local.
Peer installs a complete Supervisor list before initialization and verifies
direct connections. Legacy Discover and periodic topology queries are removed;
dial pacing, jitter and generation fencing remain. See
[`cluster/README.md`](cluster/README.md), [`proto/README.md`](proto/README.md), and
[`cluster/service-hardening-20260911.md`](cluster/service-hardening-20260911.md)
for the current implemented boundary and Windows/Ubuntu/mixed-host evidence.

The revised target is [Galaxy architecture](galaxy-architecture.md): a Galaxy
contains Peer stars; Relay planets aggregate subscriptions and cache state;
business satellites attach directly to a Peer or through a Relay. Unavailable
Peers appear as black holes. Cross-Galaxy Catalog wormholes are explicitly future
work. The Admin demo still depicts business entities as planets; neither the
new display model nor Relay runtime is implemented by this design revision.

As of 2026-09-02:

- GitHub repository `eosforge/verdandi` exists and is public; package and source
  references use this canonical identity.
- The commit containing this snapshot is the first public `alpha` source
  freeze. Its detached twelve-hour Registration and Catalog campaigns passed.
- The maintainer selected `0.1.0` as the current non-production Alpha version
  for distributed development and controlled service integration. No tag or
  package publication is implied by a working-tree metadata change.
- `1.0.0` remains reserved for the complete remaining stable scope and standard
  English production-source comments. Generic Campaign/Leader election was
  withdrawn on 2026-09-02 and is not a stable release gate. No stable protocol
  has been published.
- It was cloned to `D:\laconis\verdandi`.
- This working copy is on the public `alpha` review branch. Later edits remain
  uncommitted and unpushed until separately authorized.
- The initial Markdown foundation has been generated locally for maintainer
  review.
- Four operation-specific Registration Lua programs generated from reviewed
  shared/action fragments, the Go SDK, the Rust SDK, the C++23 SDK, and
  cross-language conformance harness now exist locally. Register and Selector
  cover bounded lifecycle, configuration, pagination, PING fencing, immutable
  views, TTL expiry, reconnect recovery, and joined cleanup. Selector has no
  Registry-wide Lua snapshot.
- Four generated Catalog Lua programs and Go/Rust/C++ Catalog child packages now
  attach to a shared root transport and implement Publisher
  Replace/Patch/Delete, one-persistent-listener plus at-most-one-temporary-sync Subscriber,
  stable Entry, per-load generic typing, Hash/ZSET/Read repair, explicit bounded
  tombstones/floor, and optional monotonic bbolt/redb/SQLite checkpoints. Current Lua,
  SDK, reconnect, checkpoint, decoder, and Redis 8.8 tests pass.
- Go, Rust, and C++ validate and parse the request/schema/capacity contract before
  Redis I/O. Registration Lua is only the atomic Redis glue for current-state
  revision, time, Hash/membership, matched expiry, reply, and publication.
- The successful Lua hot paths are fully line-audited and contain no generic
  helper closures. The four generated bodies currently total 14,112 bytes
  after adding temporary Chinese maintenance comments; executable statements
  and steady-state EVALSHA paths are unchanged. Earlier same-Redis paired
  results improve Register, Update, and Renew server time over the prior
  positional set, while Unregister is neutral.
- Go/Rust Selector records treat validated internal field bytes as immutable.
  State transitions may shallow-copy map structure or share Rust `Arc` payloads,
  but every public record remains detached. Complete-record byte size is cached
  on the immutable internal record and updated by exact revision/version/value
  deltas; every new construction or mutation path must preserve this invariant
  and its decimal-width/capacity boundary tests.
- The 2026-08-25 production review reduced measured Go Selector Update/Renew
  state application from 4.103/3.083 to 1.300/1.236 us/op and allocations from
  37 to five. The ordinary no-repair pending drain is zero-allocation. Lua
  executable logic and public APIs are unchanged; comment regeneration changes
  script bytes/SHA and therefore requires the current freshness/integration
  gate.
- Both root Clients are thin Redis wrappers owning connectivity, ordinary
  operation timeout, bounded Key/Hash commands, and a transport-close signal.
  Zone and Redis 8 validation belong to domain configuration and Registration
  bootstrap respectively. Registration and Catalog own and join their workers;
  root close broadcasts loss and closes Redis without waiting for them. Rust
  keeps awaited Fred shutdown and best-effort last-handle Drop semantics.
- Go child domains consume `Client.Redis()`, `Done()`, and
  `Timeout()` directly. `Redis()` is the same borrowed
  `*redis.Client`; the root retains close ownership, while raw operations are
  governed by ACLs and bypass Verdandi validation, limits, atomic invariants,
  and stable error mapping. Rust does not expose Fred.
- The Zone's shared Registration limits belong to the non-expiring
  `verdandi:config:<zone>` Hash. Registration Client bootstrap writes missing
  defaults; authorized backends may update them atomically; Registration
  Clients retain and refresh a last-valid snapshot. Steady-state Lua Update
  performs neither a configuration read nor a full-Hash capacity scan.
- Go now requires 1.27. Concrete generic methods expose
  `Client.Registration[A, D]`, `Client.Selector[A, D]`, and `Entry.Load[T]`;
  package-level generic constructors are removed. Internal generic function
  assignment avoids retained Selector codec closures, and promoted embedded
  fields are used where the flattened literal is explicit and complete.
- Go Selector additionally offers an optional `verdandi-refgen` facade for a
  measured callback-only hot path. It generates read-only Attr/Data accessors,
  selected-Data setters, mutable-slice clones, aliases, and a wrapper but never
  application codecs or policy. `WithOne`/`WithAny` return no detached values,
  commit only edits attached to the final Selection set, reuse the ordinary
  Selector gate/view/overlay, and runtime-fence Editors after the callback.
- A non-normative decision docket now turns the P0 and P1 open questions into
  explicit recommendations for maintainer review.
- Redis Open Source 8.0.0 or later in a qualified Redis 8 line is the Alpha
  backend baseline.
- The first complete source, documentation, conformance, and bounded-regression
  snapshot has been committed and pushed once under the maintainer's explicit
  2026-08-31 instruction. Its detached worktree is the only source accepted by
  the post-freeze twelve-hour campaigns.
- MIT is selected. Zone/Type and Registration UUID forms, positive-integer
  application Registration Version, Catalog last-write-wins batches,
  role/Zone ACL granularity, and Registration Meta/Attr/Data field classes are
  accepted. Registration events use key/value MessagePack arrays with aligned
  `register`, `update`, `renew`, and `unregister` kinds; Registry recovery uses per-Registration revisions,
  RedisClock, paginated membership, and a subscription PING/PONG fence without
  a Registry-wide revision. Go and Rust have passed standalone Redis 8.8,
  bounded per-UUID pending-event coalescing, cross-language MessagePack, and a
  real three-node Redis/three-Sentinel failover matrix. Exact typed Attr/Data
  codecs and wider event/fan-out benchmarks remain under review. Generic
  Campaign/Leader election is out of scope. Existing Registration Version and
  Register/Update behavior remain application-defined and unchanged. Rust uses
  the qualified `fred` line for the implemented slice.

## 14. Durable Decision Record

Keep only decisions that materially constrain future work. Never delete a
superseded entry; mark it superseded and link to its replacement.

### 2026-09-11: Design a C++26 replacement for the Star/Planet skeleton

- **Accepted target:** Star/Planet move toward C++26 with GCC 16.2.0 / Linux x64
  first; Supervisor remains Go. MSVC support is temporarily not required for
  this new service target. Existing Redis SDK/C ABI platform and language
  contracts are unchanged.
- **Current task boundary:** The maintainer stopped implementation and then
  requested detailed documentation. This documentation-only boundary was later
  superseded by an explicit instruction to start the C++ implementation in a new
  directory. Use `cluster-cpp/common`, `cluster-cpp/star`, and `cluster-cpp/planet`; retain
  the Rust comparison and do not remove it before C++ qualification.
- **Dependency authorization:** The maintainer explicitly approved acquisition
  and building of the named C++ source set in Ubuntu project-local
  `build/deps/peer-cpp`, with generators under `build/tools`. No global or Windows
  dependency installation is implied. The VM uses dynamic memory; build one job
  at a time and observe actual memory, rather than assuming maximum allocation.
- **Scope:** Stage one preserves gRPC v4, signed bearer admission, Star mirrored
  logical sessions, Planet single-upstream failover and owned shutdown.
  Business storage/replication, SDK Bind and Admin changes remain later work.
- **Engineering direction:** Apply reflection/annotations to private option
  metadata, expansion statements to field traversal and inplace_vector to the
  bounded candidate set. Do not create a second Protobuf codec or custom async
  framework merely to use new language features. Benchmark allocation/copy
  changes and retain explicit input validation independently of contracts.
- **Accepted simplification:** Identity/member epoch and local session generation
  belong to logical sessions; gRPC may reuse or rebuild transports. Retain two
  directed Star sessions and one active Planet upstream, without a physical TCP
  lifetime/session restriction. 64 KiB is not a protocol requirement. Count
  logical topology and physical transports separately in diagnostics and tests.
- **Internal-service validation:** Drop the proposed Register predecode Member
  scan and raw unary adapter. Use generated typed RPCs with byte limits, then
  member-count, authentication, role and epoch validation. This accepts temporary
  allocation amplification before postdecode rejection; no performance benefit
  has yet been measured. Keep lifecycle and correctness checks.
- **Runtime and optimization:** Prefer public gRPC resource/lifecycle APIs and
  measure slow-handshake cleanup before considering a small adapter. gRPC owns
  I/O workers; one application control loop initially. Explicitly reject the old
  Tokio worker option; expose separately named budgets only after qualification.
  Start with ordinary messages and bounded reuse; Arena/shared encoding require
  comparative evidence before becoming defaults.
- **TLS and version lock:** Use the selected gRPC release's BoringSSL commit for
  transport and private identity crypto, without an additional OpenSSL provider.
  Lock current stable source versions as of 2026-09-11: gRPC C++ 1.84.0,
  Protobuf/protoc 36.1 and the exact remaining versions/commits in
  [the dependency lock](cluster/cpp26-dependencies.md). No floating latest or
  automatic upgrades. Independently latest dependencies differ from gRPC's
  bundled revisions; the selected combination compiled and passed the documented
  service checks, without claiming all upstream self-tests or every platform.
- **Evidence:** The independent C++ skeleton is implemented. Debug/Release,
  ASan/UBSan, fully instrumented TSan and a 3,603.207-second fault soak passed.
  On 2026-09-12 the maintainer resumed remaining qualification: metadata negative
  compilation, contracts-disabled regression and up to 16 Stars / 32 Planets
  have passed. Five-round push comparison completed 225 trials with 194 full
  passes and 31 explicit capacity rejections; ordinary cases all passed.
  Forty separate allocation trials passed. Default Linux service entry now
  selects C++ and passes full offline gates and process regression. Rust remains
  explicitly selectable for interoperability; Windows C++ selection fails early.
  M0-M4 connection scope is qualified with the analyzer, performance and capacity
  limits in [the supplementary report](cluster-cpp/qualification-20260912.md).
  A subsequent maintenance pass separated private process facilities, tightened
  ownership, removed temporary Planet duplicate indexes and closed empty-CTest
  success and empty library-search-path cases. Debug/Release, ASan/UBSan, full
  TSan, a 61.531-second fault cycle and installation checks pass; see
  [maintenance evidence](cluster-cpp/maintenance-20260912.md) and the
  [module ownership guide](cluster-cpp/CONTRIBUTING.md). Historical hour/performance
  evidence remains attached to its original source and binary fingerprints.
- **Expanded endurance:** On 2026-09-12 the maintainer requested more nodes,
  two hours of repeated fault recovery followed by steady operation until manual
  stop. The run uses 8 C++ Stars, 16 C++ Planets and Go Supervisor on Ubuntu,
  with current qualified Release binaries. The project-owned controller advances
  phases independently of SSH, samples bounded resource history and handles STOP
  or SIGTERM with owned-resource cleanup. No upper-layer changes or downloads.
  See [endurance operation](cluster-cpp/endurance-20260912.md); running is not passed.
  The first attempt failed in the Python checker on a legitimate null upstream,
  with successful resource cleanup. Preserve that evidence and restart the full
  duration in a new directory after repair. The maintainer explicitly clarified
  that authorized testing includes investigating and fixing routine failures;
  a heartbeat must not simply stop work and wait after reporting a fixable error.
  Existing package-specific download permissions remain unchanged.
  The repaired run completed 7205.739 seconds / 833 fault cycles and then
  7046.696 seconds of steady observation including exit cleanup. The maintainer
  stopped it at 2026-09-12 16:30:17.775 Asia/Shanghai; all 25 services and the
  controller exited, ports and the owned directory were released, and the
  heartbeat was paused. Final evidence remains attached to pre-rename peer-cpp
  binaries: `testkit/results/peer-cpp-endurance-final-20260912-r2.json`.
- **Supersession:** Replaces the Rust language target in earlier Peer/Planet
  choices and the initial C++ physical-session/window/predecode requirements.
  Preserve v4 fields, role/storage decisions and historical Rust validation records.
  Full plan: [C++26 skeleton design](cluster/cpp26-skeleton-design.md).

### 2026-09-10: Revise the system as Galaxies, Peer stars, Relay planets and business satellites

- **Accepted mapping:** Multiple equal Peer stars form a Galaxy. Relay planets
  attach to Peers; Catalog/Subscriber/Registry satellites may bind either to
  a Peer or a Relay. Unavailable stars appear as black holes, preserving their
  identity rather than becoming another service type.
- **Accepted Relay role:** A Planet caches the Galaxy's complete authorized
  state even without downstream subscribers, then filters and fans out to each
  authorized binding. This supersedes demand-driven upstream subscriptions and
  last-subscriber cache eviction. Peer retains business-version and authority
  responsibilities. Relay does not become a Peer mesh member or durable voter.
- **Future scope:** Wormholes may eventually synchronize Catalog across Galaxies.
  Do not implement cross-Galaxy routing, protocols or background tasks now.
- **Later clarification:** A Planet forwards Publisher/Registry requests only
  to its current bound Star. Stars accept requests and synchronize their lawful
  state to other Stars and their attached Planets. Replicas can serve their own
  Planets without rebroadcasting each received record to the Star mesh.
- **Planet admission:** A Planet authenticates with Supervisor and obtains a
  local subset of candidate Stars rather than full Star membership. Already
  running Planets may switch and re-register using still-valid existing
  authorization while Supervisor is offline. New Star/Planet processes, even
  previously admitted deployments with disks, wait for Supervisor authentication.
- **Logical Star groups:** The maintainer wants Stars grouped by properties such
  as actual region. Treat this as candidate organization within the Galaxy's
  full authorized replication scope, not business Zone or data sharding.
  GQ-007 confirms local-group preference with cross-group failover when the
  local group is unavailable. Galaxy-wide authorized replication is unchanged;
  group labels do not grant cross-scope access. Supervisor-offline failover
  still needs previously learned candidates and valid existing authorization.
- **Shared implementation direction:** Star and Planet should reuse state storage,
  merge, snapshot/differential sync and downstream flow control. Role authority
  and request forwarding remain separate; Planet receipt is not a business ACK.
- **Accepted storage modes:** Star and Planet both support memory or disk storage;
  Stars in the same Galaxy may mix modes. Share record/merge/recovery semantics,
  but Planet disks and memory Star receipts do not count as durable Star ACKs.
  Persistent deployments retain the Publisher-offline disk recovery goal;
  all-memory deployments cannot recover after losing every live copy. The
  durable participant set, memory ingress and all-memory ACK rules still need
  a concrete design. Do not silently weaken durable success on node failure.
- **Design recommendations, not frozen choices:** One active parent per satellite
  binding and satellites representing logical bindings. Relay language and
  packaging were subsequently selected as Rust `cluster/planet`, sharing
  `cluster/common` with `cluster/star`. Memory-only cache and demand-driven upstream
  aggregation recommendations are superseded by the maintainer's answers.
- **Accepted re-registration:** When Planet changes its bound Star, it registers
  its managed active publication/Registration state at the new Star. That Star
  propagates the replacement and invalidates the old Star affiliation, without
  depending on the old Star being reachable. This supersedes the earlier fixed
  Star ingress restriction; it does not change the logical Publisher or delete
  the business key and all replicas. Preserve a running service's Registration UUID.
- **Remaining protocol work:** Prefer one versioned replacement over independent
  unconditional Delete/Register messages. Old updates, unregisters and expiry
  actions must be fenced; per-record recovery must handle partial batches.
  The order/epoch issuer, delegation proof and confirmation rules are undecided.
  Current Star membership Epoch is not a business lease epoch. Full cached
  replicas and dead downstreams do not become managed active registrations.
  Any epoch scheme must permit running Planet failover without contacting
  Supervisor for each switch; its existing authorization must remain valid.
  Catalog confirmed-state recovery and durable ACK rules still require design;
  no implementation is claimed by this accepted responsibility change.
- **Observation boundary:** Monitoring staleness and Supervisor unavailability
  must not be treated as proof all Peer stars are unavailable. Black-hole display
  is not deletion, data loss or authority transfer.
- **Implementation status:** Star/Planet connection rules, role admission,
  bounded group candidates and failover are now migrated to gRPC protocol v4.
  See [current connection contract](cluster/connection-rules.md) and
  [validation](cluster/star-planet-validation-20260910.md). No new dependencies
  were downloaded for this role split. Business storage/sync remain pending.
  Admin still has the earlier Registry/Subscriber/Publisher planet model.
  See [shared structure and implementation sequence](galaxy-architecture.md).

### 2026-09-11: gRPC and account-authorized service admission

- The Rust implementation below remains current; its future language target
  is superseded by the C++26 design decision above. Protocol v4 remains current.
- Accepted: Rust Tonic Star/Planet and Go gRPC Supervisor; same account may run
  multiple nodes, each with an independent UUID and signed bearer credential.
- Account passwords travel only to Supervisor over TLS. No TLS exporter or
  process signing key remains. TLS still verifies server roots, names and validity.
- Current implementation identifies a membership slot by SHA256(username + NUL +
  Galaxy + NUL + canonical endpoint). Different endpoints remain independent;
  same endpoint restart retains CAS fencing and response-loss idempotence.
- Supervisor accounts.json contains role permissions and salted PBKDF2 hashes;
  node login.json contains username/password. --make-account reads stdin and
  emits one account record. No password CLI argument or global configuration.
- Credentials have no independent expiry or online revocation. Password changes
  do not invalidate existing credentials. Address/account changes create new
  slots; retirement, HA and business storage remain unimplemented.
- v4 is incompatible with v3 wire/signature format. Old public_key member stores
  fail closed; deploy with a new database path, preserve the old database.
- Approved project-local dependencies: tonic/tonic-prost/tonic-prost-build 0.14.6,
  grpc-go 1.83.2, protoc-gen-go-grpc 1.6.2 and necessary dependencies on both hosts.
  Rust minimum is now 1.88; actual verified toolchain remains 1.98.1.
- New implementation and validation status: [gRPC contract](cluster/grpc-implementation.md).
  Historical v3 benchmark output is preserved and cannot qualify this version.
- Service hardening keeps v4 unchanged. Common owns shared Rust process launch
  and RPC error classification; temporary transport errors do not quarantine
  Planet candidates. All network durations are bounded to 1 ms..24 h before
  tasks are created, with explicit connection/dial/event limits.
- Go admission and membership share identity validation; Rust/Go consume the
  same public identity vectors. Slot replacements cannot change role/address.
  Membership commits reuse the validated owned snapshot instead of reading and
  decoding the full member bucket again. Tests remain with their owning layer.
- One-key service tests include Python harness unit tests; service checks can
  run bounded Go fuzz with `-FuzzSeconds` / `--fuzz-seconds`. All use existing
  project-local tools/caches and never imply authorization to download.

### 2026-09-10: Check in generated protocol source and prepare authenticated membership

- **Accepted workflow:** The maintainer approved explicit Rust/Go protocol
  generation with checked-in service-local generated source, Supervisor
  admission and mesh integration, and comprehensive tests. Finish features
  and test cases, simplify/review the code, then execute tests.
- **Generation:** `proto/generator` owns protoc/Prost invocation and append-only
  MessageIDs. Outputs live in `cluster/common/src/generated` and
  `supervisor/internal/generated`. Ordinary service builds do not invoke
  protoc; one-key service checks independently compare regenerated output.
- **Tools:** protobuf-go/protoc-gen-go 1.36.12 were specifically approved and
  installed on both hosts under project build/. This does not approve new
  unrelated dependencies or global changes.
- **Historical v3 admission (superseded by the later v4 decision):** The maintainer approved signed admission in the complete
  registration response, with no separate application. TLS 1.3 mutual authentication
  binds the deployment certificate; a fresh process key signs the TLS exporter and
  Hello negotiation. Supervisor signs raw member protobuf. Retries retain the first
  CAS baseline and the same process UUID/key.
- **Dependency authorization:** bbolt 1.4.3, x/sys 0.48.0, x/sync 0.10.0,
  testify 1.10.0, go-cmp 0.7.0 and necessary indirect dependencies are approved
  in both project-local Go caches. No global installation is authorized.
- **Progress:** Both native toolchains pass 29 Peer, 24 Go top-level and four
  generator tests; Linux Go race, actual-process regressions and 60-second
  fault loops pass. After explicit maintainer approval, two temporary Windows
  inbound rules enabled direct mixed-host qualification: two Peers per host,
  six regression groups and 13 fault cycles in 60 seconds passed on the final
  predecode count/phase guards. Both owned rules were removed and their absence
  verified in ActiveStore; test processes and owned temporary directories are gone.
  The earlier native fault loops remain historical runs before those input guards.
  See [Validation](service-admission-validation-20260910.md).

### 2026-09-09: Normalize the service foundation and process lifecycle

- **Structure:** Rust binary `peer` delegates CLI/logging/signals to `src/app`
  and borrows the existing network library. Go app owns server lifetime while
  `internal/management` owns HTTP handlers. Do not impose identical language
  trees or add empty domain/persistence abstractions.
- **Runtime:** Peer no longer stops on stdin/EOF. Observe Ctrl+C/SIGTERM and
  root-task termination; always request cleanup on error. Default worker count
  is 2, shutdown deadline 5 seconds. Exit codes are 0/1/2 for normal/runtime/
  configuration outcomes in both services. Diagnostic events remain lossy.
- **Build:** Normal Cargo commands are offline; service checks use frozen
  locks. Message IDs are still automatic but only an explicit
  `VERDANDI_UPDATE_MESSAGE_IDS=1` command may update their source lockfile.
  Checks force that switch off. No schema or existing ID changed in this pass.
- **Scope:** Binary naming and equals-style CLI are implemented, superseding
  their earlier design-only status below. Explicit `--id` and legacy Discover
  remain until the separate registration/process-identity migration. This
  foundation does not approve TLS, persistence, membership or capacity claims.
- **Dependencies:** Maintainer approved signal-hook-registry for Tokio signal;
  JSON logging reuses existing serde_json caches. Versions stay in Cargo.lock.
  [Service foundation](service-foundation.md) owns commands and release gates;
  [Contributing](CONTRIBUTING.md) owns the contributor entry point.

### 2026-09-09: Establish the Admin frontend foundation

- **Scope:** `admin/` remains a Vue 3 / TypeScript / Vite / Naive UI / Three.js
  Alpha frontend with three available stars and one isolated unavailable black
  hole in the local demo, not a live Supervisor
  topology or authorization implementation.
- **Boundaries:** App composition injects a readonly `GalaxyData` snapshot.
  The galaxy feature separates model, demo data, Vue UI/composable and Three.js
  runtime. Runtime has no Vue, Supervisor API or demo-data dependency; scene
  loading stays dynamic. A local asset loader reads the bundled GLB models.
  Stable IDs identify selections and explicit links identify adjacency.
  All celestial bodies use shared GLB geometry; unavailable nodes expose no
  planets or incident links, including when an input snapshot retains old data.
- **Ownership:** One scene owns selection, one RAF loop and a reverse-order
  resource scope. Store::Snapshot replacement, retry, failed initialization and unmount
  release old resources; cancelled imports and GLB loads cannot mount a scene.
- **Quality gates:** Node 24 built-in regression tests, strict Vue/TypeScript,
  Prettier and an import-boundary check run through `pnpm check`. No new
  dependency or automatic install/publish workflow was introduced.
- **References:** [Admin README](admin/README.md),
  [architecture](admin/docs/architecture.md) and
  [contribution rules](admin/CONTRIBUTING.md). Full snapshot replacement resets
  the scene; incremental synchronization and large-scale GPU qualification are
  separate future work. Root branch/publication rules remain unchanged.

### 2026-09-09: Start Supervisor with a bounded management service skeleton

- **Dependency update:** The maintainer subsequently approved replacing the
  private listener limiter with `golang.org/x/net/netutil`. Pin `x/net v0.59.0`
  and its `go.sum` checksums; this supersedes only the standard-library-only
  choice below. Ordinary wrappers stay offline. Explicit acquisition uses
  project-local caches and the Go module proxy/checksum database; chi and gnet
  are not selected dependencies.
- **Implemented scope:** Independent Go module `supervisor/`, executable
  `supervisor`, management HTTP listener, CLI validation, structured JSON logs,
  bounded connection admission and owned shutdown. Use the existing Go 1.27
  baseline and standard library only, with no new dependency or framework.
- **Boundary:** Management `/healthz` is not Peer/cluster readiness. Its HTTP
  listener is not the future TCP/Protobuf `peer --super` registration endpoint.
  Do not expose successful registration or persistent member responses before
  those protocols and storage are implemented. Peer UUID replacement, durable
  membership, Catalog publishing and admin integration remain separate work.
- **Operations:** `supervisor/go.ps1` and `go.sh` preserve project-local cache
  locations and force local-toolchain, offline, readonly-module operation in
  child processes only. No global environment/config changes or downloads.
- **Validation:** Native Windows vet, HTTP/lifecycle tests and binary build;
  native Ubuntu vet, race tests and build using existing Go/GCC. Linux executable
  also verified closed stdin, health, SIGTERM and released port. This qualifies
  the skeleton, not production topology or persistence. See
  [Supervisor README](supervisor/README.md).

### 2026-09-09: Simplify Peer launch and use process-scoped UUIDs

- **Target interface (CORE-012):** `peer --listen=IP[:PORT]
  --super=HOST[:PORT] --cluster=ID`, using `--name=value`. Name the local-address
  placeholder `--listen`; the target executable is `peer` / `peer.exe`.
  Keep cluster separate from business Zone and do not add a `--zone` flag.
  Port defaults remain proposals; explicit ports are shown in examples.
- **Process identity:** Generate one Peer UUID at process startup. Reconnects
  and registration retries in that process reuse it; restarting generates a new
  UUID. Do not require `--id`, persist/recover the previous Peer ID, or silently
  substitute an additional fixed Peer ID. A process UUID is not authentication.
- **Persistent state:** Business Key/version/deletion state survives unchanged.
  Separate process identity from durable ownership and replica accounting before
  implementing StateStore/OwnerPush. A new UUID does not authorize taking over an
  old owner or counting the same disk twice in durable confirmations.
- **Remaining identity work:** Review new UUID at an existing address, stale
  process cleanup, Hello/boot_id consolidation and connection generations.
  The old Supervisor-offline restart proposal relied on stable Peer identity;
  it is superseded, and how a new process reuses legitimate deployment authority
  while Supervisor is offline remains unresolved. Do not infer admission from IP
  or a cached list. The Publisher-offline durable Catalog recovery goal remains.
- **Status:** Design only. Current code still builds `verdandi`, uses explicit
  `--id`, and implements Discover rather than Supervisor registration. Current
  `next_boot_id` is not a UUID generator. No source/schema changes or downloads.
  See [Core identity/configuration](cluster/core-design.md#4-peer-身份与配置).

### 2026-09-09: Use Go for Supervisor and Rust for Peer

- **Language target superseded 2026-09-11:** Future Star/Planet select C++26;
  current Rust code remains until the separately authorized migration qualifies.
  See [C++26 design](cluster/cpp26-skeleton-design.md). Supervisor remains Go.
- **Confirmed languages:** The independent Supervisor service uses Go; the Peer
  service uses Rust. This resolves the earlier pending Supervisor language choice.
- **Responsibilities:** Supervisor handles membership registration, management
  and observation APIs, and may act as an authorized Catalog Publisher. Peer
  owns business replication, authoritative Catalog persistence and SDK sync.
  Rust's internal per-connection supervisor remains part of the Rust Peer.
- **Scope:** Language selection only; storage engines and remaining protocol
  decisions are not selected by implication. The Go Supervisor backend is not
  implemented yet. No tool or dependency downloads are authorized by this entry.
- **References:** [Supervisor design](cluster/supervisor-design.md) and
  [Peer design](cluster/design.md), P-004/P-012.

### 2026-09-09: Keep authoritative Catalog persistence in Peers

- **Updated 2026-09-10:** The Galaxy decision above allows memory/disk modes and
  mixed Stars. The following durable recovery goal applies to persistent
  deployments; memory mode cannot promise recovery after every copy is lost.
  New Star/Planet processes must now wait for Supervisor authentication, even
  when restarting a previously joined deployment. This supersedes the offline
  bootstrap suggestion below; business disk recovery does not grant admission.
- **Final clarification:** Peer must durably retain authoritative Catalog state
  and recover after the whole Peer group restarts while the Publisher is offline.
  This supersedes the intermediate chat choice of memory-only Peer business
  state. A discardable disk cache alone does not meet this requirement.
- **Supervisor role:** Supervisor may also act as Catalog Publisher for its
  authorized keys, using the normal Publisher interface and per-key single-writer
  rule. It reconciles with Peer authority before continuing updates; do not add
  a second independent authoritative Catalog database to Supervisor by default.
  Its durable membership/management identity remains a separate responsibility.
- **Recovery scope:** Peer stores complete current State, versions, deletion and
  necessary ownership metadata. Registration remains Publisher/lease based and
  Registry remains derived. Preserve A-001/A-002; quorum membership, owner
  recovery and write fencing still require protocol review. Durable ACK does not
  imply every Peer or SDK has already received the update.
- **Offline bootstrap:** If Supervisor is also offline, previously joined Peers
  need recoverable identities and persisted successful-join/known-member records
  to reconnect. New boot IDs still distinguish sessions. First-time nodes still
  need Supervisor's complete list; corrupt or missing local membership cannot
  be treated as successful prior admission. Exact persistence format is pending.
  **Superseded assumption:** CORE-012 above replaces cross-restart Peer identity
  with process UUIDs; offline admission must be redesigned, not inferred here.
- **Limits:** Surviving valid disks/backups are required; losing every durable
  copy is not ordinary process restart. Memory-only Core tests do not qualify
  production persistence. No storage engine, Supervisor language, single-socket
  switch, automatic leader election or new dependency is selected here.
  The later language decision above resolves Supervisor to Go and Peer to Rust.
- **Status:** Documentation only; current Peer code still lacks business state
  replication and storage. See [Supervisor Catalog role](cluster/supervisor-design.md#7-supervisor-兼任-catalog-publisher)
  and [Peer design](cluster/design.md). Existing Redis SDK contracts are unchanged.

### 2026-09-09: Bootstrap Peer membership through Supervisor registration

- **Latest accepted flow (CORE-011):** A new Peer registers its identity/address
  with Supervisor and obtains a complete Peer list before joining. It actively
  connects to listed peers. Existing peers learn that new peer from its own
  Hello and establish the reverse connection, even if their old lists omit it.
- **Outage boundary:** Peers that already obtained a complete list keep
  communicating and may finish joining while Supervisor is unavailable. Peers
  that have not obtained a complete list wait for Supervisor recovery. This is
  not a rule to reject an unfamiliar but valid new peer merely due to an old list.
- **Simplicity:** Remove all Peer-to-Peer topology reconciliation, list exchange
  and third-party introductions. Do not add an independent admission-ticket
  step, Supervisor-wide member-list push, or delta ACK/replay state machine.
  Keep Hello validation, mirrored TCP, bounded retries and business data sync.
- **Ordering:** Supervisor registration and complete-list snapshot must have
  a consistent order, so the later member of each pair knows the earlier one.
  Persist registrations before successful responses and make retries idempotent.
  Concurrent starts do not need Peer gossip when these conditions hold.
- **Boundaries:** Membership registration is centralized; the data plane remains
  peer-to-peer. Peer offline restart/persisted identity (subsequently refined by
  the authoritative Catalog recovery decision above), production admission
  security, member removal/address migration and business quorum changes remain
  separate decisions. Fetching a list is an initialization gate, not itself an
  authentication proof that a remote plain Hello can verify.
- **Status:** Design only. Current code still implements periodic Discover;
  Supervisor and UI are not implemented. The previous observer/pairwise design
  below is superseded, as are intermediate ideas for full-list push and tickets.
  See [Supervisor design](cluster/supervisor-design.md). No new download permission.

### 2026-09-09: Separate Peer join discovery from cluster supervision

**Superseded by [Supervisor registration](#2026-09-09-bootstrap-peer-membership-through-supervisor-registration).**
Preserved as decision history; pairwise discovery and observer-only repair are
not the current target.

- **Accepted direction:** Reconcile topology between the connected pair on join
  and reconnect; retain deduplicated mirrored TCP connections, Ping/Pong and
  bounded retries of known targets. Normal deployments add one Peer at a time
  and wait for completed interconnection with an already complete network.
- **Exceptional repair:** An independent Supervisor observes the cluster and
  alerts or schedules bounded, targeted missing-connection repair. Do not
  implement topology revision changes as all-neighbor full-list query triggers.
  Sequential startup alone is not a general convergence guarantee; if the
  Supervisor is the sole fallback, exceptional discovery recovery depends on
  its eventual availability and complete observation coverage.
- **Scope:** Peer business replication remains separate and still needs its
  own reconciliation guarantees. The cluster Supervisor is not the internal
  per-connection task supervisor or a business data leader.
- **Status:** This supersedes periodic all-neighbor discovery as the target
  repair design, not as a description of current code. Existing jittered
  periodic Discover remains implemented until replacement and fault validation.
  Supervisor, management APIs and 3D UI are not implemented. UI details are
  proposals, not frozen decisions or permission to download dependencies.
- **Design:** See [Supervisor design](cluster/supervisor-design.md) for expected
  inventory, stale observations, directed TCP accounting, bounded repair and
  shared graph data for browser 3D, 2D and connection-table views.

### 2026-09-08: Consolidate automated testing into regression and soak modes

- **Accepted interface:** One Python controller exposes `regression` and
  `soak --duration`. PS/Bash remain thin entries. SDK tests stay native and can
  be selected independently; a narrowed selection is never full-project evidence.
- **Duration:** Effective load time is per selected domain; serial domains add
  their times. Preparation/build/cleanup are additional, and the Redis TIME
  floor remains mandatory. Interrupted or shortened runs cannot pass. The
  public range is 210 seconds through 24 hours per domain and target; fault
  timing starts on workload readiness rather than process/compilation startup.
- **Ownership:** Cleanup runs on success, failure, timeout and cancellation.
  Register creation intent durably; verify run labels and immutable container
  IDs before removal. Keep resource manifests for verified recovery after a dead
  controller, while protecting active runs. Retain reports/logs/dependency caches.
- **Isolation:** All caches, local configuration and reports are under ignored
  project `build/`. Existing approved Redis images use `--pull never`. Test
  commands do not install tools or restore from remote package feeds implicitly.
  Windows dispatches Linux work to the Ubuntu VM, replacing WSL branches.
  Child-only TEMP/TMP/TMPDIR/GOTMPDIR use each campaign's owned scratch directory.
  The source-only Ubuntu copy has its own project-local Git metadata for direct
  Bash source inventory; synchronization does not copy Git history or remotes.
- **Authorization:** The maintainer specifically approved redis, msgpack,
  Paramiko, cryptography and runtime dependencies in both project Python venvs;
  existing pip reuse on Ubuntu; and its project-only .NET 10 SDK, .NET 8 runtime
  and NuGet reference packages. These supersede the earlier Python deferrals;
  they are not a general future download grant.
- **Evidence:** Keep exact source fingerprints, platform/language/scenario
  outcomes, partial results and cleanup status. Missing coverage remains visible.
  Implementation and current verification belong in `worklog.md`.

### 2026-09-08: Share native build policy in Python and keep thin platform entries

- **Decision:** The maintainer installed Python and approved implementing the
  reviewed shared-core approach. `sdk/cpp/build.py` is the standard-library
  C++ build orchestrator; `build_support.py` owns native discovery and process
  lifetime. `build.ps1`/`build.sh` preserve the existing platform command forms.
  Python 3.10+ is required for these entries; native CMake presets remain
  available. Go/Rust retain their small SDK cache wrappers; C# uses dotnet.
- **Boundaries:** Preserve the existing profile/linkage flags, per-platform
  cache layout, offline contract and system -> installed vcpkg -> project
  prebuilt OpenSSL search order. Build policy lives once in Python; CMake
  remains authoritative for targets, dependency definitions and native tests.
  No toolchain/package installation is added to the entries. Diagnostic
  environment overrides apply only to child processes.
- **Python selection:** Use an existing user interpreter, with `-Python PATH`
  on PowerShell or leading `--python PATH` on Bash when necessary. Never
  hardcode a Codex runtime path. The Windows adapter can find uv's existing
  `~/.local/bin/python.exe` without modifying PATH and skips empty Store
  placeholders. PowerShell 5.1 source retains a UTF-8 BOM.
- **Formatting authorization:** The maintainer explicitly approved Black and
  its runtime dependencies from PyPI, installed in `build/tools/python-build`.
  Black 26.5.1 was selected; pip downloads use `build/deps/pip` and Black cache
  uses `build/cache/black`. This grants no other Python package downloads.
  Ordinary C++ builds and policy tests use only the standard library. Earlier
  Redis/Sentinel Python fixture work was separate at this decision point and
  was subsequently approved by the unified-test decision above.
- **Propagation review:** C++ reuses `sdk/run-tool.ps1` for PowerShell 5.1
  argument quoting. Its Unicode stdout handling was fixed centrally for all
  Go/Rust/C++ callers, with the console encoding restored after the command.
  Bash and direct Python forwarding are checked separately; SDK/Lua protocol
  source and the C# build path are unaffected.

### 2026-09-07: Authorize SDK dependency acquisition after adopting project cache entry points

- **Maintainer authorization:** The maintainer explicitly approved downloading
  this project's Go, Rust, and C++ dependencies on Windows and the Ubuntu test
  VM at `192.168.0.119`. This supersedes the earlier no-download constraint for
  those SDK dependencies. Use the versions/manifests/checksums already selected
  by the project; record sources, cache locations, and results. Unrelated tools
  and Python dependencies are not covered by this authorization.
- **Persistent working rule:** Use the owning SDK project entry points for
  dependency restoration, building, and testing. Go/Cargo settings must remain
  limited to launched tool processes; never change the maintainer's terminal,
  user/system environment, or global tool configuration. Retain the cache
  locations and C++ system/vcpkg/project-cache order documented below.
- **Review before testing:** Before resuming runtime tests, perform a fresh
  line-by-line static review of current owned source, tests, and build tooling.
  Check unexpected errors, opportunities for idiomatic simplification, and
  cross-language differences. Record reviewed file hashes/ranges and distinguish
  confirmed defects, contractual language differences, and unverified risks.
  Each fix still requires a propagation review across all SDKs/bindings/Lua.
- **VM boundary:** Keep VM caches/build output local to its checkout and omit
  them from source synchronization. Retain the external OpenSSL-build policy;
  dependency download authorization does not turn on automatic OpenSSL source
  compilation. Python fixture fixes and downloads were deferred at this point;
  the later unified-test decision above explicitly authorizes that work.
- **Subsequent explicit VM tool approval:** The maintainer also approved Go
  1.27.1 and Rust 1.98.1 from official sources under the VM project's
  `build/tools`, plus Ubuntu official `build-essential`, `cmake`, and
  `pkg-config` in system directories. This named approval covers their required
  installation dependencies; it is not a general tool-installation grant.

### 2026-09-07: Keep this project's Go/Rust caches under its own build directory

- **Decision:** Go uses `build/deps/go/pkg/mod` for `GOMODCACHE` and
  `build/cache/go` for `GOCACHE`. Rust uses `build/deps/cargo` for `CARGO_HOME`
  and `build/rust/target` for `CARGO_TARGET_DIR`. All four are ignored by the
  existing repository-level `/build/` rule; manifests and lockfiles stay tracked.
- **Entry points:** Go provides `go.ps1`/`go.sh`; Rust provides
  `cargo.ps1`/`cargo.sh`. They pass native cache settings only to the launched
  tool and its children and run it from the owning SDK directory. All changes
  stay in the project: no activation, parent-terminal environment/working
  directory changes, or persistent user/system settings. Existing toolchains,
  `PATH`, `RUSTUP_HOME`, and shared caches stay intact. Cargo config/credentials
  in the old Cargo home are not copied implicitly. Plain tool invocations and
  IDEs bypassing these entry points retain their usual defaults. The later
  approved unified-test decision supersedes the initial Python deferral:
  migrated harnesses apply the same child-process cache policy directly.
- **Boundaries:** The initial cache-only change did not authorize downloads;
  SDK dependency acquisition was subsequently authorized in the decision above.
  It does not change C++'s system/vcpkg/project-cache discovery policy. Windows and Linux
  each populate their own ignored `build/`; source synchronization excludes it.

### 2026-09-06: Consume prebuilt OpenSSL and leave its construction external

- **Decision:** Windows and Linux native entry points try system OpenSSL,
  already installed local vcpkg packages, then extracted development packages
  under `build/deps/openssl/<platform>/x64`. Missing compatible packages produce
  requirements and external-preparation guidance only. A specific download
  requires explicit maintainer approval; no suitable binary means an external
  source build, never an automatic OpenSSL build or tool installation.
- **Scope:** Only OpenSSL requires this external build boundary. Boost,
  SQLite amalgamation, and yyjson retain their checksum-locked source fallbacks
  and compilation through the existing C/C++ toolchain. No download is implied
  by a request to build, test, or organize the project.
- **Enforcement:** SDK and probe CMake entry points force
  `VCPKG_MANIFEST_INSTALL=OFF` before `project()`. Both native wrappers validate
  installed packages with the selected profile's compile/link probe; finding
  the vcpkg executable alone is insufficient. Package roots are checked to
  reject mixed system/cache artifacts. Native wrappers disable vcpkg app-local
  deployment; dependent DLLs must be externally available at runtime.
- **Supersession:** Replaces the OpenSSL acquisition/cache portion of the
  2026-09-02 native-build decision below. The retained vcpkg manifest supports
  external preparation. Detailed locations, limitations, and commands are in
  `sdk/cpp/BUILD.md`; actual offline evidence is in `worklog.md`.

### 2026-09-05: Review every bug fix for propagation across languages

- **Maintainer requirement:** Every bug fix must review corresponding logic
  in all languages, shared cores, bindings, and test tooling. Record affected
  paths and the evidence for unaffected paths; source review is not a runtime
  pass. The recurring workflow is specified in `coding.md`, section 15.
- **Catalog example:** The original value validator fix did not cover the
  notification field decoder. Array Replace fields use numeric index order,
  while Map/Value and sparse Patch fields use lexical order. C#/C ABI/Legacy
  inherit defects in the shared C++ core; Go/Rust/Lua require separate review.

### 2026-09-05: Separate C++ Catalog value validation from transport

- **Correctness:** Canonical unique Array indices in `[0,N)` prove complete
  coverage regardless of `std::map` lexical iteration. Replace must separately
  encode numeric argument order; an eleven-entry regression covers the first
  decimal-width transition. Publisher, Subscriber, and checkpoint recovery
  continue to share one validator.
- **Implementation:** Pure validation and Replace argument encoding live in
  private `catalog_value.cpp`/`internal/catalog_value.hpp`. Validation does not
  allocate a temporary name vector. Final arguments remain owned and bounded;
  public APIs, C ABI signatures, Redis storage, and Lua bytes are unchanged.
- **Evidence boundary:** The standalone test project needs only C++23 and
  CMake. It is not a replacement for native/binding/Redis qualification. The
  current offline review and remaining gates are in
  `optimization-review-20260905.md`; no dependency download, commit, or push was
  authorized or performed for this work.

### 2026-09-02: Standardize the Windows and Linux native build entry points

- **Decision:** `sdk/cpp/build.ps1` on Windows x64 and `sdk/cpp/build.sh` on
  Linux x64 are the normal source-build entry points for the C++23 core, C ABI,
  and C++11/14/17 Legacy facade. Go, Rust, and C# remain independent source
  SDKs and are not compiled by these scripts. C# only consumes the shared DLL
  or SO from one of its documented native-library locations.
- **Toolchain boundary:** The scripts detect and validate existing compilers,
  SDKs, CMake, build generators, OpenSSL, and vcpkg, but never install a
  toolchain or package manager. They do not require .NET. Ninja is optional;
  macOS remains unsupported.
- **Dependency boundary (OpenSSL portion superseded by the 2026-09-06
  prebuilt-OpenSSL decision above):** OpenSSL 3.0 or newer comes from the system or an
  existing vcpkg installation and is never built directly by Verdandi. Boost,
  SQLite, and yyjson use compatible system packages or checksum-locked source
  archives. Offline mode permits only verified caches and cannot fall back to
  the network.
- **Output contract:** Every script-owned help, progress, diagnostic, warning,
  error, command, elapsed-time result, and final summary is detailed standard
  English. The two script sources retain detailed Chinese maintainer comments
  during the current review phase; comments never enter console output.
- **Isolation:** Generated files stay under ignored repository-level `build/`
  paths partitioned by platform, compiler, generator, OpenSSL provider,
  dependency policy, profile, and linkage. Immutable verified downloads are
  shared, while extracted sources and compiled objects remain build-tree
  scoped.
- **Evidence:** Windows MSVC/vcpkg and Linux GCC/system-OpenSSL online and cold
  offline configuration passed, as did Release native tests on both platforms.
  The Windows shared result was separately loaded by the existing C# net8/net10
  tests. Invalid explicit vcpkg roots and missing offline archives fail early
  with actionable English diagnostics.

### 2026-09-02: Add an optional generated Go Selector reference path

- **Decision:** Preserve detached `One`/`Any` as the safe default and add a
  separately generated callback-only facade for consumers whose measured
  selection path needs to avoid full Candidate/result copies.
- **Generation boundary:** `verdandi-refgen` emits only read accessors, Data
  setters, mutable-slice cloning, concrete aliases, and a wrapper constructor.
  Application `Encoder`/`Decoder`, wire field names, Redis behavior, and
  selection/weighting logic remain handwritten application contracts.
- **Lifetime boundary:** Generated views expose no raw mutable pointer and are
  borrowed for one synchronous callback. Go cannot statically encode that
  lifetime; callers must not retain or asynchronously use them. Editors and
  Selection commits remain runtime token-fenced.
- **Transaction boundary:** `WithOne` returns found/error and `WithAny` returns
  count/error. Only edits belonging to final returned Selections are encoded
  and atomically committed to the existing local overlay. Unselected edits and
  every error/cancellation/invalid-result path roll back.
- **Reason:** Selector weighting is intentionally local and mutable, while
  detached results add measurable allocations when the caller only needs one
  copied route value. The optional facade improves this path without weakening
  the ordinary API or moving business policy into the SDK.

### 2026-09-02: Withdraw generic Campaign and Leader election

- **Decision:** Remove generic Campaign readiness, Leader election, LeaderTerm,
  election keys, election Lua, Sentinel fencing adapters, and Leader SDK APIs
  from every current and stable Verdandi target. They will not be implemented.
- **Boundary:** Redis Sentinel primary promotion remains required backend
  recovery. It selects the current Redis primary and is unrelated to an
  application-level Leader API.
- **Compatibility:** Registration `@version` remains an application-defined
  positive integer transported by Register/Update. Verdandi does not compare it
  or interpret it as an election priority.
- **Supersession:** This decision supersedes every earlier Campaign/Leader,
  readiness-token, election-version, exact-term, retirement, and durable-fence
  decision in this record. Those entries remain only as historical rationale.
- **Release effect:** Leader is no longer a `1.0.0` requirement. The stable
  release still requires its remaining protocol, package/ABI, security,
  documentation, and exact-final-source qualification gates.
- **Reason:** The maintainer does not plan to provide generic Leader election;
  retaining an unimplemented design would expand the public contract and
  release burden without a consuming requirement.

### 2026-09-01: Release the implemented preview as 0.1.0

- **Decision:** Use `0.1.0` for the implemented Registration, Selector,
  Catalog, configuration, and binding surfaces. It permits distributed
  development and controlled service integration without a production or
  stable compatibility promise.
- **Partially superseded 2026-09-02:** `1.0.0` remains reserved for the complete
  stable contract and concise standard English production-source comments, but
  Leader election is no longer part of that contract.
- **Decision:** Move the canonical repository and package identity to
  `eosforge/verdandi`. Preserve historical result files as evidence from their
  original commit rather than rewriting embedded old import paths.
- **Reason:** The implemented subset has passed broad regression and two exact
  twelve-hour fault campaigns, while a SemVer `0.x` release accurately signals
  that the full stable scope and compatibility promise remain unfinished.

### 2026-08-21: Create Verdandi as an independent repository

- **Partially superseded:** The independent-repository boundary remains; its
  canonical GitHub identity moved to `eosforge/verdandi` on 2026-09-01.
- **Former decision:** Place the reusable coordination system in
  `LaconisIves/verdandi`, alongside rather than inside Bifrost or Hermes.
- **Reason:** It has an independent protocol, release lifecycle, test matrix,
  security boundary, and multiple consuming applications.

### 2026-08-21: Keep the project language-neutral

- **Decision:** Define Verdandi through schemas, protocol behavior, error
  taxonomy, Lua semantics, and conformance vectors. Go and Rust are the first
  SDKs, while future conformant languages remain possible.
- **Reason:** Coordination is a system contract rather than a property of one
  runtime or package manager.

### 2026-08-21: Use one repository with isolated SDK package roots

- **Decision:** Keep shared protocol artifacts and all official SDKs in one
  repository, with each language rooted under `sdk/<language>` and no language
  workspace at the repository root.
- **Reason:** Protocol changes, Lua behavior, test vectors, and cross-language
  qualification must evolve atomically while each package manager retains a
  clean boundary.

### 2026-08-21: Support Standalone and Sentinel, not Redis Cluster

- **Decision:** Use one public protocol and SDK API for a fixed Redis primary
  and a Sentinel-resolved primary. Exclude Redis Cluster and active/active
  merging from `1.0.0`.
- **Reason:** Sentinel supplies automatic primary/replica failover for the
  single-primary performance model without introducing cross-slot or
  multi-primary correctness problems.

### 2026-08-21: Use Pub/Sub only as a wake signal

- **Superseded:** Refined on 2026-08-22 by the short-term synchronization-history
  decision below.
- **Former decision:** Store recoverable state in durable keys and immutable
  publications; use Pub/Sub only to reduce detection latency.
- **Reason:** Pub/Sub can be missed during disconnects and cannot prove that a
  consumer received or activated a revision. The refined design still relies
  on Redis state for recovery but permits contiguous Registry/Catalog events to
  update the live local view directly.

### 2026-08-21: Separate coordination data classes

- **Partially superseded 2026-09-02:** Generic leadership was removed from the
  project scope; the remaining data-class separation still applies.
- **Decision:** Model Registration, Registry, load, Catalog KV,
  desired configuration, acknowledgements, and commands separately.
- **Reason:** Their owners, rates, security permissions, retention, and failure
  semantics differ materially.

### 2026-08-21: Avoid a fixed fleet-size ceiling

- **Decision:** Do not encode 500 or another fleet size as a protocol maximum.
  Enforce bounded local resources and qualify the later accepted 500 live Proxy
  updates/s plus 10 Catalog mutations/s workload; partition larger deployments
  explicitly.
- **Reason:** Product scale should follow measured deployment capacity while
  every individual operation remains safe and bounded.

### 2026-08-21: Base discovery and leadership on the Hermes Redis invariants (partially superseded)

- **Superseded 2026-09-02:** The readiness, ownership, retirement, and term
  clauses were withdrawn with generic Campaign/Leader election. Discovery keeps
  the following relevant decision.
- **Decision:** Carry forward Hermes's proven subscribe-before-snapshot,
  revision-gap recovery, immutable local views, and lease deadlines. Replace
  Hermes's bounded all-in-one snapshot scans with
  paginated indexes and bounded event buffering between explicit barriers so
  Verdandi does not inherit its 100-instance boundary.
- **Reason:** The state-machine and failure behavior have real implementation
  and soak evidence, while Verdandi requires a language-neutral and
  fleet-size-neutral storage contract.

### 2026-08-21: Add persistent Catalog KV synchronization

- **Superseded 2026-08-24:** Catalog remains persistent and revisioned, but the
  multi-record namespace and barrier-delimited Pub/Sub model were replaced by
  one raw Hash Value plus ordered Stream replay.
- **Decision:** Catalog is a persistent revisioned key/value namespace with
  bounded atomic mutation, explicit deletion, paginated authoritative load,
  barrier-delimited event reconciliation, and an immutable local mirror that
  exposes whether it is synchronized.
- **Reason:** Controllers need fine-grained shared state without republishing a
  complete immutable document, while desired configuration retains its
  separate immutable snapshot and activation contract.

### 2026-08-21: Store each Registration as one Redis Hash

- **Partially superseded:** Independent Hash storage remains accepted; the
  collection terminology and exact keys follow the later Zone/Type decision.

- **Decision:** Call one Node's leased registration record a `Registration` and
  its service/partition collection a `Registry`. Store each Registration under
  its own Redis Hash key, use key TTL for its lease, and permit only
  protocol-owned typed partial field updates.
- **Reason:** `Registry` conventionally names the collection rather than one
  entry. Independent Hash keys preserve field-level updates, narrow ACL patterns,
  bounded per-record reads, and avoid one global hot Hash; the expected record
  count does not justify trading those properties for modest key-memory savings.

### 2026-08-21: Use Redis-native field contracts

- **Decision:** Do not wrap ordinary coordination state in one
  CDDL/deterministic-CBOR envelope. Define exact Redis data types, Hash fields,
  scalar encodings, service capabilities, ownership, and Lua actions. SDKs retain the
  known fields they need and ignore unknown optional fields.
- **Reason:** ACLs and protocol-owned mutation paths define the supported write
  boundary. A principal deliberately granted raw write access can bypass any
  SDK contract, and a canonical binary envelope would not prevent that action.

### 2026-08-21: Require Redis 8 Hash field TTL for readiness (superseded)

- **Superseded 2026-09-02:** Campaign readiness was withdrawn. Redis 8 remains
  the baseline for implemented Registry Hash-field TTL.
- **Former decision:** Set Redis Open Source 8.0.0 or later in a qualified Redis 8
  line as the Alpha baseline. Store one Campaign readiness token per
  independently expiring Hash field.
- **Reason:** This carries forward Hermes's measured lower-memory readiness
  layout while Verdandi removes Hermes's 100-candidate boundary. SDK-side fixed
  integer comparison needs no Redis version-order or expiry index.

### 2026-08-21: Support atomic Catalog batches and optional TTL

- **Superseded:** CAS-only and logical-expiry cleanup were replaced on
  2026-08-22 by atomic last-write-wins batches and Redis key/field TTL.
- **Former decision:** A Catalog mutation was a bounded all-or-nothing CAS batch
  with logical expiry and journaled tombstone cleanup.
- **Reason:** Controllers need related key changes to become visible together,
  while the later decision removes concurrency and cleanup machinery that the
  accepted ACL/LWW and TTL semantics do not require.

### 2026-08-21: Start SDKs at 1.0.0

- **Superseded:** Replaced on 2026-09-01 by the accepted `0.1.0` Alpha preview
  and reserved full-scope `1.0.0` decision above.
- **Former decision:** The first Go and Rust SDK version was to be `1.0.0`, with
  fixed unpublished source metadata during development.
- **Reason for replacement:** The implemented subset is useful for controlled
  integration, while `0.x` communicates its unfinished stable scope honestly.

### 2026-08-21: Use one per-start Registration UUID

- **Partially superseded:** The UUID lifecycle remains accepted; the key shape
  is replaced by the later Zone/Type Registration decision.

- **Decision:** The SDK generates a fresh UUID on every service process start
  and stores the Registration at
  `verdandi:registration:<namespace>:<uuid>`. A crash relies on TTL expiry;
  graceful shutdown atomically removes that exact Registration and updates the
  Registry membership and revision. Do not require stable `node_id` plus
  `generation_id`.
- **Reason:** The UUID already is the process-instance identity. Independent
  keys prevent one start from overwriting another and remove a redundant
  generation-replacement state machine.

### 2026-08-21: Keep writer terms separate from process identity

- **Superseded:** Replaced on 2026-08-22 by the following Redis-revision
  decision.
- **Former decision:** `publisher_epoch` was renamed `writer_term` and treated
  as one continuous grant to mutate a Publisher target.
- **Replacement reason:** The maintainer determined that authoritative rewrites,
  Redis revisions, and wake signals provide sufficient synchronization without
  a separate publication authority identity.

### 2026-08-22: Use Redis revisions instead of Publisher write terms

- **Partially superseded 2026-08-24:** Catalog still has no writer term, but its
  mutation appends a Stream entry instead of emitting a Pub/Sub wake.
- **Decision:** Publisher, desired-state, and Catalog records carry no
  write-authority term. Each mutable scope keeps one Redis-owned monotonic
  revision that advances atomically with every accepted mutation and does not
  reset on Publisher restart. Every mutation emits a wake signal.
- **Reason:** The authoritative rewrite and revision already provide ordering.
  The later 2026-09-02 decision also removed generic Leader election entirely.

### 2026-08-22: Fix protocol 1.0 without capability negotiation

- **Decision:** The first protocol version is `1.0`. Every behavior defined by
  `1.0` is mandatory, and records carry no protocol-capability list or
  negotiation. Registration `service_capabilities` are application metadata.
- **Reason:** There is no concrete optional protocol feature in the first
  release, so capability negotiation would add a contract with no use.

### 2026-08-21: Use readable SDK-owned Redis keys

- **Partially superseded:** The readable unversioned prefix remains accepted;
  Registration and Registry shapes are replaced by the later Zone/Type
  decision.

- **Decision:** Use the literal `verdandi:` prefix without a key-space version.
  Put the data class before namespace and fix the Registration shape as
  `verdandi:registration:<namespace>:<uuid>`. SDKs construct and mutate all
  protocol keys; consuming applications do not receive Redis handles.
- **Reason:** `vd` and `1` were only a product abbreviation and a proposed
  layout-major marker. SDK ownership keeps each Hash mutation atomic with its
  Registry membership, revision, expiry, and event publication.

### 2026-08-22: Keep Redis keys forward-compatible

- **Decision:** Existing key names, Redis data types, and meanings do not change
  in place. Compatible protocol evolution adds optional fields or new keys. An
  unavoidable incompatible storage change requires a new key family and an
  explicit migration.
- **Reason:** Verdandi uses one readable unversioned key prefix, so forward
  compatibility must be an invariant rather than an encoded prefix version.

### 2026-08-21: Trust Redis ACLs instead of artifact signatures

- **Decision:** Do not use end-to-end signatures for desired state, commands,
  Catalog, Registration, or acknowledgements. Use Redis authentication, ACLs,
  and TLS at network trust boundaries. Retain hashes only for content assembly
  and accidental-corruption checks.
- **Reason:** A principal intentionally granted raw Redis write permission can
  bypass either SDK validation or stored signature metadata. Verdandi validates
  malformed state for process safety but does not claim to defend against that
  trusted principal.

### 2026-08-21: Keep internal Markdown on local-only alpha (superseded)

- **Original decision:** Allow only `README.md` among Markdown files on `main`
  for now. Keep development memory, version requirements, protocol drafts, work
  state, and coding rules on local-only `alpha` and block that branch from push.
- **Reason:** The maintainer requires internal development context to remain
  local while the public repository contains only deliberately released
  material.
- **Superseded on 2026-08-31:** Internal Markdown remains excluded from `main`,
  but the maintainer explicitly authorized one complete `alpha` source-freeze
  commit and push. Later `alpha` pushes again require explicit current-task
  authorization.

### 2026-08-22: Fix namespace and Registration UUID forms

- **Partially superseded:** The UUID form remains accepted; `namespace_id` is
  renamed `zone` by the later Registration synchronization decision.

- **Decision:** The application supplies `namespace_id` through SDK
  configuration and the SDK validates case-sensitive `[A-Za-z]{1,32}`. The SDK
  generates every per-start Registration identity as exactly 32 lowercase
  hexadecimal characters without separators.
- **Reason:** Namespace is a shared administrative choice and cannot be
  independently generated by every client; Registration UUID is process-local
  and must be collision-resistant and key-safe.

### 2026-08-22: Partition Registration Node, Meta, and Data fields

- **Superseded:** Replaced by the later Meta/Attr/Data field decision.

- **Decision:** Each Registration is an independent Redis Hash exposed as
  `Node`, `Meta`, and `Data`. SDK-owned Node leaves use `@name`, descriptive
  Meta leaves use `.name`, and user Data leaves are unprefixed and may not begin
  with either marker. Leaves are expanded as top-level fields, so typed SDK
  patches may atomically set and delete selected fields. Catalog keeps its
  independent reserved-plus-value Hash contract.
- **Reason:** This presents normal nested language structures while preserving
  partial Redis updates and disjoint ownership without whole-object JSON
  rewrites.

### 2026-08-22: Fix integer election versions and SDK-side comparison (superseded)

- **Superseded 2026-09-02:** Generic Campaign/Leader election was withdrawn.
  The earlier 2026-08-24 replacement below is retained only as history.
- **Superseded 2026-08-24:** The fixed positive-integer SDK comparison remains,
  but election identity and version now belong to an independent Campaign, not
  Registration. The later strict Leader decisions remove version mutation and
  any version-revision field.
- **Former decision:** Registration version is a mutable positive safe integer. Every
  SDK uses the same numeric comparison and larger values are preferred; equal
  values remain first-successful-claim wins. A dedicated version change advances
  a version revision, retires an owned term, and republishes readiness. Do not
  use an application comparator, comparator contract ID, or Redis-order index.
- **Reason:** One numeric contract removes cross-language comparison ambiguity
  while retaining the field-TTL readiness layout and no candidate-count limit.

### 2026-08-22: Fix remaining identifier and Catalog-key forms

- **Partially superseded 2026-08-24:** The direct Catalog identifier form
  remains; the opaque business-key/hash-token dimension was removed when one
  Catalog became one raw Value.
- **Decision:** Case-sensitive partition, service, and Catalog
  IDs use `[A-Za-z][A-Za-z0-9_.-]{0,63}`. An opaque Catalog business key is 1
  through 1,024 bytes; Redis keys use its lowercase unpadded base32hex SHA-256
  token and records validate the complete original key.
- **Reason:** Direct text segments exclude Redis separators, ACL glob syntax,
  and Unicode normalization differences, while fixed hashed tokens keep opaque
  Catalog keys bounded without discarding their exact identity.

### 2026-08-22: Compare application-defined election versions in the SDK

- **Superseded:** The later fixed-integer decision removes application-defined
  codecs and comparators while retaining SDK-side comparison.
- **Former decision:** Candidate version type, canonical encoding, and
  comparison were supplied through SDK configuration. Redis validated
  readiness, liveness, and exact Leader ownership but did not order versions.
- **Reason for change:** One protocol-wide positive integer preserves the
  index-free scalable design without cross-language comparison ambiguity.

### 2026-08-22: Use TTL-backed membership instead of expiry indexes (partially superseded)

- **Partially superseded 2026-09-02:** Campaign readiness was withdrawn;
  Registry membership remains current. Catalog no longer has membership fields
  or TTL.
- **Decision:** Registry membership is stored in Redis 8 Hash fields with
  independent TTL. The former Catalog proposal used the same field-
  TTL mechanism when its record has TTL. Key/field TTL determines liveness and
  natural cleanup; no separate stale expiry/version index is required.
- **Reason:** It removes redundant index cleanup while retaining bounded
  pagination and partial field mutation.

### 2026-08-22: Use atomic last-write-wins Catalog batches

- **Superseded 2026-08-24:** Redis-order LWW remains, but Catalog is now one
  Value and SDK-split patches are independently atomic; there are no records,
  multi-record batches, membership fields, or TTL.
- **Decision:** Multiple ACL-authorized Catalog writers may issue bounded atomic
  Replace/Patch/Delete batches. For overlapping data, the Lua operation executed
  later on the current Redis primary is current. Protocol `1.0` does not require
  compare-and-set. Records may be persistent or use Redis key TTL with matching
  membership-field TTL.
- **Reason:** ACL decides authorization; Redis serialization and revisions
  already define update order. CAS and a logical-expiry sweeper are unnecessary
  for the accepted semantics.

### 2026-08-22: Keep only short-term synchronization history in Redis

- **Superseded:** Replaced later on 2026-08-22 by journal-free synchronization.
- **Former decision:** Pub/Sub mutation events were the normal incremental
  Registry and Catalog path, with a bounded short-term journal filling
  bootstrap and disconnect gaps.
- **Replacement reason:** Subscribed two-barrier scans can reconcile concurrent
  mutations from current data and buffered events. Disconnect or any event gap
  invalidates the generation and restarts a complete scan, so Redis need not
  retain synchronization history.

### 2026-08-22: Use journal-free subscribe/scan/barrier synchronization

- **Superseded for Catalog 2026-08-24:** Registry uses the later
  per-Registration scan/PING design; Catalog now deliberately retains a bounded
  Redis Stream controlled by `@floor_revision`.

- **Decision:** Redis stores only current Registry and Catalog state. A mirror
  acknowledges a dedicated Pub/Sub subscription, observes a nonce-bearing
  start barrier, scans paginated membership and current records while buffering
  later mutation events, then observes an end barrier and verifies contiguous
  scope revisions before publishing an immutable view. A disconnect, event
  gap, changed connection generation, barrier timeout, malformed event, or
  buffer overflow abandons the candidate and restarts the complete procedure.
- **Reason:** Per-record revisions discard stale scan results, while the two
  barriers and contiguous event revisions also cover creations and deletions
  that occur during the scan. Pub/Sub is not replayable, so a failed generation
  cannot be repaired incrementally without retained history.

### 2026-08-22: Group Registrations through Registry membership

- **Superseded:** Replaced by the later
  `registration:<zone>:<type>:<uuid>` and `registry:<zone>:<type>` layout.

- **Decision:** Keep each record key as
  `verdandi:registration:<namespace>:<uuid>`. A Registry membership Hash is
  scoped by namespace, partition, and service and maps Registration UUID fields
  to leased membership. Partition and service remain reserved fields in the
  Registration for validation and inspection.
- **Reason:** Adding partition or service to every Registration key duplicates
  classification metadata and does not provide bounded collection enumeration.
  The membership Hash is the actual paginated grouping index and avoids
  database-wide `SCAN MATCH` behavior.

### 2026-08-22: Bound Catalog records without limiting population

- **Superseded 2026-08-24:** Catalog is one complete Value with configurable
  total bytes and patch/page limits. The record population and TTL model no
  longer exists.
- **Decision:** A Catalog record may encode at most 4 MiB. Records are
  persistent by default or expire at an explicit absolute deadline no later
  than `9999-12-31T23:59:59.999Z`; persistent is exact infinity. SDK deployment
  configuration may impose lower record, field, batch, page, event-buffer, and
  mirror-memory limits, but the protocol defines no Catalog-record count cap.
- **Reason:** A high absolute deadline supports effectively permanent finite
  records without abusing a language duration type, while configured resource
  budgets protect each operation independently of total population.

### 2026-08-22: Defer Commands and qualify the initial Redis drivers

- **Decision:** Command delivery is not part of SDK `1.0.0`. Go uses
  `go-redis/v9`. Rust provisionally prefers `fred` behind a private adapter,
  subject to Standalone, Sentinel, separate-authentication, subscription-gap,
  scripting, Hash-field-expiry, and large-record qualification; `redis-rs` is
  the fallback if that qualification fails.
- **Reason:** No current imperative use case justifies freezing a Command
  protocol. `fred` more directly matches the async subscription and reconnect
  workload, but the public SDK contract must not depend on one driver's API.
- **Qualification update (2026-08-23):** `fred` passed the implemented
  Standalone Register/Selector, explicit reconnect, bounded per-UUID event
  coalescing, PING/PONG, Go/Rust interoperability, and real Redis 8.8 Sentinel
  matrix with independently authenticated Redis and Sentinel connections.
  `redis-rs` remains only the fallback for a later blocker outside this slice.

### 2026-08-22: Accept configurable resource and qualification gates

- **Decision:** Use the protocol maxima and initial configured values recorded
  in PRT-009, with lower deployment configuration permitted. Accept configurable
  synchronization buffers/timeouts, the focused `fred` qualification with
  `redis-rs` fallback, and p95/p99 latency plus failover/recovery resource gates
  for the 500-Proxy workload.
- **Reason:** Explicit per-operation and local-memory bounds protect every SDK
  without creating a total-population ceiling. Driver and performance evidence
  remain engineering release gates rather than unresolved product semantics.

### 2026-08-22: Scope ACLs by role and Zone

- **Decision:** Redis ACL credentials grant combinations of Node, Publisher,
  Selector, Catalog Mirror, and Administrator roles within a Zone.
  Do not create a credential policy per Registration UUID.
- **Reason:** Per-start UUIDs change on every restart; role/Zone policy is
  stable and matches the chosen ACL trust boundary.

### 2026-08-22: Qualify the accepted initial workload

- **Decision:** The first capacity suite runs separate 500-live-Proxy profiles,
  each renewing or updating once per second, plus 10 Catalog mutations per
  second and recovery of that population. These rates are qualification evidence, never protocol
  count limits.
- **Reason:** The workload is concrete enough to size pages, event buffers, queues,
  and Redis connections without limiting larger deployments by schema.

### 2026-08-22: Use the MIT License

- **Decision:** Publish Verdandi under the MIT License with SPDX identifier
  `MIT` and `Copyright (c) 2026 LaconisIves`.
- **Reason:** The maintainer selected MIT for the first public release.

### 2026-08-21: Require explicit commit and push instructions

- **Decision:** Editing, generation, testing, and completion requests do not
  authorize a Git commit or push. Each commit or push requires a current,
  explicit maintainer instruction.
- **Reason:** Repository history and publication remain deliberate maintainer
  decisions.

### 2026-08-22: Use per-Registration update/delete synchronization

- **Superseded:** Replaced on 2026-08-22 by the aligned four-kind lifecycle and
  content-revision decision below.
- **Decision:** Rename the Registration administrative/grouping coordinates to
  `zone` and `type`. Store one process at
  `verdandi:registration:<zone>:<type>:<uuid>` and use
  `verdandi:registry:<zone>:<type>` as both membership Hash and Pub/Sub channel.
  Registration Meta is exactly `@uuid`, `@revision`, `@timestamp`, `@ttl`, and
  `@version`; immutable Attr is `.name`, and mutable fixed-structure Data is
  unprefixed. Do not store `@expire`.
- **Decision:** The one SDK process owns a positive per-UUID revision and one
  serialized writer. Redis and Pub/Sub expose only `update` and `delete`. A
  complete initial/recovery/reset update uses `previous=0`; an incremental
  update carries its actual base and only changed values; an empty update is the
  heartbeat. Every accepted update uses Redis `TIME`, refreshes identical key
  and membership-field deadlines, and publishes changed values inline.
- **Decision:** A Selector uses one connection-generation RedisClock, subscribes
  before paginated membership/record reads, and fences the completed scan with
  `PING <nonce>`/ordered PONG on the subscription connection. Per-UUID gaps use
  bounded targeted fetch plus another PING fence. Pending changes coalesce to
  at most one logical entry per UUID. Natural TTL expiry is removed locally from
  `timestamp + ttl`; normal operation performs no `PTTL`/`HPTTL` read.
- **Decision:** Registration Pub/Sub uses MessagePack arrays of alternating
  string keys and values. `&` is event control, `@` is Meta, `.` is Attr, and
  unprefixed names are Data. Results, statuses, and event kinds are strings;
  numeric fields remain integers. Lua receives flat RESP arguments and never
  parses JSON or application values.
- **Supersedes:** The Registration portions of the earlier namespace-only key,
  Node/Meta/Data, Registry-wide revision, two-barrier Registry synchronization,
  and record-plus-`PTTL` event rules. Its former Catalog-barrier clause is itself
  superseded by the later raw Value plus ordered Stream decision.
- **Reason:** One writer owns each UUID, so a global Registry sequence and
  application barrier event are unnecessary. Full reset events cover creation
  and recovery, incremental events avoid routine pulls, Redis timestamps retain
  precise TTL semantics, and the ordered subscription PONG closes the scan race
  without adding a third Registration event state.

### 2026-08-22: Align the four Registration lifecycle kinds

- **Retained:** The subsequently withdrawn Campaign/Leader proposal never
  altered these Registration lifecycle kinds or Version/Data update behavior.
- **Decision:** SDK operations, Lua mutations, and Pub/Sub events use the same
  four string kinds: `register`, `update`, `renew`, and `unregister`. Register
  carries complete Meta/Attr/Data; Update carries UUID, Redis timestamp, the
  next content revision, and a non-empty Data/version patch; Renew carries UUID,
  Redis timestamp, and the unchanged revision; Unregister events carry only the
  terminal UUID.
- **Decision:** Registration revision is an SDK-owned positive content version,
  not a lease-operation sequence. It advances only when Data or mutable
  `@version` changes. Attr and TTL are immutable for the UUID lifetime. Lua
  obtains timestamp from Redis for Register, Update, and Renew, refreshes the
  identical key/field deadline, and leaves membership revision unchanged for
  Renew. Revision and timestamp remain separate stored and transmitted values.
- **Decision:** A graceful Unregister command and event contain only UUID. Close
  first waits behind that UUID's admitted coordinator requests, sends only on its current healthy
  connection generation, never reconnects to issue delayed cleanup, and never
  reuses that terminal UUID. TTL expiry and fenced absence are non-explicit removal: they leave selection
  immediately but may retain payload in a configured time/byte recovery cache;
  explicit Unregister purges it.
- **Reason:** Explicit lifecycle kinds align producer and Selector state
  machines. Immutable TTL makes same-revision renewals commutative by maximum
  Redis timestamp, while content revision lets reconnect reuse unchanged
  payload without treating every heartbeat as a data change.

### 2026-08-22: Initially keep one Registration script and no Selector snapshot script

- **Superseded 2026-08-24:** The single executable-file packaging is replaced
  by four generated operation-specific scripts. The shared atomic invariant is
  still maintained once in source fragments, and every mutation still executes
  exactly one Lua program.
- **Former decision:** Implement `register`, `update`, `renew`, and `unregister`
  in one bounded Registration Lua program because they share one Hash/
  membership/TTL/event atomic invariant.
- **Decision:** Selector performs no Redis mutation and therefore owns no Lua
  program. It uses subscribe acknowledgement, paginated `HSCAN`, pipelined
  per-record reads, event reconciliation, and an ordered PING/PONG fence.
- **Reason:** A Registry-wide Selector Lua snapshot would block Redis for work
  proportional to total membership, duplicate the four-kind reconciliation
  state machine, and reintroduce the fleet-size boundary removed from Hermes.

### 2026-08-23: Pin shared Registration limits from Redis

- **Superseded:** Replaced later on 2026-08-23 by live Redis-backed defaults and
  refresh because the maintainer requires backend changes after startup.
- **Former decision:** The Administrator provisions one non-expiring
  `verdandi:config:<zone>` Hash containing the active Registration field-count,
  field-name-byte, field-value-byte, and total-byte limits. Connectivity,
  credentials, TLS, timeouts, retry behavior, concurrency, and local buffer
  limits remain local because the Client needs them before or independently of
  Redis.
- **Former decision:** A Client reads and validates the recognized Zone configuration
  before Registration or Selector initialization completes, pins the first
  valid value set for its lifetime, and rereads it on every resolved-primary
  generation. Missing, malformed, incompatible, or changed values keep that
  generation out of `Ready`; protocol `1.0` has no configuration hot reload.
- **Retained decision:** Each published Registration's worker keeps its complete
  encoded desired state and confirmation status in bounded process memory and validates
  the projected Attr/Data field count, every field, and total stored bytes before
  advancing revision or calling Redis. Lua does not repeat request, schema,
  immutable-field, or capacity validation, and a new-revision Update performs
  no `HGETALL` capacity scan.
- **Former reason:** One pinned Redis value set keeps Go, Rust, and future SDKs aligned,
  detects configuration rollback after primary failover, and removes record-
  size-proportional work from the steady-state Update path without adding a
  mutable configuration protocol.

### 2026-08-23: Seed and refresh administratively mutable Zone limits

- **Decision:** Client bootstrap fills each missing recognized field in
  `verdandi:config:<zone>` with `HSETNX`, using identical Go/Rust defaults:
  Attr fields 16, Data fields 32, field names 64 bytes, Attr values 128 bytes,
  Data values 128 bytes, and complete Registration 16 KiB. The Hash has no TTL.
- **Decision:** An ACL-authorized backend may later change related fields with
  one multi-field `HSET`. Clients retain one complete last-valid local snapshot.
  Register refreshes immediately; the first published Registration starts one
  Client-shared poller, and the last stops it. Explicit refresh remains
  available without a Registration. Invalid refresh is reported without
  publishing it. There is no configuration Pub/Sub or revision in protocol
  `1.0`.
- **Decision:** New Registration content writes use the latest local limits;
  Renew remains legal after a reduction, and Selector uses immutable protocol
  ceilings so existing records remain discoverable. The common Update path
  reads neither configuration nor the complete Registration Hash.
- **Reason:** Redis remains the editable deployment-policy source while the hot
  path stays local and constant with patch size. Protocol ceilings preserve
  cross-language safety; lower defaults remain operational choices rather than
  permanent wire limits.

### 2026-08-24: Complete and qualify Registration/Selector

- **Decision:** `verdandi:config:<zone>` now includes
  `configuration_refresh_ms`. It defaults to 30,000 ms and accepts 1,000 through
  86,400,000 ms. Go and Rust immediately load one complete bootstrap snapshot.
  While at least one Registration is published, one Client-shared task waits
  the interval with plus or minus ten percent jitter. A successful refresh
  controls the next wait; an invalid refresh retains the prior complete policy
  and interval. The SDK exposes immediate refresh without requiring a live
  Registration.
- **Decision:** Selector maintains a distinct non-selectable retained view for
  natural TTL expiry and fenced authoritative absence. Active expiry is
  `timestamp + ttl`; retained expiry is `timestamp + 2*ttl`. Explicit
  Unregister purges both views. A valid same-UUID event or fetched record may
  reactivate retained payload. The independent local byte budget is 64 MiB by
  default, zero disables it, 1 GiB is the accepted maximum, and earliest
  retained deadline is evicted first.
- **Decision:** Same-revision Registry scans reuse active or retained payload
  and fetch only `@revision` plus `@timestamp`; new or changed revisions fetch
  the complete Registration. This optimization does not weaken the subscribed
  PING/PONG fence or targeted repair.
- **Superseded 2026-08-25:** Registration code generation and generic `Schema`
  are removed from the SDK. Go application Attr/Data values directly implement
  value-receiver `FieldEncoder` and pointer-receiver `FieldDecoder`; hidden
  pointer constraints let callers name only `NewRegistration[Attr, Data]` and
  `NewSelector[Attr, Data]`. Rust application types implement `FieldValue` and
  use `Registration::<Attr, Data>::new` plus
  `Selector::<Attr, Data>::new`. Raw `Fields` implements the same interface in
  both languages. The SDK owns no application business-logic generator.
- **Decision:** Registration construction is local and allocates the process
  UUID without Redis I/O or a renewal worker. `Register` is the explicit
  readiness boundary that publishes complete Attr/Data and starts renewal.
  Typed Update accepts complete desired Data and transmits only changed encoded
  fields; Version remains mutable through `SetVersion` or atomic
  `UpdateContent`.
- **Decision:** Typed Selector `One` and `Any` execute synchronous, local-only,
  serialized policy transactions over a borrowed active view. Local Data
  mutation is allowed only through the transaction helper, commits only after a
  valid non-empty selection, and rolls back on callback error, timeout, foreign
  or duplicate candidates. Renew preserves local predictions. On later content
  revisions, only remote Data fields whose bytes changed replace corresponding
  predicted fields; unchanged predicted fields remain. Unregister/expiry removes
  the prediction. This is soft process-local load prediction, not distributed
  capacity reservation.
- **Implementation evidence:** the custom bounded Go MessagePack decoder reduced
  paired median decode time by 41.72 percent and allocations from 26 to 12.
  Go/Rust passed authenticated Redis 8.8 integration, Go real-Redis race,
  60-second/15,570,690-case fuzz, minimum Go 1.24/Rust 1.85, bidirectional
  interop, two five-minute phases per language, eight Selectors, 5,000-record
  synchronization, and the two-promotion Sentinel fault matrix. Machine-readable
  results live under `testkit/results/` and the assessment is in
  `test-results.md`.
- **Scope boundary:** this completes the current Registration/Selector slice,
  not SDK `1.0.0`. At this checkpoint Catalog, Campaign/Leader, desired state,
  acknowledgements, TLS, managed Redis, and their qualification remained later
  work; the 2026-09-02 decision subsequently withdrew Campaign/Leader. No
  commit, tag, package release, or push exists.

### 2026-08-24: Keep Registration recovery state volatile

- **Decision:** Each published Registration owns one bounded desired-state cache
  and confirmation status for its UUID in process memory. The SDK does not persist
  Registration content, UUIDs, or a replay log to local files, a local database,
  or a WAL. The cache survives Redis reconnect only because the same process
  remains alive; process restart discards it and generates a new UUID. Selector
  active and retained views are likewise process-memory-only.
- **Reason:** The Registration identity is one process start and Redis TTL owns
  crash cleanup. Disk recovery could resurrect a dead process identity, while
  high-frequency writes would add serialization, locking, filesystem I/O, and
  write amplification. A bounded RAM cache still enables no-op suppression,
  projected-limit validation, partial updates, and failover repair without a
  Redis read before every Update. Durable history remains the responsibility of
  the separate optional statistics/audit synchronizer.

### 2026-08-24: Generate operation-specific Registration Lua glue

- **Partially superseded 2026-08-24:** Four generated executables and one
  explicit manifest remain accepted. The later line audit removes shared
  runtime helper functions and keeps only fragments whose sharing has no
  per-call closure cost.

- **Decision:** Maintain common Redis-state, clock, reply, and publication
  fragments under one explicit manifest. Deterministically generate
  `register.lua`, `update.lua`, `renew.lua`, and `unregister.lua`, plus
  byte-identical Go and Rust embedded copies. Generated outputs are never
  edited directly.
- **Decision:** Clients load four SHAs at bootstrap and select exactly one for
  each lifecycle mutation. The selected SHA is the operation dispatch and fixes
  the private protocol-v1 argument layout. `NOSCRIPT` reloads only the selected
  script. Selector remains direct-command subscribe/scan/PING logic and owns no
  Lua snapshot program.
- **Decision:** SDKs exclusively validate request shape, protocol/kind,
  identifiers, canonical scalars, reserved names, immutable Attr/TTL, field
  structure and capacities, complete projected state, and no-op updates before
  Redis I/O. Lua retains only conditions that depend on the current Redis state
  or must execute atomically with it: presence, revision transition, stored
  scalar usability, Redis time and safe deadline derivation, Hash/membership
  writes, matched expiry, reply, and Pub/Sub publication. Direct raw script use
  is outside the supported ACL trust boundary.
- **Reason:** Lua is a bounded atomic glue layer, not a second SDK parser or
  schema validator. Four generated executables preserve narrow review and
  cache-reload boundaries without duplicating logic across languages.
- **Evidence:** Removing duplicate Lua validation reduced the four generated
  bodies from 44,133 to 19,948 UTF-8 source bytes. An eleven-trial isolated
  Redis 8.8 comparison found the split itself effectively neutral after that
  reduction: specialized Update was 15.67 versus 15.66 microseconds and Renew
  was 14.31 versus 14.51 microseconds for the test-only combined shape. The
  raw-bypass fixture accepts oversized fields in Lua while both SDK suites
  reject them before Redis I/O, proving the intended boundary.

### 2026-08-24: Freeze and optimize the positional Registration Lua ABI

- **Retained:** The subsequently withdrawn Campaign/Leader proposal never
  altered any Registration positional ABI or generated script SHA.
- **Decision:** Fixed request controls are value-only positional slots. Register
  receives UUID, revision, TTL, and version before complete named Attr/Data
  pairs. Update receives UUID, revision, and version-or-empty before named Data
  patch pairs. Renew receives UUID/revision; Unregister receives UUID. The
  selected SHA already identifies `v1` and operation, so requests do not repeat
  `&protocol`, `&kind`, or fixed Meta names. Replies and Pub/Sub events remain
  named alternating key/value arrays. An incompatible future layout uses a new
  script/SHA.
- **Decision:** Use direct `ARGV` Hash writes, numeric Redis-generated time and
  deadline arguments, multiple-return state reads, inlined fixed success and
  publication blocks, and Redis 8 `HSETEX PXAT` for membership value plus field
  expiry. SDKs retain all request/schema/capacity validation; Lua retains only
  current-state and atomic-write requirements.
- **Correctness boundary:** Redis 8 Hash-field absolute expiry is limited to
  `2^46-1` milliseconds, which is stricter than Lua's exact integer range. Go,
  Rust, event/record admission, and Lua enforce the field-expiry ceiling; Lua
  performs the final check after obtaining authoritative Redis time.
- **Evidence:** The pre-promotion paired Register matrix improved Redis server
  time by 28.68 percent for 2 Attr/2 Data and 25.40 percent for default 16
  Attr/32 Data, positive in all eleven trials. The promoted bodies total 14,763
  bytes. At that promotion, medians were 10.15 microseconds for small Register, 39.26 for
  default-maximum Register, 10.28 for one-field Update, 10.46 for
  version-plus-Data Update, and 9.68 for Renew on the isolated Redis 8.8
  fixture; the following line-audit entry supersedes those current figures.
- **Qualification:** Canonical Lua, Go/Rust standalone and interoperability,
  WSL/Linux race, unit/static/generator checks, a 19,697,832-execution Go fuzz
  run, four five-minute load phases, 5,000-record recovery, and the complete
  two-promotion Sentinel matrix pass. No commit, release, or push was created.

### 2026-08-24: Inline and line-qualify Registration Lua hot paths

- **Decision:** Inline every one-call-site state, clock, deadline, expiry, and
  fixed error block. Register/Update bind the dynamic `ARGV` table and append
  event pairs through one next-write index. Renew/Unregister use
  operation-specific bindings. Do not bind short protocol strings: Lua already
  stores literals in the compiled constant table.
- **Decision:** A Register skips `DEL` only when its existing-state `HMGET`
  returns neither known Meta field. Every valid present record exposes those
  fields and still receives `DEL` plus complete replacement, so obsolete Attr
  or Data cannot survive a reset.
- **Rejected optimizations:** Modulo TIME truncation, removing the repeated
  `tonumber` local, implicit arithmetic request conversion, absent-state
  short-circuit conversion, a local `KEYS` table, and caching the two Update
  version-presence comparisons as `has_version` did not pass paired consistency
  and remain absent.
- **Evidence:** The generated set is 11,278 bytes. Twenty-one alternating
  same-Redis pairs improved server time by 9.03% small Register, 7.65%
  default-maximum Register, 6.66% one-field Update, 6.18% versioned Update,
  7.12% 31-field Update, and 7.25% Renew. A separate canonical 32-field
  current-source run passes. Unregister remained neutral.
- **Qualification:** Canonical Lua and generated copies; Go/Rust static, unit,
  real-Redis, race and interoperability; a 25,420,541-execution fuzz run; four
  fresh five-minute load phases; 5,000-record recovery; and the complete
  two-promotion Sentinel matrix pass. No commit, release, or push was created.

### 2026-08-24: Require immutable Version and zero-or-one active Leader (superseded)

- **Superseded 2026-09-02:** The entire Campaign/Leader target was withdrawn.
  The historical 2026-08-24 refinement below no longer constrains current work.
- **Historical 2026-08-24 refinement:** Strict zero-or-one activation and
  Sentinel fencing were retained at that checkpoint. The 2026-09-02 decision
  later withdrew the complete Leader target; existing Registration behavior
  remains unchanged.

- **Decision:** Registration `@version` is the election priority already
  present in Meta and is immutable for the process-start UUID lifetime.
  Changing version requires a new Registration UUID. There is no
  `version_revision`, version mutation operation, or Update version field.
- **Decision:** Same-UUID Register is recovery only. An existing Hash must have
  identical Version, TTL, and Attr or return `immutable`; an absent Hash is
  restored with the original process-start values retained by the still-running
  SDK. Re-registration never promotes or demotes a Campaign.
- **Decision:** An election domain has zero or one application-active Leader.
  Higher versions retire cooperatively; uncertain ownership immediately closes
  admission; term-owned work is canceled and joined before exact-token release.
  Handoff and failure may leave the domain without a Leader. No supported mode
  accepts overlapping active terms to improve availability.
- **Decision:** Standalone uses its configured Redis primary as the term
  authority. Because Sentinel asynchronous replication can leave divergent old
  and promoted primary histories, a Sentinel Campaign must acquire one
  deployment-provided durable fence or advisory lock after Redis claim and
  before exposing a LeaderTerm or invoking application code. It holds the fence
  through invalidation and joined cleanup. Missing or failed fencing leaves the
  domain without an active Leader.
- **Implementation status:** No Registration source or ABI change belongs to
  the historical Leader task. It was future work at this checkpoint and was
  later withdrawn rather than implemented.

### 2026-08-24: Decouple Campaign and election Version from Registration (superseded)

- **Superseded 2026-09-02:** The entire Campaign/Leader target was withdrawn.
  Registration Version remains application-defined, but none of the historical
  Campaign lifecycle or election requirements remain current.

- **Decision:** Verdandi's generic Campaign creates one immutable
  positive-integer Version and private random readiness token for its own
  lifetime. The token is the internal identity; no separate public Campaign ID
  is required. Campaign neither creates nor requires a Node Registration.
  Applications may coordinate the two lifecycles, but election Lua never reads
  Registration keys.
- **Decision:** Changing election priority requires closing the Campaign,
  invalidating and joining any term-owned work, withdrawing readiness, and
  creating a new readiness-token/Version pair. Campaign re-publication after a
  Redis reconnect preserves the pair; process restart or a new Campaign does
  not. There is no in-place version mutation or version revision.
- **Scope boundary:** This decision does not change or constrain Registration
  `@version`, Register, Update, or same-UUID recovery semantics. Leader never
  reads, copies, compares, or reacts to Registration Version or lifecycle
  mutations.
- **Reason:** Hermes Primary requires an ordinary service registration because
  the winner is itself a routable service snapshot. Verdandi Leader is generic
  coordination, so inheriting that dependency couples unrelated lifecycles,
  excludes non-Node controllers, and makes Registration recovery affect
  election without adding an exclusivity guarantee. Campaign readiness already
  proves claimant liveness.
- **Implementation status:** Protocol and design documents now express this
  boundary. Campaign implementation and qualification remain future work.

### 2026-08-24: Implement Catalog as one raw Value with ordered replay (superseded)

- **Superseded 2026-08-26:** The current Replace/Patch/Delete,
  Hash/ZSET/Pub/Sub Subscriber decision below replaces this complete section.

- **Decision:** One Catalog is one raw Hash Value at
  `verdandi:catalog:<zone>:<catalog>`. Verdandi does not define Catalog `Data`;
  external SDK codecs own scalar, array, map, or other types. Publisher SDK code
  owns complete-Value validation, differencing, ordering, and splitting into
  bounded independent Patch calls. Lua is only atomic glue.
- **Decision:** Redis owns one global canonical decimal revision in
  `1..9223372036854775807`. Hash metadata is `@revision`, `@tomb_version`,
  `@floor_revision`, and `@deleted`. Every changed Patch or whole Delete appends
  its actual delta at Stream ID `<revision>-0`. Same-state calls are no-ops;
  overlapping Publishers use Redis execution-order last-write-wins.
- **Decision:** Delete is explicit, has no TTL, removes application fields, and
  leaves a tombstone. `Compact` monotonically advances the replay floor and
  trims older Stream entries. A mirror with a gap, mismatched tomb version, or
  base below the floor performs a full bounded Hash reload before catch-up.
- **Decision:** Mirrors always retain the complete Value in memory. Optional
  bbolt (Go) and redb (Rust) databases are default-off, disposable recovery
  checkpoints whose formats need not interoperate. Redis remains authoritative;
  corrupt, ahead, or too-old local state is ignored, and reverse local-to-Redis
  replay is forbidden.
- **Decision:** Go and Rust expose `Catalog[T]` over the existing raw Mirror.
  Go binds `FieldCodec[T]` and clones detached projections. Rust statically
  dispatches the external type's `CatalogValue` implementation and caches
  `Arc<T>` for O(1) repeated typed snapshots. Both diff/split complete encoded
  Values without putting type information into Redis or local checkpoints.
- **Decision:** Every language uses at most one Catalog Stream Hub per Client.
  The first Mirror initializes its dedicated blocking connection and worker;
  Registration/Selector-only and Catalog-publisher-only Clients create neither.
  The Hub multiplexes all active Catalog keys; ordinary Client commands perform
  writes, headers, replay, scans, and checkpoint work. Per-Mirror one-event
  flow control bounds memory. A revision gap detaches only that Mirror, which
  attempts exact `XRANGE` recovery before a complete Hash reload; an advanced
  Hash header also detects a wholly missing Stream tail. Continuity reload is
  immediate, transport retry remains backoff-controlled, and other Mirrors
  remain live.
- **Implementation and evidence:** Three generated Lua programs and matching Go
  and Rust Publisher/Mirror/store implementations pass canonical-byte checks,
  unit/static tests, WSL/Linux race detection, raw binary and `MAX_INT64` Lua
  tests, checkpoint recovery, Delete/recreate/compact scenarios, and live
  integration against an isolated Redis 8.8.0 container. No commit, release, or
  push was created.

### 2026-08-25: Require Redis-server time for endurance claims

- **Decision:** A long-running qualification records Redis `TIME` independently
  of the client runtime and must meet its Redis-server duration floor. Client or
  WSL monotonic time alone cannot prove how long the remote Redis deployment
  carried the workload.
- **Reason:** An otherwise successful run reported 7,201.7 Go seconds while the
  Redis host and Windows orchestrator measured only 6,990.2 seconds. The final
  run scheduled 7,500 Go seconds and independently measured 7,263.649 Redis
  seconds against a hard 7,200-second floor.
- **Evidence:** The current 500-Registration/eight-Selector run completed
  3,750,000 Updates, 25 natural-expiry cycles, 25 explicit churn cycles, and
  34/34 script-cache, connection, pause, and AOF-restart faults. It ended with
  exact revision 7,501 convergence, zero unexpected asynchronous errors, zero
  Redis keys, and Go goroutines returned to their initial count. Canonical Lua,
  Rust convergence, and the full Sentinel matrix passed afterward.
- **Direct typed follow-up:** A first 7,500-Go-second attempt was correctly
  rejected because Redis had accumulated only 7,138.759 seconds after another
  WSL clock jump. The accepted rerun budgeted 8,000 client seconds and measured
  7,608.409 Redis seconds. It completed 4,000,000 typed Updates, 639,713
  `One`/`Any` transactions, 703,688 local mutations, all 34 faults, and the
  complete Lua/Rust/Sentinel post-checks. The Redis-time gate remains mandatory;
  a larger client budget is safety margin, not evidence by itself.

### 2026-08-25: Share immutable internal Registration field values

- **Decision:** After SDK validation and caller detachment or event decoding,
  internal Registration Attr/Data byte slices are immutable. Go may shallow-copy
  the map structure and Rust may share Data with `Arc`; neither language may
  expose those shared slices through a mutable public result.
- **Decision:** Selector internal records cache their exact complete-record byte
  size. New records compute it once; Update changes it by exact removed/added
  value and decimal metadata-width deltas. The decoder validates changed field
  names/values before state mutation, and the projected complete size remains
  gated before publication.
- **Reason:** Profiles showed repeated deep field cloning and complete record
  rescans dominated local Update/Renew work. The accepted ownership and size
  invariants remove redundant O(payload) work without weakening public
  detachment, protocol bounds, or Lua responsibilities.
- **Evidence:** Ten-sample WSL/Linux comparisons improve Go Update by 68.32%,
  Renew by 59.90%, and no-repair pending drain by 49.86%; Update/Renew
  allocations fall from 37 to five. Boundary, race, static, unit, isolated
  Redis 8.8, lifecycle, ceiling, cross-language, direct typed two-hour, and
  Sentinel tests pass. The qualified run started only after the separate
  Catalog interval released its isolated runtime and port.

### 2026-08-25: Use direct application-owned typed Registration/Selector APIs

- **Partially superseded 2026-08-27:** The original destination-injection Go
  method names and ownership contract are replaced by the standard
  `Encoder`/`Decoder` decision below. Direct application ownership remains.
- **Decision:** Go values implement `Encoder`/`Decoder`; Rust values
  implement `FieldValue`. The SDK does not generate application logic or require
  a Schema object. Raw `Fields` implements the same generic interfaces and
  remains a first-class compatibility boundary.
- **Decision:** `NewRegistration[Attr, Data]` is local construction with one
  fresh stable UUID. `Register` is the explicit readiness boundary. Complete
  typed Data Updates are encoded by the application, differenced by the SDK,
  and written only as changed Redis Hash fields.
- **Decision:** typed Selector `One` and `Any` execute synchronous borrowed-view
  policy callbacks. Mutations are staged, visible within the transaction, and
  committed atomically to that Selector's local prediction overlay only after
  validation; cancellation, callback error, foreign/duplicate results, or
  deadline expiry roll back the whole transaction.
- **Superseded ownership:** Application encoders formerly received an SDK-owned
  destination and could populate it without an additional SDK clone. The
  replacement return-value contract preserves one-map ownership while allowing
  application code to allocate the exact field capacity.
- **Evidence:** Ten WSL/Linux samples of the complete Go 500-candidate policy
  transaction show the ownership change reducing median time by 4.57%, bytes by
  24.21%, and allocations by 20.37%. Final `b.Loop` reference is 19.945-21.726
  us/op, 3,881 B/op, and 43 allocs/op. The 7,608.409-second Redis-time campaign
  verifies typed lifecycle, patch derivation, field-granular prediction,
  `One`/`Any`, retained TTL, standalone recovery, and Sentinel recovery on the
  same 58-file source fingerprint.

### 2026-08-26: Bound Registration and Selector task ownership

Historical status: the per-Registration ownership and Selector topology remain
current; the request-queue representation below is superseded by the
2026-08-28 single-slot Fields mailbox decision near the end of this document.

- **Superseding decision:** Every successfully published Registration owns one
  independent bounded request queue, one long-lived synchronization worker/task,
  one desired/confirmed state, and one renewal timer. It is the sole Redis writer
  for its UUID. A Client does not multiplex different Registrations through one
  mutation coordinator. Production processes are expected to hold few
  Registrations; 500 and 5,000 are qualification workloads.
- **Queue rule:** The first Go/Rust implementation admits at most 256 requests
  per Registration. Admission is ordered. Consecutive Updates coalesce in that
  order, invalid requests fail independently, last Version/Data-field wins, and
  Renew/Unregister are FIFO barriers. Valid calls folded into one write share
  its revision/outcome. A net no-op sends nothing.
- **Lease rule:** A confirmed real Update refreshes the Redis lease and resets
  that worker's next Renew deadline. A rejected or exact local no-op Update does
  not prove liveness and cannot postpone an already-due Renew.
- **Decision:** Each Selector owns one persistent listener/state-machine task.
  Full snapshot and targeted repair share one optional temporary task slot, so
  a Selector owns one task in steady state and at most two during
  synchronization. The listener is the only Pub/Sub receiver and mutable-state
  owner; it continues coalescing events while the temporary task builds and
  fences a candidate. Targeted repair stays in the current generation, while a
  successful full reconnect synchronization advances generation once. All
  public active, retained, Store::Snapshot, Find, One, and Any access is explicitly
  unavailable until the generation crosses its fence.
- **Shutdown rule:** Client shutdown signals and joins every Registration's own
  worker within its bounded cleanup policy. Selector shutdown
  cancels and joins its optional temporary synchronization before the listener
  exits. A reconnect replaces its connection generation instead of leaving a
  reader task behind.
- **Reason:** Per-Registration ownership isolates backpressure and permits an
  accumulated Update run to collapse locally. Selector ownership remains
  explicit without duplicate permanent generation readers. This preserves
  serialized writes,
  subscribe-before-scan ordering, the PING/PONG fence, targeted repair, or
  joined shutdown.
- **Policy boundary:** A detached complete Store::Snapshot is explicitly heavy O(N).
  SDK `0.1.0` keeps injected `One`/`Any` policy evaluation as a straightforward
  O(N) borrowed-view scan. Rust implements the same invariants with its native
  channel, task, cancellation, and borrow model.
- **Current evidence:** Go/Rust unit, static, strict lint, WSL/Linux race,
  authenticated Redis 8.8 functional, live interoperability, 500-Registration/
  eight-Selector load, 5,000-record synchronization, a 210-second six-fault
  preflight, and the authoritative two-hour run pass. Frozen 58-file fingerprint
  `c7bef517173b9c298e41b6dac272e78736b317c017bbe70ba838185960bdf63a`
  completed 4,000,000 Updates over 7,866.527 Redis seconds, with Update
  p50/p95/p99 0.649/1.044/1.427 ms, 34/34 injected faults, zero unexpected
  asynchronous errors, final revision/generation 8,001/15, Go goroutines
  `2 -> 529 -> 2`, decreasing stable Redis memory, and final `DBSIZE=0`.
  Canonical Lua and Rust raw/typed convergence passed afterward. The automatic
  Sentinel tail passed two promotions, acknowledged-write loss, total Sentinel
  loss/restart, stable Go/Rust UUIDs, and both Selector generations
  `1 -> 2 -> 3`. Numeric short-load topology remains Go `2 -> 513 -> 4`
  goroutines and Rust `5 -> 521 -> 1/2` Tokio tasks with 500 live
  Registrations. Queue reduction plus removal of the redundant typed-Update
  byte copy cut observed comparable preflight peak Go heap 70.41-78.13% to a
  35,014,568-47,374,384-byte range. The formal soak's 179,485,736-byte peak is
  not comparable SDK memory: exact percentile instrumentation alone retains
  about 69.1 MiB of operation durations. The earlier 7,388.601-Redis-second
  Client-coordinator run remains protocol/Lua history, not ownership evidence.
- **Reply confirmation:** Every successful Register, Update, and Renew reply is
  accepted only with the exact expected revision and a nonzero Redis timestamp.
  Post-dispatch `corrupt` and `ambiguous` outcomes retain desired state as
  uncertain/unhealthy, and the next Update or Renew performs complete Register
  recovery. This rule was added after rejecting a pre-fix endurance run and is
  covered by false-success unit and live Redis regressions in both SDK
  semantics.

### 2026-08-26: Rebuild Catalog around strict operations and authoritative repair

- **Supersession:** This decision replaces every earlier Catalog Stream,
  Mirror, Compact, segmented Patch, field-delete, `Catalog<T>`, and
  Client-shared Stream-Hub decision. Historical sections remain evidence only.
- **Shape and identity:** One Path is
  `verdandi:catalog:<zone>:<part>:<id>` and stores exactly one raw Value,
  contiguous Array, or Map. External application codecs own all type meaning.
- **Mutation:** Publisher exposes one bounded atomic last-write-wins Replace,
  one exact-`base_revision` Patch for Map additions/overwrites or existing
  Array indices, and complete last-write-wins Delete with a fresh tombstone.
  Removal, Array shape changes, and Value updates require Replace.
- **Lease and revision:** Every mutation holds a private token-fenced TTL lease.
  Redis owns one Zone revision in `1..=2^53-1`; live Hashes and companion ZSETs
  retain latest Replace and per-field revisions. Live/deleted/deleted-time ZSETs
  provide bounded recovery identity, and floor advances only on actual
  tombstone eviction.
- **Notification and recovery:** Lua publishes the complete committed
  Replace/Patch/Delete operation on the Path channel. Pub/Sub is disposable.
  One Subscriber uses one persistent listener and at most one temporary
  synchronization/repair task for all normalized exact/Part/Zone coverage.
  Pending work coalesces into the temporary slot, which exits when idle. It
  subscribes before Hash/ZSET/Read alignment, and
  fences with subscribed PING/PONG. A Patch applies only on its exact local
  base; all loss or mismatch repairs from Redis.
- **Memory, types, persistence:** Subscribers hold complete raw covered values
  in memory. Stable Entries survive delete/recreate. Go `Load<T>` and Rust
  `Entry::load::<T>()` choose the external type per call/Path. Optional
  bbolt/redb/SQLite is a monotonic disposable restart checkpoint; Redis remains
  authoritative and store failure disables later persistence for that Client
  generation.
- **Boundaries:** The public complete encoded byte default is 512 KiB and the
  configurable protocol maximum is 4 MiB. Redis 8 Standalone/Sentinel are supported and Cluster is rejected.
  Catalog currently owns an independent Client/pool. Tombstone defaults are 24
  hours, 1,000,000 members, and 256 evictions per Delete. The additional
  65,536-field internal ceiling remains a maintainer decision.
- **Evidence:** Current Lua generation/Redis contract, Go/Rust unit/static/docs,
  remote integration, forced reconnect, bounded decoder, and monotonic
  checkpoint tests pass. The new 30-second endurance preflight accepted 960/960
  scheduled operations; the 24-hour fault interval remains pending. No commit,
  release, or push was created.

### 2026-08-28: Require language-native SDK implementations

- **Cross-language boundary:** SDKs share protocol behavior, stable errors,
  lifecycle outcomes, limits, and conformance vectors. They do not copy one
  language's Context, channel, cancellation-token, task, trait, interface, or
  generic implementation shape into another language.
- **Toolchain rule:** Before adding a custom abstraction, review the stable
  language/runtime facilities available at the declared minimum version and
  the current stable version. Raising a minimum version remains an explicit
  compatibility decision; using an already supported stable feature is
  preferred when it removes work or allocation or strengthens ownership.
- **Go lifecycle:** Request/operation Contexts are explicit first parameters
  and are not stored in a Client or other long-lived struct. A long-lived owner
  may use an explicitly owned close signal; each derived timeout/cancellation
  scope owns and releases its CancelFunc. Do not add a watcher goroutine when
  an existing owner loop can observe shutdown.
- **Rust lifecycle:** Tokio owners may retain `CancellationToken`, derive
  one-way child tokens at task-tree boundaries, and combine cancellation with
  `tokio::time::timeout`/`select!`. Cancellation still requires the owner to
  await its tasks; tokens are not created per ordinary Redis command.
- **Performance rule:** Source symmetry is not evidence. Allocation, task,
  goroutine, lock, copy, and latency choices are verified with each language's
  own profiler, race/concurrency tools, minimum toolchain, and target-platform
  benchmarks.

### 2026-08-28: Align the Rust root Client with the thin transport boundary

- **Ownership:** Root `verdandi::Client` now owns only Fred connectivity,
  ordinary timeout, bounded Key/Hash commands, one shutdown token, and the
  private recipe for dedicated Pub/Sub connections. Root admission counters,
  child guards, joined domain shutdown, and direct domain `Deref` access are
  removed.
- **Zone/API:** Root `Config::new(endpoint)` contains no Zone. Required Zone is
  supplied independently through `registration::Config::new(zone)` and
  `catalog::Config::new(zone)`, allowing one authorized transport to serve
  multiple Zones.
- **Bootstrap:** Root open performs only bounded `PING`; Registration performs
  the Redis 8 `HELLO` version check before Zone policy and Lua bootstrap.
- **Shutdown:** `close().await` cancels the transport and awaits Fred `quit()`
  without waiting for domain Clients. A separate public-owner `Arc` means
  domain transport references do not keep the public root handle alive. Last-
  handle Drop signals and schedules best-effort cleanup; explicit domain-before-
  root close remains the deterministic contract.
- **Hot path:** Root Key/Hash commands no longer take a synchronous admission
  mutex, clone an `Arc` guard, or update root active counters. Domain admission
  remains because Registration and Catalog close must join their own work.
- **Evidence:** Rust format, 52 unit tests, all-target tests, strict Clippy,
  rustdoc, isolated Redis 8.8 Lua/Go/Rust/interop qualification, the new
  two-Zone root test, and a clean-repeat three-Redis/three-Sentinel two-promotion
  matrix pass. The first Sentinel attempt reached the second promotion but its
  topology retained the excluded old primary; exact cleanup and an unchanged
  rerun completed in 29.914 seconds. No commit, release, or push was created.

### 2026-08-28: Make the Go root Client a thin transport wrapper

- **Go-only ownership decision:** Root `verdandi.Client` owns one concrete
  `*redis.Client`, connectivity configuration, ordinary operation timeout,
  bounded Key/Hash helpers, cached Hash descriptors, and one close signal. It
  owns no Zone, child admission count, request join, or domain worker lifecycle.
- **Domain configuration:** `Zone` is required independently by
  `registration.Config` and `catalog.Config`. One root transport can therefore
  serve multiple Zones when its Redis ACL credentials authorize those keys.
- **Bootstrap boundary:** Root `Open` performs bounded `PING` only and does not
  require `INFO`. Registration `Open` performs the Redis 8 check through
  `HELLO`, reads/installs Zone policy, and loads Registration Lua. Catalog loads
  and validates the operations it actually owns.
- **Shutdown:** Go root `Close() error` is idempotent, has no artificial
  Context, broadcasts transport loss, and closes go-redis immediately. Domain
  Clients observe that signal but independently cancel and join their workers.
  Applications close domains before the root when deterministic cleanup order
  matters.
- **Superseded access detail:** Registration and Catalog originally obtained
  the concrete driver, close signal, and timeout through a client-owned
  `internal` capability. The later direct-root-capability decision removes that
  bridge while retaining the same ownership and shutdown rules.
- **Ergonomics:** Concise root command methods remain alongside their explicit
  `*Context` variants. The implementation is shared; concise calls delegate
  with `context.Background()`.
- **Rust boundary:** Superseded later on 2026-08-28 by the aligned thin Rust
  transport ownership entry above.
- **Evidence:** Go format, repeated unit, vet, all-tag compilation, three peer
  gates, isolated Redis 8.8 integration, WSL/Linux race, cross-language
  interoperability, and both Registration and Catalog two-promotion Sentinel
  matrices pass. The isolated run processed 4,655 commands and the Catalog
  Sentinel run ended at revision 10 with zero keys. No long test, commit, or
  push was performed.

### 2026-08-28: Expose the Go root transport capability directly

- **Decision:** `verdandi.Client` publicly returns its borrowed concrete
  `*redis.Client` through `Redis()`, its permanent close broadcast through
  `Done()`, and its normalized ordinary timeout through `Timeout()`.
  Registration and Catalog consume the root Client directly; the former
  `internal/clientaccess` bridge is removed.
- **Ownership:** `Redis()` always returns the same pointer and transfers no
  ownership. Only root `Client.Close()` may close it. `Done()` closes once for
  permanent root shutdown and never for an ordinary disconnect, reconnect, or
  Sentinel promotion. The timeout is immutable for the root lifetime.
- **Trust boundary:** Raw go-redis operations are authorized by the supplied
  Redis ACL. They can deliberately bypass SDK validation and multi-key
  invariants and therefore do not receive Verdandi capacity, stable-error, or
  ambiguous-write guarantees. Domain APIs remain driver-neutral; Rust keeps
  Fred private and does not imitate the Go API shape.
- **Lifecycle:** Root Open and domain Open create no shutdown-only watcher.
  Registration/Selector/Catalog owner loops observe `Done()` directly, and
  explicit domain Close joins domain work before optional root Close.

### 2026-08-28: Remove the Rust private Transport capability

- **Decision:** Rust root `Client` directly owns one private `Arc<Inner>` with
  Fred state. Registration and Catalog retain a clone of the same
  `verdandi::Client`; the former `Owner -> Arc<Transport>` ownership chain and
  private `Transport` type are removed.
- **Lifecycle:** Cloning the root Client creates no connection or task. Dropping
  one caller variable leaves a domain-owned dependency valid. Explicit
  `close().await` permanently closes every clone; dropping the final
  root-or-domain-held clone schedules best-effort Fred cleanup because Rust
  `Drop` cannot await.
- **Boundary:** Fred configuration, driver commands, shutdown token, ordinary
  timeout, and Subscriber construction remain crate-private methods of the root
  Client. No Fred type or alternate transport handle enters the public API.
- **Evidence:** Rust format, 52 library tests, endpoint-free integrations,
  strict Clippy/rustdoc, eight isolated Redis 8.8 Registration/root tests, and
  the 40.327-second two-promotion Go/Rust Sentinel matrix pass on fingerprint
  `e709ae4ce1149377c2276e41e053c7b264f64cacda13da29b85559261dd628f9`.
  Test source was not modified.

### 2026-08-28: Use the single-word root timeout name

- **Decision:** The root command budget is Go `Config.Timeout`/`Timeout()` and
  Rust `Config::timeout` plus crate-private `timeout()`. Invalid configuration
  reports field `timeout`.
- **Reason:** Config and Client already scope the value to an ordinary Redis
  command, so `timeout` is clear and conventional in both languages. `deadline`
  would imply an absolute instant; `wait` and `request` are less precise.
- **Boundary:** Domain-specific budgets such as `sync_timeout` keep their
  qualified names. The first public API is `0.1.0`; no deprecated alias is
  needed for a name that was never part of a released version.
- **Maintenance:** In both Go and Rust, root Open now performs its sole `PING`
  directly and Catalog Open directly loads its scripts. Their one-use, one-
  command `bootstrap` wrappers carried no independent invariant. Registration
  keeps its multi-step bootstrap for Redis 8 validation, Zone policy
  publication, and Lua loading.
- **Review evidence:** Live-review fingerprint
  `05874222ecae71f6469039e89f6b745a58402cea53171e2fc74d470a7641e867`
  passes local static/unit gates and a 14-suite isolated Redis 8.8 functional
  run. A Sentinel review attempt passed first recovery and unavailable-state
  behavior, then hit the known second-promotion topology gate; full fault and
  long qualification is intentionally deferred until review completion. The
  follow-up Go wrapper removal passes affected-package tests and produces current
  fingerprint `bfb9396852fbf66d86f6a0d19fef35b7c5ba5a78e6098ef215366e4ef7747bc7`.

### 2026-08-27: Share transport clients and adopt Go 1.27 APIs

- **Superseded 2026-08-28 for both SDKs:** One root Go/Rust Client owns Redis
  connectivity, Redis 8 validation, child admission, cancellation, and joined
  pool shutdown. Registration and Catalog child clients reference that root and
  own only domain configuration, scripts, workers, diagnostics, and Catalog
  persistence. This supersedes the independent Catalog Client/pool statement
  above. Child close releases its root reference; root close prevents new
  children, cancels admitted children, waits for them, and then closes Redis.
  The Redis-sharing statement remains current for both SDKs; Redis 8 validation,
  Zone pinning, child admission, and joined root shutdown no longer describe
  either root implementation.
- **Go API decision:** Go 1.27 is the minimum module language version. Generic
  methods replace package-level generic entry points:
  `registrationClient.Registration[A, D](options)`,
  `registrationClient.Selector[A, D](ctx, options)`, and
  `entry.Load[T]()`. Catalog Publisher and Subscriber construction likewise
  belongs to `catalog.Client`. No compatibility wrappers are retained because
  no package version has been released.
- **Go implementation rule:** Generic function assignment may infer codec type
  arguments from a concrete function field. Store static generic function
  instances instead of closures when the behavior is identical. Promoted field
  keys may initialize a value-embedded struct literal when every copied field
  is explicit; they may not cross pointer embeddings, overlap an explicitly
  initialized embedded field, or justify flattening a safer whole-struct copy.
- **Measured result:** Removing two retained Registration codec closures lowers
  local handle construction from 288 B/five allocations to 240 B/three
  allocations on Go 1.27 Windows/amd64. Selector codec bindings no longer
  capture generic dictionaries. An interleaved ten-pair, single-core compiler
  experiment found no statistically significant change in the two measured hot
  paths when Go 1.27 size-specialized allocation was enabled; source-level
  rewrites for that automatic runtime optimization are therefore unjustified.
- **Historical boundary:** Typed Redis command wrappers were not part of the
  shared-transport/Go-1.27 change itself. They were subsequently accepted and
  implemented under the typed-root-command decision below; their current
  contract is documented in [`sdk/go/redis.md`](sdk/go/redis.md).
- **Accepted typed-Hash read behavior:** an untagged exported Go struct field
  uses its exact field name; a `redis` tag may override or exclude it. A
  missing HMGET position leaves the corresponding destination field at its Go
  zero value and does not return an error.
- **Accepted root command ergonomics:** export paired concise and
  `Context`-suffixed methods only for ordinary root Redis commands. Domain
  lifecycle operations remain Context-explicit.

### 2026-08-27: Implement typed root Redis commands

- **Version-1 surface:** Root Go/Rust Clients expose connection `Ping`, one
  complete-key/String command group, and one basic Hash command group. They
  accept complete Redis keys and never construct domain paths, advance domain
  revisions, publish events, or expose the underlying driver.
- **Scalar contract:** Built-ins are owned bytes, strings, booleans encoded as
  exact `0`/`1`, and canonical fixed-width signed/unsigned decimal integers.
  Machine-width integers, floats, and automatic JSON/Serde are excluded. Go
  custom values use standard Binary then Text marshal interfaces; Rust custom
  values implement `EncodeValue` and `DecodeValue`.
- **Hash contract:** Go derives exact ordered fields from exported top-level
  struct fields and a minimal `redis` tag, caching the descriptor per T. Rust
  uses a manual static `HashValue` trait plus the optional official derive
  macro. Missing Go and derived Rust fields remain zero/default; malformed
  present bytes fail without publishing a partial value.
- **Write and result contract:** HSET is patch behavior and returns only error.
  Single-key DEL/EXISTS return bool, HDEL returns its multi-field removal count,
  and HLEN returns its count. Rust TTL writes use the consuming one-use
  `with_ttl(ttl).set/store` mode; Go uses `SetWithTTL`/`StoreWithTTL`.
- **Resource and failure contract:** Version 1 bounds keys to 1,024 bytes, Hash
  operations to 4,096 fields, field names to 1,024 bytes, values to 512 KiB,
  and a complete Hash to 512 KiB. Deterministic server errors such as WRONGTYPE
  are protocol errors. A lost write result is ambiguous and must be reconciled
  before retry.
- **Evidence:** Go and Rust unit/static/format checks, external derive usage,
  strict Clippy, and isolated Redis 8.8 Go/Rust command tests pass. The test
  fixture removed its random labelled container. No Registration test, long
  test, commit, or push was run.

### 2026-08-27: Standardize Go application field encoding

- **Decision:** The shared Go field capabilities are `Encoder`, whose `Encode`
  method returns `(Fields, error)`, and `Decoder`, whose `Decode` method accepts
  `Fields` and returns error. The unreleased `FieldEncoder`, `FieldDecoder`,
  `VerdandiEncode`, and `VerdandiDecode` names are removed without aliases.
- **Ownership:** `Encode` returns one complete top-level representation and
  transfers ownership of its map and byte slices to Verdandi. `Decode` receives
  a detached complete representation, replaces its receiver, and may retain the
  input. Raw `Fields.Encode` deep-clones caller storage; raw `Fields.Decode`
  adopts the detached input.
- **Scope:** Registration Attr/Data and Catalog Publisher/Entry use the same
  root interfaces. Redis scalar Key/Hash conversion remains a separate private
  codec. Rust `FieldValue` and all Redis wire/storage semantics are unchanged.
- **Reason:** Short capability names are clear under the `verdandi` package,
  remove protocol-branded method noise, and let each application encoder size
  its returned map exactly without adding an SDK copy.
- **Evidence:** Go formatting, vet, ordinary tests, integration and soak compile
  gates, and Go 1.27 WSL/Linux race tests pass. No Redis or long-running test was
  started, and no commit or push was created.

### 2026-08-27: Keep Rust tests physically separate

- **Decision:** Rust test implementations, fixtures, and test-only declarations
  live under `sdk/rust/tests`. Production modules may contain only minimal
  `#[cfg(test)]` path hooks when private white-box access is required.
- **Reason:** This keeps production files focused and makes production/test
  size and ownership explicit without weakening visibility solely for tests.
- **Evidence:** All 19 formerly embedded unit-test modules are physically
  separate. Current stable Clippy and 53 endpoint-free tests pass, and the same
  all-target test suite passes on the declared Rust 1.85 minimum.

### 2026-08-28: Replace Registration request queues with one Fields mailbox

- **Superseding detail:** The 2026-08-26 per-Registration ownership decision
  remains, but its 256-request FIFO representation is superseded. Every
  published Registration now owns one single-slot Fields merge mailbox, one
  capacity-one wake signal, one long-lived sole-writer worker/task, and one
  renewal timer.
- **Merge rule:** The mailbox stores only an optional latest Version, the latest
  pending byte value for each changed Data field, and result waiters. A later
  value for the same item overwrites the earlier pending value. All Update
  calls absorbed by one worker batch share its actual outcome and content
  revision. An aggregate no-op performs no Redis command.
- **Renew rule:** Renew contributes no Fields. If its batch contains an
  effective successful Update, that atomic Update already refreshed TTL and
  satisfies Renew. If Update is a no-op or fails, Renew performs its own Redis
  mutation. Any successful Update/Renew resets the automatic-renew deadline.
- **Capacity and cancellation:** A per-Registration admission semaphore
  defaults to 8 and accepts 1..256. It bounds admitted result waiters rather
  than allocating request/Data slots. Calls may cancel while waiting for
  admission; after admission Go waits for the real worker result. In Rust the
  permit remains attached to mailbox work even if the caller drops its Future,
  preventing cancellation storms from bypassing capacity.
- **Ownership:** Typed input is synchronously encoded before mailbox admission;
  raw Fields are detached. The SDK retains no application struct after Encode
  returns. Applications still must not mutate referenced interior storage while
  their Encode method itself is running.
- **Evidence:** The frozen 104-file implementation passed a 3,600-second Redis
  8.8/AOF campaign with 1,800,000 Updates, 16 injected standalone faults, and
  pre/post Lua and Rust convergence gates. Its post-soak Sentinel matrix passed
  primary movement `16381 -> 16383 -> 16382` and Go/Rust Selector generations
  `1 -> 2 -> 3`. Go bounds each Pub/Sub receive and any synchronous go-redis
  reconnect with the same owner-loop deadline so total loss cannot indefinitely
  delay the unavailable view. Detailed evidence is in
  [`registration/fields-mailbox-config-1h-20260828.md`](registration/fields-mailbox-config-1h-20260828.md).

### 2026-08-29: Transfer detached fields and share immutable overlay baselines

- **Decision:** A Fields value freshly returned by an application Encoder is
  already owned by the SDK. After selected-value validation, Go may transfer
  that detached value directly into the result Decoder; Fields borrowed from an
  immutable SDK view must still be cloned before application code receives
  them. Result decoding completes before overlay commit.
- **Decision:** Selector overlay comparison baselines and staged field buffers
  are immutable after publication. Go shares their maps/value buffers under
  that internal convention; Rust expresses the same ownership with
  `Arc<Fields>`. Reconciliation creates a new writable map only where it must
  replace entries.
- **Retention rule:** Reusable Go transaction buffers clear published commit
  scratch and any tail removed by a shrinking view. Stable-size selections do
  not pay an additional full clear pass.
- **Evidence:** Go 1.27 Linux ten-sample measurements reduced `One(500)` from
  3,882 B/43 allocations to 2,226 B/28 and `Any(8/500)` from 14,723 B/154 to
  8,067 B/97. Current Redis 8.8 standalone and two-promotion Sentinel matrices
  pass for Go and Rust at fingerprint
  `2d3235af5a7a63049e4ba63c3a4fe2a933cd71ce829d753dbdfd9f1a89c8100b`.
  Detailed controls and limitations are in
  [`registration/selector-optimization-review-20260829.md`](registration/selector-optimization-review-20260829.md).

### 2026-08-29: Keep numeric configuration in owning structure methods

- **Decision:** The 2026-08-28 VDL/code-generation experiment is superseded.
  Numeric defaults, ranges, zero semantics, and relationship checks live in
  methods on the native configuration structures that consume them. The DSL,
  generator, and parallel Go/Rust rule modules are removed.
- **Surface:** No exported defaults/ranges constants API is added.
  `configuration.md` remains the hand-maintained cross-language review table;
  Verdandi still imposes no JSON, TOML, YAML, or other runtime carrier.
- **Ownership:** Root Redis, Registration/Selector, and Catalog retain separate
  native configuration structures. Their methods own field and cross-field
  validation alongside topology, addresses, credentials, TLS/path types,
  driver mapping, and I/O. Redis Cluster remains unsupported.
- **Transport behavior:** Root command timeout defaults to 2 seconds, connect
  timeout to 5 seconds, the shared pool to 1..4 connections, and excess-idle
  removal to 10 seconds. Reconnect uses bounded exponential backoff, but Go and
  Rust both disable automatic replay of dispatched business commands.
- **Finite foreground waits:** Every operation that can block its caller has a
  positive finite budget. Catalog Path-lock acquisition defaults to 30 seconds
  and bounds the whole contention loop; the root timeout still bounds each
  Redis command within it. Persistent background reconnect/recovery loops may
  continue for the owning client lifetime, but they expose unavailable state
  instead of holding one foreground operation forever.
- **Explicit zero semantics:** Zero may disable jitter, request immediate
  Selector publication, remove artificial RedisClock uncertainty, disable a
  retained view, or remove Catalog's aggregate view cap. It never disables a
  command, connection, synchronization, or lock-acquisition timeout. Go uses
  pointers only where omission and explicit zero must be distinct; Rust
  constructors materialize the same defaults as scalar values.
- **Domain limits:** Registration's mailbox admission defaults to 8. Redis-
  backed Registration Attr/Data policy remains initial-default plus later
  administrator override. Selector and Catalog local bounds are configurable.
  Catalog record bytes default to 512 KiB and may reach 4 MiB; aggregate view
  encoded bytes default to no additional limit and may be bounded through
  64 GiB.

### 2026-08-29: Add one strict JSON configuration boundary

- **Supersession:** The preceding decision's “no JSON/TOML/YAML carrier” clause
  is superseded. Its native validation ownership, no-code-generation rule, and
  no-exported-constants rule remain current.
- **External contract:** Verdandi v1 configuration is a JSON object containing
  `version: "v1"`, required `redis`, and optional `registration` and `catalog`
  objects. [`configuration.schema.json`](configuration.schema.json) is the
  canonical shape and [`configuration.example.json`](configuration.example.json)
  contains every field.
- **Loaders:** Go package `configuration` and Rust module `configuration`
  strictly parse at most 1 MiB, reject unknown/duplicate fields, null, trailing
  JSON, non-integer numeric values, unsupported versions, invalid topology, and
  invalid range/relationship values before Redis, checkpoint, or TLS file I/O,
  then convert into native SDK configs.
- **Defaults:** Omission selects the documented default. Explicit zero is
  accepted only for database zero, disabled jitter, immediate Selector view
  publication, zero artificial clock uncertainty, disabled retained view, or
  an unlimited aggregate Catalog view.
- **Catalog lock:** The token-fenced TTL lock remains in the implementation;
  the following decision fixes its v1 disposition and workload assumption.
- **Catalog ceiling correction:** Canonical and generated Catalog Lua now use
  the documented 4 MiB protocol ceiling rather than the stale 512 KiB literal.

### 2026-08-29: Expand TLS and retain the low-contention Catalog lock

- **TLS object:** `redis.tls` contains `enabled`, `system_roots`,
  `server_name`, `ca_file`, `cert_file`, and `key_file`. TLS uses version 1.2
  or newer, never disables peer verification, supports a private PEM CA bundle,
  and supports a paired unencrypted PEM client certificate/private key.
- **Trust semantics:** System roots default on. `ca_file` appends private roots;
  when system roots are disabled it becomes mandatory and defines the trust
  set. Each PEM file is read with a 1 MiB cap. JSON loading checks structure and
  path bounds without touching files; native transport construction reads them
  before any Redis connection.
- **Superseded 2026-09-01 — former SNI boundary:** Fixed `server_name` was
  initially limited to Standalone because Fred could not carry one override
  into a primary newly discovered by Sentinel. The later fixed-identity
  Sentinel TLS decision replaces this restriction with a cross-language
  transport implementation and deployment contract.
- **Catalog workload:** Keep the external token-fenced Path lock. A Publisher
  is normally a single writer or one of a few nearby writers, making contention
  exceptional and the acquire round trip acceptable. This preserves SDK-owned
  Patch projection and validation and is no longer a pending decision.
- **Lock guarantees:** TTL handles orphan recovery, the acquisition deadline
  bounds unfair retry, and confirmed mutation Lua deletes its exact token with
  the write. No fairness guarantee is added. Sustained contention or high-RTT
  publication requires a later protocol review rather than weakening the v1
  contract implicitly.

### 2026-08-30: Remove the Catalog Path lock and consolidate current SDK internals

- **Supersession:** This decision supersedes only the Catalog-lock clauses in
  the two preceding 2026-08-29 decisions. Their JSON, TLS, Cluster, native
  configuration, and 4-MiB ceiling decisions remain current.
- **Catalog concurrency:** Replace and Delete are one atomic Lua call and use
  Redis-primary execution-order last-write-wins. Patch performs unlocked HMGET
  projection, then Lua rechecks the exact `base_revision`, affected fields,
  projected bytes, and shape before writing. A same-base race has one winner
  and stale losers; a lost mutation reply remains ambiguous.
- **Protocol surface:** Catalog has Read, Replace, Patch, and Delete scripts
  with six mutation keys. Remove `:@lock`, Acquire/Release, token/TTL arguments,
  lock retry/deadline settings, and their JSON/native configuration fields.
- **Publisher ownership:** Publisher is a stateless view of Catalog Client and
  has no Close. Catalog Client admits each operation and owns shutdown.
  Subscriber owns one long-lived Pub/Sub reader and one long-lived repair task;
  Go's final worker performs terminal completion without a third waiter.
- **Internal consolidation:** Go shares configuration primitives and a
  Context-aware activity gate through internal packages. Rust shares a
  crate-private Activity and RAII Guard while preserving CancellationToken and
  awaited close. No cross-language source-shape requirement is introduced.
- **Compatibility evidence:** Both SDKs consume the same versioned JSON and
  binary Catalog event conformance vectors. Redis 8.8 functional, WSL/Linux
  race, Rust, and Go/Rust interoperability suites pass after the change. Both
  Registration and Catalog pass independent two-promotion Sentinel matrices.
  The exact 70-file Catalog fingerprint passed 60 Redis seconds at 128
  mutations/second with 8,448 accepted writes, zero transient/stale/unexpected
  errors, stable memory, complete post-checks, and final `DBSIZE=0`.
- **Historical deferred scope:** local bbolt/redb Catalog recovery remained.
  Leader and C++ were not implemented at this 2026-08-30 checkpoint; the C++
  status and Subscriber task topology are superseded by the 2026-08-31
  decision below. Scores and evidence for this historical state are in
  [`optimization-review-20260830.md`](optimization-review-20260830.md).

### 2026-08-31: Use transient Catalog synchronization and implement C++23

- **Catalog task ownership:** Go, Rust, and C++ Subscribers each own exactly one
  persistent Pub/Sub listener/state-machine task. Initial alignment, reconnect
  alignment, and targeted repair share at most one temporary task slot.
  Requests coalesce while it is occupied; the task drains requested work and
  exits when idle. Steady state is one task and synchronization is at most two.
- **C++ baseline:** The official C++ source baseline is C++23. One compiled
  implementation uses Boost.Redis 1.92, yyjson 0.12, OpenSSL, and SQLite 3.37
  or newer. Templates remain at compile-time Schema, typed/raw Fields,
  Catalog-load, and Selector-policy boundaries; transport and protocol state
  machines are compiled once and third-party types remain private.
- **C++ ownership:** The root Client shares one private pool/reactor and exposes
  thin Key/Hash operations. Registration owns one worker, coalescing mailbox,
  renewal timer, and desired/confirmed state per published UUID. Selector owns
  one persistent listener and at most one temporary sync task. Catalog
  Publisher is task-free; Subscriber follows the same persistent/temporary
  topology and may use a transactional monotonic SQLite restart checkpoint.
- **Configuration and topology:** C++ loads the same strict v1 JSON and owns
  equivalent native `check()` methods. Redis 8 Standalone, ACLs, Standalone TLS,
  and plain Sentinel were implemented at this checkpoint. Cluster and
  Sentinel+TLS were rejected; the 2026-09-01 decision below supersedes only the
  Sentinel+TLS restriction.
- **Compatibility:** A second C++11/14/17 SDK is not maintained. Implemented C
  ABI v1 exposes the same compiled C++23 core to C11 and C++11/14/17 callers
  through opaque handles, raw Fields, strict JSON configuration, and owned
  release functions. `verdandi::c` does not propagate C++23 to its consumer,
  although a source build still needs a C++23-capable toolchain for the core.
- **Evidence boundary:** Strict GCC, unit and authenticated Standalone
  integration, clang-format, clang-tidy, ASan/UBSan/leak checks, and an isolated
  three-node/three-Sentinel startup/integration smoke pass. At this checkpoint
  this did not claim a full two-promotion C++ failover matrix, live TLS, MSVC/Clang/macOS,
  install/export packaging, long soak, or C++ performance qualification.
  Static/shared C ABI builds and lower-standard GCC consumers now pass, but
  Windows DLL/MSVC and automated ABI compatibility remain open. Details and
  the current **9.3/10** C++ score are in
  [`cpp-review-20260831.md`](cpp-review-20260831.md).

### 2026-08-31: Add a C++11 source facade without adding another runtime

- **Accepted surface:** `verdandi::legacy` is a header-only C++11 RAII and
  typed facade over C ABI v1. C++14 and C++17 use the same facade.
- **State boundary:** The facade owns no transport, Redis pool, retry,
  synchronization, recovery, task, or checkpoint state. All calls enter the
  existing compiled C++23 implementation through C ABI v1.
- **Compatibility:** C ABI v1 remains the stable binary contract. Legacy C++
  class and template layouts are source-only; an old compiler needs a prebuilt
  runtime because source-building the core still requires C++23.
- **API shape:** Root and domain handles share ancestor lifetime, leaf handles
  are move-only RAII values, configuration remains strict v1 JSON, and raw
  Fields plus C++11 schema/codec-based Attr/Data are supported. Selector policy
  callbacks borrow candidates synchronously and translate exceptions before
  returning through C.
- **Performance boundary:** Success `result<void>` is pointer-sized and creates
  failure state only on error. Fields and detached typed values are necessarily
  copied/decoded at the ABI boundary; native C++23 remains the lowest-overhead
  Selector API because it can reuse cached typed projection.
- **Evidence:** Strict C++11/14/17 static/shared builds, typed Redis 8.8
  integration, format, separately configured static analysis, and
  ASan/UBSan/leak checks pass. Windows DLL/MSVC, Clang/macOS, install/export,
  automated ABI compatibility, performance baselines, and soak remain release
  gates.

### 2026-08-31: Add an idiomatic C# facade over C ABI v1

- **Accepted boundary:** C# targets .NET 8 and .NET 10 and forwards all root,
  Registration, Selector, and Catalog behavior through C ABI v1 into the
  existing C++23 runtime. It owns no Redis driver, pool, Lua loader, clock,
  worker, listener, recovery, or checkpoint state.
- **Managed ownership:** Source-generated `LibraryImport` is private. Dedicated
  SafeHandles own every opaque allocation, and internal parent references keep
  native release order valid after early public Dispose. No pointer, manual
  release function, callback context, or borrowed C view is public.
- **Typed boundary:** Application values implement static generic
  `IFieldValue<TSelf>` encode/decode behavior. Immutable raw Fields use the same
  API. Runtime reflection, application-record JSON, schema services, and SDK
  code generation remain absent.
- **Selector boundary:** Policies remain synchronous. Borrowed Candidates are a
  `ref struct`, Choice values carry one process-wide transaction identity, and
  all callback exceptions are translated before returning through C. The
  synchronous C ABI does not justify a `Task.Run`-based fake async surface.
- **Loading and packaging:** The loader accepts one explicit native path, a
  NuGet RID native directory, the application directory, or normal OS search,
  then requires ABI v1. No local Debug binary is silently packed. Formal NuGet
  release needs qualified per-RID native artifacts.
- **Evidence boundary:** .NET 8/10 warning-as-error builds, formatter/analyzers,
  offline Fields/Result/ABI-layout cases, independent self-contained Linux x64
  ACL Standalone tests, concurrent Registration/Selector pressure, configured
  capacity boundaries, concurrent root disposal, forced finalizer cleanup, and
  a two-promotion Sentinel fault matrix pass.
  At this checkpoint Windows/macOS, NativeAOT/trimming, NuGet packaging, TLS, direct
  cross-language C# peers, performance, and soak remain release gates. The current managed-scope score is
  **9.3/10**.

### 2026-08-31: Keep each language's qualification independently executable

- **Decision:** Go, Rust, C++ and C# each own an independently runnable
  regression. A normal language-local change does not require an aggregate
  all-language command. Shared vectors and fixture components may be reused,
  but a result belongs only to the SDK whose public API was exercised.
- **Compatibility boundary:** Cross-language tests remain separate evidence.
  They cannot replace either language's own functional gate, and they do not
  force unrelated SDKs into every rerun.
- **Shared-core boundary:** A C ABI/C++ core change requires the affected C++
  gates plus the binding gates actually relying on that change. The current C#
  run therefore owns its .NET 8/10 Standalone and Sentinel results, while the
  Release-only C++ parser correction separately owns Debug, shared Release,
  ASan/UBSan, format, clang-tidy, and live C++ Sentinel results.

### 2026-09-01: Commit to fixed-identity Sentinel TLS and local runtime capability queries

- **Sentinel TLS contract:** TLS-enabled Sentinel requires one non-empty fixed
  `redis.tls.server_name`. Every Sentinel and every Redis primary/replica that
  can be announced must present a certificate containing this same DNS name or
  IP SAN. A Sentinel-returned address is routing input and never changes the
  configured trust identity.
- **Verification:** All SDKs retain TLS 1.2+, CA-chain, validity, handshake-
  signature, and peer-identity verification. No insecure bypass is added. Go
  applies one `tls.Config.ServerName`; C++ stores the identity on `SSL_CTX` and
  reapplies DNS SNI at every OpenSSL handshake, including Boost.Redis streams
  rebuilt after discovery or failover.
- **Rust transport boundary:** rustls delegates complete validation to its
  standard WebPKI verifier with only the server identity fixed. Fred 10.1
  cannot propagate fixed SNI to every dynamically discovered node, so Sentinel
  mode disables address-derived SNI. Redis/Sentinel must terminate TLS directly
  and cannot depend on SNI virtual-host routing for dynamic node addresses.
- **Capability API:** C ABI v1 adds the allocation-free exact string query
  `verdandi_c_has_capability`. C++11 Legacy exposes `has_capability`, and C#
  exposes `Runtime.Supports`. Current names are `catalog`, `client`,
  `configuration.json`, `redis.commands`, `redis.sentinel_tls`, `registration`,
  and `selector`. Presence describes compiled code, not Redis, ACL, certificate,
  or network health. This does not supersede the 2026-08-22 decision against
  wire-protocol capability negotiation.
- **Evidence:** On both Windows x64 and Linux x64, the certificate SAN contained
  only `verdandi.test`, not the announced node IPs. Go and Rust rejected a wrong
  fixed identity and preserved UUIDs plus Selector generations `1 -> 2 -> 3`
  through two promotions, total Sentinel loss, and recovery. C++23 passed TLS
  Root/Registration/Selector/Catalog/checkpoint integration and rejected a
  wrong identity on both native runtimes. C# net8.0 and net10.0 passed the
  two-promotion matrix through each platform's C++ shared core.
- **Deferred scope:** Automated native/NuGet RID packaging remains deliberately
  deferred. Live mutual TLS, package signing, direct C++23 two-promotion
  coverage, and TLS endurance remain future release gates.

### 2026-09-01: Support Linux and Windows while excluding macOS

- **Platform contract:** Linux x64 and Windows x64 are supported source-build
  targets. macOS is intentionally unsupported and is not a deferred `1.0.0`
  release gate.
- **Windows baseline:** Native C++ targets Windows 10 or newer. The qualified
  local toolchain is Visual Studio Community 2026, MSVC 19.51, Windows SDK
  26100, CMake 4.4, and an existing vcpkg OpenSSL 3.6.0 installation. Verdandi
  does not install a compiler, SDK, package manager, or OpenSSL automatically.
- **Build behavior:** MSVC compiles project sources explicitly as UTF-8 so the
  temporary Chinese production comments remain valid under `/W4 /WX`. The
  platform baseline is expressed through `_WIN32_WINNT` and `WINVER`; warning
  severity is not reduced.
- **Evidence:** Static Debug and shared Release builds both pass the complete
  nine-test acceptance matrix with only the three intentionally endpoint-owned
  tests skipped. The DLL exports the ABI version and string capability query;
  Windows .NET 8 and .NET 10 directly load it and pass their offline suites and
  complete two-promotion Sentinel TLS matrix. Direct C++23 TLS domain smoke and
  native Go/Rust two-promotion TLS also pass on Windows. Linux GCC Debug/shared
  Release, ASan/UBSan, formatting, and the corresponding TLS matrix remain
  passing.
- **Remaining Windows scope:** Automated RID packaging, signing and binary-ABI
  automation remain unqualified. They are separate from the now-proven
  compiler, DLL, managed-loading, and server-authenticated TLS boundaries.
