# Comet Go

[English](README.md) | [简体中文](README_CN.md)

The reserved native Go SDK directory, comet/go, shares [proto.comet.v1 contracts](../../proto/README.md#comet) with [Comet C++](../cpp/README.md). It currently contains design only: no callable SDK, go.mod, or passing test record. The frozen Redis SDK is not a new-protocol implementation.

## Scope

Future Go support covers Almanac, Ephemeris, and Catalog. Business interfaces were confirmed with the [C++ contract](../cpp/README.md#c-public-api); Go names, types, and package organization remain to implement. [Astrolabe](../../astrolabe/README.md) writes required management data directly to Polaris, without a prerequisite Go SDK, separate content source, or direct Star publication. Future ordinary-data displays through Go must preserve complete views, recovery, authentication, and ownership, without opening internal __ scopes.

Only proto/README.md defines fields, versions, errors, and server acceptance. Use native Context, errors, ownership, and concurrency, exposing neither generated gRPC types nor C ABI wrappers around C++, and defining no separate synchronization version scheme.

## Public interface mapping

| Role | Confirmed semantics |
| --- | --- |
| Client | Roles share active Star, session, resources; stopping one does not close Client |
| Beacon | Synchronous factory returns handle/error; synchronous update; fixed Attr/TTL/beat; optional tick returns Data only; state/changed; destroy closes immediately |
| Observer | Local Ephemeris pool one(selector)/stop, without watch/change |
| Publisher | Scope-level update(batch, ttl), synchronous atomic multi-key commit, internal versions, no automatic renewal/content replay |
| Subscriber | Scope watch returns complete Map; exact-Key watch has explicit presence; state/changed/stop |
| Reader | Subscriber semantics plus observed Almanac authoritative-version floor |

Context bounds the entire RPC, including version query and one permitted conflict repair. Do not retain failed synchronous requests for automatic replay. Only Beacon retains most recently confirmed Data and restores the same logical id after a successful object's Star switch; initial factory failure leaves retry to the caller. Native values/errors express this without C++ bindings or another async public commit model.

Exact-Key callbacks distinguish absence from valid empty Data rather than using nil/empty byte slices ambiguously. Scope Map callbacks deliver complete state while networking remains incremental; cached application data must be safely retainable. Observer mutable Data is a local short-lived estimate, never uploaded; fresh authoritative Data wins. Use owned values/controlled edits and data generations, not raw shared map writes bypassing replacement.

## Synchronization and resources

- Initial synchronization obtains a snapshot, then Watch delivers deltas. Distinguish incomplete initialization from a ready empty collection; do not add full-poll RPCs for synchronous retrieval.
- A complete view carries instance/Scope/target/version/readiness. Incomplete pages cannot replace visible baselines. Disconnection retains stale-marked views; instance changes follow reset rather than reuse old versions as a new Star's cursor.
- Returned Go maps/[]byte are not inherently readonly. Protect installed state/in-flight payloads through controlled query/iteration, owned copies, or explicit borrowing. Do not expose shared mutable maps or pretend conventions enforce immutability.
- Client shares transport/authentication; subscriptions cancel/recover independently. Coalesce reauthentication for confirmed Session invalidation, including [credential snapshot recovery](../../proto/README.md#credential-snapshot), without treating it as initial login rejection or resetting identity/TTL. Bound streams, views, buffers, and reconnect work. stop/destroy logically closes and cancels Context immediately, retaining state until goroutines/actual references drain. Never synchronously wait inside one's own callback or equate cancellation with reclamation.
- Reader preserves the observed per-group authoritative floor; Subscriber accepts a new Star's complete view and temporarily lower Key versions. Do not mix these policies or make SDKs perform peer source reconciliation. Business persistence/Polaris management commits are not Go SDK content adapters/database transactions.
- Ordinary Comet connections obey business TLS/authentication settings. Astrolabe management credentials do not automatically become Comet sessions. Always reject __ Sector; internal Almanac management reads must not relax Go SDK rules.

## Generation and verification

Keep generated Comet Go in an independent protocol package, avoiding Orbit message-name collisions. Fix public API/module path/generation targets/dependencies before implementation. Documentation alone does not authorize generators, downloads, or resumed legacy SDK development.

See [Comet acceptance](../../docs/comet.md) for cross-language behavior and Go-specific cancellation/view ownership. Documented scenarios are not implemented cases; builds/tests need current-task authorization.
