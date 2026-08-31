# Rust Root Client API

Status: implemented for version 1.0.0, direct-Client ownership finalized on 2026-08-28.

## 1. Language shape and boundary

The Rust API performs the same Redis operations and applies the same storage,
presence, capacity, and domain boundaries as the Go root Client while using
native async and trait conventions.

- One awaited method is sufficient; there are no Context-suffixed variants.
- Missing String keys use `Result<Option<T>>`.
- `KeyCommands` and `HashCommands` borrow `Client`, allocate no connection or
  task, and cannot outlive it.
- `HashValue` supplies a static field vector because Rust has no runtime struct
  reflection.
- The public crate never exposes a `fred` type or raw command executor.

Root commands accept complete Redis keys and perform no Registration or Catalog
transition, revision update, event publication, or namespace construction.

## 2. Client

```rust
impl Client {
    pub async fn open(config: Config) -> Result<Self>;
    pub async fn close(&self) -> Result<()>;

    pub async fn ping(&self) -> Result<()>;
    pub const fn key(&self) -> KeyCommands<'_>;
    pub const fn hash(&self) -> HashCommands<'_>;
}
```

Root `Config` contains the Redis/Sentinel endpoint, public `timeout: Duration`,
and reconnect delays only. `timeout` defaults to two seconds and bounds each
ordinary command. Zone is required independently by
`registration::Config::new(zone)` and `catalog::Config::new(zone)`, so one
ACL-authorized root Client can serve multiple Zones.

Each returned Future is caller-controlled. The Client applies its configured
operation timeout and cancels pending commands during shutdown. Root commands
do not take a lifecycle mutex, allocate an `Arc` guard, or update an active
counter. Registration and Catalog retain their own admission accounting because
their explicit close operations join domain-owned work.

`Client` is a cheap clone over one private `Arc<Inner>`; cloning it creates no
connection or task. Registration and Catalog retain that same public root type,
not a second private Transport capability. Dropping the application's root
variable therefore cannot invalidate a domain that still owns its dependency.

`close().await` permanently broadcasts shutdown to every clone and awaits Fred
`quit()` without waiting for Registration or Catalog work. Repeated and
concurrent calls share the terminal result. Drop cannot await: dropping the last
root-or-domain-held `Client` clone signals shutdown and schedules best-effort
close when a Tokio runtime is available. Applications use explicit close, in
domain-before-root order, when cleanup completion matters.

Root Open performs only `PING`. Registration bootstrap performs the Redis
8 `HELLO` version check, installs/reads its Zone policy, and loads its scripts.
The root Client's private `Inner` retains Fred connection construction data so
Selectors and Catalog Subscribers can open dedicated Pub/Sub connections.
Domain clients call crate-private methods on their retained root Client; no Fred
type, raw driver accessor, or alternate transport handle is public or private.

Root and domain Open create no task whose only purpose is waiting for shutdown.
The root token directly prevents later domain admission, existing owner tasks
observe hierarchical child tokens, and explicit domain Close performs the
required join. A published Registration still owns one task; a Selector owns
one persistent listener plus at most one temporary synchronization task.

## 3. Scalar value traits

```rust
pub trait DecodeValue: Sized {
    fn decode_value(source: &[u8]) -> Result<Self>;
}

pub trait EncodeValue {
    fn encode_value(&self, destination: &mut Vec<u8>) -> Result<()>;
}
```

Verdandi implements these traits for `Vec<u8>`, `String`, `bool`, and the
fixed-width signed and unsigned integer types. `[u8]` and `str` implement the
write trait. Booleans are exactly `0`/`1`; integers are canonical base-10 text.
`usize`, `isize`, floats, and automatic Serde are intentionally absent.

An application type implements the traits for its own JSON, MessagePack,
protobuf, or binary format. Applications may instead call raw `load`/`store`
and keep all codec logic external.

## 4. Key

```rust
pub struct KeyCommands<'client> {
    client: &'client Client,
}

#[must_use]
pub struct TtlKeyCommands<'client> {
    client: &'client Client,
    ttl: Duration,
}

impl<'client> KeyCommands<'client> {
    pub async fn get<T: DecodeValue>(&self, key: &str) -> Result<Option<T>>;
    pub async fn load(&self, key: &str) -> Result<Option<Vec<u8>>>;

    pub async fn set<T: EncodeValue + ?Sized>(&self, key: &str, value: &T) -> Result<()>;
    pub async fn store(&self, key: &str, value: &[u8]) -> Result<()>;
    pub const fn with_ttl(self, ttl: Duration) -> TtlKeyCommands<'client>;

    pub async fn delete(&self, key: &str) -> Result<bool>;
    pub async fn exists(&self, key: &str) -> Result<bool>;
    pub async fn expire(&self, key: &str, ttl: Duration) -> Result<bool>;
}

impl TtlKeyCommands<'_> {
    pub async fn set<T: EncodeValue + ?Sized>(self, key: &str, value: &T) -> Result<()>;
    pub async fn store(self, key: &str, value: &[u8]) -> Result<()>;
}
```

`get::<T>` returns `Ok(None)` without invoking T for a missing key. A present
empty String remains `Some`. Persistent writes execute directly:

```rust
client.key().set(key, &value).await?;
client.key().store(key, bytes).await?;
```

`with_ttl` is a one-use write mode and performs no I/O:

```rust
client.key().with_ttl(ttl).set(key, &value).await?;
client.key().with_ttl(ttl).store(key, bytes).await?;
```

The terminal operation validates a positive exact-millisecond TTL. Consuming
the mode prevents it from accidentally affecting a later read or write and
leaves `ttl(key)` available for a future TTL-query API.

## 5. Hash and derive

```rust
pub trait HashValue: Sized {
    const FIELDS: &'static [&'static str];

    fn decode_hash(values: &[Option<Vec<u8>>]) -> Result<Self>;
    fn encode_hash(&self, destination: &mut Fields) -> Result<()>;
}

pub struct HashCommands<'client> {
    client: &'client Client,
}

impl HashCommands<'_> {
    pub async fn get<T: HashValue>(&self, key: &str) -> Result<T>;
    pub async fn load(&self, key: &str) -> Result<Fields>;

    pub async fn set<T: HashValue>(&self, key: &str, value: &T) -> Result<()>;
    pub async fn store(&self, key: &str, fields: &Fields) -> Result<()>;
    pub async fn delete(&self, key: &str, fields: &[&str]) -> Result<u64>;
    pub async fn contains_field(&self, key: &str, field: &str) -> Result<bool>;
    pub async fn len(&self, key: &str) -> Result<u64>;
}
```

The command mapping is HMGET, HGETALL, HSET, HSET, HDEL, HEXISTS, and HLEN in
that order. HSET writes are patch operations and discard Redis's “new fields”
count. HDEL preserves its useful multi-field removal count. Complete-key delete
remains `client.key().delete(key)`.

`HashValue` is usable manually and does not require a macro. The official
`#[derive(HashValue)]` generates the same static contract for a named-field
struct:

```rust
#[derive(Default, HashValue)]
struct Header {
    #[redis(name = "@revision")]
    revision: u64,
    name: String,
    #[redis(skip)]
    local_cache: String,
}
```

The exact Rust field identifier is the default Redis name. `#[redis(name =
"...")]` overrides it and `#[redis(skip)]` excludes it. The macro rejects an
enum, tuple/unit struct, duplicate or empty names, multiple Redis attributes,
and a struct without selected fields at compile time.

Each selected field must implement `DecodeValue`, `EncodeValue`, and `Default`;
skipped fields must implement `Default`. Missing HMGET positions therefore use
the field's default without invoking its decoder. A manual `HashValue` may
define another explicit missing-field policy. The wrapper validates manual
field vectors for non-empty, unique, bounded names and verifies that HSET
encoding produced exactly the advertised fields.

## 6. Limits, errors, and ownership

Rust enforces the same version-1 ceilings as Go: a 1,024-byte key, 4,096 Hash
fields, a 1,024-byte field name, a 512-KiB String or field value, and a 512-KiB
complete Hash command/result. Raw reads are owned; writes copy caller slices or
maps before the asynchronous command can outlive the borrow.

Built-in malformed values are `Code::Corrupt`; invalid arguments, trait
contracts, and capacity violations are stable `Invalid`, `Contract`, and
`Capacity` errors. Deterministic Redis errors such as WRONGTYPE are `Protocol`.
Read transport loss is `Unavailable`, read timeout is `Deadline`, and shutdown
is `Closed`. A write whose result may have been lost after dispatch is
`Ambiguous` and must be reconciled before retry.

## 7. Cross-language names

| Meaning | Go | Rust |
| --- | --- | --- |
| command group | `client.Key()` | `client.key()` |
| typed String read | `Get[T]` | `get::<T>` |
| persistent write | `Set[T]` | `set(...)` |
| expiring write | `SetWithTTL[T]` | `with_ttl(...).set(...)` |
| complete Hash load | `Load` | `load` |
| Hash-field existence | `Exists` | `contains_field` |
| Hash-field count | `Length` | `len` |

List, Set, Sorted Set, Stream, Pub/Sub, Scan, Pipeline, Transaction, Script, and
server/admin families remain outside the version-1 root surface.
