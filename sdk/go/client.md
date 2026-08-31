# Go Root Client API

Status: implemented for version 1.0.0 on 2026-08-28.

## 1. Boundary

The root `verdandi.Client` is a thin owner of one shared go-redis connection,
its ordinary operation timeout, bounded root commands, and one shutdown signal.
Registration and Catalog clients retain that transport but independently own
their Zone, every domain transition, script, revision, worker, recovery rule,
and persistence decision.

The root command layer accepts complete Redis keys. It never stores or prepends
a Zone, constructs a Catalog or Registration path, advances a domain
revision, or publishes a domain event. Go additionally exposes the same
concrete `*redis.Client` as a borrowed language-native capability; that escape
hatch is outside Verdandi's bounded command and domain guarantees.

Version 1 contains connection health, complete-key/String commands, and basic
Hash commands. Command handles are cheap immutable values containing only
`*Client`; they allocate no connection or worker and require no `Close`.

## 2. Client

```go
func Open(ctx context.Context, config Config) (*Client, error)
func (client *Client) Close() error

func (client *Client) Redis() *redis.Client
func (client *Client) Done() <-chan struct{}
func (client *Client) Timeout() time.Duration

func (client *Client) Ping() error
func (client *Client) PingContext(ctx context.Context) error
func (client *Client) Key() Key
func (client *Client) Hash() Hash
```

```go
type Config struct {
    Standalone       *Standalone
    Sentinel         *Sentinel
    Timeout          time.Duration // zero uses two seconds
}
```

Exactly one topology is required. Both `redis.NewClient` and
`redis.NewFailoverClient` produce the concrete `*redis.Client` stored by the
wrapper; `redis.UniversalClient` is not used and Redis Cluster is not implied.
One root transport may be shared by domain Clients configured for different
Zones when their ACL credentials authorize those keys.

`Open` accepts a Context because it performs a bounded `PING`. `Close` is an
idempotent local shutdown: it broadcasts transport loss and closes go-redis,
so it has no artificial Context. It does not wait for domain workers. Close
Registration and Catalog clients first when deterministic worker cleanup is
required; closing the root first still causes each domain Client to begin its
own joined shutdown.

`Open` deliberately does not issue `INFO`, parse a server version, load Lua, or
start a goroutine. Registration performs its Redis 8 `HELLO` check because its
Hash-field TTL protocol requires Redis 8. Catalog validates the commands it
actually uses. A root connection therefore needs `PING`, not administrative
`INFO`, during construction.

`Ping` is an ordinary one-command operation and has the concise/Context pair
used by `Key` and `Hash`. Both forms remain supported: concise methods are the
normal default, while explicit forms preserve caller cancellation and tracing.

`Redis` returns the exact pointer owned by the root Client; it does not create
another pool or transfer ownership. The caller must not call `Close` on it,
replace hooks/options, or assume it remains usable after root `Close`. The
method intentionally permits go-redis-native commands, pipelines, scripts,
Pub/Sub, and integrations that do not belong in Verdandi's stable root command
surface. Those operations are governed by the Redis ACL credential and
go-redis contract, can bypass Verdandi validation and multi-key invariants, and
are not mapped to Verdandi's bounded error/ambiguity semantics.

`Done` is a receive-only broadcast closed exactly once by root `Close`; it does
not represent temporary disconnection, reconnect, or Sentinel promotion.
`Timeout` returns the normalized immutable ordinary-command timeout.
Registration and Catalog use these three public capabilities directly, so no
private `clientaccess` bridge, global registry, watcher goroutine, or second
transport wrapper is required.

## 3. Key

```go
type Key struct {
    client *Client
}

func (command Key) Get[T any](key string) (T, bool, error)
func (command Key) GetContext[T any](ctx context.Context, key string) (T, bool, error)
func (command Key) Load(key string) ([]byte, bool, error)
func (command Key) LoadContext(ctx context.Context, key string) ([]byte, bool, error)

func (command Key) Set[T any](key string, value T) error
func (command Key) SetContext[T any](ctx context.Context, key string, value T) error
func (command Key) Store(key string, value []byte) error
func (command Key) StoreContext(ctx context.Context, key string, value []byte) error

func (command Key) SetWithTTL[T any](key string, value T, ttl time.Duration) error
func (command Key) SetWithTTLContext[T any](ctx context.Context, key string, value T, ttl time.Duration) error
func (command Key) StoreWithTTL(key string, value []byte, ttl time.Duration) error
func (command Key) StoreWithTTLContext(ctx context.Context, key string, value []byte, ttl time.Duration) error

func (command Key) Delete(key string) (bool, error)
func (command Key) DeleteContext(ctx context.Context, key string) (bool, error)
func (command Key) Exists(key string) (bool, error)
func (command Key) ExistsContext(ctx context.Context, key string) (bool, error)
func (command Key) Expire(key string, ttl time.Duration) (bool, error)
func (command Key) ExpireContext(ctx context.Context, key string, ttl time.Duration) (bool, error)
```

Command mapping:

| API | Redis command | Public result |
| --- | --- | --- |
| `Get[T]`, `Load` | `GET` | typed value or owned bytes plus presence |
| `Set[T]`, `Store` | `SET` | error only |
| `SetWithTTL[T]`, `StoreWithTTL` | `SET PX` | error only |
| `Delete` | single-key `DEL` | whether the key existed |
| `Exists` | single-key `EXISTS` | whether the key exists |
| `Expire` | `PEXPIRE` | whether the TTL was applied |

A missing `GET` returns zero T, `false`, and nil without invoking a decoder. A
present empty String is present: raw `Load` returns a non-nil zero-length slice,
`true`, and nil. Persistent writes clear a previous TTL as ordinary Redis `SET`
does. TTL writes and `Expire` require a positive duration represented in exact
milliseconds.

### 3.1 Scalar codec

The typed String and Hash-field codec is intentionally small and stable:

- byte slices, strings, booleans, and fixed-width `int8` through `int64` and
  `uint8` through `uint64` are built in; named types with those underlying
  shapes are also accepted;
- booleans are exactly `0` or `1`;
- integers are canonical base-10 text: no leading plus, leading zero,
  whitespace, or negative zero is accepted;
- byte-slice results are detached from the Redis reply;
- an application value may implement `encoding.BinaryMarshaler` and
  `encoding.BinaryUnmarshaler`, or the corresponding Text interfaces; Binary
  takes precedence over Text; and
- pointer T, `int`, `uint`, `uintptr`, floats, maps, interfaces, and arbitrary
  slices are rejected with `CodeContract` before Redis I/O.

No automatic JSON, MessagePack, protobuf, or other schema codec is selected
from T. Applications either implement the standard interfaces or call raw
`Load`/`Store` and use their external codec explicitly.

## 4. Hash

```go
type Hash struct {
    client *Client
}

func (command Hash) Get[T any](key string) (T, error)
func (command Hash) GetContext[T any](ctx context.Context, key string) (T, error)
func (command Hash) Load(key string) (Fields, error)
func (command Hash) LoadContext(ctx context.Context, key string) (Fields, error)

func (command Hash) Set[T any](key string, value T) error
func (command Hash) SetContext[T any](ctx context.Context, key string, value T) error
func (command Hash) Store(key string, fields Fields) error
func (command Hash) StoreContext(ctx context.Context, key string, fields Fields) error

func (command Hash) Delete(key string, fields ...string) (int64, error)
func (command Hash) DeleteContext(ctx context.Context, key string, fields ...string) (int64, error)
func (command Hash) Exists(key string, field string) (bool, error)
func (command Hash) ExistsContext(ctx context.Context, key string, field string) (bool, error)
func (command Hash) Length(key string) (int64, error)
func (command Hash) LengthContext(ctx context.Context, key string) (int64, error)
```

Command mapping:

| API | Redis command | Public result |
| --- | --- | --- |
| `Get[T]` | exact `HMGET` derived from T | decoded struct |
| `Load` | `HGETALL` | detached `Fields` |
| `Set[T]`, `Store` | `HSET` | error only |
| `Delete` | `HDEL` | fields actually removed |
| `Exists` | `HEXISTS` | whether the field exists |
| `Length` | `HLEN` | current field count |

`Get[T]` accepts one non-pointer struct. Exported top-level fields are selected
in declaration order. The exact Go field name is the default, `redis:"name"`
overrides it, `redis:"-"` excludes it, and unexported fields are ignored.
Empty and duplicate Redis names, comma options, unsupported field types, and a
struct without selected fields are contract errors. Embedded structs are not
flattened. Each Client caches one immutable descriptor per concrete T.

HMGET positions that are nil leave the destination field at its Go zero value;
a missing Hash therefore returns zero T without error. Present malformed bytes
are `CodeCorrupt` and no partially decoded T is returned.

`Set[T]` writes every selected field, including zero values. `Store` writes all
provided raw fields. Both are HSET patch operations: neither removes unknown
fields or claims complete replacement. `Delete` removes Hash fields. Complete
key deletion remains `client.Key().Delete(key)`.

`Load` is the dynamic-map path and returns a non-nil empty `Fields` map for a
missing key. It deliberately performs complete O(N) HGETALL work.

## 5. Limits and ownership

Version 1 enforces these internal protocol ceilings before writes and after
reads:

| Item | Ceiling |
| --- | ---: |
| Redis key | 1,024 bytes |
| Hash fields per operation/result | 4,096 |
| Hash field name | 1,024 bytes |
| String or individual field value | 512 KiB |
| Complete encoded Hash command/result | 512 KiB |

Raw byte writes are detached before dispatch. Raw byte and `Fields` reads are
caller-owned and do not alias mutable SDK state. Limits are deliberately not
Config options in version 1; widening them changes the supported resource
contract and requires review.

## 6. Context, shutdown, and errors

Every concise command delegates to its Context form with
`context.Background()`. The single implementation then applies the configured
operation timeout. The explicit form also preserves caller cancellation,
deadline, values, and tracing; the earlier deadline wins. Nil explicit Context
is invalid. No request Context is stored in `Client`, `Key`, or `Hash`, and an
ordinary command creates no admission or shutdown-joining bookkeeping.

Validation and codec failures occur before Redis I/O. A deterministic Redis
server error such as WRONGTYPE is `CodeProtocol`. Read transport failures are
`CodeUnavailable`, deadline expiry is `CodeDeadline`, and shutdown is
`CodeClosed`. A write whose transport result is lost after dispatch is
conservatively `CodeAmbiguous`; callers must reconcile before retrying.
The driver's definite `redis.ErrClosed` is always `CodeClosed`, including when
the command belongs to a write family.

## 7. Deferred families

List, Set, Sorted Set, Stream, Pub/Sub, Scan, Pipeline, Transaction, Script, and
server/admin commands are outside Verdandi's version-1 bounded wrappers. Each
would need its own ownership, blocking, paging, partial-result, atomicity, and
capacity review before receiving a Verdandi method. Advanced Go callers may
still reach them through borrowed `Redis()` under go-redis semantics and their
ACL; that escape hatch does not make them part of Verdandi's stable protocol
surface.
