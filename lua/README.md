# Redis Lua programs

Verdandi requires Redis Open Source 8.0.0 or later. The canonical executable
scripts in this directory are generated protocol artifacts: every SDK embeds
the same bytes, loads the Registration and Catalog operations during Client
bootstrap, executes the selected operation with `EVALSHA`, and reloads that
operation after `NOSCRIPT`.

## Registration

The canonical executables are:

```text
registration/register.lua
registration/update.lua
registration/renew.lua
registration/unregister.lua
```

Each executable owns one lifecycle operation's complete atomic invariant for
one Registration and its Registry membership field. A mutation executes
exactly one script, so Hash, membership, matched expiry, reply, and publication
remain one non-interleaved Redis operation.

The maintained sources live under [`src/registration`](src/registration). A
reviewed manifest composes shared atomic-glue/encoding fragments with one final
action fragment. This avoids copying Redis transition logic while keeping the
Redis execution body specific to the selected operation. Generated files carry
`DO NOT EDIT` headers and must be refreshed or checked with:

```text
python testkit/lua/generate_registration.py
python testkit/lua/generate_registration.py --check
```

Generation is deterministic, uses canonical LF bytes, writes atomically, and
fails if a fragment is unused, duplicated, unsafe, or assigned to the wrong
action. It emits the canonical scripts and byte-identical Go/Rust embedded
copies.

During the current unpublished maintainer-review phase, canonical Registration
fragments and generated scripts include detailed Chinese parameter and logical-
block comments. They raise source/cache bytes but do not add executable Lua
statements or change steady EVALSHA calls. Release readiness requires converting
handwritten production comments back to concise English through the generator;
generated copies are never translated or edited independently.

Every invocation supplies exactly two keys:

```text
KEYS[1] = verdandi:registration:<zone>:<type>:<uuid>
KEYS[2] = verdandi:registry:<zone>:<type>
```

`ARGV` uses a frozen operation-specific v1 layout. The selected SHA already
identifies protocol and operation, so fixed controls contain values only;
dynamic Attr/Data retain their field names so Redis can write partial fields.
Application values remain opaque binary strings.

| Kind | Fixed control slots | Named payload tail |
| --- | --- | --- |
| `register` | `[1] uuid`, `[2] revision`, `[3] ttl_ms`, `[4] version` | `[5..]` complete Attr (`.name`) then complete Data field/value pairs |
| `update` | `[1] uuid`, `[2] revision`, `[3] version-or-empty` | `[4..]` changed Data field/value pairs |
| `renew` | `[1] uuid`, `[2] revision` | none |
| `unregister` | `[1] uuid` | none |

The Update version slot is always present: an empty bulk string means
unchanged, otherwise it is the new positive integer. A future incompatible
control layout receives a new generated script/SHA; v1 positions are never
reinterpreted. Replies and Pub/Sub events remain self-describing named
key/value arrays.

The SDK validates protocol version, operation, key components, UUID, canonical
integers, reserved names, Attr/Data structure, immutable fields, field count,
field length, total bytes, and no-op updates before Redis I/O. Lua deliberately
does not repeat those checks or decode application values. Direct script use
that bypasses an authorized SDK is outside the supported ACL/trust boundary.

The selected script obtains `@timestamp` from Redis `TIME`. It retains only the
conditions that require the current Redis state: missing/stale/next revision,
monotonic timestamp, and a Redis-safe `timestamp + ttl` deadline no greater
than the Redis 8 Hash-field absolute-expiry ceiling `2^46-1`. It then joins the
Registration Hash, `PEXPIREAT`, Registry `HSETEX PXAT`, reply, and one
MessagePack event as a single non-interleaved execution. A full `register`
replaces complete state after rejecting only a lower stored revision. `renew`
rewrites the same membership revision so an absent field is repaired while its
field TTL is refreshed.

The supported SDK keeps each Registration's fully encoded desired state and its
confirmation status in one bounded process-memory cache and checks the complete
projected record against the Client's last-valid limits from
`verdandi:config:<zone>` before advancing revision or invoking the selected script. It
does not persist that cache, UUID, or a replay log to local disk. A new-revision
`update` therefore performs no `HGETALL` and remains proportional to its patch.
The configuration key is deliberately not a third Lua key: its values are
refreshed into an atomic process-memory snapshot, and rereading them on every
update would put administrative configuration lookup back on the hot path.
Selectors independently validate fetched Redis records before exposing them.

Replies are alternating RESP key/value arrays. Examples:

```text
["&result", "ok", "@revision", 42, "@timestamp", 1787371200123]
["&result", "error", "&status", "stale", "@revision", 45]
["&result", "error", "&status", "missing"]
```

All statuses come from the protocol's standard string registry. A successful
same-revision `update` is an idempotent acknowledgement: the SDK guarantees
that a revision identifies one content state, so Lua does not compare the
retry's payload. It does not refresh the lease or publish another event. A
repeated `renew` is a new liveness proof
and does both. A repeated `unregister` removes any residual membership field,
returns `ok`, and emits no event when the Registration is already absent.

## Catalog

Four Catalog executables are generated from [`src/catalog`](src/catalog):

```text
catalog/read.lua
catalog/replace.lua
catalog/patch.lua
catalog/delete.lua
```

Refresh or verify all canonical and embedded Go/Rust copies with:

```text
python -B testkit/lua/generate_catalog.py
python -B testkit/lua/generate_catalog.py --check
```

One application path is
`verdandi:catalog:<zone>:<part>:<id>`. It is a raw Value, contiguous Array,
or Map Hash. The SDK owns external codecs, validation, complete-value sizing,
Patch projection, and argument ordering. Lua joins only the private TTL lease,
Redis-owned revision, authoritative Hash/field revisions, live/deleted indexes,
and Pub/Sub notification.

All scripts validate their exact `KEYS` and `ARGV` counts. Mutations take six keys:
Zone metadata, live/deleted/deleted-time indexes, path Hash, field-revision
ZSET.

Replace arguments are member, shape, final encoded bytes, field count,
then canonical field/value pairs. Patch adds exact `base_revision` and carries
only non-empty additions/overwrites. Delete takes member. Read takes
live/deleted/deleted-time, Hash, and field-revision keys plus member and local
revision.

Every accepted mutation allocates one canonical decimal Zone revision in
`1..=9007199254740991`. Replace/Delete are last-write-wins. Patch validates
the exact base at lease acquisition and commit. Delete always allocates a fresh
tombstone, removes live state, and prunes bounded deleted history. Floor advances
only to an actually evicted delete revision.

Lua packs a positional MessagePack `v1` Replace, Patch, or Delete event from
the exact state it commits and publishes it on the path Hash key. Pub/Sub is
not durable; Subscriber recovery uses the Read script and Zone ZSET indexes.
Read returns absent, deleted, unchanged, a complete replacement, or only fields
newer than the local revision when no later Replace invalidates that delta.

Replies are alternating named string pairs. Successful mutations include the
accepted `@revision`; failures use stable statuses such as `invalid`,
`capacity`, `stale`, `transition`, or `corrupt`. A malformed ABI, wrong
Redis type, impossible index/header relationship, oversized value, or exhausted
revision is rejected before predictable mutation work.

## Selector

Selector has deliberately no Lua program. It mutates no Redis state, and a Lua
script that scanned an entire Registry would block Redis for work proportional
to the service population and reintroduce a maximum-service limit.

Selector does not refresh Registration deployment policy. Register owns that
policy lifecycle, while Selector validates discovered records against immutable
protocol ceilings. This prevents a later policy reduction from retroactively
hiding a Registration that was valid when written. Initial synchronization then
uses ordinary bounded commands:

1. `SUBSCRIBE verdandi:registry:<zone>:<type>` and wait for its acknowledgement.
2. Decode subsequent MessagePack events into a bounded accumulator containing
   at most one pending logical change per UUID.
3. Calibrate the connection-generation `RedisClock` with `TIME`.
4. Traverse the Registry with repeated
   `HSCAN verdandi:registry:<zone>:<type> <cursor> COUNT <page-limit>` calls.
5. For a UUID whose membership revision matches active or retained cached
   content, pipeline `HMGET @revision @timestamp`; fetch `HGETALL` only for a
   new or changed content revision.
6. Send `PING <nonce>` on the subscribed connection and consume buffered
   `register`, `update`, `renew`, and `unregister` events through the matching
   `PONG`.
7. Repair only UUIDs whose revision transition or fetched state is incomplete,
   then publish one immutable local view.

`HSCAN` pages are not an atomic snapshot. Correctness comes from the
subscribe-before-scan barrier, per-Registration revisions and timestamps, the
post-scan `PING` barrier, and targeted repair. This keeps every Redis operation
bounded while allowing an unbounded total Registration population. The Alpha
SDKs implement the per-UUID accumulator with independent entry and byte limits.
They also implement a separate byte-bounded retained view. Natural expiry or
fenced absence removes a record from selection immediately and may retain its
payload until `@timestamp + 2*@ttl`; a valid same-UUID event/fetch may reactivate
it, while explicit `unregister` purges it. Retained content is local only,
non-selectable, 64 MiB by default, optionally disabled, and evicted by earliest
deadline when its independent budget is exceeded.

## Integration test

The integration test requires an isolated Redis 8 endpoint plus Python package
dependencies from [`requirements.txt`](../testkit/lua/requirements.txt):

```text
python testkit/lua/registration_test.py --redis-url redis://127.0.0.1:6379/0
python testkit/lua/catalog_test.py --redis-url redis://127.0.0.1:6379/0
```

The test uses a unique Zone and removes only the keys it creates; it never
flushes the selected database. It does execute `CONFIG RESETSTAT` to prove that
a new-revision Update issues no `HGETALL`, and `SCRIPT FLUSH` to verify SDK
reload behavior for every operation independently. It also proves the boundary
directly: raw over-count/oversized input reaches Redis when the SDK is bypassed,
while SDK tests reject it before I/O. The endpoint therefore must not share
configuration statistics or a script cache with unrelated workloads. The
Catalog test additionally proves Value/Array/Map replacement, strict Patch,
fresh Delete tombstones, Pub/Sub payloads, bounded tombstone/floor pruning,
malformed ABI and Redis-type rejection, generated-script reload, and the exact
`2^53-1` revision boundary.

An isolated production hot-path benchmark measures the generated one-field,
version-plus-Data, and 32-Data-field Update paths, plus Renew and Unregister:

```text
python testkit/lua/registration_benchmark.py --redis-url redis://127.0.0.1:6379/0 --output testkit/results/lua-registration-hot-path.json
```

The benchmark rotates variant order across eleven trials. Optional
`--baseline-update-sha`, `--baseline-renew-sha`, and
`--baseline-unregister-sha` arguments compare cached pre-change scripts against
the generated candidates without flushing the shared script cache. It is evidence for
this implementation, not a protocol throughput promise. Its fixture disables
subscribers and Redis persistence so script-body effects are not confused with
Pub/Sub fan-out or disk work.

The separate [line-optimization review](../registration/lua-optimization.md) records both
line-audit passes, accepted/rejected candidates, and the final result. With the
optimized source present, its executable benchmark measures current Register
as one regression baseline for small and default-maximum payload shapes. A
cached old SHA may be supplied for paired comparison:

```text
python testkit/lua/registration_line_benchmark.py --redis-url redis://127.0.0.1:6379/0 --output testkit/results/lua-register-current.json
```

```text
python testkit/lua/registration_line_benchmark.py --redis-url redis://127.0.0.1:6379/0 --baseline-sha <cached-sha> --baseline-source-bytes <bytes>
```

It also probes the running Redis version's Hash-field absolute-expiry ceiling
and verifies Lua-number argument precision. The accepted positional ABI,
`HSETEX`, numeric arguments, inlining policy, measurements, and completed
qualification are documented in the review.
