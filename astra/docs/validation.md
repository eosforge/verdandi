# Latest validation

## Visual Studio and platform split: ordinary regression passed, complete Windows services not ready

2026-09-29 (Asia/Shanghai), branch `sdk`, HEAD `9dd362814921017cc8a676d9f1ab9f0233951a16` plus frozen uncommitted inputs. This run authorized ordinary regression, explicitly excluding Sentinel. It used existing Python, CMake, MSVC 19.51, GCC 16.2, Go 1.27.1, and prepared dependencies, without downloads, commits, pushes, or deployment changes.

| Scope | Actual result |
| --- | --- |
| Linux Release | `tools/build.py regression --profile release --jobs 6 --test-jobs 4` succeeded; 62/62 CTest, no failures/skips |
| Go | `go test -mod=readonly -count=1 -p 4 -parallel 1 ./...` succeeded; eight packages with tests passed |
| Protocol generation | C++/Go generated-file comparisons passed, without tracked-output changes |
| Windows native adapters | MSVC Release built; native_clock, native_platform, native_metrics 3/3 passed |
| Windows Comet | Standalone Release SDK built; 6/6 CTest passed |
| IDE generation/build | Six Python generator cases passed; actual MSBuild of build/Astra.sln succeeded, building Comet only with the other four projects remaining browse targets |
| Windows service capability rejection | Actual /std:c++latest probes confirmed missing reflection/contracts; configuration correctly returned nonzero. Negative checks passed, not service compilation |

Linux includes independent SDK package consumption, Pulsar processes, and real three-Star cases; the latter two took 11.91 s/56.69 s. CTest took 164.88 s; build-through-generation regression took 573.78 s. Bounded multi-node cases are not soak or mixed Windows/Linux acceptance. Linux used a separate source directory without replacing the remote workspace. About 7.0 GB memory was available beforehand; build/test concurrency was 6/4. Windows mostly used two build jobs; IDE execution reused completed SDK artifacts.

Three build issues were fixed without weakening assertions:

- Windows Metrics helper `socket` conflicted through Winsock argument-dependent lookup; renamed `create`.
- New test names clock/random conflicted with standard functions; renamed timing/entropy. Initial Windows/Linux failure logs remain.
- CMake's CXX26 flag mapping prevented actual MSVC syntax probes. Only probe-function scope now uses latest mode, preserving production C++26. Both real probes then ran and correctly rejected missing capabilities.

Raw `build/results/windows-port/run-1/` contains stage commands, per-file source hashes, exits, monotonic durations, and raw-log hashes. Final Linux input has 608 entries, manifest SHA-256 `eb784dedec0c12663f3629a896c955eee9b7e4765529b05ef7236d3d90900fa8`, also used by passing Windows native/SDK rounds. Later Windows-only capability-probe input is `621f2039361b93da5d77c067ff1cb526e4153139a50f91c34100c53f5365f016`, in runtime-rejection-2.json. Linux input was unchanged while running; the later cmake/compiler.cmake change is not loaded on Linux. Documentation followed execution; not every final file ran under one identity.

Linux evidence was retrieved to run-1/linux; archive SHA-256 `1d3b0e195068f76ca9b026bcc5867dd2210848c1b8c7f70fe50589285abf076b` was verified. Frozen sources were rechecked unchanged, with 63 Linux/13 Windows artifact hashes. Linux controller/runner exited and owned groups had no live processes. Failed/fixed stages remain separate. MSBuild exited 0; subsequent console summary printing hit GBK encoding after logs/exits were saved. Only presentation encoding was corrected; that wrapper exception was not relabeled as zero exit.

Existing Windows SDK test compilation still reports encoding/upstream gRPC deprecation warnings; Linux SQLite emits string diagnostics. No dependency edits/new suppressions hide them. **Full Windows services remain unready:** W32Time quality, logging backpressure, native Ledger/directory durability, contract execution, and formal Windows gate configuration have gaps. This run excludes Sentinel, Sanitizers, benchmarks, soak, Admin browser acceptance, real suspend/resume, and mixed clusters; it is not unconditional release qualification.

## Development gate and Polaris storage pilot: execution pending

2026-09-29, branch sdk, HEAD `61694d8a3ffd146f71fdf258461da05a1acece4d` plus then-uncommitted workspace. Implementation of schemas/gate/pilot integration was authorized; behavior tests/prerequisite builds still awaited scoped authorization. No dependencies were installed, Go tests/gate self-tests run, commits created, or pushes made in that implementation task.

Delivered source includes four Draft 2020-12 schemas (configuration/evidence/approval/rules), dependency registry, validate/collect/gate entries, 28 gate test methods with negative subcases, and four storage-contract observation cases. Existing Black/gofmt, two Python static parses, six JSON parses, local documentation links, and diff formatting were checked. These are not runner/schema-behavior/component-test passes.

Read-only inspection found Ubuntu Python 3.14.4, jsonschema 4.19.2, project Go 1.27.1/GCC 16.2 directories, and Go module cache; no installation is needed there. Windows project Python lacks jsonschema. Collector drain/timeouts/log bounds/probe association and formal schema rejection still need execution.

Pilot qualification is **undetermined**, not an emitted eligible/blocked report. Configuration retains 29 complete rule obligations and six L3 gaps; four samples do not qualify L3. CI isolation, coverage/model/mutation, measured baseline, and convergence deadline remain incomplete. After execution, update this section/native build/results/verification evidence while preserving failed directories. Other results below retain their own source identities; ordinary Go package regression does not complete this formal pilot.

## Complete Comet C++ interfaces and ordinary regression

2026-09-28, sdk HEAD `08554baf79617b69c210d04e595a8e7ebd7e0494` plus layout migration/uncommitted inputs. Cleanup, added cases, and ordinary tests were authorized. Existing tools/dependencies were used; no download, commit, push, deployment change, or restart of the stopped indefinite soak.

build/results/comet-review/inputs.json freezes 542 non-Markdown files, SHA-256 `b66415f4f469a2ab6d886ae3ca04c1c0519ed2e125730f086e079be30ceecb50`, covering source/tests/build entries/generated protocols, not HEAD alone. Windows windows-5 used earlier `c449a255b79227de997ad89f7c03cc4101c0df24fb6e2f7913c4d51678636f90`; afterward only editor LF policy and line endings in two protos/generated orbit.grpc.pb.h changed. SDK implementation/headers/tests/build entries were unchanged. Docs are maintained separately.

| Scope | Actual result |
| --- | --- |
| Windows MSVC Release SDK | 6/6 CTest, source/package consumers, export audit passed; windows-5 |
| Windows Python fixtures | 24 cases: 22 passed, two Linux-only skips |
| Linux ordinary Release | linux-3: 60/60 CTest, no failures/skips, including real RPC/process/package cases |
| Go/generated protocol | Eight tested packages passed; C++/Go generated bytes matched |
| Linux Python fixtures | 24/24 passed |
| Bounded three-Star recovery | Kill/recover A/B/C once each, then 60-second resident phase passed; total 146.537 s, normal cleanup |
| Static checks | Ten handwritten C++ files formatted, 23 Python parses, 170 local links in eight docs, git diff --check passed |

Added cases cover initial registration/update deadlines/direct cancellation, capture destruction after notification/sampling self-unregistration, sampling exceptions, selector reentrancy, same-byte new authority invalidating old estimates, and old-view/accounting preservation on capacity rejection. Independent cpp_comet_sampling is registered in SDK/root builds and checks two real workers, 64 candidate slots, capacity/close rejection, drain, and repeated wait. Existing cases retain lost-receipt no-replay, keepalive, late samples, slow notifications, atomic Maps, Almanac floors, capability/generation checks, and same-ID multisource recovery. Case counts are not exhaustive coverage percentages.

Observed fixes:

- Watching/Beacon self-unregistration could destroy the final business capture after reacquiring the object lock. Release now occurs outside it while preserving callback context, permitting destructor state reentry and rejecting self-wait. tick success/exception paths release captures before restoring markers.
- Observer estimates introduced an extra dedup node for single-page/single-item updates, failing allocation assertions on both platforms. Direct UUID-based estimate eviction and avoidance of a conditional-expression Table-root copy restored the path without weakening assertions.
- Generation comparison exposed CRLF inconsistencies between two protos and existing generated header comments. Existing generators regenerated LF-only changes; .editorconfig declares LF. Byte comparison stayed strict.
- Initial MSVC compile found sampling-parameter shadowing of an internal callback marker and a missing chrono include. Names/includes were corrected without relaxing warnings-as-errors.

Separate rounds preserve failures: windows-1/2 compilation, windows-3 allocation assertion, windows-4/5 passes; linux-1 was 58/59 on pre-fix frozen input with the same allocation failure. linux-2 retains generation-comparison failure alongside passed runtime cases. Process exit/owned cleanup was verified before each next round, without overwriting evidence or another simultaneous cluster.

All three Stars accepted their own writes: 16 Publishers/16 Beacons each, 48 records/domain, 2 KiB bodies. Fault rounds checked stable UUIDs, cross-node propagation, and complete recovered content. The resident phase executed 396 actual business rounds, not liveness-only checks. Twenty-four resource samples measured Star peak RSS about 74.1 MiB and SDK probe 32.8 MiB, FD peaks 19/14 and threads 21/62. These are sampled peaks; history growth increased RSS and the short window proves no long-term leak bound.

Linux had 16 vCPU/about 6.5 GiB available; main build/CTest 4/4. Independent package consumption ran serially, adapting compilation to then-available memory. Tools were GCC 16.2, Go 1.27.1, protoc 36.1/gRPC 1.84 and existing dependencies. Windows used VS18/MSVC 19.51/current Comet prefix. All nodes shared one VM, not separate physical fault domains.

Evidence: build/results/comet-review/windows-5 and remote/linux-3, with old failures intact. Retrieved linux-evidence.zip SHA-256 `f9c6399be996e39703bb6dc8b13c5b3f5c497bfbf3a0ed0bcf9e2a98f837fbc4` verified. remote/linux-3/verified.json checks all 542 sources/81 Linux artifacts; windows-5/binaries.json records Windows artifacts. Verified controller/runner exited, no owned executables remained, temp-data directories were empty. Three empty owned lock files were removed only after no owners remained and exclusive locks were obtained. Failure evidence was not deleted.

Excluded: Sanitizers, benchmarks, multi-hour soak, physical-machine partitions, Admin browser acceptance, Go SDK implementation. SDK is C++23, Star C++26; new protocol requires matching Star/SDK. Core-only native state still needs matching Crypto from the existing gRPC prefix. Bounded recovery is no unconditional long-term/release guarantee.

## Repository reorganization boundaries

On 2026-09-28, the uncommitted layout was prepared for the intended github.com/eosforge/astra repository. Protocol, Admin, tools, and cross-component tests moved under Astra; Go module/import/go_package names changed and existing generators regenerated Go/C++ files. Owned code is MIT; third-party notices are unchanged. This records layout preparation, not creation/publication of a new remote.

The migration itself ran no configure/build/tests/Sanitizers/performance/soak. Later Comet regression above covers its frozen input; older matrices below remain pre-migration identities. Static path/syntax/inventory checks do not prove linking/runtime behavior.

Migration checks passed for 50 Python parses, PowerShell/Bash entry syntax, document links, relative C++ includes/statically resolvable CMake references, and diff whitespace. All 1,382 pre-move files had destinations; 55 public identity files/original third-party notices were retained. The 540 non-Markdown input manifest SHA-256 was `ab3c732807032fa516f4d39546813406d5c67cfd31540860ea28b7a878343c04`, including generated protocol but excluding build/cache/docs.

Subsequent consolidation removed redundant progress/review text and invalid legacy-sdk.md, placing still-valid old Redis SDK methods in its testkit/README.md. Paths/working directories/implementation-vs-target statements were normalized. Static links/anchors in the then-remaining 59 Markdown files and 50 Python parses passed. Astra's 540 non-Markdown inputs remained byte-identical. The old SDK remote-root JSON example was corrected and statically checked, without running its runner. Documentation checks provide no new behavior/release qualification.

New raw evidence belongs in build/results, with current human conclusions here. Older build/... paths below belong to the original Verdandi workspace and historical manifests retain them. Stopped/failed soak directories remain, without rerunning/relabeling after migration. Ordinary Linux/Windows/Go follow-up is recorded above; Admin and configuration-specific gaps must still use their own evidence. The migration task downloaded nothing, committed/pushed nothing, and created no remote.

## Full SDK regression results

2026-09-28 (Asia/Shanghai), sdk HEAD `08554baf79617b69c210d04e595a8e7ebd7e0494` plus frozen uncommitted input. Full tests/Sanitizers were explicitly authorized. Existing tools/dependencies only; no download/commit/push.

**The complete matrix passed for this frozen snapshot, not the subsequently changing workspace.** build/sdk-full-validation/inputs.json contains 934 files, SHA-256 `4a7c674b0b590571573b2283866fcc54711fe1efc948035177efedf8a79b7fc0`. SDK/Admin inputs were unchanged at final audit; later documentation updates are not reruns.

| Scope | Actual result |
| --- | --- |
| Linux Debug | 59/59 CTest, no failures/skips |
| Linux Release | 59/59 CTest, no failures/skips |
| Linux Release probes | 59/59 CTest, no failures/skips |
| Linux ASan + UBSan, leak checks included | 59/59 CTest, no failures/skips/diagnostics |
| Linux TSan | 59/59 CTest, no failures/skips/diagnostics |
| Go/protocol | Eight packages and generated C++/Go comparisons per configuration; additional race run passed all eight |
| Windows MSVC SDK | Release build, 5/5 CTest, source/package consumption, export audit passed |
| Python build entry | 8/8 passed |
| Shared fixtures | Linux 39: 38 passed, one Windows-only skip; Windows 34/34 including that platform case |
| Admin | Format, 217 import boundaries, 102/102 Node cases, type checks, production build passed |
| Benchmark entry | Built with existing dependencies; cpp_baseline_workload 1/1 passed, no performance samples |
| Bounded three-Star smoke | 16 Publishers/16 Beacons per Star, 2 KiB bodies, A/B/C fault recovery once each, then 60 seconds resident; total 156.698 s, normal cleanup |

All five Linux configurations include independent SDK source/package consumers and real three-Star processes. Each Star writes locally, with fixed observers checking cross-node delivery. Smoke parameters were `--fault-seconds 0 --steady-seconds 60 --interval 0 --records 16`, not multi-hour stability. The stopped indefinite soak was not restarted; its earlier formal failure cannot become passed through this smoke. Killed nodes do not claim exit-time leak checks.

### Findings and fixes

- Initial TSan cpp_comet_publisher crashed during Protobuf serialization because SDK was instrumented but consumers lacked compile instrumentation, changing conditional TSan members/layout. Comet compile requirements changed PRIVATE→PUBLIC alongside existing link propagation. Targeted and full reruns passed without disabling checks/editing third-party source.
- Initial Windows fixtures had two import errors: testkit/soak.py shadowed the old testkit/soak namespace. Legacy Redis fixture moved to testkit/legacy_soak.py with call/import/doc updates; both platforms passed. No old Redis services/SDK performance workloads ran.
- Initial Windows export-audit helper decoded UTF-8 CMake using default GBK. Explicit UTF-8 fixed it; windows-2 qualification exited 0. Initial failures remain and are not product-test successes.

SDK cases cover synchronous Catalog queries/one definite conflict repair, uncertain no-replay, bounded baseline cache, payload release, version limits, stale scheduler cancellation, close during slow callbacks, extreme waits, and reentrant View traversal. This frozen input predates new synchronous Beacon, same-ID recovery, and new reader interfaces; their later evidence is above, not part of this historical pass.

### Environment, evidence, cleanup

Linux had 16 vCPU/about 6.5 GiB initially available. Ordinary builds/tests used at most 4/4; Sanitizers 3/3. Five configurations ran sequentially respecting CTest serial constraints. Existing GCC 16.2/Go1.27.1/pinned dependencies; SDK C++23, servers C++26. TSan used existing separately instrumented dependencies. Formatting/final whitespace checks passed.

build/sdk-full-validation retains remote/linux-1 initial failure, remote/linux-2 final stages/five JUnit files, windows-2 final qualification, admin-check.log, and two Windows fixture logs. Retrieved linux-evidence.zip SHA-256 `3497d1c0031c974a874acf4735d76892dbd34e7d6e2480085de424348c6c8e39` verified. remote/verified.json checks 934 remote inputs, 40 artifact hashes, and controller/runner PID/start ticks/boot ID. Owned processes exited without leftover project executables.

No browser visual/interaction acceptance, throughput/latency comparison, multi-hour soak, or cross-machine fault tests ran. Admin still reported an approximately 502.69 kB main bundle. Linux Sanitizer results do not extend to Windows.

### Later workspace changes

After snapshot freeze, Store, Agenda, Origin, both dynamic domains' Context<State> extraction, and tests changed. workspace-final-drift.json records twelve changed files plus new astra/star/src/context.hpp: astra/common/src/store.cpp/store.hpp; astra/star/src/agenda.hpp/origin.hpp/catalog_replica.cpp/catalog_state.cpp/catalog_state.hpp/ephemeris_replica.cpp/ephemeris_state.cpp/ephemeris_state.hpp; astra/star/tests/catalog_state_test.cpp/ephemeris_state_test.cpp. These later changes/docs were outside that matrix and preserved, not qualified by its result.

## Comet Windows/MSVC SDK validation

Existing VS18/MSVC 19.51 x64 Release used C++23 for SDK/dependencies, locally mapped to /std:c++latest, not full C++26 support. Server standard stayed unchanged. Five projection/selection/subscription/lifetime/publisher tests passed. Source/package consumers configured, built, and ran; owned installation was build/comet-msvc/install. Export audit found no workspace absolute paths, accidental .proto/private keys/server-program directories.

Approved build/deps/comet-msvc/install and existing GrpcMSVC.cmake were reused, disabling optional fusion only in gRPC fused_filters.cc. No download/reinstall/expanded compatibility flags. Test/consumer compilation retained C4819/gRPC deprecation warnings; SDK /W4 /WX /utf-8 remained. Independent consumers do not qualify real Windows servers/deployment.

## Dynamic-domain helper extraction

2026-09-28: private Context<State> shares clock checks, projection directory lookup/create, history budget, and single-item notifications. Domains retain fields/locks/Pending/Retired/commit/expiry, with Catalog batch notifications separate. State tests add recovery after backward, unready-future, and missing readings.

At extraction, only source cleanup/static checks were complete. The older full matrix excludes it; newer ordinary regression above covers state, replica, allocation-fault, and preparation cases under its own identity. No automatic Sanitizer/performance/soak follows.

## Weak-page-cache diagnostics

2026-09-28 changes after the frozen full matrix affected astra/star/src/broadcast.hpp and astra/star/tests/broadcast_test.cpp: successful-rebuild counters/probes retain weak ownership. Shared pagination cases check empty weak-slot retries not counted, initial construction/hits not counted as rebuilds, and content consistency after repeated release/rebuild. SDK scheduling did not change in that review.

Initially only formatting/static review were complete. New ordinary Release above covers cpp_broadcast/dynamic domains; the older complete matrix does not qualify these new inputs under probes/Sanitizers/performance/soak. That newer ordinary run did not enable probes.

## Coverage and release boundaries

| Capability | Evidence | Unproven boundary |
| --- | --- | --- |
| Three-domain state/snapshots | Native, allocation-fault, history-gap, version/TTL/old-view cases and source-bound Sanitizers | No line/branch percentage or exhaustive configurations |
| SDK lifecycle | Install/login/revoke/cancel/lost receipts/partial stalls/Beacon keepalive/actual OnDone cleanup | Multi-machine proxies/prolonged network blackholes |
| Multi-Star basics | Three sources, nine source-target paths, kill/recovery, visibility assertions | Same-VM loopback is not physical partition/loss acceptance |
| Control plane | Pulsar transactions/holdover/recovery, Polaris SQLite/restart, Astrolabe management/metrics | No host power loss, real disk full, or NTP step |
| Probes | Sampler/parser negatives, including Sanitizers; ordinary build has no sampling state | No current performance A/B or measured new optimization gains |
| Long-running | [Soak specification](soak.md), three faults/60-second smoke; indefinite soak remains stopped | Formal multi-hour run incomplete; smoke proves no long-term leak bound |
| Delivery | Windows/Linux SDK source/package consumption, Admin build | Target dependencies, complete deployment, upgrade/rollback |

Evidence supports continued release-candidate acceptance, not “bug-free” or completed production delivery. Soak, deployment, and fault domains require separate accounting.

## Applicability of recent performance evidence

The following is the executed **2026-09-25** baseline with original source/evidence identity. Ordinary/probe configurations each passed 58/58 CTest; 32 paired samples, two CPU explorations, and two smoke samples passed. Numbers do **not** measure sending-path changes from 2026-09-26 onward. Redis/six-Star comparisons were not rerun.

## Inputs, execution, fixes

- Separate ordinary/release-probes builds used existing GCC 16.2.0/dependencies with 4 build/4 independent test jobs. No downloads, Sanitizers, soak, commit/push, Admin, frozen SDK, Redis, or six-Star rerun.
- Final manifest had 751 project files plus 55 reused fixtures; Windows/Ubuntu hashes agreed and stayed unchanged during measurement. Inputs/binaries are in evidence.
- Initial SDK table-test build lacked probe include paths, corrected. GCC 16's RAII temporary TLS-stack borrow triggered a -Wdangling-pointer false positive; only the pointer-storage line is suppressed. Nested/early-end/unwind/concurrent tests cover restoration, without project-wide warning disablement.
- Fixed-every-N sampling aligned with Runtime alternating wait/work, producing valid records but no Runtime::step samples. Thread-local xorshift32 now selects top-level spans, inherited by children; an alternating-entry case was added. Final every-Star work-step samples were checked; pre-fix hotspot ranks are excluded.
- Rebuilt enabled/disabled fixtures and reran 58 ordinary cases. Ordinary Star/Pulsar/Comet hashes were identical before/after probe fix. Probe build reran all 58, Go, and generation checks.
- Initial optional /proc/<pid>/io reads raced process exit permissions. Only that statistic now tolerates OSError; initial failure/retest evidence remains. Final independent rerun had no interruption. Initial build/profile-validation/evidence.zip is excluded from final statistics.

## Environment and workload

- Ubuntu VM: 16 vCPU, affinity 0..15, actual fixed 7419 MiB (~7.25 GiB). Minimum available during final measurement ~6.39 GiB; MemTotal unchanged. Existing swap was 16 KiB at both ends, with unchanged swap-in/out counters and no new paging.
- Three Stars accepted local writes and replicated to both peers; consumer Clients spanned all three. Same-VM loopback, internal TLS/admission enabled, public TLS/auth disabled.
- 96 records, three Scopes, twelve subscriptions/Scope (36 total), three producer plus three consumer Clients, six writers, TTL 30 s. Catalog body 2048 B; Ephemeris Data 64 B/Attr 256 B.
- Each sample created/cleaned an exclusive cluster; window 20 s. Four rounds/domain rotated compiled-out/runtime-disabled/1:64/1:256 order. Two 2 s smoke runs are excluded. One extra CPU run/domain is exploratory, not four-round paired/confidence evidence.
- Visible closed-loop waits for every target before the next write; SDK views sampled every 1 ms. QPS includes replication/subscription visibility, **not bare RPC peak or 100k capacity qualification**. Builds/offline parsing did not overlap load.

<a id="performance"></a>
<a id="baseline"></a>

## Measured performance and probe overhead

Disabled/wall configurations show medians over four rounds; CPU is one exploration. Latencies are ms. CPU/RSS cover three Stars, Pulsar, Polaris, and benchmark SDK; resource sampling includes preparation/cleanup within its window.

| Workload | Configuration | QPS | Receipt p99 | Visible p99 | Visible p99.9 | CPU cores | Peak RSS MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Catalog 2048 B | Compiled out | 1712.63 | 2.198 | 6.541 | 8.571 | 8.59 | 176.3 |
| Catalog 2048 B | Compiled in, inactive | 1705.01 | 2.107 | 6.006 | 7.314 | 8.64 | 178.7 |
| Catalog 2048 B | Wall ~1/64 | 1692.18 | 2.125 | 6.168 | 7.867 | 8.61 | 218.6 |
| Catalog 2048 B | Wall ~1/256 | 1680.87 | 2.205 | 6.168 | 7.362 | 8.54 | 189.1 |
| Catalog 2048 B | CPU ~1/256, single | 1687.47 | 2.247 | 5.810 | 7.153 | 8.22 | 189.6 |
| Ephemeris 64 B | Compiled out | 1748.48 | 2.339 | 6.868 | 8.859 | 8.69 | 135.5 |
| Ephemeris 64 B | Compiled in, inactive | 1753.22 | 2.212 | 5.845 | 7.606 | 8.69 | 137.8 |
| Ephemeris 64 B | Wall ~1/64 | 1695.25 | 2.437 | 7.558 | 9.400 | 8.70 | 177.0 |
| Ephemeris 64 B | Wall ~1/256 | 1766.01 | 2.190 | 5.754 | 6.960 | 8.53 | 148.5 |
| Ephemeris 64 B | CPU ~1/256, single | 1467.20 | 3.044 | 9.846 | 12.640 | 8.76 | 148.3 |

Paired percentages first compute configuration/ordinary−1 per round, then median, so they differ from ratios of table medians. No paired CPU overhead is calculated.

| Workload | Configuration | Paired QPS median | Paired visible p99 median | Four-round QPS differences, % |
| --- | --- | ---: | ---: | --- |
| catalog | Compiled in, inactive | -4.07% | -1.98% | -10.74, 2.61, -15.85, 14.21 |
| catalog | Wall ~1/64 | -5.55% | 11.59% | -12.95, 1.29, -3.51, -7.59 |
| catalog | Wall ~1/256 | -6.04% | -0.23% | -12.13, 0.06, -12.21, 13.87 |
| ephemeris | Compiled in, inactive | 0.21% | -13.15% | -2.33, 2.75, -21.27, 19.17 |
| ephemeris | Wall ~1/64 | -2.80% | 1.53% | -4.02, -4.58, -1.57, 2.85 |
| ephemeris | Wall ~1/256 | -0.26% | -13.11% | -3.29, 2.78, -19.16, 20.88 |

**Low-overhead goals are not fully established.** Default 1/64 Catalog paired throughput−5.55%/visible p99+11.59% slightly exceed ~5%/10% screening goals; Ephemeris is−2.80%/+1.53%. 1/256 reduces records/memory but is not proven better for every tail.

Ordinary Catalog QPS across rounds was 1976.36,1700.80,1724.45,1457.69; Ephemeris 1857.33,1711.38,1785.58,1466.75. Identical binaries drifted and had tail spikes. Without concurrent host-frequency/preemption evidence, neither cause nor precise small causal overhead can be claimed. All unfavorable samples remain. **Production defaults to compiled-out probes; probes serve bounded diagnostics, not production performance ranking.**

## Microbenchmarks, capacity, integrity

Five runs/configuration, one million identical work calls each, with one warmup call before timing. All checksums matched. One work contains three synchronous spans, one counter, and fixed integer work; these are not single-probe costs and exclude initial file reservation.

| Configuration | Median ns/work | Increase over compiled-out ns |
| --- | ---: | ---: |
| Compiled out | 28.130 | 0.000 |
| Compiled in, inactive | 31.501 | 3.371 |
| Wall ~1/64 | 44.220 | 16.090 |
| Wall ~1/256 | 38.384 | 10.254 |
| CPU ~1/256 | 62.091 | 33.961 |

- All 95 service/SDK and 15 microbenchmark files parsed. Service/SDK totals:4,973,661 synchronous spans,235,103 counters,126,837 async intervals, including 21,925 Runtime work steps; missed=0/errors=0. Toggle, unwind, cross-thread timestamp, sampling-phase, and overflow cases passed. This is neither branch coverage nor correlated end-to-end request count.
- Each enabled process reserves 64 MiB; five files/cluster sample reserve 320 MiB disk. Touched pages/RSS are smaller. Aggregate peak RSS increased about 42 MiB at1/64 and 13 MiB at1/256.
- Median /proc I/O write deltas during 20 s: Catalog 39.66/9.89 MiB at1/64/1/256; Ephemeris 38.94/10.64 MiB. Includes some logs, excludes prior reservation and potentially later kernel flushing; not total physical disk writes.

## Hotspot evidence and next steps

CPU data is one ~1/256 run/domain for candidate ranking only. own subtracts instrumented synchronous children but includes uninstrumented work/probe overhead. These are not uninstrumented function costs; quantiles from different functions cannot be summed.

| Star path | Catalog mean own CPU μs / samples | Ephemeris mean own CPU μs / samples |
| --- | ---: | ---: |
| Downstream advance | 50.09 / 3366 | 59.09 / 3083 |
| Peer outbound begin_write | 112.03 / 205 | 135.76 / 174 |
| Peer inbound begin_write | 111.24 / 194 | 135.60 / 179 |
| Runtime step own | 16.63 / 510 | 20.95 / 457 |

Downstream advance accounts for about 35.4% of recorded synchronous own-CPU sum, **not total process CPU**. It still includes unsplit gRPC StartWrite work and cannot be attributed directly to Protobuf/allocation/network.

The next table pools four 1/256 wall rounds by observation count; it is not one request timeline.

| Interval | Catalog μs / samples | Ephemeris μs / samples |
| --- | ---: | ---: |
| Peer read ready → Runtime consumption | 225.71 / 1421 | 208.16 / 1506 |
| Peer StartWrite → completion | 174.06 / 1457 | 169.56 / 1568 |
| Downstream StartWrite → completion | 131.41 / 4629 | 124.56 / 4995 |
| SDK ready → consumption | 38.74 / 4227 | 35.79 / 4449 |
| Local change Scope-lock wait | 13.95 / 534 | 10.60 / 583 |
| Remote receive Scope-lock wait | 0.69 / 1039 | 0.63 / 1151 |

1. Separate page preparation, gRPC submission, callback/wakeup costs in downstream advance/peer begin_write, then A/B one candidate. Evidence does not justify Scope-lock, Actor, or global-RCU redesign as first priority.
2. Evaluate bounded interleaving of ready reads/writes and completion coalescing against ~200 μs peer-consumption waits, preserving ownership/fairness/drain rather than blindly changing Runtime quotas/removing ACK.
3. Capacity needs separately increasing concurrent commit load with replication/subscription lag. Six-writer all-visible closed-loop cannot answer 100k QPS. Small-gain comparisons require more stable host windows.

gRPC/TLS internals, kernel scheduling, Go control plane, cross-machine/high-key/large-recovery/idle-Watch behavior are outside this attribution. Low-sample P99.9 is not stable evidence. See [methods](profile.md).

## Performance evidence and cleanup

Final build/profile-verification/evidence.zip SHA-256 `1d751c85cf6f1811e0978398ae56a3de58d91ae5986ec1e882195413a9e4c753` contains commands/scripts, input hashes, regressions, per-sample resources/results, binary records, and offline reports. All managed processes across 36 final samples exited 0, with no owned services/background load remaining.

| Artifact | Ordinary SHA-256 | Probe SHA-256 |
| --- | --- | --- |
| star | `490fd750591239ec3cc2d683bc0a00fcf99f3bbc36cdbdccda9d8e0a1e2f92da` | `511b9e7ddbf0d99efa3be3bf7184c06be675f4b1c9fe6d42da580ec2e3159872` |
| baseline_comet | `79e8a5301345c94a99a690445e3079de4a096c8054ed2082b49caf1fc561e934` | `1206e36b16dad43dad4ab41cd59ac620a0e52f31b0b4a85c0b04c1e554324b91` |
| pulsar | `869bbb0e6f96dfe925d4ef88a481eee40cadb58860f4c3e913ad1c2794d0cb17` | `6fa29d9c22e650e8be82d3b2f0ad2840278e5d04eb73adb904dc1736786f1b09` |
| polaris | `1172df99ce787b54770265f89fe3f216a8231c34fb7d8920762d2c7d00bdafc1` | `1172df99ce787b54770265f89fe3f216a8231c34fb7d8920762d2c7d00bdafc1` |
