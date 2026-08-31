using Verdandi.Internal;

namespace Verdandi.Catalog;

/// <summary>
/// 拥有一个常驻 Catalog 监听任务和至多一个临时同步任务，并发布完整本地 Entry 视图。
/// </summary>
public sealed unsafe class CatalogSubscriber : IDisposable
{
    private readonly CatalogSubscriberHandle _handle;
    private readonly SafeHandleLease<CatalogClientHandle> _ownerLease;
    private int _disposed;

    /// <summary>
    /// 接管完成初始同步的 Subscriber 和领域生命周期引用。
    /// </summary>
    /// <param name="handle">非零 Subscriber 句柄。</param>
    /// <param name="ownerLease">保证 Subscriber 先于 Catalog Client 释放的引用。</param>
    private CatalogSubscriber(CatalogSubscriberHandle handle, SafeHandleLease<CatalogClientHandle> ownerLease)
    {
        _handle = handle;
        _ownerLease = ownerLease;
    }

    /// <summary>
    /// 编码完整覆盖范围并创建 Subscriber；返回前完成订阅确认、权威同步和同连接栅栏。
    /// </summary>
    /// <param name="owner">仍开放的 Catalog 领域。</param>
    /// <param name="subscription">不可为空的订阅范围。</param>
    /// <returns>Subscriber 或配置/同步错误。</returns>
    internal static Result<CatalogSubscriber> Create(CatalogClient owner, CatalogSubscription subscription)
    {
        ArgumentNullException.ThrowIfNull(owner);
        ArgumentNullException.ThrowIfNull(subscription);
        if (!owner.IsUsable || !SafeHandleLease<CatalogClientHandle>.TryCreate(owner.Handle, out var lease))
        {
            return Result<CatalogSubscriber>.Failure(new VerdandiError("invalid", "catalog"));
        }

        var parts = new byte[subscription.Parts.Count][];
        for (var index = 0; index < subscription.Parts.Count; index++)
        {
            var encoded = Interop.EncodeUtf8(subscription.Parts[index], "part");
            if (!encoded.IsSuccess)
            {
                lease.Dispose();
                return Result<CatalogSubscriber>.Failure(encoded.Error!);
            }

            parts[index] = encoded.Value;
        }

        var paths = new EncodedCatalogPath[subscription.Paths.Count];
        for (var index = 0; index < subscription.Paths.Count; index++)
        {
            var encoded = CatalogPublisher.EncodePath(subscription.Paths[index]);
            if (!encoded.IsSuccess)
            {
                lease.Dispose();
                return Result<CatalogSubscriber>.Failure(encoded.Error!);
            }

            paths[index] = encoded.Value;
        }

        try
        {
            using var nativeLease = new NativeCatalogSubscriptionLease(subscription.Zone, parts, paths);
            var native = nativeLease.Subscription;
            NativeError error = default;
            nint output = nint.Zero;
            if (NativeMethods.CatalogSubscriberCreate(owner.Handle, &native, &output, &error) == 0)
            {
                lease.Dispose();
                return Result<CatalogSubscriber>.Failure(error.ToManaged());
            }

            var handle = new CatalogSubscriberHandle(output);
            if (handle.IsInvalid)
            {
                handle.Dispose();
                lease.Dispose();
                return Result<CatalogSubscriber>.Failure(new VerdandiError("corrupt", "subscriber"));
            }

            return Result<CatalogSubscriber>.Success(new CatalogSubscriber(handle, lease));
        }
        catch (OverflowException exception)
        {
            lease.Dispose();
            return Result<CatalogSubscriber>.Failure(new VerdandiError("capacity", "subscription", exception.Message));
        }
        catch (OutOfMemoryException exception)
        {
            lease.Dispose();
            return Result<CatalogSubscriber>.Failure(new VerdandiError("capacity", "subscription", exception.Message));
        }
    }

    /// <summary>
    /// 查找覆盖范围内的稳定 Entry；没有覆盖或不存在是成功空值，不执行 Redis 或磁盘 I/O。
    /// </summary>
    /// <param name="path">精确 Part 和 ID。</param>
    /// <returns>稳定 Entry、成功 null 或稳定错误。</returns>
    public Result<CatalogEntry?> Find(CatalogPath path)
    {
        if (!IsUsable || !SafeHandleLease<CatalogSubscriberHandle>.TryCreate(_handle, out var lease))
        {
            return Result<CatalogEntry?>.Failure(new VerdandiError("invalid", "subscriber"));
        }

        var encoded = CatalogPublisher.EncodePath(path);
        if (!encoded.IsSuccess)
        {
            lease.Dispose();
            return Result<CatalogEntry?>.Failure(encoded.Error!);
        }

        using var part = new NativeBufferLease(encoded.Value.Part);
        using var id = new NativeBufferLease(encoded.Value.Id);
        var nativePath = new NativeCatalogPathView { Part = part.StringView, Id = id.StringView };
        NativeError error = default;
        var found = 0;
        nint output = nint.Zero;
        if (NativeMethods.CatalogSubscriberFind(_handle, nativePath, &found, &output, &error) == 0)
        {
            lease.Dispose();
            return Result<CatalogEntry?>.Failure(error.ToManaged());
        }

        var handle = new CatalogEntryHandle(output);
        if (found == 0)
        {
            handle.Dispose();
            lease.Dispose();
            return Result<CatalogEntry?>.Success(null);
        }

        if (handle.IsInvalid)
        {
            handle.Dispose();
            lease.Dispose();
            return Result<CatalogEntry?>.Failure(new VerdandiError("corrupt", "entry"));
        }

        var actualPart = Interop.CopyString(NativeMethods.CatalogEntryPart(handle), "part");
        var actualId = Interop.CopyString(NativeMethods.CatalogEntryId(handle), "id");
        if (!actualPart.IsSuccess || !actualId.IsSuccess)
        {
            handle.Dispose();
            lease.Dispose();
            return Result<CatalogEntry?>.Failure((actualPart.IsSuccess ? actualId.Error : actualPart.Error)!);
        }

        return Result<CatalogEntry?>.Success(new CatalogEntry(handle, lease, new CatalogPath(actualPart.Value, actualId.Value)));
    }

    /// <summary>
    /// 非阻塞取出一条同步、恢复、检查点或协议诊断；没有诊断是成功的 Available=false。
    /// </summary>
    /// <returns>诊断状态或调用本身错误。</returns>
    public Result<Diagnostic> TryGetError()
    {
        if (!IsUsable)
        {
            return Result<Diagnostic>.Failure(new VerdandiError("invalid", "subscriber"));
        }

        NativeError error = default;
        var available = 0;
        return Interop.Diagnostic(NativeMethods.CatalogSubscriberTryError(_handle, &available, &error), available, error);
    }

    /// <summary>
    /// 关闭常驻监听和当前临时同步任务并等待汇合；重复调用幂等。
    /// </summary>
    /// <returns>关闭结果。</returns>
    public Result Close()
    {
        if (!IsUsable)
        {
            return Result.Failure(new VerdandiError("invalid", "subscriber"));
        }

        NativeError error = default;
        return Interop.Status(NativeMethods.CatalogSubscriberClose(_handle, &error), error);
    }

    /// <summary>
    /// 最佳努力 Close，释放 Subscriber SafeHandle 并归还 Catalog 领域生命周期引用。
    /// </summary>
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        if (!_handle.IsClosed && !_handle.IsInvalid)
        {
            NativeError error = default;
            _ = NativeMethods.CatalogSubscriberClose(_handle, &error);
        }

        _handle.Dispose();
        _ownerLease.Dispose();
        GC.SuppressFinalize(this);
    }

    /// <summary>返回当前包装是否仍可访问本地 Subscriber 视图。</summary>
    private bool IsUsable => Volatile.Read(ref _disposed) == 0 && !_handle.IsClosed && !_handle.IsInvalid;
}

/// <summary>
/// 保持 Subscriber 和底层稳定 Entry 同时存活，并从同一个不可变状态加载强类型值。
/// </summary>
public sealed unsafe class CatalogEntry : IDisposable
{
    private readonly CatalogEntryHandle _handle;
    private readonly SafeHandleLease<CatalogSubscriberHandle> _ownerLease;
    private int _disposed;

    /// <summary>
    /// 接管稳定 Entry、Subscriber 生命周期引用和已经复制的 Path。
    /// </summary>
    /// <param name="handle">非零 Entry 句柄。</param>
    /// <param name="ownerLease">保证 Entry 先于 Subscriber 释放的引用。</param>
    /// <param name="path">原生 Entry 返回的拥有型 Path。</param>
    internal CatalogEntry(CatalogEntryHandle handle, SafeHandleLease<CatalogSubscriberHandle> ownerLease, CatalogPath path)
    {
        _handle = handle;
        _ownerLease = ownerLease;
        Path = path;
    }

    /// <summary>返回 Entry 的稳定拥有型 Path。</summary>
    public CatalogPath Path { get; }

    /// <summary>返回稳定小写状态；Dispose 后返回 closed。</summary>
    public string Status => IsUsable ? Interop.CopyCString(NativeMethods.CatalogEntryStatus(_handle), "closed") : "closed";

    /// <summary>返回最后已知完整 revision；Dispose 后为零。</summary>
    public ulong Revision => IsUsable ? NativeMethods.CatalogEntryRevision(_handle) : 0;

    /// <summary>返回当前不可变状态是否完成权威同步；Dispose 后为假。</summary>
    public bool IsSynchronized => IsUsable && NativeMethods.CatalogEntryIsSynchronized(_handle) != 0;

    /// <summary>
    /// 从同一个不可变 Entry 状态读取 revision、状态、同步标记和完整值；不执行 Redis 或磁盘 I/O。
    /// </summary>
    /// <typeparam name="T">实现显式 Fields Codec 的应用值类型。</typeparam>
    /// <returns>强类型快照或稳定边界/Codec 错误。</returns>
    public Result<CatalogSnapshot<T>> Load<T>()
        where T : IFieldValue<T>
    {
        if (!IsUsable)
        {
            return Result<CatalogSnapshot<T>>.Failure(new VerdandiError("invalid", "entry"));
        }

        NativeError error = default;
        ulong revision = 0;
        nint status = nint.Zero;
        var synchronized = 0;
        var present = 0;
        nint output = nint.Zero;
        if (NativeMethods.CatalogEntryLoad(_handle, &revision, &status, &synchronized, &present, &output, &error) == 0)
        {
            return Result<CatalogSnapshot<T>>.Failure(error.ToManaged());
        }

        using var fields = new FieldSetHandle(output);
        var statusValue = Interop.CopyCString(status, "closed");
        if (present == 0)
        {
            return Result<CatalogSnapshot<T>>.Success(new CatalogSnapshot<T>(revision, statusValue, synchronized != 0, false, default));
        }

        if (fields.IsInvalid)
        {
            return Result<CatalogSnapshot<T>>.Failure(new VerdandiError("corrupt", "fields"));
        }

        var copied = Interop.CopyFieldSet(fields);
        if (!copied.IsSuccess)
        {
            return Result<CatalogSnapshot<T>>.Failure(copied.Error!);
        }

        var decoded = Interop.DecodeValue<T>(copied.Value);
        return decoded.IsSuccess
            ? Result<CatalogSnapshot<T>>.Success(new CatalogSnapshot<T>(revision, statusValue, synchronized != 0, true, decoded.Value))
            : Result<CatalogSnapshot<T>>.Failure(decoded.Error!);
    }

    /// <summary>
    /// 释放稳定 Entry 句柄并归还 Subscriber 生命周期引用；不影响 Subscriber 内部 Entry 身份。
    /// </summary>
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        _handle.Dispose();
        _ownerLease.Dispose();
        GC.SuppressFinalize(this);
    }

    /// <summary>返回当前包装是否仍可访问稳定 Entry。</summary>
    private bool IsUsable => Volatile.Read(ref _disposed) == 0 && !_handle.IsClosed && !_handle.IsInvalid;
}
