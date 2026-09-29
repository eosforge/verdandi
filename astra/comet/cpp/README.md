# Native Comet C++ SDK

[English](README.md) | [简体中文](README_CN.md)

This page describes current source interfaces. Synchronous atomic Publisher updates, synchronous Beacon registration/update with beat/tick, same-logical-ID recovery, Reader/Subscriber callbacks, and Observer local selection are wired. Star, internal replication, and generated C++/Go protocols were updated together for Beacon recovery.

[Validation](../../docs/validation.md) binds exact builds/tests to source identity; older regressions do not qualify the current workspace for release. Raw-byte interfaces are defined by [Client](include/comet/client.hpp), [Beacon](include/comet/beacon.hpp), [Observer](include/comet/observer.hpp), [Subscriber](include/comet/subscriber.hpp), and [Reader](include/comet/reader.hpp).

| Interface | Current behavior |
| --- | --- |
| Client.beacon | Requires beat; returns a handle only after synchronous initial registration acknowledgment |
| Beacon.update | Synchronous Result, no resend after failure, caches only confirmed Data |
| Beacon identity/renewal | Same logical ID on recovery, registration generation separate from Data version, Update atomically renews |
| Readers | Reader/Subscriber watch/state/changed/stop; Observer.one/stop with generation/CAS-protected local estimates |

Catalog requires no application-supplied version:

```cpp
auto publisher = client.publisher({"routing", "main"});
if (!publisher) {
    return std::unexpected(publisher.error());
}
auto result = publisher->update("service-a", {1, 2, 3}, std::chrono::seconds(30));
// Result<Publisher::Receipt>; the batch overload accepts vector<Publisher::Entry>.
```

Overlapping updates on one Publisher return busy, without queue/coalescing. Synchronous RPC uses the caller thread; the shared Core loop manages authentication/switching. Client/Publisher close cancels it directly through standard stop tokens, without waiting for watch/changed callbacks. stop_callback unregisters before RPC context destruction; an old control-loop snapshot cancels only its captured call. Synchronous update inside SDK notification callbacks returns busy to avoid blocking the shared loop; explicit close takes precedence as closed.

| Topic | Authoritative definition |
| --- | --- |
| RPC, fields, versions, server TTL, Session, Watch | [Protocol](../../proto/README.md#comet) |
| Native domains, commits, snapshots | [Storage](../../common/README.md) |
| Star startup/listeners/authentication | [Build and runtime](../../docs/build.md#deployment-and-wiring) |
| Management/metrics | [Astrolabe](../../astrolabe/README.md) |
| Cases/evidence | [Acceptance](../../docs/comet.md) / [Results](../../docs/validation.md) |

## Scope and layout

Public headers are in include/comet, private implementation in src, tests in tests, namespace comet. gRPC/Protobuf types remain in adapters, never application signatures. Initial delivery is a static library, without shared-library, C ABI, or other-language bindings.

Minimum SDK language is C++23 under [coding rules](../../docs/coding.md#cpp); servers remain C++26. Windows/MSVC 19.51 x64 Release passed SDK compile/link, five local tests, source integration, and installed-package consumption; local CMake maps C++23 to `/std:c++latest`. Linux/GCC 16.2 C++23 SDK passed Debug, Release, probes, ASan/UBSan, TSan, and independent package validation as recorded. Sanitizer builds propagate compile/link requirements to avoid mismatched Protobuf instrumented layouts. Reuse pinned gRPC/Protobuf/BoringSSL without standalone OpenSSL or implicit downloads. SDK does not own Star replication, retention, authority arbitration, or time synchronization.

## Public responsibilities

Existing load/watch/select snapshots and close/wait remain available alongside these interfaces:

| Object | Responsibility |
| --- | --- |
| Client | Shared connections, active Star, TLS/Session, scheduling, budgets, cancellation |
| Beacon | Synchronous initial registration/Data updates, optional tick, mandatory beat, confirmed-data cache, same-ID recovery |
| Observer | Ephemeris only; one(selector) from the locally synchronized pool, stop() |
| Publisher | Catalog Scope binding; synchronous atomic single/multi-key updates, internal versions, per-call TTL |
| Subscriber | Catalog whole-Scope/exact-Key watch/state/changed/stop |
| Reader | Read-only Almanac, Subscriber-shaped API with authority-version floor |

Children own shared Client core. Releasing the last public Client handle does not close surviving children; explicit Client::close() closes all roles. Beacon::destroy() and reader stop() affect only themselves. Last business-handle release starts nonblocking cleanup, never waits for networking in a destructor, and internal references cannot renew an abandoned object forever.

### Private implementation consolidation

Client Core owns active endpoint, Channel/Stub, Session, shutdown, budgets, and timers. Publisher owns version metadata/current synchronous call; Beacon owns identity, confirmed Data, and finite attempts. All reader types share a private Watch core. Put state with its actual owner, not another Manager/Service/Executor per phase or duplicated session/backoff configuration per object.

Core separates shared Session/Watch Channels from unary Channels, not one connection pool per object/Watch. This isolates long-stream concurrency slots without promising exactly two TCP connections. Renewal batching/multitarget subscription proposals follow [protocol assessment](../../proto/README.md#comet-batching) and performance matrices, not object-count-based connection/gain assumptions.

Coalesce identical wakeups and recheck current operations/deadlines at scheduling time. Cancelled-but-incomplete RPCs return resources at actual completion. Shared abstractions cover networking/timing/ownership/completion, not a forced common state machine for Publisher versions and Beacon orders.

Core completion events enter a separate deduplicated weak ready queue. Ordinary network wakeups process K ready objects rather than N directory entries. Admission reserves queue space; noexcept wakeup allocates nothing and does not extend lifetime. Directory scans remain for deadlines, shared identity/close, and capacity return. Renewal/timeouts retain individual deadlines, not one-second sweeps. Targeted advancement retains a conservative earliest deadline; stale values cause at most an extra early maintenance pass, never delay work. Events arriving during processing may requeue; at most eight rounds run before yielding. No extra executor, per-object threads, or public queued state. Ordinary builds/regressions passed; [performance evidence](../../docs/validation.md) does not yet measure thousands of idle Watches or universal gains.

View::each pins its root throughout traversal even if a callback reassigns the original View, preserving current Key/Value and remaining entries. Callback parameters remain borrowed for that callback/root lifetime; retaining bodies across callbacks requires Value ownership. Cancellation/cache/traversal fixes are qualified only by recorded source-bound validation.

### Synchronous writes and explicit outcomes

Client/role wait(timeout) waits for local cleanup only. Nonpositive values immediately check completion; huge positive values saturate at the furthest representable steady_clock deadline without signed conversion/addition overflow. wait never implicitly closes, and blocking waits remain forbidden inside SDK notifications.

`client.beacon(...)`, `beacon.update(data)`, and `publisher.update(batch, ttl)` are synchronous. Factories/operations return `std::expected<T, Error>`; Beacon factory is `std::expected<Beacon, Error>`, not optional. Public writes return no future or second “accepted now, commit later” success state.

Success confirms this operation committed at the attached Star, not all replicas or durable application storage. Definite noncommit, definite rejection, and sent-but-uncertain outcomes are distinct. Timeout/disconnect/cancel may follow remote commit; late success cannot rewrite an already returned result.

One deadline covers preparation, authentication, version query, permitted conflict repair, and RPC. Substeps consume remaining budget without resets. Failure return ends resending, desired-Data caching, and merging/overwriting another call's result. The original RPC may still finish remotely; buffers/quota remain until actual completion. Each call captures target instance/lifecycle. Switching Client cannot redirect that failed request to another Star; later calls may use the new target. Beacon registration recovery is a separate lifecycle operation, not failed-update replay. Catalog has only the bounded repair described below.

Write results do not wait for watch/changed callbacks. Expected business errors are values without gRPC/Protobuf leakage. expected does not make allocation/program errors recoverable or every API noexcept.

### Raw payloads and application codecs

Applications define Data, Attr, Catalog/Almanac formats and integrity. Encode to Value before calls and explicitly decode in read callbacks. SDK fixes no JSON format, codec registry, or C++26-reflection serialization framework. Codec failures remain application-adapter errors, not nullopt deletions. tick returns Data directly, not mandatory Result/Optional; valid empty Data is a complete update, not “skip.” Encoding failure sends nothing; decoding failure is explicit, never deletion/empty/network error. Star does not interpret ordinary Buffer fields.

### Standard byte containers and ownership

No Buffer wrapper is added. Use owned `std::vector<std::uint8_t>`, borrowed `std::span<const std::uint8_t>`, and shared immutable `std::shared_ptr<const std::vector<std::uint8_t>>` as appropriate. Vectors pass by value; spans become owned before outliving the call. Shared inputs reject nullptr but accept empty vectors. Do not mechanically supply every overload for every method.

Synchronous return does not prove gRPC released input. Requests that may outlive return, including cancellation/timeouts, must own buffers rather than temporary application borrows. After transfer/sharing, applications must not mutate in-flight data through writable aliases. Keys, scopes, and callback payloads follow the same rule.

Publisher retains current-call/still-running-RPC data only, not cross-call recovery bodies. Beacon additionally stores fixed Attr and the latest successfully confirmed complete Data/version. Cleanup buffers are not permission for background business replay. Reuse generated messages only after actual completion, without separate queue/send/recovery copies. Delivered immutable snapshots may be retained and never mutated by background updates. Observer's temporary mutable selections have separate rules below; const shared_ptr alone protects neither mutable aliases nor business concurrency.

<a id="catalog"></a>

## Catalog Publisher

`client.publisher(sector, spectrum)` binds only Scope. `pub.update(batch, ttl)` synchronously commits complete Key/Data values; one key is a special case. “Batch” means a collection, not mandatory variadic syntax/container. It binds no fixed Key/TTL, requires no user version, and exposes neither Renew nor Delete.

### Versions and atomic commit

- Multiple Publishers may write different keys in one Scope; applications ensure one writer per key. SDK/Star add no distributed ownership lock, election, or global version service.
- A batch shares one version, compared per key. Disjoint keys from different Publishers may have equal numbers; a larger Scope cursor cannot skip another key's valid update.
- Internal query obtains relevant versions known by the target. Allocate above those and this Publisher's already-issued versions; gaps are allowed, overflow fails. A version consumed by an uncertain result cannot be reused with different content.
- Consecutive successes on the same target/exact key set reuse confirmed baseline so ordinary subsequent calls send only Publish. Compare sorted complete keys; reordered input is the same set. Target/set changes, uncertainty, or another admitted-call failure require Query again. Cache only the latest batch's at most 128 keys, instance, and watermark under shared budget, not Data or cumulative historical key tables.
- Restart/new Publisher queries its baseline without application-persisted versions. Query proves neither globally latest state nor unique issuance; temporary multi-Star divergence remains possible. Applications persist needed Data themselves.
- Validate/prepare/commit the entire batch atomically; any failure commits no prefix. Whole-Scope Watch/snapshots/callbacks install complete batch boundaries. Separate exact-key subscriptions do not provide joint atomic reads.
- Versions order data, not global transactions. Source history/transport must distinguish equal-number batches from different Publishers using protocol batch identity/boundaries. No global cross-Star transaction order/simultaneous visibility is promised.

### Failure and bounded repair

Only an explicit server version conflict proving the whole batch uncommitted permits one same-call Query of the same target, a new version, and one resend of the original complete batch. Query updates metadata, never replaces submitted Data. A second failure, query failure, target change, or deadline exhaustion returns immediately, not a background loop. Timeout/disconnect uncertainty and ordinary authentication/format/capacity errors do not enter this path. Failed content is not retained for future submission; only version metadata and unfinished-RPC buffers may remain.

### TTL and Star switching

Every update explicitly supplies a new TTL. SDK neither automatically Renews nor periodically repeats Data. Star computes the deadline at admission and Watch reports expiry deletion. Subscribers do not delete by client wall clock; SDK needs no Star clock synchronization. On switching, Publisher preserves issued-version evidence, queries the new target as needed, and waits for the next explicit full-data update, which may create missing keys. Switching neither restores old Scope content nor converts failure to success. Closing prevents new calls without Delete; existing records expire at their actual last deadlines.

<a id="registry"></a>
<a id="ephemeris"></a>

## Beacon

See [beacon.hpp](include/comet/beacon.hpp). Initial total deadline uses Beacon::Options::timeout; optional update timeout defaults to three seconds. Both accept `(0, 1 min]`.

`client.beacon(sector, spectrum, attr, data, ttl, beat)` combines initialization/first commit and waits synchronously for confirmation before returning a usable handle. On error the caller decides whether to invoke again; no continued initial registration after return. A timed-out initial call may leave an unrenewed remote record for TTL cleanup.

Attr/ttl/beat are fixed at creation; update replaces complete Data only. SDK validation does not replace Star TTL/identity/version/capacity checks. beat is mandatory, `0 < beat < ttl`; tick is optional. tick() validates a strictly positive interval; zero is not a keepalive-disable switch.

### Update, tick, and beat

`beacon.update(data)` synchronously allocates internal Data version/order. A successful new Update replaces Data and renews deadline in one Star commit, retaining fixed TTL. Duplicate order cannot renew again; order-only/same-byte changes may omit content notification.

`beacon.tick(interval, callback)` installs/replaces automatic sampling, whose callback returns Data. Sampling is serial per Beacon, without overlap or missed-tick backlog. A valid manual update resets the tick countdown and invalidates an earlier-started sample returning later. Sampling exceptions are reported through state/errors, not empty Data. Internal submission follows the same result rules, without resending failed samples.

beat sends Renew only if no new Update was actually sent during a beat interval. New Update may replace a separate Renew. Parameter failure, cache-only changes, or equal-content comparisons cannot postpone keepalive forever. Actual new Update/Renew schedules the next check, but send completion is not lease acknowledgment. Equality may save bandwidth only without changing synchronous-result/TTL semantics; local equality alone cannot report this call committed.

### Creation and identity recovery

A previously successful Beacon receives Client target-change notifications and restores registration automatically while preserving handle, tick settings, callbacks, and logical ID. Recovery uses fixed Attr/TTL and latest successfully confirmed Data/version. Failed/uncertain updates replace neither cache nor become incidental recovery replay.

Same-Star reconnect first continues a valid registration. Target change, instance rebuild, or expired lease may require recovery, with registration generation separate from Data version. Never relabel old cached content with a higher version merely to beat replicas. A target's newer known Data cannot be overwritten by older cache. Target/generation/attempt fencing prevents late old Update/Renew/Remove from affecting new registrations.

Recovery is intentionally weak: old-Star renewal stops and remnants expire. Temporary different-source data or an older value after switching is allowed. For example, v10 is locally confirmed, v11 commits on A but its receipt is lost, and B does not know v11; recovery may temporarily use v10, never call it v12. One subscription projection deduplicates logical IDs, without claiming one worldwide source or globally nondecreasing values.

New factories/processes do not claim arbitrary external IDs. Star initially issues a random 32-byte capability and derives a 16-byte logical UUID with fixed version/variant bits from capability, Scope, Attr, and TTL. SDK privately retains capability; recovery carries generation and confirmed order/Data. Target returns actual adopted Data/order, including newer known data. Capability never enters Watch, replication, or diagnostics. UUID knowledge/shared APIKEY alone cannot recover a new-style registration. SDK, Star, and replication protocols must upgrade together; old Stars are incompatible.

`state()`/`changed(callback)` report ready, recovering, failed, uncertain lease, closed, and reasons. Unchanged beats need not notify. Permanent parameter/identity errors stop relevant recovery rather than retry forever or create fresh identity to bypass rejection.

### Local lease budget

SDK neither compares server absolute deadlines with wall time nor contacts Pulsar. New Create/recovery/Update/Renew records first-send elapsed time; success establishes conservative first-send + acknowledged TTL budget. Same-order retries retain the original start, never restart TTL at late acknowledgment. Beat scheduling and confirmed lease state are separate.

Budget exhaustion means uncertainty, not proof of global deletion. Recovery probes retain finite deadlines/backoff. Local clock-read failure pauses dependent sends and reports errors without invented time. Resume from suspend immediately rechecks budgets without replaying every missed tick/beat. Linux uses suspend-inclusive CLOCK_BOOTTIME; Windows uses QueryInterruptTimePrecise, separate from ordinary RPC monotonic deadlines. Cross-machine rates/long suspension are not strict liveness proofs; Star decides actual expiry.

### Destroy and lifecycle

`beacon.destroy()` immediately/idempotently closes locally without waiting for network or affecting Client/other roles. Stop new update/tick/beat/recovery, cancel old attempts, and attempt unregister within bounded cleanup budget; immediate remote disappearance is not guaranteed. Self-callback destroy is valid. Already-started callbacks may finish but returned sample data is discarded; close winning first prevents new callbacks/samples. Started synchronous RPC settles once and may have committed remotely. Resources drain at real completion; never wait on one's own callback. Closed handles cannot revive after target/credential changes.

## Business authentication

[Astrolabe](../../astrolabe/README.md) and [Session protocol](../../proto/README.md#session) define deployed credentials/server access. auth defaults on; anonymous mode must be explicitly disabled and allowed by server, never automatic downgrade. Comet configures Star business endpoints only, not Pulsar/internal management/metrics ports.

One Client/Star coalesces authentication and owns one current Session stream. Protected requests wait for its first confirmation and reuse metadata. A separate short initial-confirmation timer is cancelled on success, not imposed as the long Session lifetime. Late confirmations/endings cannot overwrite a newer Session. Idle Sessions may persist while business owners exist. Stream loss retries with backoff; definite bad SECRET/revoked APIKEY does not loop indefinitely.

Normal close stops business, attempts bounded unregister, then cancels Session; one object does not close shared authentication. After ordinary admission closes, only internal Remove for existing UUIDs may consume reserved cleanup quota, without being blocked by that ordinary gate or creating registrations. Missing session/connection may leave cleanup to TTL. Credentials/tokens never enter logs, URLs, error bodies, or CLI.

### Client secret updates and business recovery

Applications may replace SECRET for the same APIKEY on an existing Client for subsequent login, without rebuilding roles. Concurrent authentication sees complete configurations; in-flight buffers are immutable. Local replacement neither sends credentials to Star/Almanac nor revalidates a dead Session. This does not imply APIKEY switching; APIKEY is not registration ownership.

New secret resumes a Client paused by old-credential rejection through shared target authentication. Merely changing local configuration does not interrupt a still-valid Session. Late old-config login/old-Session RPC invalidation cannot overwrite/fail a newer Session. Closed Clients stay closed. Explicitly resubmitting the same SECRET can retry paused login once, allowing server restoration; value equality must not ignore that user recovery action.

Deleting/recreating an APIKEY never revives old Sessions. Existing registrations may authenticate through Client::secret and then manage their original UUID under actual instance/deadline rules; new Clients/objects do not adopt UUIDs. This is object lifecycle, not APIKEY ownership. See [credentials](../../astrolabe/README.md#credential-lifecycle).

Same-Star relogin preserves subscription Scope/last complete view and resumes from the last complete cursor. Ephemeris may continue valid registrations; TTL keeps passing during authentication loss, and needed re-registration preserves Beacon logical ID. Secret changes do not change prior results. Publisher restores connectivity/version preparation for later explicit writes only, never failed-content replay. Target/instance changes retain their separate recovery rules.

When [credential snapshot recovery](../../proto/README.md#credential-snapshot) closes all old Sessions, use shared relogin, not per-object login or target switching solely for this reason. Losing an established Session differs from initial rejection: a still-valid SECRET automatically reauthenticates without Client::secret; definite new-login rejection pauses. Late old-stream/snapshot cleanup cannot overwrite the new Session. View/UUID/order/TTL recovery remains unchanged.

<a id="tls"></a>

## Optional TLS and trust sources

TLS is independent of APIKEY/APISECRET and defaults on. Explicit plaintext must match the target listener. Single-host deployment uses the same configuration, without standalone bypass. SDK never guesses mode or downgrades after handshake/certificate failure. Both self-signed and public CAs retain chain/hostname verification. [Runtime configuration](../../docs/build.md#deployment-and-wiring) owns Star defaults/material failures/internal links.

CMake may generate a private embedded byte list from a public CA certificate, with runtime file override on every platform, without `#embed`. This means X.509 CA/trust bundle, not Ed25519 admission key/raw-key pinning. It adds no client private key or mutual TLS.

- `COMET_CA_FILE` supplies public certificate input. Built-in hexadecimal reading generates bytes without another tool; certificate changes reconfigure. TLS receives explicit length, not assumed NUL termination.
- Explicit runtime file overrides embedding. Missing/invalid explicit input errors rather than silently switching trust. With neither, platform-available default CA sources may be used, without claiming identical automatic trust stores everywhere.
- Projects may embed their own self-signed CA or use external/public bundles. Star deploys matching server cert/private key; chain/hostname checks remain.
- Server/CA signing private keys and APISECRET are never embedded. An empty embedding path generates/references no byte file and supports external-only trust.

Comet trust configuration changes neither management nor node trust; see [Session](../../proto/README.md#session).

## Callbacks and lifecycle

watch/changed invoke user code after complete publication and internal-state unlock. Content callbacks for one reader are ordered and may coalesce undelivered intermediate states; state/cross-object notifications may overlap. Values own storage rather than borrowing soon-recycled messages. Write results do not depend on callback completion.

Notifications must be fast/nonblocking: no synchronous RPC, cleanup wait, or lock-held wait for another notification on SDK I/O callbacks. Post lengthy/write work to the application's bounded executor. tick is a separate sampling task; bounded SDK workers run user sampling and subsequent synchronous submission, without blocking shared gRPC reactions. Arbitrarily slow sampling may still delay its own Beacon.

Callbacks may stop/destroy immediately. SDK catches C++ callback-boundary exceptions and reports boundedly without replay/rollback. User context must outlive started callbacks. Logical close and full drain differ; cancellation is not OnDone. No self-wait or business-reference cycles. Beacon factory awaits the first receipt; other factories validate locally then may synchronize in background. Late attachment of watch/changed still receives current complete baseline/state, never misses it forever. Effective stop/destroy prevents new callbacks. Callbacks use move_only_function, no public Task/Executor/cross-language wrapper. tick returns Value: nullptr is invalid, empty vector valid. `tick(interval, {})` removes sampling but leaves beat enabled.

<a id="subscription"></a>

## Subscriptions and immutable views

Reader/Subscriber deliver owned Maps or exact Key/optional<Value> only after complete installation; state/changed return View with state/data. Observer Pool/Item support local selection/controlled estimates. Legacy immutable load/watch/select View entries remain available.

### Subscriber and Reader

Catalog offers `client.subscriber(sector, spectrum)` and `(sector, spectrum, key)`; Almanac `client.reader(...)` has the same shape.

| Entry | Meaning |
| --- | --- |
| Whole Scope `watch(callback(Map<Key, Data>))` | Fully installed logical Map |
| Exact key `watch(callback(Key, std::optional<Data>))` | Present value or nullopt for complete-baseline absence/later deletion |
| `state()` / `changed(callback)` | Ready/stale/recovering/failed/closed state and reason |
| `stop()` | Immediate idempotent object stop, not shared Client close |

Deliver the first complete baseline once, even empty Map/missing exact key, then notify at complete-change boundaries. Empty Data inside optional is valid. Unsynchronized/disconnected/decode-failed is not nullopt. Applications may cache delivered values. Logical Map does not require full-table transmission/deep copy every time; transport remains snapshots/deltas.

Multipage/atomic multikey batches prepare completely before installation/notification, never exposing a partial page/batch. Sharing reduces copies but later networking/close cannot mutate/free delivered data. Applications need no server Index::View completion protocol, generated types, or private page ABI.

### Observer's temporary mutable selections

Observer is Ephemeris-only: `client.observer(sector, spectrum)`, with new `one(selector)` and `stop()` APIs. Its private Watch continuously updates a local pool; one sends no RPC. Unready/stopped, no available item, and a selected view are distinct. No new watch/change or generic weight interface is added by this selection API.

Selected views contain complete registration/business Data. Applications may adjust short-term estimates in Data for later one calls, without remote writes or a permanently live remote object. New authoritative Data for an ID replaces estimates; late edits through older views cannot overwrite a new data generation. Retained references guarantee memory safety, not indefinite membership in the selectable pool.

Selector owns filtering, exclusion, preferred matches, scores/weights, and post-selection adjustments. SDK hardcodes neither minimum-weight-only policy nor “add M”/“fill to 500” units. Sampling/P2C, buckets, or indexes are possible; no mandatory full scan or universal O(1) claim.

Selection/adjustment has controlled concurrency with network versions taking priority. Arbitrary user logic cannot run under the whole pool's write lock, nor may raw writable pointers escape generation checks. Observer::Pool is a fixed snapshot; selector returns optional<Observer::Item>, and one returns Result<optional<Item>>. Item::record() returns full Attr/Data. Item::update(Value) CAS-checks authority-value identity and the previous local value. Separate Item copies may race and fail as obsolete/conflict; callers serialize mutation of one Item instance. After stop, old Items remain readable but cannot write the pool. Replaced/stopped pools cannot be modified through retained views.

### Shared subscription implementation

Reader/Subscriber/Observer share bounded staging, complete install, old-stream fencing, cancellation, authentication, and backoff in a private Watch core. Observer's local estimates do not mutate shared authority snapshots. Domain version/recovery semantics remain separate. Ephemeris data deltas replace Data on an existing complete record, retaining Attr; missing Attr triggers bounded [protocol reset](../../proto/README.md#registry-delta), never partial registration. Pure renewals do not redeliver unchanged content. Star deduplicates logical IDs across sources; SDK does not merge source streams.

### Disconnection and recovery

Before full synchronization, state is unready. Disconnect retains the last complete view as stale. New snapshots build independently; failure never mixes old/new state. Catalog/Observer switching accepts the new target's complete projection, potentially with missing entries/lower Data versions, without filling from the old local view. Late old-stream frames cannot enter the new target.

An Almanac Reader retains its own observed version floor for its authority Scope. After v100, a target at v90 leaves stale v100 intact until target ≥v100; never install v90. New Readers do not inherit another object's floor. This guarantees neither globally latest Polaris data nor Catalog-style TTL/expiry.

Notifications may coalesce and are not guaranteed-delivery logs. Permanent format/scope errors or inability to fit a complete view stop automatic download with an explicit reason, never repeated bad-snapshot loops. SDK cannot reclaim application-held old values, but they keep neither subscription nor Client alive.

### Recovery after errors

Automatic Beacon recovery and explicit writes retain separate results/deadlines.

| Observation | Action |
| --- | --- |
| Initial Beacon registration fails | Factory returns error; caller decides whether to retry creation |
| Explicit Update/Publish fails/is uncertain | Return once, no background resend; Beacon retains prior confirmed cache |
| Definite Catalog conflict, whole batch uncommitted | One same-target query/repair inside original deadline |
| Successful Beacon changes Star/loses registration | Bounded same-ID recovery with status, no failed-update replay |
| Old Session invalidates | Client-coalesced relogin, preserving old results and recovering Watch/Beacon |
| Login definitely rejected | Pause; Client::secret may update the same APIKEY's SECRET |
| Almanac target temporarily behind | Retain old view/floor; bounded reconnect waits |
| Permanent parameter/scope/capacity/protocol error | Stop affected recovery with reason, leaving unrelated roles intact |
| Version/order exhaustion | Fail, never wrap or hide with new identity |

Recovery success and individual-write success are separate. retry_ms cannot override total deadline or revive permanent errors. Business methods add no gRPC retry/hedging policy on top of repair/recovery. Library transparent retries rely only on the request not yet reaching server application code, not exactly-once guarantees.

## Multi-Star access

Client has one active Star. Multiple addresses enable bounded switching, not per-RPC round-robin or simultaneous Beacon creation everywhere. Addresses must stably route to the intended Star; reconnect is not necessarily instance change, and each operation validates actual instance. One business error alone cannot trigger a Client-wide switch storm. Shared Core switches; original writes never redirect, later calls use the new target. Watch drops incompatible instance cursors while Almanac retains its authority floor. Beacon weakly recovers stable ID; Catalog waits for the next explicit full update. Memory-only services promise no lossless whole-cluster restart.

<a id="rpc"></a>

## Protocol adaptation

[Protocol](../../proto/README.md#comet) and comet.proto define schemas/services/field numbers. Catalog.Query, Beacon capability/generation/order recovery, atomic Update renewal, and source merge are wired. Deploy Star/SDK together. Go generated protocol was synchronized; this does not implement a Go SDK.

## Local resources

These configurable initial values are not performance measurements. [Server/encoding budgets](../../proto/README.md#server-and-encoding-budgets) remain independent. There is no Limits/Inspect negotiation; local preflight does not replace server rejection.

| Item | Initial value | Accounting/behavior |
| --- | --- | --- |
| Explicit unary admitted per Client | 256 | Includes connection/auth waits, version preparation, in-flight; no Publisher FIFO; immediate overflow rejection, separate renewal/cleanup reserve |
| Automatic recovery/renewal/cleanup in-flight | 64 total, at least 16 reserved for renewal/cleanup | Registration recovery cannot consume reserve; waits coalesce in object state, not unlimited RPCs |
| Subscription objects / actual Watch RPCs | 64 each | Empty scopes count; RPCs include waiting/cancelling old streams until completion; no hidden extra connections |
| One reader view / building batch | 64 MiB each | Also charge retained old storage during installation; never partial publish on exhaustion |
| Client controlled storage | 256 MiB | Pending requests, current Catalog payload, Ephemeris Attr/Data, in-flight, current/building views; externally retained history cannot be forcibly reclaimed |
| Explicit unary deadline | 3 seconds | From admission through connect/auth/query/repair/RPC; remaining budget only. Renewal prefers conservative lease budget; probes after expiry still have finite RPC deadlines, not lease extension |
| Initial Session confirmation | 3 seconds | Cancellable local wait, removed on success, not total Session lifetime |
| Recovery backoff | 100 ms initially, up to 5 seconds plus jitter | Connection alone does not reset persistent-failure backoff; nearer lease deadlines constrain renewal |

Count/byte limits cover queued connection/auth waits and in-flight work, without Publisher FIFO. Business overload cannot exhaust renewal/close/settlement resources. Cancellation/local timeout cannot return gRPC-held quotas/bytes early; actual completion reclaims them. New attempts share total limits, never hide old backlog by replacing a “current attempt.” Charge real shared storage, not shared_ptr size. Application-retained old views and gRPC/TLS/allocator overhead are outside enforceable internal quota.

View/staging budgets include containers, keys, decoded bodies, and preparation peak. Oversized snapshots cancel that stream, free staging, retain stale complete view/cursor, and report local capacity. Do not redownload the same impossible snapshot; applications adjust budget/scope and explicitly recreate. HTTP/2 backpressure is not an aggregate-memory bound.

### Transport and scheduling boundaries

One active Star initially uses two bounded transport categories: Session/Watch streaming Channel and Publish/Create/Update/Renew/Remove unary Channel. Both share Client/instance/Session, without authenticating twice or requiring a single TCP connection. Separate Channel objects alone do not prove pool isolation; use supported independent subchannel pools so long streams cannot fill the same connection and block renewal. No per-subscription/unbounded Channel pool or strict-priority claim. See [gRPC performance](https://grpc.io/docs/guides/performance/).

Long-stream count and view bytes have independent quotas. Empty scopes/exact subscriptions consume HTTP/2 streams, server observers, and local objects. Session does not consume Watch business quota. Relogin pauses new Watches, cancels old-session dependents, waits for actual old Session completion/transport capacity, then logs in and restores Watches, avoiding self-blocked login. Local counters reserve no remote HTTP/2 slots; transport queues remain subject to confirmation deadlines, not physical-priority guarantees.

[External keepalive](../../proto/README.md#external-keepalive) defines both endpoints. Splitting transports only isolates stream slots; unary CPU/network load may still delay Renew. Reserves/deadline scheduling are not latency guarantees and need concurrent Publish/Update acceptance, not a speculative third Channel/per-object connection.

All stream admission/wait/in-flight work counts. At most one read and one write are in flight per stream; message reuse waits for corresponding completion. Session continues reading termination after first success; not expecting another business message does not justify abandoning reads. A second confirmation is a protocol error. Callbacks can overlap; cancellation is not OnDone. See [gRPC callback rules](https://grpc.io/docs/languages/cpp/best_practices/).

Automatic work shares bounded timers/scheduling, not one thread per Publisher/Beacon/Scope. Version allocation and actual submission have defined order; “latest pending value” cannot overwrite another synchronous result. Catalog has no automatic publishing/renewal. Beacon recovery/tick/beat share bounded scheduling, isolate recovery from current registration, and charge cancelled old attempts until completion. tick sampling/synchronous waits do not occupy shared gRPC reaction threads. Each tick-enabled Client lazily creates two sampling threads with 64 shared candidate slots. Full slots defer the next sample without historical backlog. Close drains tasks/reclaims threads; Client::wait completes that reclamation. No public Task/Executor. Last owner starts close only; internal threads cannot self-join or touch freed state. Drain before unloading the library.

## C++ public API

See include/comet for exact types/overloads. Scope is two strings. This pseudocode shows shapes; C++ passes Scope{sector, spectrum}, vector<Publisher::Entry>, and selectors taking const Observer::Pool& returning optional<Observer::Item>. SDK manages versions/generations/transport metadata.

```text
beacon = client.beacon(sector, spectrum, attr, data, ttl, beat) // Synchronous expected<Beacon, Error>
beacon.update(data)                                         // Synchronous result
beacon.tick(interval, callbackReturningData)                 // Optional; interval > 0
beacon.state()
beacon.changed(callback)
beacon.destroy()                                            // Immediate local close

observer = client.observer(sector, spectrum)
view = observer.one(selector)
observer.stop()

pub = client.publisher(sector, spectrum)
pub.update(batch, ttl)                                      // Synchronous complete values

sub = client.subscriber(sector, spectrum)
sub.watch(callback(Map<Key, Data>))
exact = client.subscriber(sector, spectrum, key)
exact.watch(callback(Key, optional<Data>))
sub.state() / sub.changed(callback) / sub.stop()

reader = client.reader(sector, spectrum[, key])              // Same shape as subscriber
reader.watch(callback)
reader.state() / reader.changed(callback) / reader.stop()
```

Actual C++ inside an application scope owning client follows. Keep role handles until business completion and handle every Result.

```cpp
using namespace std::chrono_literals;
auto beacon = client.beacon({"services", "main"}, {1}, {2}, 30s, 10s);
if (!beacon) {
    return std::unexpected(beacon.error());
}
auto updated = beacon->update(std::vector<std::uint8_t>{3});
if (!updated) {
    return std::unexpected(updated.error());
}
auto sampled = beacon->tick(1s, []() -> comet::Value {
    return std::make_shared<const std::vector<std::uint8_t>>(1, 4);
});
if (!sampled) {
    return std::unexpected(sampled.error());
}

auto observer = client.observer({"services", "main"});
if (!observer) {
    return std::unexpected(observer.error());
}
const auto id = beacon->state().identity->uuid;
auto selected = observer->one([&id](const comet::Observer::Pool& pool) {
    return pool.find(id);
}); // Before a complete baseline, returns busy/current recovery error; select again later.
if (selected && *selected) {
    auto adjusted = (**selected).update(std::vector<std::uint8_t>{5});
    if (!adjusted) {
        return std::unexpected(adjusted.error());
    }
}
```

Observer adjustment changes neither Beacon nor Star Data. Reader/Subscriber callbacks may retain Maps/Values. Unregister callbacks with the matching empty move_only_function; exact/whole-Scope overloads are not interchangeable. destroy/stop are nonblocking; application shutdown uses Client::close/Client::wait to drain.

Client::open, Client::secret, and explicit Client::close own configuration, credentials, and shared shutdown. Publisher handle release/shared Client close ends local resources without remote deletion. update is no arbitrary task framework. C++23 expected/optional/standard ownership express ordinary business errors without exception-driven normal control flow.

### Results and diagnostics

Error includes specific cause/commit certainty and distinguishes original target from response-claimed instance. Successful receipts must match target, version/order, identity, and TTL. Invalid receipts are protocol errors while sent requests may still have committed. Diagnostics may record versions without requiring applications to create/understand them.

Beacon distinguishes ready/recovering/uncertain lease/closed; Reader/Subscriber distinguish unready/usable/stale/error. State snapshots are not remote live queries. Catalog key versions, Almanac authority versions, Ephemeris Data order, and Star Watch cursors are not interchangeable; no number-mapping dictionary is created.

### C++ / Go / Rust implementation boundaries

- C++23 expected represents synchronous results, optional exact-key presence, shared immutable storage retained callbacks. Observer needs generation-checked mutable estimates, not raw references escaping to background mutation.
- Go would use synchronous values/errors and Context-bounded RPC. Mutable map/[]byte aliases require copy/ownership transfer. Explicit presence separates empty/delete. stop/destroy logically stop immediately and internally drain goroutines, without finalizers.
- Rust would use Result/Option, owned payloads/Arc, and explicitly scoped Send/Sync callback requirements. A synchronous façade cannot block internal async I/O executors; Drop is a cleanup fallback only.

Languages share contracts, not container ABI/runtime. Applications retain complete Data; no generic weight service is added. This assesses feasibility only, neither revives abandoned Rust projects nor implements Go SDK.

## Static library and CMake delivery

Initial delivery is native C++ static only, supporting source-tree and installed CMake integration, without shared library/C ABI/other bindings. [CMakeLists.txt](CMakeLists.txt) defines targets/install; [validation](../../docs/validation.md) records actual build/install/consumer evidence.

| Item | Contract |
| --- | --- |
| Library/target | Static `comet`, link `comet::comet`, no direct server-target dependency |
| Source integration | add_subdirectory on comet/cpp with shared dependency rules; standalone SDK does not build Star/Pulsar/Astrolabe executables |
| Installed integration | find_package(Comet CONFIG REQUIRED), same target; headers/archive/exports/licenses without build-machine source/cache absolute paths |
| Public dependencies | Comet/standard types only, no generated headers; C++23 minimum exported as cxx_std_23, locally mapped to /std:c++latest on MSVC |
| Link dependencies | Export real static gRPC/Protobuf dependencies; hidden generated types do not eliminate link libraries or leave archive ordering to applications |
| Protocol generation | [Unified rules](../../proto/README.md#generation): generated sources versioned; matching tools only for explicit generate/check; ordinary builds run no protoc, consumers need neither generation nor Pulsar |
| Dependency acquisition | Approved versions/caches only; missing dependencies fail clearly, no implicit download/global path or user-config edits |
| TLS materials | Optional public CA embedding/runtime override; no private keys/APISECRET in package/generated headers, no runtime-independence promise from static linkage |

Compiler, standard-library, options, and dependency ABI must match. Packages check build platform/compiler version; Linux archives cannot link on Windows. Recorded Windows/Linux integration is not cross-compiler binary compatibility. Windows Linux-specific probes/Sanitizers are unsupported. Installed packages distribute only owned files; exported CMake dependency discovery supplies third-party libraries. A future bundled binary distribution needs its own platform/license inventory.

Tests/examples are explicitly selected, never hidden in ordinary builds/package consumption. Source and installed consumer cases verify a minimal program linking only comet::comet and offline failure on missing dependencies. They include embedded public CA, explicit external CA, and disabled public TLS. RPC cases, not successful configuration, prove real TLS handshakes.

## Standalone Windows build

The Windows entry builds Comet only, not servers through tools/build.py. Current choice is Visual Studio 18/MSVC 19.51, x64 Release, `/MD`, with matching dependency architecture/runtime/configuration. SDK/generated protocols require C++23 independently of parent-server C++26. Local CMake maps cxx_std_23 to /std:c++latest; pinned gRPC also needs the compatibility hook below. Existing toolchain thresholds remain; older compilers need separate validation. See [current evidence](../../docs/validation.md).

Validated dependencies combine gRPC 1.84.0, Protobuf 36.1.0, and matching libraries, all C++ dependencies configured with CMAKE_CXX_STANDARD=23, x64 Release /MD. Generated protocol targets explicitly match handwritten language mode, avoiding Protobuf global-type link mismatch. Changing dependency standards requires fresh build directories or regenerated capability caches so old Abseil detections do not survive in installed headers. `build/deps/comet-msvc/install` was used locally, not qualification of Debug/other toolchains/machines.

[GrpcMSVC.cmake](cmake/GrpcMSVC.cmake) passed compilation of the originally failing file, gRPC rebuild, and SDK consumption. Configure Windows gRPC with `-DCMAKE_PROJECT_grpc_INCLUDE=<Astra-root>/comet/cpp/cmake/GrpcMSVC.cmake`. Only fused_filters.cc receives upstream GRPC_NO_FILTER_FUSION=1; no dependency source, language mode, or other library changes. Setting it on SDK cannot repair already-built gRPC. It skips optional fused-filter registration while preserving normal authentication/compression/message-size chains. fuse_filters defaults off in this pinned version and SDK does not enable it. This build cannot select fusion; enabling later requires removing the hook/revalidating. No performance measurement establishes a difference.

Windows lease time uses QueryInterruptTimePrecise, including suspend, requiring Windows 10/Windows Server 2016 or later. See [API](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryinterrupttimeprecise) and [semantics](https://learn.microsoft.com/en-us/windows/win32/sysinfo/interrupt-time). Do not substitute wall time or change system timer precision.

From the Astra root, with explicitly approved/prepared ABI-compatible Windows dependencies:

```powershell
cmake -S comet/cpp -B build/comet-msvc/sdk -G "Visual Studio 18 2026" -A x64 "-DCMAKE_PREFIX_PATH=$PWD/build/deps/comet-msvc/install" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
cmake --build build/comet-msvc/sdk --config Release --parallel 4
cmake --install build/comet-msvc/sdk --config Release --prefix "$PWD/build/comet-msvc/install" --component Comet
```

Commands fetch no dependencies and install SDK outside system directories. tests/consumer separately checks source/installed usage. Applications use find_package(Comet CONFIG REQUIRED), target_link_libraries(app PRIVATE comet::comet), and both SDK/dependency prefixes in CMAKE_PREFIX_PATH. Do not copy private/generated headers into applications. COMET_CA_FILE uses built-in CMake byte conversion everywhere, not #embed. Ordinary builds run no tests; COMET_BUILD_TESTS=ON/CTest still require current test authorization.

MSVC SDK uses /W4 /WX with a private C4996 exception for gRPC 1.84 old TLS-type instantiation in MSVC STL. SDK itself uses new InMemoryCertificateProvider; the exception does not propagate to applications. Windows probes/Sanitizers remain unsupported.

## Acceptance criteria

- Verify synchronous outcomes, unknown commit, one conflict repair, and no resend after failure; old future/desired-state recovery tests do not prove new interfaces.
- Cover initial Beacon failure, successful same-ID recovery, tick/beat races, Update renewal, confirmed-only cache, and immediate destroy.
- Verify authoritative Data replaces Observer estimates and Reader/Subscriber Map/optional, empty baseline, complete batches, and state notifications.
- Standard systems use at least three Stars, all locally writing, with cross-subscription/bidirectional replication. One/two-node diagnostics do not replace acceptance.
- Check authentication/TLS, budgets, old-callback fencing, C++23 static packages, and language ownership under the [acceptance specification](../../docs/comet.md).

Current execution requires explicit authorization; results belong only in [validation.md](../../docs/validation.md).

## Current SDK corrections

Publisher now binds Scope for synchronous submission, queries key versions internally, and neither renews automatically nor stores failed Data for replay. Only definite whole-batch-uncommitted conflicts permit one same-target/same-deadline repair. Unknown timeout results cannot be blindly replayed; target switching waits for the next explicit complete update.

| Lifecycle boundary | Handling |
| --- | --- |
| Old control iteration cancels a new call | Capture the actual operation stop source |
| Slow notification delays synchronous RPC cancellation | Shared stop_source cancels directly; stop_callback unregisters before context destruction |
| Endpoint switch races reader backoff | Backoff reads fixed settings, endpoint frozen under short lock |
| each callback replaces traversed View | Pin old root throughout traversal |
| Duplicate start/extreme wait | No duplicate Alarm Set; saturated deadlines |
| Missing new server RPC | UNIMPLEMENTED means incompatible protocol, not repeated switching |
| Callback unregisters itself | Final business capture released outside object lock; destructors still cannot wait on current callback/sample |

Publisher caches bounded target/key version baselines only and reuses the encoded request for same-call repair. This is neither pending-Data storage nor measured throughput improvement. Beacon/Observer/Reader/Subscriber new interfaces are implemented. Capability remains absent from public projections; candidates select by Data/generation/deadline. Shared bounded sampling workers fence late results; local estimates CAS against authority identity/old value. Observer retains the single-page/single-item allocation-free dedup path and evicts affected estimates on complete network commit without first copying fixed page roots. [Validation](../../docs/validation.md) records execution/gaps; [protocol](../../proto/README.md#confirmed-targets-and-implementation-gaps) owns wire behavior.

## SDK and Scope concurrency review

Ordinary Comet completions use a deduplicated weak ready queue and advance ready objects only. Expiry, shared binding changes, shutdown, and capacity return may still scan directories. Drain is O(K), not absolute O(1). Strong references from traversal release outside locks; concurrent new events may requeue.

Star exports its own Origin under export_ without GC; writes preserve gate_ → export_. Public reads use shared access before the next full tick, exclusive expiry advancement afterward. Scene keeps no second TTL; filtering only find would make snapshots/suffixes disagree at one version.

Remote native candidates prepare under source locks outside domain lock; final watermark/projection/deadline/budget commits remain under it. Busy-source waits release domain lock and reread time before advance. Whole-source recovery installs complete Scopes sequentially, ACKing only after all finish; intermediate recovery cannot clear another Scope's coverage.

Downstream StartWrite is outside service index lock, while stream io/final authentication Permit still cover sending. pop returns strong ownership; the same lock protects index changes. Ordinary bool is no lock-free guarantee. The in-flight Watch list narrows scans and sweep queues only expired busy streams, but periodic scanning remains O(in-flight streams).

Same-Scope atomicity applies to commits/complete views. Independent exact Watches have separate install timing, not joint transaction reads; cross-Star Catalog replication stays asynchronous. Per-Scope locks cannot replace domain locks without preserving cross-Scope source sequence, continuous ACK, and domain-wide budgets.
