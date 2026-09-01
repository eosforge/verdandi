# Verdandi Alpha Requirements

## 1. Document Role

This document defines the required outcome and acceptance criteria for the
first production-stable Verdandi release, version `1.0.0`. The current
`0.1.0` release is a bounded Alpha API and integration preview; publishing it
does not satisfy or weaken this `1.0.0` contract. This is a version-specific
contract, not a general project history. Stable architectural decisions belong in
[`codex.md`](codex.md), detailed mechanics belong in
[`architecture.md`](architecture.md) and [`protocol.md`](protocol.md), and live
execution state belongs in [`worklog.md`](worklog.md).

Status: **draft for maintainer review**. `Alpha` names the development phase.
Source and package metadata for the implemented preview is `0.1.0`, without a
production or stable compatibility promise. The first published `1.0.0`
artifacts remain immutable and require Leader plus the complete acceptance
matrix below. No implementation may present these requirements as a released
guarantee until the remaining protocol and trust decisions listed here are
resolved.

## 2. Alpha Outcome

Alpha succeeds when independently implemented Go and Rust SDKs can use the same
language-neutral protocol for the complete release matrix below. C++23 is an
additional implemented SDK and must preserve the same contract as its own
qualification expands. C# is an additional managed facade over C ABI v1 and the
same C++23 core; it must preserve observable behavior without being counted as
a fourth independent protocol implementation:

1. connect through either a fixed Redis primary or Redis Sentinel;
2. drive per-start UUID Registrations through aligned `register`, `update`,
   `renew`, and `unregister` mutations;
3. publish and recover leased Registry records without a fixed service-count
   limit;
4. maintain validated local Selector views with no Redis operation on the
   selection read path;
5. coordinate independently identified, leased, version-aware Leader terms;
6. synchronize persistent revisioned Catalog key/value namespaces;
7. synchronize bounded coarse load samples;
8. deliver ACL-authorized, versioned, opaque desired-configuration snapshots;
9. activate accepted configuration atomically through consumer callbacks;
10. publish stable acknowledgements and bounded diagnostics;
11. recover after Pub/Sub generation loss, reconnects, and primary failover; and
12. pass the same protocol vectors and cross-language integration matrix.

The `0.1.0` preview intentionally exposes only the implemented root Client and
configuration surfaces, Registration/Registry/Selector lifecycle, persistent
Catalog synchronization, and their Standalone/Sentinel recovery paths. It does
not claim Campaign/Leader, desired configuration, acknowledgements, complete
load synchronization, production availability, or a stable wire contract.
Consumers may begin distributed development and controlled integration against
these APIs with the normal SemVer expectation that a later `0.x` minor release
may contain breaking changes.

Go and Rust remain the first complete-matrix SDKs required to prove language
neutrality. C++23 is the third implementation, currently with a narrower
recorded qualification boundary. C# is the first implemented non-C++ language
binding over C ABI v1 and currently has an independent Linux x64 Standalone and
two-promotion Sentinel qualification boundary. Its platform, packaging, TLS,
performance, and endurance matrix remains narrower. The protocol, schemas,
testkit, naming, and public concepts must not prevent future SDKs in other
languages.

## 3. Required Repository Foundation

Alpha must establish this language-neutral layout without placing package
manager configuration at the repository root:

```text
spec/
schema/
lua/
sdk/go/
sdk/rust/
sdk/cpp/
sdk/csharp/
testkit/vectors/
testkit/conformance/
testkit/standalone/
testkit/sentinel/
```

Requirements:

- Redis field, scalar encoding, and error definitions have one
  language-neutral source of truth.
- Generated files identify their source and are never edited manually.
- Registration Lua executables are generated deterministically from one
  reviewed shared-fragment manifest. Each lifecycle mutation selects exactly
  one operation-specific script and remains one atomic Redis execution.
- Redis keys and Lua script entry points are protocol-owned; applications do
  not construct keys or call raw scripts directly.
- Each SDK owns its manifest, dependencies, lint configuration, tests, and
  package release below `sdk/<language>`.
- Protocol and SDK versions are distinct and recorded in compatibility
  metadata.
- Protocol `1.0` defines only mandatory behavior and carries no protocol
  capability list. Registration service capabilities are application metadata.
- Existing Redis key names, data types, and meanings remain forward-compatible;
  compatible evolution adds optional fields or new keys.
- Client bootstrap fills missing defaults in one non-expiring
  `verdandi:config:<zone>` Hash. Ordinary runtime APIs cannot mutate it; an
  authorized administrative backend may later replace related limits and the
  shared refresh interval atomically. Clients retain the last complete valid
  snapshot.
- Shared fixtures are byte-exact and consumable without language-specific
  preprocessing.
- Only `README.md` from the current Markdown set and the MIT `LICENSE` artifact
  are public. Planning documents remain local until explicitly promoted.

## 4. Required Public Concepts

Each SDK must expose an idiomatic, narrow equivalent of these concepts without
copying one language's syntax into another:

- `Client`: owns topology resolution, authenticated Redis connections,
  subscriptions, retry admission, lifecycle, and shutdown.
- `Node`: owns one process start's UUID Registration, lease renewals, desired-state
  watcher, and acknowledgements.
- `Publisher`: owns Catalog KV mutation and desired-configuration publication.
- `Selector<T>` or the language-equivalent typed selector: owns an immutable
  local Registry view and exposes application selection inputs.
- `Campaign` and `LeaderTerm`: own candidate readiness, exclusive term
  acquisition, fail-closed local admission, and joined release.
- Catalog `Publisher`, `Subscriber`, stable `Entry`, and generic `Entry.Load<T>` or
  language equivalents: bind application-owned codecs per Path while keeping
  the Redis state raw and complete in memory.

Public domain APIs must not expose types belonging to a selected Redis driver,
serialization library, task runtime, or Bifrost contract. Configuration and
errors exposed by the SDK are Verdandi-owned types. The Go root transport is a
reviewed language-specific exception: `Client.Redis()` returns its borrowed
`*redis.Client` so advanced integrations and child domains can reuse one pool.
It transfers no ownership, is not a cross-language conformance surface, and
places raw operations outside Verdandi validation under the caller's Redis ACL.
Rust keeps Fred private.

## 5. Connection Modes and Lifecycle

Alpha qualifies Redis Open Source 8.0.0 or later in an explicitly tested Redis
8 line, or a documented compatible service. Registry membership and Campaign
readiness use independently expiring Redis Hash fields.

### 5.1 Standalone

Standalone accepts one configured Redis endpoint and explicit authentication,
TLS, deadline, pool, and connection-recovery limits. It does not silently attempt
Sentinel behavior.

### 5.2 Sentinel

Sentinel accepts:

- a bounded non-empty set of Sentinel endpoints;
- one master name;
- independently configurable Redis and Sentinel credentials;
- explicit TLS and identity policy for each connection class; and
- bounded resolution, connection recovery, and jitter policy.

After a Redis connection failure, the client resolves the current primary
again. It must not reconnect to a remembered former primary when current
Sentinel resolution fails. Replica reads are excluded from correctness
decisions.

Before a Client becomes Ready, it fills missing common defaults and validates
the recognized fields in `verdandi:config:<zone>`. It retains the last complete
valid snapshot and adopts later ACL-authorized backend changes through periodic
or explicit refresh. A missing, malformed, incompatible, or above-ceiling
refresh is reported without replacing that snapshot. Redis/Sentinel endpoints,
credentials, TLS, deadlines, connection recovery, concurrency, and local
buffers remain local bootstrap/operational configuration. Dispatched Redis
commands are not automatically retried.

### 5.3 Client State

Both modes follow the same observable state progression:

```text
Disconnected -> Resolving -> Connecting -> Subscribing -> Synchronizing -> Ready
```

Every connection and subscription generation is fenced. `Ready` requires a
confirmed subscription followed by an authoritative state load. Dropping or
cancelling the client joins owned tasks and releases connections, timers,
buffers, and queue capacity.

## 6. Registration and Leases

A Registration is one process-owned Redis Hash at
`verdandi:registration:<zone>:<type>:<uuid>`. Its public SDK view contains
`Meta`, `Attr`, and `Data`, while Redis stores their top-level fields directly:

- Redis/Lua-managed Meta is exactly `@uuid`, `@revision`, `@timestamp`, `@ttl`,
  and `@version`;
- immutable SDK-supplied Attr uses `.name`; and
- mutable fixed-structure Data uses an unprefixed `name`.

Data names may not begin with `&`, `@`, or `.`. Attr and Data nested structures
remain one opaque top-level field value. `update` mutations omit unchanged
Data fields and set typed zero or null values explicitly; fixed Data has no
field-unset operation.

The process SDK generates UUID once and retains it across Redis disconnects.
Every published Registration owns one independent single-slot Fields merge
mailbox, one capacity-one wake signal, one long-lived synchronization worker,
one renewal timer, and a positive content
revision. That worker serializes at most one Redis mutation in flight for its
UUID. Initial registration, uncertain recovery, and reset publish
complete `register` state. A non-empty `update` carries changed Version and/or
Data and advances revision. A `renew` carries UUID, unchanged revision, and
the Redis-generated timestamp; TTL and Attr are immutable for the UUID lifetime.

Registration initialization obtains the Client's current last-valid Zone limits.
The Registration worker keeps its complete encoded desired record and
confirmation status in one bounded process-memory cache and validates the projected Attr/Data field
count, individual field sizes, and total stored field-name-plus-value bytes
before advancing revision or calling Redis. This cache is volatile: the SDK
writes no Registration content, UUID, replay log, local database, or WAL to
disk. A process restart discards it, generates a new UUID, and never restores
the previous Registration identity. Lua does not repeat SDK request, field, or
capacity validation; a steady-state new-revision Update performs no
complete-Hash capacity scan.

One Lua execution obtains Redis `TIME`, stores `@timestamp`, computes the
deadline as `timestamp + ttl`, rejects a value above Redis 8's Hash-field
absolute-expiry ceiling `2^46-1`, changes the Registration, sets its absolute
key expiry, changes the Registry UUID field plus matching field expiry through
`HSETEX PXAT`, and publishes one event. `@expire` is not stored. The private
request ABI uses fixed control-value positions selected by the operation SHA;
only dynamic Attr/Data remain named field/value pairs. Graceful shutdown
atomically deletes the complete Registration and membership field through
terminal UUID-only `unregister`; disconnected shutdown relies on TTL and never
sends a delayed unregister. Replies and Pub/Sub events remain named and
self-describing.

Selectors use a periodically calibrated connection-generation RedisClock and
the transmitted `timestamp` plus cached TTL to derive conservative local
deadlines. Normal synchronization performs no `PTTL` or `HPTTL`; those commands
are diagnostic/fallback tools. Natural Redis TTL expiry and conservative local
expiry must both be tested.

## 7. Registry, Catalog KV, Leadership, and Selectors

### 7.1 Registry and Selectors

Each Zone/Type Registry uses Node-owned leased Registrations and one paginated
membership Hash whose UUID fields have matching field TTL. The Registry Hash
key and Pub/Sub channel are both `verdandi:registry:<zone>:<type>`. There is no
Registry-wide revision, mutation history, Stream, or barrier event. Pub/Sub and
the SDK lifecycle use the same four string kinds: `register`, `update`, `renew`,
and `unregister`.

Each published Registration must own one independent single-slot Fields merge
mailbox, one capacity-one wake signal, one long-lived synchronization worker,
and one renewal timer. The worker serializes all Redis mutations for that UUID.
Pending Updates merge by last Version/Data-field value; the mailbox stores no
full request queue or application struct. Registrations must not share a
process-wide write queue, because backlog and failure isolation belong to each
Registration.

A Selector must:

- own one persistent Pub/Sub listener/state-machine task and at most one
  temporary task shared by full synchronization and targeted repair;
- subscribe and receive its acknowledgement before any authoritative scan;
- begin a bounded, one-logical-change-per-UUID event buffer;
- load the membership Hash and complete Registrations through bounded pages and
  bounded fetch concurrency;
- send a nonce-bearing PING on the subscribed connection after the final read,
  wait for its ordered PONG, and reconcile buffered events by Registration
  revision;
- repair a per-UUID revision gap with one complete fetch plus another PING/PONG
  fence;
- restart synchronization on a disconnect, hidden reconnect, PONG timeout,
  repeated gap, buffer overflow,
  malformed page, incompatible record, or changed subscription generation;
- build a complete candidate view off the read path and atomically replace the
  public immutable view only after synchronization;
- expire stale Nodes conservatively and remove them from selection immediately;
- retain non-explicitly expired or fenced-absent payload only in a configured
  time- and byte-bounded recovery cache, never as a selectable Node;
- purge explicitly unregistered payload immediately; and
- apply subsequent register/update/renew/unregister Pub/Sub events as the normal
  path;
- refresh liveness from the event's Redis timestamp and cached TTL through the
  current RedisClock without a routine Redis read; and
- perform no Redis call, blocking I/O, unbounded allocation, or mutable global
  traversal during ordinary selection reads.

The protocol defines no maximum Types, Nodes, or Registrations per Registry.
Every page, record, event buffer, fetch group, synchronization duration, queue,
and local byte budget is configured and bounded independently. A Selector that cannot fit its configured local byte
budget fails explicitly; that deployment limit is not a protocol count limit.

Verdandi may expose deterministic iteration and simple round robin as utility
behavior. Capacity-aware, weighted, locality-aware, least-loaded, and
reservation-aware algorithms remain application-owned.

### 7.2 Catalog KV

A Catalog is one persistent revisioned raw Value, contiguous Array, or Map at
`verdandi:catalog:<zone>:<part>:<id>`. It is separate from leased discovery
and immutable desired documents. Application codecs own every external
structure; Verdandi stores only flat binary fields and control metadata.

Publisher exposes exactly one bounded atomic Replace, strict Patch, and complete
Delete. Replace/Delete are last-write-wins. Patch requires the exact current
base revision, adds/overwrites Map fields, and overwrites existing Array
indices. Value changes, removal, append/truncate, holes, and shape changes
require Replace. There is no external Path lock: Patch commits only when Lua
still sees the exact base used by the SDK projection, so concurrent same-base
writers yield one success and stale failures. A transport failure after sending
a mutation is ambiguous and must be aligned before another Patch.

The Zone owns one Redis revision in `1..=2^53-1`. Live Hashes store current
revision, latest Replace revision, shape, encoded bytes, and complete fields.
Per-field revision ZSETs support exact delta repair. Live/deleted/deleted-time
Zone ZSETs index Paths; bounded tombstone eviction alone advances the floor.

Lua atomically commits state, indexes, revision, and a complete MessagePack
Replace/Patch/Delete notification on the Path channel. Pub/Sub is at-most-once,
so it is only the fast path. One Subscriber multiplexes every exact/Part/Zone
subscription on one persistent listener and owns at most one temporary
synchronization/repair task. It subscribes before
authoritative alignment, fences with subscribed PING/PONG, applies Patch only
on the exact local base, and repairs through atomic Read. A cursor below floor
performs complete index alignment.

Subscribers keep all covered complete raw values in memory. A stable Entry
survives deletion and recreation and reports synchronized state explicitly.
Go `entry.Load<T>()` and Rust `entry.load::<T>()` decode the application type
locally; different Paths need not share a type.

Optional bbolt/redb/SQLite is a disposable, default-off, monotonic restart checkpoint.
Redis remains authoritative, checkpoint failure disables later persistence for
that Client generation, and local state is never replayed back to Redis. The
complete Catalog defaults to 512 KiB and is configurable up to 4 MiB; pages,
concurrent reads, decoder input,
synchronization, diagnostics, and checkpoint work are bounded independently.
### 7.3 Leadership

A Campaign does not require a Registration. Its private random readiness token
is the Campaign lifetime's internal identity; every ownership attempt uses a
different private ownership token and lease. Campaign `version` is immutable
for the Campaign lifetime and is a positive integer in
`1..9007199254740991`; every SDK uses the same numeric comparison and prefers
the larger value. Changing version requires closing the Campaign and creating
another readiness-token/version pair. The SDK attempts claim only when its
local ready view considers it best; Redis verifies the exact readiness token,
matching version, and empty ownership atomically.
Equal versions are first-successful-claim wins. A live owner is not preempted;
after observing a larger ready version it invalidates new local admission,
joins term-owned cleanup, and exact-token releases before replacement.

Every election domain has zero or one application-active Leader. Every Leader
term exposes a synchronous local validity check backed by cancellation and a
conservative monotonic deadline. An ambiguous or failed renewal invalidates the
local term immediately. The SDK cancels and joins all term-owned work before
exact-token release; a replacement starts only after the prior application
cleanup and required fence handoff. Exact release publishes one Pub/Sub wake to
reduce handoff latency, while bounded retry and the Redis lease remain the
correctness path. Temporary absence is accepted; overlapping active Leaders
are not.

Standalone uses the configured Redis primary as its term authority. A Sentinel
Campaign must acquire one deployment-provided durable fence or advisory lock
after Redis claim and before exposing a LeaderTerm or invoking application
code. After acquisition it exact-token confirms Redis ownership once more;
failed confirmation releases the fence without starting the callback. It holds
an active fence until term-owned application cleanup finishes. A
promoted-primary claimant waiting for the same fence is not
application-active. Missing or failed fencing leaves the domain without an
active Leader.

## 8. Observed Load

Load synchronization is coarse state, not a per-connection event stream.

- Nodes publish after a minimum interval, lease-renewal need, readiness change,
  capacity-bucket transition, or configured absolute/relative threshold.
- Publications are debounced and rate limited.
- Registration events identify the UUID and revision and carry changed load
  fields inline.
- Selectors normally apply contiguous load changes without a Redis read.
- Reconnect performs acknowledged Registry subscribe/scan/PING synchronization.
- A consumer may add a bounded local pending reservation immediately after a
  selection and reconcile it against later Registration patches.

No selection operation may synchronously publish load or wait for Redis.

## 9. Desired Configuration

Desired configuration uses complete immutable snapshots in Alpha. The
Verdandi envelope must contain:

- protocol and payload schema identifiers;
- target service, partition, or Registration UUID scope;
- target revision;
- issue, not-before, and expiry times;
- desired lifecycle state;
- content type and encoding;
- uncompressed length and cryptographic content hash;
- complete opaque payload bytes.

Zone is implicit. Target is partition, service-within-partition, or one
Registration. Multiple Publishers use Redis-revision last-write-wins; the
highest successfully installed current revision is authoritative.

Before handing a publication to the consumer activation callback, the SDK
validates envelope structure, target, time bounds, revision rules, declared
sizes, hashes, supported encoding, and ACL-authorized Redis context.

The consuming application validates payload semantics and constructs a new
runtime state without mutating the current state. Only complete success allows
one atomic application-visible activation. Partial application is prohibited.

A cold process may not activate an expired or invalid snapshot merely
because bytes are locally available. Any anti-rollback watermark persisted by
the SDK is minimal security state, not operational configuration.

## 10. Acknowledgements

A Node owns its acknowledgement key and may report:

- received;
- validated;
- active;
- rejected;
- expired;
- draining or drained.

An acknowledgement carries the Registration UUID, desired target and revision,
Registration-local ACK revision, bounded stable error code, bounded safe
diagnostic, activation time when applicable, and TTL. A Publisher decides
convergence from acknowledgements, not wake delivery.

Lifecycle intent such as active, drain, offline, configuration revision, and
weight is desired state. Command delivery is deferred from SDK `1.0.0` until a
concrete imperative use case is approved.

## 11. Security Acceptance

Before the stable `1.0.0` release:

- Redis and Sentinel authentication are separate in configuration and tests.
- ACL fixtures prove the configured role/Zone/key scope and command set.
- SDK/Lua actions preserve owned multi-key invariants. Deliberate raw mutation
  by a principal granted write permission is outside the protocol guarantee.
- A Selector cannot publish shared topology or impersonate acknowledgements.
- Unauthorized writes, wrong targets, invalid time windows, stale
  Registrations, revision rollback on an unchanged Redis connection, and
  invalid hashes are
  rejected.
- Pub/Sub payloads contain no secret or complete configuration.
- Logs, errors, and tracing fields are checked for secret leakage. A future
  external statistics/audit service owns metrics and history.
- Malicious size/count fields fail before allocation or slicing.
- Every rejection is bounded in work, memory, and diagnostic size.

Development tests may use local unsecured Redis only when the fixture makes
that mode explicit. It must not become a production default.

## 12. Resource Acceptance

Every SDK must enforce configured limits for:

- Registration and acknowledgement bytes;
- desired document compressed and uncompressed bytes;
- chunk bytes and chunks per manifest;
- registry/Catalog page entries and bytes;
- Catalog mutation operations and encoded bytes per atomic batch;
- registry/Catalog event-buffer count, age, and bytes;
- synchronization duration and barrier timeout;
- local snapshot memory;
- concurrent document and chunk fetches;
- Redis command and Pub/Sub connections;
- queued notifications and diagnostics;
- retry concurrency and backoff state;
- acknowledgement retention; and
- shutdown duration.

Limits must be validated before work begins where possible. Exceeding one
returns a stable capacity error, does not expose partial state, and does not
discard the last valid state before its independent lease expires.

## 13. Cross-Language Acceptance

Shared test vectors must cover:

- valid and invalid Redis field/scalar encodings;
- byte-exact hashes and chunk assembly;
- unknown-field and unsupported-version behavior;
- revision and Registration-lifecycle transitions;
- chunk order, count, length, and hash failures;
- paginated Registry scan/PING fencing plus Catalog Hash/ZSET/Read alignment,
  Pub/Sub loss, floor fallback, and tombstone reconciliation;
- Catalog Value/Array/Map Replace, exact-base Patch, Delete/recreate,
  multiple-writer last-write-wins conflict, field-level repair, checkpoint
  recovery, and ambiguous-write reconciliation;
- Campaign-without-Registration readiness, independent token/version lifecycle,
  Leader acquisition, version retirement, ready-view loss, and token-fenced
  release;
- lease and queue-delay calculations;
- stable string error/status mappings;
- every Redis Lua input, output, and error condition.

The first release must prove at least:

- Go Publisher to Rust Node and Selector;
- Rust Publisher to Go Node and Selector;
- Go Registrations consumed by Rust;
- Rust Registrations consumed by Go;
- Standalone and Sentinel paths produce the same protocol state; and
- each SDK follows the declared compatibility window.

No SDK is conformant merely because its own SDK talks to itself.

## 14. Failure Qualification

Real integration tests must cover:

- Redis command timeout and ambiguous write reconciliation;
- missed Pub/Sub messages and revision-gap reload;
- complete Pub/Sub disconnect and resubscription fencing;
- Standalone reconnect;
- Sentinel primary termination and automatic promotion;
- Sentinel disagreement and temporary total Sentinel loss;
- promoted-primary Lua script-cache loss;
- acknowledged-write loss followed by self-healing republish;
- Registration UUID collision, expiry, graceful removal, and restart surge;
- paginated registry and Catalog synchronization during concurrent mutation;
- Registration revision gaps, PONG timeout/ordering, buffer overflow, and
  unsynchronized Catalog Entries/Subscribers;
- concurrent Campaigns, Leader retirement, exact-token release, and local term
  expiry;
- Publisher restart, revision continuation, wake delivery, and failover rollback;
- malformed, incomplete, oversized, incompatible, and expired documents;
- cancellation during every network and activation phase; and
- clean joined shutdown without leaked tasks, connections, timers, or buffers.

## 15. Capacity Qualification

Alpha must test 500 live Proxy Registrations under two profiles: one `renew` per
Registration per second with unchanged content, and one documented-size
`update` per Registration per second. Each profile also carries 10 Catalog
mutations per second and covers reconnect and restart surge of that same
population. An Update already refreshes the lease, so the profiles do not add a
redundant Renew to the same second. The result is a qualification point, not a
hard protocol ceiling.

The protocol and Lua actions must contain no service, Node, or Campaign count
ceiling; Catalog is one complete bounded Value. Capacity tests additionally increase population until
the tested deployment misses its latency or resource objective, recording the
measured boundary rather than turning it into a wire constant.

Reports must include hardware, operating system, Redis version and
configuration, Sentinel topology, network placement, SDK versions, connection
counts, payloads, cadence, CPU, memory, allocations, command latency, snapshot
latency, failover duration, and full recovery duration.

Large deployments may partition by region, cell, tenant-independent failure
domain, or another explicit placement boundary for fault and capacity
isolation. Cross-partition atomic Catalog mutations are not an Alpha guarantee.

## 16. Delivery Stages

### Stage 0: Freeze the Contract

- Resolve serialization, compatibility, error taxonomy, key identifiers,
  ACL ownership, configurable limits, synchronization barriers, and
  Leader policy.
- Produce schemas, Lua contracts, and adversarial vectors before SDK network
  clients.

### Stage 1: Standalone Coordination

- Catalog Publisher/Subscriber, stable Entry, generic per-load typing, and
  optional checkpoint are implemented in Go and Rust. Complete the remaining Client lifecycle,
  Campaign/Leader, load synchronization, and ACK work in both languages.
- Pass all same-language and cross-language Standalone tests.

### Stage 2: Sentinel

- Registration/Selector has added Sentinel resolution without changing the
  protocol and passed real promotion, reconnect, resubscription, acknowledged-
  loss repair, and state-recovery tests. Later data classes must independently
  extend that qualification.
- Campaign/Leader qualification must prove primary-generation invalidation,
  durable-fence acquisition before callback admission, joined fence handoff,
  and zero overlapping application-active terms. Without a fence, Sentinel
  Campaign remains unavailable.

### Stage 3: Desired Configuration

- Implement immutable desired snapshots, activation callbacks,
  acknowledgement convergence, and Redis-revision last-write-wins publication.

### Stage 4: Consumer Qualification

- Integrate Bifrost only through a released Verdandi package and opaque
  application payload.
- Prove Proxy and Dispatcher lifecycle behavior without importing Bifrost
  contracts into Verdandi.

### Stage 5: Capacity and Fault Qualification

- Retain the completed Registration baseline: 500 continuously live writers at
  500 updates/s for five minutes, a separate five-minute renewal phase, eight
  Selectors, 5,000-record pagination, the frozen-source 7,866.527-Redis-second
  fault campaign with 4,000,000 Updates, and two Sentinel promotions. Add the
  combined 10-Catalog-mutations/s, Publisher restart, and broader reconnect
  storm suites when their corresponding data classes exist.
- Publish measured boundaries and recommended deployment defaults.

## 17. Explicit Non-Goals

Alpha does not include:

- Redis Cluster;
- active/active or multi-primary state merging;
- a consensus implementation;
- cross-partition linearizable publication;
- Command delivery or exactly-once execution;
- an arbitrary workflow or task engine;
- application authentication, routes, origins, listeners, or business data;
- application-specific load-balancing policy;
- per-selection Redis requests;
- delta configuration updates;
- automatic backend abstraction for a non-Redis store; or
- a promise that every possible language has an SDK.

## 18. Remaining Decisions and Qualification Gaps

- Freeze remaining cross-language typed Attr/Data codecs and byte vectors
  beyond the accepted generated Go primitive codec. Registration Meta/Attr/Data
  fields, key/value event envelopes, and resource partitioning are accepted.
- Qualification remains for sustained maximum-size Register/reset fan-out above
  eight subscribers, prolonged driver-ingress pressure, RedisClock host-step
  behavior, broader reconnect storms, and combined Registration/Catalog load.
  The Go and Rust adapters already qualify bounded one-change-per-UUID
  coalescing, retained recovery, standalone PING/PONG ordering, cross-language
  interoperability, five-minute update/renewal phases, 5,000 records, a
  7,866.527-Redis-second Registration fault campaign, and a real Redis 8.8
  Sentinel topology. Catalog's separate 24-hour interval remains outside this
  Registration completion.
- Freeze the cross-language Sentinel fence adapter shape and qualify at least
  one durable advisory-lock implementation. The strict zero-or-one Leader
  policy and mandatory Sentinel fencing behavior are accepted.
- Audit canonical Catalog custom-field naming and flattening codecs; independent
  Hash records with reserved plus expanded custom fields are accepted.

These decisions require explicit maintainer approval. Dependency selection or
implementation work must not silently freeze them through accidental code.
