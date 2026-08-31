using System.Buffers.Text;
using System.Text;

namespace Verdandi;

/// <summary>
/// 定义应用类型与完整扁平顶层 Fields 之间的显式转换；实现不得保留调用方可变存储。
/// </summary>
/// <typeparam name="TSelf">实现该契约的应用值类型。</typeparam>
public interface IFieldValue<TSelf>
    where TSelf : IFieldValue<TSelf>
{
    /// <summary>
    /// 把当前完整值编码成拥有型 Fields；失败应返回稳定错误，不应执行 Redis I/O。
    /// </summary>
    /// <returns>完整、字段名唯一的扁平字段集合，或编码错误。</returns>
    Result<Fields> EncodeFields();

    /// <summary>
    /// 从完整拥有型 Fields 构造一个新值；实现必须验证其必需字段和业务编码。
    /// </summary>
    /// <param name="fields">在调用期间只读、不会被 Verdandi 修改的完整字段集合。</param>
    /// <returns>完整解码值，或指出字段问题的稳定错误。</returns>
    static abstract Result<TSelf> DecodeFields(Fields fields);
}

/// <summary>
/// 表示一个用于构造 Fields 的拥有型名称和值；Fields 创建时会复制 Value。
/// </summary>
/// <param name="Name">顶层字段名称。</param>
/// <param name="Value">不拥有的调用方字节；构造完成后可以立即复用或修改。</param>
public readonly record struct Field(string Name, ReadOnlyMemory<byte> Value);

/// <summary>
/// 保存按名称排序、不可变且拥有全部字节的完整顶层字段集合。
/// </summary>
public sealed class Fields : IFieldValue<Fields>
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private static readonly Entry[] NoEntries = [];
    private static readonly byte[] NoPayload = [];

    private readonly Entry[] _entries;
    private readonly byte[] _payload;

    /// <summary>
    /// 构造已经完成验证和连续布局的不可变字段集合。
    /// </summary>
    /// <param name="entries">按字段名序排列的内部索引。</param>
    /// <param name="payload">连续拥有所有 UTF-8 名称和字段值的字节。</param>
    private Fields(Entry[] entries, byte[] payload)
    {
        _entries = entries;
        _payload = payload;
    }

    /// <summary>
    /// 返回不含字段的共享不可变集合；具体操作是否接受空集合由领域协议决定。
    /// </summary>
    public static Fields Empty { get; } = new(NoEntries, NoPayload);

    /// <summary>
    /// 返回字段数量，不计算内部名称和值字节。
    /// </summary>
    public int Count => _entries.Length;

    /// <summary>
    /// 从任意字段序列构造不可变集合，复制值、严格编码字段名并拒绝重复名称。
    /// </summary>
    /// <param name="fields">要完整接管语义但不会接管存储的字段序列。</param>
    /// <returns>构造后的集合，或 contract/invalid/capacity 错误。</returns>
    public static Result<Fields> Create(IEnumerable<Field> fields)
    {
        ArgumentNullException.ThrowIfNull(fields);

        try
        {
            var pending = new List<PendingField>();
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var field in fields)
            {
                if (field.Name is null)
                {
                    return Result<Fields>.Failure(new VerdandiError("invalid", "field"));
                }

                if (!names.Add(field.Name))
                {
                    return Result<Fields>.Failure(new VerdandiError("contract", field.Name));
                }

                byte[] name;
                try
                {
                    name = StrictUtf8.GetBytes(field.Name);
                }
                catch (EncoderFallbackException exception)
                {
                    return Result<Fields>.Failure(new VerdandiError("invalid", field.Name, exception.Message));
                }

                pending.Add(new PendingField(field.Name, name, field.Value.ToArray()));
            }

            if (pending.Count == 0)
            {
                return Result<Fields>.Success(Empty);
            }

            pending.Sort(static (left, right) => StringComparer.Ordinal.Compare(left.Name, right.Name));
            var total = 0;
            foreach (var field in pending)
            {
                total = checked(total + field.NameBytes.Length + field.Value.Length);
            }

            var payload = GC.AllocateUninitializedArray<byte>(total);
            var entries = GC.AllocateUninitializedArray<Entry>(pending.Count);
            var offset = 0;
            for (var index = 0; index < pending.Count; index++)
            {
                var field = pending[index];
                field.NameBytes.CopyTo(payload, offset);
                var nameOffset = offset;
                offset += field.NameBytes.Length;
                field.Value.CopyTo(payload, offset);
                entries[index] = new Entry(field.Name, nameOffset, field.NameBytes.Length, offset, field.Value.Length);
                offset += field.Value.Length;
            }

            return Result<Fields>.Success(new Fields(entries, payload));
        }
        catch (OverflowException exception)
        {
            return Result<Fields>.Failure(new VerdandiError("capacity", "fields", exception.Message));
        }
        catch (OutOfMemoryException exception)
        {
            return Result<Fields>.Failure(new VerdandiError("capacity", "fields", exception.Message));
        }
        catch (Exception exception)
        {
            return Result<Fields>.Failure(new VerdandiError("unavailable", "fields", exception.Message));
        }
    }

    /// <summary>
    /// 返回一个新的可变构造器；构造器不会修改任何已经完成的 Fields。
    /// </summary>
    /// <returns>空的单线程构造器。</returns>
    public static FieldsBuilder CreateBuilder() => new();

    /// <summary>
    /// 复制一个原始二进制字段；缺失时返回 missing。
    /// </summary>
    /// <param name="name">要查找的精确字段名。</param>
    /// <returns>独立拥有的字段值，或 missing 错误。</returns>
    public Result<byte[]> GetBytes(string name)
    {
        if (!TryFind(name, out var entry))
        {
            return Result<byte[]>.Failure(new VerdandiError("missing", name ?? string.Empty));
        }

        return Result<byte[]>.Success(_payload.AsSpan(entry.ValueOffset, entry.ValueLength).ToArray());
    }

    /// <summary>
    /// 把必需字段严格解码为 UTF-8 文本；无效字节返回 invalid。
    /// </summary>
    /// <param name="name">要查找的精确字段名。</param>
    /// <returns>解码文本，或 missing/invalid 错误。</returns>
    public Result<string> GetString(string name)
    {
        if (!TryGetSpan(name, out var value))
        {
            return Result<string>.Failure(new VerdandiError("missing", name ?? string.Empty));
        }

        try
        {
            return Result<string>.Success(StrictUtf8.GetString(value));
        }
        catch (DecoderFallbackException exception)
        {
            return Result<string>.Failure(new VerdandiError("invalid", name, exception.Message));
        }
    }

    /// <summary>
    /// 只接受精确的 ASCII `true` 或 `false` 并解码为布尔值。
    /// </summary>
    /// <param name="name">要查找的精确字段名。</param>
    /// <returns>布尔值，或 missing/invalid 错误。</returns>
    public Result<bool> GetBoolean(string name)
    {
        if (!TryGetSpan(name, out var value))
        {
            return Result<bool>.Failure(new VerdandiError("missing", name ?? string.Empty));
        }

        if (value.SequenceEqual("true"u8))
        {
            return Result<bool>.Success(true);
        }

        if (value.SequenceEqual("false"u8))
        {
            return Result<bool>.Success(false);
        }

        return Result<bool>.Failure(new VerdandiError("invalid", name));
    }

    /// <summary>
    /// 解码一个规范十进制 Int64；拒绝加号、前导零、负零、空值和溢出。
    /// </summary>
    /// <param name="name">要查找的精确字段名。</param>
    /// <returns>有符号整数，或 missing/invalid 错误。</returns>
    public Result<long> GetInt64(string name)
    {
        if (!TryGetSpan(name, out var value))
        {
            return Result<long>.Failure(new VerdandiError("missing", name ?? string.Empty));
        }

        return ScalarEncoding.DecodeInt64(value, name);
    }

    /// <summary>
    /// 解码一个规范十进制 UInt64；拒绝符号、前导零、空值和溢出。
    /// </summary>
    /// <param name="name">要查找的精确字段名。</param>
    /// <returns>无符号整数，或 missing/invalid 错误。</returns>
    public Result<ulong> GetUInt64(string name)
    {
        if (!TryGetSpan(name, out var value))
        {
            return Result<ulong>.Failure(new VerdandiError("missing", name ?? string.Empty));
        }

        return ScalarEncoding.DecodeUInt64(value, name);
    }

    /// <summary>
    /// 返回一个完全脱离当前对象的字段数组；每个值都会复制，适合重型调试或桥接代码。
    /// </summary>
    /// <returns>按字段名序排列的拥有型字段数组。</returns>
    public Field[] ToArray()
    {
        var output = GC.AllocateUninitializedArray<Field>(_entries.Length);
        for (var index = 0; index < _entries.Length; index++)
        {
            var entry = _entries[index];
            output[index] = new Field(entry.Name, _payload.AsSpan(entry.ValueOffset, entry.ValueLength).ToArray());
        }

        return output;
    }

    /// <summary>
    /// Fields 已经拥有且不可变，因此原始 Fields 编码不会复制其连续存储。
    /// </summary>
    /// <returns>当前完整集合。</returns>
    public Result<Fields> EncodeFields() => Result<Fields>.Success(this);

    /// <summary>
    /// 原始 Fields 解码保持同一个不可变值，不建立第二套字段表示。
    /// </summary>
    /// <param name="fields">已经拥有的完整集合。</param>
    /// <returns>同一个不可变集合。</returns>
    public static Result<Fields> DecodeFields(Fields fields)
    {
        ArgumentNullException.ThrowIfNull(fields);
        return Result<Fields>.Success(fields);
    }

    /// <summary>
    /// 返回供一次同步 C ABI 调用固定的连续载荷；调用方不得在固定范围外保存地址。
    /// </summary>
    internal byte[] Payload => _payload;

    /// <summary>
    /// 返回只含连续载荷偏移的内部有序索引；调用方不得修改数组。
    /// </summary>
    internal Entry[] Entries => _entries;

    /// <summary>
    /// 在内部连续存储中借用字段 Span；仅在当前同步调用栈内使用。
    /// </summary>
    /// <param name="name">精确字段名。</param>
    /// <param name="value">找到时借用的只读字节。</param>
    /// <returns>字段是否存在。</returns>
    internal bool TryGetSpan(string? name, out ReadOnlySpan<byte> value)
    {
        if (TryFind(name, out var entry))
        {
            value = _payload.AsSpan(entry.ValueOffset, entry.ValueLength);
            return true;
        }

        value = default;
        return false;
    }

    /// <summary>
    /// 使用有序名称索引查找字段，避免为普通解码建立 Dictionary。
    /// </summary>
    /// <param name="name">精确字段名。</param>
    /// <param name="entry">找到的连续载荷索引。</param>
    /// <returns>字段是否存在。</returns>
    private bool TryFind(string? name, out Entry entry)
    {
        if (name is not null)
        {
            var low = 0;
            var high = _entries.Length - 1;
            while (low <= high)
            {
                var middle = low + ((high - low) / 2);
                var comparison = StringComparer.Ordinal.Compare(_entries[middle].Name, name);
                if (comparison == 0)
                {
                    entry = _entries[middle];
                    return true;
                }

                if (comparison < 0)
                {
                    low = middle + 1;
                }
                else
                {
                    high = middle - 1;
                }
            }
        }

        entry = default;
        return false;
    }

    /// <summary>
    /// 描述一个字段在连续载荷中的名称和值范围。
    /// </summary>
    internal readonly record struct Entry(string Name, int NameOffset, int NameLength, int ValueOffset, int ValueLength);

    /// <summary>
    /// 暂存已经复制的字段，直到名称排序和连续载荷大小检查完成。
    /// </summary>
    private sealed record PendingField(string Name, byte[] NameBytes, byte[] Value);
}

/// <summary>
/// 为应用 Codec 构造完整 Fields；实例不是线程安全的，也不会修改已经 Build 的结果。
/// </summary>
public sealed class FieldsBuilder
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private readonly Dictionary<string, byte[]> _values = new(StringComparer.Ordinal);

    /// <summary>
    /// 复制并加入一个二进制字段；重复名称返回 contract 且保留原值。
    /// </summary>
    /// <param name="name">顶层字段名称。</param>
    /// <param name="value">调用期间借用的原始值。</param>
    /// <returns>插入结果。</returns>
    public Result Add(string name, ReadOnlySpan<byte> value)
    {
        if (name is null)
        {
            return Result.Failure(new VerdandiError("invalid", "field"));
        }

        if (_values.ContainsKey(name))
        {
            return Result.Failure(new VerdandiError("contract", name));
        }

        try
        {
            _values.Add(name, value.ToArray());
            return Result.Success();
        }
        catch (OutOfMemoryException exception)
        {
            return Result.Failure(new VerdandiError("capacity", name, exception.Message));
        }
    }

    /// <summary>
    /// 以严格 UTF-8 编码并加入一个文本字段；无效 UTF-16 或重复名称返回错误。
    /// </summary>
    /// <param name="name">顶层字段名称。</param>
    /// <param name="value">要编码的完整文本。</param>
    /// <returns>插入结果。</returns>
    public Result Add(string name, string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        try
        {
            return Add(name, StrictUtf8.GetBytes(value));
        }
        catch (EncoderFallbackException exception)
        {
            return Result.Failure(new VerdandiError("invalid", name ?? string.Empty, exception.Message));
        }
    }

    /// <summary>
    /// 以稳定 ASCII `true` 或 `false` 编码并加入布尔字段。
    /// </summary>
    /// <param name="name">顶层字段名称。</param>
    /// <param name="value">布尔值。</param>
    /// <returns>插入结果。</returns>
    public Result Add(string name, bool value) => Add(name, value ? "true"u8 : "false"u8);

    /// <summary>
    /// 以无前导零的规范十进制编码并加入 Int64 字段。
    /// </summary>
    /// <param name="name">顶层字段名称。</param>
    /// <param name="value">有符号整数。</param>
    /// <returns>插入结果。</returns>
    public Result Add(string name, long value) => Add(name, ScalarEncoding.Encode(value));

    /// <summary>
    /// 以无前导零的规范十进制编码并加入 UInt64 字段。
    /// </summary>
    /// <param name="name">顶层字段名称。</param>
    /// <param name="value">无符号整数。</param>
    /// <returns>插入结果。</returns>
    public Result Add(string name, ulong value) => Add(name, ScalarEncoding.Encode(value));

    /// <summary>
    /// 复制当前全部字段并构造一个不可变连续集合；构造器在返回后仍可继续使用。
    /// </summary>
    /// <returns>按字段名排序的完整集合，或容量错误。</returns>
    public Result<Fields> Build()
    {
        try
        {
            var values = new Field[_values.Count];
            var index = 0;
            foreach (var pair in _values)
            {
                values[index++] = new Field(pair.Key, pair.Value);
            }

            return Fields.Create(values);
        }
        catch (OutOfMemoryException exception)
        {
            return Result<Fields>.Failure(new VerdandiError("capacity", "fields", exception.Message));
        }
    }
}

/// <summary>
/// 实现跨语言规范整数编码，避免区域设置、机器字长或宽松解析造成字节差异。
/// </summary>
internal static class ScalarEncoding
{
    /// <summary>
    /// 把 Int64 格式化为规范 UTF-8 十进制字节。
    /// </summary>
    /// <param name="value">要格式化的整数。</param>
    /// <returns>精确长度的拥有型字节。</returns>
    internal static byte[] Encode(long value)
    {
        Span<byte> buffer = stackalloc byte[20];
        if (!Utf8Formatter.TryFormat(value, buffer, out var written))
        {
            throw new InvalidOperationException("Int64 formatting failed.");
        }

        return buffer[..written].ToArray();
    }

    /// <summary>
    /// 把 UInt64 格式化为规范 UTF-8 十进制字节。
    /// </summary>
    /// <param name="value">要格式化的整数。</param>
    /// <returns>精确长度的拥有型字节。</returns>
    internal static byte[] Encode(ulong value)
    {
        Span<byte> buffer = stackalloc byte[20];
        if (!Utf8Formatter.TryFormat(value, buffer, out var written))
        {
            throw new InvalidOperationException("UInt64 formatting failed.");
        }

        return buffer[..written].ToArray();
    }

    /// <summary>
    /// 验证并解析规范 Int64 字节，拒绝任何等价但非规范的文本形式。
    /// </summary>
    /// <param name="value">完整字段字节。</param>
    /// <param name="field">错误中使用的字段名。</param>
    /// <returns>解析值或 invalid 错误。</returns>
    internal static Result<long> DecodeInt64(ReadOnlySpan<byte> value, string field)
    {
        if (!IsCanonicalSigned(value) || !Utf8Parser.TryParse(value, out long parsed, out var consumed) || consumed != value.Length)
        {
            return Result<long>.Failure(new VerdandiError("invalid", field));
        }

        return Result<long>.Success(parsed);
    }

    /// <summary>
    /// 验证并解析规范 UInt64 字节，拒绝任何符号或非规范文本形式。
    /// </summary>
    /// <param name="value">完整字段字节。</param>
    /// <param name="field">错误中使用的字段名。</param>
    /// <returns>解析值或 invalid 错误。</returns>
    internal static Result<ulong> DecodeUInt64(ReadOnlySpan<byte> value, string field)
    {
        if (!IsCanonicalUnsigned(value) || !Utf8Parser.TryParse(value, out ulong parsed, out var consumed) || consumed != value.Length)
        {
            return Result<ulong>.Failure(new VerdandiError("invalid", field));
        }

        return Result<ulong>.Success(parsed);
    }

    /// <summary>
    /// 检查有符号十进制的符号、前导零和字符集合，不负责数值范围。
    /// </summary>
    /// <param name="value">待检查字节。</param>
    /// <returns>文本形状是否规范。</returns>
    private static bool IsCanonicalSigned(ReadOnlySpan<byte> value)
    {
        if (value.IsEmpty)
        {
            return false;
        }

        var start = 0;
        if (value[0] == (byte)'-')
        {
            if (value.Length == 1 || value[1] == (byte)'0')
            {
                return false;
            }

            start = 1;
        }

        return IsCanonicalDigits(value[start..]);
    }

    /// <summary>
    /// 检查无符号十进制的前导零和字符集合，不负责数值范围。
    /// </summary>
    /// <param name="value">待检查字节。</param>
    /// <returns>文本形状是否规范。</returns>
    private static bool IsCanonicalUnsigned(ReadOnlySpan<byte> value) => !value.IsEmpty && IsCanonicalDigits(value);

    /// <summary>
    /// 检查非空十进制数字序列并拒绝多位前导零。
    /// </summary>
    /// <param name="value">不含符号的字节。</param>
    /// <returns>数字序列是否规范。</returns>
    private static bool IsCanonicalDigits(ReadOnlySpan<byte> value)
    {
        if (value.IsEmpty || (value.Length > 1 && value[0] == (byte)'0'))
        {
            return false;
        }

        foreach (var current in value)
        {
            if (current is < (byte)'0' or > (byte)'9')
            {
                return false;
            }
        }

        return true;
    }
}
