using Verdandi.Internal;

namespace Verdandi.Registration;

/// <summary>
/// 拥有一个 Zone 的 Registration 配置、脚本和领域生命周期，并共享根 Client 的 Redis 传输。
/// </summary>
public sealed unsafe class RegistrationClient : IDisposable
{
    private readonly RegistrationClientHandle _handle;
    private readonly SafeHandleLease<ClientHandle> _rootLease;
    private int _disposed;

    /// <summary>
    /// 接管领域句柄和根 Client 生命周期引用。
    /// </summary>
    /// <param name="handle">非零 Registration Client 句柄。</param>
    /// <param name="rootLease">保证领域句柄先于根句柄释放的引用。</param>
    private RegistrationClient(RegistrationClientHandle handle, SafeHandleLease<ClientHandle> rootLease)
    {
        _handle = handle;
        _rootLease = rootLease;
    }

    /// <summary>
    /// 使用根 JSON 中的 registration 配置打开 Zone 子域，执行 Redis 8 检查、策略引导和脚本准备。
    /// </summary>
    /// <param name="root">仍开放的根 Client；当前对象不取得其关闭权限。</param>
    /// <returns>领域 Client，或 missing/configuration/Redis 错误。</returns>
    public static Result<RegistrationClient> Open(Client root)
    {
        ArgumentNullException.ThrowIfNull(root);
        if (!root.IsUsable || !SafeHandleLease<ClientHandle>.TryCreate(root.Handle, out var lease))
        {
            return Result<RegistrationClient>.Failure(new VerdandiError("invalid", "client"));
        }

        NativeError error = default;
        nint output = nint.Zero;
        if (NativeMethods.RegistrationClientOpen(root.Handle, &output, &error) == 0)
        {
            lease.Dispose();
            return Result<RegistrationClient>.Failure(error.ToManaged());
        }

        if (output == nint.Zero)
        {
            lease.Dispose();
            return Result<RegistrationClient>.Failure(new VerdandiError("corrupt", "registration"));
        }

        return Result<RegistrationClient>.Success(new RegistrationClient(new RegistrationClientHandle(output), lease));
    }

    /// <summary>返回领域是否仍接纳新 Registration 和 Selector；Dispose 后恒为假。</summary>
    public bool IsOpen => IsUsable && NativeMethods.RegistrationClientIsOpen(_handle) != 0;

    /// <summary>
    /// 本地构造一个尚未对外可见的强类型 Registration；不执行 Redis I/O，也不启动同步任务。
    /// </summary>
    /// <typeparam name="TAttr">实现显式 Fields Codec 的不可变 Attr。</typeparam>
    /// <typeparam name="TData">实现显式 Fields Codec 的可更新 Data。</typeparam>
    /// <param name="options">Type、TTL、Version 和可选续期间隔。</param>
    /// <returns>保持 UUID 稳定的未发布 Registration，或本地选项错误。</returns>
    public Result<Registration<TAttr, TData>> NewRegistration<TAttr, TData>(RegistrationOptions options)
        where TAttr : IFieldValue<TAttr>
        where TData : IFieldValue<TData> =>
        Registration<TAttr, TData>.Create(this, options);

    /// <summary>
    /// 为一个 Registry Type 创建 Selector，并在返回前完成初始订阅、分页同步和同连接栅栏。
    /// </summary>
    /// <typeparam name="TAttr">实现显式 Fields Codec 的不可变 Attr。</typeparam>
    /// <typeparam name="TData">实现显式 Fields Codec 的可更新 Data。</typeparam>
    /// <param name="type">要发现的精确 Registry Type。</param>
    /// <returns>已完成初始同步的 Selector，或稳定同步错误。</returns>
    public Result<Selector<TAttr, TData>> NewSelector<TAttr, TData>(string type)
        where TAttr : IFieldValue<TAttr>
        where TData : IFieldValue<TData> =>
        Selector<TAttr, TData>.Create(this, type);

    /// <summary>
    /// 关闭并汇合领域拥有的 Registration、Selector 和配置刷新资源，但不关闭根传输。
    /// </summary>
    /// <returns>完成关闭时成功，或稳定关闭错误。</returns>
    public Result Close()
    {
        if (!IsUsable)
        {
            return Result.Failure(new VerdandiError("invalid", "registration"));
        }

        NativeError error = default;
        return Interop.Status(NativeMethods.RegistrationClientClose(_handle, &error), error);
    }

    /// <summary>
    /// 最佳努力关闭领域，释放当前 SafeHandle，并在所有子句柄释放后归还根生命周期引用。
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
            _ = NativeMethods.RegistrationClientClose(_handle, &error);
        }

        _handle.Dispose();
        _rootLease.Dispose();
        GC.SuppressFinalize(this);
    }

    /// <summary>返回供 Registration/Selector 建立释放顺序引用的内部领域句柄。</summary>
    internal RegistrationClientHandle Handle => _handle;

    /// <summary>返回当前领域包装是否仍可传入原生创建调用。</summary>
    internal bool IsUsable => Volatile.Read(ref _disposed) == 0 && !_handle.IsClosed && !_handle.IsInvalid;
}
