# Verdandi

Verdandi is a language-neutral distributed coordination layer for service
discovery, leader election, persistent Catalog key/value synchronization,
runtime-state synchronization, desired-configuration delivery, and application
acknowledgements.

The initial transport and active-state implementation requires a qualified
Redis 8 deployment. Both a fixed standalone Redis primary and a
Sentinel-managed primary/replica deployment are in scope. Redis Pub/Sub carries
normal Registry and Catalog events but is not durable; both domains repair from
authoritative Redis state after an acknowledged subscribe/read/PING alignment.

## Project Status

Verdandi is at non-production Alpha version `0.1.0`. The four generated,
operation-specific, positional-ABI Registration Lua programs and the Go, Rust,
and C++23 Register/Selector SDK slices are implemented. A managed C# facade now
exposes the same compiled C++23 runtime through C ABI v1 without duplicating the
Redis protocol implementation. Catalog's four generated
Lua operations, stateless Publishers, Subscribers, stable Entries, per-load generic
decoding, and optional bbolt/redb/SQLite recovery checkpoints are also implemented.
All native SDKs use one root Redis transport Client with Registration and Catalog
domain Clients attached to it. The Go SDK requires Go 1.27 and uses generic
methods for typed Registration, Selector, and Entry loading. Its root Client is
a thin connection/Key/Hash wrapper; Zone and worker lifecycle belong to each
Registration or Catalog Client, and root close does not wait for them. Rust
retains an awaited close because Fred shutdown is asynchronous, while dropping
the last public root handle performs only best-effort cleanup. C++23 uses a
compiled Boost.Redis core, `std::expected`, compile-time field descriptors, and
SQLite checkpoints; templates remain at the typed Fields boundary. Its C ABI
v1 exposes the same runtime to C11 and C++11/14/17. A header-only C++11 facade
adds RAII and typed domain APIs without duplicating protocol state. The C#
assembly targets .NET 8 and .NET 10, uses source-generated P/Invoke, dedicated
SafeHandles, static generic field codecs, and synchronous borrowed Selector
policies. Its independent Linux x64 regression covers .NET 8/10 offline and
self-contained builds, ACL-protected Redis 8.8 Standalone behavior, explicit
  and application-directory native loading, concurrency and capacity boundaries,
  concurrent disposal and finalizer cleanup,
  plus two Sentinel promotions with acknowledged-write-loss repair and Selector
  generation recovery. Windows/macOS native binaries, NuGet RID packaging,
  NativeAOT/trimming, TLS, cross-language C# peers, performance, and endurance
  qualification remain open. The unit,
Redis 8.8 integration, cross-language Pub/Sub,
disconnect/recovery, bounded per-UUID event coalescing, race, fuzz, lint,
retained expiry recovery, and current per-Registration two-hour fault plus
Sentinel campaigns are recorded in
[`test-results.md`](test-results.md). The current Catalog Hash/ZSET/Pub/Sub
revision, tombstone floor, JSON-safe maximum revision, checkpoint recovery,
reconnect, exact-base writer contention, both-language integration, Linux race,
  and Go/Rust interoperability tests pass on Redis 8.8. The C++ SDK passes strict
  GCC, clang-tidy, ASan/UBSan, authenticated Standalone integration, and an
  isolated three-node/three-Sentinel startup/integration smoke; the same compiled
  core also passes two promotions through C ABI v1 and the C# facade. Leader election, desired state, and
acknowledgements remain unimplemented. Version `0.1.0` is an API and integration
preview without a production or stable wire-compatibility promise. The future
`1.0.0` release remains reserved for the complete documented scope, including
qualified Leader election and standard English production-source comments.
No stable wire protocol has been released.
The repository is not ready for production use. C++ still requires a direct
native-API two-promotion harness, live TLS, multi-compiler/platform, packaging,
performance, and soak qualification before release.

The first reviewable Alpha source freeze, its edge-coverage audit, exact short
regression, and remaining gates are recorded in
[`freeze-20260831.md`](freeze-20260831.md). Both detached twelve-hour
Registration and Catalog endurance campaigns completed successfully against
that exact commit. The `0.1.0` release scope and evidence are summarized in
[`release-0.1.0.md`](release-0.1.0.md).

Go additionally exposes the root's borrowed `*redis.Client`, permanent `Done`
signal, and normalized operation timeout so child domains and advanced Go
integrations can reuse one pool without an internal bridge. The root alone owns
driver close. Raw operations are controlled by Redis ACLs and sit outside
Verdandi validation and atomicity guarantees; Rust keeps Fred private.

Leader uses an independent Campaign readiness token and Campaign Version. The
existing Registration Version field and Register/Update behavior are outside
the Leader design and remain unchanged. Existing Register/Selector
qualification is not evidence for Leader.

## Design Principles

- The protocol is language-neutral. Go, Rust, and C++23 are native SDK
  implementations. C# is an implemented language-native managed facade over
  the stable C ABI and same C++23 core; neither set is the permanent language
  boundary.
- Applications exchange versioned contracts rather than importing one
  another's implementation packages.
- Redis key names and existing meanings are forward-compatible and unversioned;
  compatible evolution adds optional fields or new keys.
- Service selection is performed from an immutable local view and never
  requires Redis access on the selection hot path.
- Pub/Sub events are disposable. Aligned Registration `register`, `update`,
  `renew`, and `unregister` events update live local views; reconnect and
  revision gaps recover from authoritative state.
- Registry synchronization uses a paginated membership Hash, per-Registration
  revisions, and an ordered subscription PING/PONG fence. Catalog uses one
  global Redis-owned revision, per-field update revisions, live/deleted ZSET
  indexes, complete Pub/Sub operations, and the same ordered fence. Pub/Sub is
  never the authoritative recovery log.
- Leader election separates candidate readiness from one private ownership
  term and is independent from service Registration. Each Campaign owns a fresh
  private readiness token and immutable positive-integer version; it needs no
  separate public ID. The SDK maintains the ready view, drives comparison,
  Claim/retirement/renewal, and exposes application lifetime.
  Redis only atomically validates readiness and the empty/exact-token ownership
  transition. Larger versions are preferred and equal versions are
  first-successful-claim wins. Each domain has zero or one
  application-active Leader; uncertainty closes admission and may leave it
  without one. Sentinel activation additionally requires a
  deployment-provided durable fence acquired by the SDK.
- Desired state is preferred over imperative commands. Command delivery is
  deferred from `1.0.0` until a concrete use case is approved.
- Every payload, queue, retry loop, decoder, task set, snapshot, and local
  cache has an explicit resource boundary.
- Deployment scale is qualified by measurement and partitioning rather than a
  compile-time fleet-size constant.

## Protocol Roles

- **Node** receives a fresh Registration UUID on every process start, publishes
  a `verdandi:registration:<zone>:<type>:<uuid>` Hash whose `@` Meta, `.name`
  Attr, and unprefixed Data fields expose Redis-managed state, immutable Attr,
  and mutable fixed-structure Data, refreshes its immutable-TTL lease through
  `renew`,
  consumes desired state, and owns its acknowledgements.
- **Publisher** mutates persistent Catalog KV, publishes desired configuration,
  and observes convergence.
- **Selector** maintains a validated local Registry view for
  application-owned filtering and load balancing.
- **Campaign/Leader** coordinates one independently identified, leased,
  version-aware exclusive term.
- **Administrator** provisions Zones, credentials, and deployment policy. A
  Registration Client atomically fills missing defaults in the non-expiring
  `verdandi:config:<zone>` Hash during bootstrap; an authorized backend can
  later replace related limits with one multi-field `HSET`.

```text
Administrator -> Publisher
                     |
                     v
             Redis current primary
                ^             ^
                |             |
              Node         Selector

Redis Sentinel -> resolves and monitors the current Redis primary
```

## Repository Shape

```text
lua/                  shared Lua sources and generated Redis atomic operations
sdk/<language>/       independently versioned language SDKs
testkit/              shared vectors and cross-language conformance tests
*.md                   protocol, architecture, decisions, SDK, and test records
```

Language manifests and toolchain configuration belong under the corresponding
`sdk/<language>` directory. The repository root remains language-neutral.

Registration's four lifecycle actions execute four specialized atomic scripts
generated deterministically from reviewed shared fragments. Each logical
mutation still performs exactly one `EVALSHA`; the SDK selects the action's SHA
and reloads only that script after `NOSCRIPT`. Fixed request controls use
operation-specific value positions; only dynamic Attr/Data retain field names.
SDKs validate and flatten the complete request before Redis I/O. Lua is only
the atomic glue for current revision state, Redis time, Hash/membership expiry,
reply, and publication; it does not validate application fields or decode their
values.
Selector intentionally uses bounded `HSCAN`/pipelined reads plus the subscribed
connection's PING/PONG barrier rather than a Registry-wide Lua snapshot. See
[`lua/README.md`](lua/README.md) for the executable contract and integration
test entry point. The accepted Register/Update/Renew micro-optimizations, exact
positional ABI, measurements, qualification, and Redis Hash-field expiry
boundary are recorded in
[`registration/lua-optimization.md`](registration/lua-optimization.md). Clients read the Zone's shared Registration limits before
initialization. They retain the last valid local snapshot and refresh it
at the Redis-configured interval or on demand, while steady-state partial
Update does not read the configuration key or scan the complete Registration
Hash for capacity accounting.

Each successfully published Registration owns one single-slot Fields merge
mailbox, one capacity-one wake signal, one long-lived synchronization worker,
and one renewal timer. The mailbox stores only the latest pending Version and
value for each changed Data field; later values overwrite earlier values before
the worker takes the batch. A small configurable admission semaphore (default
8, range 1..256) bounds result waiters without allocating an equal number of
request or full-Data objects. Typed and raw values are encoded or detached
before merge, so the SDK never retains an application struct. The worker
remains the sole Redis writer for its UUID and serializes Register, merged
Update, Renew, recovery, and Unregister without coupling unrelated
Registrations.
Each Selector owns one persistent Pub/Sub listener/state machine and may create
one temporary full-scan or targeted-repair task, so its hard topology is one
task in steady state and at most two during synchronization. Until the current
sync/repair fence succeeds, every public active, retained, snapshot, and policy
view returns explicit `unavailable`. First-version `One`/`Any` policy scans and
detached complete snapshots are intentionally O(N).

Catalog exposes exactly Replace, Patch, and Delete. One bounded Replace writes a
complete Value, contiguous Array, or Map using last-write-wins. Patch requires
the exact current `base_revision`, adds/overwrites Map fields, and overwrites
existing Array indices; field deletion and Array holes require Replace. Delete
removes the complete Hash and creates a fresh bounded tombstone.

Each Catalog Subscriber owns one persistent Pub/Sub connection/listener and
may create at most one temporary authoritative synchronization/repair task for
all of its exact Path and pattern subscriptions. The temporary task drains
coalesced work and exits when idle. Notifications carry
the complete accepted operation. A Patch with a missing local baseline,
reconnect, malformed frame, or checkpoint gap is repaired from the live/deleted
ZSET indexes and atomic per-path Read script; per-field revisions permit an
exact delta unless a later Replace requires a full Hash. Subscribers always
hold complete raw values in memory. Optional bbolt/redb/SQLite state is only a
monotonic disposable restart checkpoint. Go `Entry.Load<T>`, Rust
`Entry::load::<T>()`, and C++ `entry::load<T>()` decode the stable Entry into
the application-selected type without Redis or disk I/O. The current API and
performance record are in
[`catalog/api.md`](catalog/api.md) and
[`catalog/optimization.md`](catalog/optimization.md). The current whole-project
consolidation, lock-free Catalog qualification, exact scores, and remaining
weaknesses are recorded in
[`optimization-review-20260830.md`](optimization-review-20260830.md).
The C++23 build, native/C ABI/Legacy APIs, qualification, scores, and remaining release gates are in
[`sdk/cpp/README.md`](sdk/cpp/README.md) and
[`cpp-review-20260831.md`](cpp-review-20260831.md).

Registration recovery state is never persisted to local disk. A writer keeps
one bounded desired-state cache and its confirmation status in process memory
so it can suppress no-op writes, validate complete projected limits, transmit
field patches, and repair Redis while that process remains alive. Process
termination discards the cache; a restart generates a new UUID and the previous
Redis lease expires by TTL. Redis deployment settings own any Redis-side
durability, while historical audit storage belongs to a separate optional
synchronizer.

Selector keeps expired payload for at most one additional TTL in a separate
bounded, non-selectable process-memory view. It is also discarded when the
Selector process exits. Go, Rust, C++, and C# expose generic typed Registration/Selector
values whose application structs directly own field encode/decode behavior;
there is no Registration code generator or Schema object. Raw `Fields`
implements the same interfaces and remains a first-class binary boundary.
The implemented SDK APIs and lifecycle are described in
[`sdk.md`](sdk.md), with the child-package typed API and selection transaction
contract in [`registration/api.md`](registration/api.md). The managed C# API,
native-loading rules, examples, current evidence, and review are in
[`sdk/csharp/README.md`](sdk/csharp/README.md) and
[`sdk/csharp/REVIEW.md`](sdk/csharp/REVIEW.md). The direct
typed two-hour evidence, scores, strengths, and limitations are in
[`registration/typed-soak-20260825.md`](registration/typed-soak-20260825.md).
That run is retained as protocol history; the current per-Registration
Fields-mailbox/worker/timer model, one-plus-one Selector task model, resource evidence,
authoritative 7,866.527-Redis-second qualification, and final assessment are in
[`registration/concurrency-review-20260826.md`](registration/concurrency-review-20260826.md).
The implemented child-package migration, exact final-source fault gate, API
exposure audit, current score, and remaining trade-offs are in
[`registration/package-migration-20260826.md`](registration/package-migration-20260826.md).

## Redis-backed Registration Limits

The first Registration Client for a Zone fills only missing fields with these defaults:

| Redis Hash field | Default | Protocol ceiling |
| --- | ---: | ---: |
| `registration_attr_max_fields` | 16 | 128 |
| `registration_data_max_fields` | 32 | 128 |
| `registration_max_field_name_bytes` | 64 bytes | 64 bytes |
| `registration_attr_max_field_value_bytes` | 128 bytes | 16 KiB |
| `registration_data_max_field_value_bytes` | 128 bytes | 16 KiB |
| `registration_max_bytes` | 16 KiB | 64 KiB |
| `configuration_refresh_ms` | 30,000 ms | 86,400,000 ms |

The Hash also contains `protocol=v1` and has no TTL. Defaults are deployment
policy, not immutable constants: an administrative backend may change them
within the protocol ceilings. Each multi-field policy transition must use one
Redis `HSET`; `configuration_refresh_ms` itself is part of that atomic policy
and accepts 1,000 ms through 24 hours. The first published Registration starts
one Client-shared poller, all published Registrations share it, and the last one
stops it. `Register` refreshes immediately before validation, and explicit
refresh remains available without a Registration. An invalid refresh is
reported and leaves the previous valid snapshot and interval active. Lowering a
limit affects later Registration content writes; existing records remain
discoverable and lease renewal remains permitted.

Verdandi v1 now defines one strict cross-language JSON configuration structure:
[`configuration.schema.json`](configuration.schema.json) is the machine-readable
contract and [`configuration.example.json`](configuration.example.json) is the
complete example loaded by the native SDK test suites. Go, Rust, and C++ first
parse that same DTO, then convert it to their language-native Redis,
Registration/Selector, and Catalog configuration types. Unknown/duplicate
fields, `null`, trailing JSON, unsupported versions, and invalid ranges are
rejected before Redis, checkpoint, or certificate-file I/O. `redis.tls` is a
strict object supporting system roots, a private PEM CA bundle, Standalone SNI
override, and a paired PEM client certificate/private key; certificate files
are read only during native transport construction and are individually capped
at 1 MiB. Native structure methods remain the final validators; no parallel
exported constants surface or configuration code generator is introduced. See
[`configuration.md`](configuration.md) for defaults, ranges, zero semantics,
and loader APIs. C# passes the same strict UTF-8 JSON directly into the C++
parser through C ABI v1 and therefore owns no duplicate configuration model.

## Initial Scope

Version `0.1.0` exposes the implemented Registration, Selector, Catalog, root
Redis client, configuration, and language binding surfaces for distributed
development and controlled service integration. As a `0.x` release it may make
breaking API or protocol changes in a later minor version and carries no
production availability or compatibility guarantee.

Version `1.0.0` is expected to establish:

- per-start UUID Registration Hashes and leased liveness;
- paginated Registry synchronization and local Selectors;
- SDK-driven, version-aware zero-or-one Leader election with independent
  Campaign readiness tokens, private term tokens, and Sentinel fencing;
- one persistent raw Catalog Value per Path with last-write-wins Replace,
  exact-base Patch, explicit tombstone deletion, Pub/Sub notification, and
  authoritative Hash/ZSET recovery into complete local Subscribers;
- bounded observed-load synchronization;
- ACL-authorized, versioned, opaque desired-configuration delivery;
- explicit acceptance, activation, rejection, and expiry acknowledgements;
- identical protocol behavior in the first Go and Rust SDKs;
- standalone Redis and Redis Sentinel failover recovery; and
- separate qualification profiles with 500 live Proxy Registrations renewing or
  updating once per second and 10 Catalog mutations per second, without making
  those counts protocol limits.

Redis Cluster has no scheduled support horizon and every SDK rejects it rather
than silently degrading atomicity. Multi-primary merging, globally linearizable
state across partitions, exactly-once commands, and application-specific
routing semantics are also outside the first release.

## License

Verdandi is licensed under the [MIT License](LICENSE).
