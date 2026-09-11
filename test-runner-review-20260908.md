# Unified test runner review — 2026-09-08

This review covers the new regression/soak controller and the existing Python
harnesses it calls. It supplements the earlier SDK line-level audits; it does
not replace their source inventories or imply that all release gates are met.

## Repaired ownership and orchestration problems

| Finding | Impact and propagation review | Change |
| --- | --- | --- |
| A22/A23: peer construction/readiness could fail before cleanup ownership was established | Registration and Catalog interop plus Sentinel peers share the same failure boundary | Shared owned processes/peers and `ExitStack`; regressions cover a failed second spawn and failed readiness |
| Parent exit did not imply descendant exit | Affects all SDK commands, especially `go run`, Cargo and native build subprocesses | Windows Job ownership before child execution; POSIX sessions; bounded shutdown after success, failure and timeout |
| Creation intent was recorded too late | Standalone, Sentinel and both soak fixtures could leave containers/directories after an interrupted create | Register intent first; retain a locked ownership manifest; recover after a dead creator |
| Cleanup could remove or misreport another owner's resource | Shared Docker fixtures and both interop Zone cleaners | Verify labels and immutable container IDs, exact directory/owner markers, and Zone ownership; preserve occupied foreign resources |
| Linux `setsid` returned before its worker finished | Premature controller success and broken output for every Linux SDK stage | Use `setsid --wait`, validate worker exit against its final report, keep remote cleanup pending if no completion evidence arrives |
| Sentinel configuration was readable but not writable | All Go/Rust/C++/C# Sentinel stages failed at container readiness | Give only the generated Sentinel configuration the required writable mode; retain server diagnostics on failed readiness |
| Native standalone tests omitted Redis credentials | C++ native tests, C ABI tests and Legacy tests all assumed an anonymous endpoint; Go/Rust/C# already supplied credentials | All native consumer tests now accept the isolated authenticated fixture; JSON is serialized by Python and checked for truncation in the C test |
| Independent Rust peer lockfiles referenced uncached packages | Catalog and Sentinel peers do not share the SDK's exact dependency closure | Fetch the already-approved declared dependencies for each peer into both project caches; keep test execution offline |
| Logs and latency samples grew with workload length | Shared Python command output and Go Registration soak; Catalog already had a bounded histogram | Bound output queues/logs and Redis samples; share a fixed-size Go histogram for both domains |
| Temporary files used system temp and ignored cleanup errors | Sentinel peer binaries, TLS material and C# publication outputs | Use project-owned temporary directories, report cleanup errors, recover inactive directories after a crash |
| C++ accessed a success value after a failed Pub/Sub response | Redis `NOPERM` reached the error queue, then `value()` threw on the reactor thread and terminated the native process; C#/C ABI share that native core | Stop the rejected connection before accessing its success value; a deliberate denied-pattern test now checks an error return and continued root use |
| A newer C++ concurrency test bypassed Sentinel's exact-channel scope | Its new Part subscription required a literal ACL pattern that the restricted fixture intentionally did not grant | Keep the existing exact-Path subscription in Sentinel mode; test permission denial separately without loosening the fixture ACL |
| First-time Rust peer compilation consumed the readiness timeout | Both interop launch paths mixed compilation with the peer protocol | Build Go and Rust serially before starting their readiness deadlines |
| PowerShell's hidden child could outlive cancellation of its launcher | The SDK shell adapter did not forward cancellation to Python | Keep console prompting in PowerShell; a project-local cancellation request and parent-process handle trigger Python cleanup |
| Certain configurable durations omitted fault kinds | Catalog's long schedule was empty at ten minutes; Registration restarts stopped after the original two-hour profile | Shorter profiles retain all fault kinds, while longer profiles repeat the long schedule through the selected duration |
| Fault timing included compilation and setup | Both continuous Go workloads could receive compressed faults before they began exercising SDK operations | Each workload emits readiness; the shared injector starts its clock only on that marker. The public minimum is 210 seconds so lifecycle checks have sufficient time |
| SDK scratch files escaped fixture ownership | Native SQLite checkpoints and toolchain temporary files used the operating-system temp directory | Each campaign passes an owned project directory through child-only TEMP/TMP/TMPDIR/GOTMPDIR; ordinary and stale-run cleanup cover its contents |
| Sentinel agreement preceded SDK command recovery | The Catalog Go/Rust peer harness issued a new mutation before a command connection recovered from the killed primary | Probe root PING with a bounded deadline before new writes. No ambiguous mutation is retried by the harness |
| Sentinel's one-second failure detector destabilized a resource-limited fixture | Both platforms and every SDK share the same six-container topology | Use the five-second threshold already qualified by the C# fixture for every Sentinel harness; keep two actual primary losses and convergence assertions |
| Windows's Go clock can return equal consecutive readings | Registration's old zero-latency rejection failed an otherwise completed Windows load; Catalog retains zeros without this rejection, and Rust/native/managed tests have no equivalent assertion | Local Go 1.27.1 runtime source uses shared interrupt time; an independent million-read probe recorded 999,910 equal consecutive readings and no backward readings. Replace the false zero-value gate with complete sample-count validation; retain observed zeros and latency limits |

The latency change preserves exact sample counts and maxima. Percentiles are
conservative bucket estimates with at most 1/16 relative bucket width above
16 ns, so they must not be described as exact sorted percentiles. Histogram
storage remains fixed as duration grows. Rust's separate legacy load test still
retains samples for its bounded, at-most-one-hour profile; it is not used as a
continuous endurance driver by the unified soak command. C++ and C# have no
dedicated continuous collector to migrate. Those endurance gaps remain explicit.

## Verification evidence

- Windows and Ubuntu: 23 framework tests passed on each, including timeout, surviving descendants,
  second-spawn/readiness failure, foreign ownership, crash recovery,
  interrupted-run reporting, and remote cleanup uncertainty.
- A real creator process was forcibly terminated after creating a Docker
  container and directory on Ubuntu. Recovery removed both owned resources and
  preserved the pre-existing audit Redis container. Evidence:
  `build/testkit/live-cleanup-validation.json`.
- The final PowerShell 5.1 launcher-death rerun passed under
  `build/testkit/runs/b2594570120cc86f`; its interrupted campaign cleaned
  owned resources and retained its report without the former output error.
- A separate live source-sync sandbox accepted the initial transfer, rejected
  a later conflicting VM edit without overwriting it, then cleaned its owned
  directory. Evidence: `build/testkit/source-conflict-validation.json`.
- The first real campaign exposed the Sentinel permissions, native credentials,
  independent peer dependency and detached-worker problems above; its failed
  evidence is retained under `build/testkit/runs/fe33883549469fe8`.
- The final dual-platform regression passed under `c57fedac5de9e92f`:
  Windows 26/26 and Ubuntu 26/26 stages, 977.025 seconds total, both cleanup
  results `cleaned`. Source SHA-256:
  `947bf7925e4a9a510988452098b56e8be3fd938f162cc2a8c4eae47ba2420632`.
- Final soak `e4df1e6a6b3db9f8` passed 20/20 stages per platform in 1,206.204
  seconds on that same source. All four Go workloads completed six fault
  injections each, passed their 210-second Redis-time floor and left zero
  keys. Both cleanup results are `cleaned`. Redis elapsed times were 236.433/
  213.534 seconds on Windows and 230.697/212.116 on Ubuntu for Registration/
  Catalog respectively. This is short-profile validation, not hours of endurance.
- The native abort was reproduced in campaign `f05d35f321873da0`, which was
  stopped after repeated dialogs and fully cleaned. After the driver repair,
  both plain and TLS C++ Sentinel tests passed with deliberate ACL rejection.
  Go/Rust TLS two-promotion qualification also passed in the focused rerun.

The subsequent framework suite has 23 passing tests, including the workload
readiness clock, partial observations after command failure, and SDK scratch
cleanup after failure/interruption. Campaign `8e4921ac8e5db37a` retained one
Sentinel timing failure on each platform (25 passing stages on each); its
cleanup completed. Both targeted timing repairs subsequently passed on Windows.
The first 210-second-per-domain soak, `24c1b3c3e924c330`, passed both Ubuntu
domains and Windows Catalog. Windows Registration completed its workload but
failed the old zero-duration assertion. All owned resources were cleaned;
its failed report remains intact. The corrected final-snapshot soak passed
separately, with no source edits during either campaign. Its Windows
Registration result preserves 1,472 measured zero durations among exactly
105,000 update samples.

Final inventory checks found no owned test containers, fixture directories,
temporary directories, pending resource manifests, or remaining project test
processes on either host. The pre-existing audit Redis container remains.
Ubuntu had 6,662 MiB available and zero swap use after completion. Compact
evidence is in `testkit/results/unified-test-runner-20260908.json`; full logs and
the downloaded Linux workload reports remain under the campaign directories.

The local timing evidence is in `build/testkit/clock-validation.log` and the
installed Go runtime's `runtime/time_windows_amd64.s`; the related
[upstream Go discussion](https://github.com/golang/go/issues/67066) explains
why improving Windows clock resolution is a runtime concern. The harness
retains zero observations instead of changing their measured values.

## Native abort propagation review

| Layer | Reviewed path | Assessment |
| --- | --- | --- |
| C++ Registration and Catalog | `sdk/cpp/src/driver.cpp`, shared subscription receive/start paths | Affected; failed response never reaches `value()`; rejection maps to unavailable |
| C ABI and Legacy | `sdk/cpp/src/c/catalog.cpp`, `sdk/cpp/include/verdandi/legacy/catalog.hpp` | Consume the repaired core and preserve its error; no separate Redis receive loop |
| C# | `sdk/csharp/src/Verdandi/Catalog/CatalogSubscriber.cs`, `Internal/Interop.cs` | Consumes the same core through C ABI; a native reactor exception could terminate the managed host before its result wrapper runs |
| Go | `sdk/go/catalog/subscriber.go`, `sdk/go/registration/selector_core.go` | Reviewed subscribe/receive errors close the Pub/Sub connection and return before reading a successful frame |
| Rust | `sdk/rust/src/catalog/subscriber.rs`, `sdk/rust/src/registration/selector.rs`, `sdk/rust/src/client.rs` | Reviewed subscription setup propagates Result errors; ConnectionTask owns shutdown; no matching unchecked-success access |
| Lua and Python | Shared Redis scripts; Sentinel fixture and native executable launcher | Lua is not involved in Redis ACL rejection. Python now requires the exact normal failure exit in TLS negative cases; abort exit 3 remains a failure |

The native regression deliberately requests a forbidden pattern under the
existing restricted ACL, asserts NOPERM/unavailable, and confirms the root can
still PING. The test executable also prints diagnostics and exits nonzero on an
unexpected terminate; this does not catch errors inside the library or make an
abort pass. Cross-language rows distinguish source review from actual runtime
coverage; direct denied-pattern tests for each managed/foreign binding remain
separate from their shared-core regression evidence.

Redis requires literal ACL pattern matches for `PSUBSCRIBE`, unlike ordinary
channel subscription. See the [Redis ACL documentation](https://redis.io/docs/latest/operate/oss_and_stack/management/security/acl/).
The native driver must still return a normal error when that requirement is
not met. [Boost.Redis's example](https://www.boost.org/library/latest/redis/)
likewise ends the receive loop on an error-bearing response before `value()`.

## Remaining qualification boundaries

The current controller reports separate platform, language and scenario rows.
Live mutual TLS, dedicated Rust/C++/C# continuous endurance, direct native C++
two-promotion peers, packaging/AOT and platform combinations outside the tested
Windows/Ubuntu x64 checkouts are not certified by this work. Historical soak
results from older source snapshots remain historical evidence.

## Code-size tradeoff

The nine migrated existing Python harnesses fell from 4,355 to 3,410 lines
(945 fewer). The six shared controller/process/resource modules add 1,541
lines, plus 351 lines of framework tests at this review point. This work is
therefore a consolidation of policy and addition of automation/ownership
capabilities, not a claim that total repository code became smaller. Future
SDK or scenario additions should reuse these modules rather than restore
per-harness process, SSH, cache, cleanup or report implementations.
