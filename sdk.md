# Verdandi Go, Rust, C++, and C# SDK Guide

## 1. Status and Scope

The repository currently contains independent Go, Rust, and C++23 implementations of
the same Alpha `v1` Registration, Registry-Selector, and Catalog protocol, plus
an idiomatic managed C# facade over C ABI v1 and the existing C++23 core. Package
manifests use non-production Alpha SDK version `0.1.0`. This release line is
intended for distributed development and controlled integration and makes no
stable protocol promise. Registration and Selector now live in the public
`registration` child package/module/namespace; shared Client, field codecs, errors, and
connection configuration remain at the SDK root. Neither SDK re-exports the
domain declarations from its root namespace. The implemented public surface is
deliberately narrow:

- one root Client for Redis connectivity and bounded ordinary Key/Hash commands;
  Go and C++ close their concrete drivers synchronously while Rust awaits Fred
  `quit()`; none waits for domain Clients;
- one Registration child Client for Zone-limit bootstrap/refresh, scripts,
  asynchronous diagnostics, domain workers, required Zone, and Redis 8
  validation;
- one process-start Registration with `register`, `update`, `renew`, and
  `unregister` lifecycle;
- one Type-scoped Selector with acknowledged subscribe/scan/PING
  synchronization, targeted revision repair, RedisClock-based expiry, and an
  immutable local view;
- Catalog Replace/Patch/Delete publication, complete in-memory Subscribers,
  stable Entries, per-load generic decoding, and optional disposable checkpoint;
  and
- stable string error categories shared by all SDKs.

Campaign/Leader, desired configuration, acknowledgements, and Command are not
implemented by these SDK slices. Standalone Redis 8.8 and a real
three-node Redis/three-Sentinel failover topology are qualified for
Register/Selector. The current evidence includes five-minute update and renewal
phases in both languages, eight Selectors, and 5,000-record synchronization.
Catalog additionally passes its current Redis 8.8 functional/reconnect suites
and an exact-source twelve-hour campaign accepting 5,932,160 attempts through
113 planned faults with two Subscribers and final zero-key cleanup. Live TLS
topology qualification, managed Redis services, and broader reconnect-storm
profiles remain unfinished. The shared JSON and native Go/Rust/C++ TLS
configuration paths themselves support system/private roots, Standalone SNI,
and paired PEM client credentials and pass offline parsing tests. C++ passes
strict GCC, clang-tidy, ASan/UBSan, authenticated Standalone integration, and a
plain three-node/three-Sentinel smoke; its full failover, TLS, platform,
packaging, soak, and performance matrices remain open.

C# targets .NET 8 and .NET 10 with C# 14, source-generated `LibraryImport`,
dedicated SafeHandles, immutable continuous Fields, static generic
`IFieldValue<TSelf>` codecs, delayed Registration publication, ref-struct
borrowed Selector candidates, opaque transaction-fenced Choice values, and
typed Catalog loading. It adds no Redis driver, worker, listener, recovery, or
checkpoint implementation. Its independent .NET 8/10 Linux x64 Standalone and
two-promotion Sentinel regressions cover ACL separation,
acknowledged-write-loss repair, Pub/Sub generation recovery, concurrent
Registration/Selector pressure, capacity limits, and native-handle shutdown
behavior. Windows/macOS native runtimes, NuGet RID packaging,
NativeAOT/trimming, TLS, cross-language C# peers, performance, and soak remain
open.

Campaign/Leader remains unimplemented and uses its own immutable Campaign
Version plus private readiness token. It does not read or reinterpret the
existing Registration Version field, whose current Register/Update behavior is
unchanged and outside this Leader design. Stable `1.0.0` remains reserved for a
qualified Leader implementation and the complete release contract.

## 2. Shared Redis Contract

One running process receives a new 128-bit random UUID encoded as exactly 32
lowercase hexadecimal characters. The UUID is retained across Redis reconnects
and changes only when the process creates a new Registration.

```text
verdandi:registration:<zone>:<type>:<uuid>  Redis Hash with key TTL
verdandi:registry:<zone>:<type>             membership Hash and Pub/Sub channel
verdandi:config:<zone>                      non-expiring Zone policy Hash
verdandi:catalog:<zone>:<part>:<id>         complete raw Catalog Hash and Pub/Sub channel
verdandi:catalog:<zone>:@meta               global revision and tombstone floor Hash
verdandi:catalog:<zone>:@live               live Path revision ZSET
verdandi:catalog:<zone>:@deleted            retained delete revision ZSET
```

A Registration Hash contains:

```text
@uuid       SDK-generated process identity
@revision   positive per-Registration content revision
@timestamp  last accepted mutation time from Redis TIME
@ttl        immutable lease duration in milliseconds
@version    mutable positive application version
.<name>     immutable Attr top-level field bytes
<name>      mutable Data top-level field bytes
```

`@timestamp + @ttl` is the deadline; no separate expiry timestamp is stored.
The deadline may not exceed Redis 8's Hash-field absolute-expiry ceiling
`2^46-1` milliseconds. The Registry Hash field is `<uuid> = <revision>` and
receives the same absolute expiry as the Registration key. Lua changes the
record, membership plus field expiry through `HSETEX PXAT`, key expiry, and one
MessagePack event atomically.

The private Lua request ABI uses fixed control-value slots: Register
is `uuid, revision, ttl_ms, version`, Update is
`uuid, revision, version-or-empty`, Renew is
`uuid, revision`, and Unregister is `uuid`. Register then carries complete
named Attr/Data pairs; Update carries named changed Data pairs. The selected SHA
identifies protocol and operation, so request control names are not repeated.
Replies and MessagePack events remain named alternating key/value arrays.

`zone` is `[A-Za-z]{1,32}`. `type` is
`[A-Za-z][A-Za-z0-9_.-]{0,63}`. Applications never construct these keys through
the supported SDK API.

## 3. Redis-backed Zone Configuration

Configuration defaults are persisted in Redis, not merely compiled into an SDK.
During Registration child `Open`, each SDK reads `verdandi:config:<zone>`, fills only missing
recognized fields with `HSETNX`, then rereads and validates the complete set.
Concurrent first Clients use identical defaults and do not overwrite an
existing administrative value.

| Redis field | Default | Protocol ceiling |
| --- | ---: | ---: |
| `protocol` | `v1` | exactly `v1` |
| `registration_attr_max_fields` | 16 | 128 |
| `registration_data_max_fields` | 32 | 128 |
| `registration_max_field_name_bytes` | 64 | 64 |
| `registration_attr_max_field_value_bytes` | 128 | 16,384 |
| `registration_data_max_field_value_bytes` | 128 | 16,384 |
| `registration_max_bytes` | 16,384 | 65,536 |
| `configuration_refresh_ms` | 30,000 | 86,400,000 |

All numeric values use canonical positive decimal text without leading zeros.
Attr and Data counts are independent. `registration_max_bytes` is the sum of
every stored Hash field-name byte and field-value byte, including Meta. The SDK
reserves 16 decimal bytes for Redis-generated `@timestamp` during preflight
validation.

An authorized backend may change the policy after startup. Related fields must
be changed in one multi-field Redis `HSET`, for example:

```text
HSET verdandi:config:Production \
  registration_attr_max_fields 24 registration_data_max_fields 48 \
  registration_attr_max_field_value_bytes 256 \
  registration_data_max_field_value_bytes 512 \
  registration_max_field_name_bytes 64 registration_max_bytes 32768 \
  configuration_refresh_ms 5000
```

The operation is atomic in Redis. Adoption is intentionally eventual:
`configuration_refresh_ms` is itself Redis-owned deployment policy, accepts
1,000 through 86,400,000 milliseconds, and defaults to 30 seconds. Go, Rust, and C++
load one bootstrap snapshot. Register refreshes before validation; after the
first successful publication, one shared task waits that interval with plus or
minus ten percent jitter until the last Registration closes. A successful
refresh controls the next wait without restart, and explicit refresh is always
available. Selector construction does not refresh Registration policy. There is
no configuration Pub/Sub channel or revision in Alpha, so processes with live
Registrations may adopt a valid change at different points inside the configured
interval.

Each Registration Client publishes only a complete valid local snapshot. Missing, malformed,
wrong-protocol, or above-ceiling Redis state produces an asynchronous diagnostic
and leaves the previous snapshot active. Only Client bootstrap fills missing
defaults; a deletion after bootstrap is treated as invalid until a new Client
restores that field or the backend repairs it.

A lower policy applies to later content writes. A rejected Update changes
neither desired state, revision, nor Redis. Renew remains legal because it
changes no content. Selector validates records against immutable protocol
ceilings, so a valid pre-existing record remains discoverable after a policy
reduction.

Ordinary Update performs no configuration read and no complete Registration
read. That Registration's worker validates its process-memory projected record.
Lua does not duplicate SDK-owned request, schema, immutability, or capacity
validation; it retains only Redis-state-dependent atomic conditions and writes.

This working state is not local persistence. Each published Registration owns
one bounded desired-state cache and confirmation status in RAM for equality
checks, projected-limit validation, patch generation, and same-process Redis
recovery.
The SDK writes no Registration content, UUID, replay log, local database, or WAL
to disk. Process termination discards the cache; a restart generates a new UUID
and the old Redis Registration is left to TTL cleanup. Historical persistence
belongs to an external statistics/audit synchronizer, not this SDK.

## 4. Registration Lifecycle

### 4.1 Register

`registration.Client.Registration` validates local options and generates a fresh UUID without
Redis I/O, Client admission, or a renewal worker. The later `Register` readiness
operation performs an immediate Zone-configuration refresh, validates Type,
exact-millisecond TTL, positive safe-integer Version, Attr, Data, and complete
record size, then creates that Registration's Fields mailbox and worker. The
worker publishes one complete revision-1 `register`; publication succeeds before
the handle is visible as registered and automatic renewal begins. The first
successfully published Registration also starts one Client-shared configuration
refresh task; later Registrations share it, and the last Registration joins and
stops it.

Every successfully published Registration owns one single-slot Fields merge
mailbox, one capacity-one wake signal, one long-lived synchronization worker,
one desired/confirmed state, and one renewal timer. The mailbox contains only
the latest pending Version and value for each Data field. A configurable small
admission capacity defaults to 8 and may range from 1 through 256; it bounds
admitted result waiters, not full request objects or full Data copies. Register,
merged Update, Renew, recovery, and Unregister for that UUID execute through
the sole writer. Registrations share no Client-wide mutation queue. A caller
may cancel while waiting for admission; after admission, the worker returns the
actual confirmed, rejected, or ambiguous outcome.

Attr, TTL, and the set of Data field names are fixed for that UUID. Version is
mutable Registration content.
Redis and the raw protocol treat application values as opaque bytes. A complete
same-UUID Register used for reconnect or primary-loss recovery must carry the
SDK's current Version plus the same Attr and TTL. Go, Rust, and C++ callers use strong
Attr/Data structs that directly implement the language's Verdandi field
interface. There is no runtime schema service or SDK code generator; C++ uses
compile-time `VERDANDI_SCHEMA` member descriptors. Raw `Fields` implements the
same interface in all SDKs.

Automatic renewal defaults to one third of TTL with 10 percent jitter. An
explicit renewal interval must be at least 100 milliseconds and no greater than
one third of TTL.

### 4.2 Update

The typed Update API accepts one complete desired Data value. The SDK encodes it
locally, immediately releases any reference to the caller value, and merges the
resulting changed Fields into the Registration mailbox. Every encoded Data name must
already exist in the fixed initial structure; a missing or added name is a
contract error. There is no field-unset operation. An application represents a
typed zero or null with its own encoded bytes. Nested changes replace that
complete top-level field value. `SetVersion` changes Version only and
`UpdateContent` changes Version plus Data under one revision. Attr, TTL, and the
Data field-name set are immutable for the UUID lifetime.

Supplying values already equal to desired state is a local no-op: it performs no
Redis command and does not advance revision. Updates present together in the
mailbox coalesce by last field/Version value, and all calls absorbed by one
write share its result and revision. A Renew carries no Fields: when the same
batch has an effective successful Update it shares that TTL refresh; when the
Update is a no-op or fails, Renew executes independently. Invalid requests fail
before merge. A folded state equal to confirmed state performs no
Redis command. A real content change validates the complete projected state,
advances revision once, and sends only the changed Data fields plus required
Meta.

The Registration worker serializes Update, Renew, recovery, and Close. Every
successful Register, Update, and Renew reply must carry the exact expected
positive revision and a nonzero Redis timestamp. A Redis transport failure after
dispatch is `ambiguous`; a malformed or mismatched post-dispatch reply is
`corrupt`. Both retain the proposed desired revision and send a complete
`register` on the next write/renewal. This is safe whether Redis accepted the
earlier mutation or not. A confirmed `missing` response also triggers complete
reconstruction with the same desired revision.
After Sentinel promotion, a stale primary can also report `transition`; the
worker handles it like missing authoritative state and republishes the complete
desired Registration with the same process UUID and desired revision.

### 4.3 Renew and expiry

Renew changes only Redis `@timestamp` and the Registration/Registry deadlines.
It preserves content revision. Automatic renewal uses the same operation. Redis
key TTL and Hash-field TTL are authoritative for physical expiry; Selectors do
not issue routine `PTTL` or `HPTTL` calls.

A confirmed real Update refreshes the lease and resets the next automatic Renew
deadline exactly like a confirmed Renew. A rejected or exact local no-op Update
does not prove Redis liveness and cannot postpone an already-due Renew. If the
timer and admitted work become ready together, the worker handles the request
first and then renews when the deadline is still due.

### 4.4 Close

Explicit Unregister rejects new requests, drains that Registration's admitted
work, sends terminal `unregister` only while state is confirmed healthy, and
waits for that Registration worker's terminal acknowledgement.
Disconnected or ambiguous state relies on natural TTL instead of reconnecting
merely to delete. Unregister is repeatable and `Close` is its conventional
alias. Client Close cancels and joins all owned
Registrations and Selectors before closing Redis connections.

## 5. Selector Synchronization

Selector maintains one local view for a Zone/Type. Each Selector owns exactly
one persistent listener/state-machine goroutine or asynchronous task. A full
scan or targeted repair may create exactly one temporary synchronization task;
the same slot is reused for both kinds, so steady state is one task and active
synchronization is at most two. The listener remains the only Pub/Sub receiver,
coalesces events, owns the mutable live state, and atomically accepts the
temporary task's candidate result. Close cancels and joins the temporary task
before the persistent listener exits.

RedisClock calibration is part of that same persistent task, not another
goroutine. Full synchronization samples Redis `TIME`, the listener recalibrates
at `ClockRefresh`, and reconnect starts a new connection generation with a new
calibration. Registration does not need RedisClock because each atomic Lua
mutation obtains Redis time and Redis owns lease expiry.

The state machine follows this generation sequence:

1. create a dedicated subscription connection;
2. subscribe to the Registry channel and wait for acknowledgement;
3. calibrate a connection-level RedisClock with `TIME`;
4. traverse membership with bounded `HSCAN` pages and deduplicate UUIDs;
5. pipeline `HMGET @revision @timestamp` for records whose membership revision
   matches active or retained cached content, and `HGETALL` only for new or
   changed content revisions;
6. send `PING <nonce>` on the subscribed connection and process messages in
   receive order through the matching PONG;
7. fetch and fence only UUIDs with an unresolved revision gap; and
8. publish one immutable synchronized view, then apply live events.

`register` is a complete replacement. A contiguous `update` patches Version
and/or Data.
`renew` raises timestamp/deadline without changing content.
`unregister` removes the terminal UUID. Stale revisions are ignored and a gap
uses bounded authoritative repair rather than a full Registry reload.

Snapshot and Find perform no Redis I/O. One indexed deadline per UUID removes
expired records locally even though natural Redis expiry emits no Verdandi
event. A subscription disconnect, hidden reconnect, malformed event, PONG
failure, or non-converging repair marks that generation unsynchronized and
starts a new subscribe/scan/PING generation with bounded reconnect backoff.

The protocol defines no maximum Registration count. Page size, pending-entry
count, pending encoded bytes, sync timeout, local-view bytes, and publication
cadence are bounded local resources. Each subscription reader decodes and
coalesces immediately into at most one logical pending change per UUID.
Contiguous Updates merge by field, Renew raises only the timestamp, Register
replaces pending state completely, Unregister is terminal, and a revision gap
becomes a bounded targeted-repair marker. The notify path itself has capacity
one, so a burst does not create one task or wake object per event. Exceeding the
configured entry or byte budget fails that generation and rebuilds from Redis;
it never drops a change while claiming synchronization.

On natural expiry or a fenced authoritative absence, active content leaves
selection immediately and may enter the retained view until
`@timestamp + 2*@ttl`. A valid same-UUID event or fetched record can reactivate
it. Explicit `unregister` purges it. Retained content remains stored internally
during recovery but no public active, retained, snapshot, or policy view is
readable while the Selector is half-synchronized. Raw and typed `Snapshot`,
`Find`, `FindRetained`, `One`, and `Any` return `CodeUnavailable` until the
generation crosses its fence. Retained content never extends Redis liveness.
Its byte budget is independent from the active view:
64 MiB by default, zero disables it, the accepted maximum is 1 GiB, and the
earliest retained deadline is evicted first under pressure.
The active and retained views are process-memory-only and disappear when the
Selector process exits; they are never restored from local storage. `Find`,
`One`, and `Any` borrow indexed process memory and perform no Redis I/O. A
detached complete `Snapshot` necessarily copies the whole view and is an
explicit heavy O(N) operation; policy callbacks use the deliberately simple
O(N) scan contract in SDK `1.0.0`.

## 6. Catalog API

Catalog is an independent child package/module:

```text
Go:   github.com/eosforge/verdandi/sdk/go/catalog
Rust: verdandi::catalog
C++:  verdandi::catalog
```

A Catalog Client attaches scripts, optional local checkpoint, Publishers, and
Subscribers to the root Redis transport. Registration and Catalog domain
clients may share that transport; each owns and joins only its own workers.

One validated `Path(part, id)` maps to
`verdandi:catalog:<zone>:<part>:<id>`. Its top-level shape is Value, Array, or
Map. The application codec owns all meaning below those raw fields.

Publisher exposes only:

- `Replace(path, kind, completeValue)`: one bounded last-write-wins mutation;
- `Patch(path, {baseRevision, set})`: exact-base Array/Map additions or
  overwrites; and
- `Delete(path)`: complete deletion with a fresh tombstone revision.

Value updates use Replace. Array Patch may overwrite only existing indices.
Map Patch may add or overwrite fields. Patch cannot delete a field, append or
truncate an Array, create holes, change shape, or accept an empty delta.
Replace, rather than segmented publication or an SDK reference object, is the
only full-value operation.

The complete application field-name plus field-value size defaults to 512 KiB
and may be configured up to the 4 MiB protocol ceiling. Revisions are Redis-owned canonical integers in
`1..=2^53-1`; zero means no revision. Replace and Delete are atomic
last-write-wins operations, while Patch must exactly match the current revision.
Catalog uses no external Path lock.

Go publication:

```go
transport, err := verdandi.Open(ctx, verdandi.Config{
	Standalone: &verdandi.Standalone{Address: "127.0.0.1:6379"},
})
if err != nil {
    return err
}
defer transport.Close()

client, err := catalog.Open(ctx, transport, catalog.Config{
	Zone:           "Prod",
	LocalStorePath: "catalog.db", // optional
})
if err != nil {
    return err
}
defer client.Close(context.Background())

publisher, err := client.Publisher()
if err != nil {
    return err
}
path, err := catalog.NewPath("routing", "public")
if err != nil {
    return err
}
created, err := publisher.Replace(ctx, path, catalog.Map, verdandi.Fields{
    "primary": []byte("east"),
    "weight":  []byte("10"),
})
if err != nil {
    return err
}
_, err = publisher.Patch(ctx, path, catalog.Patch{
    BaseRevision: created.Revision,
    Set:          verdandi.Fields{"primary": []byte("west")},
})
```

Go subscription and per-call generic loading:

```go
subscriber, err := client.Subscriber(ctx, catalog.Subscription{
    Parts: []string{"routing"},
})
if err != nil {
    return err
}
defer subscriber.Close(context.Background())

entry := subscriber.Find(path) // stable identity; no I/O
snapshot, err := entry.Load[Routing]() // no Redis or disk I/O
if err != nil {
    return err
}
if snapshot.Synchronized && snapshot.Value != nil {
    use(*snapshot.Value)
}
```

Rust exposes the same ownership and semantics:

```rust
use verdandi::{Client as RootClient, Config as RootConfig};
use verdandi::catalog::{
    Client, Config, Kind, Patch, Path, Publisher, Subscriber, Subscription,
};

let root = RootClient::open(RootConfig::new(
    "redis://127.0.0.1:6379",
))
.await?;
let client = Client::open(&root, Config::new("Prod")).await?;
let publisher = Publisher::new(&client)?;
let path = Path::new("routing", "public")?;
let created = publisher.replace(&path, Kind::Map, &routing).await?;
publisher
    .patch(
        &path,
        Patch {
            base_revision: created.revision,
            set: changed_fields,
        },
    )
    .await?;

let subscriber = Subscriber::new(
    &client,
    Subscription {
        parts: vec!["routing".to_owned()],
        ..Default::default()
    },
)
.await?;
let entry = subscriber.find(&path).expect("covered path");
let snapshot = entry.load::<Routing>()?;
```

One Subscriber owns one dedicated persistent Pub/Sub connection/listener and at
most one temporary synchronization/repair task for any combination of exact
Paths, Parts, or the complete Zone. It normalizes overlapping coverage and
routes each actual channel locally. The temporary slot coalesces pending work
and exits when idle. The
notification contains the complete accepted Replace, Patch, or Delete
operation. Patch applies only when its `base_revision` matches the Entry;
otherwise only that Path enters authoritative repair.

Initial/reconnect synchronization subscribes before reading, aligns against the
Zone live/deleted ZSETs and per-path Read script, then uses an ordered
subscribed-connection PING/PONG fence. Per-field revision scores allow exact
newer-field recovery; a later Replace forces a complete Hash read. Falling
below the tombstone floor forces a complete index scan. Redis remains
authoritative because Pub/Sub delivery is at-most-once.

Subscribers hold every covered complete raw value in memory. Optional bbolt
(Go), redb (Rust), or SQLite (C++) persistence is only a restart accelerator. Checkpoint
entries and cursors advance monotonically, including when multiple Subscribers
share one scope. The first persistence error disables later writes for that
Client generation and is reported asynchronously; in-memory synchronization
continues.

A stable Entry reports Present, Absent, Deleted, Synchronizing, Unavailable, or
Closed. During Synchronizing/Unavailable it may retain a complete last-known
value marked `synchronized=false`. Delete never invalidates the Entry object;
a later Replace makes the same Entry Present again.

The complete API, Redis key/field layout, Lua ABI, recovery state machine, and
current limits are normative in [`catalog/api.md`](catalog/api.md). Performance,
endurance, scores, and remaining policy decisions are in
[`catalog/optimization.md`](catalog/optimization.md).

## 7. Go API

The Go module is `github.com/eosforge/verdandi/sdk/go`, targets Go 1.27, and
uses one concrete `*redis.Client`. `Client.Redis()` exposes that exact pointer
as a borrowed Go-native capability; `Done()` and `Timeout()` expose
permanent root shutdown and the normalized immutable timeout. Standalone and
Sentinel configuration still use Verdandi-owned types.

Go 1.27 is an intentional source requirement: generic methods place
`Registration[A, D]`, `Selector[A, D]`, and `Entry.Load[T]` on their owning
objects instead of adding package-level generic constructors. These methods
belong to concrete SDK types; Go interfaces still cannot declare generic
methods.

Registration and Selector live in
`github.com/eosforge/verdandi/sdk/go/registration`; the root package retains
the shared Client, typed Key/Hash commands, Fields, codecs, configuration, and
errors. The complete ordinary-command surface is in
[`sdk/go/client.md`](sdk/go/client.md).

The ordinary application boundary is strong Attr/Data structs that implement
`Encoder` and `Decoder` directly. Raw `Fields` implements those same
interfaces and can be used without a second API surface:

```go
ctx := context.Background()
transport, err := verdandi.Open(ctx, verdandi.Config{
	Standalone: &verdandi.Standalone{
        Address:  "127.0.0.1:6379",
        Username: "node",
        Password: secret,
    },
})
if err != nil {
    return err
}
defer transport.Close()

client, err := registration.Open(ctx, transport, registration.Config{
	Zone: "Production",
})
if err != nil {
    return err
}
defer client.Close(context.Background())

selector, err := client.Selector[verdandi.Fields, verdandi.Fields](
    ctx,
    registration.SelectorOptions{Type: "Proxy"},
)
if err != nil {
    return err
}
defer selector.Close(context.Background())

handle, err := client.Registration[verdandi.Fields, verdandi.Fields](registration.RegistrationOptions{
    Type:    "Proxy",
    TTL:     15 * time.Second,
    Version: 1,
})
if err != nil {
    return err
}
if err := handle.Register(ctx,
    verdandi.Fields{"region": []byte("cn-east")},
    verdandi.Fields{
        "address": []byte("10.0.0.8:8080"),
        "load":    []byte("0"),
    },
); err != nil {
    return err
}
defer handle.Unregister(context.Background())

if err := handle.Update(ctx, verdandi.Fields{
    "address": []byte("10.0.0.8:8080"),
    "load":    []byte("12"),
}); err != nil {
    return err
}

snapshot, err := selector.Snapshot(ctx)
if err != nil {
    return err // includes CodeUnavailable while synchronization is incomplete
}
record, found, err := selector.Find(ctx, handle.UUID())
if err != nil {
    return err
}
_, _ = snapshot, record
_ = found
```

### 7.1 Application-owned strong Attr/Data

Declare one struct for immutable Attr and one for mutable Data. The value type
encodes every field and its pointer type decodes a complete field map:

```go
type ProxyAttr struct {
    Region string
}

type ProxyData struct {
    Address string
    Power   int64
}

func (value ProxyAttr) Encode() (verdandi.Fields, error) {
    return verdandi.Fields{"region": []byte(value.Region)}, nil
}

func (value *ProxyAttr) Decode(src verdandi.Fields) error {
    value.Region = string(src["region"])
    return nil
}

func (value ProxyData) Encode() (verdandi.Fields, error) {
    return verdandi.Fields{
        "address": []byte(value.Address),
        "power":   strconv.AppendInt(nil, value.Power, 10),
    }, nil
}

func (value *ProxyData) Decode(src verdandi.Fields) error {
    power, err := strconv.ParseInt(string(src["power"]), 10, 64)
    if err != nil {
        return err
    }
    *value = ProxyData{Address: string(src["address"]), Power: power}
    return nil
}

handle, err := client.Registration[ProxyAttr, ProxyData](
    registration.RegistrationOptions{Type: "Proxy", TTL: 15 * time.Second, Version: 1})
if err != nil {
    return err
}
if err := handle.Register(ctx,
    ProxyAttr{Region: "cn-east"},
    ProxyData{Address: "10.0.0.8:8080"},
); err != nil {
    return err
}
defer handle.Unregister(context.Background())

next := ProxyData{Address: "10.0.0.8:8080", Power: 12}
if err := handle.Update(ctx, next); err != nil {
    return err
}
```

The SDK does not generate these methods and does not accept a Schema or codec
argument. Application code may use JSON, MessagePack, protobuf, fixed-width
binary values, or any other stable per-field representation. The complete
contract, `One`/`Any` selection transactions, local prediction reconciliation,
and raw `Fields` examples are in
[`registration/api.md`](registration/api.md).

### 7.2 Go lifecycle and local configuration

`registration.Client.Errors`, `Registration.Errors`, `Selector.Errors`, and
`catalog.Subscriber.Errors` return bounded asynchronous diagnostics.
`RegistrationLimits` returns the current last-valid policy snapshot, and
`RefreshConfiguration` performs an immediate Redis refresh. Every blocking
operation accepts a non-nil Context. A write cancelled before admission returns
immediately; once admitted, that Registration's worker returns its confirmed or
ambiguous outcome.

The Go root `verdandi.Config` contains only Standalone/Sentinel connectivity,
database/TLS/authentication, ordinary and connect timeouts, the shared
connection pool, and connection-level recovery backoff. It never retries a
dispatched Redis command. Required Zone identity lives independently in
`registration.Config` and `catalog.Config`, so one ACL-authorized transport can
serve multiple Zones. Root `Close()` is immediate and has no Context: it signals
transport loss and closes go-redis without waiting for domain workers. Close
Registration or Catalog clients first when joined cleanup must finish before
returning. Registration `Open` performs the Redis 8 `HELLO` check; root `Open`
performs only `PING` and does not require `INFO` permission.

The root owns the pointer returned by `Redis()`; callers must not close or
reconfigure it. Raw go-redis commands are intentionally available for advanced
Go integration and are authorized by the configured ACL, but they bypass
Verdandi validation, resource limits, multi-key invariants, and stable error
mapping. Registration and Catalog use the same public capabilities directly,
so no internal access bridge or shutdown-only watcher exists.

Go configuration uses zero values for documented defaults and pointer fields
where explicit zero has meaning. Root command/connect timeouts default to 2/5
seconds; the shared pool defaults to 1..4 connections with a 10-second excess-
idle timeout. Connection recovery starts at 100 ms, grows by 2 to 5 seconds,
and uses up to 10 percent subtractive jitter. The driver performs no automatic
business-command replay.

For Registration/Selector, `SelectorPublishInterval=nil` and
`ClockUncertainty=nil` select the 10 ms and 1 ms defaults; pointers to zero mean
immediate view publication and no additional artificial clock margin. They do
not disable any command or synchronization timeout.

Each Registration's Fields mailbox has one pending field map and capacity-one
wake signal. `BufferCapacity` defaults to 8 (range 1..256) and bounds admitted
result waiters; it is not a request-array size. Selector page size defaults to
256, pending UUID capacity to 4,096, coalesced pending bytes to 64 MiB, active
view to 256 MiB, retained view to 64 MiB, sync timeout to 30 seconds, and
RedisClock refresh to 30 seconds. `SelectorRetainedBytes=nil` selects 64 MiB
while a pointer to zero disables retention. Configuration polling cannot be
disabled locally because its interval is shared Redis policy; the poller exists
only while at least one Registration is published.

Catalog synchronization defaults to 30 seconds, 256-entry scan pages, 32
in-flight reads, and 256 pending repair Paths. A complete encoded record
defaults to 512 KiB and is configurable up to 4 MiB. `MaxViewBytes=0`
adds no aggregate encoded-byte limit. `catalog.Config.LocalStorePath` is empty
by default and enables a disposable bbolt checkpoint only when explicitly set.
Publisher is a lightweight stateless view of the Catalog Client and has no
independent Close. Patch's HMGET projection is deliberately unlocked: the Lua
commit repeats the exact base-revision check, so a concurrent same-Path change
returns `stale` before any write. Every Redis request remains bounded by the
root command timeout; a lost mutation reply remains `ambiguous`.

The complete v1 JSON shape is
[`configuration.schema.json`](configuration.schema.json), with all fields in
[`configuration.example.json`](configuration.example.json). Package
`configuration` in Go and module `configuration` in Rust strictly load the same
shape and convert it to the native types described here. The default/range
table is [`configuration.md`](configuration.md). Native structure methods still
own the final checks; there is no configuration code generator or parallel
constants API.

`redis.tls` is a nested object with `enabled`, `system_roots`, `server_name`,
`ca_file`, `cert_file`, and `key_file`. TLS always verifies the peer and uses
TLS 1.2 or newer. `server_name` is deliberately Standalone-only because Fred
cannot propagate one fixed override to a new primary discovered by Sentinel;
Sentinel deployments should use certificate names matching their advertised
hosts. PEM paths are structurally checked during JSON loading and read, with a
1 MiB per-file bound, only while constructing/opening the native transport.

For Sentinel, configure the root transport `Config` with `Sentinel` instead of
`Standalone`; Zone remains on each domain Config:

```go
transport, err := verdandi.Open(ctx, verdandi.Config{
	Sentinel: &verdandi.Sentinel{
        Addresses:       []string{"10.0.0.11:26379", "10.0.0.12:26379", "10.0.0.13:26379"},
        MasterName:      "verdandi-primary",
        Username:        "node",
        Password:        redisSecret,
        SentinelUsername: "sentinel-client",
        SentinelPassword: sentinelSecret,
    },
})
```

Redis and Sentinel credentials are independent. A primary change invalidates
the old Selector generation; the SDK resolves again, resubscribes before its
snapshot, and publishes a new synchronized generation only after the ordered
PING/PONG fence.

## 8. Rust API

The Rust crate is `verdandi`, uses edition 2024, declares Rust 1.85 as its
minimum version, and uses `fred` privately. Its root typed Key/Hash traits,
derive macro, and async commands are documented in
[`sdk/rust/client.md`](sdk/rust/client.md).

```rust
use verdandi::{Client as RootClient, Config as RootConfig};
use verdandi::registration::{
    Client as RegistrationClient, Config as RegistrationConfig, Registration,
    RegistrationOptions, Selector, SelectorOptions,
};

let root = RootClient::open(RootConfig::new(
    "redis://node:secret@127.0.0.1:6379/0",
))
.await?;
let client = RegistrationClient::open(
    &root,
    RegistrationConfig::new("Production"),
)
.await?;

let selector = Selector::<ProxyAttr, ProxyData>::new(
    &client,
    SelectorOptions {
        type_name: "Proxy".to_owned(),
    },
)
.await?;

let registration = Registration::<ProxyAttr, ProxyData>::new(
    &client,
    RegistrationOptions {
        type_name: "Proxy".to_owned(),
        ttl: Duration::from_secs(15),
        renew_interval: None,
        version: 1,
    },
)?;

start_listening().await?;
registration.register(&attr, &data).await?;
registration.update(&next_data).await?;

let selected = selector
    .one(Duration::from_millis(10), |candidates| {
        let choice = candidates
            .get(0)
            .ok_or_else(|| Error::field(Code::Missing, "candidate"))?
            .choice();
        candidates.mutate(choice, |data| {
            data.power += 1;
            Ok(())
        })?;
        Ok(Some(choice))
    })
    .await?;

registration.unregister().await?;
selector.close().await?;
client.close().await?;
root.close().await?;
```

`ProxyAttr` and `ProxyData` implement `FieldValue`; Verdandi does not generate
their logic. Callback reads use immutable borrowed `CandidateRef` values and an
opaque `Choice`, so scanning does not clone the whole view. `mutate` stages one
typed local prediction and a successful operation returns detached owned
Candidates. `snapshot`, `find`, and `find_retained` expose the active and
separate retained views and return `Code::Unavailable` while synchronization is
incomplete. Full examples and reconciliation semantics are in
[`registration/api.md`](registration/api.md).

`subscribe_errors` returns a bounded Tokio broadcast receiver.
`registration_limits` returns the current last-valid snapshot and
`refresh_configuration` performs an immediate refresh.
`RootConfig::new(endpoint)` supplies two-second command and five-second connect
timeouts, a dynamic 1..4 Fred connection pool, a 10-second excess-idle timeout,
and explicit exponential reconnect delays from 100 milliseconds through 5
seconds. Fred is configured for one total command send attempt; reconnecting a
transport does not replay an ambiguous mutation.
`RegistrationConfig::new(zone)` supplies the required Zone, an eight-waiter
Fields-mailbox admission bound, and Selector defaults; `selector_retained_bytes =
None` selects 64 MiB and
`Some(0)` disables retention. Configuration refresh timing is not a local
Registration Config field: the running Client follows
`configuration_refresh_ms` from its last valid Redis snapshot while at least
one Registration is published. `CatalogConfig::new(zone)` supplies its required
Zone, synchronization/read/repair bounds, `max_view_bytes`, a 512-KiB default
`max_record_bytes` configurable through 4 MiB, lock/recovery timing, and
`local_store_path`; the last is `None` by default and enables redb only when
set. Numeric defaults and ranges match Go's reviewed contract and are enforced
by the owning Rust structure methods.

Rust selects Sentinel through a `redis-sentinel://` endpoint. The primary ACL
identity is carried by the URL authority; `sentinelServiceName`,
`sentinelUsername`, `sentinelPassword`, and repeated `node` query parameters
configure the Sentinel service and endpoints. The Redis driver remains private
to the crate, so this URL is bootstrap configuration rather than a public
driver type.

SDK worker ownership is cancellation-safe: dropping an awaiting request does
not cancel an already admitted mutation. Explicit Close is repeatable and waits
for the shared terminal result. Rust root `close().await` closes Fred without
joining domain work. Registration and Catalog retain a clone of the same root
`Client`, so dropping one caller variable does not invalidate live domains;
dropping the final root-or-domain clone is best-effort because `Drop` cannot
await.

## 9. C++23 API

The C++ target is `verdandi::verdandi`, requires C++23, and keeps Boost.Redis,
Boost.Asio, OpenSSL, yyjson, and SQLite types private. One compiled library owns
transport and protocol state machines; templates are confined to compile-time
Schema expansion, typed/raw Fields conversion, Catalog loading, and Selector
policy callbacks.

```cpp
#include <verdandi/client.hpp>
#include <verdandi/registration/registration.hpp>
#include <verdandi/registration/selector.hpp>

struct ProxyAttr {
    std::string region;
};
struct ProxyData {
    std::int64_t power{};
    bool ready{};
};

VERDANDI_SCHEMA(ProxyAttr,
                VERDANDI_FIELD(ProxyAttr, region));
VERDANDI_SCHEMA(ProxyData,
                VERDANDI_FIELD(ProxyData, power),
                VERDANDI_FIELD(ProxyData, ready));

verdandi::redis_configuration redis;
redis.addresses = {"127.0.0.1:6379"};
auto root = verdandi::client::open(redis);

verdandi::registration_configuration registration_config;
registration_config.zone = "Production";
auto domain = verdandi::registration::client::open(*root, registration_config);

auto registration =
    verdandi::registration::registration<ProxyAttr, ProxyData>::create(
        *domain,
        {.type = "Proxy",
         .ttl = std::chrono::seconds{15},
         .version = 1});

start_listening();
registration->publish(ProxyAttr{"cn-east"}, ProxyData{0, true});
registration->update(ProxyData{1, true});

auto selector =
    verdandi::registration::selector<ProxyAttr, ProxyData>::create(
        *domain,
        {.type = "Proxy"});
```

`VERDANDI_SCHEMA` uses `consteval` descriptors, concepts, and fold expressions;
there is no runtime reflection or SDK source generator. Standard scalar
members use `field_codec<T>`, and applications may specialize it for their own
scalar types. `verdandi::fields` satisfies the same APIs for raw binary use.

Registration construction performs no Redis write. `publish` is the readiness
boundary. Each published object owns one coalescing Fields mailbox, one worker,
one renewal timer, and one desired/confirmed state. Selector `one` and `any`
execute injected policies synchronously over borrowed immutable candidates,
stage local Data predictions through `mutate`, and return detached values.

Each Selector and Catalog Subscriber owns one persistent listener task. Initial
synchronization and later authoritative repair share at most one temporary task
slot, so steady state is one task and active synchronization is at most two.
Publisher is task-free. C++ Catalog uses the same Replace/Patch/Delete and
stable Entry contract, with an optional monotonic transactional SQLite restart
checkpoint.

Native structs expose `check()` and strict JSON is loaded through
`verdandi::configuration::from_json` or `load_json`. Standalone, ACLs,
Standalone TLS, and plain Sentinel are implemented. Cluster and Sentinel+TLS
are rejected; the latter cannot currently retain secure hostname verification
for dynamically discovered Boost.Redis targets.

The full build instructions, API examples, ownership rules, dependency policy,
and current limitations are in [`sdk/cpp/README.md`](sdk/cpp/README.md).

### 9.1 C ABI for C and lower-standard C++

The `verdandi::c` CMake target exposes C ABI v1 from the same compiled runtime.
It supports C11 and direct C++11/14/17 callers without propagating the native
target's C++23 language requirement. A source build still needs a compiler that
can build the internal core as C++23; only the consuming target remains in its
lower language mode. A genuinely old compiler consumes a prebuilt compatible
shared runtime instead.

The boundary uses opaque handles, explicit release functions, borrowed
string/byte views, owned Blob/Fields/candidate/snapshot results, stable string
errors, and synchronous Selector callbacks. Configuration is the canonical
strict v1 JSON document rather than a fourth configuration model. Registration
Attr/Data and Catalog values are flattened binary Fields and are copied before
a write call returns. No STL type, exception, template, application struct, or
allocator ownership crosses the ABI.

The umbrella header is `verdandi/c/verdandi.h`. Static and shared source builds,
C11, C++11, C++14, C++17, Redis Registration/Selector/Catalog behavior, and
sanitizer paths have focused tests. Full examples and lifetime/evolution rules
are in [`sdk/cpp/C_ABI.md`](sdk/cpp/C_ABI.md).

### 9.2 C++11/14/17 convenience facade

The header-only `verdandi::legacy` target links C ABI v1 and provides RAII
handles, `result<T>`, `optional<T>`, `std::chrono` durations, raw Fields,
C++11 schema descriptors, typed Registration/Selector/Catalog APIs, and
exception-safe Selector callback trampolines. It is compiled into the
application and owns no Redis, synchronization, retry, recovery, or checkpoint
state. The stable binary boundary remains the C ABI.

Consumers include `<verdandi/legacy.hpp>` and link `verdandi::legacy`. The core
still requires a C++23-capable source-build toolchain; an old compiler links a
prebuilt runtime. Typed values are decoded from C Fields at the facade boundary,
so native C++23 remains preferable for the lowest Selector hot-path overhead.
Static/shared C++11, C++14, and C++17 compile tests plus a typed C++11 Redis
integration and sanitizer path are implemented. The complete facade contract
and examples are in [`sdk/cpp/LEGACY.md`](sdk/cpp/LEGACY.md).

### 9.3 C# managed facade

`sdk/csharp` builds one managed assembly for .NET 8 and .NET 10. It forwards
through C ABI v1 using source-generated P/Invoke and owns every opaque result
through a dedicated SafeHandle. Child wrappers retain an internal parent
SafeHandle reference so early public Dispose cannot release native ancestors
before their children. No pointer, borrowed C view, manual release function, or
native callback context is public.

Application records implement `IFieldValue<TSelf>` directly. The contract has
an instance full-value encoder and a static full-value decoder; raw immutable
`Fields` implements it too. There is no reflection, JSON application record,
schema service, code generator, or retained caller struct. Fields use one
continuous owned payload and one temporary native view array per write call.

`RegistrationClient.NewRegistration<TAttr,TData>` is local and the later
`Register` method is the readiness boundary. Selector `One` and `Any` execute
on the caller thread. Their borrowed `Candidates` is a `ref struct`, and each
opaque Choice includes one process-wide callback transaction identity. Invalid,
retained, foreign, or duplicate Choice values fail with `contract` and roll
back local mutation. Catalog exposes managed Publisher, Subscriber, stable
Entry, and per-load generic snapshots.

The C ABI is synchronous, so C# exposes truthful synchronous APIs instead of
moving blocking calls to `Task.Run`. An actual async API requires a future
native completion/cancellation contract or a separately approved pure-C#
backend. The complete API, loader order, examples, verification, and remaining
release gates are in [`sdk/csharp/README.md`](sdk/csharp/README.md).

## 10. Error and Trust Boundary

All SDKs use these stable string categories:

```text
invalid protocol contract target capacity missing stale transition immutable
corrupt unavailable deadline ambiguous closed
```

Field and authoritative revision context are attached when available. Backend
text is diagnostic only and is never used for protocol decisions. Credentials
are not included in public Debug output.

Redis authentication and ACLs are the data-security boundary. A conformant
runtime credential needs the commands and key/channel patterns required by its
roles; bootstrap additionally needs `HSETNX` on its Zone configuration key,
while an administrative backend needs `HSET` to change policy. Deliberately
issuing raw Redis writes with an authorized credential can bypass SDK checks and
is outside the supported consistency guarantee.

## 11. Verification

Reproducible commands and measured results are in
[`test-results.md`](test-results.md). Shared Lua and cross-language entry points
are documented in [`testkit/README.md`](testkit/README.md). Registration has
four operation-specific scripts. For each Registration operation, the
canonical, Go, Rust, and C++ copies must remain byte-identical:

```text
lua/registration/<kind>.lua
sdk/go/registration/internal/protocol/<kind>.lua
sdk/rust/src/registration/<kind>.lua
sdk/cpp/src/protocol/registration/<kind>.lua
```

Each linked Registration implementation preloads its four SHAs during Client
bootstrap, chooses exactly one SHA for each lifecycle mutation, and reloads
only that script after `NOSCRIPT`. Loading scripts does not turn one mutation
into multiple Redis calls. Catalog owns its independent script inventory and
verification contract in [`catalog/api.md`](catalog/api.md).
