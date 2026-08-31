using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Verdandi.Catalog;
using Verdandi.Internal;
using Verdandi.Registration;

namespace Verdandi.Tests;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            if (args is ["--peer", ..])
            {
                var peerConfiguration = RedisTestConfiguration.Parse(args[1..]) ??
                                        throw new InvalidOperationException("The Sentinel peer requires a configuration.");
                RunSentinelPeer(peerConfiguration);
                return 0;
            }

            RunOffline();
            var redis = RedisTestConfiguration.Parse(args);
            if (redis is not null)
            {
                RunRedis(redis.Value);
            }

            Console.WriteLine(redis is null ? "Verdandi C# offline tests passed." : "Verdandi C# offline and Redis tests passed.");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }

    private static void RunOffline()
    {
        Check(!default(Result).IsSuccess, "default Result must fail");
        Check(default(Result).Error?.Field == "result", "default Result error");
        Check(Result.Success().IsSuccess && Result.Success().Error is null, "Result success state");
        var failure = Result<int>.Failure(new VerdandiError("stale", "revision", "changed", 41));
        Check(!failure.TryGetValue(out var failedValue, out var failedError), "Result<T> failure branch");
        Check(failedValue == 0 && failedError?.Revision == 41, "Result<T> failure values");
        Check(
            failure.Error?.ToString() == "stale field=revision revision=41 detail=changed",
            "stable error formatting");
        Check(!default(Result<int>).IsSuccess, "default Result<T> must fail");
        Check(Result<int>.Success(7).Value == 7, "Result<T> success");
        Check(Result<int>.Success(7).TryGetValue(out var successfulValue, out var successfulError), "Result<T> TryGetValue success");
        Check(successfulValue == 7 && successfulError is null, "Result<T> success values");
        Expect<InvalidOperationException>(() => _ = failure.Value, "failed Result<T>.Value throws");

        var source = new byte[] { 1, 2, 3 };
        var fields = Require(
            Fields.Create([new Field("zero", "0"u8.ToArray()), new Field("bytes", source)]),
            "fields create");
        source[0] = 9;
        Check(Require(fields.GetBytes("bytes"), "bytes").SequenceEqual(new byte[] { 1, 2, 3 }), "Fields owns values");
        var detached = fields.ToArray();
        Check(detached[0].Name == "bytes" && detached[1].Name == "zero", "Fields deterministic ordering");
        Check(MemoryMarshal.TryGetArray(detached[0].Value, out var detachedBytes), "detached value storage");
        detachedBytes.Array![detachedBytes.Offset] = 8;
        Check(Require(fields.GetBytes("bytes"), "bytes")[0] == 1, "Fields ToArray detaches values");
        Check(Fields.Empty.Count == 0 && ReferenceEquals(Fields.Empty.EncodeFields().Value, Fields.Empty), "Fields empty identity");
        Check(!fields.GetBytes("missing").IsSuccess, "missing bytes rejected");
        Check(!fields.GetString("missing").IsSuccess, "missing string rejected");
        Check(
            !Require(Fields.Create([new Field("utf8", new byte[] { 0xc3, 0x28 })]), "invalid UTF-8 fields")
                .GetString("utf8").IsSuccess,
            "invalid UTF-8 rejected");
        Check(Require(fields.GetUInt64("zero"), "zero") == 0, "canonical zero");
        Check(!Require(Fields.Create([new Field("bad", "00"u8.ToArray())]), "bad fields").GetUInt64("bad").IsSuccess, "leading zero rejected");
        Check(!Require(Fields.Create([new Field("bad", "-0"u8.ToArray())]), "bad fields").GetInt64("bad").IsSuccess, "negative zero rejected");
        Check(!Require(Fields.Create([new Field("bad", "+1"u8.ToArray())]), "bad fields").GetInt64("bad").IsSuccess, "positive sign rejected");
        Check(
            !Require(Fields.Create([new Field("bad", "18446744073709551616"u8.ToArray())]), "bad fields")
                .GetUInt64("bad").IsSuccess,
            "UInt64 overflow rejected");
        Check(
            !Require(Fields.Create([new Field("bad", "9223372036854775808"u8.ToArray())]), "bad fields")
                .GetInt64("bad").IsSuccess,
            "Int64 overflow rejected");
        Check(!Require(Fields.Create([new Field("bad", "True"u8.ToArray())]), "bad fields").GetBoolean("bad").IsSuccess, "noncanonical Boolean rejected");
        Check(
            !Fields.Create([new Field("x", Array.Empty<byte>()), new Field("x", Array.Empty<byte>())]).IsSuccess,
            "duplicate fields rejected");
        Check(!Fields.Create([new Field(null!, Array.Empty<byte>())]).IsSuccess, "null field name rejected");

        var builder = Fields.CreateBuilder();
        Require(builder.Add("text", "hello"), "add text");
        Require(builder.Add("flag", true), "add bool");
        Require(builder.Add("signed", long.MinValue), "add signed");
        Require(builder.Add("unsigned", ulong.MaxValue), "add unsigned");
        var built = Require(builder.Build(), "build");
        Check(Require(built.GetString("text"), "text") == "hello", "text round trip");
        Check(Require(built.GetBoolean("flag"), "flag"), "bool round trip");
        Check(Require(built.GetInt64("signed"), "signed") == long.MinValue, "signed round trip");
        Check(Require(built.GetUInt64("unsigned"), "unsigned") == ulong.MaxValue, "unsigned round trip");
        Check(!builder.Add("text", "again").IsSuccess, "builder duplicate rejected");
        Check(!builder.Add("invalid", "\ud800").IsSuccess, "invalid UTF-16 rejected");
        Require(builder.Add("later", 1L), "builder remains usable");
        Check(built.Count == 4 && Require(builder.Build(), "rebuilt fields").Count == 5, "Build returns an immutable snapshot");

        var attr = new ProxyAttr("east", "2026.08.31");
        var decodedAttr = Require(ProxyAttr.DecodeFields(Require(attr.EncodeFields(), "attr encode")), "attr decode");
        Check(decodedAttr == attr, "typed Attr round trip");
        var data = new ProxyData("127.0.0.1:8080", 17);
        var decodedData = Require(ProxyData.DecodeFields(Require(data.EncodeFields(), "data encode")), "data decode");
        Check(decodedData == data, "typed Data round trip");

        var parts = new[] { "routing" };
        var paths = new[] { new CatalogPath("routing", "primary") };
        var subscription = new CatalogSubscription(parts: parts, paths: paths);
        parts[0] = "changed";
        paths[0] = new CatalogPath("changed", "changed");
        Check(subscription.Parts[0] == "routing" && subscription.Paths[0].Id == "primary", "Catalog subscription owns inputs");
        var absent = new CatalogSnapshot<int>(4, "deleted", true, false, 0);
        Check(!absent.HasValue && absent.Revision == 4, "Catalog absent snapshot metadata");
        Expect<InvalidOperationException>(() => _ = absent.Value, "Catalog absent Value throws");

        if (Environment.Is64BitProcess)
        {
            Check(Unsafe.SizeOf<NativeStringView>() == 16, "native string layout");
            Check(Unsafe.SizeOf<NativeFieldView>() == 32, "native field layout");
            Check(Unsafe.SizeOf<NativeError>() == 800, "native error layout");
            Check(Unsafe.SizeOf<NativeRegistrationOptions>() == 48, "registration options layout");
            Check(Unsafe.SizeOf<NativeRegistrationMetadata>() == 48, "registration metadata layout");
            Check(Unsafe.SizeOf<NativeCatalogPathView>() == 32, "catalog path layout");
            Check(Unsafe.SizeOf<NativeCatalogSubscription>() == 40, "catalog subscription layout");
            Check(Marshal.OffsetOf<NativeError>(nameof(NativeError.HasRevision)).ToInt64() == 8, "native error flag offset");
        }
    }

    private static void RunRedis(RedisTestConfiguration testConfiguration)
    {
        var registrationZone = testConfiguration.RegistrationZone;
        var catalogZone = testConfiguration.CatalogZone;

        using var client = Require(Client.Open(testConfiguration.Json), "client open");
        Require(client.Ping(), "client ping");
        Check(!Client.Open("{}").IsSuccess, "invalid root configuration rejected");

        var rawKey = $"verdandi:csharp:{registrationZone}:key";
        var rawHash = $"verdandi:csharp:{registrationZone}:hash";
        try
        {
            Check(Require(client.LoadKey(rawKey), "missing key load") is null, "missing key is null");
            Check(!Require(client.ContainsKey(rawKey), "missing key contains"), "missing key does not exist");
            Check(!Require(client.EraseKey(rawKey), "missing key erase"), "missing key erase is false");
            Check(!Require(client.ExpireKey(rawKey, TimeSpan.FromSeconds(5)), "missing key expire"), "missing key expire is false");
            Require(client.StoreKey(rawKey, "persistent"u8), "key store without TTL");
            Check(Require(client.LoadKey(rawKey), "persistent key load")?.SequenceEqual("persistent"u8.ToArray()) == true, "key no-TTL round trip");
            Require(client.StoreKey(rawKey, "payload"u8, TimeSpan.FromSeconds(5)), "key store");
            Check(Require(client.LoadKey(rawKey), "key load")?.SequenceEqual("payload"u8.ToArray()) == true, "key round trip");
            Check(Require(client.ContainsKey(rawKey), "key contains"), "key exists");
            Check(Require(client.ExpireKey(rawKey, TimeSpan.FromSeconds(5)), "key expire"), "key TTL changed");

            var rawBuilder = Fields.CreateBuilder();
            Require(rawBuilder.Add("power", 1L), "hash field");
            Require(rawBuilder.Add("region", "east"), "hash region");
            Require(client.StoreHash(rawHash, Require(rawBuilder.Build(), "hash fields")), "hash store");
            Check(Require(client.GetHashSize(rawHash), "hash size") == 2, "hash size value");
            Check(Require(client.LoadHash(rawHash), "hash load").GetInt64("power").Value == 1, "hash round trip");
            Check(Require(client.ContainsHashField(rawHash, "power"), "hash contains"), "hash field exists");
            Check(!Require(client.ContainsHashField(rawHash, "missing"), "hash missing contains"), "hash field missing");
            Check(Require(client.EraseHashFields(rawHash, new[] { "power", "missing" }), "hash erase") == 1, "hash field erased");
            Check(Require(client.GetHashSize(rawHash), "hash size after erase") == 1, "hash partial erase");

            using var registrationClient = Require(RegistrationClient.Open(client), "registration client");
            Check(
                !registrationClient.NewRegistration<ProxyAttr, ProxyData>(
                    new RegistrationOptions(string.Empty, TimeSpan.FromSeconds(5), 1)).IsSuccess,
                "empty Registration type rejected");
            Check(
                !registrationClient.NewRegistration<ProxyAttr, ProxyData>(
                    new RegistrationOptions("Proxy", TimeSpan.Zero, 1)).IsSuccess,
                "zero Registration TTL rejected");
            Check(
                !registrationClient.NewRegistration<ProxyAttr, ProxyData>(
                    new RegistrationOptions("Proxy", TimeSpan.FromSeconds(5), 0)).IsSuccess,
                "zero Registration version rejected");
            using var registration = Require(
                registrationClient.NewRegistration<ProxyAttr, ProxyData>(
                    new RegistrationOptions("Proxy", TimeSpan.FromSeconds(5), 1, TimeSpan.FromSeconds(1))),
                "registration create");
            Check(!registration.IsRegistered, "registration delayed publish");
            Check(registration.Uuid.Length == 32, "registration UUID");
            Check(!registration.Update(new ProxyData("127.0.0.1:8080", 1)).IsSuccess, "Update before Register rejected");
            Check(!registration.Renew().IsSuccess, "Renew before Register rejected");
            Require(registration.Register(new ProxyAttr("east", "2026.08.31"), new ProxyData("127.0.0.1:8080", 1)), "register");
            Check(registration.IsRegistered && registration.Revision == 1 && registration.TimestampMilliseconds != 0, "registration state");
            Check(
                !registration.Register(new ProxyAttr("east", "2026.08.31"), new ProxyData("127.0.0.1:8080", 1)).IsSuccess,
                "duplicate Register rejected");

            using var selector = Require(registrationClient.NewSelector<ProxyAttr, ProxyData>("Proxy"), "selector create");
            Choice captured = default;
            var selected = Require(
                selector.One(
                    candidates =>
                    {
                        Check(candidates.Count == 1, "selector candidate count");
                        Check(!candidates.Get(candidates.Count).IsSuccess, "selector bounds rejected");
                        Check(!candidates.Mutate(default, new ProxyData("invalid", 0)).IsSuccess, "default Choice rejected");
                        var first = candidates.Get(0);
                        if (!first.IsSuccess)
                        {
                            return Result<Choice?>.Failure(first.Error!);
                        }

                        captured = first.Value.Choice;
                        var mutated = candidates.Mutate(first.Value.Choice, first.Value.Data with { Power = 2 });
                        return mutated.IsSuccess ? Result<Choice?>.Success(first.Value.Choice) : Result<Choice?>.Failure(mutated.Error!);
                    }),
                "selector one");
            Check(selected is not null && selected.Value.Data.Power == 2, "selector local prediction");
            Check(Require(selector.One(_ => Result<Choice?>.Success(null)), "selector empty One") is null, "empty One selection");
            Check(
                Require(selector.Any(_ => Result<IReadOnlyList<Choice>>.Success(Array.Empty<Choice>())), "selector empty Any").Count == 0,
                "empty Any selection");
            var policyFailure = selector.One(_ => Result<Choice?>.Failure(new VerdandiError("unavailable", "policy")));
            Check(!policyFailure.IsSuccess && policyFailure.Error?.Field == "policy", "policy failure propagated");
            var staleChoice = selector.One(_ => Result<Choice?>.Success(captured));
            Check(!staleChoice.IsSuccess && staleChoice.Error?.Code == "contract", "stale Choice rejected");
            var callbackFailure = selector.One(_ => throw new InvalidOperationException("policy failure"));
            Check(!callbackFailure.IsSuccess && callbackFailure.Error?.Field == "callback", "callback exception translated");

            var any = Require(
                selector.Any(
                    candidates =>
                    {
                        var first = candidates.Get(0);
                        return first.IsSuccess
                            ? Result<IReadOnlyList<Choice>>.Success(new[] { first.Value.Choice })
                            : Result<IReadOnlyList<Choice>>.Failure(first.Error!);
                    }),
                "selector any");
            Check(any.Count == 1, "selector any result");
            var duplicate = selector.Any(
                candidates =>
                {
                    var first = candidates.Get(0);
                    return first.IsSuccess
                        ? Result<IReadOnlyList<Choice>>.Success(new[] { first.Value.Choice, first.Value.Choice })
                        : Result<IReadOnlyList<Choice>>.Failure(first.Error!);
                });
            Check(!duplicate.IsSuccess && duplicate.Error?.Code == "contract", "duplicate Any rejected");

            var snapshot = Require(selector.Snapshot(), "selector snapshot");
            Check(snapshot.IsSynchronized && snapshot.Active.Count == 1 && snapshot.Retained.Count == 0, "selector snapshot state");
            Require(registration.Update(new ProxyData("127.0.0.1:8080", 3)), "registration update");
            WaitUntil(
                () =>
                {
                    var result = selector.One(
                        candidates =>
                        {
                            var first = candidates.Get(0);
                            if (!first.IsSuccess)
                            {
                                return Result<Choice?>.Failure(first.Error!);
                            }

                            return Result<Choice?>.Success(first.Value.Data.Power == 3 ? first.Value.Choice : null);
                        });
                    return result.IsSuccess && result.Value is not null;
                },
                "selector receives update");
            Require(registration.SetVersion(2), "set version");
            Require(registration.UpdateContent(3, new ProxyData("127.0.0.1:8080", 4)), "update content");
            Require(registration.Renew(), "renew");
            Check(!Require(registration.TryGetError(), "registration diagnostic").Available, "registration diagnostic empty");

            RunRegistrationLimits(registrationClient);
            RunRegistrationConcurrency(registrationClient);
            RunRegistrationFinalizerPressure(client, registrationClient, registrationZone);
            RunSelectorCodecFailures(registrationClient);

            using var catalogClient = Require(CatalogClient.Open(client), "catalog client");
            using var publisher = Require(catalogClient.NewPublisher(), "publisher");
            var path = new CatalogPath("routing", "primary");
            var catalogRevision = Require(publisher.Replace(path, CatalogKind.Map, new CatalogRecord(11, "east")), "catalog replace");
            Check(catalogRevision != 0, "catalog revision");
            using var subscriber = Require(catalogClient.NewSubscriber(new CatalogSubscription(parts: new[] { "routing" })), "subscriber");
            using var entry = Require(subscriber.Find(path), "entry find") ?? throw new InvalidOperationException("entry present");
            var catalogValue = Require(entry.Load<CatalogRecord>(), "entry load");
            Check(catalogValue.HasValue && catalogValue.Value.Power == 11 && catalogValue.Status == "present", "catalog value");

            var patchBuilder = Fields.CreateBuilder();
            Require(patchBuilder.Add("power", 12L), "patch power");
            var patch = Require(patchBuilder.Build(), "patch fields");
            var stalePatch = publisher.Patch(path, catalogRevision + 1, patch);
            Check(
                !stalePatch.IsSuccess && stalePatch.Error?.Code == "stale" && stalePatch.Error.Revision == catalogRevision,
                "Catalog stale Patch reports authoritative revision");
            catalogRevision = Require(publisher.Patch(path, catalogRevision, patch), "catalog patch");
            WaitUntil(
                () =>
                {
                    var current = entry.Load<CatalogRecord>();
                    return current.IsSuccess && current.Value.Revision == catalogRevision && current.Value.HasValue && current.Value.Value.Power == 12;
                },
                "catalog receives patch");
            Check(!entry.Load<ExplodingValue>().IsSuccess, "Catalog decoder exception translated");

            catalogRevision = Require(publisher.Erase(path), "catalog erase");
            WaitUntil(
                () =>
                {
                    var current = entry.Load<CatalogRecord>();
                    return current.IsSuccess && current.Value.Revision == catalogRevision && !current.Value.HasValue && current.Value.Status == "deleted";
                },
                "catalog receives delete");
            Check(!Require(subscriber.TryGetError(), "catalog diagnostic").Available, "catalog diagnostic empty");

            RunCatalogShapesAndLimits(publisher);
            RunHandleLifetime(testConfiguration);
            RunConcurrentDispose(testConfiguration);

            Require(registration.Unregister(), "unregister");
            Require(selector.Close(), "selector close");
            Require(subscriber.Close(), "subscriber close");
            Require(registrationClient.Close(), "registration client close");
            Require(catalogClient.Close(), "catalog client close");
        }
        finally
        {
            _ = client.EraseKey(rawKey);
            _ = client.EraseKey(rawHash);
            _ = client.EraseKey($"verdandi:config:{registrationZone}");
            _ = client.EraseKey($"verdandi:registry:{registrationZone}:Proxy");
            _ = client.EraseKey($"verdandi:registry:{registrationZone}:Finalizer");
            _ = client.EraseKey($"verdandi:catalog:{catalogZone}:@meta");
            _ = client.EraseKey($"verdandi:catalog:{catalogZone}:@live");
            _ = client.EraseKey($"verdandi:catalog:{catalogZone}:@deleted");
            _ = client.EraseKey($"verdandi:catalog:{catalogZone}:@deleted_time");
            _ = client.EraseKey($"verdandi:catalog:{catalogZone}:routing:primary");
            _ = client.EraseKey($"verdandi:catalog:{catalogZone}:routing:primary:@field_revisions");
        }
    }

    private static void RunHandleLifetime(RedisTestConfiguration configuration)
    {
        var root = Require(Client.Open(configuration.Json), "lifetime root open");
        var registrations = Require(RegistrationClient.Open(root), "lifetime Registration client open");
        var registration = Require(
            registrations.NewRegistration<ProxyAttr, ProxyData>(
                new RegistrationOptions("Lifetime", TimeSpan.FromSeconds(5), 1, TimeSpan.FromSeconds(1))),
            "lifetime Registration create");
        root.Dispose();
        var closedRegistration = registration.Register(
            new ProxyAttr("lifetime", "2026.08.31"),
            new ProxyData("127.0.0.1:9900", 1));
        Check(!closedRegistration.IsSuccess && closedRegistration.Error?.Code == "closed", "Registration safely observes disposed root");
        registration.Dispose();
        registrations.Dispose();

        var catalogRoot = Require(Client.Open(configuration.Json), "lifetime Catalog root open");
        var catalog = Require(CatalogClient.Open(catalogRoot), "lifetime Catalog client open");
        var publisher = Require(catalog.NewPublisher(), "lifetime Publisher create");
        var path = new CatalogPath("lifetime", "entry");
        catalogRoot.Dispose();
        var closedPublisher = publisher.Replace(path, CatalogKind.Map, new CatalogRecord(1, "lifetime"));
        Check(!closedPublisher.IsSuccess && closedPublisher.Error?.Code == "closed", "Publisher safely observes disposed root");
        publisher.Dispose();
        catalog.Dispose();
    }

    private static void RunConcurrentDispose(RedisTestConfiguration configuration)
    {
        const int cycles = 4;
        const int workers = 8;
        for (var cycle = 0; cycle < cycles; cycle++)
        {
            var root = Require(Client.Open(configuration.Json), $"dispose pressure root {cycle} open");
            using var ready = new CountdownEvent(workers);
            var tasks = Enumerable.Range(0, workers)
                .Select(
                    _ => Task.Run(
                        () =>
                        {
                            root.Ping();
                            ready.Signal();
                            while (root.Ping().IsSuccess)
                            {
                                Thread.Yield();
                            }
                        }))
                .ToArray();
            Check(ready.Wait(TimeSpan.FromSeconds(5)), "dispose pressure workers ready");
            root.Dispose();
            Task.WaitAll(tasks);
            root.Dispose();
        }
    }

    private static void RunSentinelPeer(RedisTestConfiguration configuration)
    {
        using var root = Require(Client.Open(configuration.Json), "peer root open");
        using var registrations = Require(RegistrationClient.Open(root), "peer Registration client open");
        using var registration = Require(
            registrations.NewRegistration<ProxyAttr, ProxyData>(
                new RegistrationOptions("Sentinel", TimeSpan.FromSeconds(60), 1, TimeSpan.FromSeconds(20))),
            "peer Registration create");
        var address = $"peer-{Environment.ProcessId}";
        long expectedPower = 0;
        Require(
            registration.Register(new ProxyAttr("sentinel", "2026.08.31"), new ProxyData(address, expectedPower)),
            "peer Register");
        using var selector = Require(registrations.NewSelector<ProxyAttr, ProxyData>("Sentinel"), "peer Selector create");
        Console.WriteLine($"READY {registration.Uuid}");
        Console.Out.Flush();

        while (Console.ReadLine() is { } line)
        {
            var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts is ["UPDATE", var powerText] && long.TryParse(powerText, out var power))
            {
                var previousRevision = registration.Revision;
                Require(registration.Update(new ProxyData(address, power)), "peer Update");
                expectedPower = power;
                WaitUntil(
                    () => registration.Revision > previousRevision,
                    "peer Update confirmation",
                    45_000);
                Console.WriteLine($"UPDATED {registration.Revision}");
            }
            else if (parts is ["RENEW"])
            {
                Result renewed = default;
                WaitUntil(
                    () =>
                    {
                        renewed = registration.Renew();
                        return renewed.IsSuccess;
                    },
                    "peer Renew recovery",
                    45_000);
                Console.WriteLine($"RENEWED {registration.Revision}");
            }
            else if (parts is ["CHECK"])
            {
                ulong generation = 0;
                WaitUntil(
                    () =>
                    {
                        var selected = selector.One(
                            candidates =>
                            {
                                for (nuint index = 0; index < candidates.Count; index++)
                                {
                                    var candidate = candidates.Get(index);
                                    if (!candidate.IsSuccess)
                                    {
                                        return Result<Choice?>.Failure(candidate.Error!);
                                    }

                                    if (candidate.Value.Metadata.Uuid == registration.Uuid && candidate.Value.Data.Power == expectedPower)
                                    {
                                        return Result<Choice?>.Success(candidate.Value.Choice);
                                    }
                                }

                                return Result<Choice?>.Success(null);
                            });
                        var snapshot = selector.Snapshot();
                        if (snapshot.IsSuccess)
                        {
                            generation = snapshot.Value.Generation;
                        }

                        return selected.IsSuccess && selected.Value is not null && snapshot.IsSuccess && snapshot.Value.IsSynchronized;
                    },
                    "peer Selector convergence",
                    45_000);
                Console.WriteLine($"CHECKED {generation}");
            }
            else if (parts is ["WAIT_UNSYNC"])
            {
                WaitUntil(
                    () =>
                    {
                        var snapshot = selector.Snapshot();
                        return snapshot.IsSuccess
                            ? !snapshot.Value.IsSynchronized
                            : snapshot.Error?.Code == "unavailable";
                    },
                    "peer unsynchronized state",
                    30_000);
                Console.WriteLine("UNSYNCHRONIZED");
            }
            else if (parts is ["STOP"])
            {
                Require(registration.Unregister(), "peer Unregister");
                Console.WriteLine("STOPPED");
                Console.Out.Flush();
                return;
            }
            else
            {
                throw new InvalidOperationException($"Unknown peer command: {line}");
            }

            Console.Out.Flush();
        }
    }

    private static void RunRegistrationLimits(RegistrationClient client)
    {
        using var exact = Require(
            client.NewRegistration<Fields, Fields>(
                new RegistrationOptions("LimitsExact", TimeSpan.FromSeconds(5), 1, TimeSpan.FromSeconds(1))),
            "exact-limit Registration create");
        Require(exact.Register(CreateFields("a", 16, 128), CreateFields("d", 32, 128)), "exact default Registration limits");
        Require(exact.Unregister(), "exact-limit Registration unregister");

        using var tooMany = Require(
            client.NewRegistration<Fields, Fields>(
                new RegistrationOptions("LimitsFields", TimeSpan.FromSeconds(5), 1, TimeSpan.FromSeconds(1))),
            "over-field Registration create");
        var fieldFailure = tooMany.Register(CreateFields("a", 17, 1), Fields.Empty);
        Check(!fieldFailure.IsSuccess && fieldFailure.Error?.Code == "capacity", "Registration field-count limit");

        using var tooWide = Require(
            client.NewRegistration<Fields, Fields>(
                new RegistrationOptions("LimitsValue", TimeSpan.FromSeconds(5), 1, TimeSpan.FromSeconds(1))),
            "over-value Registration create");
        var valueFailure = tooWide.Register(CreateFields("a", 1, 129), Fields.Empty);
        Check(!valueFailure.IsSuccess && valueFailure.Error?.Code == "capacity", "Registration field-value limit");
    }

    private static void RunRegistrationConcurrency(RegistrationClient client)
    {
        const int registrations = 6;
        const int updates = 32;
        var values = new Registration<ProxyAttr, ProxyData>[registrations];
        try
        {
            for (var index = 0; index < values.Length; index++)
            {
                values[index] = Require(
                    client.NewRegistration<ProxyAttr, ProxyData>(
                        new RegistrationOptions("Concurrent", TimeSpan.FromSeconds(10), 1, TimeSpan.FromSeconds(2))),
                    $"concurrent Registration {index} create");
                Require(
                    values[index].Register(
                        new ProxyAttr($"region-{index}", "2026.08.31"),
                        new ProxyData($"127.0.0.1:{9000 + index}", 0)),
                    $"concurrent Registration {index} register");
            }

            using var selector = Require(client.NewSelector<ProxyAttr, ProxyData>("Concurrent"), "concurrent Selector create");
            WaitUntil(
                () =>
                {
                    var snapshot = selector.Snapshot();
                    return snapshot.IsSuccess && snapshot.Value.IsSynchronized && snapshot.Value.Active.Count == registrations;
                },
                "concurrent Selector initial convergence");

            var updateTasks = values.Select(
                (registration, index) => Task.Run(
                    () =>
                    {
                        for (var power = 1; power <= updates; power++)
                        {
                            Require(registration.Update(new ProxyData($"127.0.0.1:{9000 + index}", power)), "concurrent Update");
                        }

                        Require(
                            registration.Update(new ProxyData($"127.0.0.1:{9000 + index}", 1000 + index)),
                            "concurrent final Update");
                    }))
                .ToArray();
            Task.WaitAll(updateTasks);

            WaitUntil(
                () =>
                {
                    var snapshot = selector.Snapshot();
                    return snapshot.IsSuccess && snapshot.Value.Active.Count == registrations &&
                           snapshot.Value.Active.All(candidate => candidate.Data.Power >= 1000);
                },
                "concurrent Update convergence");

            var selectorTasks = Enumerable.Range(0, 4)
                .Select(
                    _ => Task.Run(
                        () =>
                        {
                            for (var iteration = 0; iteration < 32; iteration++)
                            {
                                var selected = selector.One(
                                    candidates =>
                                    {
                                        var first = candidates.Get(0);
                                        return first.IsSuccess
                                            ? Result<Choice?>.Success(first.Value.Choice)
                                            : Result<Choice?>.Failure(first.Error!);
                                    });
                                Check(selected.IsSuccess && selected.Value is not null, "concurrent Selector One");
                            }
                        }))
                .ToArray();
            Task.WaitAll(selectorTasks);
        }
        finally
        {
            foreach (var registration in values)
            {
                if (registration is null)
                {
                    continue;
                }

                _ = registration.Unregister();
                registration.Dispose();
            }
        }
    }

    private static void RunRegistrationFinalizerPressure(Client root, RegistrationClient client, string zone)
    {
        const int count = 8;
        var abandoned = Enumerable.Range(0, count)
            .Select(index => AbandonRegistration(client, index))
            .ToArray();

        for (var attempt = 0; attempt < 8 && abandoned.Any(value => value.Reference.IsAlive); attempt++)
        {
            GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: true);
            GC.WaitForPendingFinalizers();
        }
        Check(abandoned.All(value => !value.Reference.IsAlive), "abandoned Registration wrappers finalized");

        var registry = $"verdandi:registry:{zone}:Finalizer";
        WaitUntil(
            () => abandoned.All(
                value =>
                {
                    var present = root.ContainsHashField(registry, value.Uuid);
                    return present.IsSuccess && !present.Value;
                }),
            "finalized Registration membership cleanup");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (WeakReference Reference, string Uuid) AbandonRegistration(RegistrationClient client, int index)
    {
        var registration = Require(
            client.NewRegistration<ProxyAttr, ProxyData>(
                new RegistrationOptions("Finalizer", TimeSpan.FromSeconds(10), 1, TimeSpan.FromSeconds(2))),
            $"finalizer Registration {index} create");
        Require(
            registration.Register(
                new ProxyAttr($"finalizer-{index}", "2026.08.31"),
                new ProxyData($"127.0.0.1:{9800 + index}", index)),
            $"finalizer Registration {index} register");
        return (new WeakReference(registration), registration.Uuid);
    }

    private static void RunSelectorCodecFailures(RegistrationClient client)
    {
        using var raw = Require(
            client.NewRegistration<Fields, Fields>(
                new RegistrationOptions("Malformed", TimeSpan.FromSeconds(5), 1, TimeSpan.FromSeconds(1))),
            "malformed Registration create");
        Require(raw.Register(CreateFields("unexpected", 1, 1), CreateFields("unexpected", 1, 1)), "malformed Register");
        using var typed = Require(client.NewSelector<ProxyAttr, ProxyData>("Malformed"), "malformed Selector create");
        var decodeFailure = typed.One(
            candidates =>
            {
                var first = candidates.Get(0);
                return first.IsSuccess
                    ? Result<Choice?>.Success(first.Value.Choice)
                    : Result<Choice?>.Failure(first.Error!);
            });
        Check(!decodeFailure.IsSuccess && decodeFailure.Error?.Code == "invalid", "Selector malformed value rejected");

        using var throwing = Require(
            client.NewSelector<ExplodingValue, ExplodingValue>("Malformed"),
            "throwing Selector create");
        var exceptionFailure = throwing.One(
            candidates =>
            {
                var first = candidates.Get(0);
                return first.IsSuccess
                    ? Result<Choice?>.Success(first.Value.Choice)
                    : Result<Choice?>.Failure(first.Error!);
            });
        Check(!exceptionFailure.IsSuccess && exceptionFailure.Error?.Field == "codec", "Selector decoder exception translated");
        Require(raw.Unregister(), "malformed Registration unregister");
    }

    private static void RunCatalogShapesAndLimits(CatalogPublisher publisher)
    {
        var invalidValue = publisher.Replace(
            new CatalogPath("shapes", "invalid-value"),
            CatalogKind.Value,
            CreateFields("wrong", 1, 1));
        Check(!invalidValue.IsSuccess && invalidValue.Error?.Code == "contract", "Catalog Value shape validation");

        var arrayBuilder = Fields.CreateBuilder();
        Require(arrayBuilder.Add("0", "first"), "Catalog Array index zero");
        Require(arrayBuilder.Add("1", "second"), "Catalog Array index one");
        var arrayPath = new CatalogPath("shapes", "array");
        Require(publisher.Replace(arrayPath, CatalogKind.Array, Require(arrayBuilder.Build(), "Catalog Array fields")), "Catalog Array Replace");
        Require(publisher.Erase(arrayPath), "Catalog Array erase");

        var invalidArrayBuilder = Fields.CreateBuilder();
        Require(invalidArrayBuilder.Add("0", "first"), "invalid Catalog Array index zero");
        Require(invalidArrayBuilder.Add("2", "third"), "invalid Catalog Array index two");
        var invalidArray = publisher.Replace(
            new CatalogPath("shapes", "invalid-array"),
            CatalogKind.Array,
            Require(invalidArrayBuilder.Build(), "invalid Catalog Array fields"));
        Check(!invalidArray.IsSuccess && invalidArray.Error?.Code == "contract", "Catalog Array hole rejected");

        var maximumPath = new CatalogPath("limits", "maximum");
        var maximum = Fields.CreateBuilder();
        Require(maximum.Add("value", new byte[(4 * 1024 * 1024) - "value".Length]), "Catalog maximum field");
        Require(
            publisher.Replace(maximumPath, CatalogKind.Value, Require(maximum.Build(), "Catalog maximum fields")),
            "Catalog 4 MiB record");
        Require(publisher.Erase(maximumPath), "Catalog maximum record erase");

        var oversized = Fields.CreateBuilder();
        Require(oversized.Add("value", new byte[(4 * 1024 * 1024) - "value".Length + 1]), "Catalog oversized field");
        var oversizedResult = publisher.Replace(
            new CatalogPath("limits", "oversized"),
            CatalogKind.Value,
            Require(oversized.Build(), "Catalog oversized fields"));
        Check(!oversizedResult.IsSuccess && oversizedResult.Error?.Code == "capacity", "Catalog 4 MiB ceiling");
    }

    private static Fields CreateFields(string prefix, int count, int size)
    {
        var builder = Fields.CreateBuilder();
        var value = new byte[size];
        for (var index = 0; index < count; index++)
        {
            Require(builder.Add($"{prefix}{index:D2}", value), "test field");
        }

        return Require(builder.Build(), "test fields");
    }

    private static string UniqueZone(string prefix)
    {
        const string letters = "ABCDEFGHIJKLMNOP";
        Span<char> suffix = stackalloc char[10];
        var bytes = Guid.NewGuid().ToByteArray();
        for (var index = 0; index < suffix.Length; index++)
        {
            suffix[index] = letters[bytes[index] & 15];
        }

        return string.Concat(prefix, suffix);
    }

    private readonly record struct RedisTestConfiguration(string Json, string RegistrationZone, string CatalogZone)
    {
        internal static RedisTestConfiguration? Parse(string[] args)
        {
            if (args.Length == 0)
            {
                return null;
            }

            if (args is ["--redis", var address])
            {
                var registrationZone = UniqueZone("CSRegistration");
                var catalogZone = UniqueZone("CSCatalog");
                var json = JsonSerializer.Serialize(
                    new
                    {
                        version = "v1",
                        redis = new { mode = "standalone", addresses = new[] { address } },
                        registration = new { zone = registrationZone, selector = new { sync_timeout_ms = 5000 } },
                        catalog = new { zone = catalogZone, sync_timeout_ms = 5000, max_record_bytes = 4 * 1024 * 1024 },
                    });
                return new RedisTestConfiguration(json, registrationZone, catalogZone);
            }

            if (args is ["--configuration-env", var variable])
            {
                var json = Environment.GetEnvironmentVariable(variable);
                if (string.IsNullOrWhiteSpace(json))
                {
                    throw new InvalidOperationException($"Missing or empty environment variable: {variable}");
                }

                return ParseJson(json);
            }

            if (args is ["--configuration-file", var path])
            {
                return ParseJson(File.ReadAllText(path));
            }

            throw new InvalidOperationException(
                "Usage: Verdandi.Tests [--redis <address> | --configuration-env <variable> | --configuration-file <path>]");
        }

        private static RedisTestConfiguration ParseJson(string json)
        {
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;
            var registrationZone = root.GetProperty("registration").GetProperty("zone").GetString();
            var catalogZone = root.GetProperty("catalog").GetProperty("zone").GetString();
            if (string.IsNullOrWhiteSpace(registrationZone) || string.IsNullOrWhiteSpace(catalogZone))
            {
                throw new InvalidOperationException("The test configuration must contain non-empty Registration and Catalog zones.");
            }

            return new RedisTestConfiguration(json, registrationZone, catalogZone);
        }
    }

    private static void WaitUntil(Func<bool> condition, string step, int timeoutMilliseconds = 5000)
    {
        var deadline = Environment.TickCount64 + timeoutMilliseconds;
        while (Environment.TickCount64 < deadline)
        {
            if (condition())
            {
                return;
            }

            Thread.Sleep(10);
        }

        throw new InvalidOperationException($"Timed out: {step}");
    }

    private static T Require<T>(Result<T> result, string step)
    {
        if (!result.IsSuccess)
        {
            throw new InvalidOperationException($"{step}: {result.Error}");
        }

        return result.Value;
    }

    private static void Require(Result result, string step)
    {
        if (!result.IsSuccess)
        {
            throw new InvalidOperationException($"{step}: {result.Error}");
        }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private static void Expect<TException>(Action action, string message)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException)
        {
            return;
        }

        throw new InvalidOperationException(message);
    }

    private readonly record struct ProxyAttr(string Region, string Build) : IFieldValue<ProxyAttr>
    {
        public Result<Fields> EncodeFields()
        {
            var builder = Fields.CreateBuilder();
            var region = builder.Add("region", Region);
            if (!region.IsSuccess)
            {
                return Result<Fields>.Failure(region.Error!);
            }

            var build = builder.Add("build", Build);
            return build.IsSuccess ? builder.Build() : Result<Fields>.Failure(build.Error!);
        }

        public static Result<ProxyAttr> DecodeFields(Fields fields)
        {
            var region = fields.GetString("region");
            var build = fields.GetString("build");
            return fields.Count == 2 && region.IsSuccess && build.IsSuccess
                ? Result<ProxyAttr>.Success(new ProxyAttr(region.Value, build.Value))
                : Result<ProxyAttr>.Failure(new VerdandiError("invalid", "ProxyAttr"));
        }
    }

    private readonly record struct ProxyData(string Address, long Power) : IFieldValue<ProxyData>
    {
        public Result<Fields> EncodeFields()
        {
            var builder = Fields.CreateBuilder();
            var address = builder.Add("address", Address);
            if (!address.IsSuccess)
            {
                return Result<Fields>.Failure(address.Error!);
            }

            var power = builder.Add("power", Power);
            return power.IsSuccess ? builder.Build() : Result<Fields>.Failure(power.Error!);
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

    private readonly record struct CatalogRecord(long Power, string Region) : IFieldValue<CatalogRecord>
    {
        public Result<Fields> EncodeFields()
        {
            var builder = Fields.CreateBuilder();
            var power = builder.Add("power", Power);
            if (!power.IsSuccess)
            {
                return Result<Fields>.Failure(power.Error!);
            }

            var region = builder.Add("region", Region);
            return region.IsSuccess ? builder.Build() : Result<Fields>.Failure(region.Error!);
        }

        public static Result<CatalogRecord> DecodeFields(Fields fields)
        {
            var power = fields.GetInt64("power");
            var region = fields.GetString("region");
            return fields.Count == 2 && power.IsSuccess && region.IsSuccess
                ? Result<CatalogRecord>.Success(new CatalogRecord(power.Value, region.Value))
                : Result<CatalogRecord>.Failure(new VerdandiError("invalid", "CatalogRecord"));
        }
    }

    private readonly record struct ExplodingValue : IFieldValue<ExplodingValue>
    {
        public Result<Fields> EncodeFields() => throw new InvalidOperationException("intentional codec failure");

        public static Result<ExplodingValue> DecodeFields(Fields fields) =>
            throw new InvalidOperationException("intentional codec failure");
    }
}
