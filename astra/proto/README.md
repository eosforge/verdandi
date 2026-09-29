# Current gRPC Protocol and Admission

[English](README.md) | [简体中文](README_CN.md)

Implementation uses v1 only: no old v4/v6, MessageID tunnel, RPC aliases, or compatibility fallback. Interfaces and ownership follow this table; build/execution status requires source-identified validation evidence.

| Schema | Package / C++ namespace | Responsibility |
| --- | --- | --- |
| [orbit.proto](orbit.proto) | `proto.orbit.v1` / `proto::orbit::v1` | Register, List, Member, four Roles, Credential; C++ Pulsar admission |
| [astra.proto](astra.proto) | `proto.astra.v1` / `proto::astra::v1` | One typed bidirectional StarTransport stream for control and two dynamic domains; old SyncTransport removed |
| [pulsar.proto](pulsar.proto) | `proto.pulsar.v1` / `proto::pulsar::v1` | C++ Pulse four-timestamp calibration |
| [comet.proto](comet.proto) | `proto.comet.v1` / `proto::comet::v1` | Three business domains, Session, Watch, errors; Star/native C++ connected |
| [polaris.proto](polaris.proto) | `proto.polaris.v1` / `proto::polaris::v1` | Authority management and bidirectional Almanac synchronization; Go authority/C++ receiver connected |

The [architecture](../docs/architecture.md) separates Almanac, Ephemeris, and Catalog. This page owns [Comet v1](#comet), [source recovery](#replication), and [Polaris synchronization](#polaris-stream) contracts. See [C++ API](../comet/cpp/README.md), [storage](../common/README.md), [acceptance](../docs/comet.md), and [implementation status](../docs/architecture.md#status). Comet Go is deferred. Generated schemas do not prove complete delivery.

Each domain has dedicated structures. Catalog/Ephemeris source versions cover `(source Member.id, domain)`, including every Scope in that group. Ephemeris stores complete native records. Downstream dynamic Watch uses the connected Star's Scope view cursor, without exposing source inventories/reconciliation to Comet. See [data layout](../common/README.md#data) and [validation](../docs/validation.md) for implemented baselines versus unverified changes.

APIKEY/APISECRET authenticate login only and live in internal Almanac; see [Credential](#credentials). There are no Grants, Scope permissions, or APIKEY ownership rules. Credential belongs to the internal management protocol, not a duplicate public Comet type.

Handwritten C++ uses namespace `astra` and fully named generated types, without wire/orbit/probe aliases. Generated Go package `wire` is code organization, not protocol ownership. Member/Role are defined only in Orbit; Hello preserves signed Member bytes. gRPC owns HTTP/2 connections; business identity binds to logical RPC sessions.

<a id="admission"></a>

## Admission Identity

C++ Pulsar confirms exclusive SQLite membership transactions only after successful COMMIT. Membership, fault-injection, and real-process regressions cover this path. V1 explicitly initializes a new Galaxy or restores that SQLite database; no legacy journal import. Capacity/initialization details belong in [Pulsar](../pulsar/README.md#sqlite).

### One Startup

1. Star loads account/password, TLS material, and the admission public key.
2. Generate one random 32-byte `request_id` for this process's registration idempotency; it is not instance identity or authorization.
3. Send `Admission.Register` over TLS with account/password, Galaxy, endpoint, role, group, and request_id.
4. Admission validates account/role, persists membership and retry evidence, and captures the complete member snapshot.
5. After commit, sign the unique instance credential and return it with the Star list.
6. Star verifies original signed bytes and deployment information, fixes its instance identity, then connects to listed Stars.

No Challenge RPC, RegistrationChallenge, LoginRequest, startup Ticket, second signature domain, or client-supplied id/expected_epoch. Ordinary reconnect does not log in again; the existing Planet candidate-refresh path reuses its request key.

### Identity and Sessions

- Wire `id` is bytes containing valid UTF-8, currently `p_`-prefixed text, 1..128 bytes. It is opaque: clients neither generate nor parse it; C++/Go admission still validates text.
- `principal` is the raw 32-byte SHA-256 fingerprint of account, Galaxy, and canonical endpoint, identifying a replaceable deployment slot.
- Admission allocates `epoch` durably to order replacement within that deployment; it is not a business version.
- `Generation` is local RPC lifecycle state fencing old completions; it never goes to admission.
- request_id goes only to admission, not member lists, Hello, logs, or business versions.

Fresh startup requests are ordered by durable registration, not physical start time. A later commit replaces an earlier one; a delayed never-committed request cannot be recognized as an older process. This is not a wall-clock-newest guarantee. A normal process never changes its request_id.

### Idempotency and Durable Records

Retries use the same request_id and deployment information:

- Lost replies after the first commit neither change id nor increment epoch again.
- While current, return the same admission body and latest list; candidate rotation preserves identity.
- Once replaced, return Aborted, including after admission restart; old requests cannot reclaim the slot.
- Reject reuse with changed account, endpoint, role, or group.
- Failed transactions leave no partial member/request record; concurrent duplicates commit once.

Startup evidence has a persistent budget and cannot be auto-evicted to admit more requests: eviction would permit old startups to register again. There is no online compaction/reclamation protocol or unlimited-restart promise. [Pulsar](../pulsar/README.md#sqlite) owns format, limits, and recovery.

### Star Admission Validation

`StarTransport.OpenSession` starts with Hello. Locally validate:

1. Major version, message boundaries, and admission Ed25519 signature.
2. Galaxy, role, endpoint, and known deployment replacement.
3. Duplicate/conflicting logical sessions.

The sole signature domain is `proto.orbit.v1.admission` + NUL + original Member bytes. Subsequent frames reuse that identity, not per-frame signatures. Star handles no peer passwords, request keys, or durable idempotency index. Infrastructure authentication is not initial synchronization; roles constrain system duties. Ordinary authenticated Comet has no further business permission tiers, but internal Scopes remain forbidden.

TLS 1.3 provides encryption/server verification; bearer credentials identify clients. Existing nodes can communicate while admission is offline; new processes wait to register. No credential TTL, online revocation, shared-password direct connection, or mTLS issuance. Replacement is observed locally upon newer trusted credentials, not instantly by isolated replicas.

<a id="directory"></a>

### Member Directory Queries

After admission, use on-demand/low-frequency jittered read-only unary queries, not member Watch, delta logs, or a directory revision. Orbit's Pulsar control plane uses internal TLS/existing identity. Star, Polaris, and Astrolabe share it; Comet does not. Register supplies the initial list; refresh performs no Register, password derivation, issuance, or persistence.

Each request carries exactly one `astra-admission-bin` and `astra-signature-bin`, using Pulse's existing fields/domain. Pulsar checks size, signature, Galaxy, allowed infrastructure role, and current membership—not signature alone after replacement. Definite rejection cannot generate a fresh request_id to retake identity. Temporary failure preserves identity/list as stale. Query creates no login Session.

One committed immutable snapshot supplies both current-identity validation and the full current Member list for that Galaxy, excluding startup indexes, accounts, business data, and health claims. Register/List use consistent role projection. Star selects Star peers and Polaris; Astrolabe selects observation targets. Not every role joins mesh, and existing Planet paths do not become new v1 scope.

Return one bounded complete snapshot, never live-table pages from different instants. Admission capacity and encoded send/receive budgets must allow every legal list; excess fails explicitly, without successful truncation. Encode/send outside registration locks using shared published state, without scanning disk startup history or blocking Pulse on slow readers.

Each process shares one refresh scheduler and at most one actually unfinished query. Initial periodic policy is configurable 30 seconds plus jitter, not a protocol constant. Missing roles, persistent unreachable targets, or trusted replacement hints can request earlier refresh, coalesced under minimum interval/backoff/deadline/total budgets. No per-peer/group/business-RPC queries. Periodic triggers cannot bypass failure backoff; cancellation does not return capacity before completion.

Only complete valid responses update discovery; failures preserve the old complete list. A captured list cannot undo a newer same-principal identity learned from a trusted handshake. Use existing replacement/local lifecycle fencing, not another distributed directory generation. Same-epoch conflicting identities fail. Omission/fetch failure is not departure and cannot delete business data, close healthy streams, or erase replacement evidence.

New candidates wake existing connection management within dial/backoff budgets, without rebuilding every stream or rebroadcasting the list. Temporary directory failure preserves admitted communication, TTL, and installed Almanac. Definite local credential revocation stops service; cold startup without Polaris waits. `Admission.List(DirectoryRequest) -> DirectoryResponse` fields are in [orbit.proto](orbit.proto). Discovery is delayed, not instantaneous.

### Polaris and Astrolabe Management Admission

Go Astrolabe authenticates management login; v1 has no management role/Scope ACL. It owns no persistent management database and is not an Almanac publisher. It calls the unique Polaris, which durably commits and synchronizes Stars. Success means Polaris persistence, not a Star memory commit/subscription roundtrip.

Almanac atomic batches have no key-count or aggregate-byte cap. `Authority.Batch` receives pages for one Scope/version and commits once only after explicit complete followed by normal EOF. A frame limit is not a transaction-total limit. Existing single-value/configured-capacity/deadline constraints remain. Oversized synchronization transactions use full snapshots; see [Polaris commits](../polaris/README.md#durable-commit).

All infrastructure uses Pulsar admission/protected internal transport, without standalone bypass. Polaris accepts Astrolabe management; Star accepts Polaris Almanac installation. Astrolabe observation identity cannot directly alter Star authority. Service identity differs from browser login; node accounts/signing material/Comet SECRET never reach the frontend. Disabling business auth/TLS preserves internal identity and `__` isolation.

Orbit includes polaris/astrolabe without adding them to Star peer dialing, Planet candidates, or replica counts. Registration is not reachability; browser Sessions are not nodes. [Astrolabe](../astrolabe/README.md) specifies one deployment account, in-memory Sessions, and Polaris submission; necessary HTTP/RPC fields, redaction, and errors are wired, with source-specific evidence in [validation](../docs/validation.md). Role changes must cover C++/Go producers, consumers, and fixtures; unknown roles cannot default to Star.

<a id="polaris-stream"></a>

### Polaris Discovery and Synchronization

Star discovers the unique same-Galaxy Polaris from Pulsar's trusted list, without a fixed Polaris address or Astrolabe discovery proxy. Candidates prove neither online state nor recovered database/Almanac readiness. Connections still validate TLS, signature, role, Galaxy, and replacement. Star initiating the stream does not authorize uploading business drafts.

Resolve current same-principal instances using existing rules. No Polaris means bounded waiting/backoff; multiple deployment principals mean authority conflict and halt ambiguous synchronization. Never elect by reachability, cross-principal epochs, or higher data versions; no round-robin authority writes. Retain installed data with synchronization error; cold start cannot skip baseline. Refresh through [List](#directory), not an assumed Register notification service.

Star opens one current internal bidirectional stream per Star instance to carry recovery positions, installed ACKs, and assessment responses. Polaris sends inventories, snapshots, patches, and assessments for all Almanac Scopes. No per-Scope/snapshot/ACK connections. This is neither management submission, dynamic peer replication, nor Comet Session.

Before data, exchange/validate signed admission in its existing format with fixed Polaris/Star roles; later frames reuse identity. [polaris.proto](polaris.proto) defines fields, not old SyncTransport. Each Snapshot page repeats Scope/version for a private candidate, but only complete permits install/ACK. Early knowledge of version is not commit. Distinguish no baseline from installed zero; new Scope starts with a full snapshot, never an implicit zero baseline created by Patch.

Star reports fully installed authoritative Scope versions only, not received/queued/staged progress. A new Patch strictly follows +1; gaps restore history/snapshot. Replay already installed versions without rewriting. Contiguous patch packets preserve intermediate/no-op versions rather than Watch-style key coalescing. Polaris chooses retained continuous history or snapshot from durable state. Assessment shares the stream, not per-Scope periodic RPC. Include inventory/new Scopes; page failure advances no ACK. [Polaris](../polaris/README.md#synchronization-to-star) owns snapshot/persistence rules; peer source sequences never enter Almanac.

Same-database restart under [deployment rules](../polaris/README.md#deployment) may change Member.id, not Almanac versions. Reauthenticate and resume from retained installed versions only if the database supports them. A Star ahead of authority stops that Scope's overwrite and reports inconsistency, without silent rollback. Empty-Scope versions also survive; matching path/new identity cannot authorize an old-backup downgrade.

Disconnect retains complete Almanac/positions as stale. Star reconnects with bounded backoff; Polaris never reverse-dials. During Pulsar outage, admitted Stars reuse known, not-trusted-replaced Polaris identity/endpoints without resubmitting passwords. Trusted replacement rejects further old-stream installs; partial pages cannot cross streams. Local lifecycle IDs fence callbacks without new business generations.

One write is in flight per direction. Charge send/parse/snapshot/staging and exiting streams together. ACK/assessment reserves cannot preempt a large active write. Slow Stars cannot block others or durable commits. Identity/connection/backpressure waits hold neither SQLite write transactions nor Star domain locks. Service/receiver wiring exists; [validation](../docs/validation.md) identifies executed scope.

## Internal Sessions and Source Synchronization

`/proto.astra.v1.StarTransport/OpenSession` starts with Hello, then Ping/Pong, bounded rejection codes, and domain-specific source frames. At most one unfinished keepalive per direction; enqueue is not delivery and old Sessions cannot modify new ones. SessionPacket selects typed gRPC messages, not custom TCP framing.

Old SyncTransport OpenDeltaStream/OpenSnapshotStream are removed. SessionPacket carries both domains' snapshots, contiguous deltas, cumulative ACKs, and exact repair, preserving absolute deadlines. See [architecture](../docs/architecture.md); schemas/old per-key-version reconciliation do not prove implemented recovery.

<a id="star-stream"></a>

### Bidirectional Star Streams

Each authenticated Star pair ultimately retains one logical bidirectional gRPC stream for both sides' keepalive, recovery, and updates. Each endpoint exports only its own Catalog/Ephemeris sources; received replicas are never rebroadcast as local writes. Polaris sends Almanac directly, outside mesh. Topology/messages/domain recovery/Runtime are wired; component/two-Star process evidence in validation does not prove large partition recovery.

A new Star dials known Stars, without a reverse stream when a valid stream already exists. Both sides may reconnect with bounded jittered backoff, at most one outbound attempt per peer/endpoint. Logical stream count is not a fixed TCP count; gRPC manages connection reuse/recreation.

Resolve duplicates only after mutual identity validation. A single healthy stream need not flip initiator. For simultaneous opposing candidates, both ends retain the stream initiated by the smaller Member.id in unsigned UTF-8 byte lexicographic order. Do not choose by local arrival order, parse opaque IDs as UUID/start times, or compare epochs across nodes.

Same-initiator duplicates cannot preempt an accepted stream; retry after its definite end/backoff. Candidates may coexist during handshake, without instantaneous global switching. Unauthenticated candidates cannot cancel the current stream; failed candidates cannot mark a healthy peer offline. Losers stop new business/cancel, without rolling back admitted data. Replays obey existing versions/deadlines.

Existing local lifecycle IDs fence slots/backoff/completions. Old OnDone reclaims only its own references/resources, never a newer slot/position. Cancel is not completed cleanup; candidates/exiting streams consume connection/memory budgets. No extra distributed generation is introduced.

Serialize writes per gRPC rules; do not await network under connection-state locks. Send buffers/in-flight/recovery share bounded budgets. Keepalive/control have reserves and priority at the next write, not preemption. Snapshot page size is deployment policy, not a protocol-fixed 64 KiB; one stream does not eliminate queueing. Domain replication determines fact/ACK/page boundaries.

<a id="stream-batching"></a>

### Packet Batching and Cumulative ACKs

Star and Polaris streams reuse one-write-in-flight scheduling. Send immediately when idle. At the next opportunity, packetize commits accumulated during the previous write by count/encoded-byte limits. V1 has no batching timer, fixed 500 ms wait, or full active-lease scan to regenerate renewals. Full packets close promptly and sparse tails still send; packet budgets are neither TTL nor durable transactions.

Transport packets may contain contiguous complete source/domain commits or same-Scope Almanac +1 patches. Preserve each number/action/original deadline, including intermediate renewals/deletes. Validate whole-packet structure/budget, then admit commits individually. Later failure preserves the completed prefix; ACK covers only that prefix. A “source batch” below is one version's atomic fact, not a multi-version packet transaction. One source-domain commit must fit the configured message limit. Almanac management totals are unbounded by frame size; oversized transactions synchronize through snapshots. Many expirations may form bounded commits but must finish time/projection advancement, without restoring max_ticks or pretending unfinished expiry caught up. Watch uses separate [final-state coalescing](#subscription-streams-and-installation).

Sources share history; targets retain positions, bounded packets/snapshot references, and control state, not copied event FIFOs. Preparation checks continuity/captures stable references at one boundary. Slow peers outside history recover normally; batching cannot pin history forever. Submitted-to-gRPC position differs from installed ACK; Write completion proves no remote commit.

ACK is cumulative installed progress. Coalesce unsent same-group ACKs to the latest complete value rather than allocate per-version tasks. Positions cannot regress or exceed the confirmed recovery start/submitted complete-commit boundary. Repair R is not contiguous ACK. Send an idle pending tail without another business write. ACK advances no version and requests no ACK-of-ACK. No roundtrip stop-and-wait per commit; unconfirmed/actual in-flight work stays bounded.

Control/repair reserves and bounded domain/Scope scheduling prevent a large snapshot monopolizing sends. Network writes, encoding, and user code stay outside domain locks. Follow [gRPC callback rules](https://grpc.io/docs/languages/cpp/best_practices/) for one write/reaction latency. Keep generated typed messages, not GenericStub/custom bytes/another network framework. Measure throughput and tail effects.

<a id="replication"></a>

### Source Recovery

Catalog/Ephemeris each retain a uint64 group version for `(source Member.id, domain)`, initially zero. Scope locates records, not an independent sequence. Allocate the next position only with a successful source-domain commit, without preallocation holes. Admission/trusted replacement defines identity; reconnect adds no business epoch. A new Member.id has a new group space.

Verified streams identify ordinary frames' sources; peers cannot claim third-party sources. Commit source version, native records, and required bounded history together. One atomic source batch may end entries in different Scopes of one domain, with one version processed/ACKed completely. Do not combine domains or substitute content versions/orders/view cursors. Sequence same-domain commits without holding a Star-wide write lock for full preparation/encoding/network waits.

Empty groups/Scopes and trimmed history never reset positions. Charge metadata; do not reclaim sequence numbers to admit writes. Exhaustion rejects commits needing a position. Deltas span every Scope and ACK only the continuous fully processed prefix, not per-Scope progress or maximum-seen positions. Even legitimately ignored old content must process required floors/positions. Receiving, queueing, or partial preparation is not installation.

Ordinary replay at/below contiguous ACK neither rewrites data nor renews leases. The next position must continue the prefix; restore gaps first. A receiver ahead of source head is an error, never silent zero reset. Parse/budget/preparation failure cannot ACK part of one atomic commit. Exact-repair coverage skips only covered targets, not other Scopes.

Log direct local Catalog/Ephemeris facts and authoritative local Ephemeris endings only. Catalog local expiry, incoming replicas, and replica TTL cleanup create no local source events. Remote groups keep no rebroadcast log. SDK success confirms this Star's native commit/recovery evidence, not peer ACKs.

Senders use committed head and stream position; notifications merely wake them. Coalescing/idle transitions recheck both at one scheduling boundary so the final commit sends without another write. If scheduling fails after commit, retain recoverable state and explicitly fail/reconnect affected streams; do not drop the tail while claiming catch-up. Add no whole-mesh periodic version scan, per-key task, or cloned full send queue per write.

Reconnect reports contiguous position. Replay complete retained batches or restore a full source-group baseline when bounded count/byte history is insufficient. Charge send references; slow peers cannot pin unlimited history/queues. Retain group identity after trimming; one repaired Scope cannot claim group catch-up.

A full snapshot freezes source/domain, B, every Scope/record, and required floors at one boundary. Pages may be grouped by Scope without independent Scope baselines/early ACK. Explicitly complete absent/empty groups. Omission describes only that source, never deletion of others. Export neither merged client views nor borrowed records.

Scopes created after B, writes, renewals, and authoritative endings join contiguous group history/bounded staging. Initial recovery requires each domain's group baseline, not quiescent publishers or an ever-growing per-Scope startup checklist. Different-time Scope roots cannot share a fictional B; later current data is not B's snapshot.

Receive the complete group snapshot before incrementally installing each Scope's records/projection/floors/deadlines atomically. ACK only after all Scopes succeed. Interrupted reception preserves old state; interrupted installation preserves already completed Scopes with internal coverage against events through B. This adds no wire Scope version/ACK; see [storage recovery](../common/README.md#snapshots-and-history). Ephemeris replaces only this source. Catalog merges per-key versions/original deadlines without losing higher floors through omission. Recheck identity, time, and concurrent sources at final installation; old snapshots cannot overwrite newer projections.

Charge active records, bodyless floors, pages, projection preparation, and old/new roots together. Legal group capacity must fit [recovery budgets](../common/README.md#capacity). Temporary pressure or inability to bridge after B aborts/backoffs. A certainly oversized full state reports incompatibility and stops repeatedly downloading until configuration/state changes. Neither case truncates Scopes and declares success. Scope SDK deliveries remain independent; group installation does not synchronize separate subscription callbacks.

For one source instance/domain, B cannot precede contiguous ACK or active exact-repair coverage. Check at reception and final installation, not only request creation. Equal B may restore missing bytes with original deadlines/current expiry checks, never ended identity or a later ending. New-instance group numbers are incomparable.

One group has one recovery path/candidate batch at a time: no interleaved same-group deltas/second snapshots inside its snapshot. Domains share transport under separate budgets; control/repair must not deadlock behind data. Retain one-write/backpressure rules. History gaps restore groups; missing single bodies use exact repair, not full-Scope snapshots.

<a id="catalog-watermark"></a>

#### Catalog Source Watermarks

A full Catalog source snapshot includes the highest directly accepted content version for every key in every Scope, including expired/freed bodies. Each Scope/key appears once: a valid owned body carries version/full bytes/original deadline; otherwise an explicit positive-version watermark. A watermark has neither body nor lease and is not empty bytes, zero, or Delete. Valid zero-byte content uses the complete-record branch.

Owned floors come from locally accepted SDK publishes/renewals, not higher versions learned elsewhere. Separate merged anti-rollback floors from source-owned facts while sharing immutable bodies. Capture metadata/B consistently. Expiry to watermark-only creates no source deletion/version and never resets content version. Later higher direct writes continue normal deltas.

Atomically merge floors and required local deletion:

- Incoming floor 10 against local highest/visible 8 retains 10 and removes visible 8 with downstream notification, not an empty value or rebroadcast local deletion.
- Valid equal-version 10 content survives with its accepted deadline, including content originally obtained from this source. Source expiry does not imply receiver expiry. Floor/omission cannot broadcast Catalog expiry or renew TTL; an already expired body cannot be restored by a floor.
- Local 12 ignores floor 10. Equal-version body conflicts still reject; a watermark cannot prove equality of freed bytes.

These floors stay internal, not in Comet snapshots or SDK tombstone tables. Accepted highest versions survive for the Star process lifetime, including disconnect/replacement/omission/trimming. V1 offers no permanent anti-rollback proof across a whole-cluster restart.

Charge keys/source indexes/snapshot references/preparation peaks; pagination is not constant total storage. Never truncate a complete inventory or evict old floors to accept older versions. Preserve complete state/ACK and report capacity failure. Active values, floors, empty Scopes, and new Scopes retain the same complete boundary.

<a id="repair"></a>

#### Exact Record Repair

Reuse Star's stream, without per-key RPC/connections or Comet inventories. Associate source instance/domain/Scope/key or UUID/triggering source position, plus Catalog content version. Allow one current repair per source-group/Scope/target and coalesce further hints. Echo the trigger rather than invent global request IDs/generations.

At one controlled read boundary, capture the current owned native record and group position R, at least the trigger. Echo association and return full record or explicit absence. Catalog includes version/body/original deadline. Ephemeris includes Attr/current Data/original deadline/required orders together, never Attr from one instant plus Data from another. Do not borrow merged foreign content. Repair adds no source version or TTL.

The current record may differ from the triggering one; no infinite old-payload retention is required. Install checks source/version/immutable Attr/current time and ignores stale repair after a higher local version. Expired/absent results never create empty records. Missing fields, timeout, capacity rejection, or disconnect are not explicit absence.

Ephemeris absence at R ends only that source UUID's local projection and notifies downstream, without rebroadcast authority. Catalog returns retained highest version even without body, following [watermark rules](#catalog-watermark); unknown is legal only if that source never accepted the key. Unknown/bodyless cannot delete valid equal-version other-source content, hide higher knowledge, or reset floors.

R covers only this Scope/target, not the group ACK. Repairing a missing key at10 with data from 15 still requires other keys/Scopes' events 11..15. Retain local target coverage through R against stale Data/deadline/deletion; it does not skip other sources/targets or later events. ACK advances only the completely processed prefix, not a new business version.

Prepare/install the record or absence together with its coverage; failure cannot leave newer data without stale-event protection. Coverage is bounded catch-up state, not another permanent table for every key. TTL deletion/same-instance disconnect cannot release it while old events remain. Release after contiguous progress reaches R, an adequate complete snapshot covers R, or trusted replacement ends that source. Local lifecycle fencing isolates old tasks/new streams.

In-flight repair counts, record bytes, coverage, and queued deltas share recovery budgets. Slow/frequently missing sources cannot grow requests/backlogs indefinitely. Stop the affected group/back off on excess, preserving full values under original TTL without fabricated catch-up. Replacement snapshots must cover installed repair positions and retain uncovered markers. Send/encode/large preparation stays outside domain locks.

Waiting for repair blocks only affected contiguous commit/ACK, never the gRPC reader or the stream carrying its reply. Stage later data within the total budget while dispatching controls/repair. If staging fills before the reply, explicitly terminate/resume from last complete position rather than stop reading forever. Charge cancelling references and preserve installed coverage across reconnection.

A trusted newer epoch proves replacement only within one principal. Retain old Ephemeris replicas until their original deadline, without immediate global deletion/full TTL reset or translating their UUID into the new source. Reject further old events/uninstalled repairs; callbacks reclaim only their resources. New processes use new source positions. New Beacon objects receive new UUIDs; already established Beacons recover their stable logical ID under the capability/generation rules below. Ordinary disconnect, probe timeout, or unverified lists prove no replacement.

Old Catalog source content likewise expires locally at its original deadline; replacement neither deletes valid content abruptly nor removes merged floors. After active records end, streams/recovery actually exit, and snapshots release, reclaim retired source groups/schedulers. Retain merged floors/newer-principal identity evidence without permanent empty groups/wheels for each old Member.id. Late old identity cannot become a fresh empty group; ordinary disconnected sources do not qualify for retirement.

Only nodes that observed trusted replacement apply it; isolated nodes need not know immediately. Local expiry propagates downstream only. Catalog key-version knowledge survives connection closure. No full-peer scan, SDK reconciliation, or synchronous peer-ACK-before-write-success requirement is added.

<a id="comet"></a>

## Comet v1 Business Protocol

This section defines Almanac/Catalog/Ephemeris admission, messages, and synchronization boundaries. Synchronous SDK and Star recovery are wired. Execution claims are limited to the frozen inputs/configurations in [validation](../docs/validation.md); documentation updates are not a new regression pass.

### Confirmed Targets and Implementation Gaps

Confirmed C++ APIs are wired: Catalog.Query/automatic versions, synchronous Beacon Create/Update, atomic fixed-TTL extension, stable-ID recovery, Map/optional callbacks, and Observer.one. Remaining validation boundaries do not authorize old asynchronous semantics. Go has generated protocol only; Planet/other-language SDKs are outside this implementation scope.

Create capability/generation/order, internal EphemerisRecord.generation, and EphemerisData.deadline jointly define recovery. Star, replication participants, and C++ SDK must change together; mixed old-Star compatibility is not promised. See [Comet](../comet/cpp/README.md).

### Services and Instances

External services belong to proto.comet.v1, without a published compatibility promise yet. [comet.proto](comet.proto) is authoritative for method names/field numbers. These methods are wired; execution evidence remains separate.

| Service/method | Direction | Request/result |
| --- | --- | --- |
| Gateway.Session | Server stream | APIKEY/APISECRET login; first reply confirms Star instance/token; stream end invalidates Session |
| Almanac.Watch | Server stream | Authoritative Scope/exact-key view; no Comet Almanac writes |
| Catalog.Query | Unary | Same-boundary known floors for related keys, including expired bodies; no version allocation/Publisher lock |
| Catalog.Publish | Unary | Full single-key/same-Scope unique-key batch, shared business version/TTL; atomic local-memory confirmation |
| Catalog.Renew | Unary | Single/same-Scope batch versions/TTL; validate all then renew without bodies |
| Catalog.Watch | Server stream | Scope/exact-key dynamic view, with per-key content version |
| Ephemeris.Create | Unary | Fixed Attr, initial Data/TTL; initial identity issuance or capability/generation-based restoration |
| Ephemeris.Update | Unary | UUID, Data order, generation/capability, complete Data; extends fixed TTL without changing Attr |
| Ephemeris.Renew | Unary | UUID/independent Renew order; uses stored registration TTL |
| Ephemeris.Remove | Unary | Ends UUID on a specified source instance, returns Empty, ordered against its writes |
| Ephemeris.Watch | Server stream | Scope/exact-UUID complete registration view; Data-only with a valid baseline |

Astrolabe calls Polaris internally for authoritative versions, atomic single/multi-key Set/Delete, idempotency evidence, and restoration. [Polaris](../polaris/README.md) owns these rules; no public Comet write/durability bypass. Old whole-Scope Catalog CAS does not apply to dynamic Catalog.

No Inspect/dynamic Limits handshake. TLS/auth/local budgets are static; explicit anonymous configuration must match the server, without fallback. Authenticate first if required, then all ordinary Scopes are accessible without per-domain permissions/APIKEY ownership. External `__` is always forbidden. Invalid TTL reports its range; SDK neither caches Limits nor clamps requests.

Standard grpc.health.v1.Health reports readiness, not a required per-client handshake, identity/quota discovery, or substitute business checks. Follow [startup order](../docs/architecture.md#startup-and-operation); SERVING is not globally latest data. See [gRPC health checking](https://grpc.io/docs/guides/health-checking/).

Learn instance from Session or actual business responses. Session, Watch without resume position, and first Publish/Create before identity is known may omit instance, identifying only the Star actually reached. Once sent, target/body/version are fixed; late responses cannot mutate the current object. Failed Catalog/Beacon updates are never background-replayed. Initial Beacon failure returns to the caller; only established objects restore registrations, never uncertain initial outcomes/arbitrary external IDs.

Catalog.Renew requires the bound Star's nonempty instance. Ephemeris Update/Renew/Remove require the registration source instance plus current target/generation. Stable logical IDs do not remove these checks; mismatches reject, never clear instance to bypass them. Watch resumes against its original instance/domain/range/complete position. Instance changes reset rather than borrow cursors. Almanac Reader separately retains its cross-Star authority floor.

Credentials occur only in initial metadata, not repeated APISECRET fields. Unary RPCs have finite deadlines; long Session/Watch use cancellation/connection detection, not short write deadlines. Catalog.Query returns related target floors including expired bodies, so applications need not manage versions through Watch. It is neither globally latest nor an outcome-polling service; a version number cannot prove whether an unknown request committed.

<a id="comet-batching"></a>

### Business RPC Batching Boundaries

Catalog retains single-key Publish/Renew and adds atomic batches for one Sector/Spectrum on one connected Star. Publish `entries` excludes legacy `key/value`; Renew `keys` excludes `key`. A batch contains 1..128 unique keys; Publish's combined key/body bytes are at most 1 MiB. Duplicates, empty batches, any conflict, absent renewal target, or insufficient capacity reject the entire batch. All entries share positive version/ttl_ms, checked per key—not whole-Scope CAS.

Publish source root, projection, deadlines, and send history at one Star lock boundary. Snapshots see complete pre/post states only. Same-Scope Watch admits the whole active suffix under one collection lock; SDK installs once after complete. Mid-batch cursors/partial retained history reject delta resume and capture a full baseline. Existing Views remain immutable. Two exact reads/streams are not one snapshot; use one full-Scope View for consistent reads.

Atomic observation is local to the connected Star, not cross-Star/Scope/domain. Peer deltas merge source facts asynchronously; another Star may temporarily see only some new keys. Switching instances does not preserve the old atomic observation point. Publishers/readers requiring it must bind the same Star and verify receipt/view instance. Later single-key writes/TTL expiry may change batch members; batches are not permanent lease groups.

SDK owns the full batch during each synchronous update, without pending desired state/automatic renewal after return. Only a definitely-unapplied whole-batch version conflict permits one same-call Query/repair. Success confirms this Star; timeout/cancel/lost receipt remains possibly committed, never relabeled unapplied.

Shared business version is still per-key comparison; disjoint Publishers can share it. Where history/send units distinguish transactions, retain actual batch boundaries rather than merge writers by raw version. Partial history cannot masquerade as a batch. Full-Scope installation follows complete commits; cross-Star transaction order/visibility is not guaranteed. Single-key operations, Ephemeris, and independent exact Watches remain separate.

### Message Data and Identifiers

Scope contains Sector/Spectrum; service determines domain. Empty Watch target means full Scope; nonempty is exact key/UUID, without cross-Scope wildcards. Names are UTF-8 text, 1..128 bytes without NUL, encoded as bytes to avoid duplicate Protobuf text checks. Compare raw bytes without trimming, case folding, or Unicode normalization. `*` is literal; invalid UUID never falls back to full Scope. Check raw Sector for `__`, not path concatenation/client permissions.

Instance IDs reuse Star process identity, not another epoch or business UUID. Initial Ephemeris registration uses a reliable random capability to derive logical UUID; legacy generation=0 uses random UUIDv4. Wire/SDK/storage use raw 16-byte identifiers with fixed version/variant bits, not 36-character text. Validate length/bits and reject text aliases rather than normalize. Avoid per-RPC formatting. Random failure cannot fall back to timestamps/ordinary PRNG. See [RFC 9562](https://www.rfc-editor.org/rfc/rfc9562.html#section-4).

Binary UUID saves 20 payload bytes throughout Create/Update/Renew/Remove/Watch/Snapshot/Delta/Repair. Versions/orders remain uint64. Content version is positive; empty view cursor/new-registration order may be zero, but zero cannot bypass new-operation checks. Reject overflow. Almanac Watch uses authority version; dynamic Watch uses local Scope cursor, neither per-key Catalog version nor per-UUID order.

Catalog entries publish complete Buffer, including empty bytes, singly or in bounded batches, without Delete/field Patch/chunked upload. Almanac management is separate. Ephemeris uses UUID/Attr/Data; lease/order metadata never enters user Buffer. Observer does not expire data by client wall clock; Star pushes state and SDK reports staleness.

<a id="almanac"></a>

### Reading Almanac

Star installs only Polaris-authorized complete Scopes/versions and exposes read-only snapshots/deltas, including valid empty groups. Watch shares resource/recovery machinery, but authority versions cannot be renumbered by received entries or local TTL. External Watch never exposes __auth. [Polaris](../polaris/README.md) owns writes/history/install ACKs; [Reader](../comet/cpp/README.md#subscription) owns cross-target nonregression.

<a id="catalog"></a>

### Catalog Publishing and Deadlines

Keys in one Scope have independent versions/publishers; the application guarantees one writer per key. SDK manages versions while the application persists required Data, without passing/persisting counters. No APIKEY ownership, global allocator, or strong-consistency arbitration.

- Each synchronous update submits a full Key/Data batch and explicit TTL, sharing a positive version compared independently per key. Gaps are valid; an unrelated Scope maximum cannot reject another key.
- New/recreated Publisher or required target changes query relevant known floors. Allocate above both those floors and this Publisher's issued versions. An uncertain version cannot be reused with different content. Reject exhaustion; never derive versions from timestamps/Watch cursors.
- Query describes this Star, not global freshness/uniqueness. Disjoint Publishers may use the same number; competing same-key writers are not automatically arbitrated.
- Equal retained versions require byte equality; different bytes conflict. Expiry frees bodies but retains process-local highest knowledge. Missing bodies cannot be claimed compared. A later full update can create absent content on a new Star without Publisher registration/old-body caching.
- SDK never auto-renews, retains returned Data for restoration, or replays failed updates. Legacy Catalog.Renew still validates single/batch requests correctly but is not scheduled by the new SDK.
- TTL is explicit each time, with Ephemeris units/range validation; zero does not mean previous TTL. Compute deadlines at admission. Legal equal-version retries take max(original, candidate); newer versions use their own deadline. Replication preserves absolute deadlines, not receiver-generated renewal.
- Prepare content/floors/deadlines/source history/batch boundaries together, then publish atomically. Validation/clock/capacity failure cannot apply a subset. Success is local memory commit, not cluster replication.
- No public Delete. Star expiry emits Watch deletion. Publisher close cannot withdraw possible in-flight commits or guarantee global disappearance at a fixed post-close time.
- Peer merging retains higher per-key knowledge even after expiry, preventing old values resurfacing. Equal-version deadlines follow source rules. Snapshot omission cannot clear other sources; source progress is not content version.

Only explicit version conflict plus definitely-unapplied entire batch permits one same-target Query, higher allocation, and resend of the original Data within the original deadline. Query failure, second failure, target change, or timeout ends the call. Unknown sent operations never enter repair; nothing replays after return. This does not establish globally consistent versions.

Scope view cursor differs from key version. Content-version/visibility changes advance it; pure deadlines do not. Subscriber switching installs the new target's full state, allowing temporary absence/older content without SDK patching from old records. See [implementation boundaries](#confirmed-targets-and-implementation-gaps).

### Commit and Exhaustion Boundaries

Atomic data/version/required-metadata commit divides write outcomes. Observed pre-commit cancellation/deadline stops admission, but cancellation can race commit and cannot guarantee withdrawal. After commit, notification/encoding/queue/reply/disconnect failure cannot roll back or report Effect::unapplied. Return committed success if possible; otherwise the client remains uncertain. Failed delivery ends only affected subscriptions without blocking the writer for cleanup.

Unapplied describes this operation, not an unchanged Scope throughout the call: other writes/independent expiry may advance it. All fallible commit preparation precedes the boundary; no required metadata is added afterward. Map exceptions by phase rather than a blanket catch labeling committed work unapplied.

uint64 never wraps. At maximum Catalog version no higher content is possible; valid same-version renewal still depends on source/view progress capacity. Separate old Almanac commit confirmation from new-version exhaustion, and check both Ephemeris orders independently. Exhausted view/source positions cannot reset. If expiry cannot commit, stop affected catch-up promises/end Watch rather than secretly filter at the same cursor. No UUID replacement/database clearing/automatic retry conceals terminal exhaustion.

<a id="registry"></a>
<a id="ephemeris"></a>

### Ephemeris Registration and Deadlines

Initial client.beacon synchronously supplies fixed Attr, full Data, ttl, and mandatory beat semantics; return a handle only after success. Star stores complete native registration/fixed TTL/Data version/deadline. beat/tick are SDK scheduling, not server timers. No paired physical keys; see [storage](../common/README.md#data).

- Initial failure starts no background registration; the application chooses another call. Timeout may leave an orphan until TTL measured from actual commit, not necessarily shortly after client timeout.
- Established objects restore the same logical ID after target change/registration loss. Registration generation differs from Data version/order. Attr/TTL stay fixed; restore only last confirmed Data, never failed/unknown updates.
- Recovery cache cannot overwrite known newer Data or acquire invented versions. Different Stars may temporarily hold different values; switching may reveal older data. Stop old renewal and let remnants expire. Projection deduplicates IDs without globally unique instantaneous sources/election.
- A new object/process cannot claim any ID merely by sharing APIKEY. Recovery supplies private capability and increasing generation. Star validates logical UUID using Scope, Attr, TTL, and 32-byte capability; capability is neither replicated nor exposed by Watch/APIKEY.
- SDK requires `0 < beat < ttl`; optional tick interval is positive. Star independently validates TTL, identity, order, capacity, and commit conditions.
- New Update atomically replaces Data/extends fixed TTL. Same order only confirms earlier admission without extending again. Renew has independent order. Empty Attr/Data remains valid, not removal/skip.
- TTL is integer milliseconds, default 30000, default range[1000,600000]. Configurable maximum cannot fall below the fixed minimum. Reject invalid/fractional/overflowing values without clamping. Update/Renew never changes fixed TTL.
- Logical expiry precedes physical cleanup. Ended generations reject Update/Renew; recovery is an explicit new generation, not old-request upsert. Order Remove/expiry/writes; late old-generation removal cannot delete migrated state.
- Successful Update suppresses redundant beat Renew, but sending is not lease success. Conservative local time starts at first send, not receipt; no client/Star clock synchronization needed.
- Once trusted calibration exists and local time remains usable, reference outage alone does not reject new finite deadlines. Local failure/reversal/exhaustion reports clock failure, not ended registration/endless recovery. See [clock model](../pulsar/README.md#clock).

Destroy ends the local role immediately and attempts asynchronous Remove without closing Client/other roles or promising immediate remote disappearance. Closed objects never restore. [SDK lifecycle](../comet/cpp/README.md#destroy-and-lifecycle) defines actual callback/buffer cleanup.

<a id="registry-order"></a>

### Ephemeris Operation Ordering

For one logical ID choose valid sources by Data order, then generation, then absolute deadline. Conflicting fixed Attr/TTL or different bodies at one Data order reject. Expiry may reveal another valid source, with one projected record. Legacy generation=0 rejects cross-source UUID collisions. Internal full records carry generation(7); Data deltas carry deadline(3), preserving deadlines rather than regranting TTL.

Data order protects final commit across independent RPCs. Shared Client/Channel/Session is not one guaranteed TCP connection; even one TCP connection orders bytes, not HTTP/2 handlers' commits. gRPC orders messages within one streaming RPC, not separate unary writes; see [core concepts](https://grpc.io/docs/what-is-grpc/core-concepts/) and [connections](https://grpc.io/docs/guides/performance/).

If Update(A,1) stalls and the application explicitly sends B,2 after timeout, B may commit first while A still reaches commit. [Cancellation](https://grpc.io/docs/guides/cancellation/) does not prove remote completion. Check order under commit protection to reject A; locks/last-arrival-wins alone could overwrite B. Keep latest-order evidence, not a full operation log; report obsolete without replaying old Data.

Each active UUID retains independent latest Data/Renew orders, bounded registration state rather than field/global revisions. Allocate for actual new sends; retry preserves order/content. Failure/cancel may leave gaps; strict +1 is unnecessary. Synchronous writes are not coalesced into latest pending Data/results.

| Condition | Data Update | Renew |
| --- | --- | --- |
| Missing/ended/expired registration | Reject, no upsert | Reject; old success does not prove current lease |
| Greater order | Atomically replace Data, extend fixed TTL, save order/evidence | Extend once and save order/result |
| Equal latest order | Same bytes confirm old result without extension; different bytes reject | Confirm prior admission, not a new retry-time deadline |
| Lower order | Report obsolete/required latest position, without claiming it once committed | No repeat extension or deadline rollback |

Order, current Session/source/time, Data, and lease share one commit boundary. Preparation failure advances no order. Same-byte new-order Update renews but need not notify unchanged content; pure Renew resends no Attr/Data. Cleanup/Remove/update/renew are ordered within a generation: ending first rejects later work; ending afterward removes current complete state. Old generations cannot remove migrated registrations.

Keep only latest confirmation per operation class. Lower orders cannot prove historic execution, so never rewrite local uncertainty. Credential rotation does not reset orders, and recovery cannot relabel cached old Data as new. A new generation may reset Renew order while preserving Data order. Star returns adopted higher Data in CreateReply, not zeroing to hide unknown commits. Read via Watch; uncertainty/temporary failure is not confirmed expiry.

Initial Create has no idempotency cache. Recovery at the same active generation confirms current state without renewal; lower generation rejects. Failed first creation returns an error without auto-creating another initial identity, possibly leaving a TTL-bounded record. Established same-ID recovery is separately generation-fenced against old Data/Renew/Remove.

<a id="session"></a>

### Business Sessions and Authentication

Comet credentials differ from Orbit admission; Comet never logs in/registers/calibrates with Pulsar. Internal Almanac credentials are managed through [Astrolabe](../astrolabe/README.md)/Polaris. See [startup switches](../docs/build.md#deployment-and-wiring) and [SDK authentication](../comet/cpp/README.md#business-authentication).

- With auth on, Gateway.Session validates APIKEY/APISECRET and returns actual instance/random token in its first success, then remains open. Independent business RPCs carry initial `comet-session-bin`, not SECRET.
- Session exists only in this Star's memory, not persistence/replication/Catalog, node tickets, or JWT. Reject absent/unknown/invalid/cross-instance tokens; identical account configuration does not make tokens portable.
- Bind logical stream, not TCP/socket. No Release, five-minute idle scanner, or auth Ping. Idle streams may live; abnormal disconnection uses transport detection, not instantaneous cleanup.
- On observed end/cancel/revocation/rotation invalidate authorization first, then close related streams/subscriptions and drain references. Preserve committed writes, registrations, versions, and TTL. Network cleanup is no authorization grace period.
- Order initial login activation and final write authorization/commit against credential changes. Earlier revocation blocks old work; earlier commit survives. Late login cannot escape a completed revocation scan; no external auth generation.
- Parse/allocate outside critical sections where appropriate; rejection discards prepared data/version/evidence, while independent maintenance may progress. Continuous changes spare unrelated accounts; history-skipping snapshots follow [blanket invalidation](#credential-snapshot).
- No domain/Scope ACL or owner check. External `__`/internal-management isolation persists with auth off; validate instance/version/order/time independently.
- Auth off requires neither token nor Session. Explicit Gateway.Session returns FAILED_PRECONDITION, reason=input for configuration mismatch, without fake anonymous token/downgrade. TLS is independent; plaintext lacks confidentiality and triggers no fallback/request-signature workaround.
- Pending authentication and idle streams count toward Session capacity. Initial confirmation timeout is not the whole long-RPC deadline. SDK owns local wait/close/reauthentication policy.

Validate decoded Scope before lookup/group creation/preparation, consistently rejecting `__` across Publish/Create/Update/Renew/Remove/all Watches even with auth off. A normal interceptor cannot read message Sector before deserialization; v1 adds neither custom Protobuf scanning nor duplicate metadata Scope. Message/transport budgets protect predecode resources; see [gRPC receive implementation](https://github.com/grpc/grpc/blob/master/include/grpcpp/impl/call_op_set.h).

Revocation invalidates authorization before TryCancel. Closing Session cannot withdraw completed/independent unary work; every RPC checks lightweight validity, and final write checks are ordered with revocation without repeating SECRET. A bounded APIKEY→Session index locates affected streams without scanning all RPCs/new wire generations. [Cancellation](https://grpc.io/docs/guides/cancellation/) is cleanup, not authorization enforcement by itself.

Watch authorization, index insertion, and observation capture follow the same lock order so late insertion cannot miss revocation. Revoke send permission, discard unpermitted prepared pages, then cancel outside locks. Already permitted in-flight/application-delivered bytes cannot be withdrawn; no instant network erasure promise, Scope ACL, or extra Version handshake.

[Credential lifecycle](../astrolabe/README.md#credential-lifecycle), Session lifetime, and Ephemeris TTL are three separate responsibilities.

<a id="credentials"></a>

### Credential

Credential authenticates login only, without Grant/Domain/Scope/read-write bits/APIKEY ownership. Authenticated users access ordinary data; external `__` is forbidden. Infrastructure roles still govern management/replication.

Store `Almanac["__auth"]["comet"][APIKEY]`: Astrolabe submits to durable Polaris, which installs on Stars. APIKEY is the record key, not duplicated payload. No public Watch or standalone credential-file authority.

Control-plane `proto.orbit.v1.Credential` contains only `1 bytes secret`, nonempty. No grant/permission messages or public SDK import of the internal table.

Initial engineering SECRET cap is4096 bytes; encoded records/table still obey management-message/entry/byte budgets. This is not a permanent protocol cap. Charge records/indexes/preparation peaks; trusted internal links do not bypass required-field/capacity validation.

Polaris validates before commit; Star prepares records/login indexes before atomic record/index/version/required-Session-invalidation installation. Parse/preparation failure cannot half-rotate SECRET. No permission index/per-request Grant scan; ordinary RPCs still check current Session/business invariants.

Continuous same-SECRET Set and duplicate installed-version confirmation do not revoke. [Astrolabe](../astrolabe/README.md#credential-lifecycle) owns deployment rotation. Polaris durability does not prove simultaneous Star revocation: Stars enforce installed local versions without querying management per RPC.

<a id="credential-snapshot"></a>

### Credential Snapshots and Session Invalidation

A newer complete snapshot crossing missing history forces reauthentication. Keep secret-only Credential, without per-credential creation versions/tombstones/generations. Identical final bytes cannot prove uninterrupted validity after delete/recreate or rotate/restore.

| Installation path | Local Session action |
| --- | --- |
| Contiguous authoritative +1 commit | Invalidate only actually changed/deleted APIKEYs; same-value/unrelated accounts survive |
| Newer full snapshot replacing missed commits | Invalidate all prior local Comet Sessions, including unchanged final secrets |
| Same installed version replay/ACK retry | No reinstall/revocation; lost ACK cannot repeatedly eject reauthenticated Sessions |
| Old/partial snapshot or parse/resource failure | Preserve complete table/version/Sessions; old versions cannot overwrite |

Compare against the final installation version, not the initial receive-time value. New table/index/version and old-set invalidation take effect together under [Session ordering](#session); cancel/drain large connection sets outside locks. Prepared logins recheck the current table and cannot join invalidated sets. Delayed old-set cleanup cannot close newly authenticated Sessions.

Old writes/Watch still check validity at final commit/send permission. Preserve earlier commits/permitted frames; invalidation changes no Catalog version, Ephemeris UUID/order, or existing TTL. SDK treats established Session termination as shared reauthentication, not immediately wrong SECRET; only subsequent login rejection pauses. [SDK recovery](../comet/cpp/README.md#client-secret-updates-and-business-recovery) is authoritative.

This applies only to full installation of `Almanac["__auth"]["comet"]`. Bootstrap has no old business Sessions; auth=false creates none. Ordinary Scope installation, Polaris identity change, and disconnection alone do not blanket-revoke. Existing authority/local positions establish continuity without new wire fields.

### Message Field Contracts

This documents generated schema with Query/recovery/Update renewal wired, not execution acceptance. Field numbers restart within each `proto.comet.v1` message. Local Empty avoids another protocol dependency. Listed oneofs require exactly one branch; absent is not a default operation. Servers validate remaining presence requirements. Scope names follow the text rules. Scoped requests directly carry `1 bytes instance`, `2 Scope scope`, without Address wrapper; credentials stay in metadata. Reject missing Scope/empty names; each RPC controls when instance may be empty.

| Message | Fields |
| --- | --- |
| Empty | No fields |
| Scope | `1 bytes sector`, `2 bytes spectrum` |
| AlmanacChange | `1 string key`; action: `2 bytes value`, `3 Empty erase`; value and deletion differ |
| CatalogChange | `1 string key`; action: `2 bytes value`, `3 Empty erase`; `4 uint64 version`: positive key version for value, zero for erase; local erase is not authoritative deletion |
| EphemerisChange | `1 bytes uuid`; action: `2 Record record`, `3 Empty erase`, `4 bytes data`; full record or replacement Data for an existing record |
| EphemerisChange::Record | `1 bytes attr`, `2 bytes data`; UUID stays outside; both empty still means present full record |

APIKEY is nonempty UTF-8, initially at most 128 bytes without NUL. SECRET is nonempty opaque bytes, initially at most 4096, without text normalization. Session tokens are unpredictable 32-byte random values in initial comet-session-bin only. Auth-on requires exactly one raw 32-byte token, rejecting absence/duplicates/wrong length/text/Base64 aliases. Check active-table collisions before install; random failure/bounded retry exhaustion rejects login without overwrite. [UUID](#message-data-and-identifiers) is independently 16 bytes: first Create accepts no external UUID; established objects restore with capability/new generation. Business UUID, Session token, and node identity are distinct.

| Message | Fields/constraints |
| --- | --- |
| SessionRequest | `1 string key`, `2 bytes secret`; no prior instance query |
| SessionReply | `1 bytes instance`, `2 bytes session`; sole success confirmation; token lives with the stream, whose end invalidates it |

Flattening removes only Address, retaining Scope. Generated/wired endpoints use these field numbers without old-Address parsing. Allocations depend on construction/reuse/[Arena](https://protobuf.dev/reference/cpp/arenas/), not a fixed two allocations per RPC. No measured speed/memory benefit is implied.

No InspectRequest/Reply/Limits. Catalog's128-entry/1 MiB batch constraints combine with key/Buffer/encoded-message/preparation budgets, without dynamic override. Generated Go Comet is separate from Orbit; no Go SDK yet. C++ remains `proto::comet::v1`, without wire alias.

| Catalog message | Fields/constraints |
| --- | --- |
| CatalogQueryRequest | `1 bytes instance`, `2 Scope scope`, `3 repeated string keys`; 1..128 unique keys of1..1024 bytes; initial discovery may omit instance |
| CatalogQueryReply | `1 bytes instance`, `2 repeated Entry entries`; one captured boundary, request order |
| CatalogQueryReply::Entry | `1 string key`, `2 uint64 version`; unknown=0; expiry retains highest floor |
| PublishRequest | `1 bytes instance`, `2 Scope scope`, `3 uint64 version`, `4 string key`, `5 bytes value`, `6 uint32 ttl_ms`, `7 repeated CatalogEntry entries`; valid version/TTL; key/value excludes entries; empty value allowed |
| CatalogEntry | `1 string key`, `2 bytes value`; unique keys, empty values allowed |
| PublishReply | `1 bytes instance`, `2 uint64 version`; confirms content/deadline admission, not view cursor/original retry deadline |
| CatalogRenewRequest | `1 bytes instance`, `2 Scope scope`, `3 string key`, `4 uint64 version`, `5 uint32 ttl_ms`, `6 repeated string keys`; nonempty instance, key excludes keys, positive version, explicit valid TTL |
| Catalog.Renew result | Empty; context already identifies instance/key set/version; no order/remaining_ms/repeated identity |

Publish is full-value submission without Set/Delete oneof, automatic splitting, or version changes. Query validates 1..128 unique keys and returns floors under one local lock, unknown zero without creating empty Scope. SDK checks instance/count/key order, rejecting omissions. Query allocates no versions and is not Scope CAS/Limits negotiation.

| Ephemeris message | Fields/constraints |
| --- | --- |
| CreateRequest | `1 bytes instance`, `2 Scope scope`, `3 bytes attr`, `4 bytes data`, `5 uint32 ttl_ms`, `6 bytes uuid`, `7 bytes capability`, `8 uint64 generation`, `9 uint64 order`; initial generation=1, empty uuid/capability, order=0; restoration carries confirmed identity/Data order |
| CreateReply | `1 bytes instance`, `2 bytes uuid`, `3 uint32 ttl_ms`, `4 bytes capability`, `5 uint64 generation`, `6 uint64 order`, `7 bytes data`; actual adopted Data and independent generation |
| UpdateRequest | `1 bytes instance`, `2 Scope scope`, `3 bytes uuid`, `4 uint64 order`, `5 bytes data`, `6 bytes capability`, `7 uint64 generation`; positive order, empty Data valid |
| UpdateReply | `1 uint64 order`; new order confirms atomic Data/TTL, duplicate confirms original result |
| RenewRequest | `1 bytes instance`, `2 Scope scope`, `3 bytes uuid`, `4 uint64 order`, `5 bytes capability`, `6 uint64 generation`; independent positive Renew order |
| RenewReply | `1 uint64 order`; no Lease/ttl_ms/remaining_ms; duplicate does not recalculate deadline |
| RemoveRequest | `1 bytes instance`, `2 Scope scope`, `3 bytes uuid`, `4 bytes capability`, `5 uint64 generation`; removes only matching local generation |
| Ephemeris.Remove result | Empty, no RemoveReply; target is inactive at commit, without proving an earlier unknown Update unapplied |

No creation-outcome query/four-state recovery message. Existing UUID writes validate source instance/Session/active deadline. Missing/expired source reports ended; Session error is not expiry prompting recreation. No APIKEY ownership.

Remove uses request instance/Scope/UUID without repeating them in success. Validate identity/Session/range; internal-range rejection, wrong instance, and transport failure cannot become empty success. Authorized absence returns NOT_FOUND, without permanent tombstones merely to make repeated Remove return OK. Cleanup may treat definite ended as nothing left to remove. Late OK/NOT_FOUND affects only its original registration, not a new one. Empty has no Protobuf body, but status/HTTP2/TLS remain; timeout may follow committed removal.

No Lease/remaining_ms. CreateReply confirms fixed TTL/recovery identity/adopted Data, not remaining server time. [SDK local budgets](../comet/cpp/README.md#local-lease-budget) require neither Pulsar nor wall-clock comparison.

Update/Renew require nonempty expected instance/Scope/UUID/positive order. Final checks return the same order only for success/idempotent confirmation. SDK matches RPC context/current lifecycle, not bare-order identity inference. Zero/mismatched success is protocol error; stale receipts cannot update current state or rewrite returned outcomes. Replies omit repeated instance/UUID.

Small receipts still require server time/expiry/deadline checks. uint64 order 1..UINT64_MAX encodes in2..11 Protobuf bytes: one tag plus 1..10 varint bytes, excluding gRPC/HTTP2/TLS. See [encoding](https://protobuf.dev/programming-guides/encoding/); smaller bodies are not measured throughput evidence.

| Watch message | Fields/constraints |
| --- | --- |
| WatchRequest | `1 bytes instance`, `2 Scope scope`, `3 bytes target`, `4 optional uint64 version`; empty target=Scope, otherwise exact key/UUID; absent version=reset, explicit 0=resume 0 requiring nonempty instance |
| AlmanacWatchReply | `1 Mode mode`, `2 repeated AlmanacChange changes`, `3 bool complete`, `4 optional uint64 version`, `5 bytes instance`; authoritative Scope version |
| CatalogWatchReply | `1 Mode mode`, `2 repeated CatalogChange changes`, `3 bool complete`, `4 optional uint64 version`, `5 bytes instance`; complete requires version/instance |
| EphemerisWatchReply | `1 Mode mode`, `2 repeated EphemerisChange changes`, `3 bool complete`, `4 optional uint64 version`, `5 bytes instance`; same install boundary, no cross-domain Change type |

Mode is unspecified=0/reset=1/apply=2; reject unspecified/unknown. Service/response type determines domain; request determines Scope/target. No Target/Position/Domain wrapper/bytes cursor or old 4 KiB cursor limit. Absent/explicit-empty target both select full Scope per [scalar defaults](https://protobuf.dev/programming-guides/proto3/#default). Reset permits value/record only; apply also erase, and Ephemeris data under [delta rules](#registry-delta). Reject missing actions/empty identifiers/invalid UUID. Full record always has Attr/Data without repeating UUID. Version/instance are not access credentials.

Typed responses exclude cross-domain branches, while value/erase and record/data/erase oneofs still require decoding. [Oneof](https://protobuf.dev/programming-guides/proto3/#oneof) shares storage; branches are not simultaneously allocated payloads. This tightens types/removes Catalog wrapping without promising smaller objects/throughput. SDK may share completion/staging/cancellation without duplicate synchronization machines.

Incomplete pages stage changes and omit version/instance. The final complete page includes its last data, nonempty instance, and target version including valid zero. Almanac authority version differs from dynamic view cursor and CatalogChange's key version. Empty snapshots/progress can be one empty complete page. Charge actual received/allocated bytes without declared totals/pretraversal; overflow/disconnect advances no installed position.

Pure Catalog deadlines/Ephemeris renewals advance required source state but not unchanged content cursors/history/notifications. Higher Catalog content version is visible; equal-byte Ephemeris order-only is not. Empty completion may confirm existing progress, never create heartbeat content versions/mappings.

### Error Detail Fields

Non-OK RPCs may include bounded serialized Failure in trailing `comet-error-bin`, not language exceptions or a custom object disguised as google.rpc.Status. Missing/duplicate/malformed details prove no write effect; uncertainty remains. Known gRPC categories still guide backoff/pause rather than making every rejection a network fault. Unknown Effect is never unapplied.

Failure: `1 Reason reason`, `2 Effect effect`, `3 optional uint64 version`, `4 optional uint64 order`, `5 optional uint32 ttl_min_ms`, `6 optional uint32 ttl_max_ms`, `7 optional uint32 retry_ms`, `8 string limit`, `9 optional uint64 maximum`, `10 string message`, `11 bytes instance`. Actual current instance helps mismatch/read recovery, not automatic write redirection. Initial detail cap 4 KiB/message 512 bytes excludes credentials/payload. Transport failure may omit details. Optional numeric presence matters; default zero is not current version/legal TTL minimum.

[comet.proto](comet.proto) defines Reason/Effect values. Unknown reason stays unknown server error without modifying input/cross-instance replay. Effect describes this attempt only; an unapplied retry cannot erase earlier logical-operation uncertainty.

## Subscription Streams and Installation

All Watch requests share WatchRequest; each RPC fixes its response domain/type throughout. Pages carry multiple domain changes; final complete=true ends a batch without separate start/end frames/per-page ACK. One batch at a time, fixed mode, no snapshot/delta interleaving. A single message can complete directly.

First reset builds independent candidate state; apply prepares against an installed baseline. First apply must continue the request's same-instance resume position; later apply continues the last complete batch, without another baseline field. Reset is first-batch-only. Any later reset/history gap ends the stream and renegotiates on reconnect. Old callbacks never join new candidates.

Empty/single/multipage reset/apply share completion. Only after final data and budget validation publish view/actual instance/version together. Disconnect preserves stale complete state. Partial pages cannot advance resume. Duplicates/out-of-range entries/mode changes/invalid completion discard staging. Apply cannot change instance/regress version; equal version requires no changes. Empty completion can confirm catch-up without being an idle heartbeat obligation.

Stream OK is not complete: discard partial batches, retain stale last state, reconnect if open. Erase of absent keys/UUIDs is valid and installs progress, including coalesced create-delete. First apply requires an actual full baseline, not merely a numeric request version. Malformed mode/action/position/target stops automatic recovery rather than downloading the same invalid snapshot forever. Missing Attr has the limited fallback below.

Retain instance/uint64 position with domain/Scope/target. Any range change, including full↔exact, clears resume and requires snapshot. A bare integer cannot prove possessed range; calling contract supplies it, not authorization. Dynamic ahead/history-gap/instance-change resets; never compare local cursors across instances. Exact watches may skip unrelated commits, maintaining same-instance nonregression.

Almanac request version additionally carries the Reader's minimum installed authority version across instances. No extra handshake/field. A lagging Star returns UNAVAILABLE, reason=version, current version/optional backoff before any snapshot page. SDK retains stale state/floor and retries boundedly without ending Reader/clearing state/switching whole Client. After catch-up choose reset/delta by instance/history; cross-instance cannot reuse old deltas. A fresh Reader without version has no process-local floor.

At Watch creation capture root/version B and observation of later changes consistently. Send [frozen pages](../common/README.md#snapshots-and-history) outside locks without per-Watch full copies/count scans. Final completion installs B; later deltas advance it. B need not still be latest when transmission ends. Delta similarly freezes target T/final covered projection; later writes belong to the next batch.

Dynamic capture first advances required TTL under controlled scheduling, commits local expiry projection, then captures Scope root/cursor. Ephemeris is a complete native record, not paired keys. Shared loops maintain source groups without per-Watch lease scans. Rounded wheel ticks may delay deletion within a tick; Watch is not instantaneous liveness proof. Post-capture expiry arrives as later erase, never send-time filtering of the same version. Clock/preparation failure preventing expiry cannot establish a new caught-up baseline; end affected Watches temporarily and preserve stale SDK state. Reduced reference quality with usable local timing alone does not stop streams.

Authorized nonexistent Scopes/exact targets get ready empty version 0 baselines without creating business commits. Existing empty groups retain versions. Establish baseline and first-write observation together, without polling reconstruction. Invalid identity/internal Scope/Session failure is not empty success. Charge/release waiting observation state.

Watch conveys latest target state, not every intermediate event. Coalesce one final action per key/UUID in the covered range, ordering create/change/delete. Never mix Scopes or invent cross-request transactions. No content change may send progress only; see [Ephemeris deltas](#registry-delta).

From B, collect bounded pending changes/coalescing immediately. History may help but cannot be the sole hope until snapshot end, which sustained writes could repeatedly invalidate. No per-Watch complete UUID list or unlimited pinning/backlog. If continuous coverage becomes impossible, end/request a new baseline without false completion/skipped deletions. Keep installed B stale, or the previous full view if B never completed. Server chooses reset/delta on reconnect.

Full-Scope callbacks deliver full logical Map; exact-key callbacks deliver key/optional<Data>, distinguishing empty value/deletion. Notify the first empty baseline too; state/changed carries unready/error rather than deletion. Observer exposes one/stop; estimates cannot alter authoritative baseline and new Data supersedes them.

Prepare before publication for every batch. Disconnect/OOM/overload/cancel preserves delivered views/resume positions. Publish full state before invoking callbacks, without server waiting for user ACK. Direct callbacks must return quickly; blocking them has no continuous-network-progress guarantee. Held old Views cannot pin server history.

WatchRequest declares no view_bytes; server neither estimates client container memory nor pretraverses. [SDK resource rules](../comet/cpp/README.md#local-resources) govern view/candidate budgets and stop-on-limit; Star separately bounds preparation/send.

HTTP/2 flow control does not track accumulated application Map memory and cannot replace application budgets. Bound queued and actual in-flight data; if coalescing still exceeds budget, end only that stream without holding domain locks across network waits or blocking writers/other subscribers. Retain buffers until gRPC completion. See [flow control](https://grpc.io/docs/guides/flow-control/).

<a id="registry-delta"></a>
<a id="ephemeris-delta"></a>

### Ephemeris Delta Projection

EphemerisChange.data replaces full opaque Data, not its fields or Attr/TTL/UUID/order. Applications still receive complete registration views, never half-records.

| Projection/change | Action |
| --- | --- |
| Reset, including first exact-UUID hit | Full record with Attr/Data |
| UUID exists in baseline; only Data changes | data, omitting Attr |
| Created after baseline, including later merged updates | record with target-version Attr/latest Data |
| Removed/expired/finally deleted | erase |
| Pure renewal, unchanged content | No content action/new cursor; recovery may confirm existing progress |

Choose action from covered creation/update/end history, without client UUID inventories/per-Observer complete membership sets. Coalesce only baseline→target: a new record cannot become Data-only; final erase wins. New objects use new IDs; an existing Beacon can recover its ID, but post-delete visibility requires a full record and generation isolation. If Attr knowledge is unproven send record; if continuity is unproven reset. Never label later current values with an earlier completion version.

SDK applies data only to the complete baseline continued by this batch. Reset data/erase is invalid; apply data for absent/deleted UUID invalidates the batch without empty Attr, ignore-and-advance, or indefinite waiting. End the stream, retain stale full state, and allow one versionless snapshot retry. If a successful new baseline still cannot sustain valid deltas, stop/report protocol error rather than reset/fail looping. Ordinary disconnect uses normal backoff.

Empty data is valid, distinct from absent action/erase. At most one final action per UUID. Full-record fallback cannot change an existing UUID's immutable Attr. Protocol/cancel/budget/allocation failure publishes no subset. Data-only updates may share immutable Attr rather than copy it to construct the public full view.

This avoids retransmitting Attr per update/subscriber but not UUID/Protobuf/gRPC/TLS/scheduling costs. Messages/encoding exist; throughput/latency benefits require authorized measurement.

## Errors and Resource Boundaries

Errors separate stable reason, effect certainty, and bounded detail. gRPC categorizes transport; Comet details explain business semantics. Never drive recovery by strings alone. Transport rejection/timeout/disconnect may lack details; sent writes remain unknown, not implicitly unapplied. Post-commit exceptions/cancel obey [commit boundaries](#commit-and-exhaustion-boundaries).

| Reason | gRPC classification | SDK policy |
| --- | --- | --- |
| Invalid ID/missing action/TTL | INVALID_ARGUMENT | Fail without modifying input/retrying |
| Missing/invalid Session | UNAUTHENTICATED | Coalesce business reauthentication; rejected login pauses for credential update, never recursively relogs; domain retry rules still apply |
| Internal Sector/wrong service role | PERMISSION_DENIED | Reject without anonymous downgrade or new Scope ACL |
| Catalog version/equal-version body conflict | FAILED_PRECONDITION, reason=version | One same-target repair only for definitely-unapplied whole batch within the call; no background replay |
| Almanac below Reader floor | UNAVAILABLE, reason=version | Preserve full view/floor, bounded catch-up backoff; no old-snapshot downloads or Publisher-conflict handling |
| Local Catalog missing/expired/watermark-only, without higher-version conflict | NOT_FOUND, reason=ended | Current Renew fails; new SDK has no auto-renew/restore; later explicit update carries full Data/internal version |
| Ephemeris missing/ended | NOT_FOUND | Current update/renew fails; established Beacon separately restores same-ID generation, without old Data replay; Remove never starts recovery |
| Obsolete order/invalid cursor/history gap | FAILED_PRECONDITION | Distinguish reasons: old writes cannot overwrite; Watch obtains baseline |
| Oversized request/concurrency/Session/subscription capacity | RESOURCE_EXHAUSTED | reason=limit for permanent/input cap, busy for temporary admission; stop repeated oversized-view downloads, bounded pressure backoff, no string guessing |
| Temporary unavailability/changed instance | UNAVAILABLE / FAILED_PRECONDITION | Back off/reconfirm target without redirecting old writes |
| No initial calibration/unusable local clock | UNAVAILABLE, reason=clock | No new deadline commit; bounded backoff and existing identity uncertainty. Reference loss alone is not clock/ended/instant Client switch |
| Evicted Almanac commit proof, internal management | FAILED_PRECONDITION, reason=uncertain | Original outcome unprovable; no automatic higher-version overwrite based on current state |

Effect defaults unknown; only proven local/server precommit rejection is unapplied. Expose current versions/TTL ranges/resource bounds only to authorized requests, never secrets/payload. Callback exceptions cannot change returned results/commits.

“This retry changed nothing” does not prove “the original never committed.” Later auth denial, expiry, or insufficient evidence cannot erase an earlier sent operation's uncertainty. Error/Receipt distinguishes attempts from logical operations and settles once, without later rejection replacing the result.

### Server and Encoding Budgets

These are configurable, unmeasured implementation starting values, not permanent protocol maxima. [Client budgets](../comet/cpp/README.md#local-resources) are independent; no Limits negotiation/cache.

| Resource | Initial value | Accounting/policy |
| --- | --- | --- |
| Sector/Spectrum | 128 bytes each | UTF-8 bytes, no character truncation |
| Almanac/Catalog key | 1024 bytes | Nonempty; reject excess |
| Individual business Buffer | 1 MiB | Separate Catalog value/Attr/Data; empty valid |
| Catalog Publish | Single or1..128 same-Scope unique keys | Combined keys/bodies≤1 MiB; atomic, no Delete |
| Comet message send/receive | 8 MiB | Includes encoding, distinct from payload cap |
| Star peer/Polaris data message | 8 MiB | Configure both ends independently of4 KiB control limits; Admission/Pulse limits unchanged |
| Internal page/packet target | 256 KiB | Complete record/commit units; larger single item may occupy a packet under hard message cap |
| Watch page target | 256 KiB | Complete records; large item may occupy a page within message/view budgets; no64 KiB wire rule |
| Per-subscription backlog | 8 MiB | Queued and in-flight; end slow stream if coalescing cannot fit |
| Star controlled business storage | 512 MiB | Current/history/idempotency/preparation/send together; not whole-process RSS |
| Business Sessions/active subscriptions | 4096 each | Pending auth/idle Session counted without idle eviction; anonymous subscriptions counted |
| Concurrent business unary admission | 256 | Long streams separate; reserve renewal/cleanup capacity |

Validate the whole path: maximum legal Almanac/Catalog value and full Attr + Data plus Scope/key/UUID/encoding must fit internal synchronization/external Watch hard messages. Soft pages may be exceeded by one legal record, hard messages may not. Reject configurations that accept writes but cannot replicate/subscribe. Static inter-node incompatibility fails explicitly, without Limits/Inspect/truncation. Transport configuration and encoding checks must agree.

Charge current/history/evidence/preparation peaks and reserve [recovery/maintenance](../common/README.md#capacity). Charge actual shared allocation, not handles alone. gRPC/TLS/allocator overhead is not automatically covered; budgets are not RSS caps. Do not evict live data to admit writes; keep renewal/expiry/cleanup progress possible.

Handler concurrency does not bound earlier receive/Protobuf allocations; configure transport/message resources too and measure only after authorization. Count current data, highest-version tables, source/downstream histories, and in-flight references. Shared values allocate once, but pointers alone are insufficient accounting. Catalog retry has no LRU; initial Create has no deduplication cache. Recovery metadata is bounded; orphan registrations consume capacity until actual TTL.

Polaris idempotency evidence lives in bounded durable history, not Star Sessions. Dynamic source history is per-instance/domain; visible history per-domain/Scope. Neither substitutes for the other or guarantees a minimum usable window for arbitrarily slow peers.

Session/Watch use callback/async paths, not blocking Reads occupying business threads. Cold large snapshots/delta projection cannot traverse unboundedly inside one reaction. Advance bounded pages and use existing shared bounded workers for expensive work, not per-stream threads.

### External Keepalive

Configure HTTP/2 Keepalive explicitly for idle Session/Watch rather than relying on normally disabled client probing. Initial deployment:60-second client interval,20-second timeout; Star permits PING at this rate with active RPCs. Match proxies/both ends; avoid far-subminute probing. No active calls means no PING, app Session heartbeat, or idle auth scan. It checks connected transport, not business health behind a proxy or1-second TTL. Renew deadlines/local budgets remain independent. These configurable failure-detection values are not wire fields; see [Keepalive](https://grpc.io/docs/guides/keepalive/).

Long RPCs consume HTTP/2 stream slots. A shared Client need not put every domain/stream on one underlying connection; transport queueing spends deadlines. See [SDK isolation/accounting](../comet/cpp/README.md#local-resources). Handler counts/TCP ordering alone cannot guarantee renewal latency.

<a id="generation"></a>

## Generation and Sources

```powershell
./tools/generate-proto.ps1
./tools/generate-proto.ps1 -Check
```

```bash
bash tools/generate-proto.sh
bash build.sh generate
bash build.sh check-generated
```

Reuse project protoc 36.1, protoc-gen-go1.36.12, protoc-gen-go-grpc1.6.2, and grpc_cpp_plugin1.84.0. Ordinary builds do not run generators; generators download no tools. AGENTS authorization still applies. Go output is `internal/generated`, separated by proto through `tools/generate.py`; C++ output is `common/src/generated`. Pulsar generates only C++ Pulse types; Orbit pulse_endpoint generates in both languages. Go module/import/go_package is `github.com/eosforge/astra`. Never hand-edit generated code or retain alternative old-schema entry points.

`message-ids.lock` is legacy frame-number history only, not gRPC routing. Existing Rust generation checks do not restore Rust services. Isolated push protocol `bench/proto/probe.proto`, package `proto.astra.bench.v1`, is never registered in production. See [validation](../docs/validation.md).
