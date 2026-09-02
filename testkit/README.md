# Verdandi Testkit

The current testkit covers the Registration and Catalog Lua contracts,
independently executable Go, Rust, C++, and C# regressions, live Go/Rust
interoperability, and isolated Redis Sentinel fault topologies. A language's
own acceptance does not require every other language to run in the same
process or fixture; cross-language interoperability remains a separate gate.
Use an isolated Redis Open Source 8 endpoint: some tests reset command
statistics or flush the script cache. Tests generate a random alphabetic Zone,
delete only that Zone's keys, and never run `FLUSHDB` or `FLUSHALL`.

## Python dependencies

```text
python -m pip install -r testkit/lua/requirements.txt
python -m pip install -r testkit/sentinel/requirements.txt
```

## Authenticated standalone qualification

The standalone harness connects to a dedicated Docker host, creates one exact
randomly named Redis 8.8 container with generated ACL credentials, runs all
Lua/Go/Rust/interop suites, verifies final `DBSIZE=0`, and removes only the
container bearing its run label. On Windows it runs timing-sensitive Go load
and race tests in WSL/Linux.

PowerShell functional and race run:

```text
$env:VERDANDI_TEST_SSH_PASSWORD = "<temporary test-host password>"
python -B testkit/standalone/standalone_test.py --host 192.168.0.90 --ssh-user ubuntu --skip-load --result-file testkit/results/standalone-functional.json
```

The current production-cleanup Catalog evidence is
`testkit/results/catalog-production-cleanup-functional-20260825.json`. Besides
the normal matrix it injects both a visible `1 -> 3` Stream hole and an advanced
Hash revision whose final Stream entry is wholly absent; Go and Rust must
recover both without using an existing Register or Sentinel fixture.
The 2026-08-28 Rust transport-boundary matrix is
`testkit/results/rust-transport-refactor-functional-20260828.json`; it adds a
live root test proving that one transport serves two Registration Zones and
that root-first close does not join domain Clients.

Formal five-minute update plus five-minute renewal run in each language, eight
Selectors, and 5,000 records:

```text
$env:VERDANDI_TEST_SSH_PASSWORD = "<temporary test-host password>"
python -B testkit/standalone/standalone_test.py --host 192.168.0.90 --ssh-user ubuntu --load-seconds 300 --selector-fanout 8 --scale-registrations 5000 --result-file testkit/results/standalone-long.json
```

`--load-seconds` accepts 1-3,600, `--selector-fanout` 1-64, and
`--scale-registrations` 1-100,000. These bound a test run; they do not define a
protocol Registration maximum. `--keep-container` retains only that exact run
for diagnosis. JSON includes every suite's captured output plus Redis CPU,
memory, command, and network counters.

## Fault-injected Registration/Selector endurance qualification

The soak harness keeps 500 direct typed Registrations updating once per
scheduling second and fans out to eight typed Selectors. `One` and `Any`
transactions stage local `Power` mutations while remote typed Updates exercise
field-granular correction. Parallel raw compatibility workloads cover natural
expiry/retained recovery and explicit churn. The harness samples Redis every 30
seconds and injects script-cache loss, Pub/Sub and ordinary-connection loss,
Redis pauses, and AOF restarts. It verifies exact final revision/content and
local-prediction convergence, retained-view bounds, goroutine return to
baseline, stable Redis memory, zero evictions/rejected connections, and final
`DBSIZE=0`.

The producer uses one cadence scheduler rather than one goroutine per
Registration, and diagnostics are consumed through the Client aggregate rather
than one watcher per Registration. This keeps the test itself from hiding SDK
ownership costs. Every published Registration deliberately owns one worker, so
the Go gate requires at least baseline plus 500 and rejects a peak above that
expected population plus 128. The Rust gate applies the equivalent live-Tokio-
task range. Every Selector may own one persistent listener and at most one
temporary full-sync or targeted-repair worker. The allowance covers fixed test
observers and Redis driver recovery helpers; it is not a protocol service-count
limit.

```text
$env:VERDANDI_TEST_SSH_PASSWORD = "<temporary test-host password>"
python -B testkit/soak/soak_test.py --duration-seconds 8000 --minimum-redis-seconds 7200 --selector-fanout 8 --sample-seconds 30 --lifecycle-interval 5m --run-sentinel --result-file testkit/results/registration-per-instance-soak-owned-update-2h-20260826.json --sample-file testkit/results/registration-per-instance-soak-owned-update-2h-20260826-samples.jsonl --sentinel-result-name registration-per-instance-sentinel-owned-update-2h-20260826.json
```

`--minimum-redis-seconds` defaults to `--duration-seconds` and is measured with
Redis `TIME`, independently of the client/WSL clock. The example deliberately
schedules an 800-second client margin after WSL was observed jumping forward by
roughly six minutes. The Redis gate remains authoritative and rejects the run
if even that margin is insufficient. Fault scheduling uses the qualification
floor, not the safety margin. Every Redis sample is flushed to a sibling
`*-samples.jsonl` file; the standalone result is checkpointed before the
optional Sentinel matrix.

The direct typed authoritative run and detailed interpretation are in
`testkit/results/registration-typed-soak-2h-pass-20260825.json` and
`registration/typed-soak-20260825.md`. The earlier raw-core qualification is
retained in `testkit/results/registration-soak-2h-20260825.json` and
`registration/soak-20260825.md`.

Those 2026-08-25 files and the later
`registration-concurrency-redesign-*` files predate the final
per-Registration queue/worker/timer ownership. They remain protocol, Lua, and
fault-history evidence, but not current worker-topology evidence. Current
preflight evidence is retained in
`registration-per-instance-reply-validation-functional-20260826.json`,
`registration-per-instance-reply-validation-load-20260826.json`,
`registration-per-instance-reply-validation-soak-preflight-20260826.json`, and
`registration-per-instance-reply-validation-sentinel-20260826.json`.

The command above completed successfully on the frozen 58-file fingerprint
`c7bef517173b9c298e41b6dac272e78736b317c017bbe70ba838185960bdf63a`.
Its authoritative result, flushed sample stream, and automatic Sentinel result
are respectively:

- `registration-per-instance-soak-owned-update-2h-20260826.json`;
- `registration-per-instance-soak-owned-update-2h-20260826-samples.jsonl`; and
- `registration-per-instance-sentinel-owned-update-2h-20260826.json`.

Redis `TIME` measured 7,866.527 seconds; all 34 standalone faults and the
two-promotion Sentinel matrix passed. The harness cleaned its owned containers,
directories, keys, and ports after writing the results.

The later child-package migration is covered by the compact aggregate
`registration-package-migration-20260826.json`. Its exact final 88-file source
fingerprint passed the 90-second six-fault AOF gate in
`registration-package-migration-final-90s-20260826.json`; flushed samples are
in the sibling JSONL file. The separate
`registration-package-migration-sentinel-20260826.json` records the complete
two-promotion Go/Rust matrix. The rejected 30-second expiry-gate result is kept
with `expiry-gate-failed` in its name so it cannot be mistaken for acceptance
evidence.

## Fault-injected Catalog endurance qualification

The Catalog soak owns a separate Redis 8 AOF fixture and drives the current Go
Publisher plus multiple complete Subscribers. Its production defaults maintain
16 Catalog Paths, 256 fields of 256 bytes, two Subscribers, and 128
Replace/Patch/Delete attempts per second. The controller retains each last
accepted revision and uses Subscriber state only after an ambiguous result.

```text
$env:VERDANDI_TEST_SSH_PASSWORD = "<temporary test-host password>"
python -B testkit/catalog/soak_test.py --duration-seconds 7200 --minimum-redis-seconds 7200 --sample-seconds 30 --result-file testkit/results/catalog-soak-2h.json --sample-file testkit/results/catalog-soak-2h-samples.jsonl
```

Both the Go workload deadline and the outer qualification floor use Redis
`TIME`, independently of the WSL/client clock. `--minimum-redis-seconds`
defaults to `--duration-seconds`; fault scheduling uses that qualification
floor. The harness injects `SCRIPT FLUSH`, Pub/Sub and normal-command connection
loss, Redis pause, and AOF restart. It records Redis/process memory, latency,
retry, convergence, revision, and asynchronous diagnostics, then runs the
current Lua contract, Rust `catalog_v2` integration suite, and Catalog-only
Go/Rust interoperability. Its source fingerprint covers only Catalog
Lua/Go/Rust/harness files, so unrelated Registration work cannot invalidate a
Catalog run.

Every sample is flushed to the sibling `*-samples.jsonl` file. A console
interrupt writes status `interrupted`, the latest structured heartbeat,
completed faults, and all samples before removing only the exact owned
container and directory. This is the supported way to stop a long run and
retain valid partial evidence. Use `86400` for both duration options for a
24-hour campaign.

The current accepted result is
`testkit/results/catalog-soak-2h-redis-clock-20260826.json`, with flushed
samples in the sibling JSONL file. Redis 8.8 `TIME` measured 7,201.265 seconds;
960,000 attempts, all 18 planned faults, final convergence, all post-checks,
and final `DBSIZE=0` passed on the frozen 72-file fingerprint
`bb994a871b4d9c4679e8ecf800cba1c6d4f37df8ab25a67dfc8d76d161e68cd1`.

## Source-freeze twelve-hour campaign

Run both domains from a detached worktree at the exact frozen commit, not from
the mutable review checkout. They use separate ports and independently labelled
Redis 8 AOF fixtures; concurrent execution is an endurance and recovery gate,
not a comparative throughput benchmark.

```text
$env:VERDANDI_TEST_SSH_PASSWORD = "<temporary test-host password>"
python -B testkit/soak/soak_test.py --port 37380 --duration-seconds 43200 --minimum-redis-seconds 43200 --selector-fanout 8 --sample-seconds 30 --result-file <result-dir>/registration-soak-12h.json --sample-file <result-dir>/registration-soak-12h-samples.jsonl
python -B testkit/catalog/soak_test.py --port 37440 --duration-seconds 43200 --minimum-redis-seconds 43200 --sample-seconds 30 --result-file <result-dir>/catalog-soak-12h.json --sample-file <result-dir>/catalog-soak-12h-samples.jsonl
```

Both inner workloads and both outer acceptance floors use Redis `TIME`. The
source fingerprints in the results must match the frozen worktree, every
planned fault and post-check must pass, and each fixture must finish with zero
owned keys and remove only its exact container and remote directory.

The exact 2026-08-31 freeze completed this campaign on 2026-09-01. Registration
passed 43,213.948 Redis seconds and 214 faults; Catalog passed 43,201.858 Redis
seconds and 113 faults. The self-contained final results, including all Redis
samples, are
[`results/registration-soak-12h-freeze-20260901.json`](results/registration-soak-12h-freeze-20260901.json)
and
[`results/catalog-soak-12h-freeze-20260901.json`](results/catalog-soak-12h-freeze-20260901.json).

## Shared Lua contract

Generate the canonical Registration and Catalog scripts and the byte-identical
Go and Rust embedded copies, or verify that working-tree artifacts are current:

```text
python testkit/lua/generate_registration.py
python testkit/lua/generate_registration.py --check
python testkit/lua/generate_catalog.py
python testkit/lua/generate_catalog.py --check
```

```text
python testkit/lua/registration_test.py --redis-url redis://127.0.0.1:6379/0
python testkit/lua/catalog_test.py --redis-url redis://127.0.0.1:6379/0
```

This verifies the exact positional request ABI, alternating key/value replies
and MessagePack events, revision/idempotency transitions, Redis-time deadlines,
the `2^46-1` Hash-field expiry boundary, matched key/field TTL, natural expiry,
independent per-script `NOSCRIPT` reload, and the no-`HGETALL` Update hot path.
It also directly bypasses the SDK with over-count/oversized fields to prove Lua
is atomic glue rather than a second schema validator; Go and Rust tests prove
the supported SDK rejects the same invalid input locally.

The Catalog suite verifies strict four-script ABIs, Value/Array/Map Replace,
exact-base Patch, fresh Delete tombstones, full MessagePack Pub/Sub operations,
per-field revision Read, live/deleted indexes, bounded tombstone floor
advancement, Redis-type/orphan corruption rejection, reload, and the exact
`2^53-1` revision boundary, and one-winner exact-base Patch contention without
an external lock. Go and Rust additionally exercise streaming
allocation-bounded decoders, stable Entries, different typed loads per Path,
full in-memory Subscribers, notification-gap/reconnect repair, monotonic
bbolt/redb restart, below-floor full alignment, and joined shutdown. C++
additionally covers typed/raw boundaries, SQLite restart, sanitizers, and its
Standalone/Sentinel startup paths. Each Subscriber multiplexes all
exact/pattern subscriptions through one persistent Pub/Sub listener and at
most one temporary synchronization/repair task.

Measure the generated one-field, version-plus-Data, and 32-Data-field Update
paths, plus Renew and Unregister, on an isolated persistence-disabled endpoint:

```text
python testkit/lua/registration_benchmark.py --redis-url redis://127.0.0.1:6379/0 --output testkit/results/lua-registration-hot-path.json
```

The benchmark rotates variant order across eleven trials and uses the same
records, batches, connection, and Redis fixture for all operations. It
intentionally has no subscribers; it measures current script-body cost, not
Pub/Sub fan-out. Supplying all three cached `--baseline-*-sha` options enables
a same-fixture old-SHA/candidate comparison without `SCRIPT FLUSH`.

Measure current Register regression baselines for a small record and the
default 16-Attr/32-Data, 128-byte-per-field record:

```text
python testkit/lua/registration_line_benchmark.py --redis-url redis://127.0.0.1:6379/0 --output testkit/results/lua-register-current.json
```

The historical paired optimization matrix is retained in
`testkit/results/lua-register-line-optimization-20260824.json`. A cached old
script can be compared with `--baseline-sha` and
`--baseline-source-bytes`; the current script never reconstructs a superseded
body. The authoritative second-pass results are
`lua-register-line-final-20260824.json` and
`lua-registration-line-final-20260824.json`.

Measure current small/wide Replace, one-field Patch, full/delta/unchanged Read,
and Delete/tombstone paths:

```text
python -B testkit/lua/catalog_line_benchmark.py --redis-url redis://127.0.0.1:6379/0 --output testkit/results/catalog-v1-benchmark.json
```

The current measured artifact is
`testkit/results/catalog-v1-benchmark-20260826.json`. It is a sequential
same-endpoint latency comparison rather than a concurrent capacity promise;
detailed interpretation is in `catalog/optimization.md`.

## Go SDK

```text
cd sdk/go
go test ./...
go test -shuffle=on -count=10 ./...
go vet ./...
go test -tags=integration ./...
go test -race -tags=integration ./...
go test -run=^$ -fuzz=FuzzDecodeRegistrationEvent -fuzztime=60s -fuzzminimizetime=1s ./registration
go generate ./...
go test -tags="integration load" -run TestRegistrationSelector -count=1 -timeout=2400s -v ./registration
go test ./catalog
go vet ./catalog
go test -shuffle=on -count=10 ./catalog
go test -tags=integration ./catalog -run TestCatalogPublisherSubscriberIntegration -count=1 -v
go test -race -tags=integration -count=1 ./catalog
go test -run=^$ -fuzz=FuzzDecodeCatalogEvent -fuzztime=60s -fuzzminimizetime=1s ./catalog
go test -tags="integration load soak" ./catalog -run TestCatalogSoak -count=1 -v
```

Set `VERDANDI_REDIS_URL` before integration and load commands.
The explicit per-input fuzz minimization budget prevents one newly discovered
coverage input from consuming the whole campaign while Go minimizes it; crash
reproduction still receives the generated failing corpus and a separate
focused minimization run when needed.

## Rust SDK

```text
cd sdk/rust
cargo fmt --all --check
cargo test --all-targets
cargo clippy --all-targets --all-features -- -D warnings
cargo doc --no-deps
cargo test --test integration registration_and_selector_reconcile_on_redis_8 -- --ignored --nocapture
cargo test --test integration zone_configuration_refreshes_without_restart -- --ignored --nocapture
cargo test --test integration dropping_last_registration_client_handle_stops_owned_workers -- --ignored --nocapture
cargo test --test catalog_v2 -- --nocapture
cargo test --release --test load registration_selector_qualification_load -- --ignored --nocapture
cargo test --release --test load registration_selector_renewal_load -- --ignored --nocapture
cargo test --release --test load registration_selector_scale_recovery -- --ignored --nocapture
cargo test --release --test load -- --ignored --nocapture
```

Set `VERDANDI_REDIS_URL` before integration and load commands. External Rust
tests are explicitly ignored when selected without `--ignored`; they do not
silently pass when their required endpoint variable is absent. Run standalone
tests by exact name because the same file also contains the Sentinel test. The
complete Rust load command is also safe: the suite serializes its endpoint-wide
Redis command-statistics sections internally.

## C++23 SDK

From `sdk/cpp`, configure and run the strict Debug and sanitizer presets:

```text
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug --output-on-failure

cmake --preset gcc-asan-ubsan
cmake --build --preset gcc-asan-ubsan
ctest --preset gcc-asan-ubsan --output-on-failure
```

Set `VERDANDI_REDIS_ADDRESS` for authenticated Standalone integration, or use
the isolated Sentinel smoke below. The integration executable creates unique
Registration and Catalog Zones, deletes only its exact owned keys, removes its
temporary SQLite files, and skips with code 77 when no endpoint is configured.

## C# managed SDK

The C# facade owns an independent regression rather than being appended to the
Go/Rust aggregate. From `sdk/csharp`, run:

```text
$env:VERDANDI_TEST_SSH_PASSWORD = "<temporary test-host password>"
python -B tests/standalone_test.py --host 192.168.0.90 --ssh-user ubuntu --result-file ../../testkit/results/csharp-standalone.json
python -B tests/sentinel_test.py --host 192.168.0.90 --ssh-user ubuntu --result-file ../../testkit/results/csharp-sentinel.json
```

Standalone builds the C++ shared Release runtime, applies the managed
formatter/analyzer gate, builds and runs .NET 8 and .NET 10 offline, publishes
both as self-contained Linux x64 applications, then runs each against its own
ACL-protected Redis 8.8 fixture. Sentinel keeps both managed peers alive across
acknowledged-write loss, two promotions, `SCRIPT FLUSH`, complete Sentinel
loss, unavailable views, and recovery. Both harnesses remove only their exact
owned resources and write a result only after final empty-database cleanup.

The accepted 2026-08-31 evidence is
`testkit/results/csharp-standalone-20260831.json` and
`testkit/results/csharp-sentinel-20260831.json`.

## Go/Rust live interoperability

```text
python testkit/interop/interop_test.py --redis-url redis://127.0.0.1:6379/0
python testkit/catalog/interop_test.py --redis-url redis://127.0.0.1:6379/0
```

The harness starts one Go peer and one Rust peer with synchronized empty
Selectors and Catalog Subscribers. Go registers and updates a binary-valued
record that Rust observes through live Pub/Sub; Rust performs the reverse
direction. Go then Replaces a binary Map at Catalog revision 1, Rust observes
and Replaces it at revision 2, Go observes the complete notification and
Deletes at revision 3, and Rust observes the tombstone. It verifies content,
revisions, terminal cleanup, and an empty test-owned Zone.

## Redis Sentinel fault qualification

The Sentinel harness connects by SSH to a dedicated Docker host and creates
only resources bearing one random run identifier. It starts three Redis 8.8
nodes on ports 16381-16383 and three Sentinels on ports 26381-26383 with host
networking, persistence disabled, and separate generated Redis/Sentinel ACL
credentials. Do not run it on a host where those six ports are in use.

PowerShell example:

```text
$env:VERDANDI_TEST_SSH_PASSWORD = "<temporary test-host password>"
python -B testkit/sentinel/sentinel_test.py --tls --runtime win-x64 --host 192.168.0.90 --ssh-user ubuntu --result-file testkit/results/sentinel-tls-windows.json
python -B testkit/sentinel/sentinel_test.py --tls --runtime linux-x64 --host 192.168.0.90 --ssh-user ubuntu --result-file testkit/results/sentinel-tls-linux.json
python -B testkit/catalog/sentinel_test.py --host 192.168.0.90 --ssh-user ubuntu --result-file testkit/results/catalog-sentinel.json
python -B testkit/cpp/sentinel_smoke.py --tls --runtime win-x64 --build msvc-shared-release --host 192.168.0.90 --ssh-user ubuntu --result-file testkit/results/cpp-sentinel-tls-windows.json
python -B testkit/cpp/sentinel_smoke.py --tls --runtime linux-x64 --build gcc-shared-release --host 192.168.0.90 --ssh-user ubuntu --result-file testkit/results/cpp-sentinel-tls-linux.json
python -B sdk/csharp/tests/sentinel_test.py --tls --runtime win-x64 --vcpkg-root "D:\Program Files\vcpkg" --host 192.168.0.90 --ssh-user ubuntu --result-file testkit/results/csharp-sentinel-tls-windows.json
python -B sdk/csharp/tests/sentinel_test.py --tls --runtime linux-x64 --host 192.168.0.90 --ssh-user ubuntu --result-file testkit/results/csharp-sentinel-tls-linux.json
```

The harness first runs the SDK-specific Sentinel integration tests. It then
keeps one Go peer and one Rust peer alive across a minority stale Sentinel,
forced acknowledged-write loss, primary promotion, same-UUID full-state
republish, `SCRIPT FLUSH`, all-Sentinel loss, primary loss while resolution is
unavailable, Sentinel restart, a second promotion, and cross-language Selector
convergence. It verifies synchronization generations and removes only its own
containers and remote temporary directory in `finally`. `--keep-topology`
retains those exact resources only for diagnosis. `--result-file` writes the
final machine-readable scenario summary only after every assertion succeeds.
On a Windows host, the Go/Rust harness defaults to `win-x64`; selecting
`linux-x64` runs the client builds and processes through WSL and therefore
requires Linux Go and Rust toolchains in that user's login path. C++ and C#
runtime selection is explicit in the examples. The Windows C# path requires an
existing vcpkg checkout containing `openssl:x64-windows`; the harness does not
install it.
With `--tls`, the shared fixture generates a short-lived private CA and a leaf
whose SAN contains only `verdandi.test`; Redis, Sentinel and replication ports
all use TLS. The SDKs must therefore use the configured fixed identity rather
than any IP announced by Sentinel. Go/Rust and C++ also run a deliberate wrong-
identity rejection before the accepted path. Certificate generation requires
the pinned `cryptography` dependency in `testkit/sentinel/requirements.txt`.
The clean-repeat Rust transport-boundary result is
`testkit/results/rust-transport-refactor-sentinel-20260828.json`.
The C++ harness is intentionally a short root/Registration/Selector/Catalog/
checkpoint startup and integration smoke. It uses the same isolated three-node
and three-Sentinel shape but does not claim the two-promotion fault matrix.

## Isolation and interpretation

- `SCRIPT FLUSH` affects the selected Redis instance's script cache.
- `CONFIG RESETSTAT` affects command statistics used for hot-path assertions.
- Load tests disable persistence and use exact Redis command statistics only on
  a dedicated fixture.
- Rust load must use `--release`; debug timings are smoke evidence only.
- The standalone long harness gives the Go package enough timeout for both
  consecutive sustained phases rather than relying on Go's default 10 minutes.
- Standalone and the provided Sentinel harness are separate qualifications.
  `--tls` qualifies server-authenticated private-CA TLS only; it does not
  qualify mutual TLS, managed Redis services, or platform package delivery.
- Windows timings are smoke evidence. Optimization decisions should use a
  consistent Linux client/runtime and a characterized Redis host.
