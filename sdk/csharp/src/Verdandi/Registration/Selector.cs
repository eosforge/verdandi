using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Verdandi.Internal;

namespace Verdandi.Registration;

/// <summary>
/// 只在一次同步 Selector 策略回调内有效的借用候选视图；ref struct 阻止它进入堆对象、异步状态机或闭包。
/// </summary>
/// <typeparam name="TAttr">不可变 Attr 类型。</typeparam>
/// <typeparam name="TData">可更新 Data 类型。</typeparam>
public readonly ref struct Candidates<TAttr, TData>
    where TAttr : IFieldValue<TAttr>
    where TData : IFieldValue<TData>
{
    private readonly CandidateSession<TAttr, TData> _session;

    /// <summary>
    /// 包装当前回调拥有的候选会话；会话在回调返回时立即失效。
    /// </summary>
    /// <param name="session">当前唯一事务会话。</param>
    internal Candidates(CandidateSession<TAttr, TData> session)
    {
        _session = session;
    }

    /// <summary>返回当前活动候选数量；读取不执行 Redis I/O。</summary>
    public nuint Count => _session.Count;

    /// <summary>
    /// 按索引惰性复制并解码一个候选；同一回调内重复读取使用托管缓存，不会再次穿越 C ABI。
    /// </summary>
    /// <param name="index">零开始候选索引。</param>
    /// <returns>带当前事务 Choice 的候选视图，或 invalid/codec/corrupt 错误。</returns>
    public Result<CandidateView<TAttr, TData>> Get(nuint index) => _session.Get(index);

    /// <summary>
    /// 在当前事务暂存一个完整本地预测 Data；成功事务提交，失败、空选择或回调错误整体回滚。
    /// </summary>
    /// <param name="choice">必须来自当前回调的候选身份。</param>
    /// <param name="data">字段名集合固定的完整替换 Data。</param>
    /// <returns>暂存成功或稳定错误；不执行 Redis I/O。</returns>
    public Result Mutate(Choice choice, TData data) => _session.Mutate(choice, data);
}

/// <summary>
/// 拥有一个 Type 的本地 Registry 视图、一个常驻监听任务和至多一个临时同步任务，并同步执行事务选择策略。
/// </summary>
/// <typeparam name="TAttr">不可变 Attr 类型。</typeparam>
/// <typeparam name="TData">可更新 Data 类型。</typeparam>
public sealed unsafe class Selector<TAttr, TData> : IDisposable
    where TAttr : IFieldValue<TAttr>
    where TData : IFieldValue<TData>
{
    private readonly SelectorHandle _handle;
    private readonly SafeHandleLease<RegistrationClientHandle> _ownerLease;
    private int _disposed;

    /// <summary>
    /// 接管已经完成初始同步的 Selector 句柄和领域生命周期引用。
    /// </summary>
    /// <param name="handle">非零 Selector 句柄。</param>
    /// <param name="ownerLease">保证 Selector 先于 Registration Client 释放的引用。</param>
    private Selector(SelectorHandle handle, SafeHandleLease<RegistrationClientHandle> ownerLease)
    {
        _handle = handle;
        _ownerLease = ownerLease;
    }

    /// <summary>
    /// 创建 Type-scoped Selector 并在返回前完成订阅确认、权威分页同步和同连接栅栏。
    /// </summary>
    /// <param name="owner">仍开放的 Registration 领域。</param>
    /// <param name="type">精确 Registry Type。</param>
    /// <returns>可立即执行本地选择的 Selector，或稳定同步错误。</returns>
    internal static Result<Selector<TAttr, TData>> Create(RegistrationClient owner, string type)
    {
        ArgumentNullException.ThrowIfNull(owner);
        if (!owner.IsUsable || !SafeHandleLease<RegistrationClientHandle>.TryCreate(owner.Handle, out var lease))
        {
            return Result<Selector<TAttr, TData>>.Failure(new VerdandiError("invalid", "registration"));
        }

        var encodedType = Interop.EncodeUtf8(type, "type");
        if (!encodedType.IsSuccess)
        {
            lease.Dispose();
            return Result<Selector<TAttr, TData>>.Failure(encodedType.Error!);
        }

        using var nativeType = new NativeBufferLease(encodedType.Value);
        NativeError error = default;
        nint output = nint.Zero;
        if (NativeMethods.SelectorCreate(owner.Handle, nativeType.StringView, &output, &error) == 0)
        {
            lease.Dispose();
            return Result<Selector<TAttr, TData>>.Failure(error.ToManaged());
        }

        var handle = new SelectorHandle(output);
        if (handle.IsInvalid)
        {
            handle.Dispose();
            lease.Dispose();
            return Result<Selector<TAttr, TData>>.Failure(new VerdandiError("corrupt", "selector"));
        }

        return Result<Selector<TAttr, TData>>.Success(new Selector<TAttr, TData>(handle, lease));
    }

    /// <summary>
    /// 在调用线程同步执行策略，选择零或一个候选，并只在非空成功结果时提交本地预测。
    /// </summary>
    /// <param name="policy">只借用当前活动视图的同步策略；不得递归调用同一 Selector。</param>
    /// <returns>脱离的完整候选、成功空值，或 unavailable/contract/codec 错误。</returns>
    public Result<Candidate<TAttr, TData>?> One(OnePolicy<TAttr, TData> policy)
    {
        if (!IsUsable)
        {
            return Result<Candidate<TAttr, TData>?>.Failure(new VerdandiError("invalid", "selector"));
        }

        ArgumentNullException.ThrowIfNull(policy);
        var context = new OnePolicyContext<TAttr, TData>(policy);
        var contextHandle = GCHandle.Alloc(context);
        NativeError error = default;
        nint output = nint.Zero;
        int succeeded;
        try
        {
            succeeded = NativeMethods.SelectorOne(_handle, &SelectorPolicyTrampoline.Invoke, GCHandle.ToIntPtr(contextHandle), &output, &error);
        }
        finally
        {
            contextHandle.Free();
        }

        using var candidates = new CandidateListHandle(output);
        if (succeeded == 0)
        {
            return Result<Candidate<TAttr, TData>?>.Failure(error.ToManaged());
        }

        if (output == nint.Zero)
        {
            return Result<Candidate<TAttr, TData>?>.Success(null);
        }

        var count = NativeMethods.CandidateListSize(candidates);
        if (count != 1)
        {
            return Result<Candidate<TAttr, TData>?>.Failure(new VerdandiError("corrupt", "selection"));
        }

        var decoded = DecodeCandidate(candidates, 0);
        return decoded.IsSuccess
            ? Result<Candidate<TAttr, TData>?>.Success(decoded.Value)
            : Result<Candidate<TAttr, TData>?>.Failure(decoded.Error!);
    }

    /// <summary>
    /// 在调用线程同步执行策略，选择任意数量的唯一候选，并以一个事务提交所有本地预测。
    /// </summary>
    /// <param name="policy">返回当前回调 Choice 集合的同步策略。</param>
    /// <returns>脱离的只读完整候选集合，或稳定错误。</returns>
    public Result<IReadOnlyList<Candidate<TAttr, TData>>> Any(AnyPolicy<TAttr, TData> policy)
    {
        if (!IsUsable)
        {
            return Result<IReadOnlyList<Candidate<TAttr, TData>>>.Failure(new VerdandiError("invalid", "selector"));
        }

        ArgumentNullException.ThrowIfNull(policy);
        var context = new AnyPolicyContext<TAttr, TData>(policy);
        var contextHandle = GCHandle.Alloc(context);
        NativeError error = default;
        nint output = nint.Zero;
        int succeeded;
        try
        {
            succeeded = NativeMethods.SelectorAny(_handle, &SelectorPolicyTrampoline.Invoke, GCHandle.ToIntPtr(contextHandle), &output, &error);
        }
        finally
        {
            contextHandle.Free();
        }

        using var candidates = new CandidateListHandle(output);
        if (succeeded == 0)
        {
            return Result<IReadOnlyList<Candidate<TAttr, TData>>>.Failure(error.ToManaged());
        }

        if (output == nint.Zero)
        {
            return Result<IReadOnlyList<Candidate<TAttr, TData>>>.Success(Array.Empty<Candidate<TAttr, TData>>());
        }

        var count = NativeMethods.CandidateListSize(candidates);
        if (count > int.MaxValue)
        {
            return Result<IReadOnlyList<Candidate<TAttr, TData>>>.Failure(new VerdandiError("capacity", "selection"));
        }

        var values = new Candidate<TAttr, TData>[checked((int)count)];
        for (nuint index = 0; index < count; index++)
        {
            var decoded = DecodeCandidate(candidates, index);
            if (!decoded.IsSuccess)
            {
                return Result<IReadOnlyList<Candidate<TAttr, TData>>>.Failure(decoded.Error!);
            }

            values[checked((int)index)] = decoded.Value;
        }

        return Result<IReadOnlyList<Candidate<TAttr, TData>>>.Success(Array.AsReadOnly(values));
    }

    /// <summary>
    /// 显式复制活动和 retained 完整视图；这是 O(N) 重型本地操作，但不访问 Redis。
    /// </summary>
    /// <returns>脱离快照，或半同步 unavailable、容量或 Codec 错误。</returns>
    public Result<SelectorSnapshot<TAttr, TData>> Snapshot()
    {
        if (!IsUsable)
        {
            return Result<SelectorSnapshot<TAttr, TData>>.Failure(new VerdandiError("invalid", "selector"));
        }

        NativeError error = default;
        nint output = nint.Zero;
        if (NativeMethods.SelectorSnapshotCreate(_handle, &output, &error) == 0)
        {
            return Result<SelectorSnapshot<TAttr, TData>>.Failure(error.ToManaged());
        }

        using var snapshot = new SelectorSnapshotHandle(output);
        if (snapshot.IsInvalid)
        {
            return Result<SelectorSnapshot<TAttr, TData>>.Failure(new VerdandiError("corrupt", "snapshot"));
        }

        var activeCount = NativeMethods.SelectorSnapshotSize(snapshot, 0);
        var retainedCount = NativeMethods.SelectorSnapshotSize(snapshot, 1);
        if (activeCount > int.MaxValue || retainedCount > int.MaxValue)
        {
            return Result<SelectorSnapshot<TAttr, TData>>.Failure(new VerdandiError("capacity", "snapshot"));
        }

        var active = new Candidate<TAttr, TData>[checked((int)activeCount)];
        for (nuint index = 0; index < activeCount; index++)
        {
            var decoded = DecodeSnapshotCandidate(snapshot, false, index);
            if (!decoded.IsSuccess)
            {
                return Result<SelectorSnapshot<TAttr, TData>>.Failure(decoded.Error!);
            }

            active[checked((int)index)] = decoded.Value;
        }

        var retained = new RetainedCandidate<TAttr, TData>[checked((int)retainedCount)];
        for (nuint index = 0; index < retainedCount; index++)
        {
            var decoded = DecodeSnapshotCandidate(snapshot, true, index);
            if (!decoded.IsSuccess)
            {
                return Result<SelectorSnapshot<TAttr, TData>>.Failure(decoded.Error!);
            }

            retained[checked((int)index)] = new RetainedCandidate<TAttr, TData>(
                decoded.Value,
                NativeMethods.SelectorSnapshotRetainedUntil(snapshot, index));
        }

        return Result<SelectorSnapshot<TAttr, TData>>.Success(
            new SelectorSnapshot<TAttr, TData>(
                NativeMethods.SelectorSnapshotGeneration(snapshot),
                NativeMethods.SelectorSnapshotIsSynchronized(snapshot) != 0,
                active,
                retained));
    }

    /// <summary>
    /// 非阻塞取出一条同步、恢复或协议诊断；没有诊断是成功的 Available=false。
    /// </summary>
    /// <returns>诊断状态或调用本身的错误。</returns>
    public Result<Diagnostic> TryGetError()
    {
        if (!IsUsable)
        {
            return Result<Diagnostic>.Failure(new VerdandiError("invalid", "selector"));
        }

        NativeError error = default;
        var available = 0;
        return Interop.Diagnostic(NativeMethods.SelectorTryError(_handle, &available, &error), available, error);
    }

    /// <summary>
    /// 关闭常驻监听和当前临时同步任务，并等待原生核心汇合；重复调用幂等。
    /// </summary>
    /// <returns>关闭结果。</returns>
    public Result Close()
    {
        if (!IsUsable)
        {
            return Result.Failure(new VerdandiError("invalid", "selector"));
        }

        NativeError error = default;
        return Interop.Status(NativeMethods.SelectorClose(_handle, &error), error);
    }

    /// <summary>
    /// 最佳努力 Close，释放 Selector SafeHandle 并归还 Registration 领域生命周期引用。
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
            _ = NativeMethods.SelectorClose(_handle, &error);
        }

        _handle.Dispose();
        _ownerLease.Dispose();
        GC.SuppressFinalize(this);
    }

    /// <summary>
    /// 从脱离候选列表复制元数据和 Fields，再调用应用 Codec 构造拥有型候选。
    /// </summary>
    /// <param name="list">拥有候选列表的 SafeHandle。</param>
    /// <param name="index">候选索引。</param>
    /// <returns>完整候选或稳定错误。</returns>
    private static Result<Candidate<TAttr, TData>> DecodeCandidate(CandidateListHandle list, nuint index)
    {
        NativeRegistrationMetadata native = default;
        if (NativeMethods.CandidateListMetadata(list, index, &native) == 0)
        {
            return Result<Candidate<TAttr, TData>>.Failure(new VerdandiError("corrupt", "candidate"));
        }

        var attr = Interop.CopyCandidateListFields(list, index, true);
        if (!attr.IsSuccess)
        {
            return Result<Candidate<TAttr, TData>>.Failure(attr.Error!);
        }

        var data = Interop.CopyCandidateListFields(list, index, false);
        return data.IsSuccess
            ? CandidateDecoder<TAttr, TData>.Decode(native, attr.Value, data.Value)
            : Result<Candidate<TAttr, TData>>.Failure(data.Error!);
    }

    /// <summary>
    /// 从活动或 retained 快照复制并解码一个完整候选。
    /// </summary>
    /// <param name="snapshot">拥有快照的 SafeHandle。</param>
    /// <param name="retained">是否读取 retained 视图。</param>
    /// <param name="index">候选索引。</param>
    /// <returns>完整候选或稳定错误。</returns>
    private static Result<Candidate<TAttr, TData>> DecodeSnapshotCandidate(SelectorSnapshotHandle snapshot, bool retained, nuint index)
    {
        NativeRegistrationMetadata native = default;
        if (NativeMethods.SelectorSnapshotMetadata(snapshot, retained ? 1 : 0, index, &native) == 0)
        {
            return Result<Candidate<TAttr, TData>>.Failure(new VerdandiError("corrupt", "candidate"));
        }

        var attr = Interop.CopySnapshotFields(snapshot, retained, index, true);
        if (!attr.IsSuccess)
        {
            return Result<Candidate<TAttr, TData>>.Failure(attr.Error!);
        }

        var data = Interop.CopySnapshotFields(snapshot, retained, index, false);
        return data.IsSuccess
            ? CandidateDecoder<TAttr, TData>.Decode(native, attr.Value, data.Value)
            : Result<Candidate<TAttr, TData>>.Failure(data.Error!);
    }

    /// <summary>返回当前包装是否仍可传给 Selector 原生操作。</summary>
    private bool IsUsable => Volatile.Read(ref _disposed) == 0 && !_handle.IsClosed && !_handle.IsInvalid;
}

/// <summary>
/// 拥有一次策略回调的借用原生候选、事务身份和惰性解码缓存；回调结束后所有操作返回 invalid。
/// </summary>
internal sealed unsafe class CandidateSession<TAttr, TData>
    where TAttr : IFieldValue<TAttr>
    where TData : IFieldValue<TData>
{
    private readonly nint _candidates;
    private Dictionary<nuint, CandidateView<TAttr, TData>>? _cache;
    private readonly long _transaction;
    private bool _active = true;

    /// <summary>
    /// 建立当前同步回调会话并读取一次候选数量；不复制候选内容。
    /// </summary>
    /// <param name="candidates">只在当前原生策略回调内有效的集合地址。</param>
    internal CandidateSession(nint candidates)
    {
        _candidates = candidates;
        _transaction = SelectorTransaction.Next();
        Count = NativeMethods.CandidatesSize(candidates);
    }

    /// <summary>返回回调开始时的活动候选数量。</summary>
    internal nuint Count { get; }

    /// <summary>
    /// 惰性读取、复制和解码候选，并在当前回调内按索引缓存。
    /// </summary>
    /// <param name="index">候选索引。</param>
    /// <returns>当前事务候选视图或稳定错误。</returns>
    internal Result<CandidateView<TAttr, TData>> Get(nuint index)
    {
        if (!_active || index >= Count)
        {
            return Result<CandidateView<TAttr, TData>>.Failure(new VerdandiError("invalid", "candidate"));
        }

        if (_cache?.TryGetValue(index, out var cached) == true)
        {
            return Result<CandidateView<TAttr, TData>>.Success(cached);
        }

        NativeRegistrationMetadata native = default;
        if (NativeMethods.CandidatesMetadata(_candidates, index, &native) == 0)
        {
            return Result<CandidateView<TAttr, TData>>.Failure(new VerdandiError("corrupt", "candidate"));
        }

        var attrFields = Interop.CopyCandidateFields(_candidates, index, true);
        if (!attrFields.IsSuccess)
        {
            return Result<CandidateView<TAttr, TData>>.Failure(attrFields.Error!);
        }

        var dataFields = Interop.CopyCandidateFields(_candidates, index, false);
        if (!dataFields.IsSuccess)
        {
            return Result<CandidateView<TAttr, TData>>.Failure(dataFields.Error!);
        }

        var decoded = CandidateDecoder<TAttr, TData>.Decode(native, attrFields.Value, dataFields.Value);
        if (!decoded.IsSuccess)
        {
            return Result<CandidateView<TAttr, TData>>.Failure(decoded.Error!);
        }

        var value = new CandidateView<TAttr, TData>(
            new Choice(index, _transaction),
            decoded.Value.Metadata,
            decoded.Value.Attr,
            decoded.Value.Data);
        (_cache ??= []).Add(index, value);
        return Result<CandidateView<TAttr, TData>>.Success(value);
    }

    /// <summary>
    /// 验证 Choice 属于当前活动事务，编码完整 Data 并调用原生本地事务暂存。
    /// </summary>
    /// <param name="choice">当前事务候选身份。</param>
    /// <param name="data">完整预测 Data。</param>
    /// <returns>暂存结果。</returns>
    internal Result Mutate(Choice choice, TData data)
    {
        var valid = Validate(choice);
        if (!valid.IsSuccess)
        {
            return valid;
        }

        var encoded = Interop.EncodeValue(data);
        if (!encoded.IsSuccess)
        {
            return Result.Failure(encoded.Error!);
        }

        using var native = new NativeFieldsLease(encoded.Value);
        NativeError error = default;
        var result = Interop.Status(NativeMethods.CandidatesMutate(_candidates, choice.Index, native.View, &error), error);
        if (result.IsSuccess && _cache?.TryGetValue(choice.Index, out var current) == true)
        {
            _cache[choice.Index] = current with { Data = data };
        }

        return result;
    }

    /// <summary>
    /// 验证 Choice 的回调身份和索引范围，不访问原生内存。
    /// </summary>
    /// <param name="choice">待验证身份。</param>
    /// <returns>有效时成功，否则 contract。</returns>
    internal Result Validate(Choice choice) =>
        _active && choice.Transaction == _transaction && choice.Index < Count
            ? Result.Success()
            : Result.Failure(new VerdandiError("contract", "choice"));

    /// <summary>
    /// 在应用回调离开前使会话失效，防止保留的托管内部引用继续触碰借用原生视图。
    /// </summary>
    internal void Invalidate()
    {
        _active = false;
    }
}

/// <summary>
/// 为所有 Attr/Data 泛型组合分配进程内唯一的 Selector 策略事务身份，防止跨类型 Choice 偶然碰撞。
/// </summary>
internal static class SelectorTransaction
{
    private static long _next;

    /// <summary>
    /// 原子分配下一个非零事务身份；实际进程不可能在一个生命周期内耗尽 Int64 空间。
    /// </summary>
    /// <returns>进程内新事务身份。</returns>
    internal static long Next() => Interlocked.Increment(ref _next);
}

/// <summary>
/// 统一把原生候选元数据和完整 Fields 解码为拥有型强类型 Candidate。
/// </summary>
internal static class CandidateDecoder<TAttr, TData>
    where TAttr : IFieldValue<TAttr>
    where TData : IFieldValue<TData>
{
    /// <summary>
    /// 立即复制 UUID 并调用应用 Attr/Data Codec；任何失败都不会留下部分候选。
    /// </summary>
    /// <param name="native">借用 UUID 的原生元数据。</param>
    /// <param name="attrFields">完整拥有型 Attr Fields。</param>
    /// <param name="dataFields">完整拥有型 Data Fields。</param>
    /// <returns>完整 Candidate 或稳定错误。</returns>
    internal static Result<Candidate<TAttr, TData>> Decode(NativeRegistrationMetadata native, Fields attrFields, Fields dataFields)
    {
        var uuid = Interop.CopyString(native.Uuid, "uuid");
        if (!uuid.IsSuccess)
        {
            return Result<Candidate<TAttr, TData>>.Failure(uuid.Error!);
        }

        var attr = Interop.DecodeValue<TAttr>(attrFields);
        if (!attr.IsSuccess)
        {
            return Result<Candidate<TAttr, TData>>.Failure(attr.Error!);
        }

        var data = Interop.DecodeValue<TData>(dataFields);
        if (!data.IsSuccess)
        {
            return Result<Candidate<TAttr, TData>>.Failure(data.Error!);
        }

        var metadata = new RegistrationMetadata(uuid.Value, native.Revision, native.Timestamp, native.TtlMilliseconds, native.Version);
        return Result<Candidate<TAttr, TData>>.Success(new Candidate<TAttr, TData>(metadata, attr.Value, data.Value));
    }
}

/// <summary>
/// 抽象一次具体泛型策略上下文，使唯一非泛型 unmanaged 入口可以恢复并调用它。
/// </summary>
internal unsafe interface ISelectorPolicyContext
{
    /// <summary>
    /// 在原生事务借用范围内执行应用策略并填写 Selection 或错误。
    /// </summary>
    /// <param name="candidates">借用候选集合。</param>
    /// <param name="selection">借用选择结果构造器。</param>
    /// <param name="error">失败时必须填写的固定错误缓冲区。</param>
    /// <returns>一提交，零回滚。</returns>
    int Invoke(nint candidates, nint selection, NativeError* error);
}

/// <summary>
/// 保存 One 策略委托，并把其 Result 转换为一个原生事务选择。
/// </summary>
internal sealed unsafe class OnePolicyContext<TAttr, TData> : ISelectorPolicyContext
    where TAttr : IFieldValue<TAttr>
    where TData : IFieldValue<TData>
{
    private readonly OnePolicy<TAttr, TData> _policy;

    /// <summary>
    /// 保存本次同步调用的应用委托；上下文不会在调用返回后继续使用。
    /// </summary>
    /// <param name="policy">One 策略。</param>
    internal OnePolicyContext(OnePolicy<TAttr, TData> policy)
    {
        _policy = policy;
    }

    /// <summary>
    /// 执行 One 策略，验证 Choice 并至多添加一个索引；异常被转换为稳定 callback 错误。
    /// </summary>
    /// <param name="candidates">借用候选集合。</param>
    /// <param name="selection">借用结果构造器。</param>
    /// <param name="error">固定错误输出。</param>
    /// <returns>一提交，零回滚。</returns>
    public int Invoke(nint candidates, nint selection, NativeError* error)
    {
        var session = new CandidateSession<TAttr, TData>(candidates);
        try
        {
            var selected = _policy(new Candidates<TAttr, TData>(session));
            if (!selected.IsSuccess)
            {
                WriteError(error, selected.Error!);
                return 0;
            }

            if (selected.Value is not Choice choice)
            {
                return 1;
            }

            var valid = session.Validate(choice);
            if (!valid.IsSuccess)
            {
                WriteError(error, valid.Error!);
                return 0;
            }

            return NativeMethods.SelectionAdd(selection, choice.Index, error);
        }
        catch (OutOfMemoryException exception)
        {
            WriteError(error, new VerdandiError("capacity", "callback", exception.Message));
            return 0;
        }
        catch (Exception exception)
        {
            WriteError(error, new VerdandiError("unavailable", "callback", exception.Message));
            return 0;
        }
        finally
        {
            session.Invalidate();
        }
    }

    /// <summary>
    /// 在原生缓冲区存在时写入托管错误；空缓冲区只能让核心回退为 contract。
    /// </summary>
    /// <param name="output">原生错误输出。</param>
    /// <param name="error">托管稳定错误。</param>
    private static void WriteError(NativeError* output, VerdandiError error)
    {
        if (output is not null)
        {
            output->Write(error);
        }
    }
}

/// <summary>
/// 保存 Any 策略委托，并把其唯一 Choice 集合转换为原生事务选择。
/// </summary>
internal sealed unsafe class AnyPolicyContext<TAttr, TData> : ISelectorPolicyContext
    where TAttr : IFieldValue<TAttr>
    where TData : IFieldValue<TData>
{
    private readonly AnyPolicy<TAttr, TData> _policy;

    /// <summary>
    /// 保存本次同步调用的应用委托。
    /// </summary>
    /// <param name="policy">Any 策略。</param>
    internal AnyPolicyContext(AnyPolicy<TAttr, TData> policy)
    {
        _policy = policy;
    }

    /// <summary>
    /// 执行 Any 策略并依次验证、去重和添加 Choice；任一失败会让原生事务整体回滚。
    /// </summary>
    /// <param name="candidates">借用候选集合。</param>
    /// <param name="selection">借用结果构造器。</param>
    /// <param name="error">固定错误输出。</param>
    /// <returns>一提交，零回滚。</returns>
    public int Invoke(nint candidates, nint selection, NativeError* error)
    {
        var session = new CandidateSession<TAttr, TData>(candidates);
        try
        {
            var selected = _policy(new Candidates<TAttr, TData>(session));
            if (!selected.IsSuccess)
            {
                WriteError(error, selected.Error!);
                return 0;
            }

            var choices = selected.Value;
            if (choices is null)
            {
                WriteError(error, new VerdandiError("contract", "selection"));
                return 0;
            }

            HashSet<Choice>? unique = choices.Count > 1 ? [] : null;
            foreach (var choice in choices)
            {
                var valid = session.Validate(choice);
                if (!valid.IsSuccess || (unique is not null && !unique.Add(choice)))
                {
                    WriteError(error, valid.IsSuccess ? new VerdandiError("contract", "selection") : valid.Error!);
                    return 0;
                }

                if (NativeMethods.SelectionAdd(selection, choice.Index, error) == 0)
                {
                    return 0;
                }
            }

            return 1;
        }
        catch (OutOfMemoryException exception)
        {
            WriteError(error, new VerdandiError("capacity", "callback", exception.Message));
            return 0;
        }
        catch (Exception exception)
        {
            WriteError(error, new VerdandiError("unavailable", "callback", exception.Message));
            return 0;
        }
        finally
        {
            session.Invalidate();
        }
    }

    /// <summary>
    /// 在原生缓冲区存在时写入托管错误。
    /// </summary>
    /// <param name="output">原生错误输出。</param>
    /// <param name="error">托管稳定错误。</param>
    private static void WriteError(NativeError* output, VerdandiError error)
    {
        if (output is not null)
        {
            output->Write(error);
        }
    }
}

/// <summary>
/// 提供唯一非泛型 Cdecl Selector 策略入口，借助 GCHandle 恢复当前具体泛型上下文。
/// </summary>
internal static unsafe class SelectorPolicyTrampoline
{
    /// <summary>
    /// 验证上下文并调用托管策略；任何异常都在离开 managed 边界前转换为固定错误。
    /// </summary>
    /// <param name="context">指向 ISelectorPolicyContext 的 GCHandle。</param>
    /// <param name="candidates">借用候选集合。</param>
    /// <param name="selection">借用结果构造器。</param>
    /// <param name="error">固定错误输出。</param>
    /// <returns>一提交，零回滚。</returns>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    internal static int Invoke(nint context, nint candidates, nint selection, NativeError* error)
    {
        try
        {
            if (GCHandle.FromIntPtr(context).Target is not ISelectorPolicyContext policy)
            {
                if (error is not null)
                {
                    error->Write(new VerdandiError("contract", "callback"));
                }

                return 0;
            }

            return policy.Invoke(candidates, selection, error);
        }
        catch (Exception exception)
        {
            if (error is not null)
            {
                error->Write(new VerdandiError("unavailable", "callback", exception.Message));
            }

            return 0;
        }
    }
}
