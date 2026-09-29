# Three-Star Soak Acceptance

This procedure verifies resident stability, not peak-QPS ranking. It requires current authorization, consumes existing artifacts, and neither builds/downloads nor changes deployment configuration. Results belong in [validation.md](validation.md).

## SDK and acceptance boundaries

The probe uses the [current SDK](../comet/cpp/README.md): bounded workers concurrently call synchronous Catalog/Beacon updates and validate actual receipts; Beacon switching preserves logical UUID. See [validation](validation.md) for bounded three-node results. Stopped infinite soak has not restarted; old results identify old source, and bounded smoke does not establish hours of stability.

All three Stars accept local writes/cross-subscriptions. Catalog explicit updates carry TTL and expire during silence; Beacon beat renews during silence and Update atomically extends TTL. Legacy immutable Observer.select remains for full consistency verification; one/Item local estimates have targeted cases and are not authoritative Star data.

Source cases cover no automatic retry of initial registration failure, no replay of unknown updates, late-sample isolation, Reader floors, and stop/destroy. Execution still requires permission; prior evidence retains original semantics/source identity.

## Entry points and resources

```bash
python3 -B tests/soak.py --binaries build/cmake/release \
  --output build/results/soak-current \
  --fault-seconds 7200 --steady-seconds 43200 --interval 60 --records 16
```

Output must not exist, preserving evidence. Finite requested duration is at most seven days. Complete at least three fault rounds, one per Star; finish an ongoing recovery after the duration, so initialization, recovery, TTL cleanup, and finalization add wall time. --records is Publishers/Beacons per Star, each 1..16; defaults are 48 records per domain, each Catalog body/Ephemeris Data 2 KiB.

Use indefinite mode only when explicitly requested. --until-stopped conflicts with --steady-seconds; large seconds do not substitute. --fault-seconds 0 still completes A/B/C recovery, then retains the same cluster/SDK objects without an overall end time:

```bash
python3 -B tests/soak.py --binaries build/cmake/release \
  --output build/results/soak-current \
  --fault-seconds 0 --interval 0 --records 16 --until-stopped
```

RPC, convergence, and recovery deadlines remain. The resident native probe must emit increasing rounds after full target verification; 180 seconds without a new round fails. Repeated progress cannot refresh the watchdog. Unexpected exit, even zero, cannot complete indefinite mode. Resource/log budgets remain; preserve failures, investigate, and use new directories after fixes. Never combine separate durations into continuous success.

Use one Pulsar, Polaris, Astrolabe, and three Stars. Public links use TLS/APIKEY login; SQLite lives only in fixture temporaries. Comet uses public APIs. Fixed-entry Clients write concurrently; fixed-entry Subscriber/Observers verify all sources without failing over to source nodes to disguise replication.

Ordinary regression never starts soak. Legacy build.sh soak/scale/service-wrapper options remain rejected and cannot be mixed here. Do not overlap benchmarks/another large build. Preflight actual Linux memory, not VM maximums.

## Fixture stages and assertions

1. **Control plane:** Login/redaction, Almanac Set/Delete/empty values, readable cache while Polaris exits, same-database authority recovery, real Astrolabe metrics.
2. **Three-source writing:** Bounded concurrent synchronous Catalog/Beacon updates, verified completion/receipts. At all targets check each Key/UUID's source instance, version, complete content, and fixed Attr. Missing/wrong content fails; equal counts are insufficient.
3. **Silent expiry/renewal:** After convergence wait beyond the initial three-second TTL. Catalog must expire; Beacon stays alive without changing business-projection version. Later rounds change content, so renewal cannot hide stalled data.
4. **Rotating faults:** Force-kill A/B/C in turn. Switching must acquire a new Star instance while preserving the logical UUID. Old-source registrations expire without overwriting new ownership. Restart on original ports, recover Almanac/dynamic sources, then update, empty, close, and verify deletion everywhere.
5. **Resident stage:** Keep the same SDK objects, Scope/Keys, and cluster for writes/full checks. Do not repeatedly rebuild processes to clear leaks. Increase version ranges across rounds because Catalog expiry retains watermarks.
6. **Cleanup:** Normal/error/SIGINT/SIGTERM paths close owned groups/temporaries. Normal finish records completed; manual stop interrupted, never passed. Force-killed nodes cannot claim exit-time LeakSanitizer verification.

## Evidence and stop conditions

events.jsonl records phases, binary SHA-256, monotonic samples, and actual UTC. Every five seconds capture live PID RSS/peak, threads, FDs, and available memory/Swap. Compare by PID; restart-related drops do not disprove leaks. Retain final 8 KiB service logs and scan all new bytes for sanitizer diagnostics while running.

Unexpected service exits, failed confirmations, nonconvergence, cleanup errors, and sanitizer reports fail. Stop/fail below 128 MiB available memory or on exhausted log budget, rather than risk OOM to reach time targets. Logs cap at 128 MiB/file and 512 MiB total; these bound fixture evidence, not product behavior.

Send SIGINT/SIGTERM to the owned Python controller and await cleanup. No global pkill star or deleting all build/. After authorized completion inspect resident RSS/FD/thread trends, recovery counts/times, and target checks. One peak proves neither a leak nor hours-long stability.

## Coverage limits

This is real business-path preparation, not exhaustive faults. Dedicated Publisher/Beacon objects verify switching during rotation; multi-record concurrency occurs in the prefault stable window/final resident stage, not necessarily full-load throughout failures. Partitions, latency/loss, host sleep, clock steps, disk-full/power-loss, multiple machines, and many slow consumers need separate scenarios. Component/RPC injection cannot replace that evidence. cpp_pulsar_process separately verifies Pulsar-offline time continuity/recovery; soak changes no NTP/network rules.

ASan/UBSan and TSan require corresponding prebuilt directories/explicit environments, separately from ordinary builds. Sanitizer timings are not production throughput. Record actual parameters/results for finite and until-stopped modes separately, without inheriting an old fixed two-hour-plus-twelve-hour duration.
