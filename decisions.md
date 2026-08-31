# Verdandi Foundation Decision Docket

## 1. Document Role

This document turns the unresolved foundation and protocol questions into a
reviewable decision docket. It records recommendations and their consequences;
it is not a source of accepted requirements.

Decision states are:

- **proposed:** ready for maintainer review but not approved;
- **accepted:** explicitly approved by the maintainer and copied into the
  owning contract document;
- **rejected:** considered and declined, with the replacement linked; and
- **deferred:** intentionally outside the current release.

Except for the explicit maintainer directions in the next section, every item
below is currently **proposed**. No implementation may treat a recommendation
in this file as frozen. Accepted project-level decisions move to
[`codex.md`](codex.md); version requirements move to [`alpha.md`](alpha.md);
wire and storage rules move to [`protocol.md`](protocol.md).

## 2. Accepted Maintainer Directions

The following directions were accepted on 2026-08-21 and 2026-08-22 and are
reflected in the owning documents:

- Reuse the Redis lease, exact-token, callback-lifecycle, and recovery
  invariants exercised by Hermes, without inheriting Hermes Primary's required
  service-Registration dependency.
- Add persistent Catalog KV synchronization.
- Do not encode a maximum service, Node, or Campaign count. Catalog is one
  complete Path value rather than a population of protocol records.
  Replace Hermes's bounded full-domain Lua scans with paginated membership,
  subscribe-before-scan recovery, bounded event buffers, and byte-based local
  resource budgets.
- Use Redis Open Source 8.0 or later as the Alpha baseline and store Campaign
  readiness as independently expiring Hash fields.
- Call one Node's leased record a `Registration`, call its Zone/Type collection
  a `Registry`, and store each Registration in an independent Redis
  Hash with key TTL and typed partial field updates.
- Use Redis-native data structures and explicit field contracts for ordinary
  coordination state instead of a universal CDDL/deterministic-CBOR envelope.
  SDKs retain the known fields they require; ACLs and protocol-owned Lua guard
  supported writes.
- Store one persistent raw Catalog Value per Catalog name. The SDK splits a
  logical publication into bounded independent field Patches; each changed
  Patch or whole Delete changes the Hash revision and appends its exact Stream
  delta atomically.
- Start the Go and Rust SDKs at `1.0.0`. Keep source version metadata fixed
  before formal release and publish no mutable `1.0.0` artifact or tag during
  development. The first protocol version is `1.0`.
- Generate one fresh Registration UUID in the SDK on every process start. Use
  `verdandi:registration:<zone>:<type>:<uuid>`, let crashes expire by TTL, and
  remove the exact UUID through the Registry mutation on graceful shutdown.
  Do not require a separate stable Node ID plus generation ID.
- Construct and mutate all protocol keys inside the SDK. Applications use typed
  Verdandi APIs and never receive Redis clients or update protocol Hashes
  directly.
- Do not model Publisher or Catalog write authority as a protocol term. Every
  changed mutation advances the Redis-owned revision for its target; Catalog
  appends the matching Stream delta. Generic Leader election remains independent
  from publication.
- Protocol version `1.0` has no protocol-capability field or negotiation. Every
  `1.0` behavior is mandatory; Registration `service_capabilities` remain
  application/service discovery metadata.
- Keep Redis key names forward-compatible and unversioned. Existing key data
  types and meanings do not change in place; compatible evolution adds optional
  fields or new keys.
- Use Redis authentication, ACLs, and TLS trust boundaries instead of
  end-to-end signatures, including for Catalog, desired state, and commands.
  ACL-authorized raw writes are outside the protocol guarantee.
- The application supplies `zone` through SDK configuration and the SDK
  validates it as 1 through 32 ASCII letters. The SDK generates each
  process-start Registration UUID as exactly 32 lowercase hexadecimal
  characters without separators.
- Store Registrations and complete Catalog Values as independent Redis Hashes.
  A Registration exposes Redis/Lua-managed `Meta`, immutable SDK-supplied `Attr`,
  and mutable fixed-structure `Data`: Meta uses `@name`, Attr uses `.name`, and
  Data uses an unprefixed `name`. Every top-level leaf remains an independent
  Hash field.
- Candidate `version` is a protocol-defined positive safe integer. Every SDK
  uses the same numeric comparison, larger versions are preferred, and equal
  versions are first-successful-claim wins. Redis validates leases and exact
  ownership but does not centrally search for the highest candidate.
- Use Redis 8 Hash-field TTL for Registry membership and Campaign readiness so
  natural expiry removes those fields without a separate stale-order index.
- Catalog accepts multiple ACL-authorized writers and bounded independent
  last-write-wins Patches. Redis-primary execution order, not client wall time,
  determines which overlapping mutation is later.
- Redis retains current Registry state without history. Registry uses
  per-Registration revisions and subscribe/scan/PING recovery. Catalog retains
  bounded delete identities and per-field revisions for authoritative delta
  repair; metrics, long-term history, and audit storage belong to a separate
  statistics/synchronizer service, not the core SDK.
- Provision ACLs by role and Zone rather than by individual Registration
  UUID.
- Publish only `README.md` from the current Markdown set. License the project
  under MIT, with `LICENSE` as the public license artifact.
- Qualify separate profiles of 500 live Proxy Registrations, each renewing or
  updating once per second, plus 10 Catalog mutations per second. This is a
  measured baseline, not a count limit.
- Use `@` for Redis/Lua-managed Meta fields, `.` for immutable Attr, `&` for
  event controls, and no prefix for Data. Use the accepted common ASCII form
  for Type/Catalog/election identifiers. Catalog identity is exactly one
  bounded Part/ID Path and has no hashed token.
- Accept the protocol resource maxima, configurable SDK limits, Registry
  subscribe/scan/PING and Catalog Hash/ZSET/Read recovery, `fred` qualification route, and
  p95/p99/failover/recovery capacity gates.
- Seed shared Registration defaults into `verdandi:config:<zone>` during Client
  bootstrap and allow an authorized backend to change them later. Clients keep
  a last-valid local snapshot and adopt valid changes through periodic or
  explicit refresh without reading configuration on every Update.

These directions fix product scope and architecture. Exact Attr/Data value
codec, event-queue coalescing, Sentinel qualification, event encoding
benchmarks, and the cross-language shape of the mandatory Sentinel fence
adapter remain review items below.

Reference review used the actual Hermes Service and Primary implementations,
not only their overview text:

- [`SERVICE.md`](../hermes/go/core/redis/SERVICE.md) and
  [`service.lua`](../hermes/go/core/redis/service.lua) for leased per-generation
  records, revisioned wake events, subscribe-before-snapshot recovery, and the
  bounded full-snapshot limitation that Verdandi must replace;
- [`PRIMARY.md`](../hermes/go/core/redis/PRIMARY.md),
  [`primary.lua`](../hermes/go/core/redis/primary.lua), and
  [`primary_term.go`](../hermes/go/core/redis/primary_term.go) for separate
  readiness/ownership tokens, highest-ready-version retirement, exact release,
  release-only wake, and synchronous local term validity; and
- [`KV_DESIGN.md`](../hermes/go/core/redis/KV_DESIGN.md) for the proposed CAS,
  synchronized-view, and stale-view contract. Hermes KV is a reviewed design,
  not a production implementation, so Verdandi still requires its own vectors
  and integration evidence.

## 3. Recommended Review Order

Review decisions in this order because later choices depend on earlier ones:

1. FND-001 through FND-004 establish repository and release policy.
2. PRT-001 through PRT-004 establish encoding, versioning, identifiers, and
   the ACL trust boundary.
3. PRT-005 through PRT-008 establish Redis layout, Leader fencing, leases, and
   acknowledgements.
4. PRT-009 through PRT-014 establish limits, commands, Redis qualification,
   executable conformance evidence, scalable discovery, and Catalog KV.
5. SDK dependency and package-name choices follow only after the protocol
   items are accepted.

## 4. Foundation Decisions

### FND-001: First version

- **Accepted direction:** the first Go and Rust SDK versions are `1.0.0`.
- **Pre-release rule:** source metadata may remain fixed at `1.0.0` while the
  repository is unpublished. Do not publish, tag, or overwrite mutable
  `1.0.0` package artifacts during development; the first published `1.0.0`
  artifact is immutable.
- **Reason:** the maintainer wants the first supported SDK release to begin at
  `1.0.0`. SDK semantic version and protocol compatibility remain separate even
  though both first versions begin with 1.

### FND-002: Public Markdown policy

- **Accepted direction:** keep the current `main` allowlist of `README.md` only
  until schemas and generated reference documentation exist. Revisit the rule
  before the first public protocol release rather than publishing the local
  planning documents.
- **Reason:** the present documents mix durable design with internal planning
  and unresolved alternatives.
- **Consequence:** release preparation must select public artifacts from a
  clean `main` work line and must not merge local `alpha` wholesale.

### FND-003: Internal document retention

- **Accepted direction:** keep `codex.md`, `alpha.md`, `architecture.md`,
  `protocol.md`, `coding.md`, `decisions.md`, and `worklog.md` local-only until
  each document is either replaced by a public artifact or explicitly
  approved for publication.
- **Reason:** this preserves onboarding and decision history without silently
  exposing internal development context.

### FND-004: License

- **Accepted direction:** license Verdandi under the MIT License. Use SPDX
  identifier `MIT` in package metadata and keep the complete text in
  `LICENSE`.
- **Copyright line:** `Copyright (c) 2026 LaconisIves`.

## 5. Protocol Decisions

### PRT-001: Redis-native field contracts

- **Accepted direction:** do not impose one CDDL/deterministic-CBOR envelope on
  ordinary Redis coordination state. Registration, Registry, election, ACK,
  and Catalog metadata use protocol-defined Redis data
  structures, Hash fields, scalar encodings, and Lua actions directly.
- **SDK rule:** an SDK reads and retains the fields required by its supported
  behavior and ignores unknown optional Redis Hash fields. Protocol `1.0` has
  no protocol-capability metadata or negotiation; every defined behavior is
  mandatory.
- **Value rule:** desired-state payloads remain opaque bytes. A Registration
  exposes typed `Meta`, `Attr`, and `Data` views while storing every top-level leaf as an
  independent top-level Hash field. Catalog custom values are likewise
  expanded. Verdandi treats each application value as bounded bytes; an
  application codec may map a typed structure to that flat field map.
- **Validation rule:** every owned field still has an exact name, Redis type,
  required/optional status, scalar byte encoding, length/range, mutation owner,
  and stable error behavior. This is an interoperability contract, not a
  universal serialized message envelope.
- **Security boundary:** correctly provisioned ACLs and protocol-owned Lua/SDK
  actions enforce ordinary writes. A principal deliberately granted raw write
  access can violate stored invariants and is outside the protocol guarantee;
  CDDL or canonical CBOR would not prevent that bypass.
- **Security rule:** Redis authentication, ACLs, and transport security define
  write permission. Verdandi does not add end-to-end signatures to ordinary
  state, Catalog values, desired state, or commands.

### PRT-002: Protocol version and compatibility window

- **Accepted direction:** encode `protocol_major` and `protocol_minor` as bounded
  unsigned integers. The first protocol writes `1.0`; this is
  independent of the SDK package version `1.0.0`.
- **Major meaning:** changing `protocol_major` means an incompatible contract,
  such as changing an existing field's type or meaning. A reader rejects an
  unsupported major before interpreting class-specific fields.
- **Minor meaning:** changing `protocol_minor` permits only backward-compatible
  additions, such as a new optional field. It may not change the meaning,
  type, ownership, or validation of an existing field. Same-major readers
  ignore unknown optional fields.
- **Accepted capability rule:** protocol `1.0` carries no protocol capability
  list. Every `1.0` behavior is mandatory. Registration discovery capabilities
  are application/service metadata named `service_capabilities`, not protocol
  negotiation. A future optional protocol feature requires a new explicit
  maintainer decision.
- **Read rule:** accept a supported major. A future same-major minor may add
  only optional fields or new independent keys that an older reader can ignore;
  any new mandatory behavior is a major change.
- **Write rule:** emit exactly one configured protocol version; never select a
  wire version from an SDK package version.
- **Key compatibility rule:** Redis key names, existing key data types, and
  existing field meanings are forward-compatible and do not change in place.
  Compatible evolution adds optional fields or new keys. If an incompatible
  storage change ever becomes unavoidable, it requires an explicitly new key
  family and migration rather than silently reinterpreting an existing key.
- **Reason:** major identifies incompatible protocol behavior and minor records
  additive storage evolution. Capability negotiation is unnecessary while all
  supported behavior is mandatory.

### PRT-003: Identifier and revision representation

- **Accepted Zone rule:** the application supplies `zone` through
  SDK configuration. The SDK does not invent it because every participant in
  the administrative domain must use the same value. Its canonical form is
  case-sensitive `[A-Za-z]{1,32}`. An SDK configuration may impose a lower
  limit but cannot accept a wider protocol form.
- **Accepted Registration identity:** the SDK generates a fresh independent
  128-bit UUID for every service process start. Its canonical key text is
  exactly 32 lowercase hexadecimal characters `[0-9a-f]{32}` with no hyphens,
  stored as `uuid` and used in the Registration key. A crash leaves
  the key to expire; graceful shutdown removes only that exact Registration.
- **Generation rule:** a separate stable `node_id` plus `generation_id` pair is
  not required. The per-start `uuid` already identifies the process
  instance and is its fencing boundary. A future stable machine or business
  identity, if needed, is a separate optional application field and must not
  replace the Registration UUID.
- **Accepted remaining text identities:** case-sensitive `type`, `catalog`,
  and `election_domain_id` use
  `[A-Za-z][A-Za-z0-9_.-]{0,63}`. They are 1 through 64 ASCII bytes, contain no
  Redis separator or ACL glob character, and may therefore appear directly as
  one key segment without escaping. SDK configuration may impose a lower
  length. Desired targets use their accepted typed tuple rather than another
  free-form identifier.
- **Superseded Catalog business key:** the former opaque business-key/hash-token
  dimension is removed. A Catalog identifier directly names its one Value.
- **Revision rule:** each Registration owns an SDK-retained per-UUID revision.
  Each Catalog, desired target, election, and ACK scope owns a Redis-stored
  revision. Catalog revision starts at 1, advances only for changed mutations,
  never resets or wraps, and is limited to `9223372036854775807`; its Lua uses
  canonical decimal string operations. Other numeric domains retain their
  separately defined safe-integer bounds.
- **Reason:** two simultaneously live starts are two legitimate Registrations,
  so neither overwrites the other. Per-UUID revisions order Registration state;
  Redis-owned revisions order shared-state
  rewrites without introducing a Publisher authority identity. A 32-character
  hexadecimal UUID costs only 10 text bytes more than a 22-character base64url
  form per occurrence; at the accepted population this is negligible compared
  with Hash/key overhead and is worth the universal tooling and debugging form.

### PRT-004: Hashes and ACL trust boundary

- **Accepted direction:** do not sign desired configuration, commands,
  Registration state, acknowledgements, or Catalog Values. There are no
  Verdandi signing keys, public-key trust bundles, key IDs, revocation lists,
  COSE envelopes, or signature-rotation rules.
- **Authorization:** Redis authentication and ACLs decide which principals may
  read or mutate each role's state. TLS remains required when Redis or Sentinel
  traffic crosses an untrusted network boundary.
- **Integrity:** keep SHA-256 chunk and aggregate content hashes to detect
  incomplete assembly or accidental corruption. A hash is not proof of writer
  identity because an authorized writer can replace both bytes and hash.
- **Validation:** SDKs still reject malformed types, sizes, targets, hashes,
  revisions and expiry metadata before activation. This protects
  process correctness; it does not claim to resist a malicious principal that
  has been granted raw write access.
- **Raw-write boundary:** direct Redis mutation can bypass any SDK contract and
  is outside Verdandi's guarantee. ACL provisioning is therefore the security
  boundary, while SDK/Lua operations are the supported consistency boundary.
- **Reason:** the deployment trusts principals that hold write permission. An
  end-to-end signature checked by the same trusted deployment would add key
  distribution and rotation without preventing an ACL-authorized raw rewrite.

### PRT-005: Redis key namespace and ownership

- **Accepted prefix and Registration key:** use the literal product prefix
  `verdandi:` and do not embed the protocol major in it. One Registration is
  exactly `verdandi:registration:<zone>:<type>:<uuid>`.
- **Layout rule:** the data class follows `verdandi:` and the administrative
  Zone follows the data class. The remaining identifier alphabets and
  escaping rules must be frozen before implementation. Proposed related shapes
  are:

```text
verdandi:config:<zone>

verdandi:registration:<zone>:<type>:<uuid>
verdandi:registry:<zone>:<type>
verdandi:ack:<zone>:<uuid>

verdandi:catalog:<zone>:@meta
verdandi:catalog:<zone>:@live
verdandi:catalog:<zone>:@deleted
verdandi:catalog:<zone>:@deleted_time
verdandi:catalog:<zone>:<part>:<id>
verdandi:catalog:<zone>:<part>:<id>:@field_revisions

verdandi:election:<zone>:<domain>:leader
verdandi:election:<zone>:<domain>:revision
verdandi:election:<zone>:<domain>:ready

verdandi:desired:<zone>:<target-hash>:current
verdandi:desired:<zone>:<target-hash>:manifest:<revision>
verdandi:desired:<zone>:<target-hash>:chunk:<revision>:<index>
```

- **Target hash:** `target-hash` is lowercase base32hex SHA-256 over the
  canonical encoded target tuple. The manifest contains and validates the
  complete target, so a key lookup never establishes target authority.
- **Catalog identity:** one direct bounded `part` plus `id` identifies the Path.
  Catalog v1 has no hashed or opaque business-key token.
- **Accepted desired target model:** `zone` is always implicit in the
  key scope. A typed target is exactly one of `partition(partition_id)`,
  `service(partition_id, service_id)`, or
  `registration(registration_id)`. It describes which consumers may activate
  a desired document; it is not a Redis server address. The SDK constructs the
  canonical tuple and target hash, and consumers reject a document whose full
  embedded target does not match their identity.
- **Wake channels:** Registry uses its Hash key text directly as the Pub/Sub
  channel. Other data classes may use the same prefix and target through a
  separate `:wake` suffix. ACL fixtures explicitly reset channel permissions
  and grant only required patterns.
- **SDK ownership:** applications provide typed Zone/Type
  values and application payloads; the SDK generates the startup UUID,
  constructs every Redis key, and performs all Redis commands. Applications
  never receive a Redis client or construct/mutate protocol keys directly.
- **Configuration ownership:** Client bootstrap fills missing defaults in
  `verdandi:config:<zone>` with `HSETNX`; an authorized administrative backend
  may later change the complete policy as specified by PRT-015. Applications
  receive no raw configuration-write API.
- **Write rule:** immutable desired chunks and manifests use `SET NX` semantics
  and reject conflicting existing bytes. Registration Hashes, Registry indexes,
  Catalog Hashes/ZSETs, Leader state, current pointers, and ACKs change only
  through protocol-owned Lua. No Lua mutation scans a full
  Registry, Catalog, or election domain.
- **Accepted Desired concurrency:** use the same Redis-order last-write-wins
  model selected for Catalog. Every Publisher reserves a scope revision, and
  the highest successfully installed current revision remains current.
- **Forward-compatibility rule:** existing key names, Redis data types, and
  meanings do not change in place. Compatible protocol evolution adds optional
  fields or new keys while old SDKs continue to interpret existing keys.
- **Reason:** `vd` was only an abbreviation and `1` was a proposed key-layout
  major. The maintainer chose one readable, forward-compatible prefix. SDK
  ownership is required because one Registration mutation must also update
  Registry membership, matching expiry, and event state.

- **Accepted Registration key grouping:** keep
  `verdandi:registration:<zone>:<type>:<uuid>`. Grouping belongs to
  `verdandi:registry:<zone>:<type>`, so no database-wide `SCAN MATCH` is needed.

- **Accepted Registration field partition:** every Registration is one Redis
  Hash with Redis/Lua-managed Meta `@name`, immutable SDK Attr `.name`, and
  mutable Data `name`. Neither Attr nor Data is stored as JSON; every top-level
  leaf remains independently patchable with `HSET`.
- **Accepted ownership rule:** applications cannot patch `@` Meta or `.Attr`
  through the Data API. A Data field name may not begin with `&`, `@`, or `.`,
  so stored fields and event controls remain disjoint.
- **Proposed suffix form:** the name after `@` or `.`, and every top-level Data
  name, is case-sensitive `[A-Za-z][A-Za-z0-9_.-]{0,63}`. SDKs expose or
  generate an explicit flat field codec; they do not independently reflect and
  flatten nested Go/Rust objects because language-specific reflection rules
  could produce different Redis fields. Cross-language applications own
  matching codec vectors.
- **Catalog rule:** Catalog protocol fields continue to use `@name`; ordinary
  Catalog value fields are unprefixed. Registration Attr does not imply a
  Catalog Attr class.
- **Cross-language API rule:** storage prefixes are not language member names.
  Public records expose ordinary `Meta`, `Attr`, and `Data` members; language
  tags, generated descriptors, or an explicit codec map them to Redis fields.

### PRT-006: Campaign and Leader term

- **Accepted direction:** implement generic leased, SDK-driven,
  version-aware election using an independently identified Campaign, private
  readiness token, and separate private ownership token. Unlike Hermes Primary,
  Verdandi's generic Leader does not require a live service Registration. The
  SDK owns candidate-view synchronization, comparison, Claim decisions,
  retirement, renewal, and application lifetime. Redis owns bounded atomic
  eligibility and exact-owner transitions; Lua never scans or sorts the
  complete candidate population.
- **Accepted version policy:** each Campaign carries a positive integer
  `version` in the protocol-wide exact range `1..9007199254740991`. It is
  immutable for that Campaign lifetime. Every SDK compares it numerically and
  prefers the larger value. Changing version requires closing the Campaign and
  creating a fresh readiness token/version pair; there is no public Campaign
  ID, in-place version mutation, version revision, application comparator, or
  `version_contract_id`.
- **Claim policy:** the SDK maintains the ready-candidate view, applies the
  fixed numeric comparison, and claims only when it considers itself the best
  ready candidate. Redis atomically verifies the exact readiness token and
  immutable version plus the empty Leader key.
  Equal versions are first-successful-claim wins. Because comparison is
  client-side, preference is convergent rather than an atomic strongest-version
  guarantee: a stale client can win temporarily, then retires after observing a
  preferred ready version.
- **Readiness layout:** store one independently expiring Redis 8 Hash field per
  readiness token. The private field token is the Campaign lifetime's internal
  identity and its bounded value contains the immutable integer version. SDKs
  discover candidates with bounded
  `HSCAN` pages and revision/wake recovery. There is no Redis version-ordered
  index and therefore no stale version-index cleanup problem. No
  candidate-count limit is permitted.
- **Retirement:** a current Leader is not asynchronously deleted by another
  candidate. When its SDK observes a preferred ready version, it closes local
  admission, cancels and joins term-owned work, then exact-token releases. The
  business observer receives the same candidate-view update and may make its
  own application decision before attempting another claim.
- **Publication separation:** Publisher, desired-state, and Catalog mutations
  do not carry or validate a Leader/write-authority term. Each accepted
  mutation atomically advances the Redis-owned revision for its target and
  publishes a wake. An application may independently use generic election to
  coordinate controller work, but publication correctness does not depend on
  that Leader token.
- **Local fence:** every SDK term has a synchronous validity check backed by
  cancellation and a conservative monotonic deadline. Failed or ambiguous
  renewal clears validity and never reuses the old token.
- **Zero-or-one policy:** one election domain has zero or one
  application-active Leader. Handoff, renewal uncertainty, callback cleanup,
  lease expiry, and fencing may leave it without a Leader. No supported mode
  accepts overlapping active terms to improve availability.
- **Failover boundary:** Standalone uses its one configured Redis primary as
  the term authority. Sentinel asynchronous replication cannot prove that an
  old primary stopped accepting renewals after promotion. A Sentinel Campaign
  therefore requires one deployment-provided durable fence or advisory lock
  acquired after Redis claim and before application callback admission. It is
  held through invalidation and joined cleanup. While fence acquisition is
  pending the SDK renews the Redis claim within bounded limits; after
  acquisition it exact-token confirms Redis ownership again before creating the
  LeaderTerm. Failed confirmation releases the fence without invoking
  application code. Without that fence, Sentinel leadership remains
  fail-closed and no LeaderTerm is exposed. The fence authority must be
  independent from the same Sentinel replication history; its uncertainty
  invalidates the term, and a leased fence contributes to the conservative
  local admission deadline.
- **Accepted release event:** exact-token release emits one bounded latency-only
  wake. Claim and renew do not. Lease expiry and retry remain the correctness
  path when the event is missed.

### PRT-007: Lease and time calculations

- **Accepted encoding:** encode durations as positive integer milliseconds and
  signed wall times as Unix milliseconds. Checked arithmetic is mandatory.
- **Registration clock:** each Redis connection generation calibrates a
  RedisClock by sampling local monotonic time around Redis `TIME`, includes the
  measured round trip and configured uncertainty in a conservative upper Redis
  time, recalibrates periodically, and resets on primary/connection change.
- **Registration deadline:** Lua stores `@timestamp` and `@ttl` and applies
  `timestamp + ttl` as the identical Registration key and Registry field
  deadline. Every update event carries timestamp; TTL is cached and sent only
  when full or changed. Selector expires when `RedisClock.Upper()` reaches the
  derived deadline. `@expire` is redundant and absent.
- **TTL diagnostics:** normal Registration synchronization does not issue
  `PTTL` or `HPTTL`. They remain bounded diagnostics/fallback checks. Other
  leased data classes may retain their separately specified TTL-read rule.
- **Publication validity:** activate only when `now - U >= not_before` and consider
  the document expired when `now + U >= expires_at`. The issue time must not be
  later than `now - U`, and `not_before < expires_at` must hold.
- **Reason:** queueing, round-trip delay, wall-clock movement, and configured
  clock uncertainty can only shorten locally accepted lifetime.

### PRT-008: ACK states and stable error values

- **Accepted artifact rule:** define ACK states, transitions, and stable string
  statuses in one language-neutral machine-readable registry. Generate the Go
  and Rust tables, validation code, and reference documentation from it; they
  are a standard protocol table, not a naming convention copied by hand.
- **Accepted state table:** desired-state ACK values are `received`,
  `validated`, `active`, `rejected`, `expired`, `draining`, and `drained`.
- **Transitions:** permit idempotent self-transitions; `received -> validated`;
  `validated -> active`; `received|validated -> rejected|expired`;
  `active -> expired|draining`; and `draining -> drained|expired`. A newer
  desired revision starts a separate state machine. Other transitions fail as
  `invalid_transition`.
- **ACK revision:** starts at 1 and increases for every changed ACK record.
  Identical same-revision bytes are idempotent; conflicting bytes fail.
- **Core error values:** use stable lowercase strings such as `invalid`,
  `encoding`, `protocol`, `missing`, `stale`, `immutable`, `expired`,
  `capacity`, `unavailable`, `deadline`, `ambiguous`, and `corrupt`. Context is
  carried in separate string-keyed reply fields.
- **Election outcomes:** Redis actions use `ok`, `occupied`, `ineligible`, and
  `lost`; `retire` is the SDK Campaign state transition produced by its
  synchronized version comparison. These are expected state-machine outcomes,
  not stable errors.
- **Extension:** new core statuses require registry review. SDK-only diagnostics
  must not allocate stable protocol
  values.

### PRT-009: Resource units and initial safe limits

- **Accepted limit model:** distinguish immutable protocol maxima from lower
  configured deployment values. A deployment may select a lower limit but never
  a higher one for the same protocol major. PRT-015 centralizes the six active
  Registration record limits and configuration refresh interval in Redis;
  other limits remain in their specified local or data-class configuration.

| Resource | Protocol maximum | Initial configured value |
| --- | ---: | ---: |
| Zone | 32 ASCII letters | 32 letters |
| Registration UUID | exactly 32 lowercase hex characters | exact protocol form |
| Other identifier | 64 bytes | 64 bytes |
| Registration Attr fields | 128 | 16 |
| Registration Data fields | 128 | 32 |
| One application field name | 64 bytes | 64 bytes |
| One Registration Attr field value | 16 KiB | 128 bytes |
| One Registration Data field value | 16 KiB | 128 bytes |
| Complete encoded Catalog | 4 MiB | 512 KiB |
| Registration | 64 KiB | 16 KiB |
| Configuration refresh interval | 86,400,000 ms | 30,000 ms |
| ACK | 4 KiB | 2 KiB |
| Diagnostic text | 512 UTF-8 bytes | 256 bytes |
| One encoded chunk | 1 MiB | 256 KiB |
| Chunks per manifest | 256 | 128 |
| Encoded document | 64 MiB | 8 MiB |
| Decoded document | 128 MiB | 32 MiB |
| Registry entries per page | 1,024 | 256 |
| Catalog ZSET page target | internal bounded implementation | 256 |
| Catalog raw fields | 65,536 provisional | byte-bounded |
| Catalog Patch encoded field bytes | 4 MiB | complete-value limit |
| Complete in-memory Catalog encoded bytes | 64 GiB local ceiling | unlimited by default |
| One immutable local view | 1 GiB | 256 MiB |
| Non-selectable retained view | 1 GiB | 64 MiB |
| Concurrent record/chunk fetches | 64 | 16 |
| Queued wake signals | 65,536 | 4,096 |
| Concurrent retries per Client | 32 | 4 |
| Shutdown deadline | 120,000 ms | 30,000 ms |

- **Status:** the Registration defaults and all protocol maxima in this table
  are accepted as the Alpha baseline. Other data-class operational defaults
  remain subject to their capacity suites. Deployments may change Redis-backed
  Registration limits within the maxima but may not silently raise a protocol
  maximum.
- **Rule:** there is no protocol or SDK default for total service, Node, or
  Campaign count. Catalog is one Path value. Counts apply only to one page, patch, or
  bounded batch or event buffer. Counts and bytes are checked independently before
  allocation. Compression ratio is bounded by both encoded and decoded
  maxima. Defaults remain provisional until the Alpha capacity suite measures
  them.
- **Reason:** explicit conservative bounds permit implementation and
  adversarial vectors without presenting the accepted 500 Registration
  updates/s and 10 Catalog mutations/s workload as a fleet ceiling.

### PRT-010: Command log representation

- **Status:** deferred beyond SDK `1.0.0`.
- **Meaning:** a Command is a one-time imperative request whose result cannot be
  represented as a desired end state, such as collecting one diagnostic bundle
  or flushing one named cache. Ordinary lifecycle, weight, route, and
  configuration changes are desired state instead.
- **Decision:** no Command keys, APIs, handlers, ACK states, or conformance
  vectors are part of protocol `1.0` or SDK `1.0.0`. Reopen this docket only
  after the maintainer names a concrete imperative operation.
- **If retained:** use one immutable command value per command ID plus a
  sorted-set index ordered by Publisher revision. Do not use a Redis Stream in
  Alpha.
- **Append rule:** one bounded Lua operation validates Registration target,
  expected/current revision, duplicate command ID and idempotency key, count,
  total bytes,
  and expiry before writing the immutable value and index entry.
- **Read rule:** a Node reads a bounded page after its last examined revision,
  validates target, hash, and expiry, performs duplicate suppression, and writes
  a separate command-result ACK.
- **Retention rule:** Lua removes only acknowledged terminal or expired
  commands, with a bounded maximum removals per call. When count or byte
  capacity is exhausted by unacknowledged commands, append fails explicitly.
- **Reason:** explicit immutable entries and indexes make acknowledgement-aware
  retention, Registration changes, byte accounting, and conformance vectors
  visible. Stream consumer-group pending state would add backend-specific
  ownership semantics to the protocol. Deferral is preferred until those costs
  are justified by one concrete command.

### PRT-011: Redis and Sentinel qualification baseline

- **Accepted direction:** require Redis Open Source `8.0.0` or later in the
  Redis 8 line, or a documented compatible service, with Sentinel from a
  qualified compatible line. Exact patch releases remain pinned by the Alpha
  integration matrix rather than assumed compatible.
- **Readiness storage:** use one Hash field per private readiness token and
  `HSETEX`/`HGETEX` field expiration. SDK-side fixed integer comparison avoids a
  Redis version-order index; SDKs use bounded `HSCAN` and an immutable local
  ready view instead.
- **Reason:** Redis 8 provides the direct field-TTL operations used by Hermes.
  Hermes measured its relevant 100-candidate readiness layout at `5827` bytes
  versus `23003` bytes for the former ZSET plus per-token TTL Hashes, and its
  integration fixture at `6943` versus `24119` bytes. This is strong evidence
  for lower readiness memory and key overhead in the workload Verdandi is
  carrying forward; it is not a claim that every Redis 8 workload has lower
  latency.
- **ACL rule:** fixtures begin with no command, key, or channel permissions and
  add exact commands plus key/channel patterns per role. Broad command
  categories are not the conformance source of truth.
- **Accepted granularity:** issue credentials by role and Zone, not by
  per-start Registration UUID. A credential may combine explicit roles.
  Proposed roles are: Node (own SDK Registration/ACK actions plus authorized
  reads), Publisher (desired/Catalog writes and convergence reads), Selector
  (Registry read/subscription), Campaign (readiness/Leader actions), Catalog
  Subscriber (Catalog read/subscription), and Administrator (provisioning). Because
  Redis ACL patterns cannot express SDK object ownership for a newly generated
  UUID, a same-role principal deliberately using raw Redis commands remains
  outside the guarantee accepted in PRT-004.
- **References:** [Redis HSETEX](https://redis.io/docs/latest/commands/hsetex/),
  [Redis HGETEX](https://redis.io/docs/latest/commands/hgetex/), and
  [Redis ACL documentation](https://redis.io/docs/latest/operate/oss_and_stack/management/security/acl/).

### PRT-012: First executable contract artifacts

- **Recommendation:** after PRT-001 through PRT-014 are accepted, create
  artifacts in this order:

1. A language-neutral field registry for every owned Redis key, data type, Hash
   field, scalar encoding, required/optional rule, owner, and Lua action.
2. A stable string status registry with the PRT-008 values.
3. Key and channel vectors for every identifier boundary and role.
4. Valid and invalid Redis field/scalar vectors for every validation rule.
5. Hash, chunk assembly, target mismatch, and ACL-role vectors.
6. Lease vectors covering skew, delay, overflow, missing TTL, and reread.
7. Paginated Registry subscribe/scan/PING bootstrap, Catalog Hash/ZSET/Read/floor, and
   Leader state-machine vectors.
8. Lua contracts and result vectors before the scripts themselves.
9. Real Redis ACL, TTL, Lua, pagination, event-gap, election, and Pub/Sub-loss
   tests.

- **Reason:** this ordering makes the language-neutral contract executable
  before Go or Rust libraries can become accidental sources of truth.

### PRT-013: Scalable Registry synchronization

- **Accepted direction:** do not inherit Hermes's 100-instance hard limit or
  its all-record atomic Lua snapshot.
- **Accepted storage:** one Registration is one independent Redis Hash under
  `verdandi:registration:<zone>:<type>:<uuid>` and uses key TTL. The SDK creates
  a new UUID on every process start and retains it across Redis disconnects.
- **Accepted field model:** Registration Meta is exactly `@uuid`, `@revision`,
  `@timestamp`, `@ttl`, and `@version`. Immutable SDK Attr uses `.name`; mutable
  fixed-structure Data is unprefixed. `@expire` is not stored because it is
  derived from timestamp plus TTL. Data patch omission means unchanged; typed
  zero or encoded null clears a value, so there is no unset operation. Attr,
  and TTL remain immutable for the UUID lifetime. Version and Data are mutable
  Registration content. Leader uses its separate Campaign Version and never
  reads Registration Version.
- **Accepted writer (superseded by SDK-006 below):** the earlier Client-wide
  coordinator design is retained here only as decision history. The current
  design gives each successfully published Registration its own single-slot
  Fields merge mailbox, desired/confirmed state, long-lived writer, and renewal
  timer; different UUIDs do not share a mutation queue. SDK methods, Redis mutations, and Pub/Sub use
  the same four string kinds: `register`, `update`, `renew`, and
  `unregister`. Register carries complete state; non-empty Update advances
  revision and carries changed Version and/or Data; Renew retains revision and
  changes only Redis timestamp/lease expiry; graceful Unregister removes the
  whole Registration. Disconnected shutdown relies on TTL.
- **Accepted atomicity:** after SDK validation, one operation-specific Lua
  mutation obtains Redis `TIME`, atomically compares the stored revision,
  computes `deadline=timestamp+ttl`, applies identical Registration key and
  Registry membership-field absolute expiries where applicable, and publishes
  the corresponding inline event. Registry membership value and field expiry
  use one Redis 8 `HSETEX PXAT`; the Registration key uses `PEXPIREAT`. The
  stricter Redis 8 Hash-field absolute deadline ceiling is `2^46-1`
  milliseconds and is enforced by both SDKs and by Lua after Redis time is
  known. The Registry Hash and channel are both
  `verdandi:registry:<zone>:<type>`, and its UUID value is the Registration
  revision. Natural TTL removes both liveness records without an expiry index.
- **Accepted script layout:** `register`, `update`, `renew`, and `unregister`
  are four generated executables assembled from one reviewed manifest of shared
  and action-specific source fragments. An SDK loads four SHAs but invokes only
  the selected operation's script for a mutation. The selected SHA fixes both
  protocol-v1 layout and operation dispatch. Fixed request controls are
  positional values and do not repeat `&protocol`, `&kind`, or Meta names;
  dynamic Attr/Data remain named field/value pairs. A future incompatible
  request layout receives a new script/SHA. Selector retains no Registry-wide
  Lua snapshot.
- **Accepted event shape:** Pub/Sub is a MessagePack alternating key/value array.
  `&` marks control, `@` Meta, `.` Attr, and no prefix Data. Results, statuses,
  and kinds are strings. Register is self-contained. Update contains UUID,
  next content revision, Redis timestamp, and only changed Data. Renew
  contains UUID, unchanged revision, and timestamp. Unregister contains only
  UUID because it is terminal for that process identity. Close drains prior
  writes, sends only on the current healthy generation, and never reuses the
  UUID. Replies and events remain named/self-describing; only the private
  SDK-to-selected-SHA request controls are positional. Lua receives flat RESP
  arguments and never parses JSON or application field bytes.
- **Accepted synchronization:** Redis stores no Registry revision or mutation
  journal. Each Selector has one persistent listener/state-machine task and at
  most one temporary task shared by full snapshot and targeted repair. The
  listener alone receives Pub/Sub input and owns mutable live state; the
  temporary task performs bounded reads and fencing, returns one candidate, and
  is cancelled and joined before listener shutdown. A Selector acknowledges
  its dedicated subscription, calibrates a
  connection-generation RedisClock, scans membership and fetches records while
  coalescing at most one logical change per UUID, then issues `PING <nonce>` on
  the subscribed connection and waits for its ordered PONG before publishing.
  Register replaces state, contiguous Update patches it, equal-revision Renew
  raises only timestamp/deadline, Unregister purges the terminal UUID, and stale
  events are ignored. A per-UUID gap uses the same temporary task slot for one
  complete fetch plus another PING fence and does not advance the connection
  generation. Disconnect, hidden reconnect, PONG timeout, malformed
  input, buffer overflow, or non-convergence restarts synchronization.
- **Accepted lease rule:** TTL is supplied by Register and immutable for the UUID
  lifetime. Register, Update, and Renew carry Redis `@timestamp`; Selector
  derives local expiry from `timestamp + ttl` through RedisClock. Normal events do not issue `PTTL` or
  `HPTTL`; those remain diagnostic/fallback operations. Redis TTL expiry emits
  no Verdandi event, so Selector maintains one indexed deadline per UUID.
  Non-explicit expiry or fenced absence may retain non-selectable payload in a
  configured time/byte recovery cache; explicit Unregister purges it. The
  retained deadline is `timestamp + 2*ttl`, giving exactly one additional TTL.
  The initial SDK byte budget is independent from the active view: 64 MiB by
  default, zero disables retention, 1 GiB is the accepted maximum, and the
  earliest deadline is evicted first under pressure.
- **Scan proof:** Redis `HSCAN` guarantees that a field present for the full
  iteration is returned, although duplicates and concurrent-change ambiguity
  are allowed. Deduplication plus post-subscription events and the ordered PONG
  resolve those cases.
  Sync may fail explicitly if a continuously growing collection cannot finish
  within configured time/bytes; this is a deployment resource failure, not a
  protocol population ceiling.
- **No-count rule:** total Type, Node, and Registration count is absent from
  schemas and Lua operations. Page entries/bytes, fetch concurrency, buffered
  event bytes/count/age, sync duration, and local view bytes remain configured
  and bounded.
- **Qualification status:** both initial SDK adapters surface payload-bearing
  PONG in receive order and pass live Go/Rust MessagePack interoperability,
  one-pending-change-per-UUID coalescing, retained-view behavior, eight-way
  Selector fan-out, 5,000-record pagination, and RedisClock recovery across a
  real Redis 8.8 three-Sentinel fault matrix. Custom frames are no longer an
  implementation requirement; maximum-payload sustained fan-out, multi-hour
  churn, and reconnect storms remain qualification work.

### PRT-014: Catalog KV synchronization

- **Supersession:** this accepted 2026-08-26 design replaces every earlier
  Catalog membership, reference, Hash/Stream, Mirror, Compact, field-delete, and
  segmented-publication proposal.
- **Path and shape:** one Catalog is
  `verdandi:catalog:<zone>:<part>:<id>` and is exactly one Value field named
  `value`, one contiguous decimal-index Array, or one application-field Map.
  Application codecs own every external type.
- **Mutation model:** Publisher exposes only one bounded atomic Replace, strict
  Patch, and complete Delete. Replace/Delete are last-write-wins. Patch requires
  the exact current `base_revision`, adds/overwrites Map fields, and overwrites
  existing Array indices. Value change, field removal, Array append/truncate,
  holes, and shape change require Replace. Delete always creates a fresh
  tombstone revision.
- **Controller rule:** an authoritative source synchronizer may keep the current
  Redis revision and use Patch only while exact. After an ambiguous result it
  must align before another Patch. Forced publication is a complete Replace, not
  a sequence of partially visible chunks.
- **Concurrency rule:** Replace and Delete are atomic Redis-order
  last-write-wins operations. Patch projects its affected fields without a
  lock, and Lua revalidates the exact `base_revision` plus projected invariants
  before the first write. A same-base writer race has one winner and stale
  losers; no external Path lock exists.
- **Revision rule:** the Zone-global Redis execution-order range is
  `1..9007199254740991`; zero is absent. Control values are canonical decimal
  strings and ZSET scores remain exact. Every accepted Replace, Patch, or
  Delete advances once.
- **State rule:** one Zone metadata Hash stores revision/floor; live, deleted,
  and deleted-time ZSETs index Paths. A live Hash stores revision, last Replace
  revision, shape, encoded bytes, and application fields. Its companion ZSET
  stores each field's last update revision.
- **Notification rule:** the data Hash key is its Pub/Sub channel. Lua packs the
  complete accepted Replace/Patch/Delete operation from committed state. Pub/Sub
  is disposable; it is never a recovery log.
- **Subscriber rule:** one Subscriber owns one persistent Pub/Sub
  connection/listener and at most one temporary authoritative
  synchronization/repair task for all normalized exact/Part/Zone coverage.
  Pending work coalesces into that temporary slot and it exits when idle.
  Replace/Delete can
  apply directly. Patch applies only on its exact local base; otherwise the Path
  repairs from Redis. Subscribe/read/subscribed-PING/metadata-recheck forms the
  synchronization fence.
- **Recovery rule:** a valid cursor at or above floor reads newer live/deleted
  ZSET members. A zero, ahead, or below-floor cursor scans complete indexes.
  Atomic Read uses per-field revisions for exact deltas when no later Replace
  requires a complete Hash.
- **Tombstone rule:** deletion history is bounded. The current hidden defaults
  are 24 hours, 1,000,000 members, and at most 256 evictions per Delete.
  `@floor_revision` advances only to an actually evicted tombstone revision.
- **Memory and persistence rule:** every Subscriber holds complete raw covered
  values in memory. Optional bbolt/redb/SQLite is a disposable, monotonic restart
  checkpoint only; Redis remains authoritative and local state is never a
  reverse write source. The first store error disables later persistence for
  that Client generation.
- **Typed API rule:** Go `entry.Load<T>()` and Rust
  `entry.load::<T>()` project the stable raw Entry through application-owned
  field codecs. Different Paths and different calls may use different types.
  Delete changes Entry state without invalidating its identity.
- **Capacity rule:** complete encoded field-name plus field-value bytes default
  to 512 KiB and may be configured through 4 MiB. An independent Subscriber
  encoded-view budget may be configured through 64 GiB; zero means no
  additional aggregate limit. A 65,536-field internal defensive ceiling
  remains provisional because it is stricter than a literal byte-only contract.
- **Topology rule:** Catalog owns a domain Client attached to one shared root
  transport Client. Redis 8 Standalone and Sentinel are supported; Cluster is
  rejected because a mutation spans multiple Zone keys.

### PRT-015: Redis-backed live Zone configuration and SDK-side capacity

- **Accepted Redis shape:** Client bootstrap creates or fills the non-expiring
  `verdandi:config:<zone>` Hash with `protocol`,
  `registration_attr_max_fields`, `registration_data_max_fields`,
  `registration_max_field_name_bytes`,
  `registration_attr_max_field_value_bytes`,
  `registration_data_max_field_value_bytes`, and `registration_max_bytes`.
  The same Hash also contains `configuration_refresh_ms`. `protocol` is `v1`;
  recognized numbers are canonical positive decimal integers within PRT-009
  protocol maxima. Unknown optional fields are ignored.
- **Accepted defaults:** Attr fields `16`, Data fields `32`, application field
  name `64` bytes, one Attr value `128` bytes, one Data value `128` bytes, and
  the complete Registration `16384` bytes. Attr and Data counts are independent.
  Configuration refresh defaults to `30000` milliseconds, accepts `1000`
  through `86400000`, and is deployment policy rather than a local SDK switch.
- **Accepted bootstrap ownership:** each Client uses `HSETNX` only for missing
  recognized fields, then validates one complete reread. All conformant SDKs
  use the same defaults, so concurrent first Clients converge without
  overwriting an administrative value. Ordinary Register/Selector APIs cannot
  write the configuration. Raw ACL-authorized writes remain outside the SDK
  guarantee.
- **Accepted administrative lifecycle:** an authorized backend may change
  related fields after startup with one multi-field `HSET`. A Client retains
  its last complete valid snapshot. Register performs an immediate refresh
  before validation; the first successfully published Registration starts one
  Client-shared poller, and the last published Registration stops it. The
  poller waits the loaded `configuration_refresh_ms` with plus or minus ten
  percent jitter. Explicit refresh remains available without a Registration.
  A successfully loaded interval controls the next wait without restart.
  Invalid, missing, incompatible, or above-ceiling state is reported and leaves
  the previous snapshot and interval active. Protocol `1.0` has no configuration
  Pub/Sub channel or revision, so automatic adoption is bounded by the polling
  interval while a Registration is live and is not globally simultaneous.
- **Accepted lowering rule:** later content mutations validate against the
  latest local deployment snapshot; rejected mutations do not advance revision
  or touch Redis. Renew remains allowed. Selectors validate existing records
  against protocol ceilings so a policy reduction does not retroactively erase
  otherwise valid discovery state.
- **Local-only configuration:** Redis/Sentinel endpoints, credentials, TLS,
  deadlines, reconnect behavior, local concurrency, and buffer budgets remain
  local because they are needed before or independently of the Redis read.
- **Accepted capacity path:** each published Registration's sole worker keeps
  that UUID's fully encoded desired Meta/Attr/Data state and confirmation status
  in one bounded process-memory cache. It validates
  projected application field count, each
  encoded field, and total stored field-name-plus-value bytes before advancing
  revision or issuing Redis I/O. It reserves 16 decimal bytes for the
  Redis-generated `@timestamp` value.
- **Accepted volatility boundary:** the SDK writes no Registration content,
  UUID, replay log, local database, or WAL to disk. The cache exists only while
  the owning process is alive. A restart generates a new UUID and relies on TTL
  to remove the former process Registration. Selector active and retained views
  are likewise process-memory-only. Historical persistence belongs to the
  separate optional statistics/audit synchronizer.
- **Accepted Lua boundary:** the SDK validates request shape, protocol/kind,
  identifiers, canonical scalars, reserved names, immutable Attr/TTL, field
  structure, capacities, complete projected state, and no-op updates before
  Redis I/O. Lua does not duplicate those checks. It retains only conditions
  that require the current Redis state or must be joined atomically: record
  presence, revision transition, stored scalar usability, Redis time and safe
  deadline derivation, Hash/membership mutation, matched expiry, reply, and
  event publication. Raw script/write use outside the SDK remains outside the
  supported ACL trust guarantee.
- **Accepted Lua hot path:** remove the generic request Hash, use fixed control
  slots, pass SDK-supplied canonical integer strings and Redis-generated numeric
  values directly, write payload tails straight from `ARGV`, return state as
  multiple Lua values, inline one-call-site success/publication blocks, and use
  `HSETEX PXAT`. Keep Redis `TIME`, revision/state reads, deadline enforcement,
  complete-Register replacement, MessagePack events, and atomic publication.
  A future optimization requires same-fixture paired evidence and full protocol
  qualification.
- **Reason:** one Redis-backed policy keeps every language consistent while
  remaining administratively adjustable. SDK ownership of the complete desired
  state removes record-size-proportional work and configuration reads from the
  common partial-update path without introducing high-frequency disk writes or
  a mechanism that could resurrect a dead process identity.

## 6. Proposed SDK Decisions

### SDK-001: Configuration ownership

- Protocol hard bounds are identical in every SDK. The six active Registration
  record limits and refresh interval come from the Client's last-valid Zone
  configuration in Redis rather than per-process public options. Public SDK
  configuration owns connectivity, other operational timing, local
  concurrency/buffer limits, and any data-class limit not explicitly
  centralized by the protocol.
- Provisionally accepted qualification defaults are 15-second
  Registration/readiness/Leader leases,
  renewal every 5 seconds with 10 percent jitter, 2-second Redis operation
  timeout, reconnect backoff from 100 milliseconds through 5 seconds, 256
  records per page, and 16 concurrent record fetches.
- These are public SDK configuration values, not wire constants. Capacity and
  failure tests may justify changing the published defaults before release
  without changing the protocol representation.
- A Catalog Value is persistent until explicit whole-Value Delete. Catalog has
  no TTL. Local Catalog checkpoints are optional and disposable.

### SDK-002: Go package and Redis driver

- Accepted module: `github.com/LaconisIves/verdandi/sdk/go`; public package:
  `verdandi`.
- Accepted driver: `github.com/redis/go-redis/v9`. The original private-only
  adapter boundary is superseded by SDK-012 for the Go root Client; domain APIs
  remain driver-neutral.
- Superseded 2026-08-25: generic Registration `Schema` and
  `verdandi-codegen` are no longer part of the SDK. Go Attr/Data structs
  directly implement `Encoder`/`Decoder`; raw `Fields` implements the
  same contract. Typed Update still transmits only changed canonical top-level
  fields.
- The implemented module pins `go-redis/v9` `v9.22.0`; the Redis 8/Sentinel
  matrix remains a release qualification gate.

### SDK-003: Rust crate and Redis driver

- Accepted package and library crate: `verdandi`, under `sdk/rust`.
- Accepted driver for the implemented Standalone slice: `fred` with Tokio,
  rustls, Sentinel authentication, subscriber-client, scripting, Hash-expiry,
  and only the required command interfaces enabled, behind a private adapter. Its async-first lifecycle,
  automatic pipelining, dedicated subscription client, reconnection hooks, and
  zero-copy frame parsing fit Verdandi's workload better.
- Conservative fallback: `redis` from redis-rs major 1 has a larger/general
  ecosystem, sync and async APIs, a lower-level command surface, Sentinel, and
  `ConnectionManager`, but Verdandi would implement more subscription and
  recovery orchestration itself. Standalone Pub/Sub, scripting, reconnect, and
  cross-language behavior, Sentinel resolution, separate authentication, two
  promotions, and acknowledged-state recovery are qualified for the
  Registration/Selector slice. Hash-field-expiry election remains a release
  gate for the later Campaign/Leader slice.

### SDK-004: Release gate

- Publish Go and Rust `1.0.0` only after all language-neutral vectors,
  same-language tests, cross-language producer/consumer tests, Standalone and
  Sentinel failover tests, TTL/election tests, and accepted capacity tests pass.
  No partial SDK receives a formal `1.0.0` release first.

### SDK-005: Application-owned typed Registration and Selector

- Accepted 2026-08-25 and renamed 2026-08-27: application Attr and Data
  structures implement direct field conversion. Go uses `Encoder` on the value
  and `Decoder` on its pointer; Rust uses `FieldValue`. Raw `Fields` implements
  the identical contract. Verdandi does not generate application types or
  business encoding logic.
- Go `Encoder.Encode` returns one complete `Fields` and transfers map/value
  ownership to Verdandi. `Decoder.Decode` receives a detached complete map and
  replaces its receiver. The pre-release destination-injection names have no
  compatibility aliases.
- Go `registrationClient.Registration[A, D]` and Rust `Registration::new`
  create a fresh stable process UUID, validate local options, and perform no
  Redis I/O or background work.
  `Register`/`register` is the explicit service-readiness boundary and starts
  publication and renewal only after complete Attr/Data validation succeeds.
- Attr is immutable after Register. Data retains the fixed top-level field set
  established by Register. Typed Update accepts one complete Data value,
  computes a local field diff, and sends only changed fields; omission is not
  field deletion. Version-only, Data-only, combined content, Renew, and
  terminal Unregister operations remain explicit.
- Selector policy is application-supplied. `One` selects zero or one candidate;
  `Any` selects a unique set. The callback receives a borrowed immutable view
  and may stage Data predictions only through `Mutate`/`mutate`. Selection is a
  synchronous serialized local transaction with no Redis I/O. Error, timeout,
  cancellation, empty choice, duplicate, or foreign choice rolls back every
  staged mutation.
- A successful non-empty choice commits process-local predictions and returns
  detached values. Remote reconciliation is encoded-field-granular: Renew and
  unchanged remote fields preserve predictions; a remotely changed field
  replaces its corresponding prediction; removal discards the complete local
  overlay. This reduces transient load skew but provides no distributed
  reservation guarantee.

### SDK-006: Per-Registration Fields mailbox and usable Selector views

- Accepted 2026-08-26 and refined 2026-08-28: every successfully published
  Registration owns one independent single-slot Fields merge mailbox, one
  capacity-one wake signal, one long-lived synchronization worker/task, one
  desired/confirmed state, and one renewal timer. A process with `N` live
  Registrations owns `N` such units; production is expected to hold few, while
  500 and 5,000 Registrations are qualification workloads.
- One configurable admission semaphore defaults to eight result waiters and
  accepts 1..256. It does not allocate that many requests or Data copies. The
  mailbox contains only an optional pending Version, the latest value per
  changed Data field, and waiter results. Callers may cancel before admission;
  admitted work is reconciled by the sole worker. There is no Client-wide
  Registration mutation queue.
- Pending Updates use last Version/Data-field value wins. Invalid calls fail
  before merge, and valid calls absorbed by one Redis write share its revision
  and outcome. A folded state equal to confirmed state is a local no-op. Renew
  stores no Fields: it shares an effective successful Update's lease refresh in
  the same batch, otherwise it executes independently.
- A confirmed real Update refreshes the lease in the atomic Redis mutation and
  resets the next automatic Renew. Rejected/no-op Updates do not prove liveness
  and cannot postpone an already-due Renew.
- Every Selector owns one persistent Pub/Sub listener/state-machine task and at
  most one temporary synchronization/repair task. While its latest generation
  has not crossed the synchronization fence, every public active, retained,
  snapshot, and policy view returns explicit `unavailable`.
- Complete detached Snapshot is intentionally O(N) in time and memory. The
  first-version injected `One`/`Any` policy contract remains a simple O(N)
  candidate scan. Rust follows the same ownership and state invariants through
  Tokio channels, tasks, cancellation, and borrowing rather than copying Go's
  surface syntax.

### SDK-007: Registration child package and source ownership

- Accepted 2026-08-26: the public domain name is `registration`, not the
  operation name `register`. Go uses the public child package
  `github.com/LaconisIves/verdandi/sdk/go/registration`; Rust uses
  `verdandi::registration`.
- Registration and Selector belong to the same child package/module. Selector
  consumes the same Attr/Data types, Registration revisions, leases, retained
  state, and Registry synchronization contract; splitting it into a second
  public package would expose or duplicate that shared model.
- The root Go package and Rust crate retain the transport `Client`, fields and
  field conversion contracts, errors, and connection configuration.
  Registration and Catalog child clients attach domain state to that transport;
  Registration and Selector declarations are not re-exported from the root
  public namespace.
- Design and review documents live under `registration/`, matching the
  `catalog/` domain layout. Lua source/generated programs retain their existing
  `lua/src/registration/` and `lua/registration/` ownership.
- Implemented 2026-08-26: production Registration/Selector sources and tests
  now live in the child package/module, root re-exports are absent, and the Go
  and Rust testkit peers consume the new typed API. No compatibility shim is
  required because version `1.0.0` has not been published.

### SDK-008: Shared transport and Go 1.27 generic methods

- Partially superseded by SDK-010 for Go. Accepted 2026-08-27: the root Go/Rust Client owns only Redis connectivity,
  Redis 8 validation, child admission, cancellation, and joined pool shutdown.
  Registration and Catalog Clients reference it and own their domain config,
  scripts, workers, diagnostics, and Catalog checkpoint. Closing one child does
  not close the shared transport.
- Go requires 1.27. Generic methods replace unreleased package-level generic
  constructors: `registrationClient.Registration[A, D](options)`,
  `registrationClient.Selector[A, D](ctx, options)`, and `entry.Load[T]()`. The
  non-generic Catalog factories are `catalogClient.Publisher()` and
  `catalogClient.Subscriber(ctx, subscription)`.
- Static generic function instances replace retained codec closures where the
  concrete function-field type provides inference. Registration construction
  measures 240 B/three allocations instead of 288 B/five allocations on the
  current Go 1.27 Windows smoke benchmark.
- Promoted embedded fields may be named in a struct literal when the embedding
  is by value and the complete copy is deliberate. They are not available
  through pointer embedding and must not overlap explicit initialization of the
  embedded field. The new syntax is applied only where it clarifies normalized
  transport construction.
- Typed root Redis commands were outside SDK-008 itself and were subsequently
  accepted and implemented by SDK-009. They remain concrete `Key`/`Hash`
  command groups rather than raw generic handles or driver accessors.

### SDK-009: Typed root Redis command wrappers

- Accepted 2026-08-27: extend the concrete root Client from connection ownership
  into a simple typed Redis command wrapper. Its original private-driver detail
  is superseded by SDK-012; the bounded Key/Hash methods remain unchanged.
- Accepted 2026-08-27: an untagged exported struct field uses its exact Go name;
  a `redis` tag may override or exclude it. Missing HMGET positions leave the
  corresponding Go fields at zero and do not cause an error.
- `client.Hash().Get[T]` accepts a struct, derives its top-level fields, and
  executes HMGET. Scalar and map T are invalid because they do not provide a
  fixed field vector.
- `client.Hash().Load` explicitly executes HGETALL and returns the standard raw
  `Fields map[string][]byte`. Load is the dynamic-map path.
- Accepted 2026-08-27: ordinary root commands export concise and
  `Context`-suffixed pairs. Concise calls use the configured operation timeout;
  explicit Context calls additionally propagate caller cancellation, deadline,
  values, and tracing. Domain lifecycle operations remain Context-explicit.
- Accepted 2026-08-27: a typed Redis String read reports presence separately.
  Go returns `(value, found, error)`; the Rust counterpart returns
  `Result<Option<T>>`.
- Accepted 2026-08-27: persistent `Set` and expiring `SetWithTTL` are separate
  Go methods. Rust uses direct `set` and a one-use `with_ttl(ttl).set` write
  mode; `set` is the async terminal operation. `Hash.Set` is HSET patch
  behavior, while `Key.Delete` is complete-key DEL.
- Accepted 2026-08-27: version 1 contains `Ping`, complete-key/String `Key`, and
  basic `Hash`; broader command families remain deferred. `Set`/`Store` discard
  successful write counts, single-key `Delete`/`Exists` return bool, HDEL keeps
  its multi-field removal count, and HLEN keeps its field count.
- Accepted 2026-08-27: built-in scalar conversion covers bytes, strings,
  booleans as `0`/`1`, and canonical fixed-width integers. Machine-width
  integers, floats, and automatic JSON/Serde are excluded. Go application types
  use standard Binary then Text marshal interfaces; Rust uses
  `EncodeValue`/`DecodeValue`.
- Accepted 2026-08-27: Rust exact HMGET uses the manual `HashValue` trait as its
  stable contract and ships an optional `#[derive(HashValue)]` convenience
  macro. Missing derived fields use `Default`; no reflection or dynamic
  dispatch is introduced.
- Implemented 2026-08-27: both SDKs enforce a 1,024-byte key, 4,096 fields,
  1,024-byte field name, 512-KiB individual value, and 512-KiB complete Hash
  ceiling. Deterministic Redis server errors are protocol failures; an
  uncertain write transport result is ambiguous.
- The current APIs and exact contracts are in
  [`sdk/go/client.md`](sdk/go/client.md), [`sdk/go/redis.md`](sdk/go/redis.md),
  and [`sdk/rust/client.md`](sdk/rust/client.md). Go/Rust unit, format, static,
  strict Clippy, external derive, and isolated Redis 8.8 command tests pass.

### SDK-010: Thin Go root transport ownership

- Accepted 2026-08-28 for Go: root `verdandi.Client` is a thin wrapper around
  one concrete `*redis.Client`. It owns connectivity, ordinary operation
  timeout, bounded root commands, cached Hash descriptors, and a transport
  shutdown signal. It does not own Zone, domain admission, or domain workers.
- `Zone` is required independently by `registration.Config` and
  `catalog.Config`. One root connection may serve multiple Zones when the Redis
  ACL credentials authorize all affected keys.
- Root `Open` performs bounded `PING` only. Registration `Open` performs the
  Redis 8 `HELLO` check, Zone-policy bootstrap, and Lua loading. Root bootstrap
  no longer depends on `INFO` ACL permission.
- Root `Close() error` is idempotent and has no Context. It broadcasts transport
  loss and closes go-redis immediately without waiting for domain clients.
  Registration and Catalog observe the signal and independently cancel and join
  their workers; applications close domains first for deterministic ordering.
- Superseded access detail: domain packages first obtained the driver, close
  signal, and timeout through a client-owned internal capability. SDK-012
  removes that bridge and exposes the same borrowed root capabilities directly.
- Root command APIs retain both concise and `*Context` methods. The earlier
  Rust exception is superseded by SDK-011.

### SDK-011: Thin Rust root transport ownership

- Accepted 2026-08-28 for Rust: root `verdandi::Client` owns Fred connectivity,
  ordinary operation timeout, bounded root Key/Hash commands, a shutdown token,
  and the private connection recipe required to create dedicated Pub/Sub
  clients. It owns no Zone, domain admission, or domain worker lifecycle.
- Root `Config::new(endpoint)` contains no Zone. Required Zone identity moves to
  `registration::Config::new(zone)` and `catalog::Config::new(zone)`, so one
  ACL-authorized transport may serve multiple Zones.
- Root bootstrap performs bounded `PING` only. Registration bootstrap performs
  a Redis 8 `HELLO` check before Zone-policy installation and Lua loading.
- Root Key/Hash commands no longer acquire a mutex-backed root admission guard
  or update root active counters. Registration and Catalog retain independent
  admission accounting because their close operations join domain-owned work.
- `Client::close().await` broadcasts transport loss and awaits Fred `quit()`
  without joining domain Clients. Root `Client` is a cheap clone over one
  private `Arc<Inner>`; Registration and Catalog retain that same root type,
  not a second transport capability. Dropping the final root-or-domain-held
  clone cannot await and schedules best-effort cleanup when a Tokio runtime is
  available. Explicit domain-before-root close is the deterministic lifecycle
  contract.
- Crate-private root Client methods expose only the shared Fred command client,
  timeout/cancellation behavior, and bounded Subscriber construction. No Fred
  type, raw driver, `Owner`, or private `Transport` handle is public or private.

### SDK-012: Direct Go root transport capability

- Accepted 2026-08-28: Go `verdandi.Client` exposes `Redis() *redis.Client`,
  `Done() <-chan struct{}`, and `Timeout() time.Duration`.
  Registration and Catalog depend directly on `*verdandi.Client`; the
  `internal/clientaccess` package is removed.
- `Redis()` is a borrowed pointer to the one driver owned by the root Client.
  Callers must not close or reconfigure it; root `Close()` remains the sole
  close owner. Returning the same pointer creates no connection, worker, or
  allocation.
- `Done()` represents only permanent root shutdown. Temporary disconnect,
  reconnect, or Sentinel promotion must not close it. `Timeout()` is
  the immutable normalized ordinary-command timeout.
- Go applications may deliberately execute raw go-redis operations through the
  borrowed pointer. Redis ACLs are the security boundary; such operations can
  bypass Verdandi validation, limits, atomic invariants, and stable error
  mapping and are therefore outside the SDK consistency guarantee.
- This is a Go-language driver-access decision, not a wire shape. Rust keeps
  Fred private on its root Client but has no alternate transport capability.
  Root/domain Open starts no shutdown-only watcher; owner loops observe the root
  cancellation primitive directly and explicit Close joins only work owned by
  that domain.

### SDK-013: Name the ordinary root command budget `timeout`

- Accepted 2026-08-28 before 1.0.0: Go exposes `Config.Timeout` and
  `Client.Timeout()`; Rust exposes `Config::timeout` and uses `timeout()` for its
  crate-private accessor. The stable invalid-field name is `timeout`.
- `timeout` is the conventional single-word name because its containing root
  Config/Client already limits the scope to ordinary Redis commands. It remains
  a relative `Duration`; `deadline` would incorrectly suggest an absolute time.
- Domain-specific budgets retain precise names such as `sync_timeout` because
  multiple independent time limits exist in those Configs.
- No compatibility alias is retained: version 1.0.0 has not been published, and
  carrying both names would permanently enlarge the API for no compatibility
  benefit.

### SDK-014: Inline trivial one-use bootstrap wrappers

- Accepted 2026-08-28: Go and Rust root Open execute their sole bounded `PING`
  at the ownership-transfer boundary, and both Catalog Open implementations
  execute their sole script load directly. The removed private wrappers each
  had one call site, one command, and no independent state transition or
  reusable contract.
- Registration retains `bootstrap()` because its ordered Redis 8 check, Zone
  configuration publication, and Lua load form one meaningful initialization
  invariant with several failure points.
- This decision reduces private indirection and identifier surface. It makes no
  runtime-performance claim; an async private call is already compiler-
  optimizable.

### SDK-015: Numeric configuration belongs to its owning structure

- Revised 2026-08-29: the 2026-08-28 VDL/code-generation experiment is
  superseded. `schema/config.vdl`, its generator, and the parallel Go/Rust rule
  modules are removed. Defaults, ranges, zero-value handling, and relationship
  checks now live in methods on the native `Config`, `RegistrationLimits`, and
  private runtime configuration structures that consume them.
- No exported defaults/ranges constants surface is added. `configuration.md` is
  maintained as the cross-language review table, and tests protect both SDKs
  against accidental drift.
- Applications may obtain settings from JSON, TOML, YAML, environment
  variables, flags, code, or another carrier and populate language-native
  structs. Verdandi fixes no runtime serialization format. Root Redis,
  Registration/Selector, and Catalog configuration remain separate types in
  their owning packages/modules.
- Native SDK code owns Standalone/Sentinel topology, addresses, credentials,
  TLS/path objects, all field and cross-field checks, driver mapping, and I/O.
  Cluster is rejected. Go zero values select defaults; pointers distinguish
  nil-default from explicit zero where zero is valid. Rust constructors return
  explicit defaults and all public fields remain configurable.
- Root command/connect timeouts default to 2/5 seconds. Both languages use a
  configurable 1..4 default connection pool and 10-second excess-idle timeout.
  Connection recovery defaults to 100 ms exponential backoff by 2 through 5
  seconds with 10 percent jitter. It restores transport only: Go uses zero
  go-redis command retries and Rust uses one total Fred command attempt.
- Registration mailbox admission defaults to 8 and ranges 1..256. Existing
  Redis-backed Attr/Data policy defaults and later administrator overrides are
  retained. Selector local buffers, timeouts, views, RedisClock, diagnostics,
  and recovery are configurable. Catalog synchronization, read concurrency,
  repair/diagnostic buffers, recovery, locks, view bytes, record bytes, and
  checkpoint path are configurable; record bytes default to 512 KiB and may
  reach 4 MiB.

### SDK-016: One strict v1 JSON boundary converts into native configuration

- Accepted 2026-08-29: the “no fixed runtime carrier” clause of SDK-015 is
  superseded. Verdandi v1 fixes one external JSON structure with top-level
  `version`, `redis`, optional `registration`, and optional `catalog` objects.
  [`configuration.schema.json`](configuration.schema.json) is the canonical
  shape; both SDKs load [`configuration.example.json`](configuration.example.json)
  in tests.
- The JSON DTO contains only strings, booleans, integer milliseconds/counts,
  arrays, and nested objects. Go and Rust convert it to their own Duration,
  topology/TLS, checked integer, and path types. SDK-015's native ownership,
  native structure validation, lack of generated configuration code, and lack
  of exported range/default constants remain unchanged.
- Loading is bounded to 1 MiB and strict: unknown and duplicate fields, null,
  a second trailing JSON value, fractional numbers, unsupported versions, and
  invalid field or relationship values fail before Redis, checkpoint, or TLS
  certificate-file I/O. Omission selects a default; explicit zero is accepted
  only for fields whose defined behavior uses zero.
- Cluster remains unsupported. `mode` is exactly `standalone` or `sentinel`;
  addresses use validated host:port text and Sentinel owns a required
  `master_name` plus optional independent credentials.
- Catalog's external token lock remains in this implementation. SDK-017 fixes
  its long-term v1 disposition and workload assumption.
- Corrected the executable Catalog protocol ceiling to 4 MiB. The generated
  Lua had retained a stale 512 KiB hard ceiling even though both SDK configs
  already allowed `max_record_bytes` through 4 MiB.

### SDK-017: Expand TLS and retain the low-contention Catalog lock

- Accepted 2026-08-29: `redis.tls` is a nested object with `enabled`,
  `system_roots`, `server_name`, `ca_file`, `cert_file`, and `key_file` rather
  than a boolean switch. Both SDKs use TLS 1.2 or newer and always verify the
  peer. No insecure skip-verification field is exposed.
- System roots default on. A PEM CA bundle may be appended, or may become the
  complete trust set when `system_roots` is false. Client certificate chain and
  unencrypted PEM key paths must be supplied together. Every certificate file
  read is capped at 1 MiB; JSON path text is capped at 4096 UTF-8 bytes.
- JSON loading performs structural checks only. Go reads PEM while converting
  to `*tls.Config`; Rust retains `PathBuf` in its native `TlsConfig` and reads
  PEM immediately before constructing the Fred driver. Both stages occur
  before any Redis connection is established.
- A fixed `server_name` override is Standalone-only. Fred does not propagate a
  fixed SNI override to a primary newly discovered through Sentinel, so
  accepting it only in Go would violate the cross-language contract. Sentinel
  certificates must match their advertised host names.
- The external token-fenced Catalog Path lock is retained for v1. The intended
  Publisher deployment has one writer or a small number of nearby writers, so
  normal writes are uncontended and the acquire round trip is accepted to keep
  Patch projection and size validation in the SDK. This is no longer a pending
  decision.
- Lock TTL remains orphan recovery; acquisition timeout remains a finite bound
  on unfair retry; confirmed mutation Lua still deletes its exact token with
  the write. The lock does not promise fairness. Sustained high-contention or
  high-RTT publication is outside the accepted v1 workload and would require a
  separate protocol revision.

### SDK-018: Remove the Catalog Path lock and consolidate SDK internals

- Accepted 2026-08-30 and supersedes only the Catalog-lock clauses of SDK-016
  and SDK-017. Their JSON, TLS, native-validation, Cluster, and 4-MiB decisions
  remain current.
- Catalog has four generated scripts: Read, Replace, Patch, and Delete. All
  mutations use the same six keys and no `:@lock` key, token, TTL, acquisition
  deadline, retry loop, Acquire script, or Release script remains.
- Replace and Delete are one-call Redis-order last-write-wins. Patch retains
  SDK-owned HMGET projection, then its Lua commit repeats the exact base and
  projected-size checks before writing. Concurrent writers using one base
  produce one success and stale failures; a lost mutation reply remains
  ambiguous and requires alignment before another Patch.
- Publisher is a stateless lightweight view of Catalog Client and owns no
  worker, lock, timer, or Close operation. Catalog Client remains the admission
  and shutdown owner. At this historical checkpoint Subscriber still retained
  one reader and one repair worker; SDK-019 replaces that task-lifetime detail.
- Go consolidates shared duration/optional/Zone validation and domain activity
  gating in internal packages. Rust consolidates domain admission, RAII Guard,
  close joining, and public-handle ownership in one crate-private Activity.
  Public APIs and language-native cancellation patterns remain distinct.
- A versioned, language-neutral conformance corpus is consumed by both Go and
  Rust for shared JSON acceptance/error fields and binary Catalog MessagePack
  events. C++ was deferred at this checkpoint; SDK-019 selects and implements
  its driver and codec/runtime model.

### SDK-019: Use one persistent Catalog listener and implement one compiled C++23 SDK

- Accepted 2026-08-31 and supersedes SDK-018 only for Catalog Subscriber task
  lifetime and C++ status. The wire protocol, JSON shape, lock-free mutation
  model, Cluster rejection, and 4-MiB ceiling remain unchanged.
- Every Go, Rust, and C++ Catalog Subscriber owns exactly one persistent
  Pub/Sub listener/state-machine task. Initial full synchronization, reconnect
  alignment, and targeted repair share at most one temporary task slot.
  Requests arriving while the task is active are coalesced; the task drains the
  latest requested work and exits when idle. Steady state is one task and active
  synchronization is at most two.
- C++23 is an official Alpha SDK source baseline. It uses one compiled
  implementation with Boost.Redis 1.92, yyjson 0.12, OpenSSL, and SQLite 3.37
  or newer; CMake may fetch the locked reviewed revisions when compatible
  installed targets are absent.
- C++ templates are confined to compile-time Schema expansion, typed/raw Fields
  conversion, Catalog `Entry::load<T>`, and Selector policy callbacks. Redis
  transport, Lua execution, synchronization, checkpointing, and lifecycle are
  compiled once. Public headers expose no Boost, Asio, OpenSSL, yyjson, or
  SQLite types.
- `VERDANDI_SCHEMA` uses `consteval` descriptors, concepts, and fold expressions
  for application structs. `verdandi::fields` uses the identical domain APIs
  for raw values. No SDK source generator or runtime reflection is introduced.
- The C++ root is a shared owning handle over one private pool/reactor and
  exposes thin Key/Hash capabilities. Registration owns one worker/mailbox and
  renewal timer per published UUID. Selector owns one persistent listener plus
  at most one temporary synchronization task. Publisher owns no task, and
  Subscriber follows the same one-persistent-plus-one-temporary model.
- C++ Catalog checkpoints use SQLite transactions and normalized-scope SHA-256
  identity. Checkpoints are monotonic restart accelerators only and never write
  state back to Redis.
- Redis 8 Standalone, ACLs, Standalone TLS, and plain Sentinel are supported by
  the C++ implementation. Cluster remains rejected. Sentinel plus TLS is
  rejected until the driver can preserve explicit hostname verification across
  dynamically discovered data-node addresses; no insecure bypass is allowed.
- A duplicate C++11/14/17 SDK is rejected. SDK-020 supersedes the former C ABI
  deferral and supplies lower-standard consumers through the same compiled
  implementation.
- Current C++ qualification includes strict GCC, unit and authenticated
  Standalone integration, clang-format, clang-tidy, ASan/UBSan/leak checks, and
  an isolated three-data-node/three-Sentinel startup/integration smoke. Full
  two-promotion failover, live TLS, MSVC/Clang/macOS, packaging, long soak, and
  performance gates remain open and must not be inferred from this acceptance.

### SDK-020: Expose the compiled C++23 core through C ABI v1

- Accepted 2026-08-31 and supersedes SDK-019 only for the compatibility
  boundary. Native C++23 API, protocol, ownership, and lifecycle decisions are
  unchanged.
- C11 and C++11/14/17 consumers use one opaque C ABI over the existing C++23
  core. There is no second legacy SDK, header-only protocol implementation, or
  separate library per C++ standard.
- `verdandi::verdandi` remains the native C++23 CMake target. `verdandi::c`
  links the same runtime through `LINK_ONLY`, so the core is built as C++23
  without propagating `cxx_std_23` to a C or lower-standard consumer target.
- A source build requires a C++23-capable toolchain for the core even when the
  consuming target is C11 or C++11/14/17. A genuinely older compiler must use
  a prebuilt compatible shared runtime rather than compiling a second core.
- Strict v1 JSON is the only C ABI configuration carrier. Attr, Data, and
  Catalog values use flattened binary Fields. Opaque handles, borrowed views,
  owned release functions, fixed owned error text, string operation/status
  names, and synchronous callbacks keep STL, exceptions, templates,
  application structs, and allocator ownership behind the ABI.
- Public structure layouts and existing signatures are frozen within C ABI v1.
  Additive functions and opaque types are allowed; a required layout or calling
  convention change requires a new ABI version and compatibility plan.
- Both static and shared source builds are supported. Current qualification
  includes strict GCC, C11 and C++11/14/17 compile/link consumers, Linux shared
  symbol export, native and C ABI Redis 8.8 integration, and ASan/UBSan/leak
  checks. Windows DLL/MSVC, Clang, macOS, install/export packaging, and an
  automated binary-ABI checker remain release gates.

### SDK-021: Add a header-only C++11 convenience facade over C ABI v1

- Accepted 2026-08-31 and refines SDK-020 without changing its stable C ABI or
  the native C++23 implementation.
- `verdandi::legacy` is a header-only C++11 source facade. It provides RAII,
  owning string errors, small `result<T>`/`optional<T>` values, chrono
  durations, raw Fields, schema descriptors, and typed Client, Registration,
  Selector, and Catalog APIs. C++14 and C++17 use the same source surface.
- Every operation forwards through C ABI v1. The facade owns no Redis driver,
  connection pool, retry loop, clock, worker, Pub/Sub listener, synchronization,
  recovery, or checkpoint state. It therefore does not constitute a second
  legacy protocol implementation or a separate runtime.
- The C ABI remains the stable binary boundary. The facade's C++ class/template
  layout is a source contract and may not be used as a cross-compiler binary
  ABI. A source checkout still needs a C++23-capable compiler for the core; a
  genuinely old toolchain consumes a prebuilt runtime.
- Root/domain lifetime is shared; leaf handles are move-only and use paired C
  release calls. Selector candidates are borrowed only during a synchronous
  policy callback, callback exceptions are translated before crossing C, and
  detached results own decoded Attr/Data.
- Built-in codecs cover bytes, strings, booleans, and integral values. Raw
  Fields remain first-class. The Legacy schema layer deliberately lacks the
  native C++23 concepts/consteval diagnostics, and typed Selector values are
  decoded across the C boundary instead of reusing native cached projections.
- Current acceptance covers strict-warning C++11/14/17 static and shared
  consumers, typed C++11 Redis 8.8 Client/Registration/Selector/Catalog
  integration, clang-format, separately configured clang-tidy, and the full
  ASan/UBSan/leak path. Cross-platform packaging and ABI release gates remain
  unchanged.

### SDK-022: Add an idiomatic managed C# facade over C ABI v1

- Accepted 2026-08-31 and refines SDK-020 without changing C ABI v1 or the
  native C++23 implementation. C# is a language binding, not a fourth Redis
  protocol state machine.
- The managed assembly targets .NET 8 and .NET 10 with C# 14. It exposes
  language-native Result, immutable Fields, static generic codecs, IDisposable,
  delayed Registration, synchronous Selector One/Any, and typed Catalog APIs.
- Private source-generated P/Invoke calls the stable C ABI. Dedicated
  SafeHandles own every native result; internal parent-handle references retain
  the required child-before-parent release order. No pointer, C structure,
  release function, or callback context enters the public API.
- Borrowed Selector Candidates use `ref struct`; opaque Choice values carry a
  process-wide transaction identity. Invalid, stale, foreign, or duplicate
  choices fail and roll back local mutation. Managed exceptions are translated
  before the callback returns through C.
- The current C ABI is synchronous. The C# facade remains truthfully
  synchronous and does not expose a `Task.Run` wrapper as native async I/O.
  True async requires a separately reviewed native completion/cancellation
  contract or pure-C# backend.
- Native runtime discovery checks an explicit environment path, NuGet RID
  directory, application directory, and OS search path, then requires ABI v1.
  Release packaging must supply independently qualified per-RID binaries; the
  source project never embeds an arbitrary local Debug runtime.
- Current acceptance covers .NET 8/10 warning-as-error builds,
  formatter/analyzers, offline ownership/scalar/layout tests, independent
  self-contained Linux x64 ACL Standalone tests, and an independent
  two-promotion Sentinel fault matrix. Windows, macOS, NativeAOT/trimming,
  NuGet packaging, TLS, direct cross-language C# peers, performance, and
  endurance remain release gates.

### SDK-023: Qualify every language through an independently executable gate

- Accepted 2026-08-31. Go, Rust, C++ and C# each own a regression entry point
  that can build and qualify that language without requiring all SDKs to run in
  one aggregate command or one shared process.
- Shared protocol vectors and isolated Redis/Sentinel fixture components may be
  reused. Reuse does not transfer acceptance: each language must execute its
  own public API and report its own result.
- Cross-language interoperability remains a separate compatibility gate. It
  does not replace either participant's functional regression and is not a
  prerequisite for rerunning an unrelated language after a local-only change.
- A shared-core change requires the affected native core gate plus every
  binding gate exercised for that change. The 2026-08-31 C# qualification ran
  independent .NET 8/10 Standalone and Sentinel matrices; the Release-only C++
  parser correction also passed C++ Debug, shared Release, ASan/UBSan,
  clang-format, clang-tidy, and C++-owned live Sentinel integration.
