# Registration Public API

Status: implemented working-tree contract, pending maintainer release review.
This document defines the Registration/Selector API and source ownership.

## 1. Package ownership

Registration is a public child package/module rather than part of the root SDK
namespace. Selector belongs to the same child because it consumes the exact
Registration Attr/Data model, revisions, leases, and Registry synchronization
contract.

```text
sdk/go/registration/            Go package: verdandi/registration
sdk/rust/src/registration/      Rust module: verdandi::registration
sdk/cpp/include/verdandi/registration/
                                C++ namespace: verdandi::registration
lua/src/registration/           Maintained Lua fragments
lua/registration/               Generated Lua programs
registration/                   Registration design and review documents
```

Go package tests live beside `sdk/go/registration`; Rust crate-level tests
exercise `verdandi::registration`; C++ tests live under `sdk/cpp/tests`. The
scenario-oriented qualification harnesses
remain under `testkit/standalone`, `testkit/soak`, `testkit/sentinel`, and
`testkit/interop`: they deliberately share Redis fixtures and cross-language
checks with other Verdandi domains, so they are not duplicated beneath a
domain-only testkit directory.

The Go root package continues to own `Client`, `Fields`, `Encoder`, `Decoder`,
errors, and connection configuration because Registration and
other Verdandi domains share them. The Rust crate continues to own the matching
shared types. C++ owns the corresponding `verdandi::client`, `fields`, codecs,
errors, and configuration at its root namespace. Registration and Selector
declarations remain in each language's child package/module/namespace.

This produces these imports:

```go
import (
    verdandi "github.com/eosforge/verdandi/sdk/go"
    "github.com/eosforge/verdandi/sdk/go/registration"
)
```

The root Client owns only the Redis transport. Registration attaches its own
scripts, policy cache, and workers to that transport:

```go
transport, err := verdandi.Open(ctx, verdandi.Config{
	Standalone: &verdandi.Standalone{Address: "127.0.0.1:6379"},
})
if err != nil {
    return err
}
defer transport.Close()

client, err := registration.Open(ctx, transport, registration.Config{
    Zone: "Production",
})
if err != nil {
    return err
}
defer client.Close(context.Background())
```

`registration.Config` owns the Zone, per-Registration Fields-mailbox admission
and diagnostics, renew/config-refresh jitter, the initial Redis-backed
Registration policy, and every Selector page/buffer/view/clock/recovery limit.
It does not repeat Redis endpoints, credentials, pool, or command timeout from
the root Client. Go expands documented zero values; pointer fields distinguish
nil-default from an explicit zero. Rust `Config::new(zone)` materializes the
same defaults into public native fields. The owning `Config` and
`RegistrationLimits` methods perform all range and relationship checks without
exporting a separate constants API. Exact defaults and ranges are reviewed in
[`../configuration.md`](../configuration.md). The shared Go `configuration`
package and Rust `configuration` module strictly load the canonical v1 JSON and
convert its optional Registration/Selector object into these native types.

Closing the domain Client cancels and joins its own workers without closing the
shared transport. Root `Close()` broadcasts transport loss and closes go-redis
immediately; it does not wait for domain Clients. Applications should close
domain Clients first for deterministic cleanup. If the root closes first, the
Registration Client observes that signal and begins its own joined shutdown.

Registration `Open` validates Redis 8 through `HELLO`, installs missing Zone
policy defaults, and loads its scripts. The root Client performs only `PING`, so
it does not require `INFO` ACL permission. Multiple Registration Clients with
different Zones may share one transport when the transport ACL permits them.
The Go child package reads the root's public borrowed `Redis()`, permanent
`Done()`, and immutable `Timeout()` capabilities directly; it creates
neither a second client, an internal access bridge, nor a shutdown watcher.
Application raw commands through `Redis()` remain outside Registration
validation and invariants, and the root retains sole close ownership.

```rust
use verdandi::{Client, Config, FieldValue, Fields};
use verdandi::registration::{
    Candidate, Client as RegistrationClient, Config as RegistrationConfig,
    Registration, RegistrationOptions, Selector, SelectorOptions,
};
```

The name is `registration`, not `register`: Registration is the owned domain
object, while Register is one lifecycle operation on that object.

## 2. Ownership boundary

Application code owns two fixed top-level structures:

- `Attr`: immutable identity and placement attributes for one Registration;
- `Data`: mutable service state, load, readiness, address, and other values.

Each structure implements conversion to and from Verdandi `Fields`. Verdandi
owns UUID generation, Redis configuration checks, revision/timestamp handling,
partial Redis Hash updates, automatic renewal, subscription recovery, retained
views, and local selection transactions.

The encoded top-level field-name sets are fixed by the first successful
`Register`. A later typed `Update` supplies a complete `Data` value. The SDK
encodes it locally, compares it with desired state, and transmits only changed
Hash fields. Omitting a field from the encoded structure is not an unset
operation and is rejected as a contract change.

Encoding is the ownership boundary. Go receives the Data value by value and
Rust borrows it only for the synchronous `encode_fields` call; after encoding,
the mailbox retains only SDK-owned `Fields`. Verdandi therefore never keeps a
pointer or reference to the caller's struct while Redis work is pending. This
does not make concurrent mutation during encoding safe: application code must
not modify referenced maps, slices, or other interior storage until its Encode
call has returned.

`Fields` implements the same conversion interface in all SDKs. Raw users
therefore use the same generic Registration and Selector APIs without a second
codec object or Schema.

## 3. Go field contract

```go
type Encoder interface {
    Encode() (Fields, error)
}

type Decoder interface {
    Decode(Fields) error
}
```

The value type implements `Encoder`; its pointer implements `Decoder`.
Verdandi's internal pointer constraint is not exported. Go
1.27 generic methods let the domain Client own the constructors, and inference
derives `*Attr` and `*Data`, so callers specify only the two business types:

```go
handle, err := client.Registration[ProxyAttr, ProxyData](options)
selector, err := client.Selector[ProxyAttr, ProxyData](ctx, options)
```

An encoder returns every top-level field and transfers ownership of the map and
its byte slices to Verdandi. It must not retain or later mutate them. A decoder
receives a detached field map and replaces its receiver completely. Field names
and byte encodings are application protocol: they must remain stable across
languages and releases.

Example:

```go
type ProxyAttr struct {
    Region string
    Build  string
}

func (value ProxyAttr) Encode() (verdandi.Fields, error) {
    return verdandi.Fields{
        "region": []byte(value.Region),
        "build":  []byte(value.Build),
    }, nil
}

func (value *ProxyAttr) Decode(src verdandi.Fields) error {
    region, regionOK := src["region"]
    build, buildOK := src["build"]
    if !regionOK || !buildOK || len(src) != 2 {
        return errors.New("invalid ProxyAttr fields")
    }
    value.Region = string(region)
    value.Build = string(build)
    return nil
}

type ProxyData struct {
    Address string
    Power   int64
    Queued  int64
}

func (value ProxyData) Encode() (verdandi.Fields, error) {
    return verdandi.Fields{
        "address": []byte(value.Address),
        "power":   strconv.AppendInt(nil, value.Power, 10),
        "queued":  strconv.AppendInt(nil, value.Queued, 10),
    }, nil
}

func (value *ProxyData) Decode(src verdandi.Fields) error {
    address, addressOK := src["address"]
    power, powerOK := src["power"]
    queued, queuedOK := src["queued"]
    if !addressOK || !powerOK || !queuedOK || len(src) != 3 {
        return errors.New("invalid ProxyData fields")
    }
    decodedPower, err := strconv.ParseInt(string(power), 10, 64)
    if err != nil {
        return err
    }
    decodedQueued, err := strconv.ParseInt(string(queued), 10, 64)
    if err != nil {
        return err
    }
    *value = ProxyData{
        Address: string(address),
        Power:   decodedPower,
        Queued:  decodedQueued,
    }
    return nil
}
```

Verdandi calls these methods only at ownership boundaries. It caches decoded
Selector values by content revision. A borrowed selection scan does not encode
or decode every candidate.

## 4. Go Registration

```go
type RegistrationOptions struct {
    Type          string
    TTL           time.Duration
    RenewInterval time.Duration
    Version       uint64
}
```

```go
handle, err := client.Registration[ProxyAttr, ProxyData](
    registration.RegistrationOptions{
        Type:          "Proxy",
        TTL:           15 * time.Second,
        RenewInterval: 5 * time.Second,
        Version:       1,
    },
)
if err != nil {
    return err
}

// The service is not visible and no Registration-owned goroutine is created.
if err := startListening(); err != nil {
    return err
}

if err := handle.Register(ctx, ProxyAttr{
    Region: "cn-east",
    Build:  "2026.08.25",
}, ProxyData{
    Address: "10.0.0.8:8080",
}); err != nil {
    return err
}
defer handle.Unregister(context.Background())

if err := handle.Update(ctx, ProxyData{
    Address: "10.0.0.8:8080",
    Power:   12,
}); err != nil {
    return err
}
```

`Client.Registration` is local: it validates local options, creates one 32-character
UUID, performs no Redis command, admits no Client child, and starts no
goroutine. The UUID remains stable until the handle becomes terminal.

`Register` is the readiness boundary. It refreshes Zone configuration, encodes
and validates complete Attr/Data, then creates this Registration's independent
single-slot Fields merge mailbox, capacity-one wake signal, long-lived worker,
and renewal timer. The worker publishes
revision 1; successful publication makes the handle visible and starts its
automatic renewal schedule. Other Registrations owned by the same Client do not
share this mailbox or worker.

`Update` takes complete desired Data. `SetVersion` changes only Version.
`UpdateContent` changes Version and Data atomically under one content revision.
`Renew` changes only timestamp/deadline. `Unregister` is terminal; before
Register it closes only the local handle, and after Register it waits for the
Registration worker's terminal acknowledgement and performs normal terminal
cleanup. The mailbox holds only the latest pending Version and value for each
changed Data field. Later writes to the same item overwrite earlier pending
values, while every Update absorbed into one worker batch receives that batch's
result and revision. `BufferCapacity` defaults to 8 and accepts 1..256; it
bounds admitted result waiters rather than allocating full request or Data
objects. A Renew carries no Fields. If its batch contains an effective
successful Update it shares that TTL refresh; after a no-op or failed Update it
executes Renew independently. A confirmed real Update or Renew resets the next
automatic Renew deadline.
Register, Update, and Renew accept success only when Redis returns the exact
expected revision and a nonzero timestamp. An ambiguous transport result or a
corrupt success reply retains desired state as uncertain; the next Update/Renew
uses complete Register recovery instead of trusting a partial outcome.
`Close` is an alias for `Unregister`.

## 5. Go Selector policy transactions

```go
selector, err := client.Selector[ProxyAttr, ProxyData](
    ctx,
    registration.SelectorOptions{Type: "Proxy"},
)
if err != nil {
    return err
}
defer selector.Close(context.Background())
```

Every Selector owns one persistent listener/state-machine goroutine. Initial
full synchronization and later targeted repair share one optional temporary
goroutine; they can never overlap. The listener remains the only Pub/Sub
receiver and mutable-state owner, so the topology is one goroutine in steady
state and at most two while synchronizing. `Close` cancels and joins the
temporary synchronization before the listener returns.

`One` lets application policy select zero or one Registration:

```go
selected, found, err := selector.One(ctx,
    func(candidates registration.Candidates[ProxyAttr, ProxyData]) (
        registration.Candidate[ProxyAttr, ProxyData], bool, error,
    ) {
        if len(candidates) == 0 {
            return registration.Candidate[ProxyAttr, ProxyData]{}, false, nil
        }
        lowest := 0
        for index := 1; index < len(candidates); index++ {
            if candidates[index].Data.Power < candidates[lowest].Data.Power {
                lowest = index
            }
        }
        if err := candidates.Mutate(lowest, func(data *ProxyData) error {
            data.Power++
            return nil
        }); err != nil {
            return registration.Candidate[ProxyAttr, ProxyData]{}, false, err
        }
        return candidates[lowest], true, nil
    },
)
```

`Any` uses the same borrowed `Candidates` collection and returns a slice of
unique candidates. An empty slice means no match.

Selection behavior:

- the callback runs synchronously in the caller; the SDK creates no callback
  goroutine and never force-kills application code;
- waiting for the local transaction gate is context-bounded;
- the active view is borrowed and candidate buffers are reused; scanning does
  not clone or decode all candidate Attr/Data values;
- callback `Attr` and `Data` pointers are read-only by contract; direct mutation
  is detected for selected candidates and rejected as `contract`;
- `Candidates.Mutate` clones and validates only the chosen Data value, exposes
  the staged result to later reads in the same callback, and performs no Redis
  I/O;
- callback error, context expiry before return, false/empty selection, foreign
  candidate, or duplicate `Any` candidate rolls back every staged mutation;
- a valid non-empty result commits staged local predictions and returns detached
  selected candidates;
- `One`, `Any`, `Find`, and `Snapshot` never perform Redis I/O;
- while subscription/repair state is half-synchronized, `One`, `Any`, `Find`,
  `FindRetained`, and `Snapshot` fail explicitly with `CodeUnavailable`; no
  stale or partial view is exposed as usable; and
- the first-version policy contract scans the borrowed candidate view in O(N).
  A detached complete Snapshot is deliberately a heavy O(N) copy.

The callback must not call `One`, `Any`, `Snapshot`, or `Find` recursively on
the same Selector and must not retain borrowed candidates after return.

### 5.1 Optional generated Go reference path

The detached `One`/`Any` API remains the safe default when a selected value must
leave the callback. A high-frequency policy that only needs to copy one route
identifier and update process-local weighting may opt into the generated
reference path. The generator emits only strongly typed read-only accessors,
field setters, mutable-slice cloning, aliases, and the wrapper constructor. It
does not generate `Encode`/`Decode`, wire field names, Redis logic, or business
selection policy.

Place the directive beside the application structs and commit its deterministic
output:

```go
//go:generate go run github.com/eosforge/verdandi/sdk/go/cmd/verdandi-refgen -attr ProxyAttr -data ProxyData -name Proxy -output proxy_reference_generated.go

type ProxyAttr struct {
    Region string
}

type ProxyData struct {
    Address string
    Power   int64
    Queued  int64
}
```

The generated API is used without exposing `*ProxyAttr` or `*ProxyData`:

```go
reference, err := NewProxyReferenceSelector(selector)
if err != nil {
    return err
}

var address string
found, err := reference.WithOne(ctx,
    func(candidates ProxyReferenceCandidates) (
        ProxyReferenceSelection, bool, error,
    ) {
        if candidates.Len() == 0 {
            return ProxyReferenceSelection{}, false, nil
        }
        best, _ := candidates.At(0)
        power := best.Data().Power()
        for index, count := 1, candidates.Len(); index < count; index++ {
            candidate, _ := candidates.At(index)
            candidatePower := candidate.Data().Power()
            if candidatePower < power {
                best = candidate
                power = candidatePower
            }
        }

        selected := best.Select()
        address = selected.Data().Address()
        if err := selected.Edit().SetPower(power + 1); err != nil {
            return ProxyReferenceSelection{}, false, err
        }
        return selected, true, nil
    },
)
if err != nil {
    return err
}
if !found {
    return errNoProxy
}
return dial(address)
```

`WithOne` returns only `(bool, error)` and `WithAny` returns only
`(selectedCount, error)`. The application copies any route value it needs while
the callback is active. Neither operation creates detached Candidate results.
Both reuse the same Selector transaction gate and overlay as the ordinary API,
perform no Redis I/O, and remain unavailable while synchronization is
incomplete.

Reference-path rules:

- `ReferenceCandidates`, Candidate, Selection, AttrRef, and DataRef are borrowed
  for exactly one synchronous callback. Go has no lifetime types, so retaining
  them or invoking them asynchronously is a caller contract violation.
- generated read views expose no mutable pointer. Scalar, string, value-safe
  local struct, and fixed-array getters copy by value; slice getters return an
  independent copy only when that field is read.
- generated slice setters copy their input. The first setter on a candidate
  also applies the generated deep clone before changing the transaction value.
- only edits belonging to the Selection values finally returned by
  `WithOne`/`WithAny` are encoded and committed. Edits to examined but
  unselected candidates are discarded.
- callback error, false/empty selection, context cancellation, invalid or
  foreign Selection, duplicate `WithAny` Selection, encoding failure, field-
  structure change, or limit failure rolls back the complete operation.
- a selected Data value is encoded once after the callback; the reference path
  does not perform the old defensive re-encode/decode needed for public
  `*Data` and detached results.
- generated setters remain token-fenced after callback return. They fail with
  `CodeContract` instead of mutating a reused transaction buffer.

The generator accepts exported fields whose types can be proven safe from the
current package source: predeclared scalars, local scalar aliases, fixed arrays,
local value-only structs, and slices of value-safe scalar elements. It rejects
embedded or unexported top-level fields, generic structs, maps, pointers,
interfaces, functions, nested reference-bearing values, and opaque external
types. These restrictions affect only the optional reference facade; the
application-owned `Encoder`/`Decoder` remains the wire authority. CI can invoke
the same command with `-check` to reject a stale generated file.

## 6. Local prediction and remote reconciliation

Local mutation is a process-local soft prediction, not distributed capacity
reservation. Multiple Selector processes can temporarily predict different
Power values. Accurate global reservation requires a separate distributed
coordinator.

Verdandi reconciles predictions by encoded Data field:

- `renew` preserves all local predictions because content revision is unchanged;
- a Version-only content revision preserves predictions because Data bytes did
  not change;
- remote Data fields whose bytes changed replace the corresponding locally
  predicted fields;
- locally predicted fields whose remote bytes did not change remain in effect;
- `register`/resynchronization follows the same comparison when the prior base
  exists;
- `unregister`, natural expiry, or authoritative absence removes the active
  prediction with the Registration.

This supports high-frequency, small-step load prediction while guaranteeing
that fresh remote reports correct the fields they actually own.

## 7. Raw Fields

No separate raw API design is required:

```go
handle, err := client.Registration[verdandi.Fields, verdandi.Fields](
    options,
)
selector, err := client.Selector[verdandi.Fields, verdandi.Fields](
    ctx,
    selectorOptions,
)
```

`Fields` copies byte slices at SDK ownership boundaries. An empty byte slice is
a real value. Field deletion is not part of Registration Data.

## 8. Rust field contract and Registration

```rust
pub trait FieldValue: Sized {
    fn encode_fields(&self, dst: &mut Fields) -> Result<(), Error>;
    fn decode_fields(src: &Fields) -> Result<Self, Error>;
}
```

Application crates implement the trait directly. `Fields` implements it as
well. Rust uses associated constructors and borrowed inputs:

```rust
let redis = Client::open(Config::new(
    "redis://127.0.0.1:6379",
))
.await?;
let client = RegistrationClient::open(
    &redis,
    RegistrationConfig::new("Production"),
).await?;

let registration = Registration::<ProxyAttr, ProxyData>::new(
    &client,
    RegistrationOptions {
        type_name: "Proxy".to_owned(),
        ttl: Duration::from_secs(15),
        renew_interval: Some(Duration::from_secs(5)),
        version: 1,
    },
)?;

start_listening().await?;
registration.register(&attr, &data).await?;
registration.update(&next_data).await?;
registration.update_content(2, &next_data).await?;
registration.unregister().await?;
```

Construction is local and registration is delayed exactly as in Go. Async
methods return Verdandi `Result`. A published Registration owns one mutex-
protected Fields mailbox, one capacity-one Tokio MPSC wake channel, one small
Semaphore for admitted result waiters, one long-lived task, and one resettable
`Sleep`; it does not share a Client-wide Registration task. Dropping an
awaiting Future does not release its admission permit until the worker processes
or rejects the mailbox batch, so cancellation storms cannot bypass the bound.
Dropping the Registration closes future admission and signals that same task
for best-effort cleanup. Explicit `unregister().await` waits for terminal
cleanup.

Rust Zone ownership and transport shutdown match Go's domain boundary. Root
`verdandi::Config::new(endpoint)` contains no Zone; each Registration Client
requires `registration::Config::new(zone)`. Root `close().await` broadcasts loss and awaits
Fred shutdown without waiting for this Client. Registration performs the Redis
8 `HELLO` check during its own bootstrap. Close the Registration Client before
the transport when deterministic worker cleanup matters.

Root and Registration `open` create no shutdown-only Tokio task. The root
`CancellationToken` directly gates domain admission, and existing Registration,
Selector, and configuration-refresh owners observe their hierarchical token.
Explicit domain `close().await` cancels and waits for admitted work; Drop only
signals because Rust cannot await from a destructor.

## 9. Rust Selector

```rust
let selector = Selector::<ProxyAttr, ProxyData>::new(
    &client,
    SelectorOptions {
        type_name: "Proxy".to_owned(),
    },
)
.await?;

let selected = selector
    .one(Duration::from_millis(10), |candidates| {
        let choice = candidates
            .get(0)
            .ok_or_else(|| Error::field(Code::Missing, "candidate"))?
            .choice();
        candidates.mutate(choice, |data| {
            data.power += 1;
            Ok(())
        })?;
        Ok(Some(choice))
    })
    .await?;
```

Rust exposes immutable `CandidateRef<'_, Attr, Data>` during the callback and
an opaque copyable `Choice`. The borrow checker prevents direct mutation of the
view. `Candidates::mutate` stages typed Data. `one` returns
`Result<Option<Candidate<Attr, Data>>>`; `any` returns a detached vector.

The duration bounds asynchronous gate acquisition and is checked again after
the synchronous callback. The SDK does not spawn or abort the callback. Rust
reuses the immutable Arc-backed Selector view directly and does not allocate or
clone one record handle per candidate scan. `snapshot`, `find`,
`find_retained`, `one`, and `any` return `Code::Unavailable` while the Selector
is half-synchronized, matching Go's usable-view boundary.

## 10. C++23 field contract, Registration, and Selector

C++ keeps the same package boundary as namespace
`verdandi::registration`. The root transport, Fields, codecs, error/result, and
native configuration types remain in `verdandi`. Boost.Redis and other driver
types do not appear in public headers.

Application structures declare compile-time member descriptors:

```cpp
struct ProxyAttr {
    std::string region;
};

struct ProxyData {
    std::int64_t power{};
    bool ready{};
};

VERDANDI_SCHEMA(ProxyAttr,
                VERDANDI_FIELD(ProxyAttr, region));
VERDANDI_SCHEMA(ProxyData,
                VERDANDI_FIELD(ProxyData, power),
                VERDANDI_FIELD(ProxyData, ready));
```

`VERDANDI_SCHEMA` is compile-time metadata, not runtime reflection or generated
logic. It uses `consteval` field descriptors, concepts, and fold expressions to
validate wire names and invoke `field_codec<Member>`. A missing, duplicate,
reserved, or invalid field name fails compilation. `verdandi::fields` satisfies
the same `structured_value` concept for raw binary callers.

```cpp
verdandi::registration_configuration config;
config.zone = "Production";
auto client = verdandi::registration::client::open(transport, config);

auto registration =
    verdandi::registration::registration<ProxyAttr, ProxyData>::create(
        *client,
        {.type = "Proxy",
         .ttl = std::chrono::seconds{15},
         .version = 1});

// Construction is local; readiness controls the later publication.
registration->publish(ProxyAttr{"cn-east"}, ProxyData{0, true});
registration->update(ProxyData{1, true});
registration->set_version(2);
registration->update_content(3, ProxyData{2, true});
registration->renew();
registration->close();
```

`create` generates and retains the process UUID without Redis publication.
`publish` is the readiness boundary. Encoding completes synchronously before
Fields enter the Registration-owned mailbox. One published Registration owns
one worker, one coalescing mailbox, one renewal timer, and one desired/confirmed
state; unrelated Registrations share none of those objects. `try_error()`
non-blockingly returns bounded asynchronous diagnostics.

```cpp
auto selector =
    verdandi::registration::selector<ProxyAttr, ProxyData>::create(
        *client,
        {.type = "Proxy"});

auto selected = (*selector)->one(
    [](auto& candidates)
        -> verdandi::result<
            std::optional<verdandi::registration::choice>> {
        if (auto candidate = candidates.get(0)) {
            auto changed = candidates.mutate(
                candidate->identity(),
                [](ProxyData& data) { ++data.power; });
            if (!changed) {
                return std::unexpected(changed.error());
            }
            return std::optional<verdandi::registration::choice>(
                candidate->identity());
        }
        return std::nullopt;
    });
```

`one` and `any` execute the callback synchronously on the caller thread and
bound both transaction-lock wait and total callback elapsed time with the
Selector operation timeout. The SDK does not spawn or forcefully terminate
application callbacks. `candidate_ref` is borrowed and immutable; `choice` is
opaque; successful selection returns detached `candidate` values. `snapshot`
is the explicit O(N) detached copy and includes retained non-selectable values.

Each C++ Selector owns exactly one persistent listener/state-machine task.
Initial full synchronization and later targeted repair share at most one
temporary synchronization task; requests coalesce into the occupied slot and
it exits when idle. The public view returns `code::unavailable` whenever that
fence is incomplete. `close()` deterministically stops and joins both possible
tasks without closing the Registration Client or root transport.

## 11. Performance reference

Go 1.27 WSL/Linux benchmarks on the current 13th Gen Intel Core i7-13700F use
500 cached typed candidates and ten one-second `b.Loop` samples. The ordinary
safe `One` scans for minimum Power, stages `Power++`, validates the borrowed
value, commits the overlay, and returns a detached Candidate. It measured
11.387-11.714 microseconds/op with an 11.460-microsecond median, 2,225 B/op, and
28 allocations/op. Generated `WithOne` performs the same scan and local
`Power++` but intentionally returns no detached Candidate. It measured
10.118-10.298 microseconds/op with a 10.178-microsecond median, 425 B/op, and
four allocations/op: 11.18% lower median time, 80.90% fewer bytes, and 85.71%
fewer allocations in this workload.

The ordinary safe `Any` selects eight of 500 candidates and returns eight
detached values. It measured 13.820-14.202 microseconds/op with a
13.875-microsecond median, 8,065 B/op, and 97 allocations/op. Generated
`WithAny` selects the same eight into a caller-reused Selection slice and
returns only the count. It measured 5.525-5.628 microseconds/op with a
5.587-microsecond median and zero bytes/allocations per steady-state operation,
a 59.73% lower median. These pairs intentionally compare the safe detached
contract with the optional callback-only contract; they are not claims that
identical return semantics became free.

The remaining four `WithOne` allocations come from encoding and retaining the
one changed Data value with the deliberately simple decimal test codec. Codec
choices still matter. Applications may use fixed-width or reusable byte
encodings inside their own `Encoder` implementations without changing the
reference API or its selected-only transaction rules.
