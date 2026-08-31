using System.Diagnostics.CodeAnalysis;

namespace Verdandi;

/// <summary>
/// 保存跨语言稳定的 Verdandi 错误类别、相关字段、诊断说明以及可选的权威 revision。
/// </summary>
/// <param name="Code">供程序分支使用的稳定小写错误类别。</param>
/// <param name="Field">导致失败的参数、配置或数据字段；没有具体字段时为空。</param>
/// <param name="Detail">仅供诊断的说明；调用方不得依赖其文本进行程序分支。</param>
/// <param name="Revision">Redis 返回的权威 revision；错误不携带 revision 时为空。</param>
public sealed record VerdandiError(string Code, string Field = "", string Detail = "", ulong? Revision = null)
{
    /// <summary>
    /// 表示一个未经构造的默认 Result；正常 Verdandi 操作不会主动返回该错误。
    /// </summary>
    internal static readonly VerdandiError Uninitialized = new("invalid", "result", "The result was not initialized.");

    /// <summary>
    /// 返回适合人工诊断的错误文本；机器逻辑只能读取 <see cref="Code"/>。
    /// </summary>
    /// <returns>包含错误类别以及非空上下文的文本。</returns>
    public override string ToString()
    {
        var field = Field.Length == 0 ? string.Empty : $" field={Field}";
        var revision = Revision is null ? string.Empty : $" revision={Revision.Value}";
        var detail = Detail.Length == 0 ? string.Empty : $" detail={Detail}";
        return $"{Code}{field}{revision}{detail}";
    }
}

/// <summary>
/// 表示一个没有成功载荷的 Verdandi 操作结果，避免把预期的协议或生命周期失败转换成异常。
/// </summary>
public readonly struct Result
{
    private readonly VerdandiError? _error;
    private readonly byte _state;

    /// <summary>
    /// 使用明确状态和可选错误构造内部结果；公开调用方通过 <see cref="Success"/> 或 <see cref="Failure"/> 创建结果。
    /// </summary>
    /// <param name="state">一表示成功，二表示失败。</param>
    /// <param name="error">失败时拥有的稳定错误。</param>
    private Result(byte state, VerdandiError? error)
    {
        _state = state;
        _error = error;
    }

    /// <summary>
    /// 返回结果是否明确成功；默认初始化的 Result 不是成功结果。
    /// </summary>
    public bool IsSuccess => _state == 1;

    /// <summary>
    /// 返回失败错误；成功时为空，默认初始化时返回明确的 invalid/result 错误。
    /// </summary>
    public VerdandiError? Error => IsSuccess ? null : _error ?? VerdandiError.Uninitialized;

    /// <summary>
    /// 构造一个没有载荷的成功结果。
    /// </summary>
    /// <returns>明确处于成功状态的值。</returns>
    public static Result Success() => new(1, null);

    /// <summary>
    /// 构造一个失败结果并保留完整稳定错误。
    /// </summary>
    /// <param name="error">不能为空的失败原因。</param>
    /// <returns>明确处于失败状态的值。</returns>
    public static Result Failure(VerdandiError error)
    {
        ArgumentNullException.ThrowIfNull(error);
        return new Result(2, error);
    }
}

/// <summary>
/// 表示一个携带成功值或稳定 Verdandi 错误的操作结果。
/// </summary>
/// <typeparam name="T">成功路径拥有的托管值类型。</typeparam>
public readonly struct Result<T>
{
    private readonly T? _value;
    private readonly VerdandiError? _error;
    private readonly byte _state;

    /// <summary>
    /// 使用明确状态、成功值和可选错误构造内部结果。
    /// </summary>
    /// <param name="state">一表示成功，二表示失败。</param>
    /// <param name="value">成功路径拥有的值。</param>
    /// <param name="error">失败路径拥有的错误。</param>
    private Result(byte state, T? value, VerdandiError? error)
    {
        _state = state;
        _value = value;
        _error = error;
    }

    /// <summary>
    /// 返回结果是否明确成功；默认初始化的 Result 不是成功结果。
    /// </summary>
    public bool IsSuccess => _state == 1;

    /// <summary>
    /// 返回成功值；失败或默认初始化时抛出 <see cref="InvalidOperationException"/>，正常分支应先检查 <see cref="IsSuccess"/>。
    /// </summary>
    public T Value => IsSuccess ? _value! : throw new InvalidOperationException((Error ?? VerdandiError.Uninitialized).ToString());

    /// <summary>
    /// 返回失败错误；成功时为空，默认初始化时返回明确的 invalid/result 错误。
    /// </summary>
    public VerdandiError? Error => IsSuccess ? null : _error ?? VerdandiError.Uninitialized;

    /// <summary>
    /// 构造一个拥有成功值的结果；引用类型成功值允许由其业务契约决定是否为空。
    /// </summary>
    /// <param name="value">成功路径拥有的值。</param>
    /// <returns>明确处于成功状态的结果。</returns>
    public static Result<T> Success(T value) => new(1, value, null);

    /// <summary>
    /// 构造一个拥有稳定错误的失败结果。
    /// </summary>
    /// <param name="error">不能为空的失败原因。</param>
    /// <returns>明确处于失败状态的结果。</returns>
    public static Result<T> Failure(VerdandiError error)
    {
        ArgumentNullException.ThrowIfNull(error);
        return new Result<T>(2, default, error);
    }

    /// <summary>
    /// 在成功时返回值并清空错误，在失败时返回默认值和稳定错误；该方法不会抛出 Verdandi 操作错误。
    /// </summary>
    /// <param name="value">成功值；失败时为默认值。</param>
    /// <param name="error">失败错误；成功时为空。</param>
    /// <returns>结果是否成功。</returns>
    public bool TryGetValue([MaybeNull] out T value, [NotNullWhen(false)] out VerdandiError? error)
    {
        if (IsSuccess)
        {
            value = _value!;
            error = null;
            return true;
        }

        value = default;
        error = _error ?? VerdandiError.Uninitialized;
        return false;
    }
}

/// <summary>
/// 表示一次非阻塞异步诊断读取；没有可用诊断与读取失败是两个不同状态。
/// </summary>
/// <param name="Available">本次调用是否取出了一条诊断。</param>
/// <param name="Error">取出的拥有型诊断；没有诊断时为空。</param>
public readonly record struct Diagnostic(bool Available, VerdandiError? Error);
