# Polaris

[English](README.md) | [简体中文](README_CN.md)

Polaris is the sole Almanac publishing authority, implemented in Go with GORM and SQLite. It stores complete current data, authoritative versions, and bounded incremental history. Deployment guarantees one instance in the first version; there is no election, automatic standby, or multi-writer mode. See [implementation status](../docs/architecture.md#status) and [executed validation](../docs/validation.md).

## Call boundaries

Astrolabe is the only management write entry. After durable commit, Polaris distributes data to all Stars. Stars keep memory replicas and serve Comet Readers; they do not replicate Almanac peer-to-peer. Catalog/Ephemeris are not stored in Polaris; their publishers/source Stars maintain dynamic state under their own rules.

Polaris obtains infrastructure identity from Pulsar. Stars discover it through Pulsar membership and initiate internal synchronization streams. Polaris uses registered Stars to identify expected targets/report missing connections, not to dial them. Membership does not prove liveness; neither Polaris nor Astrolabe joins the Star peer network or replica count. Internal credentials belong to Almanac, so initial recovery cannot depend on a Comet session that does not yet exist.

<a id="deployment"></a>

## Fixed deployment and restart

The initial deployment fixes Polaris's Pulsar account name, Galaxy, and canonical internal registration endpoint. Normal restart supports the same authority database and deployment identity only. These fields determine principal. A restart still submits a new startup request and receives new Member.id/epoch; it neither pins process identity nor resets Almanac versions/commit evidence.

Explicit initialization stores the deployment binding in database metadata. Normal startup verifies configuration against it before registering with Pulsar. Missing/mismatched binding rejects startup, without rewriting it or first creating another durable member. Endpoints are canonicalized before comparison. This local check introduces no wire field or second identity generation.

Account name and password are distinct: password rotation/TLS renewal preserving binding and trust requirements are not deployment migration. Changing account name, Galaxy, or endpoint is unsupported; no retirement, endpoint transfer, aliases, or automatic takeover are added. A stopped process does not release Pulsar's deployment slot. Multiple Polaris principals created by another database/configuration remain a single-authority deployment conflict, never resolved by reachability, clearing membership, deleting startup evidence, or creating an empty authority database. Deployment remains responsible for singleton execution; local metadata cannot stop another independent deployment starting outside that boundary.

## Durable commit

Each Sector/Spectrum is an authoritative Almanac group with an independent durable version; Star need not represent it with the generic Store class. Management supports single-key Set/Delete and atomic multi-key Set/Delete within one Scope. Set supplies a complete Buffer. One durable transaction checks version conditions and commits data, version, and required delta evidence together. A new valid commit advances authority once, independently of Star cursors or physical time.

`CommitRequest.change` retains single-key semantics and is mutually exclusive with `changes`. Batches must be nonempty with unique keys, without a batch-wide key-count or byte cap. Individual values remain limited to 1 MiB; configured Scope/global storage budgets and request deadlines still apply. Requests exceeding one frame use client-streaming `Authority.Batch`: every page has the same scope/version; commit occurs only after a `complete=true` last page and normal EOF. Missing completion, cancellation, duplicate keys across pages, position changes, or trailing data after completion never commit a prefix. All Astrolabe multi-key writes use this stream. A batch consumes one strictly +1 authority version; records, global/Scope accounting, and complete retry evidence share one SQLite transaction. Set, empty values, and Delete may mix; any error rolls everything back. Internal `__` scopes retain only the original single-key entry, preserving credential-revocation boundaries.

The four-table physical schema is unchanged. A nonempty history key denotes a single-key commit; an empty key denotes an `AB02` binary batch: 8-byte count, then per item a 2-byte key length, 4-byte body length, 1-byte delete flag, and raw bytes, with big-endian integers. Current records always contain real separate keys; applications cannot write an empty key. A whole batch occupies one history version and is retained/evicted atomically. Retry evidence compares the complete ordered request, never inferring past success from present values. Old Polaris binaries cannot decode retained batch history; rollback compatibility needs separate handling. This change does not alter/migrate deployed databases.

Transactions fitting a synchronization frame use one Patch with `changes`. Larger ones use a frozen complete snapshot covering at least the pending suffix; pages do not advance installation versions, and only complete termination installs once. Transactions exceeding Feed/history retention budgets may still commit durably; downstream recovery rebuilds a complete baseline. Transport page capacity does not cap atomic transaction size. Star prepares all changes in a private COW root and installs content/version once; active Reader suffixes are also accepted atomically. Old receivers reject the new batch shape; do not mix old Stars with batch writes. `Commit`/`Batch` success proves Polaris durability only, not installation on every Star or a cross-domain Catalog transaction.

New groups start at version 0. New requests must use current +1, including same-value Set and Delete of an absent key. Empty values differ from Delete; neither single-key nor batch requests bypass checks with version 0. uint64 exhaustion must not wrap. Replica recovery installs authority versions directly rather than applying ordinary +1 semantics record by record to snapshots.

For the same Scope/version, retries confirm the original result only while evidence remains and key, operation, and complete bytes match. They do not recommit, compare Protobuf encoding order, infer from current state, or use another idempotency LRU. Trimmed evidence yields an explicit inability to confirm; different content at the same version is rejected without automatic version increments. Restart from the same authority database retains this evidence despite a new network identity.

A successful receipt means durable Polaris commit, not memory enqueue, gRPC write completion, or Star installation. Failure leaves no partial record/early version advance. A lost response, Astrolabe exit, or unreachable Star after commit never rolls back committed data. A dependency timeout alone cannot establish noncommit; confirmation needs valid original evidence. Deleting the last key retains group version. Restart restores data/versions without renumbering after Pulsar login. Read-side versions never overwrite the editing baseline, and management conflicts cannot be hidden by automatically increasing the request version.

<a id="gorm"></a>

## GORM access and transactions

The approved stack is GORM with official `gorm.io/driver/sqlite`, backed by `github.com/mattn/go-sqlite3` and CGO. Building requires a target-compatible C compiler; an arbitrary MSVC installation does not establish a working Go CGO toolchain. Cross-compilation likewise needs the matching compiler. Tool/CGO settings affect only the project build process, not global environment. See the [official driver](https://github.com/go-gorm/sqlite) and [go-sqlite3](https://github.com/mattn/go-sqlite3).

There is no generic Repository/multi-database layer, and GORM types do not escape into gRPC or Comet interfaces. ORM handles mapping and controlled queries/writes; Polaris explicitly owns version conditions and transactions. The first version remains one SQLite authority without database resolver, read/write routing, or durable background task tables.

Each valid management commit uses one explicit transaction for version read/check, key mutation, group version, and delta evidence. Success and synchronization notification wait for the outer COMMIT to succeed, not merely a transaction closure returning nil. Every step uses the same tx, never the root db. Per-SQL default transactions cannot replace this boundary. Do not nest business transactions or send network requests from model hooks. See [GORM transactions](https://gorm.io/docs/transactions.html).

Write transactions, including history trimming, are serialized with bounded waits. Durable conditions/unique constraints remain necessary; in-process serialization does not prove database version correctness. Reads, snapshot references, and pools share a total budget. One writer does not mean every reader must share its occupied connection. [SQLite constraints](#sqlite) govern connection initialization, durability, and checkpoints instead of ORM defaults.

- Conditional group updates check both Error and expected affected-row count. Never let Save implicitly insert after a failed conditional update and bypass CAS. New groups/keys have explicit branches and unique constraints. Same-value Set/missing Delete still advance the version even if content rows do not change.
- Complete payload updates explicitly select fields, including zero-byte values, rather than relying on Updates(struct)'s nonzero-field default. Empty BLOB, absent row, and historical Delete are distinct; a nil Go slice must not silently become Delete or SQL NULL. See [GORM updates](https://gorm.io/docs/update.html).
- Models contain only needed metadata, group versions, current Key/Value, and bounded history. Do not embed gorm.Model for unused auto IDs/timestamps/soft deletion. Almanac Delete removes the current row; history retains termination evidence.
- Sector/Spectrum/Key use separate columns and explicit composite keys with byte-exact matching, not wildcard paths. Missing Where must not widen scope. Bind SQL parameters, select needed columns, and avoid per-record association preload/N+1 queries.
- uint64 authority versions use fixed 8-byte big-endian BLOBs for storage/index comparison, never signed INTEGER, autoincrement IDs, or REAL. Go checks increment/exhaustion; reads/writes validate type/length. Scanner/Valuer support stays internal. See [SQLite types](https://www.sqlite.org/datatype3.html).

The project explicitly owns schema/format versions. Initialization creates a known schema; normal startup verifies metadata/tables/indexes. AutoMigrate must not repair unknown/corrupt databases or apply unreviewed production changes. The first implementation supports explicit formats; later migrations need separate boundaries, not a generic framework. [GORM migration facilities](https://gorm.io/docs/migration.html) do not authorize automatic startup changes.

Errors retain their real commit phase. Constraint conflicts, busy waits, exhaustion, and uncertain COMMIT are not all “uncommitted.” Uncertain durability pauses new writes for recovery/reconciliation, without increment-and-retry. Notification failure after durability affects synchronization only. SQL logs must not expose payloads, Credential SECRET, or other sensitive bound parameters; diagnostics retain error classes and bounded context.

<a id="sqlite"></a>

## SQLite operating constraints

Polaris uses `journal_mode=WAL` and `synchronous=FULL` on supported local filesystems, never network shares. WAL allows readers alongside one writer, not multiple authorities or standby. Pulsar's independent DELETE + EXTRA configuration is unchanged. See [SQLite WAL](https://www.sqlite.org/wal.html).

Startup sets and reads back the main database's actual journal mode, rejecting silent fallback/ignored PRAGMA results. Journal mode persists, but synchronous/busy settings must reach every new/recreated connection through controlled DSN/initialization, not just one opportunistically acquired pool connection. Extra DSN parameters cannot override durability. Writes remain closed if configuration cannot be enforced.

With FULL, successful COMMIT is the durability acknowledgment; no later checkpoint is required for the receipt. NORMAL process-crash recovery is not power-loss durability. An incomplete checkpoint does not revoke prior commits. I/O errors retain phase-specific reporting and stop unsafe new writes. Filesystem/device sync behavior must be correct; ordinary process termination is not a power-loss test. See [synchronous](https://www.sqlite.org/pragma.html#pragma_synchronous).

Initially use SQLite's bounded-page-threshold automatic PASSIVE checkpoint, without an independent thread/task hierarchy. Initialize the threshold consistently per connection; 1000 pages is an engineering starting value, not a WAL byte cap. Checkpoints may add latency to the triggering commit; move them off the write path only if measurements justify it. Do not force TRUNCATE after every commit or busy-spin on incomplete checkpoints.

Snapshot preparation obtains consistent data/version in a bounded read transaction, then closes cursors/transaction before sending immutable payloads. Reuse a shared baseline payload rather than cloning the authority per Star. Preparing, prepared, and in-flight bytes all count. SQL Rows and transactions must not span gRPC writes, ACK waits, or slow-client backpressure. Separate SELECTs cannot impersonate one consistent snapshot; data capacity must fit preparation/installation budgets.

Long reads prevent checkpoint completion/WAL reuse; journal_size_limit alone is no hard disk bound. Bound read duration, preparation concurrency, and occupancy. Cancellation/exhaustion releases quota only after actual cursor close/rollback. At configured WAL/disk pressure, stop admitting new writes/snapshot preparation, let bounded reads finish, and attempt checkpoints instead of indefinitely appending while reporting busy. Distinguish uncheckpointed data from retained reusable file size. Resume after pressure clears; maintenance must continue while writes stop. Forced checkpoints, if needed, run boundedly in controlled maintenance windows. Never delete WAL or revoke successful commits.

Pinned dependencies are GORM v1.31.2, gorm.io/driver/sqlite v1.6.0, and go-sqlite3 v1.14.52. The cached `sqlite3-binding.h` identifies embedded SQLite 3.53.4. See [go.mod](../go.mod) and [approved/cache status](../docs/architecture.md#approved-cached-polaris-dependencies). No silent system-libsqlite3 fallback. Cached source/version inspection is not runtime validation.

## Bounded history

Only complete current data and bounded incremental history are retained, not all historical content forever. History supports short Star catch-up and original-commit confirmation while evidence remains; it is not an audit database and exposes no arbitrary historical query/rollback API.

History committed with the baseline is the recovery source: no extra Outbox, durable task queue, or per-Star replicated log. Notifications coalesce each affected Scope's latest target version; synchronizers still read intermediate +1 commits from continuous durable history. Coalescing notifications must not drop commits. Rechecking committed Scope state/installed versions detects missed notifications without repeating management transactions.

History has count/byte budgets and optionally a retention-time limit; initial values belong in implementation configuration, not protocol guarantees. Trimming removes old deltas only, preserving current data and empty-group versions. Valid commits and transaction preparation also need resource bounds; accepting unbounded content before relying on eviction is insufficient.

When a Star position falls outside continuous history, send a complete Scope snapshot and authority version. Never advance installation on incomplete deltas or invent a replacement version sequence. Offline Stars cannot prevent trimming or acquire unlimited private queues. Acquired send data has a stable lifetime during trimming; retrieval/trimming share continuity checks. If the full required interval is unavailable, restart with a snapshot. Concurrent post-snapshot commits need an explicit continuation position; staging exhaustion ends that recovery with backoff rather than extending history without bound.

## Synchronization to Star

Each Star connects to the unique Polaris discovered through Pulsar and uses one internal bidirectional stream for all Almanac groups and installed positions. Polaris pushes continuously, without reverse dialing or per-group connections. [Protocol](../proto/README.md#polaris-stream) alone defines discovery conflicts, identity, reconnect, and stream budgets; Stars need no second configured Polaris address.

New internal Patch messages preserve strict per-Scope version +1, including same-value Set/missing Delete. Consecutive patches may share a transport batch, but key coalescing cannot omit intermediate authority commits. Downstream Star Watch may still coalesce final state over covered ranges. Gaps recover continuous history, then full Scope snapshots if unavailable; replay at/below installed positions is not recommit.

Initial snapshots fix declared scope, complete data, and authority version, including legitimate empty scopes and internal credentials. Pagination is transport only; reading a changing current table page by page is not a snapshot. ACK requires complete installation. Failed partial snapshots retain the old complete state or remain unready, without claiming catch-up. [Credential snapshot semantics](../proto/README.md#credential-snapshot) own session effects, without new per-account identity markers in Polaris.

Bootstrap fixes an initial Scope inventory and minimum versions in one bounded consistent read. Each Scope then independently prepares a complete baseline at or above that minimum, with content/version from one transaction; payloads need not share one global database instant. Release the inventory transaction immediately, rather than retaining a global read across networking or cloning the whole database. Scopes independently complete and continue +1 updates. Bootstrap completes only when all initial entries satisfy requirements and the final marker arrives, including an explicitly terminated empty inventory.

New Scopes/later commits enter bounded subsequent synchronization without endlessly expanding the initial inventory. Inventory/discovery handoff must not lose new Scopes; committed-state rechecks/version reconciliation close the gap. Bootstrap promises no cross-Scope transaction or global simultaneous view. See [startup ordering](../docs/architecture.md#startup-and-operation) for public-service readiness.

Startup validates/recovers the authority database before serving baselines. A listening endpoint or Pulsar registration does not prove recovery; until ready, return unavailable, never a temporary empty database. Polaris serves connected targets without waiting for all Stars; Star bootstrap does not wait for Astrolabe.

Scheduling uses [batched sending/cumulative ACK](../proto/README.md#stream-batching); durable commits do not await page round trips. Concurrent recoveries may share an already prepared immutable Scope/version snapshot, without retaining another whole-database memory cache or prefreezing every Scope. Actual references govern reclamation; slow targets cannot prevent others progressing.

Restart retains neither old memory send queues nor installed-position tables. Reconnecting Stars report actually retained complete versions; the recovered authority determines continuation, independent of new login identity. Lost streams/ACKs do not roll back installed data. Missing/lower authority versions relative to a Star are errors, not empty-state replacement. Registered but disconnected Stars remain unknown/stale, never falsely synchronized.

Network calls hold no write transaction/business-state lock. Snapshot lifetime, send caches, history references, and database read snapshots all remain bounded; long database snapshots cannot bypass memory budgets while causing unlimited disk growth. Pagination/storage implementation must preserve these boundaries without cloning all history per Star.

<a id="assessment"></a>

## Installed-version reconciliation

Alongside pushes, reconcile connected Stars' installed Scope versions approximately every 30 seconds with jitter by default. The configurable period is not a protocol requirement. Reuse the stream/per-target recovery state and bounded Scope/version metadata: no per-Scope timer, separate reconciliation RPC, or full-content polling. One discrepancy drives the existing recovery task, never duplicate rebuilds.

Compare durable authority versions with complete installed versions, not sends, partial pages, or enqueues. Empty groups retain versions. Repair only lagging Scopes; an ahead-of-authority Star stops erroneous overwrite and triggers diagnosis. Reconciliation catches occasional missed notifications. Slow Stars block neither peers nor management commits; matching numbers alone do not prove matching content.

## Files and recovery boundaries

Configure the database path explicitly, separately from Pulsar membership storage. Never fall back to temporary/in-memory storage or delete/recreate corrupt, conflicting, or unsupported databases. Initial creation differs from recovery. Consistent backup/migration preserves current data and empty Scope versions.

Service-level exclusivity guarantees one authority process; SQLite file locks/multiple connections do not establish the Polaris singleton. Preserve crash-left WAL and complete recovery/validation before opening service; do not remove `-wal`/`-shm` or treat not-yet-checkpointed commits as lost. Online backup uses SQLite's consistent backup mechanism. Copying only the main file, or independently copying changing main/WAL files, is invalid. This does not add a backup-management RPC; delivery defines operational steps.

Restoring an old backup is not a normal restart. Lower versions cannot overwrite newer Stars or prove cluster catch-up. No automatic rollback is supported; inconsistencies stop affected overwrites and are reported. No cluster-wide rollback protocol exists. Version comparison detects lag/ahead positions but not equal-version divergence after unauthorized database rollback/new writes. The first version relies on one authority and no silent rollback; it does not claim database-fork detection or backup-lineage verification.

Polaris stores neither Astrolabe management accounts/sessions nor Catalog/Ephemeris. The official GORM CGO driver and WAL + FULL are implemented. [go.mod](../go.mod) pins dependencies; [schema.go](internal/storage/schema.go) validates format. New downloads require specific approval. GORM does not spread into database-free Astrolabe or SQLite-C-API Pulsar.

## Implementation and validation

Storage, stream, and real-process cases cover commits, history trimming, snapshots/deltas, installation ACK, and restart. Case mappings and additional fault/scale scenarios belong in the [acceptance plan](../docs/comet.md); actual evidence belongs only in [validation.md](../docs/validation.md). Ordinary regression does not establish power-loss durability, large-scale behavior, or every interleaving.
