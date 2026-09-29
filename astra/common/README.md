# Three-domain storage, snapshots, and TTL

[English](README.md) | [简体中文](README_CN.md)

Star's business storage is independently managed by Almanac, Catalog, and Ephemeris, rather than one generic Store. Almanac connects management synchronization to public Readers; native Catalog/Ephemeris commits, source recovery, and public projections are wired. The existing [Store](src/store.hpp) is no longer Runtime's empty business placeholder. Its KV, wheel, and index tests cannot qualify the new domains. See [implementation](#current) and [executed validation](../docs/validation.md).

Star has neither SQLite nor business persistence. Polaris durably owns Almanac; Astrolabe is the management entry; applications own Catalog persistence; Ephemeris is temporary registration. [Architecture](../docs/architecture.md) defines authority/admission/components; [protocol](../proto/README.md) defines messages. Shared mechanisms do not imply one business model: Pagination shares public paging, Reading shares read synchronization, while each domain owns native merge, UUID ownership, and expiry commits. See [reuse boundaries](#reuse); execution status remains in validation.

## Storage support for the new SDK

Catalog.Query uses State::versions to return merged watermarks for relevant keys, including expired bodies, under one shared read lock without creating scopes or scanning the table. Ephemeris support below is also implemented. [Validation](../docs/validation.md) binds results to configuration/source; older frozen snapshots do not cover later changes. See [protocol implementation boundaries](../proto/README.md#confirmed-targets-and-implementation-gaps).

- SDK generates Catalog content versions internally; Star queries known versions for relevant keys. Compare per key, with one version per batch, never the Scope maximum as every key's conflict condition. Equal-number batches from distinct publishers remain distinguishable. Query does not prove globally latest state.
- Every Catalog update carries a new TTL; Publisher needs neither automatic renewal nor a complete desired-state cache. Any retained Renew storage path still validates its own semantics, without defining new automatic Publisher tasks.
- Ephemeris Update atomically commits Data/order, a new deadline from fixed TTL, source facts, and scheduling. Duplicate order does not extend again. Changing only Data cannot acknowledge the new Update contract.
- A successful Beacon's logical ID survives cross-Star recovery. Multiple sources may temporarily hold it, but each projection deduplicates. Higher known Data version wins independently of registration generation. Old-generation updates/deletes cannot affect new generations. Candidates are selected by Data order/generation/deadline; fixed attributes/equal-order bodies are checked for conflicts. Legacy generation=0 retains unique-source restrictions.
- Full snapshots/deltas still install atomically. Observer's local business estimates belong to SDK selection, never mutate authoritative Attr/Data or feed mutable local views into Index writes.

<a id="data"></a>

## Three native structures

This table defines ownership, not mandatory std::map/unordered_map at every level. Sector/Spectrum still form Scope without implying a generic Store instance.

| Domain | Grouping/record | Version/deadline |
| --- | --- | --- |
| Almanac | Sector → Spectrum → authority group; Key → immutable Value | One Polaris authority version per Scope; no business TTL |
| Catalog | Source Star → source group → Sector → Spectrum → Key → Record | One replication version per source group; positive content version with optional live body/finite deadline |
| Ephemeris | Source Star → source group → Sector → Spectrum → UUID → Record | One replication version per source group; Attr, Data, one finite deadline, fixed TTL, two required operation orders |

Source Star means admitted process Member.id. Catalog/Ephemeris have separate source groups, each versioning all Scopes of that source/domain rather than a sequence per Scope. The local node is one source group; only its own commits broadcast. Remote groups update directly from their source, without third-party forwarding.

Almanac keeps independent authority groups/immutable read roots. Only complete Polaris installations or consecutive authority Patch messages are accepted. Star neither accepts Comet writes nor creates authority versions nor accepts peer Almanac forwarding. Legitimate no-op commits retain versions; empty groups never reset. Full install adopts authority version directly, not per-key increments.

Catalog Record combines content version, body, and deadline. Retained pure Renew changes only a valid complete local-source record, never borrowing a remote body/later deadline to invent local ownership. Admission also verifies no higher merged watermark; see [Catalog](../proto/README.md#catalog). Expiry may free body/scheduling while retaining that source's highest directly accepted version. Bodyless watermarks differ from valid empty values. Per-source accepted maxima differ from this Star's cross-source known maximum, which belongs to the merge index; another source's version cannot be written into the local group as a local acceptance. See [source watermarks](../proto/README.md#catalog-watermark).

Ephemeris stores one native Record per UUID, not uuid:attr/uuid:data keys. Attr/Data hold immutable byte references. Update atomically replaces Data/order and extends the same deadline; pure Renew changes deadline/renew order only. Duplicate order confirms without another extension. Create, expiry, and unregister operate on the entire record, avoiding suffix paths, paired preparation/wheel nodes/snapshot matching. A record with both bodies empty still exists. See [Ephemeris](../proto/README.md#ephemeris).

Index keys supply UUID/Key; owning groups supply source/Scope, without repeated strings per record. Queries may borrow string_view; stored/async data owns required lifetimes. Creation/recovery validates capabilities and keeps bounded source candidates per Scope/UUID. Data version, registration generation, and valid deadline select one public record; incompatible Attr/TTL or same-version body rejects. Source expiry may fall back to another valid candidate, never splice Attr or overwrite newer values by arrival order.

Internal credentials remain `Almanac["__auth"]["comet"][APIKEY]`, encoded as [Credential](../proto/README.md#credentials). Record/version/login-index/session invalidation share one installation boundary. History-skipping full installation follows [credential snapshots](../proto/README.md#credential-snapshot), without per-record identity markers. External entries isolate `__` Sectors uniformly; no Grant, APIKEY ownership, or standalone credential files return.

<a id="indexing"></a>

## Direct indexing and hot paths

Source groups own facts; Scope projections serve readers. They share bodies and stable record/key handles rather than duplicate complete Buffers or concatenated source/Scope/key paths. Use find/transparent lookup, not operator[] creating containers for failed requests, reads, or absent keys. Reuse standard containers/page indexes without mechanically adding per-layer shared_ptrs, virtual calls, or global string interning.

Known-source writes, renewals, and exact repairs locate source/Scope then Key/UUID directly. Public exact reads use Scope projection rather than scanning sources. Catalog's merge index retains current maximum/visible candidate and necessary source references. Higher versions do not require scanning/deleting every lower source record; source facts retain their own deadline/watermark rules and reads use merged projection.

Observation directories index Scope and exact Key/UUID. Body changes visit full-Scope and matching exact subscriptions only. Unrelated exact subscriptions advance continuous coverage through Scope Progress, sending empty deltas if needed. Bounded rotation does not eliminate progress work; see [performance boundaries](../docs/profile.md#performance). Pure renewal/order-only changes bypass content observers. Baseline capture and observer attachment share a commit boundary; empty-target waits are budgeted and the directory is released after the last observer.

Unknown Scope reads return complete empty version-0 views or exact absence without permanent business-directory allocation. Read-only scope counts cannot consume write slots. First real write creates the projection. A previously committed Scope retains its cursor even when empty; directory reclamation cannot disguise an old position as a new baseline.

Ephemeris Renew touches identity/order/deadline/scheduling only; Data updates reuse fixed Attr. Ordinary bodies are not decoded and Proto objects are not permanent records. Native records organize hot/cold fields by access, without pack, pointer tagging, or extra polymorphism merely to reduce sizeof. Copy paths only when pages are actually shared/frozen. use_count cannot replace read-completion/lock synchronization.

<a id="origin"></a>

## Source groups and versions

| Position | Advanced by | Meaning |
| --- | --- | --- |
| Almanac authority version | Polaris durable commit | One Scope's authority state |
| Catalog content version | SDK publisher allocation, per-key Star validation | Content order for Scope/Key |
| Dynamic source-group version | Source Star's commit in that domain | Continuous facts across all Scopes of that source/domain |
| Dynamic view cursor | Attached Star's visible projection commit | Downstream state for this instance/domain/Scope |

Versions are independent, with no content→source/cursor mapping tables. Groups start at zero and advance only for new facts; a source's two domains remain independent. Empty/reclaimed Scopes and history trimming do not reset group versions. Restart creates a new source space through new Member.id; local Generation only fences old RPC callbacks.

New local Publish/Renew, Ephemeris Create/new-order Update/new-order Renew/authoritative termination commit source records and bounded history together. Idempotent old-order confirmation creates no fact. Remote installation advances processed position without generating local-source events. Local Ephemeris expiry is source termination; replica expiry only changes local visibility. Catalog expiry never broadcasts peer deletion on any node.

Never publish group versions by fetch_add before independent data/log updates: failures/concurrency would create gaps or reorder commits. A short domain boundary sequences source commits; preparation succeeds before publishing the next position, with state/history/version visible together. The two domains, Polaris Almanac, and network sends do not share one Star-wide write lock. Serialization has real cost; fewer version spaces do not imply lock-free operation or measured throughput gain.

Local groups retain current state and one bounded send history; trimming preserves current state. Remote groups retain installed state, continuous processed position, and bounded recovery staging, not another forwarding history. Peer senders keep positions/finite in-flight references and obtain entries from shared history through [batching](../proto/README.md#stream-batching), not per-peer event FIFOs. Records/history/sends share immutable bodies.

Group numbering cannot be reused within an active identity, but empty historical-process containers need not live forever. After trusted replacement, active TTLs/in-flight references drain and [retirement](../proto/README.md#repair) reclaims groups/wheels. Local Catalog anti-regression watermarks/newer known identity remain. Reclamation adds no background full-table scan.

<a id="projection"></a>

## Downstream views and local deadlines

Comet Watch subscribes by domain/Scope/optional exact Key or UUID. Star maintains bounded read projections/history; SDK does not collect source versions, merge origins, or perform peer recovery. Native records are facts; projections share visible content/metadata, not independently writable authority KV copies.

Catalog merge state holds the highest known content version and visible result for Scope/Key. Only valid complete records at that version participate; equal-version deadlines take the protocol maximum. Once a higher version is accepted, losing its source cannot expose a lower version. A bodyless high watermark excludes old visible values. Same-version watermark-only records or snapshot omissions cannot delete a still-valid received body, even from the same source, or source TTL cleanup would become peer deletion. Disconnect, replacement, omissions, and history trimming do not remove this process's version floor. Watch need not scan every source/copy each Buffer.

Ephemeris projects whole registrations, capturing fixed Attr/current Data together while Data deltas omit unchanged Attr. Local replica expiry may remove visible records/free bodies; later source deadlines require a full record to restore. Renew cannot create an Attr-less registration, and local deletion is not source termination.

Dynamic view cursors advance only for visible content/existence changes in that Scope. Catalog content version is visible, so higher version advances even with identical bytes. Ephemeris fixed TTL/deadline/two orders are hidden from Observer. Pure deadline refresh, same-byte new Data order, or invisible watermark/source progress creates no view commit/content wakeup. Expiry/recovery/content changes create real cursors/history, never hidden mutation under an unchanged cursor. Almanac retains authority versions instead of this optimization.

Source versions resume peer replication; view cursors resume downstream streams. Renewal may advance only source version; replica expiry may advance only view cursor. No 1:1 requirement or remote TTL cleanup in source history. Used empty Scopes retain cursors and cannot recreate/reuse positions.

<a id="commit"></a>

## Atomic commit and locking

Atomicity covers native state, deadline scheduling, required versions/history, and visible projection, not indirect generic Store put/remove/renew adaptation. Final admission rechecks parameters, identity, Session, content version, order, and expiry. Allocation/budget failure cannot install a record without replication evidence.

- Index lookup obtains stable handles, then releases index protection before business processing. Erasable Scope directories must transfer ownership, such as shared_ptr handles; map-node address stability does not survive erase. Invalidation differs from destruction; active groups/views/schedulers stay alive. Reader/writer locks may reduce lookup contention but are not assumed faster or added mechanically at every layer.
- Each dynamic domain has its own short commit guard publishing origin state and local projection together, preventing torn watermark/value merges from concurrent sources. Almanac guards authority groups separately, without a three-domain lock. Further partitioning requires hotspot evidence.
- Comet final admission takes Session validity protection before business commit protection. Credential install/revoke follows the same order and never waits in reverse on active commits. No per-Scope permission lock is added.
- Large payload preparation, full snapshot traversal, encoding, network sends, cancellation, and user callbacks occur outside business locks. Dependent index-node/path preparation remains controlled, without copying whole source/projection maps under lock.
- Complete sources are received/built as private Drafts outside locks, then installed Scope by Scope. Each remote Replica has a separate preparation lock and stable shared ownership. Waiters release the domain lock before waiting, then revalidate directory identity. Native candidates, old-item enumeration, body validation/reuse, and deadline nodes prepare outside the domain lock, without blocking other sources/local writes. Reacquire it to merge against current watermarks/projections, recheck identity/position/deadline, and publish atomically. Preparation transactions do not span Scopes or ACK early. Final projection/budget/notification/native-root publication still uses domain gate; there are no per-Scope commit locks yet, and individual remote deltas are admitted under the domain lock.
- Send failure after successful commit affects recovery/subscription only, without rollback, another increment, or misreporting uncommitted. Transfer old-root ownership at commit and release potentially large final-reference destruction outside locks. An O(1) root swap does not make whole-tree destruction cheap inside a critical section.

Catalog writes may atomically batch keys in one Scope; Ephemeris remains one registration per operation. Same-Scope snapshots/downstream see complete commits, never preparation prefixes. Source sequencing adds no cross-Scope management transaction. Whole-source recovery covers multiple Scopes, each delivered as a complete projection; SDK receives no atomic observation across separate subscriptions.

## Time and scheduling

Use Clock's continuous Unix-nanosecond axis and Wheel's complete catch-up semantics. Native records keep one absolute deadline, not per-record era/second deadline. Business entries validate TTL units/ranges/overflow. After calibration, Pulsar loss alone does not revoke new-lease eligibility; see [time](../pulsar/README.md#clock).

Almanac has no wheel. Dynamic groups prepare schedulers lazily before finite deadlines, not Wheel<4, 10>, threads, or timers per Sector/Spectrum. Source-group nodes advance through the shared loop. One native Ephemeris record has one node; bodyless Catalog watermarks have none. Node/wheel addresses remain stable until removal; allocation/close count toward total budgets.

Source snapshots require original deadlines. Renewal updates capturable native state and schedule together without mutating frozen snapshots. Downstream views contain public content only, so hidden deadline changes neither rebuild content indexes nor retain duplicate content history. Source history still carries renewal traffic; recovery windows cannot be estimated from visible-content frequency.

Local-source capture/suffix/exact repair use a separate shared export lock without reading business time or cleaning other sources. Local writes take exclusive domain gate → export, never reverse. Const projection reads use shared gate; only crossing a source wheel's next full tick retries exclusively to read time/advance. Injected-clock/reversal checks have their own short lock. View completion synchronizes through the corresponding shared lock without changing root/page lifetime contracts.

TTL maintenance under domain lock only tries remote preparation locks and never blocks in reverse. Busy sources retain expiry boundaries; retiring empty sources retain reclamation responsibility. Local writes may commit after their own deadline checks. Public capture/find/suffix and background tick encountering pending maintenance wait outside the domain lock, then reread time/fully advance before promising fresh views. Private preparation retains the original root to force COW; publication and old-View completion share gate synchronization.

Background and pre-Watch advancement share controlled scheduling, without per-key clock reads or scanning all source records for one Scope query. Reread business time before commit; an expired local UUID cannot revive using pre-wait/preparation time. Replica expiry changes local projection/scheduling only, not source ACK/version. Catch up every elapsed tick with fractional remainder, round finite deadlines upward, never delete early, and do not cap max_ticks leaving internal time behind. Preparation failure publishes neither partial expiry nor false catch-up; affected subscriptions report stale/recovery failure per protocol. Large catch-up/bulk expiry still need performance evidence, not constant-time claims.

## Snapshots and history

Downstream batches with equal domain/Scope/target/start/end cursor share immutable record sources. Each stream has its own page position/final Write acknowledgment. A recent batch may supply a complete prefix of a known continuous suffix while newer changes remain pending per stream. The control thread reuses still-in-flight same-page Protobuf messages. Scope holds a weak reference to its latest batch; batches hold separate weak page references, so fast/slow interleaving does not overwrite other in-flight pages. No consumer releases bodies; later requests may rebuild. Page slots/weak control blocks grow with this frozen batch's pages and die with it, never accumulate across batches. Page indices extend contiguously, rejecting huge sparse indices. Per-stream budgets remain conservatively separate; cancelling one does not release another's borrowed message. Active suffix-vector consolidation runs outside downstream queue locks; commit collection still maintains per-subscription pending maps, not O(1) broadcast or zero-copy gRPC serialization.

Source snapshots freeze a whole source group's native state/version; downstream snapshots freeze a Scope's merged projection/cursor; Almanac freezes its authority-group version. Capture root/position at the short commit boundary, then page outside locks from resumable positions without rescanning prefixes, pre-copying complete maps, or pairing Ephemeris fragments.

A source snapshot covers every Scope in that source/domain, including an explicitly complete empty group. Scope-organized pages are not separate recovery positions. New Scopes/concurrent updates continue from group-baseline history. Incomplete reception changes nothing. Once complete, Scopes may install atomically in sequence, but one completed Scope is not a completed group. Missing single records use [exact repair](../proto/README.md#repair); history gaps require whole-group recovery.

The full-source Draft spans all Scopes; each control round installs at most one. Source ACK is indivisible. Record count, metadata, bodies, frozen pages, projection preparation, and in-flight references are budgeted under [recovery capacity](#capacity); paging does not imply constant total staging memory. Interruption/later-Scope failure preserves installed Scopes and leaves others old. Internal Scope coverage records complete target B, preventing old events at/below B after reconnect from overwriting installed scopes, including omitted keys and locally expired UUIDs. Only all-Scope completion advances source ACK/clears coverage. A Scope capacity failure may be deferred once to let others free capacity, not retried indefinitely.

Downstream baseline capture simultaneously attaches observation. During snapshots, bounded changes coalesce by target without pinning history indefinitely or assuming it survives. Extraction preserves complete coverage, never labels a newer live value with an older target version. Ephemeris history preserves create/change/end and sends data-only only when the subscription baseline is proven to contain Attr; see [projection deltas](../proto/README.md#ephemeris-delta).

Source-send and local-projection histories independently have count/byte/time budgets. Eviction does not imply payload release while snapshots/in-flight references remain; retained costs still count. Exact Key/UUID queries get only the target/boundary, not a whole-group snapshot. Full streams share immutable bases rather than per-subscriber registries.

Origin/Scene retention is lazy write-time history age trimming, not read expiry. Same-instance/scope continuous suffixes remain replayable until actually trimmed. Reads neither refresh timestamps nor extend retention at the next write. Actual gaps require full baselines; send count/byte limits remain. Zero retention evicts new history at commit. Business TTL advances independently; replay retains original absolute deadlines, never a fresh lease.

Immutable views avoid long-held business write locks during traversal; reference publication/counting/reclamation are not inherently lock-free. [atomic shared_ptr](https://eel.is/c++draft/util.smartptr.atomic.shared) implementation determines lock freedom. No special RCU/hazard-pointer framework is added. Publish one new root per complete change batch, sharing unchanged pages instead of snapshotting for every internal record.

Clock, intrusive Wheel, immutable bytes, and path-copy indexes are reusable. [Pages](src/pages.hpp) stores different native Records; Index = Pages<Cell> preserves existing KV use. Do not encode native Ephemeris/bodyless Catalog in Cell or recreate a unified Store, business inheritance hierarchy, or generic Actor. Preserve read-completion/reclamation boundaries: use_count() is no concurrency barrier.

Almanac's outside-lock preparation, atomic install, consecutive commits, and bounded history are patterns for dynamic groups, not a per-Scope independently numbered StarSlice. [Source-group version scope](#origin) and domain merge/deadline rules remain. Full snapshots cannot replace [exact-repair coverage](../proto/README.md#repair); repair ahead of group position must still allow older events for other targets.

Index::View is internal to Store read regions and must synchronize through its state lock after reading. It cannot directly serve as an SDK public View outliving Client. Reuse must close read-completion/final-reference reclamation, not just O(1) capture. SDK read storage has independent safe lifetime; final release touches no destroyed network core/mutex, and old views keep no closed connection running. See [SDK views](../comet/cpp/README.md#subscription).

<a id="capacity"></a>

## Capacity and recovery reserves

Business budget is not all available for active data. Admission/configuration must satisfy resident state + admitted concurrent recovery peak + maintenance reserve ≤ controlled total budget. Resident includes watermarks/empty-scope cursors/history; recovery includes coexisting roots, projection paths, encoded/decoded pages, and actual in-flight data; maintenance includes expiry/renewal/close. Shared physical storage has one internal accounting owner and remains charged until references release it. Accounting is conservative engineering, not an exact allocator/RSS equation.

New keys, larger bodies, or higher deployment limits must also ensure the maximum legal whole source group/Almanac Scope can recover within reserves. Normal put's incremental cost or today's smaller groups cannot justify unlimited growth. Multi-source/target recovery shares one admission budget; queues retain positions, not precloned candidates per connection. Initial implementation uses bounded component counters/RAII returns, not a global memory manager or per-byte callbacks across modules.

Separate encoded-item hard limits, permanent watermark capacity, and transient in-flight pressure. Transient pressure may back off boundedly. A known oversized complete state reports incompatible capacity and stops redownloading that baseline until configuration/data permits recovery. Retain the old complete state/reason; endless reconnect cannot fit impossible data. Polaris durable Scope/Star capacity is a deployment contract, not per-request remote Limits negotiation or an all-Star-ACK requirement before management commit.

Never evict highest Catalog versions, valid records, or in-flight bodies to free space. Ordinary writes cannot consume maintenance reserves, but arbitrary allocation failure still may prevent expiry. If advancement is impossible, end affected Watch catch-up promises under protocol. Separately verify maximum item, maximum legal recovery, retained old roots, multi-target rebuild, and preparation failure; see [initial budgets](../proto/README.md#server-and-encoding-budgets).

<a id="current"></a>

## Existing primitives and validation scope

[Almanac](../star/src/almanac.hpp) owns one Scope's native content, authority version, private full candidate, and bounded continuous history without Store. Draft builds outside lock, reset swaps complete state, and apply installs authority +1. Point lookup/page index share Key/Value. Capturing View copies no Map. Traversal completion, including exceptions, synchronizes through an independently owned mutex; old Views retain pages/synchronization after owner destruction. Reused Index KV pages always have empty deadline, without a wheel/second version.

View::each callbacks hold no commit lock and may reenter public operations on a live Almanac. Completion synchronization can still wait for writers, without fixed/no-wait guarantees. Callers holding outer locks must preserve commit-path lock order rather than prohibit all callback reentrancy to hide ordering problems. Legal commits exceeding history budget still take effect, evicting their own and all old history. New continuous history can accumulate afterward, but cannot bridge the evicted commit with an incomplete delta.

This is a local core: current capacity measures active content/history, not a process-wide budget across Scopes, staging, old Views, and networking. Sector/Spectrum Library, full receive/install, public Gateway/Readout, and C++ Client/Reader are integrated/regressed. Native entries assume callers already validated UTF-8/identity; they check key length/NUL, body limits, and state version. [Ordinary](../star/tests/almanac_test.cpp) and [allocation-fault](../star/tests/almanac_fault_test.cpp) cases belong to CTest; actual execution is in [validation](../docs/validation.md).

[Catalog](../star/src/catalog.hpp)/[Ephemeris](../star/src/ephemeris.hpp) have separate native records/allocation-free candidate rules. Catalog distinguishes source facts, merged watermarks, and visible changes; Ephemeris distinguishes new facts from same-order confirmation. [Origin](../star/src/origin.hpp) shares source grouping/native pages/continuous positions/send history only, not TTL/order/conflict decisions or Almanac. Edit publishes only after all preparation succeeds. Completed private Drafts enter Restore for atomic per-Scope installation, acknowledging continuity after the entire source. Move retired objects outside outer commit locks for destruction. Views retain completion synchronization, not reference-count-as-barrier assumptions.

[Agenda](../star/src/agenda.hpp) lazily allocates Wheel<4, 8> per source with fixed 10 ms ticks, not per Scope/second deadline per record. Callback rescheduling uses the current tick; failure preserves a later expiry opportunity. Large advance has no max_ticks cap. Source history conservatively charges full native references; shape tags select data/renew/full transport. Broadcast/shared-accounting effects still require measurement.

[Scene](../star/src/scene.hpp) stores public content/independent cursors and shares names/immutable bodies with Origin; renewal does not copy downstream pages. [Ephemeris::State](../star/src/ephemeris_state.hpp) combines local origin, two-level projection, total history quota, and source wheel, rereading time after preparation before publication. Runtime registers Ephemeris unary/Watch and C++ Observer is wired; Catalog composition, Comet Beacon/Publisher, and multi-Star recovery are also integrated. Component faults, public RPC, and real-process cases qualify different boundaries; component tests alone do not prove end-to-end results.

Store remains an internal KV primitive without these business structures, authentication, peer replication, or durable recovery. Buffer aliases std::vector<std::uint8_t>; Value is std::shared_ptr<const Buffer>. One state lock protects data/index/history. A separate build lock serializes snapshot Map construction without blocking writers on it. snapshot() briefly captures root/local version, then copies Map/Key outside lock while sharing Values: complete Map still costs O(N) time/memory.

Store version represents this instance's commits only. Removing an absent key does not advance; an expiry batch shares one version. extract(since) extracts complete history batches; gaps/ahead cursors require snapshots. Default 32 MiB copy budget covers Delta arrays/key bytes, not retained bodies, RSS, or wire encoding. Limit/allocation failure may throw.

| Primitive parameter | Meaning |
| --- | --- |
| Clock::Time | Nonnegative int64 Unix nanoseconds |
| optional<Clock::Time> deadline | Empty means permanent; maximum integer is still finite |
| interval | Default 10 ms, strictly positive |
| retention | Default ten minutes, nonnegative local Steady elapsed time; zero retains no delta history, not age-filter disablement |
| capacity | Default 1000 history batches; zero retains none, not a business-key/memory limit |
| initial | No time by default; first tick establishes the boundary, without catch-up from 1970 |
| Timer | Embedded Wheel<4, 10>, four layers of 1024 slots |

With zero capacity/retention, lagging cursors need a snapshot; caught-up extract(version()) still returns an empty stale=false delta. Existing `store_test::test_retention_and_idle_maintenance` covers this contract; this statement does not claim it was rerun.

On target x64, each Store's 4096 slot heads alone imply about 32 KiB, before records/history. This source-layout estimate motivates avoiding a Store/wheel per Scope; it is not measured new-model memory. Existing put does not implicitly advance time. Runtime uses native Ephemeris and Catalog/remote-source state/scheduling, not three wrapper layers over all old containers.

Existing Store/Wheel/Clock tests retain only their original coverage. Native models/source-version/projection relationships have separate [acceptance plans](../docs/comet.md)/business cases. Integrated execution paths do not qualify all scales. Tests require current authorization; [validation.md](../docs/validation.md) is the sole current record.

<a id="reuse"></a>

## Dynamic-domain reuse boundaries

Catalog content versions, post-expiry watermarks, and multisource merge differ from Ephemeris UUID ownership, fixed Attr, and separate Data/Renew orders. Differences affect commit, rollback, deletion, and recovery; two Boolean Traits cannot safely turn both States into aliases.

| Shared mechanism | Implementation and retained distinction |
| --- | --- |
| Native pages, candidate commits, source logs | Pages/Origin/Scene shared; domains choose records/semantics |
| Source recovery/transport | Restore, Borrowing, Dispatch, Landing, Exchange::Pipe share recovery/budgets |
| Public pagination | [Pagination](../star/src/pagination.hpp) shares frozen references, sorting/compaction, page budgets; Encoding/merge remain domain-specific |
| Public reads | [Reading](../star/src/reading.hpp) shares shared fast path, exclusive advance, outside-lock source waits/reclamation |
| Clock, scope directory, accounting | Private [Context](../star/src/context.hpp) shares reading/locate/obtain/allowance/single-item publish; State still owns locks, fields, Pending, business methods |
| SDK Watch | Watching shares network recovery/cancellation/callbacks/accounting; installation retains domain candidate/version rules |
| SDK writes | Publishing/Beaming remain separate: synchronous Catalog commits differ from Beacon registration/renewal/recovery |

Context introduces no new owning object, inheritance layer, runtime domain selector, or moved state locks. Clock order remains gate_ → timing_; invalid readings leave observed_ unchanged. Generic helpers do not take over Catalog batches or Ephemeris owners_. Template reuse is not evidence of faster builds, smaller binaries, or better performance.

## Algorithm and resource boundaries

| Suggestion/question | Current conclusion |
| --- | --- |
| Zero Store retention | [Constructor](src/store.hpp) explicitly means no delta history, consistent with trim. Do not reinterpret it as capacity-only eviction; zero capacity likewise retains none. Later workspace corrections need their own validation |
| Default Scene::Batch move | Not equivalent: owner_ carries rollback responsibility and must be exchanged to nullptr; default pointer move could let the old destructor undo a valid edit |
| Direct Pagination sort | Retain index sort/permutation cycles. Earlier direct Event moves hit GCC Release maybe-uninitialized with warnings-as-errors; no fabricated measured move-cost benefit |
| Pagination tail capacity | resize/move into shared_ptr do not release actual vector capacity; keep charging it. Real shrinking requires allocation/peak analysis, not deleting accounting |
| In-place Dispatch encoding | Retain temporary Entry validation then Swap. RemoveLast retains reusable submessages; soft-budget-rejected capacity must not hide in prepared packets |
| Partial Scene batch replay | upper_bound need not return the first batch item; first->version != first->first rejects history-trimmed batches, including since == first->first - 1 |
| Projection dedup buckets | discard swaps an empty container to release buckets, also cleared on success; records * 2 is a staging bound, not every reset's inevitable peak |
| Exchange dispatch | Explicit body_case switch retains domain validation/rejection/budgets, without reflection |
| Exchange workspace | Acquire new quota, clear old bytes_ + workspace_, then register the new workspace. Guard cleans failure/exception; successful pagination retains it without double return |
| coverage lookup | Combine exact location, maximum Scope coverage, and capacity count in one pass, still O(N), without another rollback-maintained index |
| Directory sorting | principal map order differs from business ID order; container ordering does not justify deleting sort |
| Wheel ceiling division | Quotient plus nonzero remainder avoids unsigned overflow in remaining + width - 1 |
| Bitmap/big-endian encoding | countr_zero locates and bits &= bits - 1 consumes; they complement each other. Portable fixed-eight-byte encoding is not unconditionally replaced by byteswap |

Runtime shutdown's server_ null check is defensive hardening, not a reproduced crash. Resource guards make immediate return/exception boundaries explicit; quota previously returned during disconnected-stream destruction is not a proven permanent leak.
