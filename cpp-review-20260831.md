# C++23, C ABI, and C++11 Facade Implementation Review — 2026-08-31

## 1. Scope reviewed

This review covers the first C++23 implementation of Verdandi's existing
language-neutral Alpha contract:

- root Redis Client, strict v1 JSON configuration, binary Key/Hash operations;
- Registration and Selector;
- Catalog Publisher, Subscriber, stable Entry, and SQLite checkpoint;
- C ABI v1 for C11 and C++11/14/17 consumers, plus the header-only C++11
  Legacy facade;
- generated Registration and Catalog Lua integration;
- Standalone and Sentinel transport paths.

Leader, desired state, acknowledgements, Redis Cluster, and release packaging
are outside this implementation. No commit, tag, package, or push was created.

The physical C/C++ inventory contains 50 production header/source/template
files with 14,537 lines and thirteen test translation units with 1,325 lines. The C
ABI accounts for 2,086 production lines; the header-only Legacy facade adds
2,454 lines of RAII, result, codec, and typed forwarding. Both wrap rather than
duplicate native state machines. The earlier compile-time JSON and Selector
refactors remain intact. This is one compiled implementation: templates remain
at strong-type encode/decode and policy-callback boundaries instead of
duplicating Redis state machines for each application type or language
standard.

## 2. Architecture result

### Root and configuration

The public `verdandi::client` is a small shared owning handle. Boost.Redis,
Boost.Asio, OpenSSL, yyjson, and SQLite types do not cross public headers. One
private driver owns the connection pool, reconnect state, subscription
connections, and I/O reactor. Ordinary operations return `std::expected`-based
results; driver, allocation, and application-codec exceptions are translated at
Verdandi boundaries.

The C++ native configuration structures reproduce the canonical v1 JSON
defaults, ranges, optional-domain semantics, topology rules, and one-MiB input
bound. Standalone, plain Sentinel, ACLs, and Standalone TLS are represented.
Cluster is rejected. Sentinel plus TLS is rejected explicitly rather than
silently weakening hostname verification for Sentinel-discovered nodes.

Strict JSON object dispatch now uses literal field names as non-type template
parameters and a variadic short-circuit fold. Concrete callbacks remain visible
to the optimizer, each binding owns its duplicate-presence bit, and the parser
allocates no generic name-to-callback table or duplicate-name vector. Required
flags are committed only after successful conversion.

### Registration and Selector

One published Registration owns one long-lived worker, one capacity-one wake
signal, one coalescing `Fields` mailbox, one renewal timer, and one
desired/confirmed state. Typed inputs are synchronously encoded before entering
the mailbox, so pending work never borrows application structs. Full recovery
uses the process UUID and desired content already owned by the Registration.

One Selector owns one persistent listener/state-machine task. Initial full
synchronization and targeted repair share exactly one temporary task slot. A
request arriving while that task is running is coalesced into its pending work;
the worker drains the latest requested work and exits when idle. Therefore the
steady-state topology is one task and the synchronizing topology is at most two.
Half-synchronized state is explicitly unavailable.

Typed `one` and `any` execute an injected policy synchronously under one bounded
transaction, expose borrowed immutable candidates, accept opaque Choices, and
commit staged local Data predictions only on a successful selection. Remote
field updates reconcile the prediction rather than treating it as a distributed
reservation.

The typed Selector boundary caches Attr/Data projection in each immutable
record. Two plain projector function pointers replace `std::function`; common
copy-constructible values detach and begin mutation directly from those cached
objects, while non-copyable types retain a decode fallback. `any` uses a
generation-tagged reusable mark vector instead of allocating `vector<bool>` per
transaction. Schema traversal and callback/codec exception translation are
single inline higher-order helpers expanded for each concrete type.

### Catalog

Publisher is task-free. Each Subscriber owns one persistent multi-channel
Pub/Sub listener and at most one temporary full/scope synchronization and
repair task. The temporary task is not retained in steady state. It coalesces
requests, performs authoritative Hash/ZSET/Read alignment, waits for the
subscribed PING/PONG fence, rechecks metadata, publishes immutable Entry state,
and then exits.

Catalog notifications use the existing bounded MessagePack wire contract.
Stable Entry identity and atomic immutable state allow typed `load<T>()` without
Redis or disk I/O. The SQLite checkpoint is a transactional monotonic restart
accelerator namespaced by the SHA-256 digest of normalized coverage. Redis is
authoritative; a persistence failure is diagnostic and disables unsafe further
checkpoint advancement for that Client generation.

### C ABI v1

The C boundary uses opaque handles over the same native Client, Registration,
Selector, and Catalog implementations. Strict v1 JSON enters once and is
converted into native configuration; flattened binary Fields cross value
boundaries. Borrowed string/byte views and synchronous policy callbacks are
paired with explicit owned Blob, field-set, candidate-list, snapshot, and
release operations.

All fallible operations convert native results and exceptions into a nonzero
success/zero failure convention with bounded owned string errors. No STL type,
exception, template, application Attr/Data, Redis driver type, or allocator
ownership crosses the ABI.

The header-only `verdandi::legacy` target now supplies the missing C++11
convenience layer without adding another runtime. It owns RAII handles,
ancestor lifetime, results/options, chrono durations, Fields/schema codecs,
and typed domain forwarding only. Every operation enters C ABI v1; no driver,
worker, Pub/Sub listener, clock, retry, recovery, or checkpoint logic is
duplicated. Its class/template layout is source-only while C ABI v1 remains the
stable binary boundary.

## 3. Dependency and build policy

- C++23 is the sole implementation baseline; C ABI v1 is the lower-standard
  source and binary consumption boundary.
- Boost.Redis 1.92 provides the Redis/Asio transport.
- yyjson 0.12 provides strict bounded JSON parsing.
- SQLite 3.37 or newer is accepted; the locked fallback is SQLite 3.53.4.
- OpenSSL supplies TLS and SHA-256.
- CMake fetches reviewed revisions only when compatible installed targets are
  absent and dependency fetching is enabled.
- Public headers contain no third-party types.
- Project-owned code builds with strict warnings as errors.
- `verdandi::verdandi` propagates C++23. `verdandi::c` links the same runtime
  through `LINK_ONLY`, so C11 and C++11/14/17 consumers retain their own
  language mode. Building from source still requires a C++23-capable toolchain
  for the core.
- `verdandi::legacy` is header-only, propagates only C++11, and links
  `verdandi::c`. C++11/14/17 consumers share one source facade and one runtime.
- Static and shared runtime builds are supported. Public C layout/signature
  changes require a new ABI version; additive functions and opaque types may be
  introduced within v1.

## 4. Verification evidence

The accepted short verification used the current source after the final
subscription/lifecycle corrections, C++23 source-expansion cleanup, and C ABI
implementation. Per the maintainer request, no long-duration test was run:

- GCC Debug configure/build with strict warnings: passed;
- C++ unit, C11 ABI, and strict-warning C++11/14/17 Legacy consumer tests:
  passed;
- every Legacy component and umbrella header as an independent strict C++11
  translation unit: passed;
- shared GCC build and 88 exported C symbols: passed;
- shared GCC Release build with optimization and warnings as errors: passed;
- authenticated Redis 8.8 Standalone root, Registration, Selector, Catalog, and
  checkpoint integration: passed;
- clang-format dry-run gate: passed;
- clang-tidy high-signal project-owned checks with warnings as errors: passed;
- GCC ASan/UBSan build, unit test, live Standalone integration, leak detection,
  and halt-on-error: passed;
- C ABI root, Registration, Selector, Catalog, and cleanup integration under
  both shared Debug and ASan/UBSan/leak builds: passed;
- C++11 Legacy root Key/Hash, typed Registration/Selector/Catalog, local
  prediction, diagnostics, and cleanup integration under static, shared, and
  ASan/UBSan/leak builds: passed;
- isolated ACL-protected three-data-node/three-Sentinel C++ shared-Release
  root/Registration/Selector/Catalog/checkpoint smoke: passed in 3.780 seconds
  on Redis 8.8.0;
- Sentinel smoke cleanup: test database empty and no labeled test container
  remained;
- Registration and Catalog generated-Lua identity checks: passed.

The C# Release consumer exposed an optimization-only GCC
`maybe-uninitialized` diagnostic at two Catalog optional-shape extraction
sites. Presence validation and concrete enum extraction are now separate; no
wire or runtime behavior changed. The corrected source passed C++ Debug,
shared Release, ASan/UBSan, format, clang-tidy, and the live C++-owned smoke
above. Its structured result is
[`testkit/results/cpp-shared-release-regression-20260831.json`](testkit/results/cpp-shared-release-regression-20260831.json).
Per the independently executable language-gate decision, this C++ acceptance
does not depend on rerunning Go, Rust, or C# in the same command.

## 5. Scores

| Area | Score | Main deductions |
| --- | ---: | --- |
| Root transport/configuration | 9.0/10 | no C++ two-promotion Sentinel campaign; no live TLS matrix; Sentinel+TLS intentionally rejected |
| Registration | 9.2/10 | short integration only; no C++ soak or dedicated throughput benchmark |
| Selector | 9.4/10 | allocation sources were removed structurally, but policy latency/allocation baselines and broader reconnect storms remain unqualified |
| Catalog | 9.2/10 | no two-promotion C++ failover; pattern subscriptions require correctly provisioned Redis channel ACL patterns |
| Strong-type API/schema | 9.5/10 | custom scalar codecs still require explicit specializations; no package-level compatibility matrix yet |
| C ABI and lower-standard boundary | 9.4/10 | Legacy usability is complete, but no Windows DLL/MSVC or automated ABI/install matrix exists |
| Tests and release engineering | 9.0/10 | Linux GCC only; no MSVC/Clang/macOS matrix, install/export package, current long soak, or fuzz campaign |
| **Overall current C++ SDK** | **9.3/10** | production-shaped native and C ABI Alpha implementation, not release-qualified |

## 6. Strengths

- One compiled protocol core keeps template expansion and binary size bounded as
  more application Attr/Data types are introduced.
- C11 and C++11/14/17 reuse that core through a narrow allocator-safe ABI;
  static/shared target tests prove the C++23 compile feature does not leak into
  consumer translation units.
- Lower-standard C++ callers no longer hand-manage opaque handles or callbacks:
  the header-only facade adds RAII, typed Fields, and language-appropriate
  policy callables while retaining one runtime.
- Public headers are narrow, language-native, and independent of Redis-driver
  types.
- Strong and raw Fields modes share the same APIs and state machines.
- Compile-time JSON and Schema expansion compress repetitive source while
  preserving direct branches, static callback types, and strict failure paths.
- Typed Selector records avoid repeat projection, normal `any` calls reuse
  duplicate marks, and application exceptions cross one consistent result
  boundary.
- `std::expected`, RAII, `std::jthread`, stop tokens, scoped locks, and immutable
  snapshots give explicit ownership and terminal behavior.
- Registration and both subscriber domains have deterministic, bounded task
  topologies and bounded diagnostic queues.
- Pub/Sub is never treated as a recovery log; every gap returns to authoritative
  Redis state.
- SQLite recovery is local, monotonic, transactional, optional, and incapable
  of overwriting Redis.
- Strict warnings, static analysis, sanitizers, Standalone integration, and an
  isolated Sentinel topology found real startup/error-path defects before this
  review.

## 7. Remaining weaknesses and recommended gates

1. Run the C++ Registration and Catalog two-promotion Sentinel matrices,
   including acknowledged-write loss, script flush, Sentinel minority/all-down,
   and post-promotion repair.
2. Build a live TLS matrix for Standalone private CA and mTLS. Revisit
   Sentinel+TLS only when the chosen driver can verify the discovered node
   against an explicit deployment identity without an insecure bypass.
3. Add GCC and Clang release builds on Linux, MSVC on Windows, and Apple Clang on
   macOS. Treat compiler-specific workarounds as private build details.
4. Add CMake install/export/version package files and downstream
   `find_package(verdandi CONFIG)` consumption tests.
5. Add dedicated Registration update/renew, Selector `one`/`any`, Catalog event,
   and checkpoint benchmarks before making performance claims.
6. Add bounded long soak, reconnect-storm, malformed-frame fuzz, and checkpoint
   fault-injection campaigns.
7. Add a Windows DLL/MSVC C ABI/Legacy matrix and automated symbol/layout
   compatibility check before publishing ABI v1. Keep the Legacy facade as a
   forwarding source layer instead of forking a second protocol runtime.

These are release-qualification deductions, not reasons to change the current
one-persistent-plus-one-temporary synchronization architecture.

## 8. Source-freeze addendum

The final Alpha source-freeze pass added three pieces of evidence without
changing the public API or Redis protocol:

- every project-owned C, C++11/14/17, and C++23 target now receives the same
  strict warning-as-error policy; the complete Debug, shared Release, and
  sanitizer trees pass it;
- C++ now consumes the shared strict-JSON configuration corpus. Unknown and
  duplicate members report the same stable `json` field as Go/Rust, while exact
  value/range/relationship diagnostics remain unchanged; and
- a single-field Schema no longer declares an otherwise unused compile-time
  tuple count. The index sequence expands directly from the tuple type and has
  no runtime effect.

The current direct C++23 root/Registration/Selector/Catalog/SQLite Sentinel
smoke is
[`freeze-cpp-sentinel-20260831.json`](testkit/results/freeze-cpp-sentinel-20260831.json)
and passed in 3.418 seconds. The identical compiled core additionally survived
acknowledged-write loss, total Sentinel loss, and two promotions through C ABI
v1 in the .NET 8/10 matrix recorded by
[`freeze-csharp-sentinel-20260831.json`](testkit/results/freeze-csharp-sentinel-20260831.json).
This is strong shared-runtime evidence, but it does not replace a future direct
native C++ peer that remains alive through the same two promotions.

The overall C++ score remains **9.3/10**. Strictness and cross-language
configuration consistency improved, while the remaining score deductions are
still a direct native two-promotion campaign, TLS, MSVC/Clang/macOS, install and
package export, automated ABI compatibility, native fuzzing, dedicated
performance baselines, and bounded C++-owned endurance.
