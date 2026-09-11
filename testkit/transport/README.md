# Star/Planet transport probe

This is an isolated experiment, not a second production transport backend.
Production now uses gRPC v4. The following v3 matrix remains historical; current source uses bearer admission. A shared `peer/common` control-frame
flush bug exposed by the experiment is fixed; the v3 experiment did not change Go Supervisor. SDKs are outside
the experiment. See [review](../../peer/grpc-migration-review.md) and the
[complete v3 report](../../peer/grpc-benchmark-results.md).

The 2026-09-10 final matrix contains 660 trials: 650 completed and 10 failed.
Windows completed 216/220, Ubuntu 220/220, and Ubuntu-publisher/Windows-receiver
214/220. Failures remain in the report; successful throughput does not override
them. The evidence does not establish a migration with no performance loss.

## Primary workload: normal push scheduling

The default workload is now `push`, following the maintainer's clarification
that most Star/Planet traffic is streaming. Echo is only a supplementary
transport calibration and is not the migration decision workload.

- A single shared publisher updates a `HashMap` with real string keys and
  broadcasts each update to 1 / 4 / 16 receivers. Registry cardinality is
  100 / 1,000 / 10,000 / 100,000 active records, each with an independent
  128 B body. Another 1,000 keys hold Catalog state. Every source update
  allocates new content; fanout shares immutable `Bytes` during dispatch.
- The same deterministic initial state is preloaded on both sides **before
  Join**. These are warm incremental-cache measurements, not network full
  synchronization or cold restoration tests. Registry count is entry count,
  not connection count. Hashing the final complete table is outside throughput.
- Mixed traffic is 25% Registry (128 B) and 75% Catalog (256 B). Four 16 KiB synthetic
  snapshot fragments are interleaved every 500 ms. These fragments exercise
  dispatch and framing, not a complete snapshot restoration algorithm.
- Source rates are 2,000/s for normal traffic and 8,000/s for bursts. Dispatch
  ticks are 1 ms and 50 ms respectively, subject to OS timer resolution.
  This models asynchronous update
  batches rather than waiting for every receiver's response.
- Each receiver sends only 20 diagnostic control probes/s and 5 small
  publications/s. There is **no per-update ACK**. Publications stop before the
  final completion watermark so every submitted publication must be confirmed.
- A slow-receiver case pauses one receiver for 200 ms while its independent
  control producer continues. Control RTT therefore exposes the delay instead
  of hiding it by pausing the generator too.
- Both transports use the same dispatcher, control priority, bounded queues
  and shared 512-message broadcast ring. A receiver that exceeds the ring
  fails explicitly and requires resynchronization; it cannot silently skip
  revisions. Successful runs verify every revision and equal final cache hashes.
- Dedicated Registry value updates use 8,000 updates/s across four table sizes.
  They replace existing values, not mass key creation/deletion or rehashing.
  Dedicated Catalog runs exclude snapshot fragments and sweep 64 / 256 / 1,024 B
  at 2,000 / 8,000 / 20,000 / 60,000 source updates/s, with four recipients.
  The offered delivery rate is therefore four times the source rate. Both
  completed and failed trials remain in raw JSON; passing one offered rate
  does not establish the maximum capacity.

`p50_ms` / `p95_ms` / `p99_ms` measure control enqueue-to-response RTT, including
transport queues and receiver consumption. `scheduled_control_p99_ms` also
includes the generator's timer delay. `update_p50_ms` / `update_p95_ms` /
`update_p99_ms` are the **worst recipient percentile**, with individual
recipient percentiles also retained. Each recipient reports cumulative
applied progress every 128 updates, about 0.8% reverse messages. The publisher
uses its own clock from the update's **scheduled** publication to receiving
that application confirmation. This includes source scheduling lateness,
dispatch, encoding, forward transport, receiver application and the return
path; it is an upper bound on application delivery latency, not pure one-way
latency. The last cumulative watermark is reliably exchanged before completion.
Skipped progress and expired timestamps are explicit counters, not zero-latency
samples. Low-rate two-second trials have only about 31 samples per recipient,
so their P99 is close to a sampled maximum and is not a high-confidence SLA.

`dispatch_queue_p99_ms` is measured on
the server clock from publication to the transport queue handoff. None is an
unsynchronized cross-host one-way timestamp subtraction. Client CPU includes
cache construction, connection setup and final hashing; local server CPU is
the delta after readiness, excluding its initial cache construction. Remote
server CPU includes setup. RSS is sampled every 20 ms. Windows process CPU
accounting is coarse, so short trials may report zero CPU ticks. Pure wire bytes and allocator
counts are not measured.

The publisher is a transport fixture, not the production Star ownership,
durability, Supervisor CAS, candidate failover or snapshot recovery state
machine. Star and Planet roles use signed test admissions and the current
production signed-bearer verifier. SDKs remain outside scope.

## Supplementary echo workload

- The same generated `Transfer { sequence, body }` over either the existing
  six-byte framing layout or one long-lived gRPC bidirectional stream.
- TLS 1.3, server certificate verification, signed bearer fixture admission and the
  current production signed-bearer verifier on both paths. The public
  test signer does not replace a live Supervisor or test its CAS/candidates.
- Star or Planet client role, upstream Star role, reusable immutable payload,
  two Tokio workers per process, bounded queues and no compression.
- 256 B / 1 KiB / 16 KiB application bodies; 1 / 4 / 16 / 64 concurrent
  sessions; window 1 or 64; saturated and fixed aggregate 4,000 messages/s.
- Window 1 is serialized request/response **inside the stream**, not unary
  gRPC. Fanout counts simultaneous echo sessions, not an implemented Star mesh
  or a store broadcasting one authoritative update to many nodes.

Both paths encode every outgoing Protobuf message. Push TCP reuses its encoding
buffer and combines up to 16 already-ready frames into one `write_all` followed
by `flush`. It never waits to fill a batch; the last frame is flushed even if
no further message arrives. Production control frames and supplementary echo
flush immediately. Tonic can coalesce HTTP/2 output. Window
64 is a common in-flight budget, not a forced identical batching algorithm.
Thus results compare these two concrete implementations, not the theoretical
upper limit of TCP and gRPC. Further batching/window tuning could change the
result. Earlier per-frame-flush and no-flush trials are retained
as pre-experiments only and excluded from the final v3 comparison. No-flush
reproduced idle completion timeouts: tokio-rustls may retain ciphertext after
`write_all`, as documented in its [flush contract](https://docs.rs/tokio-rustls/0.26.4/tokio_rustls/).

Latency uses the client monotonic clock. Fixed-rate latency starts at the
scheduled send time, includes generator delays and queueing, and drains every
planned request or fails with a timeout. It must not be read as pure network
RTT. Histogram buckets are 100 microseconds. Saturated tests are reported
separately. Setup and 250 ms warmup are excluded from steady throughput;
sampled process CPU/RSS include setup and warmup. Allocator counts, wire bytes,
pure one-way push, durability, replay recovery, and topology failover are not
measured by this probe.

## Offline commands

From the repository root, using the already installed project Python:

```powershell
& build/tools/python-build/Scripts/python.exe -B -m testkit.transport.run --build --workload push --rounds 5 --seconds 3 --output build/transport/windows.json
```

```bash
build/tools/python-build/bin/python -B -m testkit.transport.run --build --workload push --rounds 5 --seconds 3 --output build/transport/linux.json
```

`--build` invokes Cargo release with `--frozen`, offline mode and one build
job. It cannot download a dependency. `--quick --rounds 1 --seconds 1` checks
the harness only and is not a performance conclusion. Caches and target output
stay in `build/deps/cargo` and `build/transport/target`. No global environment
configuration is changed. Add `--workload echo` to reproduce the supplementary
echo matrix rather than the default push matrix.

For direct Cargo checks, use `sdk/run-tool.ps1` or an equivalent child-process
environment with `CARGO_HOME=<root>/build/deps/cargo`,
`CARGO_TARGET_DIR=<root>/build/transport/target`, and `CARGO_NET_OFFLINE=true`:

```text
cargo test --manifest-path testkit/transport/Cargo.toml --frozen --jobs 1
cargo clippy --manifest-path testkit/transport/Cargo.toml --workspace --all-targets --frozen --jobs 1 -- -D warnings
cargo fmt --manifest-path testkit/transport/Cargo.toml --all -- --check
```

Code generation is explicit, using the existing protoc 36.1 executable:

```text
cargo run --manifest-path testkit/transport/Cargo.toml --frozen -p verdandi-transport-probe-generator -- <absolute-protoc-path>
rustfmt --edition 2024 testkit/transport/src/generated/verdandi.probe.rs
```

Generated RPC code lives under `src/generated` and is ordinary source input.
Normal builds do not run protoc or edit it. The separate generator package
keeps build-time generator dependencies out of the probe binary.

## Cross-host sessions and cleanup

After source/cache synchronization and the Linux release build, run on Windows:

```powershell
& build/tools/python-build/Scripts/python.exe -B -m testkit.transport.mixed --config build/testkit/services-remote.json --output build/transport/mixed.json
```

This uses the existing SSH credentials without printing them. The remote
Python owner starts only its own Rust test server. Explicit stop, SSH EOF,
owner timeout, and the server's own lifetime limit bound cleanup. Local tests
use Windows Jobs or POSIX process groups and verify listener release. Raw
sample JSON is saved incrementally. The available-memory guard stops a test
below 256 MiB; do not run benchmarks alongside builds or other benchmarks.
The final v3 cross-host failures preserve client errors and remote cleanup/resource
results, but not the removed remote server log. Their root causes are unresolved;
the generic resynchronization status alone does not prove broadcast-ring overflow.

Fourteen Rust prototype tests exercise both transports: exact data and half-close, old TLS
proof replay, missing/oversized proofs, body/codec limits, cancellation/slow
readers, stopping active streams, truncated TCP frames, unauthenticated-session
deadline, rejecting a Planet impersonating the upstream Star, shared-publisher
cache convergence, explicit ring-overflow failure, and small Catalog push
with 100,000 Registry entries and an observable 200 ms receiver pause. Batch
tests verify idle-tail flushing, message boundaries and the 16-frame bound;
the isolated TCP writer retains idle flushing regressions; production gRPC owns its transport flushing.

Use `--suite standard|cardinality|catalog` to select a subset, or the default
`all`. `--rates 2000,8000,20000,60000` configures the increasing Catalog load
ramp; values above 200,000/s are rejected. These are experiment parameters,
not supported production limits. The VM may expand dynamic memory; the guard
uses current available memory rather than treating initial RAM as a fixed cap.
