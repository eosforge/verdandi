using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Verdandi.Internal;

/// <summary>
/// 在一个同步原生调用期间固定拥有型字节；Dispose 后任何视图立即失效。
/// </summary>
internal unsafe ref struct NativeBufferLease
{
    private GCHandle _pin;
    private readonly byte[] _bytes;

    /// <summary>
    /// 固定调用方已经拥有的数组，不复制内容。
    /// </summary>
    /// <param name="bytes">在 Lease 结束前保持强引用的数组。</param>
    internal NativeBufferLease(byte[] bytes)
    {
        ArgumentNullException.ThrowIfNull(bytes);
        _bytes = bytes;
        _pin = bytes.Length == 0 ? default : GCHandle.Alloc(bytes, GCHandleType.Pinned);
    }

    /// <summary>返回不拥有且无需零结尾的字符串视图。</summary>
    internal NativeStringView StringView => new(Pointer, checked((nuint)_bytes.Length));

    /// <summary>返回不拥有的二进制视图。</summary>
    internal NativeBytesView BytesView => new(Pointer, checked((nuint)_bytes.Length));

    /// <summary>
    /// 解除固定；重复使用该 ref struct 的副本属于调用方错误，因此只在词法 using 范围内使用。
    /// </summary>
    internal void Dispose()
    {
        if (_pin.IsAllocated)
        {
            _pin.Free();
        }
    }

    /// <summary>返回固定数组首地址；空数组返回空指针。</summary>
    private byte* Pointer => _pin.IsAllocated ? (byte*)_pin.AddrOfPinnedObject() : null;
}

/// <summary>
/// 把 Fields 的单一连续载荷固定一次，并建立本次调用所需的原生字段视图数组。
/// </summary>
internal unsafe ref struct NativeFieldsLease
{
    private GCHandle _payloadPin;
    private NativeFieldView* _views;
    private readonly int _count;

    /// <summary>
    /// 借用不可变 Fields 并为每个索引建立地址；值本身不会再次复制。
    /// </summary>
    /// <param name="fields">调用期间保持存活的不可变集合。</param>
    internal NativeFieldsLease(Fields fields)
    {
        ArgumentNullException.ThrowIfNull(fields);
        _count = fields.Count;
        _payloadPin = fields.Payload.Length == 0 ? default : GCHandle.Alloc(fields.Payload, GCHandleType.Pinned);
        _views = null;
        try
        {
            _views = _count == 0 ? null : (NativeFieldView*)NativeMemory.Alloc(checked((nuint)_count), (nuint)sizeof(NativeFieldView));
            var payload = _payloadPin.IsAllocated ? (byte*)_payloadPin.AddrOfPinnedObject() : null;
            var entries = fields.Entries;
            for (var index = 0; index < entries.Length; index++)
            {
                var entry = entries[index];
                _views[index] = new NativeFieldView(
                    new NativeStringView(payload + entry.NameOffset, checked((nuint)entry.NameLength)),
                    new NativeBytesView(payload + entry.ValueOffset, checked((nuint)entry.ValueLength)));
            }
        }
        catch
        {
            Dispose();
            throw;
        }
    }

    /// <summary>返回只在当前 Lease 存活期间有效的完整字段视图。</summary>
    internal NativeFieldsView View => new(_views, checked((nuint)_count));

    /// <summary>释放临时视图数组并解除连续载荷固定。</summary>
    internal void Dispose()
    {
        if (_views is not null)
        {
            NativeMemory.Free(_views);
            _views = null;
        }

        if (_payloadPin.IsAllocated)
        {
            _payloadPin.Free();
        }
    }
}

/// <summary>
/// 为多个 UTF-8 文本建立一个连续非托管载荷和视图数组，适用于 Hash 字段删除等低频批量边界。
/// </summary>
internal unsafe ref struct NativeStringArrayLease
{
    private byte* _payload;
    private NativeStringView* _views;
    private readonly int _count;

    /// <summary>
    /// 复制已编码文本并建立连续视图；编码验证由调用方在构造前完成。
    /// </summary>
    /// <param name="values">每项一个完整 UTF-8 文本。</param>
    internal NativeStringArrayLease(IReadOnlyList<byte[]> values)
    {
        ArgumentNullException.ThrowIfNull(values);
        _count = values.Count;
        var total = 0;
        for (var index = 0; index < values.Count; index++)
        {
            total = checked(total + values[index].Length);
        }

        _payload = total == 0 ? null : (byte*)NativeMemory.Alloc(checked((nuint)total));
        _views = null;
        try
        {
            _views = _count == 0 ? null : (NativeStringView*)NativeMemory.Alloc(checked((nuint)_count), (nuint)sizeof(NativeStringView));
            var offset = 0;
            for (var index = 0; index < values.Count; index++)
            {
                var value = values[index];
                if (value.Length != 0)
                {
                    value.CopyTo(new Span<byte>(_payload + offset, value.Length));
                }

                _views[index] = new NativeStringView(_payload + offset, checked((nuint)value.Length));
                offset += value.Length;
            }
        }
        catch
        {
            Dispose();
            throw;
        }
    }

    /// <summary>返回借用视图数组首地址。</summary>
    internal NativeStringView* Views => _views;

    /// <summary>返回视图数量。</summary>
    internal nuint Count => checked((nuint)_count);

    /// <summary>释放连续载荷和视图数组。</summary>
    internal void Dispose()
    {
        if (_views is not null)
        {
            NativeMemory.Free(_views);
            _views = null;
        }

        if (_payload is not null)
        {
            NativeMemory.Free(_payload);
            _payload = null;
        }
    }
}

/// <summary>
/// 保存已经严格编码的 Catalog Path，供一次 Subscription 或 Path 调用建立原生视图。
/// </summary>
/// <param name="Part">拥有型 UTF-8 Part。</param>
/// <param name="Id">拥有型 UTF-8 ID。</param>
internal readonly record struct EncodedCatalogPath(byte[] Part, byte[] Id);

/// <summary>
/// 为一次 Catalog Subscriber 创建调用拥有连续 Part/Path 视图和文本载荷。
/// </summary>
internal unsafe ref struct NativeCatalogSubscriptionLease
{
    private byte* _payload;
    private NativeStringView* _parts;
    private NativeCatalogPathView* _paths;
    private readonly int _partCount;
    private readonly int _pathCount;
    private readonly bool _zone;

    /// <summary>
    /// 复制已验证的 UTF-8 范围并建立 C ABI 订阅结构；所有地址在 Dispose 时统一释放。
    /// </summary>
    /// <param name="zone">是否覆盖完整 Zone。</param>
    /// <param name="parts">完整 Part UTF-8 数组。</param>
    /// <param name="paths">完整精确 Path UTF-8 数组。</param>
    internal NativeCatalogSubscriptionLease(bool zone, IReadOnlyList<byte[]> parts, IReadOnlyList<EncodedCatalogPath> paths)
    {
        ArgumentNullException.ThrowIfNull(parts);
        ArgumentNullException.ThrowIfNull(paths);
        _zone = zone;
        _partCount = parts.Count;
        _pathCount = paths.Count;

        var total = 0;
        for (var index = 0; index < parts.Count; index++)
        {
            total = checked(total + parts[index].Length);
        }

        for (var index = 0; index < paths.Count; index++)
        {
            total = checked(total + paths[index].Part.Length + paths[index].Id.Length);
        }

        _payload = null;
        _parts = null;
        _paths = null;
        try
        {
            _payload = total == 0 ? null : (byte*)NativeMemory.Alloc(checked((nuint)total));
            _parts = _partCount == 0 ? null : (NativeStringView*)NativeMemory.Alloc(checked((nuint)_partCount), (nuint)sizeof(NativeStringView));
            _paths = _pathCount == 0
                ? null
                : (NativeCatalogPathView*)NativeMemory.Alloc(checked((nuint)_pathCount), (nuint)sizeof(NativeCatalogPathView));

            var offset = 0;
            for (var index = 0; index < parts.Count; index++)
            {
                var part = parts[index];
                Copy(part, _payload + offset);
                _parts[index] = new NativeStringView(_payload + offset, checked((nuint)part.Length));
                offset += part.Length;
            }

            for (var index = 0; index < paths.Count; index++)
            {
                var path = paths[index];
                Copy(path.Part, _payload + offset);
                var part = new NativeStringView(_payload + offset, checked((nuint)path.Part.Length));
                offset += path.Part.Length;
                Copy(path.Id, _payload + offset);
                var id = new NativeStringView(_payload + offset, checked((nuint)path.Id.Length));
                offset += path.Id.Length;
                _paths[index] = new NativeCatalogPathView { Part = part, Id = id };
            }
        }
        catch
        {
            Dispose();
            throw;
        }
    }

    /// <summary>返回只在当前 Lease 范围内有效的完整原生订阅结构。</summary>
    internal NativeCatalogSubscription Subscription => new()
    {
        Zone = _zone ? 1 : 0,
        Parts = _parts,
        PartCount = checked((nuint)_partCount),
        Paths = _paths,
        PathCount = checked((nuint)_pathCount),
    };

    /// <summary>释放文本载荷、Part 视图和 Path 视图。</summary>
    internal void Dispose()
    {
        if (_paths is not null)
        {
            NativeMemory.Free(_paths);
            _paths = null;
        }

        if (_parts is not null)
        {
            NativeMemory.Free(_parts);
            _parts = null;
        }

        if (_payload is not null)
        {
            NativeMemory.Free(_payload);
            _payload = null;
        }
    }

    /// <summary>
    /// 把拥有型数组复制到已分配非托管区域；空数组不解引用目标地址。
    /// </summary>
    /// <param name="source">完整源字节。</param>
    /// <param name="destination">足够容量的目标地址。</param>
    private static void Copy(byte[] source, byte* destination)
    {
        if (source.Length != 0)
        {
            source.CopyTo(new Span<byte>(destination, source.Length));
        }
    }
}

/// <summary>
/// 集中完成 UTF-8、错误、时间、Blob 和字段集合的拥有权转换。
/// </summary>
internal static unsafe class Interop
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    /// <summary>
    /// 严格编码一个托管字符串；无效 UTF-16 返回 invalid，避免静默写入替换字符。
    /// </summary>
    /// <param name="value">待编码文本。</param>
    /// <param name="field">失败错误中的参数名。</param>
    /// <returns>拥有型 UTF-8 字节或稳定错误。</returns>
    internal static Result<byte[]> EncodeUtf8(string? value, string field)
    {
        if (value is null)
        {
            return Result<byte[]>.Failure(new VerdandiError("invalid", field));
        }

        try
        {
            return Result<byte[]>.Success(StrictUtf8.GetBytes(value));
        }
        catch (EncoderFallbackException exception)
        {
            return Result<byte[]>.Failure(new VerdandiError("invalid", field, exception.Message));
        }
        catch (OutOfMemoryException exception)
        {
            return Result<byte[]>.Failure(new VerdandiError("capacity", field, exception.Message));
        }
    }

    /// <summary>
    /// 把 TimeSpan 转为正整数、整毫秒 UInt64；子毫秒、零值和负值均返回 invalid。
    /// </summary>
    /// <param name="value">调用方时间长度。</param>
    /// <param name="field">错误中的参数名。</param>
    /// <returns>精确毫秒数或 invalid。</returns>
    internal static Result<ulong> PositiveMilliseconds(TimeSpan value, string field)
    {
        if (value <= TimeSpan.Zero || value.Ticks % TimeSpan.TicksPerMillisecond != 0)
        {
            return Result<ulong>.Failure(new VerdandiError("invalid", field));
        }

        return Result<ulong>.Success(checked((ulong)value.Ticks / (ulong)TimeSpan.TicksPerMillisecond));
    }

    /// <summary>
    /// 把 C ABI 的非零成功约定和固定错误转换为托管无载荷 Result。
    /// </summary>
    /// <param name="succeeded">C ABI 返回值。</param>
    /// <param name="error">失败时已填写的固定错误。</param>
    /// <returns>托管结果。</returns>
    internal static Result Status(int succeeded, in NativeError error) => succeeded != 0 ? Result.Success() : Result.Failure(error.ToManaged());

    /// <summary>
    /// 区分诊断调用失败与调用成功但队列为空，并立即复制可用诊断。
    /// </summary>
    /// <param name="succeeded">C ABI 调用是否成功。</param>
    /// <param name="available">诊断队列是否返回一项。</param>
    /// <param name="error">失败或已取出诊断的固定缓冲区。</param>
    /// <returns>调用结果和可用状态。</returns>
    internal static Result<Diagnostic> Diagnostic(int succeeded, int available, in NativeError error)
    {
        if (succeeded == 0)
        {
            return Result<Diagnostic>.Failure(error.ToManaged());
        }

        return Result<Diagnostic>.Success(new Diagnostic(available != 0, available == 0 ? null : error.ToManaged()));
    }

    /// <summary>
    /// 从原生借用文本立即复制严格 UTF-8；空指针配非零长度或超出托管数组范围返回 corrupt/capacity。
    /// </summary>
    /// <param name="view">调用期间借用的原生文本。</param>
    /// <param name="field">失败错误中的字段。</param>
    /// <returns>拥有型托管字符串或错误。</returns>
    internal static Result<string> CopyString(NativeStringView view, string field)
    {
        if (view.Size > int.MaxValue)
        {
            return Result<string>.Failure(new VerdandiError("capacity", field));
        }

        if (view.Data is null && view.Size != 0)
        {
            return Result<string>.Failure(new VerdandiError("corrupt", field));
        }

        try
        {
            return Result<string>.Success(StrictUtf8.GetString(new ReadOnlySpan<byte>(view.Data, checked((int)view.Size))));
        }
        catch (DecoderFallbackException exception)
        {
            return Result<string>.Failure(new VerdandiError("corrupt", field, exception.Message));
        }
    }

    /// <summary>
    /// 从原生借用二进制视图立即复制为拥有型数组。
    /// </summary>
    /// <param name="view">调用期间借用的原生字节。</param>
    /// <param name="field">失败错误中的字段。</param>
    /// <returns>拥有型数组或 corrupt/capacity。</returns>
    internal static Result<byte[]> CopyBytes(NativeBytesView view, string field)
    {
        if (view.Size > int.MaxValue)
        {
            return Result<byte[]>.Failure(new VerdandiError("capacity", field));
        }

        if (view.Data is null && view.Size != 0)
        {
            return Result<byte[]>.Failure(new VerdandiError("corrupt", field));
        }

        try
        {
            return Result<byte[]>.Success(new ReadOnlySpan<byte>(view.Data, checked((int)view.Size)).ToArray());
        }
        catch (OutOfMemoryException exception)
        {
            return Result<byte[]>.Failure(new VerdandiError("capacity", field, exception.Message));
        }
    }

    /// <summary>
    /// 读取 Blob 借用视图并在释放句柄前复制全部字节。
    /// </summary>
    /// <param name="handle">拥有 Blob 的安全句柄。</param>
    /// <returns>拥有型字节或边界错误。</returns>
    internal static Result<byte[]> CopyBlob(BlobHandle handle) => CopyBytes(NativeMethods.BlobView(handle), "blob");

    /// <summary>
    /// 按原生规范顺序复制 FieldSet 的全部字段并构造不可变连续 Fields。
    /// </summary>
    /// <param name="handle">拥有字段结果的安全句柄。</param>
    /// <returns>托管 Fields 或 corrupt/capacity 错误。</returns>
    internal static Result<Fields> CopyFieldSet(FieldSetHandle handle)
    {
        var count = NativeMethods.FieldSetSize(handle);
        if (count > int.MaxValue)
        {
            return Result<Fields>.Failure(new VerdandiError("capacity", "fields"));
        }

        try
        {
            var fields = new Field[checked((int)count)];
            for (nuint index = 0; index < count; index++)
            {
                NativeFieldView native = default;
                if (NativeMethods.FieldSetAt(handle, index, &native) == 0)
                {
                    return Result<Fields>.Failure(new VerdandiError("corrupt", "fields"));
                }

                var name = CopyString(native.Name, "field");
                if (!name.IsSuccess)
                {
                    return Result<Fields>.Failure(name.Error!);
                }

                var value = CopyBytes(native.Value, name.Value);
                if (!value.IsSuccess)
                {
                    return Result<Fields>.Failure(value.Error!);
                }

                fields[checked((int)index)] = new Field(name.Value, value.Value);
            }

            return Fields.Create(fields);
        }
        catch (OutOfMemoryException exception)
        {
            return Result<Fields>.Failure(new VerdandiError("capacity", "fields", exception.Message));
        }
    }

    /// <summary>
    /// 通过 C ABI 同步回调复制一个借用候选的 Attr 或 Data；回调结束后不保留任何原生地址。
    /// </summary>
    /// <param name="candidates">当前 Selector 策略借用候选集合。</param>
    /// <param name="index">候选索引。</param>
    /// <param name="attr">为真读取 Attr，否则读取当前预测优先 Data。</param>
    /// <returns>拥有型 Fields 或回调/原生错误。</returns>
    internal static Result<Fields> CopyCandidateFields(nint candidates, nuint index, bool attr)
    {
        var collector = new FieldCollector();
        var context = GCHandle.Alloc(collector);
        NativeError error = default;
        int succeeded;
        try
        {
            succeeded = attr
                ? NativeMethods.CandidatesVisitAttr(candidates, index, &CollectField, GCHandle.ToIntPtr(context), &error)
                : NativeMethods.CandidatesVisitData(candidates, index, &CollectField, GCHandle.ToIntPtr(context), &error);
        }
        finally
        {
            context.Free();
        }

        return FinishCollection(succeeded, error, collector);
    }

    /// <summary>
    /// 从拥有型脱离候选列表复制一个候选的 Attr 或 Data。
    /// </summary>
    /// <param name="candidates">拥有候选列表的 SafeHandle。</param>
    /// <param name="index">候选索引。</param>
    /// <param name="attr">为真读取 Attr，否则读取 Data。</param>
    /// <returns>拥有型 Fields 或回调/原生错误。</returns>
    internal static Result<Fields> CopyCandidateListFields(CandidateListHandle candidates, nuint index, bool attr)
    {
        var collector = new FieldCollector();
        var context = GCHandle.Alloc(collector);
        NativeError error = default;
        int succeeded;
        try
        {
            succeeded = attr
                ? NativeMethods.CandidateListVisitAttr(candidates, index, &CollectField, GCHandle.ToIntPtr(context), &error)
                : NativeMethods.CandidateListVisitData(candidates, index, &CollectField, GCHandle.ToIntPtr(context), &error);
        }
        finally
        {
            context.Free();
        }

        return FinishCollection(succeeded, error, collector);
    }

    /// <summary>
    /// 从拥有型 Selector 快照复制活动或 retained 候选的 Attr 或 Data。
    /// </summary>
    /// <param name="snapshot">拥有完整快照的 SafeHandle。</param>
    /// <param name="retained">是否读取 retained 视图。</param>
    /// <param name="index">候选索引。</param>
    /// <param name="attr">为真读取 Attr，否则读取 Data。</param>
    /// <returns>拥有型 Fields 或回调/原生错误。</returns>
    internal static Result<Fields> CopySnapshotFields(SelectorSnapshotHandle snapshot, bool retained, nuint index, bool attr)
    {
        var collector = new FieldCollector();
        var context = GCHandle.Alloc(collector);
        NativeError error = default;
        int succeeded;
        try
        {
            succeeded = attr
                ? NativeMethods.SelectorSnapshotVisitAttr(snapshot, retained ? 1 : 0, index, &CollectField, GCHandle.ToIntPtr(context), &error)
                : NativeMethods.SelectorSnapshotVisitData(snapshot, retained ? 1 : 0, index, &CollectField, GCHandle.ToIntPtr(context), &error);
        }
        finally
        {
            context.Free();
        }

        return FinishCollection(succeeded, error, collector);
    }

    /// <summary>
    /// 在原生字段遍历期间立即复制名称和值；任何托管异常都被捕获并以零返回中止 C 回调。
    /// </summary>
    /// <param name="context">指向当前托管 FieldCollector 的 GCHandle。</param>
    /// <param name="name">回调期间借用的字段名。</param>
    /// <param name="value">回调期间借用的字段值。</param>
    /// <returns>一继续遍历，零中止。</returns>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CollectField(nint context, NativeStringView name, NativeBytesView value)
    {
        try
        {
            if (GCHandle.FromIntPtr(context).Target is not FieldCollector collector)
            {
                return 0;
            }

            return collector.Add(name, value) ? 1 : 0;
        }
        catch
        {
            return 0;
        }
    }

    /// <summary>
    /// 合并回调本地复制结果与原生返回状态；本地容量/编码错误优先于派生的 callback 错误。
    /// </summary>
    /// <param name="succeeded">原生遍历是否成功。</param>
    /// <param name="nativeError">原生失败错误。</param>
    /// <param name="collector">本次拥有型字段收集器。</param>
    /// <returns>完整 Fields 或稳定错误。</returns>
    private static Result<Fields> FinishCollection(int succeeded, in NativeError nativeError, FieldCollector collector)
    {
        if (collector.Error is not null)
        {
            return Result<Fields>.Failure(collector.Error);
        }

        if (succeeded == 0)
        {
            return Result<Fields>.Failure(nativeError.ToManaged());
        }

        return Fields.Create(collector.Fields);
    }

    /// <summary>
    /// 把零结尾原生状态立即复制为托管 UTF-8；空地址使用调用方提供的后备状态。
    /// </summary>
    /// <param name="value">零结尾 UTF-8 地址。</param>
    /// <param name="fallback">空地址时使用的稳定状态。</param>
    /// <returns>拥有型状态文本。</returns>
    internal static string CopyCString(nint value, string fallback) => value == nint.Zero ? fallback : Marshal.PtrToStringUTF8(value) ?? fallback;

    /// <summary>
    /// 调用应用编码契约并把异常转换为 codec 错误，确保原生调用前已经取得完整拥有型 Fields。
    /// </summary>
    /// <typeparam name="T">应用值类型。</typeparam>
    /// <param name="value">同步编码的完整值。</param>
    /// <returns>编码 Fields 或稳定错误。</returns>
    internal static Result<Fields> EncodeValue<T>(T value)
        where T : IFieldValue<T>
    {
        try
        {
            return value.EncodeFields();
        }
        catch (OutOfMemoryException exception)
        {
            return Result<Fields>.Failure(new VerdandiError("capacity", "codec", exception.Message));
        }
        catch (Exception exception)
        {
            return Result<Fields>.Failure(new VerdandiError("unavailable", "codec", exception.Message));
        }
    }

    /// <summary>
    /// 调用应用静态解码契约并把异常转换为 codec 错误。
    /// </summary>
    /// <typeparam name="T">应用值类型。</typeparam>
    /// <param name="fields">完整拥有型字段集合。</param>
    /// <returns>应用值或稳定错误。</returns>
    internal static Result<T> DecodeValue<T>(Fields fields)
        where T : IFieldValue<T>
    {
        try
        {
            return T.DecodeFields(fields);
        }
        catch (OutOfMemoryException exception)
        {
            return Result<T>.Failure(new VerdandiError("capacity", "codec", exception.Message));
        }
        catch (Exception exception)
        {
            return Result<T>.Failure(new VerdandiError("unavailable", "codec", exception.Message));
        }
    }

    /// <summary>
    /// 保存一次 C 字段回调已经复制的托管字段和首个失败；它从不保留原生视图。
    /// </summary>
    private sealed class FieldCollector
    {
        /// <summary>已经复制的字段。</summary>
        internal List<Field> Fields { get; } = [];

        /// <summary>首个复制或 UTF-8 失败。</summary>
        internal VerdandiError? Error { get; private set; }

        /// <summary>
        /// 立即复制一个原生字段并追加；失败后拒绝后续字段。
        /// </summary>
        /// <param name="name">借用名称。</param>
        /// <param name="value">借用值。</param>
        /// <returns>本次字段是否成功拥有型保存。</returns>
        internal bool Add(NativeStringView name, NativeBytesView value)
        {
            if (Error is not null)
            {
                return false;
            }

            var copiedName = CopyString(name, "field");
            if (!copiedName.IsSuccess)
            {
                Error = copiedName.Error;
                return false;
            }

            var copiedValue = CopyBytes(value, copiedName.Value);
            if (!copiedValue.IsSuccess)
            {
                Error = copiedValue.Error;
                return false;
            }

            try
            {
                Fields.Add(new Field(copiedName.Value, copiedValue.Value));
                return true;
            }
            catch (OutOfMemoryException exception)
            {
                Error = new VerdandiError("capacity", "callback", exception.Message);
                return false;
            }
        }
    }
}
