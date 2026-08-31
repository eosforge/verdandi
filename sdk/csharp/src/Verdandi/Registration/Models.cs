namespace Verdandi.Registration;

/// <summary>
/// 定义一条 Registration 的不可变本地构造选项。
/// </summary>
/// <param name="Type">Zone 内的 Registry Type；具体字符和长度限制由同一原生核心验证。</param>
/// <param name="Ttl">正整数、整毫秒租约时长。</param>
/// <param name="Version">正整数应用版本；内容更新可独立修改它。</param>
/// <param name="RenewInterval">可选正整数整毫秒自动续期间隔；为空时由核心按 TTL 默认。</param>
public readonly record struct RegistrationOptions(string Type, TimeSpan Ttl, ulong Version, TimeSpan? RenewInterval = null);

/// <summary>
/// 保存 Selector 和脱离结果可见的稳定 Registration 元数据。
/// </summary>
/// <param name="Uuid">当前进程构造 Registration 时生成的 32 位小写十六进制 UUID。</param>
/// <param name="Revision">仅在 Version 或 Data 内容变化时递增的内容 revision。</param>
/// <param name="TimestampMilliseconds">最近一次 Redis 确认的绝对毫秒时间戳。</param>
/// <param name="TtlMilliseconds">不可变租约时长。</param>
/// <param name="Version">当前应用版本。</param>
public readonly record struct RegistrationMetadata(
    string Uuid,
    ulong Revision,
    ulong TimestampMilliseconds,
    ulong TtlMilliseconds,
    ulong Version);

/// <summary>
/// 表示当前 Selector 策略事务中的不透明候选身份；只能把同一次回调产生的 Choice 交还该回调。
/// </summary>
public readonly struct Choice : IEquatable<Choice>
{
    private readonly nuint _index;
    private readonly long _transaction;

    /// <summary>
    /// 由当前借用候选会话构造身份；应用无法自行构造有效 Choice。
    /// </summary>
    /// <param name="index">原生候选索引。</param>
    /// <param name="transaction">当前托管策略事务身份。</param>
    internal Choice(nuint index, long transaction)
    {
        _index = index;
        _transaction = transaction;
    }

    /// <summary>
    /// 比较候选索引和事务身份；来自不同回调的同一 Registration 仍不相等。
    /// </summary>
    /// <param name="other">另一个不透明 Choice。</param>
    /// <returns>两者是否属于同一回调和索引。</returns>
    public bool Equals(Choice other) => _index == other._index && _transaction == other._transaction;

    /// <summary>
    /// 比较一个对象是否为相同 Choice。
    /// </summary>
    /// <param name="obj">待比较对象。</param>
    /// <returns>类型和身份都相同时为真。</returns>
    public override bool Equals(object? obj) => obj is Choice other && Equals(other);

    /// <summary>
    /// 返回同时包含索引和事务身份的哈希码。
    /// </summary>
    /// <returns>哈希码。</returns>
    public override int GetHashCode() => HashCode.Combine(_index, _transaction);

    /// <summary>返回内部候选索引；只供 Selector 当前事务验证后使用。</summary>
    internal nuint Index => _index;

    /// <summary>返回内部事务身份；只供当前借用会话验证。</summary>
    internal long Transaction => _transaction;

    /// <summary>
    /// 比较两个 Choice 是否完全相同。
    /// </summary>
    /// <param name="left">左值。</param>
    /// <param name="right">右值。</param>
    /// <returns>是否相同。</returns>
    public static bool operator ==(Choice left, Choice right) => left.Equals(right);

    /// <summary>
    /// 比较两个 Choice 是否不同。
    /// </summary>
    /// <param name="left">左值。</param>
    /// <param name="right">右值。</param>
    /// <returns>是否不同。</returns>
    public static bool operator !=(Choice left, Choice right) => !left.Equals(right);
}

/// <summary>
/// 保存策略回调内已经解码的候选值以及只能在该回调内使用的 Choice。
/// </summary>
/// <typeparam name="TAttr">不可变 Attr 类型。</typeparam>
/// <typeparam name="TData">可更新 Data 类型。</typeparam>
/// <param name="Choice">当前事务身份。</param>
/// <param name="Metadata">稳定 Registration 元数据。</param>
/// <param name="Attr">解码后的完整不可变 Attr。</param>
/// <param name="Data">当前本地预测优先的完整 Data。</param>
public readonly record struct CandidateView<TAttr, TData>(Choice Choice, RegistrationMetadata Metadata, TAttr Attr, TData Data);

/// <summary>
/// 保存从 Selector 事务或快照脱离的完整候选；它不再借用锁、原生视图或策略事务。
/// </summary>
/// <typeparam name="TAttr">不可变 Attr 类型。</typeparam>
/// <typeparam name="TData">可更新 Data 类型。</typeparam>
/// <param name="Metadata">稳定 Registration 元数据。</param>
/// <param name="Attr">拥有型完整 Attr。</param>
/// <param name="Data">拥有型完整 Data。</param>
public readonly record struct Candidate<TAttr, TData>(RegistrationMetadata Metadata, TAttr Attr, TData Data);

/// <summary>
/// 保存一个暂不可选择但仍在有限恢复窗口内的完整候选。
/// </summary>
/// <typeparam name="TAttr">不可变 Attr 类型。</typeparam>
/// <typeparam name="TData">可更新 Data 类型。</typeparam>
/// <param name="Candidate">脱离且拥有的完整候选。</param>
/// <param name="RetainedUntilMilliseconds">超过该 Redis 绝对毫秒后核心可最终删除候选。</param>
public readonly record struct RetainedCandidate<TAttr, TData>(Candidate<TAttr, TData> Candidate, ulong RetainedUntilMilliseconds);

/// <summary>
/// 保存一次显式重型 Selector 完整视图；活动与 retained 数组均与内部监听状态脱离。
/// </summary>
/// <typeparam name="TAttr">不可变 Attr 类型。</typeparam>
/// <typeparam name="TData">可更新 Data 类型。</typeparam>
public sealed class SelectorSnapshot<TAttr, TData>
{
    /// <summary>
    /// 接管一次完整解码结果并用只读集合包装数组，防止普通调用方改写快照槽位。
    /// </summary>
    /// <param name="generation">视图 generation。</param>
    /// <param name="isSynchronized">快照创建时是否完成权威同步。</param>
    /// <param name="active">活动候选数组。</param>
    /// <param name="retained">恢复窗口候选数组。</param>
    internal SelectorSnapshot(
        ulong generation,
        bool isSynchronized,
        Candidate<TAttr, TData>[] active,
        RetainedCandidate<TAttr, TData>[] retained)
    {
        Generation = generation;
        IsSynchronized = isSynchronized;
        Active = Array.AsReadOnly(active);
        Retained = Array.AsReadOnly(retained);
    }

    /// <summary>返回不可变视图 generation。</summary>
    public ulong Generation { get; }

    /// <summary>返回快照创建时是否完成权威同步。</summary>
    public bool IsSynchronized { get; }

    /// <summary>返回可参与选择的完整脱离候选。</summary>
    public IReadOnlyList<Candidate<TAttr, TData>> Active { get; }

    /// <summary>返回不可选择但等待有限恢复的完整脱离候选。</summary>
    public IReadOnlyList<RetainedCandidate<TAttr, TData>> Retained { get; }
}

/// <summary>
/// 同步执行一个选择零或一个候选的策略；回调不能保存借用 Candidates，也不能递归调用同一 Selector。
/// </summary>
/// <typeparam name="TAttr">不可变 Attr 类型。</typeparam>
/// <typeparam name="TData">可更新 Data 类型。</typeparam>
/// <param name="candidates">仅在本次回调词法范围内有效的借用视图。</param>
/// <returns>空 Choice 表示不选择，Choice 表示选择，失败会回滚全部本地 Mutate。</returns>
public delegate Result<Choice?> OnePolicy<TAttr, TData>(scoped Candidates<TAttr, TData> candidates)
    where TAttr : IFieldValue<TAttr>
    where TData : IFieldValue<TData>;

/// <summary>
/// 同步执行一个选择任意数量候选的策略；结果不得含重复或来自其他回调的 Choice。
/// </summary>
/// <typeparam name="TAttr">不可变 Attr 类型。</typeparam>
/// <typeparam name="TData">可更新 Data 类型。</typeparam>
/// <param name="candidates">仅在本次回调词法范围内有效的借用视图。</param>
/// <returns>Choice 集合或失败；空集合表示成功但不选择。</returns>
public delegate Result<IReadOnlyList<Choice>> AnyPolicy<TAttr, TData>(scoped Candidates<TAttr, TData> candidates)
    where TAttr : IFieldValue<TAttr>
    where TData : IFieldValue<TData>;
