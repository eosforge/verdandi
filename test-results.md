# Verdandi Go, Rust, C++, and C# Register/Selector and Catalog Test Results

## 1. Scope and final result

Initial qualification date: 2026-09-01

Last updated: 2026-09-03

Scope update, 2026-09-02: generic Campaign/Leader election was withdrawn from
all release targets. Historical references below describe the scope at their
recorded checkpoint only; Redis Sentinel primary failover remains supported.

Scope update, 2026-09-03: the complete Alpha tree was re-audited, Go parser hot
paths were measurably optimized, and the Windows PowerShell 5.1 OpenSSL probe
fallback was corrected. The current source tree completed the local short
matrix below; the remote Redis host was unavailable before fixture creation,
so no current-tree live or endurance qualification is claimed.

The implemented Go, Rust, and C++23 Registration/Selector and Catalog slices,
plus the C# facade over C ABI v1, have the evidence boundaries recorded below.
Go and Rust have prior
unit, static, race, fuzz, authenticated Redis 8.8 integration, cross-language,
sustained-load, scale-recovery, and Redis Sentinel evidence recorded below. The
2026-08-28 thin shared-transport boundary and Go 1.27 API revision have current
unit, static, Linux race, all-tag compile, peer-build, fuzz, and isolated Redis
8.8 coverage.
The 2026-08-28 Fields-mailbox/configuration revision passed the recorded
one-hour owned campaign. The newer 2026-08-29 optimization fingerprint has its
own microbenchmark, full short Redis, race, interoperability, and Sentinel
evidence below; it does not inherit the earlier fingerprint's one-hour label.

### 2026-09-03 full-tree optimization and boundary regression

- The complete review and remaining-boundary analysis is
  [`optimization-review-20260903.md`](optimization-review-20260903.md); the
  machine-readable summary is
  [`optimization-regression-20260903.json`](testkit/results/optimization-regression-20260903.json).
- Go passed format, module verification, vet, all packages, ten random-order
  runs, complete WSL/Linux race, 100 repeated new boundary cases, and all
  benchmarks. Configuration, Catalog, and Registration fuzz targets each
  passed 15 seconds. Ordinary non-live statement coverage was 47.4%; Redis,
  Sentinel, load, and soak tests remain opt-in and are not represented by that
  percentage.
- Ten-sample Linux comparisons reduced 512-field Catalog Replace decode from
  85.87 to 27.20 microseconds (-68.33%), with bytes/op -10.36% and allocations
  -25%. Redis int64/uint64 decode improved 42.86%/45.11%, with bytes/op -75%
  and allocations -50%. Registration stored-record parsing improved 2.99% and
  removed two allocations.
- Rust stable passed fmt, 77 endpoint-independent tests, strict all-target/
  all-feature Clippy, and warning-denied rustdoc. Rust 1.85 passed all-target/
  all-feature check and the same endpoint-independent tests.
- Linux and Windows C++ shared Release build/test entry points, Linux GCC
  ASan/UBSan/leak, C++23, C ABI v1, C++11/14/17 Legacy, clang-format, and
  project-owned clang-tidy passed. Each native tree ran six endpoint-independent
  tests and skipped the three Redis URL tests by contract.
- C# .NET 8/10 format/analyzers/zero-warning Release and native-DLL offline
  tests passed. Both Lua generators, the Go reference generator, and in-memory
  syntax compilation of 17 Python testkit files passed.
- Windows PowerShell 5.1 and PowerShell 7.6 now both complete the real C++
  doctor with automatic vcpkg fallback when system OpenSSL is unavailable;
  the system-only expected-failure diagnostic also passed. Linux doctor passed
  with system OpenSSL.
- `192.168.0.90:22` and ICMP timed out. The Standalone harness stopped before
  creating an owned container or starting product tests. This is recorded as
  environment unavailable, not a test failure; prior live and frozen-source
results remain historical only.

### 2026-09-02 Windows/Linux native build-entry regression

- Added the normal `sdk/cpp/build.ps1` and `sdk/cpp/build.sh` entry points for
  C++23, C ABI, and C++11/14/17 Legacy. Go, Rust, and C# are explicitly outside
  this native build boundary. C# independently compiles and loads a shared
  DLL/SO from its application output.
- Windows actual doctor passed C++23 compile/link with Visual Studio 18 2026,
  MSVC toolset 14.51, CMake 4.4.2, existing vcpkg, and OpenSSL 3.6.0. Linux
  actual doctor passed GCC 13.3, CMake 4.3.2, Ninja, and system OpenSSL 3.0+
  compile/link. Neither path requires .NET.
- Fresh empty Release trees configured offline from checksum-verified caches on
  Windows and Linux. Full offline builds/tests passed, then online reuse of the
  same trees passed without dependency re-extraction. Corrupt/missing offline
  archives cannot fall through to the network.
- Final Windows Release tests passed six endpoint-independent native entries,
  with three expected endpoint-owned skips. Final Linux Release tests passed
  the same six native entries and three expected endpoint-owned skips. The
  final Windows shared result was separately loaded by the existing C#
  net8/net10 tests after managed orchestration was removed; C# is not a script
  stage.
- PowerShell 5.1/current parsing, detailed comment-based help, Bash syntax and
  help, check-profile dry runs, exact tool-path reporting, invalid explicit
  vcpkg roots, atomic environment-manifest writes, JSON parsing, line length,
  trailing whitespace, and script-owned ASCII/English runtime-string audits
  passed.
- Every script-owned console message is detailed standard English. Script
  source comments remain detailed Chinese for the current maintainer review.
  The machine result is
  [`native-build-entry-20260902.json`](testkit/results/native-build-entry-20260902.json).
  No commit or push was performed.

### 2026-09-02 generated Go Selector reference API

- Added the optional callback-only `ReferenceSelector` and deterministic
  `verdandi-refgen` facade while preserving the ordinary detached APIs.
- Generation/check, vet, ten shuffled all-package runs, complete WSL/Linux
  race, 100 shuffled targeted race repetitions, mutable-ownership/rollback/
  panic/concurrency tests, and generated public-API compilation passed.
- A random-Zone Redis 8.8 integration passed typed publication, discovery,
  local `Power++`, remote Update correction, joined cleanup, and final zero
  owned keys. It performed no global flush and no Catalog operation.
- Ten one-second Linux samples reduced selected-and-mutated 500-candidate `One`
  from a median 11.460 microseconds/28 allocations to 10.178 microseconds/four
  allocations. Eight-of-500 `Any` fell from 13.875 microseconds/97 allocations
  to 5.587 microseconds/zero allocations. Reference operations intentionally
  return no detached values, so this is an explicit contract tradeoff.
- Complete samples, source fingerprints, test matrix, discarded attempts, and
  release boundary are in
  [`go-selector-reference-20260902.json`](testkit/results/go-selector-reference-20260902.json).
- This is short-regression evidence, not inherited endurance qualification.

### 2026-09-01 complete working-tree optimization regression

- The detailed current-tree review is
  [`optimization-review-20260901.md`](optimization-review-20260901.md); its
  machine-readable gate summary is
  [`optimization-regression-20260901.json`](testkit/results/optimization-regression-20260901.json).
- Go passed format, vet, all packages, and WSL/Linux race. Rust passed format,
  all-feature strict Clippy, warning-denied rustdoc, 73 library tests, six
  offline external tests, and the declared Rust 1.85 check. C# passed format/
  analyzers and zero-warning net8/net10 Release builds.
- C++ passed clang-format, project clang-tidy, GCC Debug, GCC ASan/UBSan, MSVC
  Debug, and MSVC shared Release. Every tree accepted 9/9 CTest entries: six
  endpoint-independent tests passed and three Redis-URL tests skipped by
  contract; live endpoint behavior was exercised separately below.
- The isolated Redis 8.8 Standalone matrix passed Lua Registration/Catalog,
  Go, Linux race, every Rust live domain, root Redis APIs, and Go/Rust
  interoperability with 4,228 processed commands. Evidence:
  [`full-project-regression-20260901.json`](testkit/results/full-project-regression-20260901.json).
- The C#-owned Standalone matrix passed 11 build, analyzer, offline,
  self-contained, loader, ACL and full-domain suites in .NET 8/10. Evidence:
  [`csharp-standalone-full-review-20260901.json`](testkit/results/csharp-standalone-full-review-20260901.json).
- Direct C++ Windows/MSVC and Linux/GCC shared Release each passed private-CA
  Sentinel TLS Root/Registration/Selector/Catalog/checkpoint smoke and rejected
  the wrong identity. Evidence:
  [`cpp-sentinel-tls-windows-full-review-20260901.json`](testkit/results/cpp-sentinel-tls-windows-full-review-20260901.json)
  and
  [`cpp-sentinel-tls-linux-full-review-20260901.json`](testkit/results/cpp-sentinel-tls-linux-full-review-20260901.json).
- C# Windows/Linux net8/net10 and Go/Rust Linux peers passed the full private-CA
  two-promotion matrix, including acknowledged-write loss, `SCRIPT FLUSH`, all
  Sentinels unavailable, primary loss, recovery, UUID preservation where
  applicable, and Selector generations `1 -> 2 -> 3`. Evidence:
  [`csharp-sentinel-tls-windows-full-review-20260901.json`](testkit/results/csharp-sentinel-tls-windows-full-review-20260901.json),
  [`csharp-sentinel-tls-linux-full-review-20260901.json`](testkit/results/csharp-sentinel-tls-linux-full-review-20260901.json),
  and
  [`sentinel-tls-go-rust-linux-final-review-20260901.json`](testkit/results/sentinel-tls-go-rust-linux-final-review-20260901.json).
- Catalog Go/Rust completed `16381 -> 16382 -> 16383`, script-cache recovery,
  final revision 10, Delete, and zero keys. Evidence:
  [`catalog-sentinel-final-review-20260901.json`](testkit/results/catalog-sentinel-final-review-20260901.json).
- Three-run Linux Go measurements place 500-record Selector publication at
  47.056..47.914 us with six allocations, RedisClock reads at 21.76..21.88 ns
  with zero allocations, typed `One` at 11.641..11.774 us, and typed `Any(8)`
  at 12.593..12.693 us. Full ranges and allocation counts are in the review.
- This is a complete short regression, not a new endurance qualification. It
  cannot inherit the exact earlier frozen source's twelve-hour result.

### 2026-08-31 Alpha source-freeze short regression

- The complete final review, boundary matrix, controlled benchmark ranges, and
  remaining release gates are in
  [`freeze-20260831.md`](freeze-20260831.md). The commit containing that report
  is the immutable source used by the post-freeze twelve-hour campaign.
- Deterministic Registration/Catalog generator checks, Python testkit syntax,
  Go module verification/format/vet/unit tests, ten shuffled runs, WSL/Linux
  race, and two 60-second fuzz targets passed. Registration executed 12,950,664
  fuzz cases. Catalog executed 8,987,644 cases with a bounded per-input
  minimization budget; diagnostics proved the earlier flat execution counter
  was corpus minimization rather than a decoder stall.
- Rust current stable and the declared 1.85 minimum both passed all-target,
  all-feature tests and strict Clippy. Current stable formatting and
  warning-denied rustdoc also passed. The MSRV gate found one three-way
  deadline comparison that is now expressed with native `Ordering`.
- C++ passed strict GCC Debug and shared Release builds, C++23/C11/C++11/14/17
  tests, clang-format, project-owned clang-tidy, and ASan/UBSan/leak gates. The
  shared configuration corpus now runs in C++ and aligns duplicate/unknown JSON
  diagnostics with Go/Rust as `invalid/json`.
- C# .NET 8/10 passed zero-warning Release builds, offline tests, concurrent
  root disposal, forced finalizer cleanup, both Linux x64 native loader modes,
  Standalone Redis, and the two-promotion Sentinel matrix.
- The full isolated Redis 8.8 result is
  [`freeze-standalone-20260831.json`](testkit/results/freeze-standalone-20260831.json).
  Lua, Go, Linux race, Rust, Go/Rust interoperability, 500 live Registrations,
  eight Selectors, 60,000 updates per language at 500/s, independent renewal,
  and 5,000-record recovery all passed with final cleanup.
- Registration/Selector Sentinel evidence is
  [`freeze-sentinel-registration-20260831.json`](testkit/results/freeze-sentinel-registration-20260831.json):
  two promotions, acknowledged-write loss, `SCRIPT FLUSH`, Sentinel loss,
  same-UUID repair, and generations `1 -> 2 -> 3` passed in both languages.
- Catalog Sentinel evidence is
  [`freeze-sentinel-catalog-20260831.json`](testkit/results/freeze-sentinel-catalog-20260831.json):
  two promotions, final revision 10, final Delete, and zero keys passed.
- Direct C++ Sentinel evidence is
  [`freeze-cpp-sentinel-20260831.json`](testkit/results/freeze-cpp-sentinel-20260831.json).
  C# Standalone and Sentinel evidence are
  [`freeze-csharp-standalone-20260831.json`](testkit/results/freeze-csharp-standalone-20260831.json)
  and
  [`freeze-csharp-sentinel-20260831.json`](testkit/results/freeze-csharp-sentinel-20260831.json).
- The two twelve-hour workloads subsequently completed from the detached
  worktree and are recorded in the next section.

### 2026-09-01 exact frozen-source twelve-hour qualification

- Registration/Selector ran for 43,213.948 Redis seconds and 43,262.810 outer
  seconds. It completed 21,600,000 Updates, 3,707,005 selection transactions,
  145 expiry cycles, 155 churn cycles, and every one of 214 planned faults.
- Catalog ran for 43,201.858 Redis seconds and 43,261.458 outer seconds. It
  attempted 5,932,160 scheduled mutations and completed every one of 113 planned
  faults, with 5,931,934 accepted mutation-latency samples.
- Both domains recorded exactly 1,441 monotonic Redis JSONL samples, no sampling
  failure, no unexpected asynchronous error, no eviction or rejected
  connection, and no sustained memory-growth violation.
- All canonical Lua, Rust convergence, typed API, and Catalog interoperability
  post-checks passed. Independently recomputed source fingerprints matched the
  frozen commit.
- Both databases ended at `DBSIZE=0`. Remote ownership checks confirmed both
  containers and data directories were removed and ports 37380/37440 were
  closed.
- Machine evidence is retained in
  [`registration-soak-12h-freeze-20260901.json`](testkit/results/registration-soak-12h-freeze-20260901.json)
  and
  [`catalog-soak-12h-freeze-20260901.json`](testkit/results/catalog-soak-12h-freeze-20260901.json).

### 2026-09-01 fixed-identity Sentinel TLS and runtime capabilities

- The isolated Redis 8.8 fixture now has an explicit `--tls` mode. It generates
  one private CA and one leaf whose SAN contains only `verdandi.test`, not any
  announced node IP. Redis data ports, Sentinel control ports, and replication
  links all use TLS; Redis and Sentinel retain separate generated ACL users.
- Go and Rust each rejected `wrong.verdandi.test` on native Windows x64 and
  WSL/Linux x64, then their integration suites and persistent peers passed the
  full two-promotion matrix on both runtimes. Both UUIDs survived, both Selector
  generations advanced `1 -> 2 -> 3`, acknowledged-write loss was repaired,
  `SCRIPT FLUSH` recovered, all Sentinels could disappear before the primary,
  and each final fixture was removed. Machine evidence:
  [`sentinel-tls-go-rust-windows-20260901.json`](testkit/results/sentinel-tls-go-rust-windows-20260901.json)
  and
  [`sentinel-tls-go-rust-linux-20260901.json`](testkit/results/sentinel-tls-go-rust-linux-20260901.json).
- C++23 shared Release rejected the wrong certificate identity and then passed
  root Key/Hash, Registration, Selector, Catalog, and SQLite-checkpoint
  integration through three TLS Sentinels on Windows/MSVC and Linux/GCC. Both
  runs left `DBSIZE=0`; evidence:
  [`sentinel-tls-cpp-windows-20260901.json`](testkit/results/sentinel-tls-cpp-windows-20260901.json)
  and
  [`sentinel-tls-cpp-linux-20260901.json`](testkit/results/sentinel-tls-cpp-linux-20260901.json).
- C# self-contained Windows x64 and Linux x64 net8.0/net10.0 peers used the same
  platform-native C++ core and remained alive through two TLS promotions. Every
  Selector generation advanced `1 -> 2 -> 3`, every recovery scenario passed,
  and final `DBSIZE=0` was verified. Machine evidence:
  [`sentinel-tls-csharp-windows-20260901.json`](testkit/results/sentinel-tls-csharp-windows-20260901.json)
  and
  [`sentinel-tls-csharp-linux-20260901.json`](testkit/results/sentinel-tls-csharp-linux-20260901.json).
- C ABI v1, C++11 Legacy, and C# offline tests cover the new string capability
  query with known, unknown, empty, and invalid inputs. The GCC shared Release
  library exports 90 unique `verdandi_*` symbols including
  `verdandi_c_has_capability`.
- The three older result files without a platform suffix remain historical
  Linux evidence; the six platform-suffixed files above are authoritative for
  the current matrix. This is server-authenticated private-CA Sentinel TLS
  evidence. It does not claim live mutual TLS, direct C++23 two-promotion
  coverage, automatic native/NuGet RID packaging, or a TLS endurance campaign.

### 2026-09-01 Windows MSVC and native .NET boundary

- Visual Studio Community 2026 supplied MSVC 19.51, Windows SDK 26100, CMake
  4.4, Ninja, LLVM, and vcpkg. The existing vcpkg tree supplied OpenSSL 3.6.0;
  no toolchain or package was installed during qualification.
- C++ now compiles source and Chinese review comments explicitly as UTF-8,
  targets Windows 10 or newer, and remains under `/W4 /WX /permissive-`.
- MSVC x64 static Debug and shared Release builds both completed. Each CTest
  matrix accepted 9/9 tests: six offline tests passed and three endpoint-owned
  Redis tests skipped by contract. The DLL exports both
  `verdandi_c_abi_version` and `verdandi_c_has_capability`.
- Windows .NET 8 and .NET 10 directly loaded the generated Release DLL and
  passed the complete managed offline suite, including configuration
  conformance and runtime capability discovery.
- Linux GCC Debug, shared Release, ASan/UBSan, and format gates were rerun after
  the portability corrections and passed. Native Windows Redis/Sentinel TLS
  subsequently passed for Go, Rust, C++23, and C# as recorded above. Automated
  packaging remains open. macOS is intentionally unsupported rather than an
  unqualified release target. Machine evidence:
  [`windows-msvc-20260901.json`](testkit/results/windows-msvc-20260901.json).

### 2026-08-31 C# independent regression and C++ Release boundary

- Acceptance is language-local: C# owns independently executable offline,
  Standalone, and Sentinel gates. It does not require Go/Rust/C++ to run in the
  same command. Cross-language interoperability remains a separate gate, while
  an affected shared C++ core still receives its own regression.
- The C# Standalone result
  [`csharp-standalone-20260831.json`](testkit/results/csharp-standalone-20260831.json)
  passed in 26.801 seconds. It configured and built the GCC shared Release
  runtime, restored/formatted/analyzed/built .NET 8 and .NET 10 with zero
  warnings, ran both offline, published both as self-contained Linux x64, and
  exercised each against an ACL-protected Redis 8.8 fixture. The two managed
  runs used explicit-path and application-directory native loading and ended
  with an empty database and removed fixture.
- Offline coverage includes default/success/failure Result behavior, immutable
  Field ownership/order, strict UTF-8 and canonical scalar boundaries,
  duplicate/invalid names, typed and malformed codecs, Catalog subscription
  ownership, absent snapshots, and the x64 C ABI layout. Live coverage includes
  raw Key/Hash, delayed Registration lifecycle, exact 16 Attr/32 Data fields at
  128 bytes, over-limit rejection, six concurrent Registrations with concurrent
  Selector calls, One/Any rollback and prediction, stale/duplicate choices,
  exact 4 MiB Catalog acceptance and over-limit rejection, stale Patch repair,
  shape validation, decoder exceptions, and safe terminal errors after
  parent-first disposal.
- The C# Sentinel result
  [`csharp-sentinel-20260831.json`](testkit/results/csharp-sentinel-20260831.json)
  passed in 59.041 seconds against three Redis 8.8 nodes and three Sentinels
  with separate generated Redis/Sentinel ACLs. The .NET 8 and .NET 10 peers
  survived `16381 -> 16383 -> 16382`, repaired an acknowledged lost write,
  reloaded after `SCRIPT FLUSH`, remained usable with all Sentinels down,
  reported unavailable after the primary also disappeared, and recovered both
  Selector generations `1 -> 2 -> 3`. It unregistered both peers and verified
  final `DBSIZE=0` before removing the exact topology.
- Early C# second-promotion attempts stopped at Sentinel's
  `failover-abort-no-good-slave`: with a one-second `down-after`, waiting for
  the C++ transport-backed managed view to report unavailable made the sole
  surviving replica too old for selection. The C#-owned fixture now uses five
  seconds, preserving the unavailable-state assertion while giving its recovery
  candidate an approximately 50-second eligibility window. The unchanged
  Go/Rust control result
  [`sentinel-control-20260831.json`](testkit/results/sentinel-control-20260831.json)
  passed separately in 56.085 seconds; it diagnosed the fixture boundary and is
  not counted as C# acceptance.
- The C# Release build exposed GCC's optimization-only `maybe-uninitialized`
  diagnostic at two Catalog optional-shape extraction sites. Presence checking
  and concrete enum extraction are now separate. C++ then independently passed
  Debug, shared Release, and ASan/UBSan builds; each preset reported 9/9 CTest
  entries successful with three endpoint-dependent entries explicitly skipped
  in the local no-endpoint run. Clang-format and clang-tidy passed. The live
  shared-Release C++ root/Registration/Selector/Catalog/checkpoint Sentinel
  smoke passed in 3.780 seconds; evidence is
  [`cpp-shared-release-regression-20260831.json`](testkit/results/cpp-shared-release-regression-20260831.json).
- This earlier bounded result did not yet claim concurrent-disposal/finalizer
  coverage. The source-freeze regression above adds both gates. Windows/macOS,
  NuGet/RID packaging, NativeAOT/trimming, TLS, managed cross-language,
  performance, and endurance remain open. The current implemented managed-scope
  score remains **9.3/10**.

### 2026-08-29 Go/Rust configuration contract review

- Root Redis, Registration/Selector, and Catalog defaults, closed numeric
  bounds, exact-millisecond requirements, explicit-zero behavior, relationship
  checks, and exact invalid-field diagnostics now have focused tests in both
  SDKs.
- Catalog Path-lock competition has a positive total timeout: 30 seconds by
  default and configurable from 100 milliseconds through 1 hour. Root command
  timeout continues to bound each Redis attempt within that total budget.
- Go passed all-package tests and vet on Windows plus all-package race tests on
  Go 1.27 Linux/WSL. Rust passed formatting, 58 library tests, four
  endpoint-aware external tests, strict Clippy, and warning-denied rustdoc. All
  three Go peers built and all three Rust peers passed `cargo check`.
- Redis/Sentinel environment variables were intentionally not enabled for this
  focused short review, so endpoint-dependent cases exited or remained ignored;
  no new live-failover or long-duration claim is made for this source revision.
  Detailed evidence boundaries and scores are in
  [`configuration-review-20260829.md`](configuration-review-20260829.md).

### 2026-08-29 Typed Selector ownership and allocation optimization

- Current 104-file fingerprint:
  `2d3235af5a7a63049e4ba63c3a4fe2a933cd71ce829d753dbdfd9f1a89c8100b`.
  Public APIs, Redis/Lua protocol, configuration, and selection semantics are
  unchanged.
- Go now transfers the freshly re-encoded selected Attr/Data Fields directly
  into detached decoders, completes output decoding before commit, shares
  immutable overlay state, and clears transaction scratch/tails that could
  retain removed records. New rollback and stale-reference tests pass.
- Ten-sample Go 1.27 Linux measurements reduce `One(500)` from 3,882 B/43
  allocations to 2,226 B/28 and `Any(8/500)` from 14,723 B/154 to 8,067 B/97.
  Conservative immediate elapsed-time improvements are 3.20% and 16.90%.
- Rust overlay baselines now share `Arc<Fields>`. Registration event decoding
  uses a bitmask for fixed controls, application maps for field duplicates, a
  lazy set only for unknown controls, consuming String conversion, and direct
  protocol comparison. Fixed, unknown, Attr, and Data duplicates remain tested.
- Lua remained unchanged after line review. A Go event-kind experiment that
  traded one 8-byte allocation for worse CPU time was rejected and reverted.
- Go all-package, vet, WSL/Linux race; Rust 52 library plus four endpoint-free
  external tests, strict Clippy and rustdoc; deterministic generators; and
  Python testkit compilation passed.
- The isolated Redis 8.8 functional result passed 14 suites, 4,770 commands,
  empty-database cleanup, and Go/Rust live interoperability. The Sentinel result
  passed `16381 -> 16382 -> 16383`, generations `1 -> 2 -> 3`, unavailable views
  during total loss, UUID preservation, and recovery. Evidence:
  [`registration-selector-optimization-functional-20260829.json`](testkit/results/registration-selector-optimization-functional-20260829.json)
  and
  [`registration-selector-optimization-sentinel-20260829.json`](testkit/results/registration-selector-optimization-sentinel-20260829.json).
- The prior one-hour campaign has a different fingerprint and remains
  historical evidence. See the
  [full optimization review](registration/selector-optimization-review-20260829.md)
  for benchmark controls, scores, strengths, and remaining deductions.

### 2026-08-28 Fields mailbox one-hour fault qualification

- The accepted run `b6af4e4f` passed a frozen 104-file scope with SHA-256
  `38448c747230a72eb4d0b1a4ea838b83467a2b8d66d366909bbb1b73b6dd8f77` on
  Redis 8.8.0 with AOF `everysec`: 3,600 seconds of workload, 3,608.788 seconds
  measured by Redis TIME, and 3,650.336 seconds for the complete suite.
- All 1,800,000 scheduled Updates completed for 500 Registrations and eight
  Selectors. Update p50/p95/p99 was 0.604057/0.841814/1.135046 ms; 294,982
  selection transactions measured 0.182851/0.317371/0.915748 ms. The
  approximately three-second maximum Update and schedule lag occurred during
  deliberate three-second Redis pauses.
- All 16 standalone faults passed: seven script-cache flushes, five Pub/Sub
  client kills, two three-second Redis pauses, one AOF restart, and one ordinary
  client kill. Expected fault errors numbered 85; unexpected asynchronous
  errors, evictions, rejected connections, and monitor failures were all zero.
- Go goroutines returned `2 -> 530 -> 2`. Redis late median memory was 175,672
  bytes below its early median, and the owned database was empty at completion.
  Canonical Lua, Rust raw convergence, and Rust typed Registration/Selector
  checks passed both before and after the hour.
- The post-soak three-Redis/three-Sentinel matrix passed two promotions
  (`16381 -> 16383 -> 16382`), Go/Rust Selector generations `1 -> 2 -> 3`,
  total-Sentinel-loss unavailable views, UUID preservation, and cross-language
  convergence. Every owned remote resource was removed.
- Final short regression passed deterministic configuration/Lua generation,
  all Go packages, 52 Rust library plus four endpoint-free external tests, and
  Go/Rust interoperability-peer compile checks. Redis-dependent cases in the
  final local command were ignored because the accepted owned campaign had
  already exercised their endpoints.
- A first one-hour attempt is retained but rejected: its workload completed,
  then the post-check found an old Rust 100 ms clock-refresh fixture below the
  generated 1,000 ms minimum. The fixture and qualification gates were fixed
  before the accepted run. See the
  [detailed qualification report](registration/fields-mailbox-config-1h-20260828.md)
  for corrections, metrics, evidence boundaries, scores, and limitations.

### 2026-08-28 Fields mailbox and configuration DSL review

The generated configuration layer described in this historical result was
removed on 2026-08-29. Its mailbox and one-hour qualification evidence remains
valid for that frozen source identity; generation checks are no longer part of
the current build.

- Go and Rust now give each published Registration one single-slot merged
  Fields mailbox, capacity-one wake signal, sole-writer worker/task, and renewal
  timer. The default eight-entry admission bound limits result waiters rather
  than storing eight request or Data objects. Later pending Version/Data-field
  values overwrite earlier values, and same-batch Renew avoids a redundant
  Redis call only when Update actually refreshed TTL.
- Focused mailbox tests cover last-field/Version wins, one taken batch, empty
  mailbox after take, result completion, and Rust permit retention after both
  receiver Futures are dropped. Existing integration coverage verifies that a
  merged Update advances one revision and refreshes the lease.
- `schema/config.vdl` deterministically generates Go/Rust defaults, ranges, and
  leaf validators plus `configuration.md`. `python
  testkit/config/generate.py --check` passed. Strict Rust linting additionally
  verified that zero-minimum validators contain no absurd unsigned comparison
  and generated-default conversion contains no panic path.
- Windows Go format, all-package tests, and vet passed. WSL/Linux
  `go test -race ./...` passed after the options test stopped constructing a
  real `MinIdleConns=1` background dial merely to inspect driver settings.
  This was a test-isolation correction; no Registration goroutine leak or data
  race was observed.
- Rust format, all-target/all-feature strict Clippy, and rustdoc passed. `cargo test
  --all-targets --all-features` passed 52 library tests plus four endpoint-free
  integration/API tests. Seven Redis integration, three load, and two root
  Redis tests were explicitly ignored because `VERDANDI_REDIS_URL` and
  `VERDANDI_SENTINEL_URL` were not set for this short review run. The Windows
  linker's localized import-library message remains informational and is
  explicitly outside `-D warnings`.
- No external Redis fixture, long campaign, commit, or push was used.

### 2026-08-28 Root timeout naming review

- Go now names the ordinary root Redis command budget `Config.Timeout` and
  `Client.Timeout()`; Rust uses `Config::timeout` and a crate-private
  `timeout()`. Stable invalid-configuration diagnostics use field `timeout`.
  Domain-specific limits such as `sync_timeout` retain their qualified names.
- Go and Rust root and Catalog one-use bootstrap wrappers were inlined at their
  only call sites. Registration keeps its multi-step bootstrap invariant. This
  is a source-maintenance change, not a performance claim.
- Go format, all-package tests and vet passed. Rust format, 52 library tests,
  four endpoint-free integration/API tests, strict all-target/all-feature
  Clippy, and rustdoc passed. Both Lua generated-copy checks passed. The known
  Windows import-library linker message remains informational.
- The short isolated Redis 8.8 run passed 14 suites: both Lua contracts, Go
  integration and WSL/Linux race, six Rust Registration cases, Rust Catalog,
  two Rust root cases, and Go/Rust Registration/Catalog interoperability. It
  processed 4,751 commands, peaked at 2,978,384 bytes, verified an empty test
  database, and removed its exact container.
- A Sentinel review run passed SDK-specific integration and the first
  promotion, then correctly made both Selector views unavailable during total
  Sentinel loss. It stopped at the known topology gate when Sentinel continued
  to report excluded primary `16383` instead of publishing a usable second
  primary. No SDK assertion failed before that gate. All six ports, labelled
  containers, and remote directories were verified absent.
- The live functional/Sentinel review fingerprint was
  `05874222ecae71f6469039e89f6b745a58402cea53171e2fc74d470a7641e867`.
  Functional evidence is in
  [`root-timeout-functional-20260828.json`](testkit/results/root-timeout-functional-20260828.json);
  the incomplete Sentinel attempt is recorded separately and is not presented
  as a pass. The follow-up Go wrapper removal passed the affected root and
  Catalog package tests; current fingerprint is
  `bfb9396852fbf66d86f6a0d19fef35b7c5ba5a78e6098ef215366e4ef7747bc7`.
  Full qualification remains deferred until maintainer review ends.

### 2026-08-28 Direct Go root capability and production-comment verification

- Go root `Client` now exposes the same borrowed `*redis.Client`, permanent
  `Done()` broadcast, and normalized `Timeout()` used by Registration
  and Catalog. The former `internal/clientaccess` bridge is removed. The root
  remains the sole driver-close owner; raw go-redis calls are an ACL-controlled
  escape hatch outside Verdandi validation, limits, multi-key invariants, and
  stable error mapping.
- Go stores no long-lived request Context. Each Registration owns one Fields
  merge mailbox, writer/renew timer worker, and desired/confirmed state. Each Selector owns
  one persistent listener and at most one temporary synchronization goroutine.
  A typed Selector transaction now decodes every staged overlay before
  publishing any of them, so a later decoder failure cannot partially commit
  earlier candidates.
- Rust domain Open no longer creates a shutdown-only watcher task. Root and
  domain `CancellationToken`s directly gate admission; explicit domain Close
  performs the join. `Registration::is_registered()` now also observes the
  unique worker's terminal state. Registration Lua cleanup deliberately remains
  bounded by the ordinary operation timeout rather than being pre-empted by
  domain cancellation.
- The follow-up Rust ownership audit removed the remaining private `Transport`
  and `Owner` capability chain. Root `Client` now directly owns one
  `Arc<Inner>`; Registration and Catalog retain a clone of that same Client and
  invoke its crate-private Fred methods. Cloning creates no connection or task,
  and dropping one caller variable cannot invalidate a live domain dependency.
- Every handwritten production Go/Rust function and field now has a detailed
  Chinese declaration contract, and nontrivial ownership, concurrency,
  synchronization, capacity, and recovery blocks are annotated. Test source
  was not translated or mechanically reformatted. Rust test implementations
  remain physically under `sdk/rust/tests`.
- Registration Lua canonical fragments now document fixed KEYS/ARGV positions
  and each atomic phase in Chinese. Deterministic generation produced exact
  Go/Rust/canonical copies. Comments raise the four source bodies from 11,278
  to 14,112 bytes and change their SHAs, but executable statements and the
  steady EVALSHA path are unchanged. Independent `NOSCRIPT` reload passed.
- Endpoint-free gates pass: Go unit/vet, Go 1.27 WSL/Linux race, Rust format,
  52 library tests plus four endpoint-free integration/API tests, strict
  all-target/all-feature Clippy, generator freshness, generator syntax, and
  Black 26.5.1 formatting for the modified generator. The Windows Rust linker
  emits its known import-library message; Clippy still passes with warnings
  denied because `linker_messages` is separately informational.
- An isolated Redis 8.8.0 Registration/root run passed 17 suites, processed
  48,120 commands, peaked at 3,707,896 Redis bytes, ended with zero keys, and
  removed its labelled container. The existing shared interop peer also ran a
  small Catalog exchange inside that disposable container; Catalog functional,
  load, and Sentinel suites were otherwise excluded to avoid another task's
  resources.
- Go and Rust each completed 5,000 scheduled Updates with 500 continuously live
  Registrations at exactly 500.0/s. Go operation p50/p95/p99 was
  0.572/0.971/1.308 ms; Rust was 0.616/0.961/1.401 ms. Automatic-renew calls
  completed at 482.6/s and 471.4/s during the short jittered windows. A
  1,000-record Selector synchronization completed in 24.305 ms for Go and
  33.636 ms for Rust.
- The qualified Sentinel run first rejected an incomplete topology because
  Sentinel continued reporting excluded primary `16383`; exact cleanup and an
  unchanged rerun then passed in 33.094 seconds. Both SDKs preserved UUIDs,
  both Selector generations advanced `1 -> 2 -> 3`, and state converged after
  acknowledged-write loss, `SCRIPT FLUSH`, total Sentinel loss, and a second
  promotion.
- After the load run, comment completion, signature wrapping, and generated
  derive documentation changed the 88-file fingerprint without changing
  executable behavior. The exact frozen source then passed 12 isolated Redis
  8.8.0 Registration/root suites, processed 2,264 commands, peaked at 2,632,832
  Redis bytes, ended with zero keys, and removed its labelled container.
- Two intermediate-frozen-source Sentinel attempts passed SDK-specific integration,
  first promotion, acknowledged-loss recovery, full-Sentinel-loss unavailable
  state, and cleanup. Neither is counted as a complete Sentinel pass: after
  restart, Sentinel retained the excluded primary (`16383`, then `16382`)
  through the bounded second-promotion window. No source or test change was
  made between attempts, and all six dedicated ports were verified closed.
- The direct-Rust-Client source passed format, all-target/all-feature compile,
  52 library tests, four endpoint-free integration/API tests, strict Clippy and
  rustdoc. An isolated Redis 8.8.0 run passed the existing six Registration and
  two root tests, processed 548 commands, ended with zero keys, and removed its
  exact container. Test source was not edited.
- The final current-source Sentinel rerun passed in 40.327 seconds. Initial,
  acknowledged-loss, and recovered masters were `16381 -> 16382 -> 16383`;
  both UUIDs were preserved and both Selector generations advanced
  `1 -> 2 -> 3` through `SCRIPT FLUSH`, total Sentinel loss, restart, and the
  second promotion. All six ports, labelled containers, and remote directories
  were verified absent afterward.
- The direct-Rust-Client freeze fingerprint was
  `e709ae4ce1149377c2276e41e053c7b264f64cacda13da29b85559261dd628f9`
  across 88 Go/Rust/Lua/generator files. The prior comment-only freeze was
  `a5323e162ef7778b4cb19847f56214ece0dd8e0634a7180e872df8db8e586739`;
  the load and completed Sentinel qualification used fingerprint
  `bf703e372f259100a2332533c15d299eb22db43b411880afb2b19abce29987c8`;
  machine-readable evidence keeps all identities separate in
  [`registration-direct-root-functional-20260828.json`](testkit/results/registration-direct-root-functional-20260828.json)
  and
  [`registration-direct-root-sentinel-20260828.json`](testkit/results/registration-direct-root-sentinel-20260828.json).

Current implementation score for the Registration/Selector slice is
**9.8/10**: Lua is **10.0**, Go is **9.8**, and Rust improves to **9.8**. This
scores the accepted implementation and evidence, not release readiness. The
main remaining deductions are the unenforceable borrowed-driver ownership on
Go's escape hatch, large but cohesive Go/Rust state-machine files, Rust Drop's
necessarily best-effort async cleanup, Redis/Sentinel-specific behavior, the
observed Sentinel topology-election instability before the final clean pass,
and the absence of a fresh multi-hour campaign for this fingerprint.

### 2026-08-28 Thin Rust root Client verification

- Root Rust `Config` now owns only endpoint, ordinary operation timeout, and
  reconnect delays. Registration and Catalog Configs independently require
  Zone. Root Client's private `Inner` retains the Fred command client and a
  dedicated Subscriber factory; domain clients retain the root Client directly.
  A private Transport capability, root admission mutex/counters, child guards,
  joined domain shutdown, manual handle counting, and domain `Deref` access are
  absent.
- Root Key/Hash commands no longer allocate an `Arc` guard or update root active
  counters. Root open performs only `PING`; Registration performs the Redis 8
  check through `HELLO`. `close().await` awaits Fred `quit()` without waiting
  for domain Clients. The final root-or-domain-held Client clone performs
  explicitly best-effort Drop cleanup because Rust Drop cannot await.
- Added unit coverage for Zone ownership and HELLO response parsing. Added a
  live Redis regression that opens two independent Registration Zones over one
  transport, verifies both policy Hashes, closes the root before one domain,
  rejects later child construction, and closes the remaining domain
  independently.
- Rust formatting, 52 library tests, all-target tests, strict Clippy, and
  rustdoc pass. The isolated authenticated Redis 8.8.0 functional matrix passed
  both Lua contracts, Go integration/race, six Rust Registration tests, two
  Rust Catalog tests, two Rust root tests, and bidirectional Go/Rust
  Registration/Catalog interoperability. It processed 4,750 commands, peaked
  at 2,945,264 bytes, left no keys, and removed its exact container.
- The first Sentinel attempt passed direct SDK checks and the first promotion
  but its second topology election continued reporting the excluded old primary
  `16383`. Exact cleanup followed by an unchanged run passed in 29.914 seconds:
  both SDKs preserved UUIDs, both Selector generations advanced `1 -> 2 -> 3`,
  and cross-language state converged after two promotions and complete Sentinel
  loss/restart.
- Machine-readable evidence is retained in
  [`testkit/results/rust-transport-refactor-functional-20260828.json`](testkit/results/rust-transport-refactor-functional-20260828.json)
  and
  [`testkit/results/rust-transport-refactor-sentinel-20260828.json`](testkit/results/rust-transport-refactor-sentinel-20260828.json).
  No load/soak campaign, commit, tag, publication, or push was performed.

### 2026-08-28 Thin Go root Client verification

- Root Go `Config` now owns only Standalone/Sentinel connectivity and ordinary
  operation timeout. Registration and Catalog Configs independently require
  Zone. Root stores concrete `*redis.Client`; the global child-binding table,
  `redis.UniversalClient`, root admission accounting, and joined root shutdown
  are absent.
- Added unit coverage for idempotent close broadcast, invalid domain Zones, and
  definite closed-write classification. Added a live integration that opens two
  different Registration Zones over one transport, verifies both Zone policy
  Hashes, closes the root first, rejects later domain construction, and joins
  both domain Clients without leaked workers.
- Go formatting, three shuffled unit repetitions, vet, combined
  `integration,load,soak` compilation, and all three Go peer test/vet gates
  pass. The existing concise root commands remain paired with their
  `*Context` forms.
- One isolated authenticated Redis 8.8.0 fixture passed all 13 functional
  suites: both canonical Lua contracts, Go integration, WSL/Linux race, six
  Rust Registration/Selector cases, Rust Catalog, Rust root commands, and
  bidirectional Go/Rust Registration/Catalog interoperability. It processed
  4,655 commands, peaked at 2,974,568 bytes, left no keys, and removed only its
  owned container.
- The three-Redis/three-Sentinel Registration matrix passed two promotions in
  48.404 seconds. Go and Rust retained their UUIDs and both Selector generations
  advanced `1 -> 2 -> 3`. The Catalog Sentinel matrix passed two promotions in
  28.112 seconds, converged to revision 10, removed three owned keys, and ended
  with zero keys.
- Detailed machine evidence is retained in
  [`testkit/results/go-thin-client-functional-20260828.json`](testkit/results/go-thin-client-functional-20260828.json).
  No load/soak campaign, commit, tag, publication, or push was performed.

This result includes:

- Redis-backed Registration policy with a Redis-owned refresh interval;
- `register`, `update`, `renew`, and `unregister` lifecycle symmetry;
- a non-selectable retained recovery view after natural expiry or fenced
  absence;
- same-revision header reuse during Selector reconciliation;
- Go/Rust generic strong types with application-owned field codecs and raw
  `Fields` support through the same API;
- 500 continuously live Registrations updating for five minutes at 500
  updates/s;
- 500 continuously live Registrations automatically renewing for five
  minutes;
- eight simultaneous Selectors;
- paginated synchronization and cleanup of 5,000 Registrations; and
- two Redis primary promotions, including demonstrable loss and recovery of an
  acknowledged write; and
- current Catalog Value/Array/Map Replace, exact-base Patch, Delete,
  Hash/ZSET/Pub/Sub recovery, optional checkpoint, and `2^53-1` operation; and
- a strict Redis-clock two-hour Catalog run with 960,000 attempts, two
  Subscribers, 18 injected faults, final convergence, and complete cleanup.

The current source and SDK version is the non-production `0.1.0` Alpha. Stable
`1.0.0` remains reserved for the complete supported contract. Desired
configuration, acknowledgements, managed Redis services, and production
readiness are outside this result; generic Campaign/Leader election is excluded
from the project rather than deferred.
No commit, tag, package publication, or push was created.

### 2026-08-27 Rust test-source separation verification

- All 19 private unit-test implementations now live under
  `sdk/rust/tests/internal`; production modules retain only conditional path
  hooks. Test-only helper types and script tables no longer appear in
  production source.
- Current stable Rust passes formatting, Clippy with warnings denied, and all
  53 endpoint-free tests. Rust 1.85 passes the same all-workspace,
  all-targets, all-features test command after replacing two unsupported
  `let` chains with equivalent nested checks.
- Eleven Redis, Sentinel, and load tests remain explicitly ignored without
  opt-in endpoints. No live Redis or long-running campaign was started.

### 2026-08-27 Go 1.27 and shared-transport verification

- Go 1.27.0 Windows/amd64: `gofmt`, unit tests, `go vet`, and compile-only
  `integration`, `integration,load`, and `integration,load,soak` tag gates pass.
- Go 1.27.0 WSL2/Linux amd64: formatting, shuffled unit tests, `go vet`, the
  combined `integration,load,soak` compile gate, and the race detector pass.
- All three Go peer modules build after their language directive moved to
  `go 1.27.0`; their Linux unit and vet gates also pass. The Go 1.27
  `atomictypes`, `embedlit`, `slicesbackward`, and `unsafefuncs` modernizers
  produce no remaining diff.
- Registration construction measures 240 B/three allocations after removing
  retained codec closures, versus 288 B/five allocations before the change.
  The current Linux median is 165.3 ns/op with a 2% confidence interval.
- Ten-pair alternating single-core comparisons of Go 1.27's default
  size-specialized allocator against `nosizespecializedmalloc` find no
  statistically significant Windows or Linux change in Registration
  construction, Registration Update, or the 500-candidate typed Selector
  transaction; bytes and allocations are identical.
- Rust passes 49 crate unit tests plus four endpoint-free integration/API
  tests, strict formatter, Clippy with warnings denied, documentation
  generation, and all three peer checks. Eleven Redis/Sentinel/load tests
  remain explicitly ignored without their opt-in endpoints.
- Native Windows race execution remains unavailable because that toolchain has
  `CGO_ENABLED=0` and no C compiler. The upgraded WSL Go 1.27.0 toolchain has
  `CGO_ENABLED=1` and GCC, and its complete non-Redis race suite passes. No live
  Redis or long-running test was performed.

### 2026-08-27 Lua/Go/Rust source-review regression

- Both canonical Lua generators are current. The specialized Lua programs were
  retained because consolidating their operation-specific bodies would add
  calls to high-cardinality hot paths; bounded field loops also remain required
  to avoid Redis Lua stack ceilings.
- Go passes Windows unit/vet and WSL/Linux race and all-tag compile gates. Rust
  passes stable format, warnings-denied Clippy, tests, and docs, plus the Rust
  1.85 all-target/all-feature test gate. The two Go wire decoders pass 10-second
  fuzz runs with 3,014,944 Registration and 152,006 Catalog executions.
- The final isolated Redis 8.8.0 matrix passes 13/13 Lua, Go, Rust, Catalog,
  Registration/Selector, root-command, and live-interoperability suites. It
  processed 4,579 Redis commands, peaked at 3,050,104 bytes, reported zero
  background-thread exceptions, cleaned its owned fixture, and did not run a
  load or soak campaign.
- The regression driver now forces UTF-8 child-process decoding, validates that
  directed Rust filters execute the expected number of tests, covers the typed
  transactional Selector, and includes the Rust root Redis API. These changes
  prevent a successful zero-test Cargo invocation from being reported as
  coverage.

Raw machine-readable evidence is retained in:

- [`testkit/results/standalone-lua-line-long-20260824.json`](testkit/results/standalone-lua-line-long-20260824.json);
- [`testkit/results/sentinel-lua-line-20260824.json`](testkit/results/sentinel-lua-line-20260824.json);
- [`testkit/results/lua-register-line-final-20260824.json`](testkit/results/lua-register-line-final-20260824.json);
- [`testkit/results/lua-registration-line-final-20260824.json`](testkit/results/lua-registration-line-final-20260824.json);
- [`testkit/results/lua-registration-hot-path-final-20260824.json`](testkit/results/lua-registration-hot-path-final-20260824.json); and
- [`testkit/results/lua-register-line-optimization-20260824.json`](testkit/results/lua-register-line-optimization-20260824.json);
- [`testkit/results/catalog-functional-20260824.json`](testkit/results/catalog-functional-20260824.json);
- [`testkit/results/catalog-production-functional-final-20260824.json`](testkit/results/catalog-production-functional-final-20260824.json); and
- [`testkit/results/catalog-production-full-20260824.json`](testkit/results/catalog-production-full-20260824.json);
- [`testkit/results/catalog-rust-typed-functional-20260824.json`](testkit/results/catalog-rust-typed-functional-20260824.json); and
- [`testkit/results/catalog-stream-hub-functional-20260824.json`](testkit/results/catalog-stream-hub-functional-20260824.json); and
- [`testkit/results/catalog-stream-gap-functional-20260825.json`](testkit/results/catalog-stream-gap-functional-20260825.json); and
- [`testkit/results/catalog-production-cleanup-functional-20260825.json`](testkit/results/catalog-production-cleanup-functional-20260825.json);
- [`testkit/results/catalog-lua-soak-production-functional-20260825.json`](testkit/results/catalog-lua-soak-production-functional-20260825.json);
- [`testkit/results/lua-catalog-line-final-20260825.json`](testkit/results/lua-catalog-line-final-20260825.json);
- [`testkit/results/catalog-v1-benchmark-20260826.json`](testkit/results/catalog-v1-benchmark-20260826.json);
- [`testkit/results/catalog-soak-interrupt-rehearsal-20260825.json`](testkit/results/catalog-soak-interrupt-rehearsal-20260825.json); and
- [`testkit/results/catalog-soak-2m-steady-20260825.json`](testkit/results/catalog-soak-2m-steady-20260825.json); and
- [`testkit/results/catalog-soak-2h-redis-clock-20260826.json`](testkit/results/catalog-soak-2h-redis-clock-20260826.json); and
- [`testkit/results/catalog-soak-2h-redis-clock-20260826-samples.jsonl`](testkit/results/catalog-soak-2h-redis-clock-20260826-samples.jsonl); and
- [`testkit/results/catalog-soak-8h-redis-clock-20260827.json`](testkit/results/catalog-soak-8h-redis-clock-20260827.json);
- [`testkit/results/catalog-soak-clock-fix-preflight-20260827.json`](testkit/results/catalog-soak-clock-fix-preflight-20260827.json); and
- [`testkit/results/catalog-soak-8h-redis-clock-20260827-rerun-interrupted-summary.json`](testkit/results/catalog-soak-8h-redis-clock-20260827-rerun-interrupted-summary.json); and
- [`testkit/results/registration-production-review-20260825.json`](testkit/results/registration-production-review-20260825.json).
- [`testkit/results/source-review-regression-20260827.json`](testkit/results/source-review-regression-20260827.json).

Earlier `lua-split` and `lua-glue` files remain historical intermediate
evidence. The Registration-specific files above remain authoritative for that
domain. All Catalog artifacts dated before the 2026-08-26
Replace/Patch/Delete redesign are implementation-history evidence only.
Section 18 and `catalog-v1-benchmark-20260826.json` describe the current
Catalog protocol and evidence.

## 2. Environment

### Redis fixtures

- Redis Open Source 8.8.0, Docker image `redis:8.8.0`;
- Linux host at `192.168.0.90`, LAN-connected to the clients;
- standalone Redis on port 16380 with generated ACL credentials;
- final Catalog-only and full production-review fixtures on isolated ports
  36429, 36430, the Rust typed follow-up on 36432, and the cross-language
  Stream-Hub/cleanup follow-ups on 36433, 36434, and 36436;
- paired Lua and Catalog endurance fixtures on additional preflighted ports,
  including the accepted 120-second gate on 36439, each removed after success;
- Sentinel topology with Redis ports 16381-16383 and Sentinel ports
  26381-26383;
- independent generated Redis and Sentinel ACL identities;
- persistence disabled for reproducible test isolation; and
- exact random run labels, container names, and remote temporary directories.

### Client toolchains

- Windows amd64: Go 1.27.0, Rust 1.98.0, Cargo 1.98.0, Python 3.14.7;
- WSL2 Linux amd64: Go 1.27.0, Linux
  6.18.33.2-microsoft-standard-WSL2, 24 visible processors;
- current minimum Go toolchain and module language: Go 1.27.0; and
- minimum Rust toolchain: Rust 1.85.0, edition 2024.

Windows, WSL, Go, and Rust timings use different runtime paths. They are not a
language shoot-out. Optimization decisions use Linux/WSL Go measurements;
end-to-end load uses the same remote Redis fixture and records client build
mode explicitly.

## 3. Static, unit, generator, and compatibility checks

| Verification | Result |
| --- | --- |
| Canonical Lua contract on Redis 8.8 | PASS |
| Registration four-script and Catalog three-script generated Lua sets | PASS, byte-identical per operation and SDK |
| Go unit and saved fuzz-corpus tests | PASS |
| Go shuffled repetition | PASS, `-shuffle=on -count=10` |
| Go `vet` | PASS |
| Go Linux race detector | PASS |
| Go Linux race with real authenticated Redis | PASS |
| Go current minimum toolchain | PASS on Go 1.27.0 |
| Go code generation repeatability/fixture freshness | PASS |
| Go Catalog decoder fuzz | PASS, 60 seconds, 20,405,860 executions |
| Rust unit tests | PASS, 41 tests |
| Rust all targets | PASS |
| Rust formatter | PASS |
| Rust Clippy with warnings denied | PASS on the current toolchain |
| Rust documentation build | PASS |
| Rust minimum toolchain | PASS on Rust 1.85.0 |

The minimum Rust run found two uses of a newer `if let` chain. They were
rewritten as an equivalent match/nested condition and the full Rust 1.85 test
suite then passed. The standalone harness subsequently recompiled that code and
the Sentinel matrix passed, so the compatibility fix is not merely a compile
fixture.

The four-script bootstrap initially exposed a Rust test-process stack overflow:
placing all four large `Script::load` futures directly inside `try_join!` kept
their state in one async task. Heap-pinning each future retained concurrent
loading while bounding task-stack use. Rust unit/integration and live
Go/Rust interoperability passed after the correction.

Unit coverage exercises canonical identities and safe integers, invalid UTF-8,
independent 16-Attr/32-Data defaults, 128-byte default values, complete-record
accounting, fixed Data names, no-op updates, cancellation after admission,
RedisClock uncertainty, RESP2/RESP3 PONGs, deadline indexes, bounded pending
state, retained expiry/reactivation/purge/eviction, typed field boundary
detachment, and every supported MessagePack width. Array/map/extension values,
impossible 32-bit lengths, truncated input, duplicates, trailing bytes, and
unknown non-scalar controls are rejected before expansion.

### Operation-specific Lua glue generation and measurement

One manifest composes the shared hot bindings used by Register/Update and the
four operation actions into canonical executables. Renew and Unregister retain
only their operation-specific bindings. The generator rejects
missing, unused, duplicated, unsafe, or incorrectly ordered fragments, emits
canonical LF bytes atomically, and verifies the canonical/Go/Rust copies byte
for byte.

SDKs validate request shape, protocol/kind, identifiers, canonical scalars,
reserved names, Attr/Data structure, immutable fields, field capacities,
complete projected state, and no-op updates before Redis I/O. Lua deliberately
does not repeat those checks. The Redis fixture proves the boundary by directly
calling the Register SHA with positional controls, 129 Data fields, and a
16-KiB-plus-one value: Lua accepts and atomically stores it, while Go and Rust
tests reject equivalent input locally. The fixture independently exercises
`SCRIPT FLUSH` recovery for all four SHAs.

The historical pre-positional Redis 8.8 comparison used 500 live Registrations, 20 rounds per
trial, 10,000 calls per measured phase, eleven paired trials, alternating
order, no subscribers, and disabled persistence. The test-only combined
executable was reconstructed from the exact same minimal fragments; it is not
a production artifact.

| Operation | Shape | Redis script median | Wall median |
| --- | --- | ---: | ---: |
| Update | test-only combined | 15.66 us/call | 30,734.18/s |
| Update | operation-specific | 15.67 us/call | 30,843.33/s |
| Renew | test-only combined | 14.51 us/call | 34,421.70/s |
| Renew | operation-specific | 14.31 us/call | 33,271.57/s |

The operation split changed measured Redis execution by -0.06% for Update and
+1.38% for Renew; wall throughput changed by +0.36% and -3.34%. The direction
disagreement and small server differences make the split runtime-neutral; wall
measurements include client, network, and host scheduling noise. Its
justification is four narrow review/cache-reload boundaries, not a throughput
claim. Bootstrap owns four cached SHAs, while a logical mutation still performs
one `EVALSHA`.

Removing duplicated Lua validation first reduced the four generated bodies from
44,133 to 19,948 UTF-8 source bytes. Positional controls, direct writes,
numeric arguments, initial inlining, and `HSETEX` then produced a 14,763-byte
set. The final line audit removed every one-call-site helper closure and generic
reply builder, specialized narrow bindings, cached the tail-traversing `ARGV`
table, used explicit event write indexes, and skipped absent-key Register
`DEL`; the current four files total 11,278 bytes including exact ABI and
fragment comments. The old combined-shape figures above remain historical
evidence that splitting scripts was runtime-neutral.

The subsequent pre-promotion Register line review used eleven alternating trials,
2,000 small or 500 default-maximum fresh records per variant, no subscribers,
and disabled persistence. Redis `cmdstat_evalsha` measured the following
cumulative candidates before the accepted candidate replaced production:

| Shape | Baseline | Fixed header | Direct HSET | Full + HSETEX |
| --- | ---: | ---: | ---: | ---: |
| 2 Attr + 2 Data | 14.23 us | 12.38 us | 11.86 us | 10.19 us |
| 16 Attr + 32 Data, 128 B each | 51.83 us | 43.11 us | 40.63 us | 38.70 us |

The final candidate improved paired server time by 28.68% and 25.40%
respectively, with all eleven pairs positive. Fixed positional v1 controls and
direct `ARGV` writes produced the largest stable gains; isolated reply,
publication, local-call, and `HSETEX` changes were individually near benchmark
noise. Raw trials and parent-relative statistics are in
`testkit/results/lua-register-line-optimization-20260824.json`.

The second line audit retained the previous production SHAs inside the same
Redis process and alternated old/candidate execution. It rejected modulo TIME
conversion, removing the repeated `tonumber` local, implicit arithmetic request
coercion, absent-state conversion short-circuiting, a local `KEYS` table, and a
cached `has_version` predicate because they did not meet the paired consistency
gate. The predicate candidate was -0.23% for one-field Update and -0.16% for
32-field Update by median server time. Short protocol literals remain compiled
constants rather than per-call locals. Accepted cumulative results over 21
paired trials were:

| Operation | Shape | Previous | Final | Paired improvement / wins |
| --- | --- | ---: | ---: | ---: |
| Register | 2 Attr + 2 Data | 9.60 us | 8.69 us | +9.03%, 21/21 |
| Register | 16 Attr + 32 Data, 128 B | 39.00 us | 35.57 us | +7.65%, 20/21 |
| Update | one Data field | 9.05 us | 8.43 us | +6.66%, 21/21 |
| Update | version + one Data field | 9.33 us | 8.73 us | +6.18%, 21/21 |
| Update | 31 Data fields | 19.86 us | 18.28 us | +7.12%, 21/21 |
| Renew | unchanged content | 8.43 us | 7.82 us | +7.25%, 21/21 |
| Unregister | existing record | 3.72 us | 3.72 us | -0.27%, 9/21 |

Unregister is classified as neutral. Its specialization is retained because it
removes unrelated initialization, shrinks the program, and does not change the
median. All functional protocol behavior remained byte-compatible.

The same fixture found a correctness boundary before promotion: Redis 8.8
accepted Hash-field absolute expiry through `70368744177663` (`2^46-1`) and
rejected the Lua safe-integer maximum, while Hash storage and key-level expiry
accepted `9007199254740991` exactly. Go, Rust, event/record admission, and Lua
now enforce the stricter Hash-field ceiling; exact ceiling, ceiling-plus-one,
rollback, `HPEXPIREAT`, and `HSETEX PXAT` vectors pass. Numeric arguments and
`HSETEX PXAT` are canonical.

Fresh absolute production measurements on the same isolated Redis 8.8 fixture
are:

| Operation | Shape | Redis script median | Wall median |
| --- | --- | ---: | ---: |
| Register | 2 Attr + 2 Data | 8.69 us/call | 49,877.36/s |
| Register | 16 Attr + 32 Data, 128 B each | 35.57 us/call | 7,364.20/s |
| Update | one Data field | 8.43 us/call | 49,803.82/s |
| Update | version plus one Data field | 8.73 us/call | 49,802.33/s |
| Update | canonical 32 Data fields | 19.74 us/call | 15,813.84/s |
| Renew | unchanged content | 7.82 us/call | 60,092.47/s |
| Unregister | existing record | 3.72 us/call | 93,566.38/s |

All rows except canonical 32-field Update are the final side of the 21-pair
comparison. The corrected 32-field row is a current-source-only run and is not
given an old-SHA improvement claim.

These no-subscriber microbenchmarks measure Redis script-body cost. They do not
include end-to-end SDK pacing, subscriber fan-out, or persistence and are not
compared numerically with the load test below.

The post-promotion qualification repeated all four formal five-minute phases
with eight Selectors and the 5,000-record recovery case:

| Implementation | Update | Renew | 5,000-record sync |
| --- | ---: | ---: | ---: |
| Go/WSL | 150,000, 500.0/s, p99 1.221 ms | 149,597, 498.7/s | 56.489 ms |
| Rust release | 150,000, 500.0/s, p99 1.219 ms | 148,610, 495.4/s | 322.066 ms |

Every phase passed and final cleanup left the isolated database empty. Absolute
Redis `cmdstat_evalsha` averages from paced Go and Rust runs are retained in
raw JSON but are not compared across client/runtime paths; the alternating
same-fixture benchmark above is the Lua-shape measurement.

## 4. Redis-backed policy and retained view

Both SDKs bootstrap and refresh the same non-expiring
`verdandi:config:<zone>` Hash. The final recognized fields are:

| Field | Default | Accepted range/ceiling |
| --- | ---: | ---: |
| `registration_attr_max_fields` | 16 | 1-128 |
| `registration_data_max_fields` | 32 | 1-128 |
| `registration_max_field_name_bytes` | 64 | 1-64 |
| `registration_attr_max_field_value_bytes` | 128 | 1-16,384 |
| `registration_data_max_field_value_bytes` | 128 | 1-16,384 |
| `registration_max_bytes` | 16,384 | 1-65,536 |
| `configuration_refresh_ms` | 30,000 | 1,000-86,400,000 |

Clients immediately load one complete valid bootstrap snapshot. Register
refreshes synchronously before validation. The first successfully published
Registration starts one Client-shared poller; the last Registration stops it.
While active, the poller waits the currently loaded
`configuration_refresh_ms` plus or minus ten percent jitter. A successful
refresh changes the next interval without restart. Missing, malformed, partial,
or above-ceiling state reports a diagnostic and keeps the previous valid limits
and interval. Explicit refresh remains available without a Registration.

For a Registration with `deadline = @timestamp + @ttl`:

```text
now < deadline                    selectable active view
deadline <= now < deadline+@ttl   non-selectable retained view
deadline+@ttl <= now              removed locally
```

Natural expiry and fenced authoritative absence may enter retained state.
Explicit `unregister` purges both active and retained state. A valid
same-UUID update, renew, register, or recovered Hash can reactivate retained
content. The retained cache is independent from active-view bytes: 64 MiB by
default, zero disables it, 1 GiB is the local maximum, and pressure evicts the
earliest retained deadline first. Redis stores no retained copy; this is a
bounded local recovery aid, never a lease extension or selectable endpoint.

## 5. Authenticated standalone integration

The functional harness created one isolated Redis 8.8 container, generated an
ACL password, and passed all suites before proving the selected database was
empty. Its final run also executed Go integration under the Linux race
detector.

| Scenario | Go | Rust |
| --- | ---: | ---: |
| Initial configuration bootstrap and complete reread | PASS | PASS |
| Live refresh and invalid-refresh last-valid fallback | PASS | PASS |
| Register and binary MessagePack event | PASS | PASS |
| Partial Data/version update | PASS | PASS |
| Equal-value local no-op | PASS | PASS |
| Renew changes timestamp but not content revision | PASS | PASS |
| 24 concurrent updates serialize exactly | PASS | PASS |
| Revision gap uses bounded targeted repair | PASS | PASS |
| Missing record/membership recovery | PASS | PASS |
| Natural expiry enters retained, then expires after a second TTL | PASS | PASS |
| Explicit unregister purges retained state | PASS | PASS |
| `SCRIPT FLUSH` reload | PASS | PASS |
| Protocol-ceiling 128 Attr + 128 Data record | PASS | PASS |
| Repeated close and joined workers | PASS | PASS |
| Go typed generated Registration/Selector | PASS | N/A |
| Go/Rust live Pub/Sub interoperability in both directions | PASS | PASS |

The protocol-ceiling fixture recovered a 256-application-field Registration
whose encoded application values were 240 bytes each. This is a maximum-shape
correctness test, not a sustained maximum-payload fan-out benchmark.

The Go typed integration additionally proved canonical big-endian primitive
storage, defensive `[]byte` detachment, no Attr/Data re-decode for Renew, Data-
only projection refresh for a Data update, patch-only Redis transmission, and
terminal typed-state removal.

## 6. Sustained 500-Registration qualification

The formal load used 500 continuously live Registration writers, one update per
Registration per second, evenly staggered across every second. It did not
divide a five-round burst by only the four seconds between first and last
round. Eight independent Selectors observed the same Type. Go ran from WSL2
Linux; Rust used `--release`.

### Go, WSL2 Linux

| Measurement | Result |
| --- | ---: |
| Register 500 | 142.758 ms, 3,502.4/s |
| Register p50 / p95 / p99 / max | 17.698 / 19.599 / 20.023 / 20.315 ms |
| Initial HSCAN sync, page 64, fan-out 8 | 4.789 ms |
| Sustained updates | 150,000 in 299.999 s, 500.0/s |
| Update p50 / p95 / p99 / max | 619.2 us / 930.353 us / 1.221 ms / 4.388 ms |
| Schedule lag p50 / p95 / p99 / max | 497.581 us / 1.027 ms / 1.113 ms / 2.493 ms |
| Redis `EVALSHA` | 150,000 calls, 0.70 us average |
| Graceful unregister 500 | 22.566 ms, 22,157.2/s |
| Config + Registry + 500 Registration keys | 145,919 bytes |

### Rust, Windows release profile

| Measurement | Result |
| --- | ---: |
| Register 500 | 17.825 ms, 28,050.8/s |
| Register p50 / p95 / p99 / max | 1.208 / 1.687 / 1.898 / 7.612 ms |
| Initial HSCAN sync, page 64, fan-out 8 | 7.784 ms |
| Sustained updates | 150,000 in 300.005 s, 500.0/s |
| Update p50 / p95 / p99 / max | 685.4 us / 1.009 ms / 1.219 ms / 3.519 ms |
| Schedule lag p50 / p95 / p99 / max | 7.697 / 14.678 / 15.539 / 16.608 ms |
| Redis `EVALSHA` | 150,000 calls, 26.34 us average |
| Graceful unregister 500 | 6.314 ms, 79,186.6/s |
| Config + Registry + 500 Registration keys | 145,919 bytes |

Both implementations therefore meet the accepted 500 updates/s profile for a
continuous five-minute window. These are qualification observations, not
promised SLOs.

Earlier reports described 2,500 updates in about four seconds as 617.6-621.4
updates/s. That calculation measured five synchronized rounds from the first
round start to the last round completion, not a sustained per-second cadence,
and must not be used as capacity evidence. The table above supersedes it.

### Fault-injected two-hour endurance run

The final current-source qualification extended the same 500-writer/eight-
Selector profile across **7,263.649 Redis-server seconds**. It scheduled 7,500
Go seconds as clock-drift safety margin and enforced a separate 7,200-second
Redis `TIME` floor. The exact source fingerprint, fault events, samples, and
post-checks are in
[`registration-soak-2h-20260825.json`](testkit/results/registration-soak-2h-20260825.json)
and
[`registration-soak-2h-20260825-samples.jsonl`](testkit/results/registration-soak-2h-20260825-samples.jsonl).

| Measurement | Result |
| --- | ---: |
| Updates | 3,750,000 |
| Update p50 / p95 / p99 / max | 0.759 / 1.463 / 2.386 ms / 3.163 s |
| Schedule-lag p50 / p95 / p99 / max | 0.448 / 1.080 / 1.203 ms / 2.164 s |
| Transient retries | 2,689 |
| Natural-expiry cycles / records | 25 / 3,200 |
| Churn cycles / explicit deletes | 25 / 400 |
| Peak retained records | 34 |
| Expected / unexpected async errors | 514 / 0 |
| Final revision / Selector generation | 7,501 / 15 |
| Goroutines initial / peak / final | 2 / 1,541 / 2 |
| Redis stable-memory growth / gate | 634,476 / 2,097,152 bytes, PASS |
| Redis used-memory / RSS peak | 11,201,200 / 30,253,056 bytes |
| Evictions / rejected connections / final DB size | 0 / 0 / 0 |

All 34 injected faults passed: 14 `SCRIPT FLUSH`, 11 complete Pub/Sub
connection kills, four three-second pauses, three AOF restarts, and two ordinary
connection kills. Canonical Lua, Rust standalone convergence, and the complete
Go/Rust Sentinel matrix passed afterward. The dedicated report
[`registration/soak-20260825.md`](registration/soak-20260825.md) explains the
clock gate, isolated source snapshot, cleanup proof, strengths, weaknesses, and
score.

## 7. Automatic-renewal and 5,000-record scale qualification

The separate renewal test uses a three-second TTL and one-second automatic
renew interval. It verifies all 500 records remain live and every content
revision stays exactly one.

| Implementation | Duration | Renew calls | Rate | Redis script average |
| --- | ---: | ---: | ---: | ---: |
| Go | 300.001 s | 149,597 | 498.7/s | 0.61 us |
| Rust release | 300.010 s | 148,610 | 495.4/s | 22.22 us |

The small difference from the theoretical 150,000 calls is process startup and
shutdown boundary time, not expiry or content mutation.

The scale test then registered 5,000 records with concurrency 64, synchronized
one Selector through `HSCAN COUNT 64`, and verified terminal convergence:

| Implementation | Register | Initial sync | Unregister |
| --- | ---: | ---: | ---: |
| Go/WSL | 1.275 s, 3,922.5/s | 56.489 ms | 1.028 s, 4,861.6/s |
| Rust release | 603.7 ms, 8,282.3/s | 322.066 ms | 197.283 ms, 25,344.3/s |

Five thousand is test evidence, not a protocol limit. The protocol has no
maximum Registration count; page, event, time, active-view, and retained-view
budgets remain finite local controls.

Across the final line-audited standalone suite Redis processed 1,107,752
commands, 42,579,485 input bytes, and 39,646,320 output bytes. Peak Redis memory
was 5,288,400 bytes and measured Redis CPU was 23.617 system plus 28.865 user
seconds. Final database size was zero.

## 8. Go hot-path and generated-codec measurements

Measurements used WSL2 Linux, Go 1.26.4, amd64, `-benchmem`, ten samples.

| Operation | Final range | Bytes/op | Allocs/op |
| --- | ---: | ---: | ---: |
| Decode one Registration event | 893.0-906.4 ns | 896 | 12 |
| Coalesce 32 contiguous updates for one UUID | 5.920-6.108 us | 112 | 1 |
| Validate default maximum record | 1.163-1.223 us | 0 | 0 |
| Publish immutable active view of 500 records | 49.038-51.252 us | 35,624 | 6 |
| RedisClock upper-bound read | 23.78-24.06 ns | 0 | 0 |
| Generated Data encode | 365.6-387.7 ns | 872 | 5 |
| Generated Data decode | 122.3-136.3 ns | 24 | 2 |
| Generated Data clone | 14.88-15.10 ns | 8 | 1 |

Replacing the generic Go MessagePack event decoder with a bounded flat decoder
changed the paired median from 1,530.5 ns to 892 ns (`-41.72%`), 1,656 to 896
bytes (`-45.89%`), and 26 to 12 allocations (`-53.85%`). The decoder accepts
the protocol's flat scalar envelope, lazily allocates only for unknown control
names, and copies application bytes before returning them.

Generated codecs allocate one shared, capacity-limited encoded-byte slab per
Attr or Data value. Appending to one returned field cannot overwrite a
neighboring field. Fixed integers and floats use canonical big-endian bytes;
bool uses one byte; strings require UTF-8; `[]byte` and `[N]byte` retain exact
bytes. Reflection is absent from codec encode/decode hot paths.

## 9. Redis Sentinel fault qualification

The final line-audited isolated topology completed in 152.842 seconds with:

```text
initial master:            192.168.0.90:16381
acknowledged-loss master:  192.168.0.90:16382
recovered master:          192.168.0.90:16383
Go Selector generations:   [1, 2, 3]
Rust Selector generations: [1, 2, 3]
```

| Fault or invariant | Result |
| --- | --- |
| SDK-specific Go and Rust Sentinel integrations | PASS |
| Separate Redis and Sentinel ACL identities | PASS |
| Minority Sentinel unavailable and stale | PASS |
| Forced acknowledged write lost during promotion | PASS, loss demonstrated |
| Same process UUID and complete desired state republished | PASS |
| Promoted-primary `SCRIPT FLUSH` | PASS |
| All Sentinels unavailable while current primary remains connected | PASS |
| Primary loss while all Sentinels are unavailable | PASS, views became unsynchronized |
| Sentinel restart and second promotion | PASS |
| Go/Rust content and generation convergence | PASS |
| Exact topology cleanup | PASS |

Redis replication is asynchronous. Verdandi does not prevent acknowledged
state from being lost during promotion. A still-running Registration owns its
bounded desired state and confirmation status in process memory and repairs the
new primary with the same UUID and revision when absence or transition is
observed. The SDK writes no Registration recovery state to local disk; process
restart creates a new UUID. This is recoverable coordination, not consensus or
durable history.

## 10. Engineering assessment

Subjective score for the implemented Alpha Registration/Selector slice:
**9.7/10**. This is a code and evidence assessment, not production, security,
or release certification.

| Area | Score | Basis |
| --- | ---: | --- |
| Registration Lua atomicity | 10.0 | every accepted responsibility is operation-specific, line-audited, paired-benchmarked, and fully regression-qualified |
| Protocol/decoder safety | 9.7 | canonical scalars, fixed envelope, strict expansion bounds, saved corpus, 25.4M-case fuzz |
| Go SDK | 9.8 | direct application-owned strong types, delayed readiness, transactional policy API, optimized decoder/coalescer, Linux race evidence |
| Rust SDK | 9.6 | idiomatic `FieldValue`, immutable borrowed policy views, matching lifecycle/failure behavior, and MSRV coverage |
| Selector synchronization | 9.6 | subscribe/scan/PING proof, targeted repair, header reuse, indexed TTL, bounded retained view |
| Sentinel recovery | 9.5 | independent ACLs, demonstrated acknowledged loss, total resolver loss, two promotions, interop |
| Test/operational evidence | 10.0 | authenticated Redis 8.8, explicit SDK/Lua boundary, two toolchains, race, fuzz, Redis-clock-gated two-hour faults, 5,000-scale, two promotions, and raw JSONL |

The Lua `10.0` is an accepted-contract implementation score: no known defect or
unfinished approved Lua responsibility remains. Production-evidence confidence
is now 9.9. The two-hour run covers sustained churn, reconnect storms, cache
loss, pauses, AOF restarts, and post-run Sentinel failover, but only Redis 8.8
supplied line-timing/endurance evidence. Deliberate Redis clock steps, TLS,
managed Redis, wider subscriber counts, and multi-day operation remain
unqualified.

### Principal strengths

- SDKs own all request/schema/capacity validation; Lua is a small atomic glue
  layer and no longer duplicates cross-language business rules.
- Four generated operation scripts share reviewed Redis-state fragments; each
  Registration mutation selects exactly one and remains atomic.
- String kinds, statuses, and error codes remain readable across languages.
- Redis never parses JSON; application values are opaque flat binary fields.
- Update performs no configuration read or complete-record read and transmits
  only changed top-level values.
- Renew changes only Redis time/TTL state; it does not advance content revision
  or rebuild a public snapshot.
- Selector treats Pub/Sub as disposable notification, not authoritative
  history, and has an explicit bounded reconstruction proof.
- Same-revision reconnect scans use only `@revision`/`@timestamp` headers;
  changed content uses `HGETALL`.
- One pending logical change per UUID bounds bursts without creating a fleet
  count ceiling.
- Retained payload provides bounded short-disconnect recovery while remaining
  visibly non-selectable.
- Go and Rust callers use application-owned strong Attr/Data field conversion
  while internal Redis storage stays efficient and patchable.
- Sentinel transition handling repairs live process state without changing its
  UUID.

### Principal weaknesses and trade-offs

- Redis Pub/Sub is non-durable; any gap or reconnect requires targeted repair or
  a new full synchronization generation.
- Direct ACL-authorized `EVALSHA` bypass receives no SDK business validation.
  Malformed input may store unsupported state or raise a Redis script error;
  this is an intentional trust boundary, not defense in depth.
- Same-revision idempotency relies on the SDK invariant that one UUID/revision
  identifies one content state; Lua does not compare a retry's payload.
- Asynchronous Sentinel failover can lose acknowledged writes. Repair requires
  the owning Node process to remain alive.
- Retained state is process-memory-only and non-durable. A hidden explicit
  unregister can be indistinguishable from absence until the retained second
  TTL expires.
- **Historical topology:** this qualification build gave each live Registration
  a writer/renewal worker, was temporarily superseded by a Client coordinator,
  and is not resource evidence for the final independent bounded-queue design.
  Current measurements are recorded in the later per-Registration addendum.
- A visible content change publishes a new immutable O(N) view. Renew is
  optimized away, but high-frequency content changes with very large views may
  become allocation/GC work.
- Application codecs define cross-language bytes and must be tested together;
  the SDK cannot prove semantic compatibility between independently written
  Go and Rust implementations.
- Selection callbacks are serialized per Selector. A slow callback blocks later
  local policy operations, and its deadline is cooperative: Verdandi rejects a
  late result but cannot forcibly stop application code.
- Local Data mutation is a soft process-local prediction. Multiple Selectors
  can temporarily diverge until higher-frequency reports and field-granular
  remote correction converge; it is not a distributed reservation.
- Zone configuration adoption is eventual and jittered, with no configuration
  revision or wake channel.
- Four script SHAs and three generated copies per operation increase bootstrap
  and artifact bookkeeping. The operation split alone was runtime-neutral, but
  the accepted positional/direct-write/line-audited composite is materially
  faster. Its executable implementation historically occupied 11,278 source
  bytes; temporary detailed Chinese maintenance comments make the current four
  generated bodies 14,112 bytes without changing the EVALSHA execution path.
  Deterministic generation, byte-exact checks, and selected-script `NOSCRIPT`
  recovery remain mandatory.
- Maximum-shape correctness is tested, but sustained 64-KiB register fan-out,
  arbitrary Redis clock steps, TLS, managed Redis, wider subscriber counts, and
  multi-day operation remain unqualified.
- At this historical checkpoint Campaign/Leader, desired state, and
  acknowledgements were deferred. Campaign/Leader was later withdrawn on
  2026-09-02. Catalog is functionally and bounded-fault qualified, but its
  24-hour interval remains unfinished and is not a production-readiness claim.

### 2026-08-25 Register/Selector production optimization addendum

The post-qualification production review preserves the four accepted Lua
bodies and public SDK behavior while tightening Go/Rust internal ownership and
size accounting. Go's immutable Selector records now shallow-copy maps rather
than every byte value, transfer decoded ownership, allocate repair work only
when required, reset retry backoff after a successful synchronization, and
cache exact complete-record size. Rust moves detached Update values into the
next writer state, shares immutable internal Data through `Arc`, and maintains
the same exact size cache while keeping public records detached.

Ten-sample WSL/Linux Go comparisons on an i7-13700F used a 750 ms benchmark
interval and `benchstat`:

| Hot path | Before | After | Change |
| --- | ---: | ---: | ---: |
| apply Update | 4.103 us/op | 1.300 us/op | -68.32% |
| apply Renew | 3.083 us/op | 1.236 us/op | -59.90% |
| no-repair pending drain | 38.25 ns/op | 19.18 ns/op | -49.86% |
| Update/Renew memory | 6,984 B/op, 37 allocs | 2,888 B/op, 5 allocs | -58.65% bytes, -86.49% allocs |
| no-repair drain memory | 48 B/op, 1 alloc | 0 B/op, 0 allocs | -100% |

Every comparison reports `p=0.000`, `n=10`. Current source passes Go unit,
ten-repeat shuffled, vet, format, and WSL/Linux race checks; Rust unit,
formatter, all-target/all-feature Clippy with warnings denied, and rustdoc
checks; exact Lua generation/copy checks; and an isolated Redis 8.8 matrix for
the Registration Lua contract, both SDK integrations, ceiling recovery,
lifecycle cleanup, and live Go/Rust interoperability.

Implementation quality is assessed at **9.8/10**: Lua 10.0, Go 9.8, Rust 9.6,
synchronization 9.8, and maintainability 9.8. The later direct typed campaign
raises current-source operational evidence to **9.9/10** after 7,608.409 Redis
seconds, four million Updates, all 34 standalone faults, and the complete
post-soak Sentinel matrix. The separate Catalog interval remains an independent
unfinished qualification and does not affect this Registration score. Full
changes, rejected optimizations, isolation evidence, and weaknesses are in
[`registration/production-review-20260825.md`](registration/production-review-20260825.md).

### 2026-08-26 Registration child-package migration addendum

Registration and Selector now live in `sdk/go/registration` and
`sdk/rust/src/registration`; neither SDK root re-exports their declarations.
Rust's Registration script transport moved with its four Lua embeddings, while
Go uses an internal capability bridge so the shared root Client does not expose
Redis or create an import cycle. The Go generator target, saved fuzz corpus,
standalone/load/soak package selectors, Rust public-Client lifetime accounting,
and endpoint-wide load-test serialization were corrected as part of the move.

The exact final 88-file fingerprint is
`3af6405f80a4ef3af123f4ad143b4714bb0a94c50190ac023ce6356ecf346758`.
Its isolated Redis 8.8 AOF gate ran 500 Registrations and eight Selectors for 90
seconds: 45,000 Updates, one fault retry, zero unexpected asynchronous errors,
Update p50/p95/p99 of 0.668/1.096/1.415 ms, selection p99 of 1.156 ms, three
natural-expiry cycles, nine churn cycles, generation three, goroutines
`2 -> 528 -> 2`, Redis memory change of -3,784 bytes, zero evictions/rejected
connections, and final `DBSIZE=0`. Script flush, Pub/Sub kill, three-second
pause, ordinary connection kill, AOF restart, and a second script flush all
recovered. Canonical Lua and both Rust Registration integration checks passed
after the fault phase.

The final Rust release profile sustained exactly 500.0 Updates/s and 471.7
automatic Renew executions/s for 500 Registrations with eight Selectors; a
5,000-record sync completed in 132.885 ms. The isolated three-Redis/
three-Sentinel matrix preserved Go/Rust UUIDs and advanced both Selector
generations `1 -> 2 -> 3` across acknowledged loss, total Sentinel loss, and a
second promotion. Go full tests/vet, Linux race/shuffle repeats, a 7,342,037-
execution fuzz run, Lua generation/Redis tests, Rust format/all-target tests/
Clippy/rustdoc, and external Sentinel peers pass.

The first complete Sentinel retry timed out at an excluded old primary; cleanup
passed and the next full matrix passed in 33.42 seconds. A deliberately too-
short 30-second fault run completed every injected fault but was rejected for
only one natural-expiry cycle; the two-cycle gate was retained and the 90-
second replacement completed three. Both failures are preserved in the result
set. The exact commands, path review, benchmarks, score, strengths, weaknesses,
and evidence links are in
[`registration/package-migration-20260826.md`](registration/package-migration-20260826.md).
The compact aggregate is
[`testkit/results/registration-package-migration-20260826.json`](testkit/results/registration-package-migration-20260826.json).

## 11. Catalog Value qualification

The production-review harness created isolated authenticated Redis 8.8.0
containers on preflighted ports `36429` and `36430`. It verified an empty
database after every successful matrix and removed only its own randomly
labelled fixture. It did not use or modify any existing Register test endpoint.
Results were:

| Check | Result |
| --- | --- |
| Canonical/Go/Rust Catalog Lua bytes | PASS |
| Raw binary Hash and Stream delta contract | PASS |
| Live-empty Patch, no-op repeat, Delete, and empty resurrection | PASS |
| 4,097-field Patch comparison/write/delete command chunking | PASS |
| No-op and interleaved last-write-wins retry | PASS |
| Whole Delete, tomb version, recreate, and floor trim | PASS |
| Explicit Compact and below-floor full-load fallback | PASS |
| Corrupt metadata, orphan key, and stray tombstone-field rejection | PASS |
| Lua canonical revision through `9223372036854775807` | PASS |
| Go Publisher/Mirror plus bbolt checkpoint restart | PASS |
| Go `Catalog[T]` cached projection and bounded complete-Value differencing | PASS |
| Go bounded MessagePack decoder and declared-allocation-bomb rejection | PASS |
| Go 60-second Catalog decoder fuzz | PASS, 20,405,860 executions |
| Go integration under WSL/Linux race detector | PASS |
| Rust Publisher/Mirror plus redb checkpoint restart | PASS |
| Rust generic `Catalog<T>` and static external `CatalogValue` codec | PASS |
| Rust typed cached/no-op/Compact/Delete/empty-resurrection/close lifecycle | PASS |
| Go/Rust Client-level Stream Hub with simultaneous Mirrors | PASS |
| Per-Mirror gap isolation and one-event acknowledgement backpressure | PASS |
| Missing middle delta and wholly absent Stream tail recovery | PASS in Go and Rust, with bounded diagnostic and full Hash fallback |
| Rust streaming bounded decoder and blocking-XREAD null handling | PASS |
| Shared-store monotonic checkpoint replacement in Go and Rust | PASS |
| Same-Catalog concurrent bbolt replacement regression | PASS, deterministic and Linux race count 10 |
| Mirror close preserves the last snapshot and closes diagnostics | PASS |
| Go writes -> Rust Mirror and Rust writes -> Go Mirror | PASS |
| Catalog Lua 19-scenario paired optimization matrix | PASS, all candidate medians improved |
| 120-second Catalog fault-injected steady gate | PASS, 15,360 Patch operations and 60 complete lifecycle cycles |
| Structured console-interrupt rehearsal | PASS, partial result and all samples retained before exact cleanup |
| Go 1.24 and Rust 1.85 minimum toolchains | PASS |
| Go `vet`; Rust formatter, docs, all targets, and denied-warning Clippy | PASS |

The accepted Catalog Lua review compared cached pre-change SHAs with the final
candidate inside the same Redis 8.8 process over 21 alternating paired trials.
All 19 scenario medians improved. The gains range from 1.69% for absent creation
and 1.79% for a 64-field no-op through 14.08% for a missing 64-field delete and
35.09% for a 19-digit Compact. Functional contracts and canonical Go/Rust
generated bytes passed after promotion. Accepted changes preserve corruption
detection while removing redundant present-state type work, specializing
one-field comparison, avoiding a temporary multi-delete name table, and
incrementing decimal revisions without Lua-number conversion.

The dedicated production-parameter gate used Redis 8.8.0 with AOF enabled,
16 Catalogs, 256 256-byte fields per Catalog, two Mirrors per Catalog, one
Client-level Stream Hub, and one shared bbolt checkpoint. In 120 seconds it
completed 15,360 Patch operations, 60 Delete/complete-restore/Compact cycles,
and 122 full Mirror checks. Script-cache loss, ordinary-connection loss, a
three-second Redis pause, and AOF restart all recovered. There were no retries
or asynchronous errors; Patch p50/p95/p99 were 1/2/2 ms and the 3.168-second
maximum covered the deliberate pause. Stream length peaked at 241, goroutines
returned from 71 to 2, heap returned from 13.48 MiB to 1.19 MiB, the stable
Redis-memory growth gate passed, and final `DBSIZE` was zero before fresh Lua
and Rust checkpoint checks passed.

An earlier steady run exposed a real Go local-store race rather than being
discarded as test noise: concurrent Mirrors replacing the same Catalog across
multiple bbolt transactions could let a faster replacement clean the slower
writer's inactive epoch. A deterministic 32,768-field interleaving test
reproduced the nil-Bucket panic. Full replacement sequences are now serialized
only per `zone + catalog` by a reference-counted keyed lock, with explicit
missing-bucket errors and lock-entry cleanup. Different Catalogs remain
independently scheduled. The regression passes repeatedly under WSL/Linux race.

The final 14-suite matrix processed 482,079 Redis commands, transferred
20,073,842 input and 17,569,192 output bytes, and reached 5,291,816 bytes peak
Redis memory. It included Lua contracts, Go and Rust integration, Linux race,
live Go/Rust interoperability, two 120-second Go load cases, two 120-second
Rust load cases, and 5,000-record recovery in both SDKs. Every suite passed.
These aggregate loads primarily prove that the Catalog changes do not regress
the existing coordination system; they are not a dedicated sustained Catalog
mutation profile.

The Rust typed follow-up used a fresh isolated Redis 8.8.0 fixture on port
36432. Its ten functional suites processed 3,187 commands and reached
4,119,184 bytes peak Redis memory. It added real-Redis `Catalog<T>` publication,
cached `Arc<T>` identity, no-op, immediate floor projection, Delete, live-empty
and non-empty resurrection, close behavior, and simultaneous raw/typed Mirror
coverage. The first multi-Mirror attempt exposed shared blocking-`XREAD`
head-of-line blocking; assigning each Rust Mirror a dedicated Stream-reader
connection fixed the root cause, and the complete matrix then passed with an
empty final database.

The cross-language Stream-Hub follow-up superseded that per-Mirror connection
topology. Go and Rust now multiplex every Client's Catalog streams through one
dedicated blocking reader while all ordinary commands share the Client command
path. Unit tests prove oldest-revision stream coalescing, one-event local
acknowledgement, per-subscription gap detection, and isolation of unrelated
Mirrors. The final Redis 8.8.0 fixture on port 36434 additionally injected a
real revision hole (`1 -> 3`) into both SDK integration paths. Both Mirrors
rejected incremental application, failed exact replay, completed authoritative
Hash recovery, and converged at revision 3. Its 10 Lua, Go, Rust, Linux-race,
lifecycle, typed/raw, and live-interoperability suites processed 3,378 commands
and reached 4,118,720 bytes peak Redis memory before the harness verified
cleanup and removed its isolated fixture.

The production-cleanup follow-up then used fresh port 36436 and extended the
same Go and Rust integration case from revision 3 to revision 4 by changing the
authoritative Hash without appending any revision-4 Stream entry. Periodic
header validation detected the absent tail, exact `XRANGE` returned empty, each
Mirror emitted one bounded `corrupt/catalog_stream` diagnostic, and immediate
continuity recovery completed the authoritative `HSCAN` fallback. Both Mirrors
converged to revision 4. The 10-suite final-code matrix, including WSL/Linux
race and live interoperability, processed 3,669 Redis commands and reached
4,119,392 bytes peak Redis memory. The harness removed its exact container and
an independent probe confirmed port 36436 was closed.

The final Go load held 500 live Registrations at 500 updates/s for 120 seconds:
operation p95 was 0.991 ms, p99 1.337 ms, and maximum 4.356 ms. Its renewal
case sustained 497.7 calls/s. Rust held the same update rate with operation p95
0.937 ms, p99 1.279 ms, and maximum 3.257 ms; renewal sustained 493.8 calls/s.
Go and Rust 5,000-record recovery synchronized in 66.1 ms and 122.4 ms,
respectively. These values are end-to-end qualification observations, not a
language comparison because the clients use different build/runtime paths.

Paired WSL/Linux Go microbenchmarks on the same i7-13700F measured the internal
Patch apply median improving from 110.65 to 94.815 ns/op (`-14.3%`), from 16 to
3 bytes/op, and from 2 to 1 allocation/op after transferring decoded value
ownership directly into Mirror state. The 1,000-field decoder median improved
from 82.109 to 72.224 us/op (`-12.0%`) and from 171,585 to 122,433 bytes/op
(`-28.6%`) after removing the intermediate `[][]byte`; it sustains about
249 MB/s on that fixture. A one-field difference in a 1,000-field typed Value
remained effectively unchanged at about 38.8 us/op, 432 bytes/op, and 3
allocations/op.

The final Go Hub cleanup separately measured its 1,000-Mirror position-planning
substep on Windows/amd64. Reusing the map and work slices reduced the paired
working-session median from about 87.56 to 26.66 us/op (`-69.6%`) and from
108,760 bytes/op with 20 allocations/op to zero bytes and zero allocations.
This isolates local plan bookkeeping; it is not an end-to-end Redis `XREAD`
latency or a large-fan-in capacity result.

### Catalog engineering score

Subjective score for the reviewed Catalog slice: **9.7/10 before the 24-hour
interval completes**. This is an open-source implementation and evidence score,
not production, security, or release certification.

| Area | Score | Basis |
| --- | ---: | --- |
| Protocol and invariants | 9.5 | opaque raw Value, exact signed-int64 revision, explicit tomb/floor relations, middle/tail gap detection, replay/full fallback |
| Lua atomic glue | 10.0 | all accepted responsibilities line-audited, deterministic, same-fixture paired, and functionally regressed |
| Go SDK | 9.7 | generic `Catalog[T]`, bounded decoder/fuzz, Stream Hub, deterministic checkpoint-race regression, Linux race evidence |
| Rust SDK | 9.4 | static `Catalog<T>`, O(1) `Arc<T>` snapshots, bounded diff/decoder, Client-level Stream Hub, monotonic redb, Rust 1.85 |
| Test and operational evidence | 9.6 | Redis 8.8 integration, typed/raw interop, real gaps, race/fuzz/MSRV, fault gate and interrupt rehearsal; 24 hours unfinished |
| Documentation and maintainability | 9.5 | canonical sources and generated copies, explicit trust/recovery contract, executable evidence paths, narrow public APIs |

Principal strengths:

- Redis stores one opaque binary Value; concrete schemas and batching remain
  external SDK/application concerns.
- Every accepted write has one Redis-owned global revision, deterministic LWW
  behavior, a replay delta, and an explicit tomb/floor transition.
- Lua stays an atomic storage adapter while SDKs own validation, differencing,
  type projection, recovery, and optional persistence.
- Decoders reject non-canonical shapes, oversized declared containers, trailing
  bytes, duplicate/overlapping fields, and impossible metadata before applying
  state.
- Mirrors always hold a complete in-memory Value. Optional local databases are
  default-off acceleration checkpoints, monotonic across multiple Mirrors, and
  never authoritative over Redis.
- Cross-language behavior is proven live in both directions, including delete,
  empty resurrection, compaction, checkpoint restart, middle/tail Stream loss,
  and `MAX_INT64`.

Principal weaknesses and trade-offs:

- Publisher-split batches are independent LWW writes. Concurrent publishers can
  interleave those batches and produce a valid hybrid Value; there is no
  complete-Value transaction or CAS across batches.
- Redis executes each Lua mutation on its single command thread and the Stream
  stores the full delta. Very large individual batches increase temporary
  memory, network, and shard-blocking time even though Redis command arguments
  are chunked internally.
- Raw Snapshots and Go detached typed snapshots clone the complete Value. Rust
  typed snapshots clone `Arc<T>` in O(1), but retaining old snapshots also
  retains those complete typed revisions in memory.
- An enabled checkpoint writes on the Mirror recovery path and therefore adds
  local-disk latency. It improves restart time but is not a durable source of
  truth.
- Rust static generic specialization increases compile time and machine-code
  size per external type, and it still lacks a dedicated Criterion-style
  Catalog benchmark harness.
- Each Client pays for one dedicated blocking Catalog Stream-reader connection.
  Mirror count no longer grows Redis connection count, but a very large number
  of active Catalog keys enlarges each dynamic `XREAD` and its dispatch table;
  this fan-in still needs a dedicated scale profile. Go now reuses its position
  plan storage, while Rust still rebuilds its ordered request plan per read.
- If a Stream delta is genuinely missing, the global-revision Hash has no
  per-field revision from which to request a narrower authoritative diff. The
  SDK must perform a complete bounded `HSCAN` comparison before catch-up.
- Client timeouts after an accepted Redis write remain ambiguous. The caller
  must reconcile through Mirror state/revision rather than assume failure.
- Redis Cluster, TLS/managed Redis, broader reconnect churn, extreme Stream-Hub
  fan-in, and the complete 24-hour Catalog interval remain unqualified.

## 12. Reproduction

Commands, environment variables, isolation rules, and both fixture harnesses
are maintained in [`testkit/README.md`](testkit/README.md). Future results must
record client OS/runtime, Redis version/configuration, topology, payload shape,
fan-out, build profile, and duration. A later run may supersede a named result
only by stating why; it must not silently reinterpret or erase prior evidence.

## 13. Direct typed Registration and transactional Selector API

The 2026-08-25 API revision removes Go business-schema generation. Go Attr and
Data values implement `FieldEncoder`/`FieldDecoder`; Rust values implement
`FieldValue`; raw `Fields` follows the same generic path. Construction is local,
Register is the readiness boundary, and typed Update keeps a fixed Data field
shape while preserving patch-proportional Redis writes.

Verification covers:

- local construction, inferred Go pointer decoders, stable 32-character UUID,
  and terminal behavior before Register;
- detached raw Fields ownership without byte-slice aliasing;
- synchronous One/Any commit and same-callback staged visibility;
- rollback on callback error, context cancellation/deadline, duplicate Any
  result, foreign result, and forbidden direct Go borrowed-view mutation;
- field-granular reconciliation across Renew, an unrelated remote Data update,
  and a later authoritative update to the locally predicted field;
- compilation of integration/load/soak-tagged Go consumers and both Go/Rust
  testkit programs; and
- authenticated isolated Redis 8.8 live publication, selection, prediction,
  remote correction, Unregister, and cleanup in both languages.

The Go WSL/Linux 500-candidate benchmark scans the borrowed ordered view,
chooses minimum Power, stages `Power++`, commits the overlay, and detaches the
selected result. Ten final `b.Loop` samples measured 19.945-21.726
microseconds/op with a 21.313-microsecond median, 3,881 bytes/op, and 43
allocations/op. Removing the redundant clone of the encoder-owned destination
reduced the comparable ten-sample median by 4.57%, bytes by 24.21%, and
allocations by 20.37%. This is the complete policy transaction, not a Redis
round trip.

The direct typed production qualification then passed against isolated Redis
8.8.0 with a 58-file source fingerprint. Redis `TIME` measured 7,608.409
seconds against a hard 7,200-second floor. The workload completed 4,000,000
typed Updates across 500 Registrations, 639,713 `One`/`Any` transactions across
eight Selectors, and 703,688 committed local mutations. Final typed snapshots
proved exact remote Data plus field-granular local `Power` preservation; final
revision was 8,001 and final generation was 15.

All 34 standalone faults passed: 14 `SCRIPT FLUSH`, 11 Pub/Sub connection
kills, four three-second Redis pauses, three AOF restarts, and two ordinary-
connection kills. Update p50/p95/p99 were 0.665/0.933/1.239 ms; selection
p50/p95/p99 were 0.223/0.381/0.623 ms. Natural expiry completed 27 cycles over
3,456 records and explicit churn completed 27 cycles over 432 records. There
were 612 expected transient diagnostics and zero unexpected asynchronous
errors.

Redis stable memory grew 556,728 bytes against the 2 MiB gate; evictions and
rejected connections stayed zero. Go goroutines returned from a fault-recovery
peak of 1,553 to the initial two, and final Redis `DBSIZE` was zero. Canonical
Lua, Rust raw and typed convergence, and the complete Go/Rust two-promotion
Sentinel matrix all passed afterward.

The first 7,500-Go-second attempt remains recorded as failed because WSL moved
its clock forward and Redis had accumulated only 7,138.759 seconds. The
accepted 8,000-second rerun experienced the same environment issue but had
already exceeded the Redis-time floor. No client-only clock is used to claim
the two-hour result.

The API and exact ownership, rollback, deadline, reconciliation, and raw Fields
contracts are documented in
[`registration/api.md`](registration/api.md). No Lua wire
format changed, no Catalog runtime or test endpoint was used, and no commit or
push was created. Exact results, score, strengths, and limitations are in
[`registration/typed-soak-20260825.md`](registration/typed-soak-20260825.md).

## 14. Historical Client-level Registration coordinator qualification

Date: 2026-08-26

This section records the now-superseded Client-level Registration coordinator.
It owned one bounded request FIFO, indexed renewal heap, and timer per Client.
Its protocol, Lua, fault, and Sentinel results remain useful history, but its
worker/resource measurements do not describe the final per-Registration
queue/worker/timer design. Each Selector already used the retained one persistent
listener plus one optional temporary synchronization task model.

Qualification passed:

- Go formatting, unit tests, `go vet`, and WSL/Linux race detection;
- Rust formatting, 43 unit tests, all targets, strict Clippy with warnings
  denied, and rustdoc;
- deterministic Registration Lua generation, authenticated Redis 8.8
  integration, raw/typed Go and Rust convergence, and live interoperability;
- a 500-Registration/eight-Selector load gate at 500 Updates/s in both SDKs;
- a current-source Redis-time-gated endurance run and all post-run checks; and
- a three-Redis/three-Sentinel, two-promotion Go/Rust recovery matrix.

The authoritative run used the 58-file source fingerprint
`24da63ef5d057bf6b2410cbd5e35f491421e8512fb5bdb5b180d0253cfd3b601`.
Redis `TIME` measured 7,388.601 seconds, above the hard 7,200-second floor. It
completed 4,000,000 Updates, 639,722 selection transactions, 703,698 committed
local mutations, 25 natural-expiry cycles, and 27 explicit churn cycles. Final
revision/generation were exactly 8,001/15; Go goroutines were `2 -> 30 -> 2`;
203 expected transient diagnostics and zero unexpected asynchronous errors were
observed.

All 34 standalone faults passed: 14 `SCRIPT FLUSH`, 11 Pub/Sub connection
kills, four three-second pauses, three AOF restarts, and two ordinary-connection
kills. Update p50/p95/p99 were 0.663/1.168/1.548 milliseconds and selection
p50/p95/p99 were 0.231/0.450/0.739 milliseconds. Redis stable memory grew only
28,300 bytes against the 2-MiB gate; evictions and rejected connections remained
zero, and final `DBSIZE` was zero. The Sentinel matrix then passed in 90.444
seconds with Go and Rust generations `1 -> 2 -> 3`.

Source audit and qualification found and fixed three redesign-specific issues:
Rust closed-handle deadline nodes now leave an indexed heap immediately, a
saturated destructor path uses one coalesced abandoned-handle notification
instead of leaking until a future renewal, and targeted Selector repair no
longer falsely advances generation. The rejected 722-second pre-fix run is not
counted as evidence.

Structured evidence is in
[`registration-concurrency-redesign-soak-2h-final-20260826.json`](testkit/results/registration-concurrency-redesign-soak-2h-final-20260826.json),
its
[`sample stream`](testkit/results/registration-concurrency-redesign-soak-2h-final-20260826-samples.jsonl),
and the
[`Sentinel result`](testkit/results/registration-concurrency-redesign-sentinel-2h-final-20260826.json).
The current superseding topology and final assessment are in
[`registration/concurrency-review-20260826.md`](registration/concurrency-review-20260826.md).
The test owned isolated Redis/Sentinel ports, cleaned them afterward, did not
use the Catalog test endpoint, and created no commit or push.

## 15. Historical per-Registration queue/worker/timer qualification

Date: 2026-08-26. The ownership and sole-writer evidence remains valid, but the
256-request queue representation was superseded by the 2026-08-28 single-slot
Fields mailbox. The following numbers describe only that frozen historical
source.

The then-final Go and Rust design gave every successfully published Registration
one independent 256-entry queue, one long-lived synchronization worker/task,
one desired/confirmed state, and one renewal timer. Consecutive Updates coalesce
in that Registration's admission order. Every Selector keeps one persistent
listener/state-machine worker and at most one temporary synchronization/repair
worker. Public active, retained, Snapshot, Find, One, and Any APIs return
explicit `unavailable` while the view is half-synchronized.

Current-source gates passed before the formal two-hour run:

- Go unit, vet, tagged compilation, WSL/Linux race integration, and Rust 44
  unit tests, formatting, strict Clippy, and rustdoc;
- Redis 8.8 lifecycle, Update-reset-Renew timing, configuration refresh,
  protocol ceilings, Go/Rust interoperability, and canonical Lua identity;
- final-source 30-second loads at 500.0 Go and 499.8 Rust Updates/s with eight
  Selectors; Update p99 was 1.324 ms for Go and 1.585 ms for Rust;
- 5,000-record synchronization in 64.76 ms for Go and 113.52 ms for Rust;
- numeric ownership evidence of Go goroutines `2 -> 513 -> 4` and Rust Tokio
  tasks `5 -> 521 -> 1/2` with 500 live Registrations;
- a 210-second, 105,000-Update six-fault run with zero unexpected asynchronous
  errors, Go goroutines `2 -> 529 -> 2`, stable Redis memory, and final
  `DBSIZE=0`; and
- an independent final-source 63.970-second Go/Rust Sentinel matrix with two promotions and
  both Selector generations `1 -> 2 -> 3`.

The 256-entry queue plus removal of a redundant typed-Update byte copy reduced
observed comparable 500-Registration preflight peak Go heap from 160,098,688 to
a 35,014,568-47,374,384-byte range (70.41-78.13%) without weakening throughput
or fault recovery. The frozen 58-file source fingerprint is
`c7bef517173b9c298e41b6dac272e78736b317c017bbe70ba838185960bdf63a`.

Structured evidence:

- `testkit/results/registration-per-instance-reply-validation-load-20260826.json`;
- `testkit/results/registration-per-instance-reply-validation-soak-preflight-20260826.json`
  and its `-samples.jsonl` stream; and
- `testkit/results/registration-per-instance-reply-validation-sentinel-20260826.json`.

Before the formal run, a frozen-source audit found and fixed missing Go
validation of successful Update/Renew reply revision and timestamp fields. The
pre-fix run was rejected. Both SDKs now treat post-dispatch `corrupt` and
`ambiguous` outcomes as uncertain and repair the retained complete desired state
with a later full Register. Unit and live false-success regressions pass.

The authoritative two-hour qualification then passed on that exact fingerprint:

- Redis `TIME`: **7,866.527 seconds** against a 7,200-second hard floor;
- workload: **4,000,000** typed Updates across 500 Registrations, seven retries,
  and exact final revision 8,001;
- Update p50/p95/p99: **0.649/1.044/1.427 ms**;
- Selector work: **639,704** transactions, 703,672 local mutations, selection
  p99 **0.845 ms**, and final generation 15;
- lifecycle: 25 natural-expiry cycles over 3,200 records and 27 explicit churn
  cycles over 432 records;
- faults: **34/34 passed** — 14 `SCRIPT FLUSH`, 11 Pub/Sub connection kills,
  four three-second pauses, three AOF restarts, and two ordinary-connection
  kills;
- diagnostics/resources: 212 expected transient errors, zero unexpected async
  errors, Go goroutines `2 -> 529 -> 2`, zero Redis sample failures, zero
  evictions, zero rejected connections, and final `DBSIZE=0`; and
- Redis stable-memory median changed from 2,441,152 to 2,385,312 bytes
  (**-55,840 bytes**) under the 2 MiB growth gate.

Canonical Lua, Rust raw convergence, and Rust typed convergence passed after the
soak. The automatic Sentinel tail passed in **39.690 seconds** with two
promotions, acknowledged-write loss, total Sentinel loss/restart, full-state
republish, stable UUIDs, and Go/Rust Selector generations `1 -> 2 -> 3`.

The Go test process's 179,485,736-byte heap peak is not an SDK steady-state
number. Exact percentile instrumentation retains two four-million-element
duration arrays (64,000,000 bytes) and 639,704 selection durations (about
5.1 MiB); this also explains the 71,985,416-byte final heap while those samples
remain live. The shorter equal-topology preflights are the comparable queue/SDK
resource evidence. O(operation count) duration storage remains a test-harness
scalability limitation.

Final structured evidence:

- `testkit/results/registration-per-instance-soak-owned-update-2h-20260826.json`;
- `testkit/results/registration-per-instance-soak-owned-update-2h-20260826-samples.jsonl`;
  and
- `testkit/results/registration-per-instance-sentinel-owned-update-2h-20260826.json`.

The final reviewed score is **9.8/10**. Component scores, strengths, trade-offs,
and the exact interpretation of Lua's scoped 10.0/10 are recorded in
`registration/concurrency-review-20260826.md`.

## 16. Lazy Catalog Stream Hub qualification

Date: 2026-08-26

Go and Rust Client construction no longer creates the Catalog Stream Hub.
Catalog Patch/Delete/Compact remain on the ordinary command path and do not
initialize it. The first Catalog Mirror initializes exactly one blocking reader
for the Client; simultaneous first Mirrors converge on that same Hub, later
Mirrors reuse it, and Client shutdown joins it only when present.

Final gates passed:

- Go formatting, unit tests, vet, and WSL/Linux race detection;
- Rust formatting, 44 unit tests, all targets, strict Clippy, and rustdoc;
- explicit Go assertions for no Hub after Client open or Catalog Patch;
- concurrent first-Mirror construction and shared-Hub behavior in Go and Rust;
- authenticated Redis 8.8 Registration and Catalog Lua contracts;
- complete Go integration and WSL/Linux race integration;
- Rust Registration, renewal, live configuration, shutdown, protocol-ceiling,
  and Catalog integration; and
- live Go/Rust Registration and Catalog interoperability.

The accepted fixture used isolated port 16420. It reported Redis 8.8.0,
3,284 processed commands, 4,102,808 bytes peak Redis memory, and no remaining
keys. The owned container was removed and the port was verified closed. The
first concurrent-Mirror preflight is rejected evidence because its newly added
Rust `shadow` fixture omitted key cleanup; the cleanup was fixed before the
entire final suite was rerun.

Machine-readable evidence is
[`testkit/results/catalog-hub-lazy-20260826.json`](testkit/results/catalog-hub-lazy-20260826.json).
This initialization/lifecycle change does not alter Registration/Selector Lua,
the Selector Pub/Sub topology, Catalog steady-state dispatch, or wire formats;
no new load or endurance claim is made. No commit or push was created.

## 17. Registration-owned configuration refresh and Selector RedisClock

Date: 2026-08-26

Go and Rust Client open now performs only the required bootstrap configuration
read. It creates no configuration polling worker. Register refreshes
synchronously before validating content; the first successful publication
starts one Client-shared poller, additional Registrations share it, and the last
Registration cancels and joins it. Explicit refresh remains available while the
reference count is zero.

Each Selector still calibrates a connection-generation RedisClock during full
synchronization and periodically samples Redis `TIME` at `ClockRefresh` from its
existing persistent listener/state-machine task. Selector construction no
longer refreshes Registration deployment limits. No additional clock goroutine
or Tokio task is necessary.

Final gates passed:

- Go reference-count lifecycle regression, formatting, unit tests, vet, tagged
  integration compilation, and WSL/Linux race integration;
- Rust formatting, 44 unit tests, all targets, and strict Clippy;
- live Go and Rust proof that an active Selector increases Redis `TIME` calls
  while changed Registration limits remain at the bootstrap snapshot;
- immediate policy adoption at Register, periodic valid/invalid policy handling
  while Registration is live, stopped adoption after its close, and successful
  explicit refresh afterward;
- authenticated Redis 8.8 Registration and Catalog Lua contracts, complete
  Go/Rust integration and shutdown tests, and live interoperability; and
- the isolated three-Redis/three-Sentinel matrix, including acknowledged-write
  loss, complete Sentinel outage, two promotions, stable UUIDs, and Go/Rust
  Selector generations `1 -> 2 -> 3`.

The accepted isolated fixture used port 16421, processed 3,805 Redis commands,
reported 4,102,712 bytes peak Redis memory, left no keys, removed its owned
container, and left the port closed. Machine-readable evidence is
[`testkit/results/client-worker-lifecycle-20260826.json`](testkit/results/client-worker-lifecycle-20260826.json).
The Sentinel rerun completed in 31.813 seconds; its evidence is
[`testkit/results/client-worker-lifecycle-sentinel-20260826.json`](testkit/results/client-worker-lifecycle-sentinel-20260826.json).
An immediately preceding attempt is retained as a failed qualification: after
the total-Sentinel outage, Sentinel itself continued to report the excluded
dead primary for the complete 60-second second-promotion window. The harness
still removed every owned container and closed all six ports. The independent
rerun then passed without a source change, so this is recorded as a transient
Sentinel election timeout rather than an SDK pass or a reproducible SDK
regression.

No wire format, Lua program, Catalog behavior, Selector Pub/Sub topology, load
claim, or endurance claim changed. No commit or push was created.

## 18. Current Catalog Replace/Patch/Delete qualification

Date: 2026-08-27. This section supersedes Section 11 and Section 16 for current
Catalog behavior; those sections remain historical evidence for the removed
Hash/Stream design.

The current source passed:

- byte-identical six-script generation and Redis 8.8 Lua contract, including
  strict ABI, corruption, Pub/Sub, tombstone/floor, and `2^53-1` boundaries;
- Go Catalog unit/property tests, ten shuffled repetitions, goleak, vet,
  60-second decoder fuzzing, WSL/Linux race with authenticated Redis, remote
  integration, forced notification-baseline repair, Pub/Sub reconnect, and
  full/delta/below-floor checkpoint restart;
- Rust Catalog unit tests, check, strict clippy/rustdoc, remote `catalog_v2`
  integration, forced Pub/Sub reconnect, and monotonic redb regression;
- bounded streaming MessagePack parsers in both languages, including malicious
  declared-length allocation cases; and
- six-revision live Go/Rust Catalog interoperability with both peers
  publishing Replace/Patch/Delete and binary fields, without executing the
  unrelated Registration test flow; and
- a Catalog-only three-Redis/three-Sentinel matrix with two primary
  promotions, script-cache loss, final revision/content convergence, and
  complete cleanup.

The current 2,000-iteration sequential Redis 8.8 benchmark measured small
Replace at 1,492.0 operations/s (p95 1,147.8 us), one-field Patch at 1,273.6/s
(p95 1,262.0 us), full 64-field Read at 1,838.8/s, one-field delta Read at
2,953.4/s, unchanged Read at 3,622.8/s, Delete at 1,567.1/s, and 256-field
Replace at 450.8/s (p95 2,952.8 us). This is a shared-host wall-latency
snapshot, not a concurrent capacity promise. See
[`catalog-benchmark-qualification-20260826.json`](testkit/results/catalog-benchmark-qualification-20260826.json).

The formal isolated AOF endurance result is
[`catalog-soak-2h-redis-clock-20260826.json`](testkit/results/catalog-soak-2h-redis-clock-20260826.json).
The workload used 16 Catalogs, 256 fields of 256 bytes, two Subscribers, and a
128-attempt/s target. Redis `TIME` measured 7,201.265 seconds on the frozen
72-file fingerprint
`bb994a871b4d9c4679e8ecf800cba1c6d4f37df8ab25a67dfc8d76d161e68cd1`.

The driver made 960,000 attempts: 937,468 Patch, 18,768 Replace, and 3,760
Delete commits. One transient result and three stale retries occurred only at
the deliberate AOF restart boundary and were repaired from authoritative
revisions. All 18 planned script-cache, Pub/Sub, ordinary-connection, pause,
and restart faults passed; no unexpected asynchronous error occurred. Mutation
p50/p95/p99 were below 2.10/4.20/8.39 ms. Stable-window Redis memory growth was
19,560 bytes with zero eviction or rejected connection. A fresh Subscriber
matched every final complete value, Lua/Rust/Go-Rust post-checks passed, and
final `DBSIZE` was zero.

Both Subscribers in this historical run shared the persistent Catalog Client,
so every checkpoint update was applied twice. This does not invalidate its
mutation, recovery, convergence, or Redis-health result, but its local-disk
pressure is not representative. The corrected harness persists one Subscriber
by default and keeps the remaining fanout in memory.

Go integration statement coverage is 74.5%. The two-hour result does not imply
a completed 24-hour campaign or equal-duration Rust-specific mutation load.
Current assessment and remaining policy decisions are recorded in
[`catalog/optimization.md`](catalog/optimization.md).

## 19. Current Registration/Selector hot-path optimization

Date: 2026-08-26, with the complete endurance gate finished 2026-08-27. The
exact 88-file source fingerprint is
`feb767345b8b09323d53dea9c3ead5427be21ece7de668c55e9577eedf5173b0`.

Go and Rust now retain ordered shared record references in immutable Selector
views, skip overlay reconciliation when the published view identity has not
changed, and reuse transaction-local generation marks for `Any` duplicate
detection. Lua remained byte-identical after audit.

Ten-sample WSL/Linux `benchstat` comparisons on an i7-13700F measured:

| Go hot path | Baseline | Final | Change |
| --- | ---: | ---: | ---: |
| publish 500-record view | 105.68 us | 54.08 us | -48.83%, p=0.000 |
| typed `One` over 500 | 26.54 us | 15.92 us | -40.02%, p=0.000 |

Publication memory fell 11.50% to 31,528 bytes/op. Final typed `Any` selecting
eight of 500 measured 19.69 us, 14,721 bytes/op, and 154 allocations/op; no
baseline percentage is claimed because the initial comparison was contaminated
by host load.

The exact fingerprint passed Lua generation, Go unit/vet, ten shuffled Linux
race repetitions, a 30-second fuzz run with 7,658,786 executions, Rust's 28
Registration unit tests, format/strict-Clippy/rustdoc, and focused ordering,
identity, duplicate, and wraparound regressions.

The isolated Redis 8.8 AOF gate ran 500 Registrations and eight Selectors for 90
seconds: 45,000 Updates, no Update retry, 7,137 selection transactions, update
p50/p95/p99 0.702/1.227/1.701 ms, selection p50/p95/p99
0.187/0.482/1.094 ms, six passed faults, zero unexpected asynchronous errors,
stable memory, and final `DBSIZE=0`. The independent two-promotion Sentinel
matrix preserved both UUIDs and advanced both Go and Rust Selector generations
`1 -> 2 -> 3`. Cleanup left no owned container, directory, listening test port,
or Redis key.

The separate current scores are **Lua 10.0/10**, **Go 9.8/10**, and **Rust
9.7/10**. Lua's 10 is scoped to its accepted atomic-glue responsibilities; Go's
principal deduction is typed-result allocation and bridge/state-machine
complexity; Rust's is the absence of allocator-counted statistical benchmarks
and its large Selector module. The complete scoring rubric, strengths,
weaknesses, and evidence links are in
[`registration/optimization-review-20260826.md`](registration/optimization-review-20260826.md).

The exact fingerprint then passed the complete regression and formal two-hour
gate. An 8,000-second workload measured 7,759.124 Redis seconds against the
7,200-second floor, completed 4,000,000 Updates and 639,743 selection
transactions, passed 34/34 faults, 25 expiry cycles, 27 churn cycles, zero
unexpected asynchronous errors, stable Redis memory, and final `DBSIZE=0`.
Goroutines returned `2 -> 528 -> 2`; final revision/generation were 8,001/15.
Canonical Lua plus Rust raw and typed convergence passed after the workload.

A preceding 7,200-second workload remains rejected evidence: its functional
workload and 34 faults passed, but only 6,984.007 Redis seconds were measurable,
below the explicit floor. Two Sentinel attempts also remain rejected evidence;
they exposed a harness race between first promotion and surviving-replica
discovery. A new `ROLE` plus three-Sentinel `SENTINEL REPLICAS` readiness gate
removed that race, after which the full matrix passed in 34.503 seconds with
both SDK generations `1 -> 2 -> 3`.

Complete evidence and interpretation are in
[`registration/full-regression-2h-20260827.md`](registration/full-regression-2h-20260827.md)
and
[`registration-production-optimization-full-regression-20260827.json`](testkit/results/registration-production-optimization-full-regression-20260827.json).

No Catalog source or test changed. No commit or push was created.

## 20. Catalog eight-hour campaign disposition

Date: 2026-08-27. The requested continuous eight-hour qualification is **not
complete** and is not counted as a pass.

The first attempt reached 27,907.583 Redis seconds, 3,761,920 attempts, and
73/73 reached faults before a WSL-local `duration + 10m` context expired. All
observed workload and Redis health gates remained stable, but the attempt is
rejected because it stopped 892.417 Redis seconds before the 28,800-second
floor and did not run final post-checks.

The driver was corrected so Redis `TIME`, rather than a VM-local duration
context, owns workload completion. A complete 121.195-Redis-second preflight
then accepted all 16,384 attempts, passed all six fault classes, converged,
passed Lua/Rust/Go-Rust post-checks, and left `DBSIZE=0`.

The corrected eight-hour rerun was stopped at the user's request after at least
4,384.824 Redis seconds. It reached 597,760 attempts, passed all 10 reached
faults, and had only five stale retries plus one transient result around the
deliberate AOF restart. Redis stable-window memory growth was 24,508 bytes with
zero eviction, rejected connection, or blocked client. Final convergence and
post-checks were not run because of the interruption. The owned Redis container
and data directory were removed manually after the outer terminal interrupt,
and port 36440 was verified closed.

The exact chronology, metrics, correction, evidence links, and final scoring
are in [`catalog/soak-8h-20260827.md`](catalog/soak-8h-20260827.md). No
Registration test or runtime state was used or changed during this campaign.

Post-campaign review found that the two Subscribers shared one persistent
Client and duplicated every synchronous checkpoint update. The previous runs
remain functional evidence but must not be used as local-disk performance
evidence. The harness now exposes `--persistent-subscribers`, defaults it to
one, and places all remaining Subscribers on a memory-only Client. No new long
test was started; the correction awaits review.

## 21. Lua/Go/Rust production-source review

Date: 2026-08-27. This review sought smaller, clearer production code without
changing protocol bytes, public API behavior, ownership, or revision and
concurrency guarantees.

The canonical Catalog and Registration Lua fragments were inspected line by
line and intentionally left unchanged. Replace/Patch specialization prevents
new Lua call overhead in the largest field loops; incremental Redis calls avoid
building argument tables that can exceed the Lua stack. Lease fencing, delete
retention pruning, and post-read lock checks remain irreducible protocol work.

Go now avoids a second clone for built-in Redis scalar encodings, but preserves
detachment for application-owned marshalers. Catalog status transitions use a
struct copy, field cloning has one implementation, version-only Registration
events no longer copy Data, and Selector update application has one checked
body. Test-only raw Registration compatibility helpers moved out of production
source. The review also corrected an empty-value map-comparison defect where a
missing key could compare equal to a present empty value.

Rust now fills encoded Fields directly, builds Fred multi-key arguments from
iterators, avoids duplicate key validation and configuration-field vectors,
reuses hexadecimal encoding, and centralizes successful desired-state commits.
Selector update application is shared while live-contiguous and pending-base
revision checks remain explicit at their callers.

The paired WSL/Linux Go benchmark samples retain the same allocation counts.
The sampled Catalog validation/event decoding and Registration event/update/
renew paths overlap their baselines; no measured performance regression or
statistically defensible speedup is claimed. The built-in Go scalar encoding
change removes one full byte-slice copy by construction but does not yet have a
dedicated benchmark.

Final short regression evidence is
[`testkit/results/source-review-regression-20260827.json`](testkit/results/source-review-regression-20260827.json):
13/13 suites passed on Redis 8.8.0, with 4,579 commands, 3,050,104-byte peak
Redis memory, and zero background-thread exceptions. This run used a unique
container, credentials, Zone, and cleanup path; it did not touch a shared
Registration fixture and did not start a long test.

## 22. Shared JSON configuration and Catalog lease correction

Date: 2026-08-29.

The same complete [`configuration.example.json`](configuration.example.json)
was loaded by the strict Go and Rust configuration layers. Tests cover default
materialization, explicit zero semantics, Standalone and Sentinel conversion,
unknown/duplicate/null/trailing JSON rejection, bad address forms, unsupported
versions, invalid zero timeout, and the 1-MiB source bound. The expanded TLS
object is covered on both sides for private roots, paired client credentials,
Standalone SNI, deferred certificate I/O, relationship errors, legacy-boolean
rejection, and the 1-MiB bound on each PEM file.

Short source verification passed:

- Go `go test ./...` and `go vet ./...`;
- Rust 68/68 library tests plus 4/4 offline external tests;
- Rust format, all-target/all-feature Clippy with `-D warnings`, and rustdoc with
  `-D warnings`;
- Registration and Catalog generator `--check`;
- standard JSON syntax parsing for the schema and complete example.

Twelve existing Rust tests that explicitly require isolated Redis/Sentinel
environment variables remained ignored in this short review run. No long soak,
live TLS handshake, or failover qualification was claimed.

The canonical Catalog Redis 8.8.0 protocol suite ran against database 15 on the
existing test Redis endpoint, using a randomized `CatalogTest...` Zone. It
passed lock fencing, Replace/Patch/Delete, Pub/Sub, tombstone floor,
`2^53-1`, and the new encoded-byte boundary. Specifically, a Value whose
encoded length exceeded the former 524,288-byte Lua ceiling succeeded; a
declared length above 4,194,304 was rejected, the error path removed its lock,
and the prior Value remained unchanged. Cleanup verification found zero keys
matching the owned test prefix.

The complete interpretation, revised scores, and remaining design choices are
in [`configuration-review-20260829.md`](configuration-review-20260829.md).
No commit or push was created.

## 23. Lock-free Catalog and shared-lifecycle optimization regression

Date: 2026-08-30.

Catalog now has four generated scripts and no external Path lock. Replace and
Delete are one atomic Redis-primary operation; Patch performs unlocked SDK
projection and an exact-base Lua commit. A real two-writer contention test
requires one success, one stale result, final revision 2, and one intact final
value. Go and Rust Publisher values are stateless, and at this historical
checkpoint Go Catalog Subscriber owned one Pub/Sub reader plus one repair
worker; section 24 supersedes that task lifetime.

Go Registration and Catalog now share internal lifecycle and validation
primitives. Rust shares one crate-private Activity/RAII Guard implementation.
Both SDKs consume the same binary Catalog event and strict JSON configuration
conformance vectors. Optional bbolt/redb Catalog recovery remains unchanged.
This paragraph records the 2026-08-30 Go/Rust state; section 24 adds C++ and
supersedes the retained repair-worker topology.

Final static and offline gates passed:

- Registration and Catalog generated-source checks plus JSON syntax checks;
- all Go packages, `go vet`, and WSL/Linux race detection;
- Rust 71 library tests and four offline external tests;
- Rust all-target/all-feature strict Clippy and warning-denied rustdoc;
- Rust 1.85 minimum-toolchain all-feature check.

The authenticated isolated Redis 8.8.0 functional matrix passed both Lua
contracts, standalone Go/Rust integrations, root Redis APIs, exact-base
contention, script reload, maximum safe revision, 4-MiB Catalog values, and
Go-to-Rust plus Rust-to-Go Registration/Catalog interoperability. It processed
4,305 commands, peaked at 6,797,160 bytes, and left the database empty. Evidence
is in
[`optimization-functional-20260830.json`](testkit/results/optimization-functional-20260830.json).

The Registration Sentinel matrix passed two promotions, forced acknowledged
write loss and republish, minority/all-Sentinel outages, script flush, and
Go/Rust generation `1 -> 2 -> 3`. The Catalog Sentinel matrix passed two
promotions, reached revision 10, deleted its final value, and left zero keys.
Evidence is in
[`optimization-sentinel-registration-20260830.json`](testkit/results/optimization-sentinel-registration-20260830.json)
and
[`optimization-sentinel-catalog-20260830.json`](testkit/results/optimization-sentinel-catalog-20260830.json).

The exact 70-file Catalog fingerprint
`14b4ca0895ded150e3f04dd82ab607e1e9d937fbffa36cd660064a5b028edac3`
passed a 60-Redis-second AOF-everysec workload: 8,448 accepted mutations at a
target 128/second, zero transient error, stale retry, or unexpected asynchronous
error, 1.245-ms average mutation latency, 7.906-ms maximum, goroutines
`9 -> 10 -> 2`, 28,656-byte stable-window Redis memory growth, no eviction,
rejected connection, or blocked client, and final `DBSIZE=0`. Lua, Rust, and
Go/Rust interoperability post-checks passed. Evidence is in
[`optimization-catalog-60s-20260830.json`](testkit/results/optimization-catalog-60s-20260830.json)
and its adjacent JSONL sample file.

The first tagged workload build found an obsolete test-only shutdown variable
left by stateless Publisher conversion. It was fixed, independently compile-
checked with all soak tags, and the accepted workload was rerun from zero. The
failed attempt is not counted.

Current accepted-scope scores are Lua **9.8/10**, Go **9.6/10**, Rust
**9.5/10**, shared protocol/configuration **9.5/10**, qualification **9.7/10**,
and overall **9.6/10**. The exact rubric, physical source inventory, strengths,
deductions, and recommended follow-up are in
[`optimization-review-20260830.md`](optimization-review-20260830.md). C++ and
Leader were outside this historical score; section 24 records the later C++
implementation. No commit or push was created.

## 24. Transient Catalog synchronization and C++23 implementation

Date: 2026-08-31.

Go and Rust Catalog Subscribers now retain only one persistent Pub/Sub
listener/state-machine task. Full alignment and targeted repair share at most
one temporary synchronization task. Focused tests cover pending scope/Path
coalescing, retirement when the queue becomes empty, reservation of exactly one
replacement worker, initial convergence to one worker, recovery convergence to
one worker, and terminal close. The complete package/crate regressions passed
after this topology change.

The C++23 SDK implements the root Client and strict v1 JSON configuration,
binary Key/Hash operations, Registration, Selector, Catalog Publisher,
Subscriber, stable Entry, bounded MessagePack recovery, and optional SQLite
checkpoint. C++ Selector and Catalog Subscriber use the same one-persistent-
plus-at-most-one-temporary task topology. Publisher owns no task.

Accepted C++ build and offline gates:

- GCC C++23 Debug configuration and build with `-Wall -Wextra -Wpedantic
  -Wconversion -Wsign-conversion -Werror`: passed;
- `verdandi_cpp_tests`: passed;
- clang-format dry-run with `--Werror`: passed;
- project-owned high-signal clang-tidy checks with warnings as errors: passed;
- GCC ASan/UBSan configuration and build: passed;
- ASan/UBSan/leak unit tests with halt-on-error: passed.

Accepted live gates against Redis 8.8.0:

- authenticated Standalone root Key/Hash operations: passed;
- Registration delayed publication, Update, local revision/timestamp, Renew,
  Unregister, and exception translation: passed;
- Selector initial synchronization, live Update, local `power++` prediction,
  callback exception mapping, duplicate-choice rejection, and close: passed;
- Catalog Replace, exact-base Patch, live observation, stable typed Entry,
  SQLite checkpoint restart, Delete, and exact owned-key cleanup: passed;
- the same Standalone integration under ASan/UBSan/leak checks: passed;
- an isolated ACL-protected three-data-node/three-Sentinel C++ smoke covering
  root, Registration, Selector, Catalog, and checkpoint: passed in 3.424
  seconds;
- Sentinel database cleanup returned empty and no container labeled
  `verdandi.test` remained.

The structured Sentinel result is
[`testkit/results/cpp-sentinel-smoke-20260831.json`](testkit/results/cpp-sentinel-smoke-20260831.json).
The final Go `go test ./...`, Rust `cargo test --all-targets`, Rust
all-target/all-feature Clippy with `-D warnings`, and Rust formatting checks
also passed. A current Go race rerun is not claimed: Windows lacked a CGo
toolchain and the available WSL image had no Go installation. Historical WSL
race evidence remains historical only.

This is C++ startup/integration Sentinel evidence, not a full failover
qualification. At this dated checkpoint, two consecutive promotions,
acknowledged-write-loss repair, live TLS, MSVC/Clang/macOS, install/export
packaging, a long soak, fuzzing, and dedicated C++ performance baselines
remained open, and Sentinel+TLS was explicitly rejected until dynamic target
discovery could preserve hostname verification. The current top-level
2026-09-01 section supersedes that historical TLS restriction with the tested
fixed-identity contract.

At this first implementation checkpoint the C++ SDK score was **9.1/10**:
root transport/configuration 8.8,
Registration 9.2, Selector 9.1, Catalog 9.2, strong-type API/schema 9.3, and
tests/release engineering 8.7. The rubric, strengths, deductions, and ordered
release gates are in
[`cpp-review-20260831.md`](cpp-review-20260831.md). No commit or push was
created.

## 25. C++23 source-expansion and Selector hot-path regression

Date: 2026-08-31.

The focused post-implementation review retained one compiled protocol core and
used C++23 expansion only at boundaries where the concrete types are valuable.
Strict JSON configuration dispatch now binds literal member names as non-type
template parameters and uses a variadic short-circuit fold. Per-binding seen
bits replace the former dynamically allocated duplicate-name vector. Schema
member traversal and exception translation use inline higher-order helpers.

Selector projectors are now plain function pointers rather than
`std::function`. Immutable view records retain their typed Attr/Data projection;
copy-constructible candidates detach and begin local mutation from that cache,
while non-copyable values preserve a decode fallback. `any` reuses a
generation-tagged mark vector instead of allocating a new `vector<bool>` for
each transaction. Signed scalar decoding additionally rejects non-canonical
negative zero. The production header/source/template inventory is 32 files and
9,049 lines, 142 lines below the initially accepted C++ implementation; tests
are two translation units and 517 lines.

Accepted short gates after the final source:

- GCC C++23 Debug strict-warning build and offline tests: passed;
- clang-format dry-run and project-owned clang-tidy with warnings as errors:
  passed; clang-tidy identified and removal verified three now-redundant moves
  of the trivially copyable projector pair;
- Redis 8.8 Standalone root, Registration, Selector, Catalog, and SQLite live
  integration: passed;
- GCC ASan/UBSan/leak build, offline tests, and the same live integration with
  halt-on-error: passed;
- uncached Go full-package tests and `go vet`: passed;
- Rust formatting, 72 unit tests plus offline integration/API suites,
  all-target/all-feature Clippy with `-D warnings`, and warning-denied rustdoc:
  passed;
- generated Registration and Catalog Lua identity checks: passed.

No long-duration, new Sentinel, or performance-baseline run was performed in
this focused regression. Earlier Sentinel startup evidence remains valid but
was not relabeled as current. The revised C++ score is **9.2/10**: root
transport/configuration 9.0, Registration 9.2, Selector 9.4, Catalog 9.2,
strong-type API/schema 9.5, and tests/release engineering 8.8. Remaining
deductions are cross-platform packaging, full two-promotion C++ failover, live
TLS, dedicated performance baselines, fuzzing, and a future bounded soak. No
commit or push was created.

## 26. C ABI v1 and lower-standard source-build regression

Date: 2026-08-31.

The C ABI was added to the same C++23 runtime rather than to a second legacy
implementation. It covers strict JSON root construction, binary Key/Hash,
Registration, Selector transactional mutation and One/Any selection, detached
and retained snapshots, Catalog Publisher/Subscriber/stable Entry, bounded
owned diagnostics, and explicit release operations. The new production
boundary contains 2,086 lines; the complete C/C++ production inventory is 43
files and 12,083 lines. Five test translation units contain 898 lines.

The CMake target `verdandi::c` links the native runtime through `LINK_ONLY`.
The core remains C++23, while independent consumer targets were compiled as
C11, C++11, C++14, and C++17. No consumer inherited the C++23 compile feature.
This proves mixed target language modes with a modern source-build toolchain;
it does not claim that an old compiler can compile the C++23 core.

Accepted static and shared gates:

- strict GCC Debug static build: passed;
- seven CTest entries: five passed and both live Redis tests correctly skipped
  with code 77 when no address was supplied;
- C11 ABI test: passed;
- separate C++11, C++14, and C++17 compile/link/runtime tests: passed;
- GCC shared runtime build: passed;
- shared C11 and C++11/14/17 tests: passed;
- the Linux shared runtime exported 88 unmangled `verdandi_*` C symbols;
- the C++11 shared consumer resolved `libverdandi_cpp.so` dynamically;
- clang-format dry-run with C headers, C++ sources, and C tests: passed;
- project-owned clang-tidy target: passed.

The first shared link exposed that the fetched SQLite amalgamation fallback was
static but not position-independent. The private `verdandi_sqlite` target now
enables PIC. Dependency build products are also isolated per build tree so
static/shared and sanitizer configurations cannot rewrite one another. The
shared build was rebuilt with an independent dependency output tree and all
shared consumers then passed; the failed pre-fix link is not counted as
accepted evidence.

Accepted live Redis 8.8 gates against `192.168.0.90:6369`:

- native C++ root, Registration, Selector, Catalog, and checkpoint integration:
  passed;
- shared C ABI root Key/Hash, Registration publish/update/renew/close, Selector
  synchronization/One/local mutation/snapshot, Catalog Replace/Subscriber/Entry,
  and exact owned-key cleanup: passed;
- ASan/UBSan/leak build and all offline tests with halt-on-error: passed;
- native and C ABI live integrations under ASan/UBSan/leak with halt-on-error:
  passed.

No long-duration or new Sentinel run was performed. Existing native Sentinel
startup evidence remains historical and was not relabeled as C ABI failover
qualification. Windows DLL/MSVC, Clang, macOS, CMake install/export packaging,
an automated binary symbol/layout compatibility gate, long soak, fuzzing, and
dedicated performance baselines remain open.

The revised C++ score is **9.3/10**: native root 9.0, Registration 9.2,
Selector 9.4, Catalog 9.2, strong-type API/schema 9.5, C ABI/lower-standard
boundary 9.3, and tests/release engineering 9.0. The boundary is implemented
and source-buildable, but it is still Alpha and no binary compatibility promise
has been published. No commit or push was created.

## 27. C++11 Legacy facade regression

Date: 2026-08-31.

The header-only `verdandi::legacy` facade was added over C ABI v1. It contains
no Redis or protocol state machine. Its six component headers plus umbrella
contain 2,454 lines; the complete C/C++ production inventory is now 50 files
and 14,537 lines. Thirteen test translation units contain 1,325 lines.

Accepted gates:

- C++11, C++14, and C++17 facade consumers compiled with GCC strict warnings
  and warnings as errors: passed;
- all six component headers and the umbrella header compiled independently as
  strict C++11 translation units: passed;
- scalar canonical encoding boundaries, Schema round trips/missing fields,
  duplicate Fields, optional/result copy and move behavior, value-bearing
  Result non-assignment, raw-Fields domain instantiation, non-copyable Selector
  policy forwarding, and invalid-handle API instantiation: passed in all three
  language modes;
- Redis 8.8 C++11 facade integration covering strict JSON Client, PING,
  Key TTL/load/contains/expire, Hash store/load/contains/size/erase,
  Registration publish/renew/update/version/content/diagnostics, Selector
  synchronization/One/Any/local mutation/snapshot/diagnostics, Catalog
  invalid-Kind rejection/replace/exact-base patch/Subscriber/Entry typed
  load/delete/diagnostics, and exact owned-key cleanup: passed;
- static GCC Debug: all nine CTest entries passed;
- shared GCC Debug: all nine CTest entries passed;
- GCC ASan/UBSan/leak with halt-on-error: all nine entries passed;
- clang-format dry-run: passed;
- clang-tidy: passed after the C++23 core and C++11/14/17 consumer units were
  analyzed with their own correct language-feature environments.

One expanded offline assertion initially inverted the expected truth value
after assigning a successful `result<void>`; it failed all three lower-standard
tests, was corrected, and the complete accepted matrix was rerun. No production
defect was involved and the failed pre-fix run is not counted as evidence.

The facade score is **9.4/10**. Its principal strengths are one runtime,
allocator-safe ABI forwarding, RAII ancestor lifetimes, raw and typed Fields,
explicit callback borrowing, and verified static/shared/sanitizer behavior.
The deductions are source-build dependence on C++23, C-boundary copies and
repeat typed Selector decoding, less precise pre-C++20 schema diagnostics, a
deliberately small expected/optional surface, and the unchanged absence of
Windows DLL/MSVC, Clang/macOS, install/export, and automated ABI gates. The
overall C++ SDK remains **9.3/10** because release qualification did not change.
No long, new Sentinel, or performance-baseline run was performed. No commit or
push was created.

## 28. Cross-language configuration normalization regression

Date: 2026-09-01.

The external v1 JSON contract was normalized across Go, Rust, C++23, C ABI and
C#. Redis physical reconnect now has one fixed `delay_ms`; Selector and Catalog
retain separate business-recovery backoff. Endpoint syntax, Unicode boundaries,
strict object shape, missing required fields, null, duplicate/unknown fields,
numeric lexical rules, TLS safety, bounded file reads, path checks and stable
error fields now share one 47-case conformance corpus.

Go retains idiomatic native topology and TLS types behind a thin go-redis
driver. Rust retains Duration/PathBuf and isolates Fred URL adaptation. C++23
uses compile-time JSON member bindings and an iterative null walk. A new C ABI
offline validator lets C++11/14/17 and C# reuse the same parser; C# performs
strict UTF-16 and 1-MiB preflight without creating a duplicate configuration
DTO.

Accepted short gates:

- Go all-package test/vet and WSL/Linux race: passed;
- Rust format, 72 library tests plus offline suites, strict all-target/all-feature
  Clippy and Rust 1.85 minimum-toolchain tests: passed; 12 external Redis/load
  cases remained explicitly ignored;
- C++ Debug static, Release shared and ASan/UBSan: each reported six offline
  passes and three expected Redis skips out of nine CTest entries;
- C++ clang-format and project-owned clang-tidy: passed;
- C# Release net8.0/net10.0: zero warnings/errors; both Linux self-contained
  offline runners passed against the latest shared native library;
- shared configuration corpus: Go/Rust/C++ passed 41 semantic plus six raw
  cases; C# passed all 41 semantic and five string-representable raw cases,
  while native tests own the remaining invalid-UTF-8 byte case;
- shared native library: 90 exported unmangled `verdandi_*` symbols, including
  the string capability query.

The initial focused configuration run did not use an endpoint. The subsequent
fixed-identity Sentinel TLS qualification is recorded in the current top-level
2026-09-01 section and its three machine results; no long-duration TLS run was
performed. The configuration-layer score is **9.7/10**. The
detailed language scores, strengths, deductions and remaining release work are
in [`configuration-review-20260901.md`](configuration-review-20260901.md); the
machine-readable result is
[`testkit/results/configuration-normalization-20260901.json`](testkit/results/configuration-normalization-20260901.json).
No commit or push was created.
