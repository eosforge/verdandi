# Verdandi C# SDK

## Status

The C# SDK is an idiomatic managed facade over Verdandi C ABI v1 and the same
compiled C++23 runtime used by C, lower-standard C++, and other native
consumers. It does not contain another Redis driver, Lua loader, recovery state
machine, RedisClock, Registration worker, Selector listener, or Catalog
subscriber. Those components remain in the C++23 core.

The managed assembly currently targets `net8.0` and `net10.0`, is compiled with
the pinned .NET 10 SDK and C# 14, and has no third-party managed dependency.
Version metadata remains `1.0.0`, but no NuGet package, native RID package, tag,
or stable ABI has been released.

Current qualification covers:

- warning-as-error builds for .NET 8 and .NET 10;
- formatter and .NET analyzer verification;
- offline ownership, scalar-codec, Result, typed-codec, malformed-codec, and
  64-bit ABI-layout tests;
- self-contained .NET 8 and .NET 10 Linux x64 consumers loading the GCC shared
  Release runtime through both explicit and application-directory discovery;
- ACL-protected Redis 8.8 Key/Hash, Registration, Selector, and Catalog
  behavior, exact configured limits, concurrent callers, and terminal handle
  behavior, including concurrent root disposal and finalizer-backed native
  cleanup; and
- an independent three-Redis/three-Sentinel matrix with separate Redis and
  Sentinel ACLs, two promotions, acknowledged-write-loss repair, `SCRIPT
  FLUSH`, total Sentinel loss, unavailable views, and generation recovery.

Windows DLL/MSVC, macOS, NativeAOT, trimming, NuGet RID packaging, TLS,
cross-language C# peers, allocation benchmarks, and soak tests remain release
gates. The SDK is not ready for
production use.

## Architecture

The public surface is fully managed:

- `Verdandi.Client` owns the root transport and raw Key/Hash operations;
- `Verdandi.Registration.RegistrationClient` owns one Registration Zone;
- `Registration<TAttr,TData>` preserves local construction and delayed
  readiness publication;
- `Selector<TAttr,TData>` exposes synchronous `One` and `Any` policy
  transactions over a borrowed local view;
- `Verdandi.Catalog.CatalogClient`, `CatalogPublisher`, `CatalogSubscriber`,
  and `CatalogEntry` expose Catalog lifecycle and typed loading; and
- `Result<T>`, `VerdandiError`, `Fields`, and `IFieldValue<TSelf>` keep stable
  errors and application codecs independent from the native implementation.

Private source-generated `LibraryImport` declarations call C ABI v1. Every
native allocation is owned by a dedicated `SafeHandle`. Child wrappers hold an
internal SafeHandle reference until their native handle has been released, so
disposing a public parent early cannot reverse the C ABI release order. Public
`Dispose` performs best-effort cleanup; callers that need to observe a failure
use `Close()` or `Unregister()` first.

The API is intentionally synchronous. The current C ABI is synchronous, so the
managed SDK does not consume a thread-pool thread with `Task.Run` and call that
an asynchronous Redis operation. A future async API requires an actual native
completion/cancellation contract or an independently approved pure-C# backend.

## Build

Build and verify the managed projects from this directory:

```powershell
dotnet restore Verdandi.slnx
dotnet format Verdandi.slnx --verify-no-changes --no-restore
dotnet build Verdandi.slnx --configuration Release --no-restore
dotnet run --project tests/Verdandi.Tests --configuration Release --framework net8.0 --no-build
dotnet run --project tests/Verdandi.Tests --configuration Release --framework net10.0 --no-build
```

The managed assembly requires a matching shared Verdandi C++ runtime at
execution time. A source checkout can build it with the existing C++ shared
preset under WSL/Linux:

```text
cmake --preset gcc-shared-release
cmake --build --preset gcc-shared-release
```

The loader checks these locations in order:

1. the exact file in `VERDANDI_NATIVE_LIBRARY`;
2. `runtimes/<runtime-identifier>/native/` below the application directory;
3. the application directory; and
4. the operating system's normal native-library search path.

Expected names are `verdandi_cpp.dll`, `libverdandi_cpp.so`, and
`libverdandi_cpp.dylib`. The runtime must report C ABI version 1; a missing
library returns `unavailable/native_library`, while a wrong binary or ABI
returns `incompatible` before Redis is opened.

The source project deliberately does not copy an arbitrary local Debug runtime
into pack output. A formal NuGet release must build, test, sign where required,
and place each qualified binary under its exact RID native directory.

## Configuration and root Client

Configuration remains the same strict cross-language v1 JSON. The managed
layer performs only strict UTF-8 conversion and passes the bytes to the native
parser, so it has no duplicate DTO, defaults, bounds, credential storage, or
TLS interpretation.

```csharp
using Verdandi;

var opened = Client.Open(File.ReadAllText("configuration.json"));
if (!opened.IsSuccess)
{
    Console.Error.WriteLine(opened.Error);
    return;
}

using var client = opened.Value;
var ping = client.Ping();
```

`Client` additionally exposes complete raw Key and Hash operations. They are
ACL-controlled escape hatches outside Registration and Catalog validation and
atomicity guarantees.

## Strong field values

Redis stores flattened binary Fields, not JSON application records. An Attr,
Data, or Catalog value implements the static generic contract directly:

```csharp
using Verdandi;

public readonly record struct ProxyData(string Address, long Power)
    : IFieldValue<ProxyData>
{
    public Result<Fields> EncodeFields()
    {
        var fields = Fields.CreateBuilder();
        var address = fields.Add("address", Address);
        if (!address.IsSuccess)
        {
            return Result<Fields>.Failure(address.Error!);
        }

        var power = fields.Add("power", Power);
        return power.IsSuccess
            ? fields.Build()
            : Result<Fields>.Failure(power.Error!);
    }

    public static Result<ProxyData> DecodeFields(Fields fields)
    {
        var address = fields.GetString("address");
        var power = fields.GetInt64("power");
        return fields.Count == 2 && address.IsSuccess && power.IsSuccess
            ? Result<ProxyData>.Success(new ProxyData(address.Value, power.Value))
            : Result<ProxyData>.Failure(new VerdandiError("invalid", "ProxyData"));
    }
}
```

`Fields` is immutable and owns one continuous payload containing all UTF-8
names and values. Native input pins that payload once and builds only a small
temporary view array. Public raw-byte reads return a copy. Built-in helpers use
strict UTF-8 plus canonical `true`/`false` and decimal Int64/UInt64 encodings.
They reject signs where forbidden, leading zeros, negative zero, trailing
bytes, and overflow. Application-specific byte encodings remain explicit.

Raw `Fields` implements `IFieldValue<Fields>`, so callers that do not want a
typed record use the same generic APIs without another raw surface.

## Registration

```csharp
using Verdandi.Registration;

using var registrations = RegistrationClient.Open(client).Value;
using var registration = registrations
    .NewRegistration<ProxyAttr, ProxyData>(
        new RegistrationOptions(
            "Proxy",
            TimeSpan.FromSeconds(15),
            version: 1,
            renewInterval: TimeSpan.FromSeconds(5)))
    .Value;

// The service is still invisible and owns no Registration synchronization task.
StartListening();

var registered = registration.Register(attr, data);
var updated = registration.Update(nextData);
var versioned = registration.UpdateContent(2, nextData);
var stopped = registration.Unregister();
```

`NewRegistration` is local. It validates local option shape through the native
core, generates one 32-character UUID, performs no Redis command, and starts no
worker. `Register` is the readiness boundary and publishes complete immutable
Attr plus complete Data before it returns success. Update encodes synchronously
and the native Registration-owned mailbox retains only SDK-owned Fields.

`SetVersion`, `UpdateContent`, `Renew`, `TryGetError`, `Revision`, and
`TimestampMilliseconds` map to the same C++ core lifecycle. `Unregister` is the
result-bearing terminal operation; `Dispose` is its best-effort fallback.

## Selector

```csharp
using var selector = registrations
    .NewSelector<ProxyAttr, ProxyData>("Proxy")
    .Value;

var selected = selector.One(
    candidates =>
    {
        if (candidates.Count == 0)
        {
            return Result<Choice?>.Success(null);
        }

        var first = candidates.Get(0);
        if (!first.IsSuccess)
        {
            return Result<Choice?>.Failure(first.Error!);
        }

        var predicted = first.Value.Data with
        {
            Power = first.Value.Data.Power + 1,
        };
        var staged = candidates.Mutate(first.Value.Choice, predicted);
        return staged.IsSuccess
            ? Result<Choice?>.Success(first.Value.Choice)
            : Result<Choice?>.Failure(staged.Error!);
    });
```

The policy runs synchronously on the caller's thread. `Candidates<TAttr,TData>`
is a `ref struct`, so it cannot be stored in a normal object, captured by an
async state machine, or used after the callback. Every opaque `Choice` also
contains the current process-wide transaction identity; a retained, foreign,
or duplicate Choice returns `contract` and rolls back every staged mutation.

Candidate Attr/Data are copied and decoded lazily, then cached for repeated
reads within that callback. `Mutate` encodes a complete replacement Data and
updates the native local prediction transaction without Redis I/O. A successful
non-empty `One`/`Any` commits staged predictions and returns fully detached
candidates. Empty selection, callback error or exception, invalid Choice, and
native failure roll back. `Snapshot()` is the explicit O(N) detached active and
retained view.

The managed binding adds no Selector task. The C++ core still owns one
persistent listener and at most one temporary synchronization/repair task.

## Catalog

```csharp
using Verdandi.Catalog;

using var catalog = CatalogClient.Open(client).Value;
using var publisher = catalog.NewPublisher().Value;

var path = new CatalogPath("routing", "primary");
var revision = publisher.Replace(path, CatalogKind.Map, record).Value;

using var subscriber = catalog
    .NewSubscriber(new CatalogSubscription(parts: ["routing"]))
    .Value;
using var entry = subscriber.Find(path).Value;

if (entry is not null)
{
    var snapshot = entry.Load<RouteRecord>().Value;
    if (snapshot.HasValue)
    {
        Use(snapshot.Value);
    }
}
```

Publisher supports complete last-write-wins `Replace`, exact-base `Patch`, and
`Erase`. Subscriber creation is synchronous and returns only after initial
alignment. `Find` and `CatalogEntry.Load<T>` read the local immutable view and
perform no Redis or disk I/O. A `CatalogSnapshot<T>` distinguishes `HasValue`
from a default value and preserves the stable string status, revision, and
synchronization flag from one immutable Entry state.

## Errors and callbacks

Fallible APIs return `Result` or `Result<T>`. Check `IsSuccess` before `Value`;
`Error.Code` is the only machine-stable category. `Field`, `Detail`, and optional
`Revision` are owned diagnostics. A default-initialized Result is deliberately
an `invalid/result` failure rather than a false success.

No managed exception crosses an unmanaged callback. Codec and Selector-policy
exceptions become stable `capacity/codec`, `unavailable/codec`,
`capacity/callback`, or `unavailable/callback` failures. Fatal runtime failures
are not represented as a Redis protocol success.

## Verification

The dependency-free executable under `tests/Verdandi.Tests` runs offline by
default. Its optional direct live mode is:

```text
Verdandi.Tests --redis <host:port>
Verdandi.Tests --configuration-file <path>
```

The complete C# regression is intentionally executable without running another
language's SDK suite:

```powershell
$env:VERDANDI_TEST_SSH_PASSWORD = "<temporary test-host password>"
python -B tests/standalone_test.py --host 192.168.0.90 --ssh-user ubuntu --result-file ../../testkit/results/csharp-standalone.json
python -B tests/sentinel_test.py --host 192.168.0.90 --ssh-user ubuntu --result-file ../../testkit/results/csharp-sentinel.json
```

The Standalone harness builds the shared Release core, restores and analyzes
the managed projects, runs both target frameworks offline, publishes both as
self-contained Linux x64 applications, and gives each framework its own
ACL-protected Redis 8.8 integration. It covers raw Key/Hash, delayed Register,
Update, SetVersion, UpdateContent, Renew, Unregister, exact Attr/Data and 4 MiB
Catalog boundaries, concurrent Registration/Selector calls, One/Any rollback
and prediction, malformed codecs, Catalog stale Patch and shape validation,
parent-first and concurrent Dispose safety, forced finalizer cleanup, both
loader paths, final key cleanup, and fixture removal.

The Sentinel harness keeps one .NET 8 and one .NET 10 peer alive through two
promotions. It proves loss of an acknowledged write, same-UUID desired-state
repair, `SCRIPT FLUSH` reload, continued use while every Sentinel is down,
unavailable views after the primary also disappears, and recovery to Selector
generation `1 -> 2 -> 3`. Its C#-owned fixture uses a five-second Sentinel
`down-after` so waiting for the managed view to declare unavailability does not
itself disqualify the only surviving replica. This is independent Linux x64
Standalone/Sentinel evidence, not a TLS, platform, package, NativeAOT,
performance, or endurance claim.
