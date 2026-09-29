# Visual Studio Projects and Windows Server Port

Feature ID: `windows`.

Status: implementation authorized by the user's request to proceed with solution generation and cross-platform adaptation; delivery is staged. build/Astra.sln and five browse projects exist, as do native time/random/stop/metrics adapters and independent verification targets. Ordinary regression, Windows Comet/native checks, and solution compilation have run; Section 7/[validation](../validation.md) identify source/scope. Three server semantic blockers remain. Windows production services are not supported yet.

Paths are relative to Astra root. [Development](../development.md), [coding](../coding.md), and [AGENTS.md](../../AGENTS.md) govern process/expression/permissions. [Pulsar](../../pulsar/README.md), [storage](../../common/README.md), and actual interfaces retain business time, durability, and shutdown contracts.

## 1. Goals and existing boundaries

- Browse Comet, Star, Pulsar, Common, and Planet together. Comet has real Windows builds; servers default to browsing, with capability checks only for explicit experimental builds.
- Prepare platform adaptations/build branches/cases before MSVC supports required language/library features. Probe actual reflection, annotations, and contracts, not predicted dates/version numbers.
- Reuse CMake/Python/PowerShell, without parallel handwritten production .vcxproj source manifests, a new build framework, or downloads.
- Target x64 desktop/console processes, minimum APIs Windows 10/Server 2016. Exclude service installation, host-time management, revived Planet business, ARM64, and protocol changes.
- Reject Windows probe/sanitizer profiles explicitly until adapted/verified; empty implementations cannot claim equivalence.
- Linux/GCC 16.2 remains the registered server toolchain. Preserve existing source/command/evidence identity; Windows preparation does not expand passes.

Verified dependencies include [GCC build flags](../../CMakeLists.txt), [BOOTTIME/timerfd](../../common/src/clock.cpp), [adjtimex](../../pulsar/src/physical_clock.cpp), [POSIX signals/descriptors](../../common/src/process.cpp), [socket/eventfd metrics](../../star/src/metrics.cpp), [getrandom identities](../../star/src/ephemeris.cpp), and [file locks/identity/sync](../../pulsar/src/ledger.cpp). Removing a CMake platform check is not a port.

## 2. Projects, directories, and commands

| Output | Responsibility |
| --- | --- |
| build/Astra.sln | Overall solution referencing projects; ignored by Git |
| build/Comet/ | Project, objects, PDBs, libraries, SDK build data |
| build/Star/, build/Pulsar/ | Server browse/explicit experimental outputs |
| build/Common/, build/Planet/ | Shared/deferred-component browse/build outputs |

Separate architectures, toolchains, CRTs, and configurations within each directory; incompatible Debug/Release/standalone SDK intermediates cannot mix. Top-level coordination is build/Astra/, not a fictional CMakeCache per target. Preserve the existing root Ninja build/CMakeCache.txt and old caches/results/dependencies. Verify owned cache source/generator/architecture and reject mismatches instead of overwriting.

The existing build.ps1 → tools/build.py → tools/solution.py chain now provides:

- solution: generate browse projects/Comet entry without compiling, testing, or opening IDEs.
- solution --runtime: explicitly probe Windows server C++26; fail on capabilities/known semantic gaps without browsing fallback or implying full compilation.
- Separate build/test commands. Experimental records identify compiler, SDK, CRT, dependencies, and probe results; /std:c++latest is not proof.

Ordinary generation uses LANGUAGES NONE, without compiler identification/try_compile. SDK, --runtime, and native verification do compile and need corresponding authorization. Publish the requested traditional .sln using actual CMake project identities regardless of its default solution format.

IDE input lives in cmake/ide/, orchestration in tools/. Source-associated custom targets/source_group provide browsing, excluded from real default builds. Reuse component build definitions rather than force Linux root configuration into browse mode. Maintain one source inventory; select platform files conditionally and regenerate to include new files correctly.

Missing Comet dependencies permit explicitly selected, clearly nonbuildable browse-only mode; normal SDK builds still fail. IntelliSense diagnostics on GCC syntax are not Linux compilation results.

## 3. Standard library and time adaptation

Under [coding Section 3.2](../coding.md#32-abstraction-and-efficiency), prefer cross-platform standard facilities when semantics, correctness, resources, and performance all meet requirements. Use duration/time_point/conversions, locks, threads, cancellation, and condition variables where appropriate; portability cannot weaken contracts. Retain one Clock/Filter implementation for continuous correction, error aging, four-timestamp filtering, TTL, and explicit-time tests.

steady_clock promises monotonicity, not a universal suspend-accounting policy. system_clock provides wall time, not synchronization state, error bounds, or trusted provenance. Nanosecond representation is not accuracy; standard aliases cannot replace platform contracts. See draft [steady_clock](https://eel.is/c++draft/time.clock.steady), [system_clock](https://eel.is/c++draft/time.clock.system), and [Clock requirements](https://eel.is/c++draft/time.clock.req).

| Boundary | Linux | Windows design |
| --- | --- | --- |
| Suspend-inclusive elapsed time | CLOCK_BOOTTIME | QueryInterruptTimePrecise; bound 100 ns-to-ns conversion; not unbiased sleep-excluding clocks |
| Local precision calibration | Existing edge search/timerfd budget | Shared search, independently cancellable system budget; stopped tested clocks/fixed iteration counts cannot impersonate deadlines |
| Absolute reference | CLOCK_REALTIME | GetSystemTimePreciseAsFileTime with epoch/unit/overflow checks |
| Host reference quality | Readonly adjtimex | Investigate readonly local W32Time status; readable wall time/running service do not imply quality |

See [Interrupt Time](https://learn.microsoft.com/en-us/windows/win32/sysinfo/interrupt-time) and [QueryInterruptTimePrecise](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryinterrupttimeprecise). Recheck sample age/error after resume without renewing existing TTLs. Preserve 500 ms quality bound, 5 s freshness, and initial-readiness/outage-continuity distinction.

Candidate Windows quality input is public [MS-W32T W32TimeQueryStatus](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-w32t/7e80a465-f5f4-4c3c-87ef-12f76e45f8d1), with [W32TIME_STATUS_INFO](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-w32t/f60ebce0-df96-4c96-b40b-fdbd34a2c936). Define units, synchronization result/source, local permissions, query timeout, and error growth. Root delay/dispersion fields alone do not prove the bound. No private DLL ABI, localized w32tm-text parsing as stable API, or system-time mutation.

Inspected SDK headers lack ready RPC declarations. Verify whether existing MIDL/RPC tools can generate clients and whether external protocol material is required before implementation. New packages/tool downloads need separate approval.

**Quality blocker:** Until error/freshness derivation and bounded queries are verifiable, Windows Pulsar must not announce synchronized reference. This blocks cold Star anchoring; always-invalid samples do not constitute a complete port. Solve through a separately authorized prototype.

## 4. Platform facilities and failure boundaries

Keep adapters with actual owners, such as common/src/windows/ and linux/, with Pulsar-specific storage/reference adapters under pulsar/src/. Use narrow declarations, no platform-selection-only hot-path virtual dispatch. RAII owns private handles without public Comet leakage; do not build a universal syscall framework.

| Facility | Windows plan/required guarantees |
| --- | --- |
| Compiler flags | Split compiler/platform options; no -freflection/-fcontracts/ELF RPATH/-lm passed to MSVC; preserve warnings, standards, assertions |
| Language capability | Compile real minimal reflection/annotation/expansion/contract syntax; separately execute contract-mode validation. Block unsupported runtimes, never remove contracts/duplicate handwritten option parsers |
| Stop | Console callbacks request stop only; original control flow stops admission/drains RPCs/threads/releases dependencies. Match handler/state lifetime; TerminateProcess is not graceful |
| Logs | Bounded diagnostics and no business-thread wait for slow consumers. Blocking WriteFile is not nonblocking-pipe equivalence; define supported handle types/cancellation/drain |
| Metrics | SOCKET handles, explicit Winsock ownership, nonblocking I/O/independent wakeup, readonly responses/limits/connections/timeouts |
| Random identity | System cryptographic source; fail before commit, never time/rand()/unqualified random_device |
| Database locks/identity | Exclusive Win32 locks, noninherited handles, reject untrusted reparse points, verify opened identity; preserve 64-bit handles/Unicode paths |
| SQLite | Existing pinned version/hash/transactions/PRAGMAs, correct system libraries; no source patches/new format |
| Test processes | Existing Windows Job ownership, handle/creation identity, verified child/thread exit without affecting deployments/other work |

**Logging blocker:** Inherited stdout does not guarantee bounded async writes. Determine supported sinks/cancellation/draining before implementation. No unbounded workers/queues, leaked writes, or silently disabled logging. New sink configuration or changed stdout behavior requires user confirmation.

**Storage blocker:** Pulsar initialization syncs the new database and parent directory. Ordinary-file [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers) is not proof of Linux directory-fsync equivalence. Independently reason/verify directory-entry durability, SQLite VFS commits, and local filesystem boundaries. Do not downgrade to clean-exit reopen or require admin whole-volume flush. Unproven initialization remains unaccepted and blocks relevant release claims.

## 5. Contracts and verification obligations

| Contract | Oracle/counterexamples |
| --- | --- |
| WIN-LAYOUT | Root solution, per-project files/intermediates; spaces/non-ASCII, old Ninja cache, CRT/configuration differences, regeneration, missing dependencies |
| WIN-BROWSE | No server compilation/install/tests; grouped sources; empty-target success is not service compilation |
| WIN-COMPILER | Actual identified failure on missing features; no conditional removal of contracts; Comet C++23/server actual requirements |
| WIN-TIME | Conversion bounds, reversal/freeze/cancel/suspend-resume/wall jumps/staleness/initial readiness/outage continuity; shared calculation cases |
| WIN-REFERENCE | Independent quality vectors; reject unsynchronized/stale/failed/overflow/timeout, not self-generated expected bounds |
| WIN-LIFETIME | Partial startup, concurrent stops, in-flight RPC, broken pipes/backpressure; drain workers/handles/tasks without late use-after-free |
| WIN-STORE | Two-instance exclusion, Unicode, reparse/replacement races, empty DB/sidecars/rollback/reopen/initial durability; preserve failure scene |
| WIN-RANDOM | API failure never commits; correct fixed UUID bits; fake sources stay out of production |
| WIN-CLUSTER | Three Stars each writing, cross-node reads/two-way replication; mixed Windows/Linux recovery, atomic snapshots, TTL |

Tests follow actual component ownership. Nonreflection adapters use independent supported-toolchain targets, not claims of full service compilation. Native time/storage/cancellation require real Windows checks, not mocks only; shared changes require Linux regression. Release still requires applicable [intensive obligations](../development.md).

Record four stages separately: static preparation, platform verification, complete-service build/regression, mixed-cluster acceptance, each with source/tool identity/scope/gaps. Subsequent user authorization covered ordinary regression excluding Sentinel, not sanitizers, benchmarks, soak, downloads, or system changes. [validation.md](../validation.md) remains the sole real-results record.

## 6. Configuration, admission, and implementation order

Register features/contracts in [maintenance](../project.md). Current development.json/schema cover the Polaris storage pilot only; design is a single path and the adapter is not a generic C++ runner. Do not insert unknown fields, replace the pilot design, or fabricate checks to claim Windows gate adoption. Extend parsing/collection compatibly with negative cases for formal Windows configuration; until delivered, report this gap.

1. Implementation/paths confirmed; freeze design/input digests before verification without automatic commits.
2. Deliver IDE generation/browse/standalone Comet wiring, rejecting cache conflicts independently of server C++26.
3. Extract owner-local platform boundaries, preserving Linux algorithms/protocol/contracts/public ABI; add Windows implementations/counterexamples.
4. Produce executable prototypes/evidence for quality, logging, initialization durability. Before execution permission, deliver source/pending conditions. Contract changes require separate design confirmation; port permission does not allow degradation.
5. Integrate MSVC capability checks/experimental builds. Preserve browsing/precise gaps when unsupported, without lowering standards/downloading compilers.
6. Run authorized platform/Linux checks, then mixed three-node acceptance; update support only with evidence.

Decisions:

- Confirmed combined solution/Windows-port work.
- Confirmed build/Astra.sln and build/<Project>/ project/intermediate locations.
- Shared standard computation/lifecycle primitives plus thin platform adapters; initial x64 console, no system services or unadapted probes/sanitizers.
- Implementation authorized, no semantic downgrades. Servers remain unready until blockers resolve.
- Port authorization does not replace execution/install/release permission; no fictitious approvals, exit codes, or passes.

## 7. Delivered implementation and remaining work

| Boundary | Delivered | Remaining |
| --- | --- | --- |
| IDE | cmake/ide, tools/solution.py, root .sln/per-component .vcxproj; actual MSBuild/Python negative cases executed | IDE UI acceptance; servers browse only |
| Time | common/src/{linux,windows}/clock.cpp; shared Clock/Filter; suspend-inclusive timing, checked 100 ns conversion, independent 200 ms calibration budget; shared models/bounds/precancel executed | Real suspend/resume, full calibration faults, W32Time quality source |
| Random | getrandom/BCryptGenRandom without fallback; fixed UUID bits | Native API fault injection/full identity regression |
| Stop | Platform Signals; Ctrl+C/Break/self CRT SIGTERM request stop only | Console-event/draining acceptance; no graceful promise for window close/logoff/force-kill |
| Metrics | Shared HTTP state machine; Linux poll/eventfd; Winsock/WSAPoll/bounded UDP wakeup/exclusive listen; Windows HTTP cases run | Complete handle-reclamation/concurrency-stress evidence; platform results in validation |
| Platform checks | cmake/platform, Common/Star output directories; three native Windows tests configured/built/run | Only implemented adapters/models, not full services/mixed cluster |
| Server gate | Actual C++26 probes in cmake/compiler.cmake; verified current MSVC rejection for missing reflection/contracts without weakening | Contract execution/STL hardening and three semantic blockers, beyond waiting for MSVC |

Quality, log backpressure, and Ledger locks/file identity/directory durability still need implementation/verification. Existing Linux Source/stdout Logger/Ledger remain intact, without always-success/always-invalid Windows substitutes. These and formal Windows gate gaps block complete-service/release claims. Native passes still require Linux regression and mixed three-Star acceptance.
