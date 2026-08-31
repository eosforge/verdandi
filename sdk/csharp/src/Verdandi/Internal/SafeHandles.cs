using Microsoft.Win32.SafeHandles;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.InteropServices;

namespace Verdandi.Internal;

/// <summary>
/// 为根 Client 提供唯一原生释放所有权。
/// </summary>
internal sealed class ClientHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>构造供互操作框架使用的空句柄。</summary>
    private ClientHandle()
        : base(true)
    {
    }

    /// <summary>
    /// 接管 C ABI 返回的根 Client 地址。
    /// </summary>
    /// <param name="value">非零原生地址。</param>
    internal ClientHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>
    /// 尽力关闭并释放根 Client；SafeHandle 保证每个地址最多调用一次。
    /// </summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.ClientRelease(handle);
        return true;
    }
}

/// <summary>为二进制 Blob 提供唯一原生释放所有权。</summary>
internal sealed class BlobHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的 Blob 地址。</summary>
    /// <param name="value">允许为零的原生地址。</param>
    internal BlobHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>释放 Blob。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.BlobRelease(handle);
        return true;
    }
}

/// <summary>为拥有型字段集合提供唯一原生释放所有权。</summary>
internal sealed class FieldSetHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的字段集合地址。</summary>
    /// <param name="value">允许为零的原生地址。</param>
    internal FieldSetHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>释放字段集合。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.FieldSetRelease(handle);
        return true;
    }
}

/// <summary>为 Registration Client 提供唯一原生释放所有权。</summary>
internal sealed class RegistrationClientHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的 Registration Client 地址。</summary>
    /// <param name="value">非零原生地址。</param>
    internal RegistrationClientHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>尽力关闭并释放 Registration Client。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.RegistrationClientRelease(handle);
        return true;
    }
}

/// <summary>为单条 Registration 提供唯一原生释放所有权。</summary>
internal sealed class RegistrationHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的 Registration 地址。</summary>
    /// <param name="value">非零原生地址。</param>
    internal RegistrationHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>尽力注销并释放 Registration。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.RegistrationRelease(handle);
        return true;
    }
}

/// <summary>为 Selector 提供唯一原生释放所有权。</summary>
internal sealed class SelectorHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的 Selector 地址。</summary>
    /// <param name="value">非零原生地址。</param>
    internal SelectorHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>尽力关闭并释放 Selector。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.SelectorRelease(handle);
        return true;
    }
}

/// <summary>为一次脱离候选列表提供唯一原生释放所有权。</summary>
internal sealed class CandidateListHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的候选列表地址。</summary>
    /// <param name="value">允许为零的原生地址。</param>
    internal CandidateListHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>释放脱离候选列表。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.CandidateListRelease(handle);
        return true;
    }
}

/// <summary>为一次 Selector 重型快照提供唯一原生释放所有权。</summary>
internal sealed class SelectorSnapshotHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的 Selector 快照地址。</summary>
    /// <param name="value">非零原生地址。</param>
    internal SelectorSnapshotHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>释放 Selector 快照。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.SelectorSnapshotRelease(handle);
        return true;
    }
}

/// <summary>为 Catalog Client 提供唯一原生释放所有权。</summary>
internal sealed class CatalogClientHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的 Catalog Client 地址。</summary>
    /// <param name="value">非零原生地址。</param>
    internal CatalogClientHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>尽力关闭并释放 Catalog Client。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.CatalogClientRelease(handle);
        return true;
    }
}

/// <summary>为无任务 Catalog Publisher 提供唯一原生释放所有权。</summary>
internal sealed class CatalogPublisherHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的 Catalog Publisher 地址。</summary>
    /// <param name="value">非零原生地址。</param>
    internal CatalogPublisherHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>释放 Catalog Publisher。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.CatalogPublisherRelease(handle);
        return true;
    }
}

/// <summary>为 Catalog Subscriber 提供唯一原生释放所有权。</summary>
internal sealed class CatalogSubscriberHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的 Catalog Subscriber 地址。</summary>
    /// <param name="value">非零原生地址。</param>
    internal CatalogSubscriberHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>尽力关闭并释放 Catalog Subscriber。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.CatalogSubscriberRelease(handle);
        return true;
    }
}

/// <summary>为稳定 Catalog Entry 提供唯一原生释放所有权。</summary>
internal sealed class CatalogEntryHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>接管 C ABI 返回的 Catalog Entry 地址。</summary>
    /// <param name="value">非零原生地址。</param>
    internal CatalogEntryHandle(nint value)
        : base(true)
    {
        SetHandle(value);
    }

    /// <summary>释放 Catalog Entry。</summary>
    /// <returns>释放入口没有可报告返回值，因此恒为真。</returns>
    protected override bool ReleaseHandle()
    {
        NativeMethods.CatalogEntryRelease(handle);
        return true;
    }
}

/// <summary>
/// 使用 SafeHandle 的内部引用计数延长父句柄原生释放时间，保证 C ABI 要求的子先父后销毁顺序。
/// </summary>
/// <typeparam name="THandle">需要延长的具体 SafeHandle。</typeparam>
internal sealed class SafeHandleLease<THandle> : IDisposable
    where THandle : SafeHandle
{
    private THandle? _handle;

    /// <summary>
    /// 在构造时持有一次 DangerousAddRef；调用方必须只传入当前可用的句柄。
    /// </summary>
    /// <param name="handle">父原生句柄。</param>
    private SafeHandleLease(THandle handle)
    {
        var added = false;
        try
        {
            handle.DangerousAddRef(ref added);
            _handle = handle;
        }
        catch
        {
            if (added)
            {
                handle.DangerousRelease();
            }

            throw;
        }
    }

    /// <summary>
    /// 作为显式 Dispose 遗漏时的最后释放兜底；它只归还引用，不执行结果可见的 Close。
    /// </summary>
    ~SafeHandleLease()
    {
        Release();
    }

    /// <summary>
    /// 尝试为尚未关闭的父句柄取得一个生命周期引用；失败不会改变句柄。
    /// </summary>
    /// <param name="handle">要延长的父句柄。</param>
    /// <param name="lease">成功时的新引用所有者。</param>
    /// <returns>是否成功取得引用。</returns>
    internal static bool TryCreate(THandle handle, [NotNullWhen(true)] out SafeHandleLease<THandle>? lease)
    {
        try
        {
            lease = new SafeHandleLease<THandle>(handle);
            return true;
        }
        catch (ObjectDisposedException)
        {
            lease = null;
            return false;
        }
    }

    /// <summary>
    /// 归还父 SafeHandle 引用；重复调用安全。
    /// </summary>
    public void Dispose()
    {
        Release();
        GC.SuppressFinalize(this);
    }

    /// <summary>
    /// 原子取走当前父句柄并归还一次 DangerousAddRef，避免 Dispose 与终结器重复释放。
    /// </summary>
    private void Release()
    {
        var handle = Interlocked.Exchange(ref _handle, default);
        handle?.DangerousRelease();
    }
}
