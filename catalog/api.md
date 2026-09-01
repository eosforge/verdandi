# Catalog v1 API and Redis protocol

Status: implemented latest contract. This document supersedes the former
Hash/Stream Mirror and `Catalog<T>` designs.

## 1. Contract

Catalog stores independent persistent raw values at:

```text
verdandi:catalog:<zone>:<part>:<id>
```

The application owns every external type and codec. Redis and Lua know only
opaque field names and bytes. One path has exactly one shape:

- Value: one field named `value`;
- Array: contiguous canonical decimal fields `0..n-1`; or
- Map: application-named fields.

One Publisher can mutate paths with different structures. One Subscriber keeps
complete raw values in memory, and each `Load<T>` chooses its own target type.
Deleting a path changes its stable Entry to Deleted; a later Replace changes the
same Entry back to Present.

Revisions are Redis-primary execution order in `1..=9_007_199_254_740_991`.
Zero means no revision. Every control integer is transported as canonical
decimal text, while sorted-set scores remain exact IEEE-754 integers.

## 2. Mutation semantics

Publisher exposes only Replace, Patch, and Delete.

| Operation | Allowed input | Concurrency rule |
| --- | --- | --- |
| Replace Value | exactly `value` | last-write-wins |
| Replace Array | complete contiguous indices | last-write-wins |
| Replace Map | complete field map | last-write-wins |
| Patch Array | overwrite existing indices only | exact `base_revision` |
| Patch Map | add or overwrite fields only | exact `base_revision` |
| Delete | complete path | last-write-wins, always creates a new tombstone |

Value changes, Array append/truncate/index deletion, Map field deletion, and
shape changes require Replace. Patch never deletes a field and never accepts an
empty delta. Replace is one bounded atomic operation; segmented Replace and
large-value references do not exist.

There is no external Path lock. Replace and Delete are one atomic Lua call and
use Redis-primary execution order for last-write-wins. Patch first uses HMGET
to project the complete encoded size from its exact base, then invokes one Lua
script that rechecks the same `base_revision`, affected fields, projected size,
and shape before its first write. If another same-Path mutation wins between
projection and commit, Patch returns Stale and changes nothing. Two writers
racing with the same base therefore produce exactly one successful Patch.

A timeout or transport failure after any mutation is sent is reported as
Ambiguous because Redis may already have committed it. Callers must align the
authoritative revision before another Patch. The ordinary root command timeout
bounds each HMGET or script call; no Catalog lock TTL, acquisition deadline, or
retry-loop configuration exists.

`MaxRecordBytes/max_record_bytes` defaults to 524,288 bytes and may be
configured through 4,194,304 bytes. It counts all application field-name bytes
plus value bytes. `MaxViewBytes/max_view_bytes` independently bounds the total
encoded application bytes retained by one Subscriber; zero means no additional
aggregate limit. These counters do not attempt to predict language-runtime allocator
overhead. A fixed 65,536-field defensive ceiling still protects Lua and
notification decoders and is not configurable.

## 3. Go API

Package:

```go
import (
    verdandi "github.com/eosforge/verdandi/sdk/go"
    "github.com/eosforge/verdandi/sdk/go/catalog"
)
```

The root Client owns Redis connectivity. A Catalog Client attaches only
Catalog scripts, policy, workers, and optional persistence to that shared
transport:

```go
type Config struct {
    Zone                      string
    SyncTimeout               time.Duration
    ScanPageSize              int
    MaxInflightReads          int
    EventBufferCapacity       int
    ErrorBufferCapacity       int
    MaxViewBytes              int64
    MaxRecordBytes            int
    RecoveryInitialDelay      time.Duration
    RecoveryMaxDelay          time.Duration
    RecoveryMultiplier        int
    RecoveryJitterPercent     *int
    LocalStorePath            string
}

func (Config) Check() error
func Open(context.Context, *verdandi.Client, Config) (*Client, error)
func (*Client) Close(context.Context) error
```

All numeric defaults, ranges, and relationship checks are owned by Catalog's
native `Config` methods and listed in [`../configuration.md`](../configuration.md).
No parallel exported constants API is provided. Catalog configuration remains
separate from root Redis and Registration/Selector configuration. Go zero values
select documented defaults; the recovery-jitter pointer distinguishes its nil
default from an explicit zero, while `MaxViewBytes=0` directly disables the
additional aggregate view limit.
The shared Go `configuration` package strictly loads the canonical v1 JSON and
converts its optional Catalog object into this native type.

Closing a Catalog Client cancels and joins its own operations and persistence
without closing the shared transport. Root `Close()` broadcasts transport loss
and closes go-redis immediately; it does not wait for Catalog. Applications
should close Subscriber, Catalog Client, and then root Client for deterministic
cleanup. Publisher is a stateless view and has no Close. If the root closes first, Catalog observes the signal
and begins its own joined shutdown.

Zone belongs to Catalog configuration, not the root connection. One root
transport may therefore serve multiple Catalog or Registration Zones when its
ACL credentials authorize them.

The Go Catalog package consumes the root's borrowed `Redis()`, permanent
`Done()`, and normalized `Timeout()` directly. It creates no second
pool or internal access bridge, and it never closes the borrowed driver. Raw
application use of `Redis()` remains governed by ACLs and outside Catalog's
revision and multi-key guarantees.

Identity and operations:

```go
type Path struct { /* opaque */ }
func NewPath(part, id string) (Path, error)
func (Path) Part() string
func (Path) ID() string

type Kind uint8
const (
    Value Kind = iota + 1
    Array
    Map
)

type Result struct { Revision uint64 }
type Patch struct {
    BaseRevision uint64
    Set          verdandi.Fields
}

func (*Client) Publisher() (*Publisher, error)
func (*Publisher) Replace(
    context.Context, Path, Kind, verdandi.Encoder,
) (Result, error)
func (*Publisher) Patch(context.Context, Path, Patch) (Result, error)
func (*Publisher) Delete(context.Context, Path) (Result, error)
```

Subscription and typed loading:

```go
type Subscription struct {
    Zone  bool
    Parts []string
    Paths []Path
}

func (*Client) Subscriber(context.Context, Subscription) (*Subscriber, error)
func (*Subscriber) Find(Path) *Entry
func (*Subscriber) Errors() <-chan error
func (*Subscriber) Close(context.Context) error

type Status uint8
const (
    StatusSynchronizing Status = iota + 1
    StatusPresent
    StatusAbsent
    StatusDeleted
    StatusUnavailable
    StatusClosed
)

func (*Entry) Path() Path
func (*Entry) Status() Status
func (*Entry) Revision() uint64
func (*Entry) Synchronized() bool

type Snapshot[T any] struct {
    Revision     uint64
    Status       Status
    Synchronized bool
    Value        *T
}

func (*Entry) Load[
    T any,
    P interface {
        *T
        verdandi.Decoder
    },
]() (Snapshot[T], error)
```

Callers name only `T`; Go infers `P` as `*T`. `Find` and `Load` perform no
Redis or disk I/O. An Entry is stable for the lifetime of its Subscriber.
During Synchronizing, Unavailable, or Closed, Load
may return the last complete value with `Synchronized=false`; after Close,
`Find` returns nil but already-held Entries remain readable in that terminal
state.

## 4. Rust API

Catalog is a child module and is not re-exported at crate root:

```rust
use verdandi::{Client as RedisClient, Config as RedisConfig, FieldValue, Fields};
use verdandi::catalog::{
    Client, Config, Entry, Kind, MutationResult, Patch, Path, Publisher,
    Snapshot, Status, Subscriber, Subscription,
};
```

Connectivity:

```rust
pub struct Config {
    pub zone: String,
    pub sync_timeout: Duration,
    pub scan_page_size: usize,
    pub max_inflight_reads: usize,
    pub event_buffer_capacity: usize,
    pub error_buffer_capacity: usize,
    pub max_view_bytes: u64,
    pub max_record_bytes: usize,
    pub recovery_initial_delay: Duration,
    pub recovery_max_delay: Duration,
    pub recovery_multiplier: u32,
    pub recovery_jitter_percent: u8,
    pub local_store_path: Option<PathBuf>,
}

impl Config {
    pub fn new(zone: impl Into<String>) -> Self;
    pub fn check(&self) -> Result<()>;
}

impl Client {
    pub async fn open(base: &RedisClient, config: Config) -> Result<Self>;
    pub async fn close(&self) -> Result<()>;
}
```

Rust `Config::new(zone)` materializes the same reviewed numeric defaults into
native public fields, and `Config` validates its own ranges and relationships.
The shared `verdandi::configuration::Config` strictly loads the canonical v1
JSON and converts its optional Catalog object into this native type.

The root Redis Client is opened separately with
`RedisClient::open(RedisConfig::new(endpoint))` and may be shared by Catalog
and Registration Clients configured for independent Zones. Catalog close joins
only Catalog-owned work. Root `close().await` broadcasts loss and awaits Fred
shutdown without waiting for Catalog.

Mutation:

```rust
pub enum Kind { Value, Array, Map }
pub struct MutationResult { pub revision: u64 }
pub struct Patch {
    pub base_revision: u64,
    pub set: Fields,
}

impl Publisher {
    pub fn new(client: &Client) -> Result<Self>;
    pub async fn replace<T: FieldValue>(
        &self, path: &Path, kind: Kind, value: &T,
    ) -> Result<MutationResult>;
    pub async fn patch(
        &self, path: &Path, patch: Patch,
    ) -> Result<MutationResult>;
    pub async fn delete(&self, path: &Path) -> Result<MutationResult>;
}
```

Subscription and loading:

```rust
pub struct Subscription {
    pub zone: bool,
    pub parts: Vec<String>,
    pub paths: Vec<Path>,
}

impl Subscriber {
    pub async fn new(
        client: &Client, subscription: Subscription,
    ) -> Result<Self>;
    pub fn find(&self, path: &Path) -> Option<Entry>;
    pub fn subscribe_errors(&self) -> broadcast::Receiver<Error>;
    pub async fn close(&self) -> Result<()>;
}

impl Entry {
    pub fn path(&self) -> &Path;
    pub fn status(&self) -> Status;
    pub fn revision(&self) -> u64;
    pub fn synchronized(&self) -> bool;
    pub fn load<T: FieldValue>(&self) -> Result<Snapshot<T>>;
}

pub struct Snapshot<T> {
    pub revision: u64,
    pub status: Status,
    pub synchronized: bool,
    pub value: Option<T>,
}
```

Rust uses static `FieldValue` dispatch and returns an owned typed projection.
It does not require all subscribed paths to share one `T`.

## 5. C++23 API

C++ uses namespace `verdandi::catalog` and the same root transport as
Registration. Its public headers expose only C++ standard-library and Verdandi
types; Boost.Redis, yyjson, OpenSSL, and SQLite remain compiled implementation
details.

```cpp
#include <verdandi/catalog/catalog.hpp>

verdandi::catalog_configuration config;
config.zone = "Production";
config.local_store_path = "catalog.sqlite3"; // Empty disables persistence.

auto client = verdandi::catalog::client::open(transport, config);
auto publisher = verdandi::catalog::publisher::create(*client);
auto target = verdandi::catalog::path::create("routing", "primary");

auto replaced = publisher->replace(
    *target,
    verdandi::catalog::kind::map,
    Routing{"west", 1});

verdandi::catalog::patch change;
change.base_revision = replaced->revision;
change.set.emplace("weight", encoded_weight);
auto patched = publisher->apply(*target, std::move(change));
auto erased = publisher->erase(*target);
```

`path::create` validates Part and ID before I/O. `replace<Value>` accepts a
compile-time Schema type or raw `verdandi::fields`; `patch::set` is raw Fields
because Patch names only changed top-level fields. Every mutation returns
`mutation_result{revision}` through `verdandi::result`.

```cpp
verdandi::catalog::subscription scope;
scope.parts.emplace_back("routing");

auto subscriber = verdandi::catalog::subscriber::create(*client, scope);
auto entry = (*subscriber)->find(*target);
auto snapshot = entry->load<Routing>();
```

`subscription` may combine whole-Zone, Part, and exact-Path coverage; overlap is
normalized. `subscriber::create` returns only after subscribe acknowledgement,
authoritative alignment, the ordered subscribed PING/PONG fence, and metadata
recheck. An Entry is stable for the Subscriber lifetime and atomically swaps
immutable state. `entry::load<Value>` performs local typed projection only and
returns revision, status, synchronization flag, and optional value.

Each Subscriber owns exactly one persistent Pub/Sub listener. Initial
alignment, reconnect alignment, and targeted repair share at most one temporary
task. Work arriving while it runs is coalesced into the same slot; the task
drains it and exits when idle. Publisher owns no task. `try_error()` drains
bounded diagnostics, and `close()` joins the listener and any current temporary
task without closing the Catalog Client or root transport.

When `local_store_path` is non-empty, C++ stores the normalized subscription
scope, monotonic Entry states, and cursor in SQLite transactions. The scope key
is a SHA-256 digest of normalized coverage. Redis remains authoritative and the
checkpoint is never replayed as a Redis write. A persistence failure disables
unsafe further checkpoint advancement for that Client generation while live
in-memory synchronization continues.

## 6. Redis state

For zone prefix `verdandi:catalog:<zone>`:

| Key | Type | Meaning |
| --- | --- | --- |
| `:@meta` | Hash | `@revision`, `@floor_revision` |
| `:@live` | ZSET | member `<part>:<id>`, score current live revision |
| `:@deleted` | ZSET | retained deleted member, score delete revision |
| `:@deleted_time` | ZSET | retained deleted member, score Redis TIME in ms |
| `:<part>:<id>` | Hash | current header and application fields |
| `:<part>:<id>:@field_revisions` | ZSET | field name, score last field update |

A live Hash has exactly four control fields:

```text
@revision
@replace_revision
@kind
@encoded_bytes
```

The Hash key is also its Pub/Sub channel. There is no Stream. Deleted paths have
no data Hash or field-revision ZSET; their retained identity lives only in the
two deleted ZSETs.

## 7. Lua ABI and Redis calls

Four generated scripts are maintained from `lua/src/catalog` and copied
byte-for-byte to Lua, Go, Rust, and C++ outputs.

### Replace

```text
KEYS: meta, live, deleted, deleted_time, hash, field_revisions
ARGV: member, kind, encoded_bytes, field_count, field/value...
```

The script validates canonical ordering and size, allocates the next global
revision, logically removes the old Hash/ZSET with `UNLINK`, writes the
complete Hash, writes every field revision, updates live/deleted indexes,
publishes the complete Replace event.

### Patch

```text
KEYS: same six keys
ARGV: member, base_revision, final_encoded_bytes,
      set_count, field/value...
```

The SDK reads the current header and affected fields with `HMGET` to project
the final size without locking. Lua independently verifies the exact
base revision, shape rules, canonical order, affected fields, and projected
size; it then updates the Hash, field-revision ZSET, live ZSET, meta revision,
and publishes the complete Patch event including both revisions. A race that
changes the Path between HMGET and Lua fails the exact base check before write.

### Delete

```text
KEYS: same six keys
ARGV: member
```

Delete always allocates a revision, unlinks current data, updates the deleted
revision/time ZSETs, prunes retained tombstones, and publishes Delete.

Tombstones are retained while both hidden bounds permit it: 24 hours and
1,000,000 members. Every Delete prunes expired entries first, then only the
remaining count excess, at most 256 total. `@floor_revision` advances to the
highest revision actually evicted and never advances merely because a Delete
occurred.

### Read

Read atomically snapshots one path:

```text
KEYS: live, deleted, deleted_time, hash, field_revisions
ARGV: member, local_revision
RESULT: absent | deleted | present(replace | patch | unchanged)
```

If the local revision is at or after `@replace_revision`, Read returns only
fields whose field-revision score is newer. Otherwise it returns the complete
Hash.

All mutation scripts derive Pub/Sub payloads from the exact state they commit.
SDK callers cannot supply a second event that could disagree with Redis.

## 8. Pub/Sub and recovery

One Subscriber owns one dedicated Pub/Sub connection, one persistent listener
task, and at most one temporary authoritative synchronization/repair task. The
temporary slot coalesces pending work and exits when idle. One connection can issue multiple `SUBSCRIBE` and
`PSUBSCRIBE` commands:

- exact Path -> exact Hash channel;
- Part -> `verdandi:catalog:<zone>:<part>:*`;
- Zone -> `verdandi:catalog:<zone>:*`.

Coverage is normalized so an exact channel is removed when a selected Part or
Zone already covers it. Every received frame is routed by its actual channel to
one local Entry.

Initial synchronization subscribes first, waits for acknowledgements, and then:

1. reads Zone `@revision` and `@floor_revision`;
2. if the checkpoint cursor is zero, ahead, or below floor, ZSCANs both live and
   deleted indexes; otherwise reads `(cursor, revision]` from both indexes with
   `ZRANGEBYSCORE`;
3. reads selected paths through the atomic Read script, up to 32 concurrently;
4. sends PING on the subscribed connection and waits for the ordered PONG;
5. rereads Zone metadata and repeats with a full scan if floor overtook the
   captured revision;
6. publishes aligned Entries and persists the cursor.

A valid Replace or Delete event can be applied directly. Patch applies only
when the local Entry revision exactly equals its `base_revision`; otherwise
only that path enters Read repair. Pub/Sub lag, parse failure, reconnect, or a
subscription acknowledgement after reconnect requests authoritative repair.
Redis state, not Pub/Sub, remains authoritative.

Pub/Sub notifications carry the complete operation. All SDKs use bounded
streaming MessagePack parsers: declared array/string lengths are checked before
allocation, trailing bytes are rejected, and canonical ordering is validated.

## 9. Optional local persistence

Go uses bbolt, Rust uses redb, and C++ uses SQLite. The checkpoint is a restart accelerator only:

- the complete working set is still loaded into memory;
- Redis remains authoritative;
- entries and cursor are namespaced by normalized subscription scope;
- state is persisted before the scope cursor can advance past it;
- entry revisions and scope cursors advance monotonically even when multiple
  same-scope Subscribers share one store;
- after the first write failure the store is disabled for that Client
  generation, so a failed earlier revision cannot be skipped while a newer
  cursor is recorded;
- persistence errors appear on Subscriber diagnostics; live synchronization
  continues in memory.

## 10. Supported topology and lifecycle

Redis 8 Standalone and Sentinel are supported. C++ currently supports plain
Sentinel but rejects Sentinel+TLS because secure hostname verification cannot
yet be applied to dynamically discovered Boost.Redis targets. Redis Cluster is rejected
because the atomic scripts intentionally span Zone keys without hash tags.

Close is terminal and idempotent. Subscriber cancellation joins its persistent
listener and any current temporary synchronization task. Publisher owns no tasks or resources and therefore has no Close;
each call is admitted directly by the Catalog Client. Catalog Client close
cancels every child, waits for active work, and closes its checkpoint, but
never closes the shared root Redis transport. No Close operation deletes
Catalog data.

## 11. Provisional implementation defaults

Two choices are implemented and can still be changed without altering the
public mutation model:

1. Catalog shares the thin root Redis transport while retaining an independent
   Zone, scripts, synchronization/repair limits, and persistence lifecycle.
2. Tombstone retention is fixed at 24 hours, 1,000,000 members, and at most 256
   evictions per Delete. These are deliberately not public tuning knobs.

The fixed 65,536-field decoder/Lua safety ceiling is the only point that is
stricter than a literal “encoded bytes only” interpretation. Raising it toward
the 4 MiB-derived theoretical maximum is possible, but increases worst-case Lua
table and Pub/Sub framing overhead substantially.
