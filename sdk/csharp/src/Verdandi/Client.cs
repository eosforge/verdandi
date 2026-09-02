using Verdandi.Internal;

namespace Verdandi;

/// <summary>
/// 拥有一个共享 Redis 传输和严格 JSON 配置副本，并提供有界 Key/Hash 原始能力以及领域 Client 的根生命周期。
/// </summary>
public sealed unsafe class Client : IDisposable
{
    private readonly ClientHandle _handle;
    private int _disposed;

    /// <summary>
    /// 接管已经成功打开的根句柄；构造后由当前对象唯一负责最终 SafeHandle Dispose。
    /// </summary>
    /// <param name="handle">非空原生根句柄。</param>
    private Client(ClientHandle handle)
    {
        _handle = handle;
    }

    /// <summary>
    /// 从跨语言严格 v1 JSON 打开根 Client；该调用验证本地 C ABI、配置和 Redis 传输，但不打开 Registration 或 Catalog 子域。
    /// </summary>
    /// <param name="configurationJson">完整配置 JSON 文本；凭据由原生核心按同一跨语言规则解析和保存。</param>
    /// <returns>拥有根传输的 Client，或 native/configuration/Redis 错误。</returns>
    public static Result<Client> Open(string configurationJson)
    {
        var abi = NativeRuntime.CheckAbi();
        if (!abi.IsSuccess)
        {
            return Result<Client>.Failure(abi.Error!);
        }

        var encoded = Configuration.Encode(configurationJson);
        if (!encoded.IsSuccess)
        {
            return Result<Client>.Failure(encoded.Error!);
        }

        using var json = new NativeBufferLease(encoded.Value);
        NativeError error = default;
        nint output = nint.Zero;
        if (NativeMethods.ClientOpenJson(json.BytesView, &output, &error) == 0)
        {
            return Result<Client>.Failure(error.ToManaged());
        }

        if (output == nint.Zero)
        {
            return Result<Client>.Failure(new VerdandiError("corrupt", "client"));
        }

        return Result<Client>.Success(new Client(new ClientHandle(output)));
    }

    /// <summary>
    /// 返回根 Client 是否仍接纳新工作；Dispose 后恒为假。
    /// </summary>
    public bool IsOpen => IsUsable && NativeMethods.ClientIsOpen(_handle) != 0;

    /// <summary>
    /// 使用配置的单命令超时执行一次 Redis PING，不打开任何 Verdandi 子域。
    /// </summary>
    /// <returns>Redis 已确认时成功，否则返回稳定连接或超时错误。</returns>
    public Result Ping()
    {
        if (!IsUsable)
        {
            return Invalid();
        }

        NativeError error = default;
        return Interop.Status(NativeMethods.ClientPing(_handle, &error), error);
    }

    /// <summary>
    /// 终止共享传输但不删除 Redis 数据；调用方应先关闭领域对象，重复调用由原生核心保证幂等。
    /// </summary>
    /// <returns>传输完成关闭时成功，或返回关闭期间的稳定错误。</returns>
    public Result Close()
    {
        if (!IsUsable)
        {
            return Invalid();
        }

        NativeError error = default;
        return Interop.Status(NativeMethods.ClientClose(_handle, &error), error);
    }

    /// <summary>
    /// 读取一个原始二进制 Key；不存在是成功的空值，不执行 Verdandi 领域结构验证。
    /// </summary>
    /// <param name="key">完整 Redis Key；ACL 仍由 Redis 强制执行。</param>
    /// <returns>存在时拥有型字节，不存在时 null，或稳定 Redis 错误。</returns>
    public Result<byte[]?> LoadKey(string key)
    {
        if (!IsUsable)
        {
            return Result<byte[]?>.Failure(InvalidError());
        }

        var encodedKey = Interop.EncodeUtf8(key, "key");
        if (!encodedKey.IsSuccess)
        {
            return Result<byte[]?>.Failure(encodedKey.Error!);
        }

        using var nativeKey = new NativeBufferLease(encodedKey.Value);
        NativeError error = default;
        var found = 0;
        nint output = nint.Zero;
        if (NativeMethods.KeyLoad(_handle, nativeKey.StringView, &found, &output, &error) == 0)
        {
            return Result<byte[]?>.Failure(error.ToManaged());
        }

        using var blob = new BlobHandle(output);
        if (found == 0)
        {
            return Result<byte[]?>.Success(null);
        }

        if (blob.IsInvalid)
        {
            return Result<byte[]?>.Failure(new VerdandiError("corrupt", "blob"));
        }

        var copied = Interop.CopyBlob(blob);
        return copied.IsSuccess ? Result<byte[]?>.Success(copied.Value) : Result<byte[]?>.Failure(copied.Error!);
    }

    /// <summary>
    /// 无 TTL 覆盖写入一个原始二进制 Key；该能力位于 Verdandi 领域不变量之外。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <param name="value">同步调用期间借用并在返回前由原生核心复制的字节。</param>
    /// <returns>Redis 已确认写入时成功，或稳定错误。</returns>
    public Result StoreKey(string key, ReadOnlySpan<byte> value) => StoreKeyCore(key, value, null);

    /// <summary>
    /// 以精确正整数毫秒 TTL 覆盖写入一个原始二进制 Key。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <param name="value">同步调用期间借用并在返回前由原生核心复制的字节。</param>
    /// <param name="ttl">必须为正且没有子毫秒部分的 TTL。</param>
    /// <returns>Redis 已确认写入时成功，或 invalid/Redis 错误。</returns>
    public Result StoreKey(string key, ReadOnlySpan<byte> value, TimeSpan ttl) => StoreKeyCore(key, value, ttl);

    /// <summary>
    /// 删除完整原始 Key，并报告删除前是否存在。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <returns>实际删除返回 true，不存在返回 false，调用失败返回错误。</returns>
    public Result<bool> EraseKey(string key)
    {
        if (!IsUsable)
        {
            return Result<bool>.Failure(InvalidError());
        }

        var encodedKey = Interop.EncodeUtf8(key, "key");
        if (!encodedKey.IsSuccess)
        {
            return Result<bool>.Failure(encodedKey.Error!);
        }

        using var nativeKey = new NativeBufferLease(encodedKey.Value);
        NativeError error = default;
        var removed = 0;
        return NativeMethods.KeyErase(_handle, nativeKey.StringView, &removed, &error) != 0
            ? Result<bool>.Success(removed != 0)
            : Result<bool>.Failure(error.ToManaged());
    }

    /// <summary>
    /// 判断一个完整原始 Key 是否存在。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <returns>存在状态，或稳定 Redis 错误。</returns>
    public Result<bool> ContainsKey(string key)
    {
        if (!IsUsable)
        {
            return Result<bool>.Failure(InvalidError());
        }

        var encodedKey = Interop.EncodeUtf8(key, "key");
        if (!encodedKey.IsSuccess)
        {
            return Result<bool>.Failure(encodedKey.Error!);
        }

        using var nativeKey = new NativeBufferLease(encodedKey.Value);
        NativeError error = default;
        var present = 0;
        return NativeMethods.KeyContains(_handle, nativeKey.StringView, &present, &error) != 0
            ? Result<bool>.Success(present != 0)
            : Result<bool>.Failure(error.ToManaged());
    }

    /// <summary>
    /// 为现存原始 Key 设置精确正整数毫秒 TTL，并报告是否实际改变了 Key。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <param name="ttl">必须为正且没有子毫秒部分的 TTL。</param>
    /// <returns>现存 Key 被设置返回 true，不存在返回 false，调用失败返回错误。</returns>
    public Result<bool> ExpireKey(string key, TimeSpan ttl)
    {
        if (!IsUsable)
        {
            return Result<bool>.Failure(InvalidError());
        }

        var milliseconds = Interop.PositiveMilliseconds(ttl, "ttl");
        if (!milliseconds.IsSuccess)
        {
            return Result<bool>.Failure(milliseconds.Error!);
        }

        var encodedKey = Interop.EncodeUtf8(key, "key");
        if (!encodedKey.IsSuccess)
        {
            return Result<bool>.Failure(encodedKey.Error!);
        }

        using var nativeKey = new NativeBufferLease(encodedKey.Value);
        NativeError error = default;
        var changed = 0;
        return NativeMethods.KeyExpire(_handle, nativeKey.StringView, milliseconds.Value, &changed, &error) != 0
            ? Result<bool>.Success(changed != 0)
            : Result<bool>.Failure(error.ToManaged());
    }

    /// <summary>
    /// 读取一个完整原始 Redis Hash；不存在时返回空 Fields。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <returns>拥有型完整字段集合，或稳定 Redis/边界错误。</returns>
    public Result<Fields> LoadHash(string key)
    {
        if (!IsUsable)
        {
            return Result<Fields>.Failure(InvalidError());
        }

        var encodedKey = Interop.EncodeUtf8(key, "key");
        if (!encodedKey.IsSuccess)
        {
            return Result<Fields>.Failure(encodedKey.Error!);
        }

        using var nativeKey = new NativeBufferLease(encodedKey.Value);
        NativeError error = default;
        nint output = nint.Zero;
        if (NativeMethods.HashLoad(_handle, nativeKey.StringView, &output, &error) == 0)
        {
            return Result<Fields>.Failure(error.ToManaged());
        }

        using var fields = new FieldSetHandle(output);
        return fields.IsInvalid ? Result<Fields>.Failure(new VerdandiError("corrupt", "fields")) : Interop.CopyFieldSet(fields);
    }

    /// <summary>
    /// 用一次原子 HSET 写入完整原始字段集合；空集合由原生契约拒绝。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <param name="value">不可变完整字段集合。</param>
    /// <returns>Redis 已确认写入时成功，或稳定错误。</returns>
    public Result StoreHash(string key, Fields value)
    {
        if (!IsUsable)
        {
            return Invalid();
        }

        ArgumentNullException.ThrowIfNull(value);
        var encodedKey = Interop.EncodeUtf8(key, "key");
        if (!encodedKey.IsSuccess)
        {
            return Result.Failure(encodedKey.Error!);
        }

        using var nativeKey = new NativeBufferLease(encodedKey.Value);
        using var nativeFields = new NativeFieldsLease(value);
        NativeError error = default;
        return Interop.Status(NativeMethods.HashStore(_handle, nativeKey.StringView, nativeFields.View, &error), error);
    }

    /// <summary>
    /// 删除指定原始 Hash 字段，并返回实际删除数量；名称数组在调用返回前已经复制。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <param name="names">要删除的精确字段名序列。</param>
    /// <returns>实际删除数量，或稳定错误。</returns>
    public Result<nuint> EraseHashFields(string key, IReadOnlyList<string> names)
    {
        if (!IsUsable)
        {
            return Result<nuint>.Failure(InvalidError());
        }

        ArgumentNullException.ThrowIfNull(names);
        var encodedKey = Interop.EncodeUtf8(key, "key");
        if (!encodedKey.IsSuccess)
        {
            return Result<nuint>.Failure(encodedKey.Error!);
        }

        var encodedNames = new byte[names.Count][];
        for (var index = 0; index < names.Count; index++)
        {
            var encoded = Interop.EncodeUtf8(names[index], "name");
            if (!encoded.IsSuccess)
            {
                return Result<nuint>.Failure(encoded.Error!);
            }

            encodedNames[index] = encoded.Value;
        }

        try
        {
            using var nativeKey = new NativeBufferLease(encodedKey.Value);
            using var nativeNames = new NativeStringArrayLease(encodedNames);
            NativeError error = default;
            nuint removed = 0;
            return NativeMethods.HashErase(_handle, nativeKey.StringView, nativeNames.Views, nativeNames.Count, &removed, &error) != 0
                ? Result<nuint>.Success(removed)
                : Result<nuint>.Failure(error.ToManaged());
        }
        catch (OverflowException exception)
        {
            return Result<nuint>.Failure(new VerdandiError("capacity", "names", exception.Message));
        }
        catch (OutOfMemoryException exception)
        {
            return Result<nuint>.Failure(new VerdandiError("capacity", "names", exception.Message));
        }
    }

    /// <summary>
    /// 判断原始 Hash 中是否存在指定字段。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <param name="name">精确字段名。</param>
    /// <returns>存在状态，或稳定错误。</returns>
    public Result<bool> ContainsHashField(string key, string name)
    {
        if (!IsUsable)
        {
            return Result<bool>.Failure(InvalidError());
        }

        var encodedKey = Interop.EncodeUtf8(key, "key");
        var encodedName = Interop.EncodeUtf8(name, "name");
        if (!encodedKey.IsSuccess)
        {
            return Result<bool>.Failure(encodedKey.Error!);
        }

        if (!encodedName.IsSuccess)
        {
            return Result<bool>.Failure(encodedName.Error!);
        }

        using var nativeKey = new NativeBufferLease(encodedKey.Value);
        using var nativeName = new NativeBufferLease(encodedName.Value);
        NativeError error = default;
        var present = 0;
        return NativeMethods.HashContains(_handle, nativeKey.StringView, nativeName.StringView, &present, &error) != 0
            ? Result<bool>.Success(present != 0)
            : Result<bool>.Failure(error.ToManaged());
    }

    /// <summary>
    /// 返回原始 Hash 当前字段数量，不建立完整字段副本。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <returns>字段数量，或稳定错误。</returns>
    public Result<nuint> GetHashSize(string key)
    {
        if (!IsUsable)
        {
            return Result<nuint>.Failure(InvalidError());
        }

        var encodedKey = Interop.EncodeUtf8(key, "key");
        if (!encodedKey.IsSuccess)
        {
            return Result<nuint>.Failure(encodedKey.Error!);
        }

        using var nativeKey = new NativeBufferLease(encodedKey.Value);
        NativeError error = default;
        nuint size = 0;
        return NativeMethods.HashSize(_handle, nativeKey.StringView, &size, &error) != 0
            ? Result<nuint>.Success(size)
            : Result<nuint>.Failure(error.ToManaged());
    }

    /// <summary>
    /// 执行最佳努力 Close 并释放当前包装持有的 SafeHandle；子领域的生命周期引用会延迟原生内存释放但不会阻止传输关闭。
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
            _ = NativeMethods.ClientClose(_handle, &error);
        }

        _handle.Dispose();
        GC.SuppressFinalize(this);
    }

    /// <summary>
    /// 返回供子领域取得释放顺序引用的内部 SafeHandle；它不转移 Close 权限。
    /// </summary>
    internal ClientHandle Handle => _handle;

    /// <summary>返回当前包装是否仍可安全传给 P/Invoke。</summary>
    internal bool IsUsable => Volatile.Read(ref _disposed) == 0 && !_handle.IsClosed && !_handle.IsInvalid;

    /// <summary>
    /// 统一实现有无 TTL 的 Key Store，确保 TimeSpan 校验和字节固定范围完全一致。
    /// </summary>
    /// <param name="key">完整 Redis Key。</param>
    /// <param name="value">调用期间借用的值。</param>
    /// <param name="ttl">为空表示无 TTL，否则必须是正整毫秒。</param>
    /// <returns>写入结果。</returns>
    private Result StoreKeyCore(string key, ReadOnlySpan<byte> value, TimeSpan? ttl)
    {
        if (!IsUsable)
        {
            return Invalid();
        }

        Result<ulong> milliseconds = default;
        if (ttl is not null)
        {
            milliseconds = Interop.PositiveMilliseconds(ttl.Value, "ttl");
            if (!milliseconds.IsSuccess)
            {
                return Result.Failure(milliseconds.Error!);
            }
        }

        var encodedKey = Interop.EncodeUtf8(key, "key");
        if (!encodedKey.IsSuccess)
        {
            return Result.Failure(encodedKey.Error!);
        }

        using var nativeKey = new NativeBufferLease(encodedKey.Value);
        fixed (byte* data = value)
        {
            // C ABI 在同步返回前已把命令参数复制进原生拥有型存储，借用调用方 Span
            // 可以避免每次原始 Key 写入额外分配并复制一份完整值。
            var nativeValue = new NativeBytesView(data, checked((nuint)value.Length));
            NativeError error = default;
            var succeeded = ttl is null
                ? NativeMethods.KeyStore(_handle, nativeKey.StringView, nativeValue, &error)
                : NativeMethods.KeyStoreTtl(_handle, nativeKey.StringView, nativeValue, milliseconds.Value, &error);
            return Interop.Status(succeeded, error);
        }
    }

    /// <summary>构造根 Client 无效状态的无载荷失败。</summary>
    /// <returns>invalid/client。</returns>
    private static Result Invalid() => Result.Failure(InvalidError());

    /// <summary>构造根 Client 无效状态的稳定错误。</summary>
    /// <returns>invalid/client。</returns>
    private static VerdandiError InvalidError() => new("invalid", "client");
}
