using System.Runtime.InteropServices;
using System.Text;

namespace Verdandi.Internal;

/// <summary>
/// 对应 C ABI 的不拥有 UTF-8 字符串视图；地址只能在声明它的同步调用期间使用。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal readonly unsafe struct NativeStringView
{
    /// <summary>
    /// 构造一个不拥有的字节范围。
    /// </summary>
    /// <param name="data">首字节地址；空值允许为空。</param>
    /// <param name="size">字节数量，不包含零结尾。</param>
    internal NativeStringView(byte* data, nuint size)
    {
        Data = data;
        Size = size;
    }

    /// <summary>借用数据首地址。</summary>
    internal readonly byte* Data;

    /// <summary>借用字节数量。</summary>
    internal readonly nuint Size;
}

/// <summary>
/// 对应 C ABI 的不拥有二进制视图；地址只能在声明它的同步调用期间使用。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal readonly unsafe struct NativeBytesView
{
    /// <summary>
    /// 构造一个不拥有的二进制范围。
    /// </summary>
    /// <param name="data">首字节地址；空值允许为空。</param>
    /// <param name="size">字节数量。</param>
    internal NativeBytesView(byte* data, nuint size)
    {
        Data = data;
        Size = size;
    }

    /// <summary>借用数据首地址。</summary>
    internal readonly byte* Data;

    /// <summary>借用字节数量。</summary>
    internal readonly nuint Size;
}

/// <summary>
/// 对应一个借用名称和值的 C ABI 顶层字段。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal readonly struct NativeFieldView
{
    /// <summary>
    /// 构造一个借用字段。
    /// </summary>
    /// <param name="name">UTF-8 字段名称。</param>
    /// <param name="value">原始字段值。</param>
    internal NativeFieldView(NativeStringView name, NativeBytesView value)
    {
        Name = name;
        Value = value;
    }

    /// <summary>借用字段名称。</summary>
    internal readonly NativeStringView Name;

    /// <summary>借用字段值。</summary>
    internal readonly NativeBytesView Value;
}

/// <summary>
/// 对应 C ABI 的完整借用字段数组。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal readonly unsafe struct NativeFieldsView
{
    /// <summary>
    /// 构造一个借用字段数组。
    /// </summary>
    /// <param name="data">连续字段视图首地址。</param>
    /// <param name="size">字段数量。</param>
    internal NativeFieldsView(NativeFieldView* data, nuint size)
    {
        Data = data;
        Size = size;
    }

    /// <summary>借用字段视图首地址。</summary>
    internal readonly NativeFieldView* Data;

    /// <summary>字段数量。</summary>
    internal readonly nuint Size;
}

/// <summary>
/// 精确映射 C ABI v1 的固定拥有型错误缓冲区。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeError
{
    private const int CodeBytes = 16;
    private const int FieldBytes = 256;
    private const int DetailBytes = 513;
    private static readonly Encoding ErrorUtf8 = new UTF8Encoding(false, false);

    /// <summary>可选权威 revision。</summary>
    internal ulong Revision;

    /// <summary>非零表示 Revision 有效。</summary>
    internal byte HasRevision;

    /// <summary>零结尾稳定错误类别。</summary>
    internal fixed byte Code[CodeBytes];

    /// <summary>零结尾字段上下文。</summary>
    internal fixed byte Field[FieldBytes];

    /// <summary>零结尾人工诊断。</summary>
    internal fixed byte Detail[DetailBytes];

    /// <summary>
    /// 把固定 C 缓冲区立即复制为完全拥有的托管错误。
    /// </summary>
    /// <returns>不再借用当前结构的错误。</returns>
    internal readonly VerdandiError ToManaged()
    {
        fixed (byte* code = Code)
        fixed (byte* field = Field)
        fixed (byte* detail = Detail)
        {
            return new VerdandiError(
                Decode(code, CodeBytes),
                Decode(field, FieldBytes),
                Decode(detail, DetailBytes),
                HasRevision == 0 ? null : Revision);
        }
    }

    /// <summary>
    /// 把托管错误截断写入固定 C 缓冲区，供 Selector 回调把失败交还原生事务。
    /// </summary>
    /// <param name="error">要拥有型复制的稳定错误。</param>
    internal void Write(VerdandiError error)
    {
        this = default;
        if (error.Revision is not null)
        {
            Revision = error.Revision.Value;
            HasRevision = 1;
        }

        fixed (byte* code = Code)
        fixed (byte* field = Field)
        fixed (byte* detail = Detail)
        {
            var codeValue = error.Code.Length != 0 && ErrorUtf8.GetByteCount(error.Code) < CodeBytes ? error.Code : "contract";
            Encode(codeValue, code, CodeBytes);
            Encode(error.Field, field, FieldBytes);
            Encode(error.Detail, detail, DetailBytes);
        }
    }

    /// <summary>
    /// 解码固定容量内第一个零字节之前的 UTF-8 文本；错误诊断中的无效序列使用替换字符。
    /// </summary>
    /// <param name="data">固定缓冲区首地址。</param>
    /// <param name="capacity">缓冲区总容量。</param>
    /// <returns>托管文本。</returns>
    private static string Decode(byte* data, int capacity)
    {
        var length = 0;
        while (length < capacity && data[length] != 0)
        {
            length++;
        }

        return ErrorUtf8.GetString(new ReadOnlySpan<byte>(data, length));
    }

    /// <summary>
    /// 在保留零结尾的前提下按 UTF-8 字符边界写入固定容量，确保任何托管异常都不会穿越 C 回调。
    /// </summary>
    /// <param name="value">要写入的文本。</param>
    /// <param name="destination">固定缓冲区首地址。</param>
    /// <param name="capacity">包含零结尾的总容量。</param>
    private static void Encode(string value, byte* destination, int capacity)
    {
        var output = new Span<byte>(destination, capacity);
        output.Clear();
        if (value.Length == 0 || capacity <= 1)
        {
            return;
        }

        var encoder = ErrorUtf8.GetEncoder();
        encoder.Convert(value.AsSpan(), output[..^1], true, out _, out _, out _);
    }
}

/// <summary>
/// 精确映射 C ABI Registration 固定构造参数。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeRegistrationOptions
{
    /// <summary>借用 Type 文本。</summary>
    internal NativeStringView Type;

    /// <summary>精确毫秒 TTL。</summary>
    internal ulong TtlMilliseconds;

    /// <summary>显式续期间隔；HasRenewInterval 为零时忽略。</summary>
    internal ulong RenewIntervalMilliseconds;

    /// <summary>正整数应用版本。</summary>
    internal ulong Version;

    /// <summary>非零表示调用方显式提供续期间隔。</summary>
    internal byte HasRenewInterval;
}

/// <summary>
/// 精确映射 C ABI 候选元数据；UUID 仍是原生借用视图。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeRegistrationMetadata
{
    /// <summary>借用的 32 字节 UUID。</summary>
    internal NativeStringView Uuid;

    /// <summary>内容 revision。</summary>
    internal ulong Revision;

    /// <summary>最近一次 Redis 毫秒时间戳。</summary>
    internal ulong Timestamp;

    /// <summary>租约毫秒数。</summary>
    internal ulong TtlMilliseconds;

    /// <summary>应用版本。</summary>
    internal ulong Version;
}

/// <summary>
/// 精确映射 C ABI Catalog Path 借用视图。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeCatalogPathView
{
    /// <summary>借用 Part。</summary>
    internal NativeStringView Part;

    /// <summary>借用 ID。</summary>
    internal NativeStringView Id;
}

/// <summary>
/// 精确映射 C ABI Catalog 订阅范围；所有数组仅在创建调用内借用。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCatalogSubscription
{
    /// <summary>非零表示订阅完整 Zone。</summary>
    internal int Zone;

    /// <summary>借用 Part 数组。</summary>
    internal NativeStringView* Parts;

    /// <summary>Part 数量。</summary>
    internal nuint PartCount;

    /// <summary>借用精确 Path 数组。</summary>
    internal NativeCatalogPathView* Paths;

    /// <summary>Path 数量。</summary>
    internal nuint PathCount;
}
