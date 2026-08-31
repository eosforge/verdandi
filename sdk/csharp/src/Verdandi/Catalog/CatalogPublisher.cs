using Verdandi.Internal;

namespace Verdandi.Catalog;

/// <summary>
/// 提供无后台任务的强类型 Catalog Replace、Patch 和 Delete 原子写入。
/// </summary>
public sealed unsafe class CatalogPublisher : IDisposable
{
    private readonly CatalogPublisherHandle _handle;
    private readonly SafeHandleLease<CatalogClientHandle> _ownerLease;
    private int _disposed;

    /// <summary>
    /// 接管 Publisher 句柄和 Catalog 领域生命周期引用。
    /// </summary>
    /// <param name="handle">非零 Publisher 句柄。</param>
    /// <param name="ownerLease">保证 Publisher 先于 Catalog Client 释放的引用。</param>
    private CatalogPublisher(CatalogPublisherHandle handle, SafeHandleLease<CatalogClientHandle> ownerLease)
    {
        _handle = handle;
        _ownerLease = ownerLease;
    }

    /// <summary>
    /// 创建轻量 Publisher，不启动任务或访问 Redis。
    /// </summary>
    /// <param name="owner">仍开放的 Catalog 领域。</param>
    /// <returns>Publisher 或生命周期错误。</returns>
    internal static Result<CatalogPublisher> Create(CatalogClient owner)
    {
        ArgumentNullException.ThrowIfNull(owner);
        if (!owner.IsUsable || !SafeHandleLease<CatalogClientHandle>.TryCreate(owner.Handle, out var lease))
        {
            return Result<CatalogPublisher>.Failure(new VerdandiError("invalid", "catalog"));
        }

        NativeError error = default;
        nint output = nint.Zero;
        if (NativeMethods.CatalogPublisherCreate(owner.Handle, &output, &error) == 0)
        {
            lease.Dispose();
            return Result<CatalogPublisher>.Failure(error.ToManaged());
        }

        var handle = new CatalogPublisherHandle(output);
        if (handle.IsInvalid)
        {
            handle.Dispose();
            lease.Dispose();
            return Result<CatalogPublisher>.Failure(new VerdandiError("corrupt", "publisher"));
        }

        return Result<CatalogPublisher>.Success(new CatalogPublisher(handle, lease));
    }

    /// <summary>
    /// 原子覆盖完整 Value、Array 或 Map，并返回 Redis 分配的新全局 revision。
    /// </summary>
    /// <typeparam name="T">实现显式 Fields Codec 的完整 Catalog 值。</typeparam>
    /// <param name="path">目标 Part 和 ID。</param>
    /// <param name="kind">稳定结构类别。</param>
    /// <param name="value">同步编码后立即释放引用的完整值。</param>
    /// <returns>新 revision，或编码/协议/Redis 错误。</returns>
    public Result<ulong> Replace<T>(CatalogPath path, CatalogKind kind, T value)
        where T : IFieldValue<T>
    {
        if (!IsUsable)
        {
            return Result<ulong>.Failure(new VerdandiError("invalid", "publisher"));
        }

        var encoded = Interop.EncodeValue(value);
        if (!encoded.IsSuccess)
        {
            return Result<ulong>.Failure(encoded.Error!);
        }

        var kindText = kind switch
        {
            CatalogKind.Value => "value",
            CatalogKind.Array => "array",
            CatalogKind.Map => "map",
            _ => null,
        };
        if (kindText is null)
        {
            return Result<ulong>.Failure(new VerdandiError("invalid", "kind"));
        }

        var nativePath = EncodePath(path);
        var nativeKind = Interop.EncodeUtf8(kindText, "kind");
        if (!nativePath.IsSuccess)
        {
            return Result<ulong>.Failure(nativePath.Error!);
        }

        if (!nativeKind.IsSuccess)
        {
            return Result<ulong>.Failure(nativeKind.Error!);
        }

        using var part = new NativeBufferLease(nativePath.Value.Part);
        using var id = new NativeBufferLease(nativePath.Value.Id);
        using var kindValue = new NativeBufferLease(nativeKind.Value);
        using var fields = new NativeFieldsLease(encoded.Value);
        var pathView = new NativeCatalogPathView { Part = part.StringView, Id = id.StringView };
        NativeError error = default;
        ulong revision = 0;
        return NativeMethods.CatalogReplace(_handle, pathView, kindValue.StringView, fields.View, &revision, &error) != 0
            ? Result<ulong>.Success(revision)
            : Result<ulong>.Failure(error.ToManaged());
    }

    /// <summary>
    /// 在 base revision 精确匹配时原子覆盖给定字段；真正局部 Patch 应传入只含目标字段的 Fields 类型。
    /// </summary>
    /// <typeparam name="T">编码 Patch 字段的值类型。</typeparam>
    /// <param name="path">目标 Part 和 ID。</param>
    /// <param name="baseRevision">调用方读取和投影所基于的精确 revision。</param>
    /// <param name="value">要覆盖的字段集合。</param>
    /// <returns>新 revision，或 stale/编码/Redis 错误。</returns>
    public Result<ulong> Patch<T>(CatalogPath path, ulong baseRevision, T value)
        where T : IFieldValue<T>
    {
        if (!IsUsable)
        {
            return Result<ulong>.Failure(new VerdandiError("invalid", "publisher"));
        }

        var encoded = Interop.EncodeValue(value);
        if (!encoded.IsSuccess)
        {
            return Result<ulong>.Failure(encoded.Error!);
        }

        var nativePath = EncodePath(path);
        if (!nativePath.IsSuccess)
        {
            return Result<ulong>.Failure(nativePath.Error!);
        }

        using var part = new NativeBufferLease(nativePath.Value.Part);
        using var id = new NativeBufferLease(nativePath.Value.Id);
        using var fields = new NativeFieldsLease(encoded.Value);
        var pathView = new NativeCatalogPathView { Part = part.StringView, Id = id.StringView };
        NativeError error = default;
        ulong revision = 0;
        return NativeMethods.CatalogPatch(_handle, pathView, baseRevision, fields.View, &revision, &error) != 0
            ? Result<ulong>.Success(revision)
            : Result<ulong>.Failure(error.ToManaged());
    }

    /// <summary>
    /// 原子删除完整 Path、产生 tombstone revision，并让订阅者收到 delete 更新。
    /// </summary>
    /// <param name="path">目标 Part 和 ID。</param>
    /// <returns>新全局 revision，或稳定错误。</returns>
    public Result<ulong> Erase(CatalogPath path)
    {
        if (!IsUsable)
        {
            return Result<ulong>.Failure(new VerdandiError("invalid", "publisher"));
        }

        var nativePath = EncodePath(path);
        if (!nativePath.IsSuccess)
        {
            return Result<ulong>.Failure(nativePath.Error!);
        }

        using var part = new NativeBufferLease(nativePath.Value.Part);
        using var id = new NativeBufferLease(nativePath.Value.Id);
        var pathView = new NativeCatalogPathView { Part = part.StringView, Id = id.StringView };
        NativeError error = default;
        ulong revision = 0;
        return NativeMethods.CatalogErase(_handle, pathView, &revision, &error) != 0
            ? Result<ulong>.Success(revision)
            : Result<ulong>.Failure(error.ToManaged());
    }

    /// <summary>
    /// 释放无任务 Publisher 句柄并归还 Catalog 领域生命周期引用；不会关闭领域或删除数据。
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

    /// <summary>
    /// 严格编码 Catalog Path 的两段文本，确保任何原生写入前已完成托管验证。
    /// </summary>
    /// <param name="path">调用方 Path。</param>
    /// <returns>拥有型 UTF-8 Path 或 invalid/capacity。</returns>
    internal static Result<EncodedCatalogPath> EncodePath(CatalogPath path)
    {
        var part = Interop.EncodeUtf8(path.Part, "part");
        if (!part.IsSuccess)
        {
            return Result<EncodedCatalogPath>.Failure(part.Error!);
        }

        var id = Interop.EncodeUtf8(path.Id, "id");
        return id.IsSuccess
            ? Result<EncodedCatalogPath>.Success(new EncodedCatalogPath(part.Value, id.Value))
            : Result<EncodedCatalogPath>.Failure(id.Error!);
    }

    /// <summary>返回当前包装是否仍可用于原生写入。</summary>
    private bool IsUsable => Volatile.Read(ref _disposed) == 0 && !_handle.IsClosed && !_handle.IsInvalid;
}
