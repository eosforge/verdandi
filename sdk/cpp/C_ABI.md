# Verdandi C ABI v1

## Purpose

Verdandi has one C++23 implementation of Redis transport, Registration,
Selector, Catalog, recovery, checkpointing, and lifecycle state machines. The
C ABI is a second language boundary over that same compiled implementation; it
is not a second C++11/14/17 SDK and does not duplicate protocol logic.

The boundary supports three use cases:

- C11 applications;
- C++11, C++14, and C++17 applications that call the C surface directly or use
  the header-only `verdandi::legacy` RAII/typed facade;
- language bindings that can consume a conventional C ABI; the implemented C#
  managed facade is the first such binding.

`VERDANDI_C_ABI_VERSION` and `verdandi_c_abi_version()` currently report `1`.
No binary package has been released, so this is an implemented Alpha ABI rather
than a published compatibility promise.

## Source-build model

A source build still needs a toolchain capable of compiling the Verdandi core
as C++23. The application target itself may remain C11 or C++11/14/17. One
modern compiler can compile different targets with different language modes;
the `verdandi::c` target deliberately does not propagate `cxx_std_23` to its
consumer.

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
target_link_libraries(example PRIVATE verdandi::c)
```

An idiomatic lower-standard C++ application instead links
`verdandi::legacy` and includes `<verdandi/legacy.hpp>`. That header-only layer
still forwards every operation through this ABI and owns no protocol state.
Its complete API is documented in [`LEGACY.md`](LEGACY.md).

For a C application, set `C_STANDARD 11` and link the same `verdandi::c`
target. CMake selects the required final linker and C++ runtime from the
Verdandi dependency even though the application translation unit is C.

The default build is static. `-DBUILD_SHARED_LIBS=ON` builds the same runtime as
a shared library and enables the Windows import/export annotations. A consumer
that links a shared build without the CMake target must define
`VERDANDI_C_SHARED` while compiling its own translation units. There is no
separate DLL for each C++ language standard.

An actually old compiler that cannot compile C++23 cannot build the core from
source. Such an environment must consume a prebuilt shared Verdandi runtime for
its platform and use the same C headers. Static manual linking outside CMake
must also include the C++ runtime and Verdandi's private link dependencies.

## Public surface

Include the complete boundary with:

```c
#include <verdandi/c/verdandi.h>
```

The modules are:

- `types.h`: string/byte/Fields views, owned results, errors, ABI version, and
  runtime capability query;
- `configuration.h`: connection-free strict JSON validation;
- `client.h`: strict JSON configuration, root Client, and raw Key/Hash access;
- `registration.h`: Registration domain and delayed publish/update lifecycle;
- `selector.h`: borrowed transactional candidates, One/Any, detached results,
  retained snapshots, and diagnostics;
- `catalog.h`: Catalog domain, Publisher, Subscriber, stable Entry, and Fields
  loading.

Configuration enters as the canonical strict v1 JSON document. The C ABI does
not maintain another configuration structure or another default/range table;
the C++23 implementation parses JSON and constructs the same native validated
configuration used by the native API.

`verdandi_configuration_validate_json` performs the complete version, shape,
range, topology, TLS-relationship, Registration, Selector, and Catalog checks
without reading certificate files, opening a checkpoint, connecting to Redis,
or allocating an opaque handle. It is also the single validator used by the
C# `Configuration.Validate` facade.

Attr, Data, and Catalog records cross the boundary as flattened binary
`verdandi_fields_view` values. Verdandi copies input before returning. The C
caller owns its application structures and scalar codecs; Redis never parses a
JSON record value.

## Runtime capabilities

`verdandi_c_has_capability` performs an allocation-free exact lookup against
the loaded native runtime. Unknown and empty names return zero. A nonzero result
means that code path exists; it does not probe Redis, certificates, ACLs,
network reachability, or deployment correctness.

The current stable names are:

- `catalog`;
- `client`;
- `configuration.json`;
- `redis.commands`;
- `redis.sentinel_tls`;
- `registration`;
- `selector`.

Names are strings so additive modules do not consume numeric enum values or
change public structure layouts. This is local library feature detection, not
Redis wire-protocol capability negotiation. Bindings should treat an unknown
name as unsupported and a missing function symbol as an incompatible older
runtime.

## Ownership and lifetimes

Opaque handles are allocated and destroyed by the same Verdandi runtime:

- every successful `open`, `create`, `load`, or detached-result operation that
  returns a handle has one matching `*_release` function;
- `*_close` performs deterministic lifecycle shutdown and reports failure;
  `*_release` performs best-effort close where applicable and then frees the
  handle;
- Selector, Registration, Catalog, and Entry handles must be released before
  their owning domain/root handles;
- a `verdandi_string_view`, `verdandi_bytes_view`, or callback field view never
  transfers ownership. Its declaring function documents whether it lasts for
  the call, callback, or owning handle lifetime;
- application callbacks are synchronous. They must not retain borrowed
  candidates, selections, metadata views, or field views after returning.

Owned `verdandi_blob`, `verdandi_field_set`, detached candidate list, and
Selector snapshot handles remain valid until their matching release call.

The C# facade maps every opaque allocation to a dedicated `SafeHandle` and
holds internal parent-handle references for release ordering. Its public API
does not expose these C handles or require application code to call a release
function. See [`../csharp/README.md`](../csharp/README.md).

## Result and callback rules

Fallible operations return nonzero on success and zero on failure. They reset
the supplied `verdandi_error` before work and return stable string categories
such as `invalid`, `contract`, `unavailable`, `deadline`, and `closed`.
`field`, `detail`, and optional authoritative `revision` are bounded owned
diagnostics. C++ exceptions, allocation failures, and native
`std::expected` errors cannot cross the ABI.

Selector policies receive borrowed candidates and a selection builder. A
policy returns nonzero to commit or zero to roll back. If it returns zero it
should fill the supplied `verdandi_error`; an empty callback error is converted
to `contract`. Local candidate mutations are committed only through a
successful Selector transaction.

The asynchronous `*_try_error` operations use two outputs: the function return
still reports whether the call itself succeeded, while `available` reports
whether the supplied error structure contains one drained diagnostic.

## ABI evolution

The following rules apply to v1:

- no STL type, exception, template, application struct, driver type, or
  allocator ownership crosses the boundary;
- exported operation and status names are strings, not numeric protocol enums;
- opaque handle layout remains private;
- existing public structure layout and existing function signatures cannot be
  changed within ABI v1;
- new functions or new opaque types may be added without changing existing
  layouts;
- a required public-structure or calling-convention change requires a new ABI
  version and an explicit compatibility plan.

Current qualification covers strict GCC static and shared builds, C11 and
C++11/14/17 consumers, runtime capability queries, exported Linux shared-
library symbols, Redis 8.8 live Registration/Selector/Catalog behavior,
private-CA Sentinel TLS, and ASan/UBSan/leak checks. Windows MSVC static Debug
and shared Release builds, C11/C++11 consumers, DLL exports, and .NET 8/10
loading also pass offline. The Windows DLL additionally passes live private-CA
Sentinel TLS directly through C++23 and the complete two-promotion matrix
through C# net8.0/net10.0. Linux Clang, install/export packages, and an
automated binary-ABI checker remain release gates. Automated package production
is intentionally deferred. macOS is not a supported target.
