# Current Astra Architecture

This document owns confirmed component boundaries, authority, and implementation scope. The core includes Pulsar, Polaris, Star's three domains/multinode recovery, native Comet C++, and essential management. Catalog uses synchronous writes/internal versions; Beacon identity-preserving recovery and new read/selection APIs are wired. See the [SDK contract](../comet/cpp/README.md) and [protocol differences](../proto/README.md#confirmed-targets-and-implementation-gaps). [Implementation status](#status) and [executed validation](validation.md) are separate: design choices/source availability do not prove a pass.

## Components and stages

| Component | Responsibility | Boundary |
| --- | --- | --- |
| Pulsar | Infrastructure admission, membership, continuous Unix reference time | C++; nanosecond units, not nanosecond accuracy or physical-time business-version arbitration |
| [Polaris](../polaris/README.md) | Sole Almanac authority/persistence | Go/GORM/SQLite; deployment-enforced singleton initially, no election/automatic standby |
| Star | In-memory three-domain state, subscriptions, peer replication | C++26; no business SQLite or async business persistence |
| Comet | Native business access, views, object lifecycle | Initially comet/cpp; no Pulsar connection; other native languages follow later |
| Astrolabe | Essential management and live observation | Go; no own persistent database/external monitoring system |
| Orrery | Management/galaxy UI | Evolves from admin/; complete real interface deferred |
| Planet / Moon | Relay, edge access, aggregation | Proposed; not prerequisites for this business loop |

There is no architectural standalone fallback. Single-machine/All-in-One are deployment shapes using the same components/protocols, without bypassing initialization. Existing Planet scaffolding does not expand current scope. The deleted Go Supervisor is available only in Git history.

Polaris uses GORM's official CGO SQLite driver with WAL + synchronous=FULL; [Polaris](../polaris/README.md#sqlite) alone defines durability/connections/recovery. Pulsar's separate member database uses DELETE + EXTRA; neither database nor driver layer is shared.

Polaris remains independent of Astrolabe. Astrolabe has no persistent database; its restart/observation failures do not directly stop authority-to-Star synchronization or new-Star recovery. Management changes still pass through Astrolabe to Polaris, but management unavailability does not require running authority/data planes to stop.

## Three data domains

<a id="data"></a>

| Domain | Content/authority | Persistence | API direction |
| --- | --- | --- | --- |
| Almanac | Polaris-authoritative snapshots/continuous updates received by Stars | Polaris SQLite | Polaris Commit / Reader.load() |
| Ephemeris | Fixed Attr, mutable Data, TTL, same logical ID recovered by registration generation | No business persistence | Client.beacon() / Beacon.update() / Observer.one() |
| Catalog | TTL data; application ensures one publisher per Key; SDK manages versions | Application persists what it needs | publisher.update(batch, ttl) / subscriber.watch() |

Domains use native structures, not a unified map<Sector, map<Spectrum, Store>>. Sector/Spectrum address business data. Almanac is Scope-based; Catalog/Ephemeris group by source Star, each source/domain having one replication sequence spanning every Scope. Local groups broadcast directly accepted facts only; remote replicas never rebroadcast. [Storage](../common/README.md#data) owns records/index/version details.

### Almanac

Polaris is the sole writer, Astrolabe the management entry; Stars do not replicate Almanac among themselves. Single/atomic multi-key Set/Delete advances Scope authority exactly +1. Success means Polaris durable commit, not installation everywhere. Atomic batches have no Key-count/aggregate-byte cap; receive large requests in segments and commit once. Oversized frames/insufficient bounded history recover the whole Scope; normal restart preserves versions. [Polaris](../polaris/README.md#assessment) defines transactions, original-request receipts, and installed-version assessment.

Reader/Subscriber watch delivers complete Maps or exact Key/optional<Value>; state/changed reports status and stop/wait controls local exit. Background work cannot mutate delivered data; legacy View queries remain. Reader preserves the authoritative floor observed by the same object/Scope. Caught up means a complete attached-Star view, not proof that Star has Polaris's latest commit. See [Almanac protocol](../proto/README.md#almanac).

### Ephemeris

Initial Beacon registration/update confirm synchronously; updates atomically extend fixed TTL. beat is mandatory, tick optional. Retain only most recently confirmed Data, never replay failed updates after return.

Client switches automatically restore existing Beacons' logical ids, separating registration generations and Data versions. Old cache cannot overwrite higher versions known at the target. Temporary cross-Star disagreement is allowed; old sources stop renewing and expire. No global single-source promise exists. Stars keep bounded multisource candidates per stable ID and isolate stale operations with capabilities/generations. Observer.one(selector)/stop provides short-lived locally mutable selection Items; fresh authoritative Data replaces local estimates.

Trustfully superseded sources retain original TTL and reject old-process mutations, without immediate bulk deletion or full renewal. See [registration/Data/Renew ordering](../proto/README.md#ephemeris) and [source replacement/recovery](../proto/README.md#replication).

### Catalog

Applications ensure one writer per Key; Publishers in one Scope may own different Keys. A Scope-bound Publisher synchronously submits atomic complete Key/Data batches with one internal batch version, per-Key comparisons, and fresh TTL. Applications supply no version. There is no Delete, automatic renewal, or retained-content replay.

Internal RPCs obtain target-known baselines for creation/rebuilding/switched targets, not global version allocation. Only explicitly uncommitted whole-batch version conflicts may query/repair the same target once within the original call/deadline. Unknown timeouts cannot be replayed this way. The next explicit full update after switching may create missing Keys. Success means attached-Star in-memory atomic commit, not simultaneous cross-Star visibility.

Stars retain highest known content versions until process exit; expiry does not clear watermarks, and exhausted capacity rejects new entries. Source/merged watermarks differ; local expiry does not broadcast deletion. Subscribers may temporarily see lower/missing values after switching; the SDK does not retain permanent per-Key watermarks. See [Catalog](../proto/README.md#catalog), [watermarks](../proto/README.md#catalog-watermark), and [Publisher lifecycle](../comet/cpp/README.md#catalog).

## Admission and internal boundaries

APIKEY/APISECRET grant Comet admission and all ordinary business scopes, without Grant, read/write ACLs, or registration ownership. External entries always reject Sectors beginning __. Credentials live in internal Almanac["__auth"]["comet"]. Business TLS/authentication switches are separate from protected internal entries; see [listeners](build.md#listeners-and-configuration-entries).

Session validity is ordered against revocation at request commit/send, without rechecking SECRET each time. Credential snapshots skipping history require old Sessions to reauthenticate; [credential protocol](../proto/README.md#credential-snapshot) alone defines continuous patches/same-version replay.

Infrastructure uses Pulsar admission. Polaris uses [fixed deployment/same-database restart](../polaris/README.md#deployment), initially without migration/takeover. Pulsar's SQLite backend supports explicit new-group initialization and recovery of that database, not importing/clearing journals. Astrolabe uses [deployment admin accounts/in-memory sessions](../astrolabe/README.md#management-account), distinct from browser, Comet, and node identities.

## Startup and operation

Business startup orders Pulsar admission/first trusted calibration, complete initial Polaris Almanac snapshot, initial Star peer/dynamic synchronization, then public admission. Initial Almanac includes valid emptiness and internal credentials; incomplete initialization cannot masquerade as empty success. Dynamic domains are Ephemeris/Catalog; all publishers need not be online before Star opens.

Star discovers its Galaxy's sole Polaris through Pulsar and initiates one internal bidirectional synchronization stream receiving pushes/reporting installed versions. No separate Polaris address, reverse dialing, or per-Scope connection exists. Missing targets back off; multiple independent Polaris instances are deployment conflicts, not election candidates. [Polaris stream](../proto/README.md#polaris-stream) owns discovery/identity/reconnection.

After admission, lightweight readonly RPCs refresh membership on demand/at low frequency using issued identity, without repeated password login/persistent registration. Each process shares one bounded jittered/backoff schedule. Directory presence differs from actual online state; no directory stream/global revision is introduced. See [directory](../proto/README.md#directory) for replacement, late responses, and resources.

Internal listeners/recovery become available during their synchronization stages, before public readiness, avoiding mutual startup waits. Initial peer/dynamic synchronization has bounded wait; expiry permits explicitly degraded opening with incomplete sources recorded and background recovery. Missing sources may have no visible records. Timeout, partial snapshots, or capacity rejection are not complete synchronization; membership is not online proof.

Report local readiness/source progress separately. Degraded opening promises no globally complete dynamic data and cannot skip admission, first trusted calibration, or initial Almanac. Freeze an initial member-set boundary; later joins synchronize in the background instead of extending startup indefinitely.

Running Stars tolerate temporary component loss. After first trusted calibration, healthy local timing continues TTL, new leases, and renewal while Pulsar is offline; report quality separately rather than reject after a five-second observation age. See [time model](../pulsar/README.md#clock).

New Stars dial directory peers. Each pair converges to one bidirectional gRPC logical stream carrying each side's own Catalog/Ephemeris source. [Star streams](../proto/README.md#star-stream) define simultaneous-dial arbitration/stale completion isolation. Topology/Runtime implement single-stream arbitration and two-way replication; separate inbound/outbound lifecycle classes do not imply two business streams. Logical streams are not TCP connection counts.

Each (source Member.id, domain) has group version/continuous recovery position spanning all Scopes. Insufficient history triggers a complete source/domain group snapshot, not per-Scope cursors; emptiness/trimming do not reset versions. Source commits need short ordering boundaries; group preparation/recovery scale with scope. See [recovery](../proto/README.md#replication). SDKs consume attached-Star merged Scope cursors, not source reconciliation.

Missing payload triggers exact full-source-record repair for that Key/UUID, not an entire Scope fetch. Newer repairs cover only their target; they cannot skip other Keys' events or be overwritten by old frames. Coalesce same-target requests and bound repair/markers/backlog under [repair](../proto/README.md#repair). History gaps still fall back to full group snapshots.

Low-frequency Almanac assessment does not become all-Star pairwise polling. Group progress/repair reuse existing streams without new global business generations, per-Key connections, or cross-domain write locks.

## Minimal initial implementation

<a id="minimal"></a>

| Location | Retained responsibility | Not added now |
| --- | --- | --- |
| Pulsar | One persistent admission path, readonly directory, independent Pulse | New directory log, business storage, multi-source system time service |
| Polaris | Current state/bounded history, Scope single/batch transactions, one stream per Star | Second Outbox, persistent per-Star send queues, generic Repository |
| Star | Native domains/direct indexes/source history/projections/shared deadline scheduling | Generic Store wrapper, per-Scope threads/wheels, per-peer copies |
| Comet C++ | Shared Client core, domain objects, private Watch flow | Task/Executor, full publish FIFO, per-object threads/connections |
| Astrolabe | Deployment login, synchronous management forwarding, bounded latest observations | Own database, persistent replay queues, content adapters, time-series store |

Share mechanical capabilities by responsibility, without forcing native records/versions into one model. [Batch sending](../proto/README.md#stream-batching) coalesces transport, not source commits/Almanac +1. See [indexes](../common/README.md#indexing) and [recovery capacity](../common/README.md#capacity).

Mesh cost remains n(n-1)/2 logical streams and usually n-1 transmissions per local fact. Batching/shared payloads reduce local repetition, not fanout or full-group recovery to O(1). Retain source/domain sequences and short commit protection; evaluate per-Scope sequences, finer locks, cross-connection preencoding, or lock-free reclamation only after measured bottlenecks, not as prerequisites.

## Further implementation boundaries

Native units, Comet C++, complete source replication, and three-Star process faults are wired; validation identifies each regression's source. Prioritize current fixes/scale boundaries, not another single-machine protocol. Single-Star deployments still use real Pulsar/Polaris and required Astrolabe management, without standalone fallback.

Internal schemas, typed indexes, scheduler ownership, initial resource settings, and required HTTP/RPC management are implemented. Capacity must accommodate one legal encoding and complete recovery peaks, rather than filling all budget with normal writes. Remaining lock/index/scanning [performance opportunities](profile.md#performance) require layered [measurement](comet.md), not claims based on modern C++/gRPC.

Current C++ Beacon, Observer, Publisher, Subscriber, and Reader APIs are implemented; SDK documentation and validation separately describe source completion/execution.

## Documentation and verification boundaries

This page owns relationships, authority, stages, and implementation status. [Protocol](../proto/README.md) owns fields, [storage](../common/README.md) owns implementation, [Comet](../comet/cpp/README.md) owns C++ APIs, and [Polaris](../polaris/README.md) owns persistent transactions. Do not duplicate detailed fields, recovery branches, or quotas here.

The old Catalog/Registry split, unified Store, dual physical Keys, Grant, standalone, and Astrolabe database are not new-feature foundations. Frozen SDK history retains its original meaning. [validation.md](validation.md) alone records execution with actual dates/source/limits; design, acceptance lists, and static checks are not behavioral passes.

<a id="status"></a>

## Current implementation

| Component | Capability | Entry |
| --- | --- | --- |
| Pulsar | Admission, trusted directory, SQLite membership transactions, continuous reference; readiness separate from clock quality | [Service](../pulsar/src/server.cpp), [ledger](../pulsar/src/ledger.cpp) |
| Polaris | Sole Go/GORM SQLite Almanac authority, WAL/FULL, atomic Scope commits, shared snapshots, bounded history/install ACK | [Storage](../polaris/internal/storage/store.go), [stream](../polaris/internal/server/stream.go) |
| Star Almanac | Scope routing, atomic complete candidates, continuous deltas, credential revocation/read synchronization | [Library](../star/src/library.hpp), [Receiver](../star/src/receiver.hpp) |
| Star Catalog | Atomic multi-Key publish, internal version query, source/merged watermarks, TTL, snapshots/exact repair | [State](../star/src/catalog_state.hpp), [replica](../star/src/catalog_replica.cpp) |
| Star Ephemeris | Logical UUID/capability, registration generations, independent Data/Renew order, multisource deduplication, atomic TTL/recovery | [State](../star/src/ephemeris_state.hpp), [replica](../star/src/ephemeris_replica.cpp) |
| Peer replication | One bidirectional stream per pair, own-source only, continuous ACK, paged recovery/repair/budgets | [Exchange](../star/src/exchange.hpp), [Session](../common/src/grpc_session.hpp) |
| Comet C++ | Shared Client/Session, synchronous writers, beat/tick, same-ID recovery, reads/selection | [Public header](../comet/cpp/include/comet/client.hpp), [Core](../comet/cpp/src/core.cpp) |
| Astrolabe | Deployment accounts, memory Cookies, Polaris commits, NDJSON reads, redacted credentials, bounded metrics; no database | [Entry](../astrolabe/main.go), [metrics](../astrolabe/internal/bridge/metrics.go) |
| Admin | Separate real/demo modes; login, directory/metrics, complete reads, single-Key editing, credentials | [Adapter](../admin/src/app/management/api.ts), [UI](../admin/src/app/management/Management.vue) |
| Tools/delivery | Independent Astra root, static SDK CMake export, offline builds/process tests | [Build](../tools/build.py), [tests](../tests/README.md) |

Multi-Key atomicity is wired: Almanac advances one Scope version in one Polaris transaction without batch Key-count/total-byte caps; segmented/oversized-snapshot intake exposes no partial batch. Catalog commits 1..128 unique Keys at one Star, at most 1 MiB combined keys/bodies. Protocol single-value/resource/deadline constraints remain. There is no simultaneous cross-Star visibility, cross-domain transaction, or joint snapshot across exact Watches; [protocol](../proto/README.md) owns details.

Domains remain native. Pagination, Reading, and private Context share mechanical work, retaining domain-owned versions, watermarks, ownership, and expiry. Downstream shares immutable batches/in-flight identical pages; ordinary SDK completions enter a deduplicated ready queue. See [storage](../common/README.md) and [SDK](../comet/cpp/README.md) for lock boundaries; implementation does not prove speedups.

## SDK implementation and verification boundaries

C++ implements synchronous Beacon factory/update, mandatory beat, optional bounded tick, confirmed-Data recovery cache, stable logical ID, and corresponding server capability/generation/multisource rules. Publisher remains synchronous/atomic with automatic versions. Reader/Subscriber deliver complete Map or Key/optional callbacks and state/changed/stop. Observer.one returns controlled Items with local-estimate CAS and authoritative-network priority.

Fault cases, standalone package consumption, and three-node probes are updated. Callback-capture destruction occurs outside locks with self-wait guards; single-item Observer updates retain the no-dedup-allocation path. Targeted cases cover sampling capacity/drain, deadlines/cancellation, and local estimates. Results belong only in [validation](validation.md). Legacy View/close/wait remain; Go/Rust SDKs are outside this increment. See [callable APIs](../comet/cpp/README.md#c-public-api) and [fields](../proto/README.md#comet).

## Outstanding acceptance

Current ordinary regression freezes updated directories/module names/generation paths and Store/Agenda/Origin/Context/Broadcast diagnostics. Consult actual statuses, without inheriting older sanitizer/performance passes. Stopped infinite soak stays stopped; bounded three-Star smoke does not prove long-term stability.

Remaining focus includes new-Star first trusted calibration, cross-machine partitions/cross-recovery, large-source installation, long RSS/FD/thread trends, and real browser/GPU interactions. Measure lock waits, scans, page rebuilding, and allocations before replacing algorithms. Static review or clean sanitizer output is not complete safety proof.

Moon, Planet, Comet Go, and complete live 3D Orrery remain deferred. Planet retains scaffolding; Moon has no implementation; legacy Redis SDK is frozen. Builds, regression, sanitizers, performance, and soak follow [authorization](../AGENTS.md) separately; commands/todos grant no permission.

## Approved cached Polaris dependencies

[go.mod](../go.mod) and [Polaris SQLite](../polaris/README.md#sqlite) own versions. Existing caches support authorized offline work, not claims that every environment has dependencies or permission to download/upgrade. Report missing dependencies without restoring them automatically.
