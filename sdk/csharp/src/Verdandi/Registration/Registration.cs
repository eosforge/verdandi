using Verdandi.Internal;

namespace Verdandi.Registration;

/// <summary>
/// 拥有一条延迟发布、UUID 稳定且只有一个原生同步任务的强类型 Registration。
/// </summary>
/// <typeparam name="TAttr">首次 Register 后不可改变的完整 Attr 类型。</typeparam>
/// <typeparam name="TData">后续 Update 以完整值提交的 Data 类型。</typeparam>
public sealed unsafe class Registration<TAttr, TData> : IDisposable
    where TAttr : IFieldValue<TAttr>
    where TData : IFieldValue<TData>
{
    private readonly RegistrationHandle _handle;
    private readonly SafeHandleLease<RegistrationClientHandle> _ownerLease;
    private int _disposed;

    /// <summary>
    /// 接管原生 Registration、领域生命周期引用和已经复制的稳定 UUID。
    /// </summary>
    /// <param name="handle">非零 Registration 句柄。</param>
    /// <param name="ownerLease">保证 Registration 先于领域释放的引用。</param>
    /// <param name="uuid">构造时生成并从原生视图复制的 UUID。</param>
    private Registration(RegistrationHandle handle, SafeHandleLease<RegistrationClientHandle> ownerLease, string uuid)
    {
        _handle = handle;
        _ownerLease = ownerLease;
        Uuid = uuid;
    }

    /// <summary>
    /// 本地构造未发布对象，验证精确时间单位并生成 UUID；该调用不访问 Redis。
    /// </summary>
    /// <param name="owner">仍开放的 Registration 领域。</param>
    /// <param name="options">固定 Type、TTL、初始 Version 和可选续期间隔。</param>
    /// <returns>未发布对象或本地选项错误。</returns>
    internal static Result<Registration<TAttr, TData>> Create(RegistrationClient owner, RegistrationOptions options)
    {
        ArgumentNullException.ThrowIfNull(owner);
        if (!owner.IsUsable || !SafeHandleLease<RegistrationClientHandle>.TryCreate(owner.Handle, out var lease))
        {
            return Result<Registration<TAttr, TData>>.Failure(new VerdandiError("invalid", "registration"));
        }

        var type = Interop.EncodeUtf8(options.Type, "type");
        var ttl = Interop.PositiveMilliseconds(options.Ttl, "ttl");
        if (!type.IsSuccess)
        {
            lease.Dispose();
            return Result<Registration<TAttr, TData>>.Failure(type.Error!);
        }

        if (!ttl.IsSuccess)
        {
            lease.Dispose();
            return Result<Registration<TAttr, TData>>.Failure(ttl.Error!);
        }

        Result<ulong> renew = default;
        if (options.RenewInterval is not null)
        {
            renew = Interop.PositiveMilliseconds(options.RenewInterval.Value, "renew_interval");
            if (!renew.IsSuccess)
            {
                lease.Dispose();
                return Result<Registration<TAttr, TData>>.Failure(renew.Error!);
            }
        }

        using var nativeType = new NativeBufferLease(type.Value);
        var nativeOptions = new NativeRegistrationOptions
        {
            Type = nativeType.StringView,
            TtlMilliseconds = ttl.Value,
            RenewIntervalMilliseconds = options.RenewInterval is null ? 0 : renew.Value,
            Version = options.Version,
            HasRenewInterval = options.RenewInterval is null ? (byte)0 : (byte)1,
        };
        NativeError error = default;
        nint output = nint.Zero;
        if (NativeMethods.RegistrationCreate(owner.Handle, &nativeOptions, &output, &error) == 0)
        {
            lease.Dispose();
            return Result<Registration<TAttr, TData>>.Failure(error.ToManaged());
        }

        var handle = new RegistrationHandle(output);
        if (handle.IsInvalid)
        {
            handle.Dispose();
            lease.Dispose();
            return Result<Registration<TAttr, TData>>.Failure(new VerdandiError("corrupt", "registration"));
        }

        var uuid = Interop.CopyString(NativeMethods.RegistrationUuid(handle), "uuid");
        if (!uuid.IsSuccess)
        {
            handle.Dispose();
            lease.Dispose();
            return Result<Registration<TAttr, TData>>.Failure(uuid.Error!);
        }

        return Result<Registration<TAttr, TData>>.Success(new Registration<TAttr, TData>(handle, lease, uuid.Value));
    }

    /// <summary>返回对象构造时生成且在整个生命周期内不变的 32 位 UUID。</summary>
    public string Uuid { get; }

    /// <summary>返回是否已经完成首次 Redis Register；Dispose 后恒为假。</summary>
    public bool IsRegistered => IsUsable && NativeMethods.RegistrationIsPublished(_handle) != 0;

    /// <summary>返回当前期望内容 revision；未注册或 Dispose 后可能为零。</summary>
    public ulong Revision => IsUsable ? NativeMethods.RegistrationRevision(_handle) : 0;

    /// <summary>返回最近一次 Redis 确认的绝对毫秒时间戳；尚未确认或 Dispose 后为零。</summary>
    public ulong TimestampMilliseconds => IsUsable ? NativeMethods.RegistrationTimestamp(_handle) : 0;

    /// <summary>
    /// 在服务完成自身准备后发布完整 Attr 和 Data，并启动该 UUID 唯一的同步与自动续期任务。
    /// </summary>
    /// <param name="attr">完整不可变身份和放置属性。</param>
    /// <param name="data">完整可更新服务状态。</param>
    /// <returns>首次发布获得 Redis 确认时成功；编码、容量、协议或 Redis 失败均返回错误。</returns>
    public Result Register(TAttr attr, TData data)
    {
        if (!IsUsable)
        {
            return Invalid();
        }

        var encodedAttr = Interop.EncodeValue(attr);
        if (!encodedAttr.IsSuccess)
        {
            return Result.Failure(encodedAttr.Error!);
        }

        var encodedData = Interop.EncodeValue(data);
        if (!encodedData.IsSuccess)
        {
            return Result.Failure(encodedData.Error!);
        }

        using var nativeAttr = new NativeFieldsLease(encodedAttr.Value);
        using var nativeData = new NativeFieldsLease(encodedData.Value);
        NativeError error = default;
        return Interop.Status(NativeMethods.RegistrationPublish(_handle, nativeAttr.View, nativeData.View, &error), error);
    }

    /// <summary>
    /// 提交一个完整期望 Data；原生核心比较已拥有状态、合并积压字段并只发送实际变化。
    /// </summary>
    /// <param name="data">字段名集合必须与首次 Register 完全一致的完整 Data。</param>
    /// <returns>该调用所属合并批次获得确认时成功，或稳定编码/协议/Redis 错误。</returns>
    public Result Update(TData data)
    {
        if (!IsUsable)
        {
            return Invalid();
        }

        var encoded = Interop.EncodeValue(data);
        if (!encoded.IsSuccess)
        {
            return Result.Failure(encoded.Error!);
        }

        using var native = new NativeFieldsLease(encoded.Value);
        NativeError error = default;
        return Interop.Status(NativeMethods.RegistrationUpdate(_handle, native.View, &error), error);
    }

    /// <summary>
    /// 只修改正整数应用 Version，并产生一个内容 revision；Data 保持不变。
    /// </summary>
    /// <param name="version">新的正整数应用版本。</param>
    /// <returns>Redis 确认结果或稳定错误。</returns>
    public Result SetVersion(ulong version)
    {
        if (!IsUsable)
        {
            return Invalid();
        }

        NativeError error = default;
        return Interop.Status(NativeMethods.RegistrationSetVersion(_handle, version, &error), error);
    }

    /// <summary>
    /// 用一个内容 revision 原子修改 Version 和完整期望 Data，避免观察到两次独立内容变更。
    /// </summary>
    /// <param name="version">新的正整数应用版本。</param>
    /// <param name="data">字段名集合固定的完整 Data。</param>
    /// <returns>Redis 确认结果或稳定错误。</returns>
    public Result UpdateContent(ulong version, TData data)
    {
        if (!IsUsable)
        {
            return Invalid();
        }

        var encoded = Interop.EncodeValue(data);
        if (!encoded.IsSuccess)
        {
            return Result.Failure(encoded.Error!);
        }

        using var native = new NativeFieldsLease(encoded.Value);
        NativeError error = default;
        return Interop.Status(NativeMethods.RegistrationUpdateContent(_handle, version, native.View, &error), error);
    }

    /// <summary>
    /// 显式刷新 timestamp 和 TTL，不修改 Data 或内容 revision；正常自动续期仍由原生唯一任务负责。
    /// </summary>
    /// <returns>Redis 已确认续期时成功，或稳定错误。</returns>
    public Result Renew()
    {
        if (!IsUsable)
        {
            return Invalid();
        }

        NativeError error = default;
        return Interop.Status(NativeMethods.RegistrationRenew(_handle, &error), error);
    }

    /// <summary>
    /// 非阻塞取出一条后台同步诊断；没有诊断是成功的 Available=false。
    /// </summary>
    /// <returns>诊断状态，或调用本身失败的错误。</returns>
    public Result<Diagnostic> TryGetError()
    {
        if (!IsUsable)
        {
            return Result<Diagnostic>.Failure(new VerdandiError("invalid", "registration"));
        }

        NativeError error = default;
        var available = 0;
        return Interop.Diagnostic(NativeMethods.RegistrationTryError(_handle, &available, &error), available, error);
    }

    /// <summary>
    /// 终止、排空并尽力删除 Redis Registration；Register 前调用只终止本地对象，重复调用由核心保证幂等。
    /// </summary>
    /// <returns>终止清理结果。</returns>
    public Result Unregister()
    {
        if (!IsUsable)
        {
            return Invalid();
        }

        NativeError error = default;
        return Interop.Status(NativeMethods.RegistrationClose(_handle, &error), error);
    }

    /// <summary>
    /// 最佳努力 Unregister，释放原生句柄并归还领域生命周期引用；需要观察清理失败时应先显式调用 <see cref="Unregister"/>。
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
            _ = NativeMethods.RegistrationClose(_handle, &error);
        }

        _handle.Dispose();
        _ownerLease.Dispose();
        GC.SuppressFinalize(this);
    }

    /// <summary>返回当前包装是否仍可传给原生 Registration 操作。</summary>
    private bool IsUsable => Volatile.Read(ref _disposed) == 0 && !_handle.IsClosed && !_handle.IsInvalid;

    /// <summary>构造失效 Registration 的统一错误结果。</summary>
    /// <returns>invalid/registration。</returns>
    private static Result Invalid() => Result.Failure(new VerdandiError("invalid", "registration"));
}
