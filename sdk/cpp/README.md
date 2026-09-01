# Verdandi C++23 SDK

## Status

This directory contains the C++23 implementation of the Verdandi `0.1.0`
non-production Alpha Registration, Selector, and Catalog surfaces. Stable
`1.0.0` remains reserved for the complete contract, including Leader. It is one compiled SDK,
not a second header-only protocol implementation: templates are limited to the
typed `Fields` boundary, while Redis transport, Lua dispatch, synchronization,
checkpointing, and lifecycle state machines are compiled once.

The current implementation supports Redis 8 Standalone, plain Sentinel, ACLs,
Standalone TLS, strict v1 JSON configuration, raw Key/Hash commands,
Registration, Selector, Catalog Publisher/Subscriber, and optional SQLite
Catalog checkpoints. Redis Cluster is rejected. Sentinel plus TLS is rejected
because the current Boost.Redis discovery path cannot preserve hostname
verification when Sentinel returns a dynamic data-node address.

No C++ package or stable wire protocol has been released. The implemented
Alpha C ABI v1 exposes the same compiled core to C11 and C++11/14/17 callers.
A header-only `verdandi::legacy` facade adds C++11 RAII, results, durations,
typed Fields, and domain APIs over that ABI; it owns no second transport,
protocol state machine, or per-standard runtime.

## Requirements and dependencies

- CMake 3.28 or newer;
- a C++23 compiler and Ninja for the supplied presets;
- OpenSSL;
- Boost.Redis 1.92 or newer;
- yyjson 0.12 or newer;
- SQLite 3.37 or newer.

When compatible Boost.Redis, yyjson, or SQLite targets are unavailable and
`VERDANDI_FETCH_DEPENDENCIES=ON`, CMake fetches the reviewed dependency
revisions into the active build tree's `_deps` directory. Dependency build
artifacts remain isolated between Debug/Release, static/shared, and sanitizer
trees. Public headers expose only the C++ standard library and Verdandi types;
third-party types remain private.

## Build and verification

From `sdk/cpp`:

```text
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug --output-on-failure

cmake --preset gcc-asan-ubsan
cmake --build --preset gcc-asan-ubsan
ctest --preset gcc-asan-ubsan --output-on-failure

cmake --preset gcc-shared-debug
cmake --build --preset gcc-shared-debug
ctest --preset gcc-shared-debug --output-on-failure

cmake --preset gcc-shared-release
cmake --build --preset gcc-shared-release
ctest --preset gcc-shared-release --output-on-failure
```

The shared Release preset is the native runtime consumed by the C# Linux x64
qualification. It remains a separate C++ build/test gate so optimization-only
compiler diagnostics cannot be hidden by a managed-binding pass.

The live integration executable skips with code 77 unless either
`VERDANDI_REDIS_ADDRESS` or `VERDANDI_SENTINEL_ADDRS` is set. The isolated
three-node/three-Sentinel smoke harness is
`testkit/cpp/sentinel_smoke.py`; its SSH password is read from
`VERDANDI_TEST_SSH_PASSWORD`, never from source.

When the tools are installed, the build also exposes
`verdandi_cpp_format_check` and `verdandi_cpp_clang_tidy`. Project-owned code is
compiled with warnings as errors. The sanitizer preset enables AddressSanitizer,
UndefinedBehaviorSanitizer, and leak detection through the test preset.

## C and lower-standard C++ consumers

The native `verdandi::verdandi` target requires C++23. The `verdandi::c` target
exposes the opaque C ABI without propagating that language requirement to the
consumer target. The header-only `verdandi::legacy` target links that C target
and exposes an idiomatic C++11 facade. C11 and C++11/14/17 compile/link tests
exercise static and shared runtime builds; a C++11 Redis integration test also
exercises typed Registration, Selector, and Catalog behavior.

The Verdandi source build itself still requires a C++23-capable toolchain: the
core target is compiled as C++23 while the consuming target is compiled in its
own lower language mode. A compiler that cannot compile the core must use a
prebuilt runtime. Configuration crosses this boundary as canonical strict v1
JSON, while Registration and Catalog values remain raw binary Fields.

The C++11 facade API, ownership, schema, and performance boundaries are in
[`LEGACY.md`](LEGACY.md). Direct C ownership, callback, error, and ABI evolution
rules are in [`C_ABI.md`](C_ABI.md).

## Root Client and configuration

```cpp
#include <verdandi/client.hpp>

verdandi::redis_configuration redis;
redis.mode = verdandi::redis_mode::standalone;
redis.addresses = {"127.0.0.1:6379"};
redis.auth = {.username = "verdandi", .password = "secret"};

auto transport = verdandi::client::open(redis);
if (!transport) {
    return transport.error();
}

auto ping = transport->ping();
auto stored = transport->key().set("application:key", std::int64_t{42});
auto loaded = transport->key().get<std::int64_t>("application:key");
```

`verdandi::configuration::from_json` and `load_json` parse the canonical v1
configuration into language-native `redis_configuration`,
`registration_configuration`, and `catalog_configuration` values. Every
configuration structure owns a `check()` method with the same defaults, ranges,
zero semantics, and relationship checks documented in
[`../../configuration.md`](../../configuration.md).

The JSON implementation binds literal member names as C++23 non-type template
parameters. A variadic short-circuit fold expands the strict dispatch table at
compile time, while each concrete binding records duplicate presence without a
dynamic lookup container. This is source-level expansion, not generated source:
unknown and duplicate members remain rejected, required-member state is marked
only after successful decoding, and no callback type erasure is introduced.

The root Client is a small shared handle over one private connection pool and
I/O reactor. Copies share transport ownership. `close()` is terminal and
idempotent; it does not delete application data or wait for independently owned
domain Clients. Normal failures use `verdandi::result<T>`, an alias based on
`std::expected`, rather than exposing Boost exceptions.

## Strongly typed Fields

Applications declare top-level Attr/Data structures directly. `consteval`
descriptors, concepts, and fold expressions validate and expand the schema at
compile time; there is no runtime reflection or SDK code generator.

```cpp
struct proxy_attr {
    std::string region;
};

struct proxy_data {
    std::int64_t power{};
    bool ready{};
};

VERDANDI_SCHEMA(proxy_attr,
                VERDANDI_FIELD(proxy_attr, region));
VERDANDI_SCHEMA(proxy_data,
                VERDANDI_FIELD(proxy_data, power),
                VERDANDI_NAMED_FIELD(proxy_data, ready, "available"));
```

Each member type uses `verdandi::field_codec<T>`. The SDK supplies the standard
scalar codecs; applications may specialize the codec for their own scalar
types. `verdandi::fields` implements the same `structured_value` contract for
raw binary use without a second Registration, Selector, or Catalog API.

## Registration

```cpp
#include <verdandi/registration/registration.hpp>

verdandi::registration_configuration config;
config.zone = "Production";

auto domain = verdandi::registration::client::open(*transport, config);
auto registration =
    verdandi::registration::registration<proxy_attr, proxy_data>::create(
        *domain,
        {.type = "Proxy", .ttl = std::chrono::seconds{15}, .version = 1});

// Construction is local. Publication waits until the process is ready.
registration->publish(proxy_attr{"cn-east"}, proxy_data{0, true});
registration->update(proxy_data{1, true});
registration->set_version(2);
registration->renew();
registration->close();
```

Each published Registration owns one worker, one coalescing `Fields` mailbox,
one renewal timer, and one desired/confirmed state. Update requests admitted
before a wake are merged by field; a confirmed Update resets the next renewal
deadline. `renew()` changes only liveness. Ambiguous or lost state is recovered
with the same process UUID and complete desired content. `try_error()` drains
bounded asynchronous diagnostics without blocking.

## Selector

```cpp
#include <verdandi/registration/selector.hpp>

auto selector =
    verdandi::registration::selector<proxy_attr, proxy_data>::create(
        *domain,
        {.type = "Proxy"});

auto selected = (*selector)->one(
    [](auto& candidates)
        -> verdandi::result<
            std::optional<verdandi::registration::choice>> {
        std::optional<verdandi::registration::choice> best;
        std::int64_t power = std::numeric_limits<std::int64_t>::max();
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            auto value = candidates.get(index);
            if (value && value->data().ready && value->data().power < power) {
                power = value->data().power;
                best = value->identity();
            }
        }
        if (best) {
            auto changed = candidates.mutate(
                *best,
                [](proxy_data& data) { ++data.power; });
            if (!changed) {
                return std::unexpected(changed.error());
            }
        }
        return best;
    });
```

`one` and `any` run the injected policy synchronously on the caller thread
under one bounded Selector transaction. The callback borrows immutable
candidates, may stage local Data predictions through `mutate`, and returns
opaque `choice` values. Successful remote updates reconcile only the fields
they own. `snapshot()` is an explicit heavy detached copy.

Each Selector owns exactly one persistent Pub/Sub/state-machine task. Initial
full synchronization and later targeted repair share one temporary task slot;
there are at most two Selector tasks while synchronizing and only one in steady
state. A half-synchronized view returns `unavailable` and retained entries are
never selectable.

Selector strong-type projection is performed once when an immutable Redis
record enters the published view. Projectors are plain compile-time-selected
function pointers rather than `std::function`. For copy-constructible Attr/Data,
selection detachment and local `mutate` begin from the cached typed objects;
non-copyable values retain the decode fallback. `any` reuses generation-tagged
duplicate marks, so the ordinary transaction does not allocate a fresh bitmap.
Application callbacks and codecs share one inline exception-to-result boundary.

## Catalog

```cpp
#include <verdandi/catalog/catalog.hpp>

verdandi::catalog_configuration catalog_config;
catalog_config.zone = "Production";
catalog_config.local_store_path = "catalog.sqlite3"; // Optional.

auto catalog = verdandi::catalog::client::open(*transport, catalog_config);
auto publisher = verdandi::catalog::publisher::create(*catalog);
auto target = verdandi::catalog::path::create("routing", "primary");

auto replaced = publisher->replace(
    *target,
    verdandi::catalog::kind::map,
    proxy_data{10, true});

verdandi::catalog::subscription scope;
scope.parts.emplace_back("routing");
auto subscriber = verdandi::catalog::subscriber::create(*catalog, scope);
auto entry = (*subscriber)->find(*target);
auto snapshot = entry->load<proxy_data>();

verdandi::catalog::patch change;
change.base_revision = replaced->revision;
change.set.emplace(
    "power",
    *verdandi::field_codec<std::int64_t>::encode(11));
publisher->apply(*target, std::move(change));
publisher->erase(*target);
```

Publisher is a task-free view. Each Subscriber owns one persistent Pub/Sub
listener and at most one temporary authoritative synchronization/repair task.
The temporary task is created only while work exists, coalesces pending
full/scope repair requests, and exits after the queue is drained. Subscription
creation returns only after subscribe acknowledgement, Redis alignment, an
ordered subscribed PING/PONG fence, and metadata recheck.

Stable Entries survive delete/recreate and publish immutable atomic states.
SQLite persistence is a monotonic, transactional restart accelerator keyed by
the normalized subscription scope; Redis remains authoritative and checkpoint
failure does not stop in-memory recovery.

## Current qualification limits

The optimized source currently passes strict GCC compilation, unit and live
Redis 8.8 Standalone integration, clang-tidy, and ASan/UBSan/leak checks. The
initial implementation checkpoint additionally passed an isolated
ACL-protected three-node/three-Sentinel startup/integration smoke; the focused
source-expansion regression did not relabel that earlier smoke as a current
rerun. The following release gates remain open:

- two consecutive C++ Sentinel promotions with acknowledged-write-loss repair;
- a live TLS topology, including private CA and mTLS cases;
- MSVC, Clang, and macOS build matrices;
- long soak and dedicated C++ performance/regression benchmarks;
- CMake install/export/package artifacts;
- Windows DLL/MSVC, Clang, and macOS qualification of C ABI v1;
- an automated binary-ABI compatibility gate.

The complete evidence and scoring are recorded in
[`../../cpp-review-20260831.md`](../../cpp-review-20260831.md) and
[`../../test-results.md`](../../test-results.md).
