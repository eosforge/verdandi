namespace Verdandi.Catalog;

/// <summary>
/// 表示 Catalog 完整记录的稳定结构类别；Redis 仍只保存扁平二进制 Fields。
/// </summary>
public enum CatalogKind
{
    /// <summary>由应用 Codec 定义字段布局的语义单值。</summary>
    Value,

    /// <summary>使用协议数组索引字段名的连续数组。</summary>
    Array,

    /// <summary>按字段名组织的 Map。</summary>
    Map,
}

/// <summary>
/// 标识 Catalog Zone 内的一条完整记录。
/// </summary>
/// <param name="Part">一级分区名称。</param>
/// <param name="Id">Part 内的完整记录身份。</param>
public readonly record struct CatalogPath(string Part, string Id);

/// <summary>
/// 定义 Catalog Subscriber 的不可变覆盖范围；构造时复制 Part 和 Path 序列。
/// </summary>
public sealed class CatalogSubscription
{
    /// <summary>
    /// 构造一个覆盖完整 Zone、若干完整 Part 和/或若干精确 Path 的订阅。
    /// </summary>
    /// <param name="zone">为真时覆盖当前 Catalog Zone 的全部 Path。</param>
    /// <param name="parts">要完整覆盖的 Part；为空表示没有 Part 范围。</param>
    /// <param name="paths">要覆盖的精确 Path；为空表示没有精确范围。</param>
    public CatalogSubscription(bool zone = false, IEnumerable<string>? parts = null, IEnumerable<CatalogPath>? paths = null)
    {
        Zone = zone;
        Parts = Array.AsReadOnly(parts?.ToArray() ?? []);
        Paths = Array.AsReadOnly(paths?.ToArray() ?? []);
    }

    /// <summary>返回是否覆盖完整 Zone。</summary>
    public bool Zone { get; }

    /// <summary>返回构造时复制的完整 Part 范围。</summary>
    public IReadOnlyList<string> Parts { get; }

    /// <summary>返回构造时复制的精确 Path 范围。</summary>
    public IReadOnlyList<CatalogPath> Paths { get; }
}

/// <summary>
/// 保存从同一个不可变 Catalog Entry 状态读取的 revision、状态、同步标记和可选强类型值。
/// </summary>
/// <typeparam name="T">应用 Catalog 值类型。</typeparam>
public sealed class CatalogSnapshot<T>
{
    private readonly T? _value;

    /// <summary>
    /// 构造一份完全托管的 Entry 快照。
    /// </summary>
    /// <param name="revision">最后已知完整 revision。</param>
    /// <param name="status">稳定小写状态。</param>
    /// <param name="isSynchronized">是否完成权威同步。</param>
    /// <param name="hasValue">当前状态是否包含完整值。</param>
    /// <param name="value">存在时已经解码的值。</param>
    internal CatalogSnapshot(ulong revision, string status, bool isSynchronized, bool hasValue, T? value)
    {
        Revision = revision;
        Status = status;
        IsSynchronized = isSynchronized;
        HasValue = hasValue;
        _value = value;
    }

    /// <summary>返回 Entry 当前不可变状态的完整 revision。</summary>
    public ulong Revision { get; }

    /// <summary>返回稳定小写状态，如 present、deleted 或 unavailable。</summary>
    public string Status { get; }

    /// <summary>返回该状态是否完成权威同步。</summary>
    public bool IsSynchronized { get; }

    /// <summary>返回当前状态是否包含完整值。</summary>
    public bool HasValue { get; }

    /// <summary>
    /// 返回完整解码值；<see cref="HasValue"/> 为假时抛出 <see cref="InvalidOperationException"/>。
    /// </summary>
    public T Value => HasValue ? _value! : throw new InvalidOperationException("The catalog snapshot has no value.");
}
