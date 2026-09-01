# Go Typed Redis Hash Contract

Status: implemented for non-production Alpha version 0.1.0 on 2026-08-27. The complete root Client,
Key, codec, limit, lifecycle, and error inventory is in [`client.md`](client.md).

## 1. Exact projection

`Hash.Get[T]` maps a non-pointer Go struct to one exact HMGET. It does not call
HGETALL, infer a dynamic map schema, recursively flatten a struct, or prepend a
Verdandi namespace.

```go
type Header struct {
    Revision uint64 `redis:"@revision"`
    Kind     string `redis:"@kind"`
    Bytes    uint64 `redis:"@encoded_bytes"`
}

header, err := client.Hash().Get[Header](key)
header, err := client.Hash().GetContext[Header](ctx, key)
```

Both calls execute exactly:

```text
HMGET <key> @revision @kind @encoded_bytes
```

Field discovery is deterministic:

- exported top-level fields remain in declaration order;
- an untagged field uses its exact Go identifier;
- `redis:"name"` replaces that name and `redis:"-"` excludes the field;
- unexported fields are ignored;
- empty names, duplicates, comma options, unsupported field types, and no
  selected fields are contract errors; and
- embedded fields are not flattened; the embedded value itself needs a scalar
  codec or must be excluded.

One immutable descriptor is cached per concrete T on each root Client. The same
descriptor drives `Get[T]` and `Set[T]`, so field names and order cannot drift.

## 2. Missing and malformed fields

HMGET reply positions map directly to destination fields. A nil position leaves
the fresh destination at its Go zero value and does not invoke a decoder. A
missing Hash therefore returns zero T and nil error. This intentionally does
not distinguish a missing field from a present encoded zero.

A present empty value is decoded. Malformed booleans, noncanonical or
out-of-range integers, invalid application encoding, response-shape mismatch,
and unexpected reply types are `CodeCorrupt`. The method never publishes a
partially decoded T.

Callers that need raw presence or unknown fields use `Load`:

```go
fields, err := client.Hash().Load(key)
fields, err := client.Hash().LoadContext(ctx, key)
```

Those methods execute HGETALL and return every field as detached
`Fields map[string][]byte`. A missing key returns a non-nil empty map. The call
is explicit O(N) work and is bounded to 4,096 fields and 512 KiB total encoded
field-name/value bytes.

## 3. Symmetric HSET patching

```go
err := client.Hash().Set(key, header)
err := client.Hash().SetContext(ctx, key, header)

err := client.Hash().Store(key, fields)
err := client.Hash().StoreContext(ctx, key, fields)
```

`Set[T]` encodes every selected struct field, including zero values, through
the same scalar plan used by `Get[T]`. `Store` copies and writes every supplied
raw field. Both execute HSET and discard its “new fields” count because zero is
also a successful overwrite.

Neither method is a replacement operation. Fields not represented by the
input remain in Redis. Field removal and complete-key removal are deliberately
distinct:

```go
removed, err := client.Hash().Delete(key, "fieldA", "fieldB")
deleted, err := client.Key().Delete(key)
```

The first is HDEL and returns the number of fields actually removed. The second
is single-key DEL and reports whether the complete key existed.

## 4. Type boundary

Hash fields share the root scalar codec:

- bytes, strings, booleans encoded as `0`/`1`, and fixed-width integers encoded
  as canonical decimal text;
- `encoding.BinaryMarshaler`/`BinaryUnmarshaler`, with the Text pair as
  fallback; and
- no `int`, `uint`, float, pointer T, interface, inferred JSON, MessagePack,
  protobuf, map conversion, or schema registry.

`Hash.Get[string]`, `Hash.Get[map[string][]byte]`, and `Hash.Get[[]byte]` are
invalid because HMGET needs a fixed field vector. `Key.Get[T]` is the scalar
String API; `Hash.Load` is the dynamic Hash-map API.

## 5. Domain boundary

These wrappers are ordinary Redis commands only. They do not align Catalog
revisions, create tombstones, publish Catalog notifications, manage
Registration leases, or update Registry membership. Atomic domain transitions
remain in the child package's private Lua capability. The root API exposes no
raw Redis CRUD escape hatch beyond its reviewed `Key` and `Hash` command sets.
