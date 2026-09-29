# Astrolabe

[English](README.md) | [简体中文](README_CN.md)

Astrolabe is the Go management/live-observation backend for [admin/](../admin/README.md). Its first version supplies login, Almanac administration, and node status; the complete Orrery interface is deferred. The [Go entry](main.go) connects HTTP management, the Polaris proxy, and metrics collection. The old C++ placeholder is no longer built. See [implementation status](../docs/architecture.md#status) and [runtime/browser validation](../docs/validation.md).

## Language and data responsibilities

| Object | Ownership/boundary |
| --- | --- |
| Management account/deployment parameters | Deployment supplies one account and backend access materials; no Astrolabe database |
| Current Almanac/bounded history | Sole durable authority is [Polaris](../polaris/README.md) SQLite; Astrolabe reads/commits through management RPC |
| APIKEY/APISECRET | Internal Almanac, using the same Polaris commit/distribution path; no second credential table |
| Management sessions | Astrolabe process memory only; fixed lifetime under [sessions](#management-sessions) |
| Nodes/metrics | Pulsar directory and actual service observations are sampled separately; bounded latest state in memory, no long-term time series |

Astrolabe introduces no SQLite/PostgreSQL, GORM, file-content source, or KV adapter. It does not save an independent source and then write directly to Star. Polaris alone owns the Almanac baseline; application publishers own dynamic Catalog durability. Observed views never become authoritative writes.

The initial Comet C++ SDK does not depend on completion of Comet Go. A future Go SDK may provide ordinary business views for management, but essential writes, internal credentials, and Star bootstrap use internal protocols directly, without waiting for Go SDK/frontend completion.

## Management admission

Infrastructure retains Pulsar admission/internal TLS with separate Polaris/Astrolabe roles. Polaris accepts Astrolabe management writes; Star accepts Almanac installation only from Polaris. Observation identity cannot bypass Polaris or automatically become a Comet session.

There is no architectural standalone/same-host authentication bypass. All-in-One is a deployment profile using the same startup/admission chain. Comet authentication/TLS switches do not change internal management identity, transport, or `__` Sector boundaries.

Astrolabe registers its reachable backend endpoint, not a browser address, Vite port, or static-site URL. Polaris/Astrolabe join neither Star full mesh, Planet candidates, nor business replica counts. Browser login registers no node. Orbit defines explicit roles/consumer conversions; unknown roles never degrade to Star.

## Management login and Admin integration

The first version authenticates management users without account tiers, read/write roles, Sector/Spectrum ACLs, or per-button permission tables. All authenticated users share the exposed capabilities, still subject to versions, validation, and resource limits. Browser authentication is separate from infrastructure identity; Pulsar accounts, node credentials, and private keys never reach the frontend.

### Management account

Deployment provides one account, without account tables, online CRUD, public signup, or Polaris-hosted management-account Almanac. Startup fully loads/validates configuration; missing/invalid settings cannot enable anonymous access. Account changes require deployment update and Astrolabe restart, invalidating memory sessions without adding hot-reload revocation.

The account contains username/salt/hash. Password hashing follows [Pulsar parameters](../pulsar/README.md#startup-and-existing-materials); configuration stores a digest, never plaintext. Do not import node login.json, Pulsar accounts.json, or Comet APISECRET automatically. Go uses identical hashing/comparison semantics without a C ABI link to C++ identity code.

Bound login input/concurrency; password derivation holds no session-table lock. Ordinary requests check memory sessions only. Browsers receive neither hashes nor service access materials. Login itself requires neither Pulsar nor Polaris online; individual management operations still depend on admission/target readiness. Login success is not dependency readiness.

### Management sessions

Opaque sessions exist only in the current process and expire exactly eight hours after successful issuance. Activity, status requests, and refresh do not extend them. No refresh token, JWT, or session database. Expiry requires login; logout/process restart invalidates corresponding sessions, regardless of a retained browser Cookie.

Generate credentials from reliable randomness and check active-table collisions; never derive from username, timestamps, or counters. Use HttpOnly Cookies, not frontend tokens for localStorage. HTTPS uses Secure. Cookies bind to the backend host without shared Domain; Path, SameSite, and deletion attributes are consistent. Cookie lifetime cannot exceed remaining server lifetime, and every admitted request checks actual server deadline.

Login, total sessions, and cleanup are bounded. Logout invalidates memory state before deleting Cookie and is idempotent. Already-admitted Polaris writes do not roll back on logout, expiry, or page close. Closing one page does not log out other tabs. Sessions are not shared across Astrolabe instances; equal account configuration does not make another instance accept a token. Deployment must pin a backend or require explicit relogin.

### Same-origin and cross-origin deployment

Both same-origin and explicitly configured cross-origin browsers are supported. Configure Admin's backend URL separately from allowed frontend Origins. Match normalized scheme/host/port exactly, never suffixes, arbitrary reflection, or `*` for credentialed requests. `null` Origin is untrusted. Same-origin proxies and cross-origin connections share login/business APIs.

Cross-origin clients use `credentials: include`. For allowed Origins only, return matching Access-Control-Allow-Origin, Access-Control-Allow-Credentials: true, and Vary: Origin, with explicit method/header allowlists. Allowed-origin error responses also carry CORS headers. OPTIONS needs no login Cookie, checks only origin/method/headers, causes no management side effect, and proves no login. See the [Fetch standard](https://fetch.spec.whatwg.org/#cors-protocol-and-credentials).

Cross-origin differs from cross-site. Same-site deployments may use SameSite=Lax; cross-site Cookies explicitly require SameSite=None; Secure over HTTPS. Browsers may still block third-party Cookies; CORS cannot bypass that policy. Report session establishment failure and use same-site domains/proxies if appropriate, never silently switch to URL tokens, localStorage, or anonymous access. See [Cookie attributes](https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Set-Cookie) and [browser policy](https://developer.mozilla.org/en-US/docs/Web/HTTP/Guides/CORS#third-party_cookies).

Writes, login, and logout validate Origin and require a nonsimple header before side effects, independently of whether browsers can read responses. Missing/disallowed Origin rejects these browser writes. GET/HEAD do not mutate. Origin checking does not replace Cookie authentication. Trust proxy forwarding only when explicitly configured, not arbitrary Host/Forwarded values. Interface definitions fix concrete headers/fields.

## Almanac editing and distribution

### Content source

Polaris owns the authoritative baseline; Astrolabe reads an explicit version for editing. Star views observe installation, never supply a new Polaris authority baseline. Missing/lagging/unreachable Stars cannot imply Delete; UI search, pagination, or failed reads cannot clear a Scope.

Management retains single-key Set/Delete and accepts an atomic POST `changes` array for unique keys in one Scope. Entries are `{key, value}` with Base64 or `{key, erase:true}`; outer sector/spectrum/version are shared and mutually exclusive with old single-key fields. Almanac imposes no batch key-count/total-byte cap and no separate HTTP batch-body cap. Individual values retain 1 MiB limits; configured storage capacity/request deadlines still apply. Astrolabe pages through `Authority.Batch`, committing once after completion plus normal EOF. Empty Buffer differs from Delete; internal credentials do not expose batching. Admin UI still edits one key; batching is available through HTTP API. No file/database/KV import adapters or dynamic Catalog business persistence are added.

### Saving and publishing

Management calls Polaris synchronously and returns “committed” only after durable confirmation. Polaris enqueue, Star memory installation, and Watch observation do not replace it. Report durable position separately from each Star's installation; a valid commit does not wait for every Star.

Concurrent edits obey group-version conditions even with one Polaris process. Report conflicts for caller reread/decision; do not substitute latest version and resend stale edits. Internal protocol defines requests/receipts.

Timeout, disconnect, or Astrolabe restart may follow Polaris commit, so uncertain is not uncommitted. Do not roll back confirmed data, maintain another durable task queue, or republish through Watch callbacks. Only valid evidence for the original operation confirms its outcome; equal current content/version alone is insufficient.

Any definite atomic-batch failure rolls back everything; uncertain outcomes require the complete request for confirmation. For independent single-key sequences, conflict/failure/uncertainty stops later operations in that round while preserving confirmed prefixes. Later successful edits do not rewrite an earlier uncertain outcome. Bounded Polaris history does not permanently retain every retry proof, and no arbitrary-version rollback API exists.

### Star bootstrap recovery

Star receives a complete version-preserving Almanac baseline from Polaris, including valid empty sets/internal credentials. Bootstrap waits for neither browser login, an open Astrolabe page, nor Comet Go subscription. [Architecture](../docs/architecture.md#startup-and-operation) owns startup/degradation ordering.

An empty Polaris still explicitly completes its baseline; failed reads are not empty databases. With authentication enabled and no Comet credentials, business login fails. Astrolabe can install initial credentials through independent internal identity. Star need not expose public `__` access to break a bootstrap cycle.

## Comet credential management

APIKEY/APISECRET belong to `Almanac["__auth"]["comet"]`, one complete credential per APIKEY, committed durably by Polaris then distributed. There are no Grants, per-Scope permissions, or registration owners. Authenticated Comet can access all ordinary business data; Star's external entry still rejects `__` Sectors.

Management validates internal credential structure; ordinary empty Buffer legality does not make required credential fields optional. Star atomically installs records, version, login index, and required session invalidation before ACK. Only APIKEY lookup is needed, not domain/Sector/Spectrum permission bitmaps. [Protocol](../proto/README.md#credentials) owns fields/capacity.

### Credential lifecycle

Deployed APIKEY/APISECRET remain valid until explicit rotation/revocation, without automatic expiry/periodic login. Each APIKEY has one active SECRET, no overlap window. Contiguous installation of a new SECRET or deletion invalidates old Sessions and associated subscriptions. Same-value contiguous updates and replay at the same installed version do not incorrectly close connections.

Polaris durability and Star installation are not simultaneous. Unupdated Stars cannot promise simultaneous revocation, and sent data cannot be recalled. During disconnection, Stars use installed credentials without anonymous fallback or per-request Polaris lookup. Protocol closes rotation/session-check races; removing per-Scope permissions does not permit stale Sessions.

Deleting/recreating an APIKEY allows new login only, not old-session resurrection, version/order reset, or renewed full TTL. APIKEY is neither Ephemeris ownership nor Catalog authority. SDK objects recover under original identity/deadlines/targets; a reused credential name does not authorize arbitrary UUID takeover.

When a Star skips intermediate history and installs a newer complete credential snapshot, all its old Comet Sessions must reauthenticate, even if final SECRET is unchanged. This accepted recovery cost keeps Credential secret-only without per-account identity markers. [Credential snapshots](../proto/README.md#credential-snapshot) alone define installation atomicity, duplicate snapshots, and SDK recovery.

Deployment distributes APISECRET to applications. Comet gets no credential table and queries neither Astrolabe nor Pulsar for SECRET. Local SDK SECRET replacement does not update Polaris. Service passwords, hashes, private keys, APISECRET, and session tokens belong in neither logs, URLs, CLI, errors, metrics, nor ordinary frontend topology responses.

## Live observation

Astrolabe caches a bounded [read-only Pulsar directory](../proto/README.md#directory), then independently observes real connection/readiness/synchronization state. Browser refresh does not initiate a new Pulsar query. Registration is not online status; failed scrape is not confirmed exit. Observations include sampling time and success/unknown/stale state. One failure never automatically deletes membership, rewrites Almanac, or switches authority.

Star implements independent read-only HTTP GET `/metrics` in Prometheus text format, directly scraped by Astrolabe. The first version deploys/requires neither Prometheus nor Grafana, stores no long-term series, and adds no private metrics gRPC. Standard formatting permits future integrations without expanding current scope.

Metrics are disabled by default and require explicit listening configuration. Use loopback locally and protected networks/proxies across hosts. Service registration/resource budgets are separate from business/internal gRPC. Disabling Comet authentication/TLS cannot widen metrics exposure. This endpoint carries neither management writes nor sensitive dumps.

Eight fixed state gauges are currently exposed, not request counters/fixed-bucket histograms. Labels use bounded method/state/role sets, never APIKEY, UUID, Key, or arbitrary Sector/Spectrum. Event-maintained metrics do not scan every group per scrape or retain per-request samples. Scrape concurrency/buffers/waits are bounded. Every trusted-directory Star is covered; individual results publish as they complete, but round duration varies with count/latency. Business-accounted bytes are not RSS.

Admin consumes results through an adapter. Three.js owns no infrastructure credentials/recovery. Live backend and demo modes are explicit; unavailable/stale data never silently becomes demo success. Essential management APIs and complete map UI are separate deliveries.

## Implementation and acceptance boundaries

One management account, eight-hour memory sessions, origin rules, Polaris durability, and direct observation are confirmed. Interfaces follow below; scope is not expanded into an account platform, universal importer, or external monitoring integration. [Acceptance](../docs/comet.md) follows current domains/contracts. Old Astrolabe database, direct-Star writes, Grant, and standalone cases are not new-feature acceptance. HTTP, real-process, and browser validation have baselines; current evidence/unverified changes live only in [validation](../docs/validation.md).

## HTTP API and current parameters

Parameters include `--listen=IP:PORT`, optional `--advertise`, `--super=IP:PORT`, `--galaxy`, `--group`, `--identity=directory`, `--account=digest-file`, and `--public=https://management-api-origin`. Management defaults to TLS 1.3 with deployed certificates. `--http` permits loopback only for a separately configured reverse proxy. `--public` determines Cookie Secure without trusting arbitrary Forwarded headers. `--origins` adds comma-separated browser origins; explicit `--crosssite` requires an HTTPS public origin. Memory sessions default to at most 4096, reducible with `--sessions`.

All mutations require allowed `Origin` and `X-Astra-Request: 1`. JSON uses `application/json`, rejecting duplicate/unknown fields. Authenticated reads use the same `astra-session` Cookie. Management versions are decimal strings, never JavaScript Number conversions. Writes are not automatically retried; `effect` is `committed`, `unapplied`, or `unknown`.

| Method/path | Request/result |
| --- | --- |
| `POST /api/session` | username/password; success sets HttpOnly Cookie and returns username/expires, never a token body |
| `GET /api/session` | Current username/fixed expiry, without renewal |
| `DELETE /api/session` | Idempotent logout/deletion of the same-scope Cookie |
| `GET /api/metrics` | Latest samples: instance ID, sample/attempt time, stale flag, fixed numeric strings; no request-triggered scraping |
| `GET /api/nodes` | Redacted cached members and observed/stale, without online claims, principal, passwords, or signing materials |
| `GET /api/almanac` | Complete Polaris group inventory: sector/spectrum/version |
| `GET /api/almanac?sector=...&spectrum=...` | One-group NDJSON snapshot: key/value rows with Base64 values, then complete/position |
| `POST /api/almanac` | sector/spectrum/version plus single key and Base64 value or erase=true; alternatively the mutually exclusive `changes` batch described above. Success position comes only from Polaris durability |

Valid empty value is `""`, not deletion. Strict Polaris version rules apply to every commit. Astrolabe retains no second complete baseline: receive pages, emit rows, and emit final `complete: true` only after normal source gRPC termination. HTTP closure, error rows, or missing completion mean incomplete snapshot; consumers discard staging instead of clearing old views. Internal credential Buffers encode [Credential](../proto/README.md#credentials), never reinterpret ordinary settings as default credentials.

Initial limits are four concurrent KDFs, sixteen ordinary management requests, and two full snapshot streams. Password work holds no session lock. Mutation RPC deadline is five seconds; total snapshot reception is thirty seconds; slow HTTP writes have separate server deadlines. These are engineering defaults without measured throughput/tails/RSS qualification.

`POST /api/credentials` accepts key, positive decimal-string version, and either Base64 secret or erase:true. Astrolabe encodes internal Credential; browsers need no Protobuf handling. `GET /api/credentials` streams APIKEY/redacted:true rows plus final completion/version. Generic Almanac reads of `__auth/comet` also redact, never exposing stored SECRET through another URL. Ordinary scopes return Base64 values.

Optional `--metrics=metrics.json` explicitly maps `{ "Star-internal-IP:port": "http://metrics-IP:port/metrics" }`. There is no separate 64-node collection cap. The file has an 8 MiB malformed-input bound; all configured trusted Stars participate. Responses cover every directory Star, retaining unconfigured/uncollected/replaced instances with empty values and “not collected,” rather than hiding them. Frontend capacity follows the full directory, without truncating at 64.

HTTPS protected proxies may use the deployment CA. Scraping follows no redirects/environment proxies and sends no node bearer. Four workers, two-second requests, and 64 KiB per-endpoint bodies bound work. Idle pool capacity follows target count, not concurrent request count. A five-second timer follows each completed round; arbitrary-size rounds are not promised within five seconds. Installation checks an endpoint/instance index published at the same directory boundary to reject late old-instance results. Failure retains same-instance old values marked stale; no successful scrape for fifteen seconds also marks stale.

Orbit Member currently advertises only internal gRPC endpoints, not metrics addresses. Automatic metrics discovery is unimplemented: use explicit mapping, never adjacent-port guesses or unknown-address scans. Pulsar/Polaris currently have directory observation only; do not invent Star metrics for them.

Star generates at most one fixed snapshot per second without business-lock access during scraping. If its control loop has not updated for ten seconds, it refuses stale success. Gauges are astra_ready, astra_almanac_ready, astra_clock_ready, astra_clock_synchronized, astra_clock_uncertainty_nanoseconds, astra_members, astra_sessions, and astra_recovery_bytes. Recovery bytes are logical accounting, not RSS; global Almanac ready is not per-Scope installed version.
