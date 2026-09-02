# C# Managed Binding Review

Date: 2026-08-31

## Scope

This review covers the unpublished `sdk/csharp` managed facade, not the native
C++23 implementation behind C ABI v1. The score is an Alpha engineering score,
not a production-readiness claim.

## Score

**9.3/10 for the implemented managed-binding scope.**

The score reflects a complete managed mapping of the currently exported root,
Registration, Selector, and Catalog behavior, clean language-native ownership,
strict builds, and independent full-domain Standalone and two-promotion
Sentinel regressions. It does not award release evidence that has not been run.

## Strengths

- Public callers see only C# types, `IDisposable`, generics, immutable Fields,
  synchronous delegates, and explicit Results. No C structure, pointer,
  allocator, callback context, or release function escapes.
- Source-generated `LibraryImport` avoids reflection-based invocation and keeps
  every signature statically checked. The live test confirms the ABI layout and
  calling convention on Linux x64.
- Dedicated SafeHandle types own every native allocation. Internal parent
  leases use SafeHandle reference counting, including a finalizer fallback, to
  preserve child-before-parent release order even after early public Dispose.
- Application records implement one static generic `IFieldValue<TSelf>`
  contract. Raw Fields use the same API, and no runtime reflection, hidden JSON
  record serialization, code generator, or per-object codec service is added.
- Fields copy caller input into one immutable continuous payload. Native writes
  pin it once and build only a temporary view array; callback output is copied
  before returning to managed code.
- Borrowed Selector Candidates are a `ref struct`; opaque Choice values are
  fenced by one process-wide transaction ID. The live suite proves stale and
  duplicate selection rejection, callback exception translation, mutation
  commit, and detached output.
- The loader supports explicit deployment control and normal app/RID layouts,
  validates ABI v1 before Redis work, and returns stable load errors.
- Both target frameworks compile with nullable analysis and warnings as errors.
  The test runner has no external managed test dependency.
- The independent ACL Standalone matrix covers exact field/record limits,
  malformed codecs, stale Catalog writes, concurrent Registration/Selector
  calls, concurrent root disposal, forced finalizer cleanup, both native loader
  paths, and terminal handle behavior in .NET 8 and .NET 10 self-contained
  Linux x64 applications.
- The independent Sentinel matrix keeps both managed targets alive across
  acknowledged write loss, two promotions, script-cache loss, total Sentinel
  loss, unavailable views, desired-state repair, and Selector generation
  `1 -> 2 -> 3` recovery.

## Remaining deductions

- A Windows x64 MSVC DLL now passes direct .NET 8/10 offline loading and the
  complete server-authenticated Sentinel TLS two-promotion matrix. Automated
  RID packaging remains unqualified; macOS is intentionally unsupported.
- The current C ABI is synchronous. The binding correctly avoids fake
  `Task.Run` APIs, but high-concurrency ASP.NET callers do not yet receive true
  cancellation-aware async I/O.
- A formal NuGet package still needs reproducible per-RID native builds,
  symbols, package layout, license notices, and automated load tests. The source
  project intentionally does not package a local Debug `.so`.
- NativeAOT, trimming, single-file extraction, and ARM64 have not been
  qualified from a C# consumer. Server-authenticated Sentinel TLS is qualified;
  live mutual TLS remains separate and unqualified.
- Selector candidate decoding necessarily crosses the C ABI and builds managed
  Attr/Data on first access in each policy callback. It is cached within the
  callback, but no allocation/latency benchmark yet compares it with the native
  C++ typed path.
- The managed suite is bounded rather than an endurance campaign. Parent-first
  and concurrent Dispose plus forced GC/finalizer pressure are covered, but
  cancellation storms and soak remain open.
- No C# peer has yet been paired with Go or Rust, so wire compatibility is
  inherited through the qualified C ABI/C++ core rather than proven by a
  direct managed cross-language run.

## Recommended next gates

1. Preserve the completed `win-x64` and `linux-x64` Release-runtime Sentinel
   TLS matrix as a regression gate.
2. Add cancellation-storm and bounded endurance ownership gates before package
   publication.
3. Measure One/Any candidate decode allocations and Registration call overhead;
   optimize only from those results.
4. Add live mutual TLS and one direct Go/Rust-to-C# compatibility case by
   reusing the existing isolated protocol fixtures.
5. Build a private NuGet with RID assets and verify clean-project restore,
   application-directory/RID resolution, trimming, and NativeAOT where claimed.

## 2026-09-01 Sentinel TLS and capability addendum

The managed-scope score is now **9.6/10**. Net8.0 and net10.0 self-contained
Linux x64 and Windows x64 peers passed the complete two-promotion matrix over
private-CA Sentinel/data/replication TLS through their platform-native C++ core,
with every Selector generation advancing `1 -> 2 -> 3` and final `DBSIZE=0`.
`Runtime.Supports` also exposes the loaded native runtime's stable string
capabilities and reports a missing query symbol as incompatible.

The earlier generic and Windows TLS deductions are superseded by this
server-authenticated Sentinel TLS evidence. Remaining deductions are live
mutual TLS, NuGet/RID automation,
NativeAOT/trimming/single-file/ARM64, direct managed cross-language peers,
allocation benchmarks, cancellation storms, and endurance. Automated
packaging remains deliberately deferred; macOS is unsupported.

## 2026-09-01 allocation and hermetic-regression addendum

The managed-scope score remains **9.6/10**. `Fields.Create` now pre-sizes when
possible, retains each input only for the synchronous call, sorts value-type
descriptors, detects adjacent duplicate names, and writes names and values once
into final continuous storage. It no longer allocates a PendingField object,
UTF-8 name array, copied value array, and HashSet entry for every field. Raw Key
store likewise pins the borrowed Span for the synchronous C ABI call instead of
copying the complete value first; a live empty-value case covers the null/zero
native view.

The independent Standalone harness now supplies an explicit matching host
runtime for offline tests and records a skip when none exists, while both Linux
self-contained integrations remain mandatory. Windows runtime dependencies are
copied beside the shared C++ target. The final net8/net10 Standalone and
Windows/Linux TLS two-promotion matrices pass. The score does not increase
because native package automation, true asynchronous I/O, NativeAOT/trimming,
allocation benchmarks, direct managed cross-language peers, mTLS and current-
source endurance remain open.
