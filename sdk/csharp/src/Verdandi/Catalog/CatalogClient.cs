using Verdandi.Internal;

namespace Verdandi.Catalog;

/// <summary>
/// 拥有一个 Zone 的 Catalog 配置、Subscriber 和检查点资源，并共享根 Client 的 Redis 传输。
/// </summary>
public sealed unsafe class CatalogClient : IDisposable
{
    private readonly CatalogClientHandle _handle;
    private readonly SafeHandleLease<ClientHandle> _rootLease;
    private int _disposed;

    /// <summary>
    /// 接管 Catalog 领域句柄和根生命周期引用。
    /// </summary>
    /// <param name="handle">非零 Catalog Client 句柄。</param>
    /// <param name="rootLease">保证领域先于根句柄释放的引用。</param>
    private CatalogClient(CatalogClientHandle handle, SafeHandleLease<ClientHandle> rootLease)
    {
        _handle = handle;
        _rootLease = rootLease;
    }

    /// <summary>
    /// 使用根 JSON 中的 catalog 配置打开 Zone 子域；缺少配置时返回 missing。
    /// </summary>
    /// <param name="root">仍开放的根 Client；当前对象不取得其关闭权限。</param>
    /// <returns>Catalog Client，或配置/Redis 错误。</returns>
    public static Result<CatalogClient> Open(Client root)
    {
        ArgumentNullException.ThrowIfNull(root);
        if (!root.IsUsable || !SafeHandleLease<ClientHandle>.TryCreate(root.Handle, out var lease))
        {
            return Result<CatalogClient>.Failure(new VerdandiError("invalid", "client"));
        }

        NativeError error = default;
        nint output = nint.Zero;
        if (NativeMethods.CatalogClientOpen(root.Handle, &output, &error) == 0)
        {
            lease.Dispose();
            return Result<CatalogClient>.Failure(error.ToManaged());
        }

        if (output == nint.Zero)
        {
            lease.Dispose();
            return Result<CatalogClient>.Failure(new VerdandiError("corrupt", "catalog"));
        }

        return Result<CatalogClient>.Success(new CatalogClient(new CatalogClientHandle(output), lease));
    }

    /// <summary>返回领域是否仍接纳 Publisher 和 Subscriber；Dispose 后恒为假。</summary>
    public bool IsOpen => IsUsable && NativeMethods.CatalogClientIsOpen(_handle) != 0;

    /// <summary>
    /// 创建一个没有后台任务的轻量 Publisher；每次写入仍由原生核心执行一个原子 Lua 操作。
    /// </summary>
    /// <returns>Publisher 或领域生命周期错误。</returns>
    public Result<CatalogPublisher> NewPublisher() => CatalogPublisher.Create(this);

    /// <summary>
    /// 建立订阅并在返回前完成确认、权威同步和同连接栅栏。
    /// </summary>
    /// <param name="subscription">构造时已复制的覆盖范围。</param>
    /// <returns>可立即 Find 的 Subscriber，或同步错误。</returns>
    public Result<CatalogSubscriber> NewSubscriber(CatalogSubscription subscription) => CatalogSubscriber.Create(this, subscription);

    /// <summary>
    /// 关闭 Subscriber 和检查点资源但不关闭根传输；重复调用由原生核心保证幂等。
    /// </summary>
    /// <returns>关闭结果。</returns>
    public Result Close()
    {
        if (!IsUsable)
        {
            return Result.Failure(new VerdandiError("invalid", "catalog"));
        }

        NativeError error = default;
        return Interop.Status(NativeMethods.CatalogClientClose(_handle, &error), error);
    }

    /// <summary>
    /// 最佳努力 Close，释放领域 SafeHandle，并在子对象释放后归还根生命周期引用。
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
            _ = NativeMethods.CatalogClientClose(_handle, &error);
        }

        _handle.Dispose();
        _rootLease.Dispose();
        GC.SuppressFinalize(this);
    }

    /// <summary>返回供 Publisher/Subscriber 建立释放顺序引用的内部句柄。</summary>
    internal CatalogClientHandle Handle => _handle;

    /// <summary>返回当前领域包装是否仍可用于创建子对象。</summary>
    internal bool IsUsable => Volatile.Read(ref _disposed) == 0 && !_handle.IsClosed && !_handle.IsInvalid;
}
