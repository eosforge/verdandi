# Verdandi C++11 Legacy Facade

## Purpose

`verdandi::legacy` is a header-only C++11 convenience facade over Verdandi C
ABI v1. It gives C++11, C++14, and C++17 applications RAII ownership,
`result<T>`, `optional<T>`, `std::chrono` durations, typed Attr/Data schemas,
and typed Registration, Selector, and Catalog calls without duplicating Redis
transport or protocol state machines.

The facade and the native C++23 API execute the same compiled runtime. The C
ABI remains the stable allocator-safe binary boundary; the facade is a source
API compiled into the consuming application.

## Build model

A source checkout still needs a C++23-capable compiler for the Verdandi core.
Only the application target is compiled as C++11/14/17. A toolchain that cannot
compile the core must link a prebuilt compatible runtime.

```cmake
cmake_minimum_required(VERSION 3.28)
project(example LANGUAGES C CXX)

add_subdirectory(vendor/verdandi/sdk/cpp)

add_executable(example main.cpp)
set_target_properties(example PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(example PRIVATE verdandi::legacy)
```

Include the complete facade with:

```cpp
#include <verdandi/legacy.hpp>
```

`verdandi::legacy` links `verdandi::c`, which links the same native runtime
through `LINK_ONLY`; the C++23 compile feature does not propagate to the
consumer target. Static and shared runtime builds use the same facade.

## Results, errors, and ownership

Fallible calls return `verdandi::legacy::result<T>`. Test the result before
accessing `value()`/`operator*()` or `failure()`:

```cpp
verdandi::legacy::result<verdandi::legacy::client> opened =
    verdandi::legacy::client::open(configuration_json);
if (!opened) {
    log(opened.failure().code(),
        opened.failure().field(),
        opened.failure().detail());
    return;
}

verdandi::legacy::client root = std::move(*opened);
```

Errors own the stable C ABI `code`, `field`, `detail`, and optional authoritative
revision. `result<void>` allocates no success payload; failure storage is
created only on an error path. `optional<T>` represents an absent Key, Catalog
Entry, or Selector choice without a heap allocation.

Root and domain Client copies share ownership. Registration, Selector,
Publisher, and Entry handles are move-only. Child handles retain their owning
domain and root state, so ordinary C++ destruction cannot free an ancestor
while a child still needs its C handle. Explicit `close()` reports shutdown
failure; destructors perform the C ABI's best-effort release.

## Strong and raw Fields

The facade owns top-level Fields as an ordered `std::map<std::string, bytes>`.
Values are binary and Redis does not parse application JSON. Built-in scalar
codecs cover `bytes`, `std::string`, `bool`, and integral types using canonical
text encoding. Applications may specialize `value_codec<T>` or `codec<T>` for
their own types.

```cpp
struct proxy_attr {
    std::string region;
};

struct proxy_data {
    std::int64_t power;
    bool ready;

    proxy_data() : power(0), ready(false) {}
};

VERDANDI_LEGACY_SCHEMA(
    proxy_attr,
    VERDANDI_LEGACY_FIELD(proxy_attr, region));

VERDANDI_LEGACY_SCHEMA(
    proxy_data,
    VERDANDI_LEGACY_FIELD(proxy_data, power),
    VERDANDI_LEGACY_NAMED_FIELD(proxy_data, ready, "available"));
```

Schema decoding requires every declared field and ignores no malformed scalar.
Duplicate raw field insertion returns `contract`. `fields` itself also satisfies
the typed API, so callers that already own the protocol's flattened Fields can
avoid declaring an application schema.

The C++11 schema layer cannot offer the native C++23 concepts and `consteval`
diagnostics. An invalid descriptor therefore fails through ordinary template
instantiation diagnostics, while runtime input failures use `result<T>`.

## Root Client

The canonical strict v1 JSON document is the sole Legacy configuration model:

```cpp
const std::string configuration_json =
    "{\"version\":\"v1\","
    "\"redis\":{\"mode\":\"standalone\","
    "\"addresses\":[\"127.0.0.1:6379\"]},"
    "\"registration\":{\"zone\":\"Production\"},"
    "\"catalog\":{\"zone\":\"Production\"}}";

verdandi::legacy::result<verdandi::legacy::client> opened =
    verdandi::legacy::client::open(configuration_json);
verdandi::legacy::client root = std::move(*opened);

root.ping();
root.key_store("application:key", payload,
               std::chrono::seconds(30));
root.hash_store("application:hash", raw_fields);
```

The facade does not define another configuration default/range table. Parsing,
validation, ACLs, pooling, reconnect behavior, and topology remain in the
native runtime selected by the JSON document.

Before constructing optional integrations, a lower-standard consumer can query
the loaded runtime without allocating a handle:

```cpp
if (!verdandi::legacy::has_capability("redis.sentinel_tls")) {
    report_incompatible_runtime();
}
```

Unknown and empty names return false. Capability presence describes compiled
code only; it does not test the current Redis topology, certificate files, ACLs,
or network.

## Registration

Registration construction is local. `publish` remains the explicit readiness
boundary:

```cpp
verdandi::legacy::result<verdandi::legacy::registration_client> domain_result =
    verdandi::legacy::registration_client::open(root);
verdandi::legacy::registration_client domain =
    std::move(*domain_result);

verdandi::legacy::registration_options options;
options.type = "Proxy";
options.ttl = std::chrono::seconds(15);
options.version = 1;

typedef verdandi::legacy::registration<proxy_attr, proxy_data>
    proxy_registration;
verdandi::legacy::result<proxy_registration> created =
    proxy_registration::create(domain, options);
proxy_registration registration = std::move(*created);

start_listening();

proxy_attr attr;
attr.region = "cn-east";
proxy_data data;
data.ready = true;
registration.publish(attr, data);
registration.update(data);
registration.renew();
registration.close();
```

The wrapper performs schema conversion on the calling thread, then forwards
one owned Fields input through the C ABI. The core still owns the single
per-Registration worker, coalescing mailbox, renewal timer, desired/confirmed
state, recovery, and Redis interaction.

## Selector

`one` and `any` accept any C++11 callable with the documented signature.
Candidates are borrowed only for the synchronous callback. Returned candidates
are detached typed values:

```cpp
struct least_power {
    verdandi::legacy::result<
        verdandi::legacy::optional<verdandi::legacy::choice> >
    operator()(verdandi::legacy::candidates<proxy_attr, proxy_data>& values) const {
        if (values.size() == 0) {
            return verdandi::legacy::optional<verdandi::legacy::choice>();
        }

        verdandi::legacy::result<proxy_data> data = values.data(0);
        if (!data) {
            return data.failure();
        }
        ++data->power;

        verdandi::legacy::choice selected(0);
        verdandi::legacy::result<void> changed =
            values.mutate(selected, *data);
        if (!changed) {
            return changed.failure();
        }
        return verdandi::legacy::optional<verdandi::legacy::choice>(selected);
    }
};

typedef verdandi::legacy::selector<proxy_attr, proxy_data> proxy_selector;
verdandi::legacy::result<proxy_selector> created_selector =
    proxy_selector::create(domain, "Proxy");
proxy_selector selector = std::move(*created_selector);

verdandi::legacy::result<
    verdandi::legacy::optional<
        verdandi::legacy::candidate<proxy_attr, proxy_data> > > selected =
    selector.one(least_power());
```

Policy exceptions are caught before the C callback returns and converted to a
stable error. Borrowed candidate objects must not escape the callback. Local
mutation commits only when the policy and C ABI transaction both succeed.
`snapshot()` performs the documented heavy detached copy.

Compared with the native C++23 API, the Legacy facade decodes typed candidates
from C Fields at each callback/result boundary. The native API can reuse cached
typed projections and is preferred for the lowest Selector hot-path overhead.

## Catalog

```cpp
verdandi::legacy::result<verdandi::legacy::catalog_client> catalog_result =
    verdandi::legacy::catalog_client::open(root);
verdandi::legacy::catalog_client catalog = std::move(*catalog_result);

verdandi::legacy::result<verdandi::legacy::catalog_publisher> publisher_result =
    verdandi::legacy::catalog_publisher::create(catalog);
verdandi::legacy::catalog_publisher publisher =
    std::move(*publisher_result);

verdandi::legacy::catalog_path path("routing", "primary");
publisher.replace(path, verdandi::legacy::catalog_kind::map, data);

verdandi::legacy::catalog_subscription scope;
scope.parts.push_back("routing");
verdandi::legacy::result<verdandi::legacy::catalog_subscriber> subscriber_result =
    verdandi::legacy::catalog_subscriber::create(catalog, scope);
verdandi::legacy::catalog_subscriber subscriber =
    std::move(*subscriber_result);

verdandi::legacy::result<
    verdandi::legacy::optional<verdandi::legacy::catalog_entry> > entry_result =
    subscriber.find(path);
verdandi::legacy::catalog_entry entry = std::move(**entry_result);
verdandi::legacy::result<
    verdandi::legacy::catalog_snapshot<proxy_data> > snapshot =
    entry.load<proxy_data>();
```

Publisher remains task-free. Subscriber listener, transient synchronization,
repair, and checkpoint behavior remain in the native core. A Legacy Entry
retains its Subscriber state and loads one atomic immutable Fields snapshot.

## Deliberate limits

- The stable binary contract is C ABI v1, not the C++ class layout of this
  header-only facade.
- The facade owns no Redis driver, retries, event queues, background tasks,
  clocks, synchronization, repair, or checkpoint state.
- Fields and detached typed results necessarily copy data across the C ABI.
  The facade favors safe ownership and old-standard usability over matching
  every native C++23 hot-path optimization.
- `result<T>` and `optional<T>` are intentionally small C++11 substitutes, not
  complete implementations of C++23 `std::expected` or C++17 `std::optional`.
  Accessing the inactive branch is a caller contract violation. A value-bearing
  `result<T>` is copy/move constructible but deliberately non-assignable because
  C++11 cannot safely switch an inline Union between arbitrary throwing user
  types; construct the next result as a new value instead.
- Windows MSVC static and shared offline builds now pass for C++11/14/17. The
  shared C++23/C ABI core also has live Windows private-CA Sentinel TLS evidence;
  the Legacy facade itself remains offline-qualified. Linux Clang,
  install/export packages, and automated ABI compatibility remain release
  gates. macOS is unsupported.

Direct C ownership and binary-evolution rules remain documented in
[`C_ABI.md`](C_ABI.md).
