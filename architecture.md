# Verdandi Architecture

## 1. Purpose

Verdandi separates reusable distributed coordination from the business logic
of Bifrost, Hermes, and future consumers. It supplies leased discovery,
observed-state synchronization, version-aware leadership, persistent Catalog
KV synchronization, desired-state distribution, acknowledgement, and local-view
mechanics. Consuming systems retain their own configuration schemas, routing
rules, admission policy, and traffic behavior.

The architecture is language-neutral. A conformant SDK is one implementation
of the protocol, not the protocol's source of truth.

## 2. System Context

```text
                       administrative API / source of truth
                                      |
                                      v
                               +-------------+
                               |  Publisher  |
                               +-------------+
                                      |
                  Catalog/desired/Leader state + wake signals
                                      |
                                      v
      +-----------+           +---------------+           +-----------+
      | Sentinel  |---------->| Redis primary |<----------|   Node    |
      +-----------+ resolves  +---------------+  observe  +-----------+
                                      ^
                                      |
                                  synchronize
                                      |
                               +-------------+
                               |  Selector   |
                               +-------------+
                                      |
                             immutable local view
                                      |
                                      v
                             consuming application
```

Publisher, Node, Selector, Campaign/Leader, and Catalog Subscriber are
logical roles. One process may hold more than one role, but each role retains
separate permissions and ownership. For example, a Bifrost Dispatcher can
publish its own Registration while using a Selector to consume eligible Proxy
Registrations.

## 3. Why Redis Is Not the Product Boundary

Redis initially provides:

- TTL-backed leased records;
- atomic small state transitions through Lua;
- bounded paginated membership indexes;
- immutable document storage;
- fast current-pointer reads;
- low-latency Pub/Sub mutation events; and
- mature Standalone and Sentinel deployment modes.

Verdandi still owns the behavior that Redis does not provide: schemas,
Registration UUID fencing, revision rules, hashes, resubscribe
ordering, authoritative reload, local immutable views, shared-limit
interpretation, capacity enforcement, error taxonomy, acknowledgement
convergence, and cross-language semantics.

Public APIs therefore use Verdandi types and do not expose a Redis client,
pool, command result, script object, or Sentinel library type. This makes
driver replacement possible and prevents consumers from bypassing protocol
ownership.

## 4. Roles and Ownership

### 4.1 Node

One Node is one running service process. On every start the SDK creates a fresh
Registration UUID and stores the process under its own Redis Hash key.

The Node owns:

- its current Registration Hash and lease;
- its bounded load sample;
- its bounded process-memory desired Registration cache and confirmation status;
- its desired-configuration acknowledgements;
- its local desired-state validation and activation lifecycle; and
- republishing its complete Registration after recovery.

The supported Node API cannot mutate shared Catalog KV, desired state, another
Registration, or another Registration's acknowledgement. ACLs bound the
credential's role and key scope; a deliberately raw ACL-authorized mutation is
outside the SDK contract.

### 4.2 Publisher

The Publisher owns:

- desired lifecycle and configuration publications;
- Catalog KV mutation;
- Redis-backed revision allocation for desired targets and Catalog mutations;
- retention and cleanup of superseded immutable publications;
- observing Registrations and acknowledgements; and
- reconciling desired and observed state.

Redis persistence is the protocol `1.0` source for current Publisher state.
The core SDK does not require or manage a second durable configuration or audit
store. The Redis-backed Zone configuration is current protocol state in the same Redis
deployment, not a second source of truth. A future statistics/audit synchronizer
may consume changes and persist history independently; it does not become part
of ordinary publication.

### 4.3 Selector

The Selector owns:

- subscription and authoritative Registry recovery;
- acknowledged subscribe/scan/PING synchronization;
- one connection-generation RedisClock;
- immutable local publication;
- conservative local Node lease expiry;
- a separately bounded non-selectable retained recovery view;
- application callbacks or accessors for eligible records; and
- bounded local overlays such as optimistic pending reservations.

Its execution topology is fixed: one persistent listener/state-machine task is
the sole Pub/Sub receiver and live-state owner, while one optional temporary
task performs either a full scan or targeted repair. The temporary task returns
a fenced candidate to the listener and is cancelled and joined on shutdown.
Thus a Selector uses one task in steady state and at most two during
synchronization; reconnect does not accumulate generation readers.

The public view is usable only after the current generation crosses its
subscribe/scan/PING or targeted-repair fence. During initial sync, reconnect,
or repair, Snapshot, Find, FindRetained, One, and Any return explicit
`unavailable`; private last-known and retained state may still be used only to
build the next candidate view.

The Selector does not own the universal meaning of eligibility or load. The
consumer supplies domain filtering and choice policy.

Selection policy runs as a serialized, synchronous local transaction. Go
borrows a reusable candidate view; Rust exposes immutable borrowed candidate
references plus opaque choices. The callback may stage Data changes through
the SDK, then return zero, one, or multiple unique choices. A callback error,
deadline, invalid choice, or empty result rolls back the complete transaction.
A valid result commits only process-local predictions and returns detached
values. Redis publication continues through the owning Registration; selection
itself performs no Redis I/O.

SDK `0.1.0` deliberately evaluates injected `One`/`Any` policy by scanning the
borrowed candidate view in O(N). A detached complete Snapshot is an explicit
heavy operation and copies O(N) records; callers requesting it accept that
time and memory cost.

Prediction reconciliation is field-granular. Renew preserves every local Data
prediction because it does not change content revision. A later remote content
revision replaces fields whose encoded bytes changed while retaining locally
predicted fields whose remote base bytes did not change. Unregister, natural
expiry, or authoritative absence removes the prediction with the Registration.
This mechanism reduces short-term load skew but is not a distributed capacity
reservation.

### 4.4 Campaign and Leader

The Campaign owns one immutable positive safe-integer version and one private
readiness token/lease. The token is its internal lifetime identity; no separate
public Campaign ID is required. It does not create or require a Registration.
Each ownership attempt creates a different private Leader token. The Leader
term owns synchronous admission validity, renewal, callback cancellation,
joined cleanup, and exact-token release. Readiness and ownership tokens remain
distinct from each other and from routing identity.

Every SDK uses the same numeric comparison and claims only when its local ready
view considers the Campaign best. Changing priority requires closing that
Campaign and creating another readiness-token/version pair. A larger Campaign
version causes cooperative retirement; it never deletes a live owner before
that owner's application cleanup completes. Equal versions use the first
successful Redis claim.

### 4.5 Catalog Publisher and Subscriber

A Catalog Publisher encodes an application-owned Value, Array, or Map and uses
one atomic last-write-wins Replace, one exact-base Patch, or complete Delete.
Patch adds/overwrites Map fields or overwrites existing Array indices; removal,
shape change, and Value updates require Replace. A Subscriber owns complete raw
covered values in memory, applies full Pub/Sub operations when safe, repairs
from authoritative Hash/ZSET state, and labels retained data non-synchronized
as soon as continuity becomes uncertain.

### 4.6 Administrator

The Administrator owns Zone creation, identities, Redis/Sentinel ACLs,
credential rotation, and later changes to the non-expiring Zone capacity
configuration. Client bootstrap fills missing common defaults, but
administrative mutation remains outside ordinary Register/Selector APIs.

## 5. Data Ownership Matrix

| Data | Writer | Reader | Authority |
| --- | --- | --- | --- |
| Registration | owning process UUID | Publisher, authorized Selectors | exact Registration UUID plus lease |
| Registry | owning Registrations | Publisher, authorized Selectors | atomic Registration, membership, expiry, and event mutation |
| Catalog Value | authorized Publishers | Catalog Subscribers and authorized Nodes | raw Hash plus global/field revisions and live/deleted ZSET indexes |
| Campaign readiness | owning Campaign | election Lua | immutable version plus readiness token and lease |
| Leader term | winning Campaign token | Leader Selector and authorized consumers | private ownership token and lease |
| Desired configuration | Publisher | targeted Nodes | Redis ACL, target, and revision |
| Acknowledgement | owning Registration | Publisher | exact Registration UUID |
| Zone configuration | Registration Client, Administrator | Registration Clients | Redis Hash plus last-valid local snapshot |
| Zone/ACL policy | Administrator | Redis/Sentinel runtime | administrative source of truth |

ACL credentials are scoped by role and Zone rather than by Registration
UUID. No ordinary role receives a catch-all credential. ACLs define which principals
are trusted to write each scope; atomic SDK/Lua actions preserve supported
multi-key invariants. Raw writes granted by an ACL are outside the guarantee.

## 6. Client Lifecycle

One root Client owns the Redis connection; Registration and Catalog Clients
attach their own scripts, configuration, workers, diagnostics, and optional
Catalog persistence without duplicating it.

Both root Clients are deliberately thin Redis wrappers. They own connectivity,
ordinary/connect timeouts, the shared connection pool, connection-recovery
backoff, bounded root Key/Hash commands, and a shutdown signal. They own no Zone, domain admission, or domain worker join. Registration
and Catalog configure Zones independently, so one ACL-authorized root Client may
serve multiple Zones. Root close broadcasts loss and closes the driver without
waiting for domain Clients; each domain observes the signal and owns its own
joined shutdown. Deterministic application cleanup closes domain Clients before
the root. Registration bootstrap validates Redis 8 with `HELLO`; root bootstrap
uses only `PING` and does not require `INFO` permission.

Go `Close()` synchronously releases go-redis. Rust `close().await` awaits Fred's
asynchronous `quit()` but still does not join domain work. Rust `Client` is a
cheap clone over one private `Arc<Inner>`; domain Clients retain that root type
directly, and its crate-private methods include the bounded factory for their
dedicated Pub/Sub connections. There is no second private Transport type.
Dropping the last root-or-domain-held clone schedules best-effort cleanup when a
Tokio runtime is available; explicit close is the deterministic contract.

Go uses its language-native concrete transport directly: `Client.Redis()`
returns the same borrowed `*redis.Client`, `Done()` exposes only permanent root
shutdown, and `Timeout()` exposes the normalized immutable timeout.
Registration and Catalog consume those capabilities without an internal bridge
or watcher goroutine. The root still owns `Close`; a borrower must not close or
reconfigure the driver. Raw commands made through the pointer are controlled by
Redis ACLs and deliberately sit outside Verdandi validation, resource bounds,
multi-key invariants, and stable error mapping.

```text
Disconnected
    -> Resolving
    -> Connecting
    -> Configuring
    -> Subscribing
    -> Synchronizing
    -> Ready
```

- `Resolving` returns the configured endpoint in Standalone mode and queries a
  bounded set of Sentinels in Sentinel mode.
- `Connecting` establishes the authenticated ordinary Redis-command path with
  explicit deadlines. A Selector opens its dedicated Pub/Sub connection when
  constructed. Each Catalog Subscriber opens one dedicated multi-channel
  persistent Pub/Sub listener and creates at most one temporary authoritative
  synchronization/repair task; Publisher-only Catalog Clients start no
  background worker.
- Registration `Configuring` fills missing common defaults, reads and validates
  `verdandi:config:<zone>`, and publishes one complete local snapshot. The first
  published Registration starts one Client-shared refresh task; the last
  Registration stops it. A new Register first performs an immediate refresh,
  while explicit refresh remains available without a Registration. Invalid
  refreshes retain the prior complete snapshot and interval and emit
  diagnostics.
- `Subscribing` waits for the current subscription acknowledgement and assigns
  a new local subscription generation.
- `Synchronizing` loads authoritative pointers, documents, records, and leases
  after subscription is live. Each Selector calibrates its connection-generation
  RedisClock during this step and periodically recalibrates it inside the same
  persistent listener task; it does not create a second clock task.
- `Ready` permits current state to be consumed. Notifications from an older
  subscription generation are ignored.

Any connection failure invalidates the affected generation. Connection
establishment/recovery uses bounded exponential backoff with jitter, while the
configured pool bounds concurrent dials. Neither SDK automatically replays a
dispatched Redis command; its caller or domain state machine must explicitly
reconcile the stable error or ambiguous outcome.

## 7. Subscribe-Before-Read Ordering

The safe bootstrap sequence is:

1. connect Redis-command and subscription channels;
2. subscribe to the bounded set of authorized wake channels;
3. wait for subscription acknowledgement;
4. for a Registry, calibrate RedisClock and load bounded membership/record
   pages while buffering later events;
5. send a nonce-bearing PING on the subscribed connection and wait for its
   ordered PONG;
6. reconcile pages and events by per-Registration revision;
7. for a Catalog or another shared-revision class, execute its separately
   specified barrier/revision procedure;
8. validate and publish the local view;
9. process later events by checking their target revision; and
10. restart synchronization on a gap, mismatch, malformed message, overflow,
   or reconnect.

Reading before subscription creates a race where a publication between the
read and subscribe can be missed forever. Pub/Sub events are the normal live
incremental path but are not durable or replayable. Registry synchronization
uses a subscription acknowledgement, per-Registration revisions, complete
reset events, and an ordered subscription PONG to fence the scan without a
Registry-wide revision. Catalog uses the same subscribe-before-read/PONG
ordering around its global revision, live/deleted ZSET indexes, and atomic
per-path Read; the two data classes still use distinct authoritative state
machines.

## 8. Authoritative Synchronization Patterns

### 8.1 Registry

One operation-specific Registration Lua mutation changes one bounded record,
its Registry membership field, both matching expiries, and one inline event:

```text
Registration + membership + expiry -> register/update/renew/unregister event
```

The Registration owner supplies a per-UUID content revision. `register` carries
complete state, `update` carries a non-empty content patch and advances the
revision, `renew` changes only Redis timestamp and the fixed lease deadline, and
`unregister` removes the whole Registration. There is no Registry-wide
revision, mutation history, Stream, or business barrier event.

A Selector subscribes before walking bounded membership pages. After its final
record read it sends `PING <nonce>` on the same subscribed connection and waits
for the ordered PONG. Buffered register/update/renew/unregister events reconcile the
scan. A per-UUID gap performs one bounded record fetch followed by another
subscription PING fence. A subscription disconnect, hidden reconnect, PONG
timeout, malformed event, or buffer overflow restarts the generation.

Every register, update, and renew event carries the Redis write timestamp. The
Selector combines it with the immutable cached TTL and its connection-generation RedisClock, so normal events
refresh local deadlines without a `PTTL` read. Natural TTL expiry has no event;
one indexed local deadline per UUID removes stale state from selection. A
non-explicit expiry or fenced absence may retain payload in a bounded recovery
cache, while explicit `unregister` purges it immediately.

### 8.2 Catalog Value

Catalog remains a different data class with multiple writers, one Redis-owned
Zone revision, per-field update revisions, one current Hash per Path, and
bounded live/deleted indexes. Publisher SDK code owns external typing and
complete-value validation. Replace and Delete are last-write-wins; Patch is
independently atomic only against its exact base. Subscribers apply complete
notifications or repair with atomic Read. A cursor below `@floor_revision`
scans complete indexes; a usable cursor reads only newer members.

One complete Catalog defaults to a 512 KiB encoded limit and may be configured
up to the 4 MiB protocol ceiling. Index pages, notification parsing,
concurrent path reads, synchronization duration, diagnostics, and local
checkpoint I/O are bounded independently. A typed Load decodes a detached
projection; notification and field-level repair remain proportional to the
accepted operation whenever a later Replace does not require a full read.

### 8.3 Immutable Desired-Document Publication

Desired configuration uses the immutable publication pattern:

```text
payload -> immutable chunks -> immutable manifest -> atomic current pointer
                                                       |
                                                       v
                                                   wake signal
```

Publisher procedure:

1. validate complete input and resource limits;
2. encode canonical payload bytes;
3. split into bounded chunks;
4. atomically reserve the next target revision;
5. write revision-specific chunks;
6. write a manifest containing counts, lengths, hashes, and metadata;
7. atomically replace the small current pointer under the target's revision
   rules;
8. emit a bounded wake notification; and
9. retain previous material for a bounded recovery window before cleanup.

Consumer procedure:

1. read the current pointer;
2. reject rollback on an unchanged Redis connection, unsupported protocol,
   invalid target, or expired state;
3. fetch the bounded manifest and chunks with bounded concurrency;
4. verify every count, length, chunk hash, and aggregate hash;
5. decode into a new candidate state without mutating the current state;
6. run consumer validation where the payload is application-owned;
7. atomically expose the complete new state; and
8. acknowledge the resulting state when applicable.

A partial write can consume storage but can never become visible because no
current pointer references it.

## 9. Registration and Load

Each process owns one small Registration Hash at
`verdandi:registration:<zone>:<type>:<uuid>`. Redis/Lua-managed Meta uses
`@name`, immutable SDK-supplied Attr uses `.name`, and mutable fixed-structure
Data is unprefixed. The complete Meta set is UUID, per-Registration revision,
Redis timestamp, TTL, and positive integer Registration Version. No absolute
expiry field is stored. Leader election uses its separate Campaign Version and
does not inspect Registration Version.

The Registry Hash and Pub/Sub channel are both
`verdandi:registry:<zone>:<type>`. Its UUID field stores the current
Registration content revision and receives the same absolute field expiry as the
Registration key. Redis keys and Pub/Sub channels have independent namespaces,
so sharing the text is unambiguous.

Initial registration and uncertain recovery send a complete `register`.
Steady-state `update` sends changed Version and/or Data, plus UUID, the next
content revision, and Redis-generated timestamp. TTL and Attr are immutable for
the UUID lifetime. `renew` sends UUID, unchanged revision, and timestamp;
`unregister` carries only UUID as its event identity and removes the complete
Registration and membership field. Close drains admitted work, sends it only on
the current healthy generation, and never reuses that terminal UUID. Every
published Registration owns one single-slot Fields merge mailbox, one
capacity-one wake signal, one long-lived worker, one desired/confirmed state,
and one renewal timer. That worker is the sole Redis writer for its UUID. The
mailbox keeps only the latest pending Version and value for each changed Data
field; its small admission semaphore bounds result waiters without retaining
full request or Data objects. A same-batch Renew reuses an effective Update's
TTL refresh and otherwise executes independently. One Lua execution makes each
accepted record/membership/expiry/event transition atomic.

The selected SHA fixes the private protocol-v1 request layout. Its control
arguments are values at operation-specific positions, while only dynamic Attr
and Data use named field/value pairs. This removes request-map parsing from
Redis without changing named replies or MessagePack events. Registry membership
value and field expiry use one Redis 8 `HSETEX PXAT`; Lua rejects an absolute
deadline above the Hash-field ceiling `2^46-1` after obtaining Redis time.

Client bootstrap stores the six default Registration field/count/byte limits in
the non-expiring Zone configuration Hash without overwriting existing fields.
An authorized backend can later replace related limits atomically with one
multi-field `HSET`. Each Client retains the last complete valid snapshot,
refreshes it on a configurable interval or explicit request, and validates each
UUID's complete projected record locally before incrementing revision or
calling Redis. Consequently, steady-state Lua Update work is proportional to
the patch and performs neither a configuration read nor a complete-Hash
capacity scan. Lua does not repeat request-shape, field, immutability, or
capacity validation. It joins only the Redis-state-dependent revision, clock,
Hash/membership, expiry, reply, and publication transition atomically.
ACL-authorized bypass of the SDK is outside the supported consistency boundary.

Each Registration worker's desired/confirmed cache is volatile working state,
not local persistence. No SDK-owned Registration UUID, content record, replay
log, local database, or WAL survives process termination. A surviving process
may use its cache to repair Redis after reconnect or failover; a restarted
process generates a new UUID and lets the former Registration expire by TTL.
The Selector active and retained views follow the same process-memory-only rule.

Load is sampled and quantized. Publishing every accepted or closed connection
would create avoidable Redis command volume and synchronization noise. A Node
publishes `renew` when its lease requires renewal. It publishes `update` when a
minimum interval passes, readiness or capacity bucket changes, or a configured
load threshold is crossed.

A Selector that assigns work can maintain local pending reservations:

```text
estimated load =
    last authoritative Registration load
    + local pending reservations
```

The reservation begins immediately after local selection and expires or
reconciles when a later authoritative Registration patch arrives. Its size,
lifetime, and storage are bounded. This improves decisions without pretending Redis
provides a transaction spanning the Dispatcher and selected Proxy.

## 10. Catalog Value Synchronization

Catalog is persistent coordination state, not a leased Registration and not a
desired-document replacement. One `Path(part, id)` identifies one bounded
Value, Array, or Map Hash. Values have no TTL. Deletion is explicit; the
live/deleted ZSET indexes retain enough bounded identity to distinguish known
deletion from unknown absence.

One Zone-global Redis revision orders every accepted mutation. A live Hash
stores its current revision, most recent Replace revision, shape, encoded byte
size, and complete application fields. A companion field-revision ZSET records
the last mutation of each application field.

Publisher uses three state transitions:

```text
complete source -> Replace -> complete Hash
exact local base + field delta -> Patch -> complete Hash
any current/absent state -> Delete -> retained tombstone
```

Replace and Delete use Redis-primary execution-order last-write-wins. Patch
projects affected fields without a lock, then Lua must still match the exact
base revision at atomic commit; a same-Path race becomes a stale failure rather
than a mis-sized write. There is no external Path lock, segmented Replace,
field deletion, large-value reference, or multi-Path transaction.

The Hash key is also the Pub/Sub channel. Lua publishes the complete accepted
operation from the same state it commits, but Pub/Sub is only the fast path. One
Subscriber multiplexes all of its exact and pattern coverage on one dedicated
persistent listener and routes actual channels into stable Entries. Initial
alignment and later repair share one temporary task slot; pending work
coalesces there and the task exits once idle.

The synchronization fence is:

```text
subscribe acknowledgements
        -> Zone revision/floor + live/deleted index alignment
        -> atomic per-Path Read
        -> subscribed PING/PONG
        -> Zone metadata recheck
        -> publish complete local Entries
```

Replace and Delete can apply immediately. Patch applies only when its event
base equals the Entry revision. Otherwise Read uses per-field revisions for an
exact delta, unless a later Replace requires the complete Hash. Reconnect,
malformed frames, or a cursor outside retained history uses the same
authoritative path. Floor advances only when bounded tombstone retention
actually evicts history.

Optional bbolt (Go) or redb (Rust) state is a monotonic restart checkpoint. The
complete working set remains in memory, Redis remains authoritative, local
formats need not interoperate, and nothing is replayed from disk back to Redis.
The first checkpoint error disables persistence for that Client generation
without stopping live in-memory synchronization.

Go `Load<T>(Entry)` and Rust `Entry::load::<T>()` decode through
application-owned field codecs. A Subscriber may therefore hold unrelated
configuration types at different Paths. Delete changes a stable Entry to
Deleted; later Replace reuses the same Entry identity.

## 11. Leadership

Leadership follows the hardened Hermes separation of concerns:

1. A fresh private readiness token identifies one in-process Campaign lifetime
   independently from service discovery.
2. Its readiness lease proves that Campaign worker is actively eligible.
3. A different private ownership token identifies one exact Leader term.
4. The SDK applies the protocol-defined immutable positive-integer version
   comparison and attempts claim only when its local ready view considers the
   candidate best.
5. Redis atomically verifies the exact readiness token/version and empty
   ownership; equal versions are first-successful-claim wins.
6. A live owner is never preempted. When its SDK observes a preferred ready
   version, local admission closes, cleanup joins, then
   exact-token release permits replacement.
7. Exact release emits one latency-only wake, while bounded retry and lease expiry
   remain authoritative.
8. Every election domain has zero or one application-active Leader; handoff,
   cleanup, uncertainty, and fence acquisition may leave it without one.

Redis 8 stores Campaign readiness as one independently expiring Hash field per
private token. SDKs page it with `HSCAN` and maintain an immutable local ready
view. Redis keeps no version-order index and has no stale index to sweep; a
stale SDK view can therefore elect a lower version temporarily before
converging. No candidate-count limit is encoded into the election contract.

`LeaderTerm.Valid()` or its language equivalent checks both cancellation and a
conservative monotonic deadline before every new application operation. Any
uncertain renewal invalidates the term immediately. The SDK closes admission,
cancels term-owned work, and joins application cleanup before releasing exact
ownership or activating another local term. Verdandi has no availability mode
that admits work through ownership uncertainty.

Standalone uses its one configured Redis primary as the term authority.
Sentinel is asynchronously replicated and can expose divergent old and promoted
primary histories. A Sentinel Campaign therefore acquires one
deployment-provided durable fence or advisory lock after Redis claim and before
the application callback begins. The SDK keeps the claimed Redis term renewed
while fence acquisition is pending, then exact-token confirms Redis ownership
again after acquisition. Only that confirmed pair starts the callback; failed
confirmation releases the fence without activating application work. The SDK
holds an active fence through invalidation and joined cleanup. A replacement
Redis claimant is not application-active while waiting for the same fence.
Missing or failed fencing leaves the election domain without an active Leader.

## 12. Desired-State Reconciliation

Desired configuration is a validated envelope around opaque consumer bytes.
Verdandi validates transport, ACL-authorized metadata, and integrity invariants; the consumer
validates business semantics.

Zone is implicit. Desired targets remain partition, service-within-partition, or one
Registration. Concurrent Publishers use Redis-revision last-write-wins; the
highest successfully installed current revision is active.

The activation boundary is transactional from the local application's public
view:

```text
receive -> verify envelope -> decode candidate -> consumer validate
        -> build candidate runtime -> atomic swap -> ACK active
```

Failure at any stage preserves the previous valid runtime only until its own
lease expires and emits a rejection ACK with a stable code. Verdandi must not
partially update a live route table, listener set, policy object, or another
application payload.

Lifecycle such as `Standby`, `Active`, `Draining`, and `Offline` should be
modeled as desired state when the consumer supports it. This permits a
Publisher to observe eventual convergence through ACKs.

## 13. Commands

Commands are deferred from SDK `1.0.0`. A future command is the exception, not the default coordination mechanism. A command
is appropriate only when the desired end state cannot describe the operation,
for example a one-time diagnostic capture with explicit expiry.

Command delivery is at least once. Each command has an ID, idempotency key,
target-Registration policy, issue and expiry time, bounded opaque payload, and
result ACK. Handlers record or derive duplicate suppression before repeating a
side effect. History retention is bounded and acknowledgement-aware.

Exactly-once execution is not claimed.

## 14. Standalone and Sentinel

### 14.1 Standalone

Standalone uses one configured Redis primary. It is operationally simple but
has a deliberate single point of failure. Protocol keys, Lua, schemas,
recovery, and local leases are identical to Sentinel mode.

### 14.2 Sentinel

Sentinel resolves one named Redis primary. A production reference topology is
expected to use one primary, replicas, and at least three independently placed
Sentinel processes, but exact deployment policy belongs to operator guidance.

After connection loss:

1. discard or fence failed Redis-command and subscription connections;
2. query current Sentinel state with bounded disagreement handling;
3. connect to the resolved primary;
4. reread the Zone configuration and publish it only if the complete recognized
   set is valid, otherwise retaining the last valid local snapshot;
5. reload the four Registration operation scripts as necessary;
6. subscribe and wait for acknowledgement;
7. reload all authoritative state; and
8. republish Node-owned leased state; Publisher applications separately decide
   whether to issue a new semantic publication after reading current state.

A healthy existing Redis connection may continue while Sentinel itself is
temporarily unavailable. Once that Redis connection fails, the client cannot
safely recreate it from a remembered address without current resolution.

## 15. Consistency and Failure Semantics

### 15.1 Redis Command Failure

Return an explicit stable error. If a write outcome is ambiguous, reconcile by
reading authoritative state or republishing the same idempotent operation.
Blind retry that can create a new semantic action is prohibited.

### 15.2 Pub/Sub Loss

Invalidate the subscription generation, reconnect, resubscribe, then reload
authoritative current state. Registration `update` applies only as the next
content revision against a matching per-UUID base, while `register` carries a
complete replacement; Registry recovery restarts
subscribe/scan/PING. Catalog repairs from its live/deleted ZSET indexes and
atomic Read; per-field
revision recovery is used when possible, while a cursor below floor or a later
Replace forces broader authoritative alignment.

### 15.3 Primary Failover

Asynchronous replication can lose a write acknowledged by the former primary.
Nodes self-heal their leased Registrations by republishing complete state.
That repair uses the bounded volatile cache of the same still-running process;
it is not disk recovery and cannot resurrect a process after restart.
Publisher applications must read the new primary and decide whether to issue a
new semantic write; the core SDK does not retain a second durable source.
Applications must not claim zero acknowledged-write loss. A primary-generation
change immediately invalidates local Leader admission. A promoted primary that
lost the Redis term may grant another claim, but a Sentinel claimant cannot
become application-active until it acquires the same deployment-provided
durable fence previously held by the old generation. Fence failure preserves
safety by leaving the domain without an active Leader.
Every Redis primary change also invalidates subscriber revision watermarks and
forces an authoritative reload, which may establish a lower revision when the
promoted primary lost acknowledged state.

### 15.4 Publisher Failure

Redis retains the latest persisted Catalog Value and desired-state records. No new
revision appears while no Publisher is active. A replacement Publisher reads
current Redis state and may publish a new application-supplied value. The core
SDK does not own a second durable Publisher source or audit history; Sentinel
asynchronous failover can therefore lose an acknowledged latest write, as
already stated in the failover contract.

### 15.5 Redis Unavailability

Existing application traffic continues independently. Once Redis continuity is
uncertain, a Selector marks every public view unavailable rather than serving a
half-synchronized snapshot. Its private state still expires active entries at
their RedisClock deadlines and may keep payload in the separate retained cache
until `timestamp + 2*ttl`, subject to its independent byte budget, so a later
fenced recovery can reactivate it cheaply. A Node follows its separate
desired-state lease and consumer-owned drain behavior.

### 15.6 Incompatible or Oversized State

Reject the new state before partial activation, preserve the last valid state
only within its lease, expose a stable error, record a bounded safe diagnostic,
and publish a rejection ACK when authorized.

## 16. Scale and Partitioning

No library-wide constant defines the maximum number of services, Nodes, or
Campaigns. Each Catalog Path is one bounded complete value. Every Redis
mutation, index page, fetch group, event buffer, queue, and decoder remains
bounded independently. Complete local Subscriber state is bounded by covered
Path count times the per-Path byte limit.

The completed Registration qualification runs separate five-minute profiles
with 500 live Registrations at one `renew` or one documented-size `update` per
second, eight Selectors, 5,000-record page recovery, and Sentinel fault recovery.
Catalog functional qualification covers its Redis and SDK state machines. Its
new endurance driver passed 960/960 operations in a 30-second preflight across
four Catalogs and two Subscribers. The production-sized fault profile targets
16 Catalogs, two Subscribers, 256 fields, and 128 Replace/Patch/Delete
operations per second; its separate 24-hour interval remains unfinished. An
Update already refreshes its lease. A future installation that exceeds one
primary's measured capacity partitions by region, cell, tenant-independent
failure domain, or another explicit placement key. Each partition may use an
independent Standalone or Sentinel deployment.

There is no cross-partition atomic Catalog mutation. A global consumer merges
independent partition views and retains each partition's revision, lease, and
failure state separately.

## 17. SDK Architecture

Each SDK should preserve the same conceptual layers while remaining idiomatic:

```text
public Verdandi API
        |
protocol encode/decode and validation
        |
lifecycle, recovery, and local immutable state
        |
project-owned Redis driver adapter
        |
selected language Redis library
```

Go and Rust place application-owned field conversion above the raw protocol
values:

```text
application Attr/Data structs
        |
application Encoder/Decoder or FieldValue implementations
        |
generic typed Registration and Selector handles
        |
raw flat Fields and shared lifecycle implementation
```

The SDK does not generate application business logic. Each application owns its
stable field names and byte encodings; raw `Fields` implements the same
conversion contract. Selector values are decoded once per remote content
revision and cached, while Redis still stores independent flat fields and a
typed Update remains patch-proportional.

Rust keeps its driver adapter private. Go exposes only the root transport's
borrowed concrete pointer as an explicit language-specific escape hatch; all
Registration, Selector, Catalog, cancellation, error, and configuration
contracts remain Verdandi-owned. Metrics aggregation and history do not. A
future driver change may affect that Go-only escape hatch but must not force a
domain API or wire-protocol migration.

The SDK owns every background task it creates. Shutdown prevents new work,
cancels retry and subscription loops, joins tasks, closes connections, and
releases memory within a configured deadline.

## 18. Cross-Language Conformance

Schemas alone do not guarantee compatibility. The shared testkit owns:

- canonical encoded bytes;
- invalid boundary and malicious inputs;
- hashes and chunk-assembly vectors;
- revision and Registration-lifecycle state-machine cases;
- manifest and chunk cases;
- lease calculations;
- stable errors;
- Lua invocation results; and
- real Redis/Sentinel scenarios.

Every new SDK must both produce data accepted by existing SDKs and accept data
produced by them. One implementation is never treated as the undocumented
behavioral source of truth; a mismatch is resolved by the frozen protocol and
test vectors.

## 19. Consumer Integration

### 19.1 Bifrost

The intended mapping is:

```text
Bifrost Controller  -> Verdandi Publisher + Campaign + Catalog Writer
Bifrost Proxy       -> Verdandi Node
Bifrost Dispatcher  -> Verdandi Node + Selector<ProxyRegistration>
```

The active Controller term mutates Bifrost Catalog Values and publishes route
or configuration snapshots as opaque payloads. Proxy decodes and validates
them through Bifrost-owned contracts, activates them atomically, then ACKs
through Verdandi. Dispatcher consumes a local eligible Proxy view and makes its
own selection decision.

### 19.2 Other Consumers

Hermes or another system supplies its own service metadata and desired payload
schema, uses Verdandi envelopes and lifecycle, and passes the same protocol
conformance suite. No consumer receives privileged assumptions in the core
protocol.

## 20. Statistics, History, and Audit Boundary

The core SDK does not aggregate metrics, persist history, or provide an audit
database. A separate statistics/audit synchronizer service may consume the
SDK's bounded state and change subscriptions and write its own durable history.
That service owns metric names, labels, retention, audit schema, storage,
backpressure, and gap policy. It is a future module outside SDK `1.0.0`.

The SDK still returns bounded structured errors and synchronization status
needed for correct operation, but it does not turn them into a metrics system.
Registration worker caches and Selector active/retained views are
operational process memory, not history stores, and disappear with their owning
process.

## 21. Repository Architecture

The repository root remains a protocol/product boundary, not a language
workspace:

```text
README.md
spec/
schema/
lua/
sdk/
  go/
  rust/
  <future>/
testkit/
```

Internal planning Markdown exists only on local `alpha` under the current
branch policy. Public protocol documents and generated API documentation
require an explicit publication decision before release.
