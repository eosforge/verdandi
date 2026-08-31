using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Verdandi.Internal;

/// <summary>
/// 声明 C ABI v1 的源生成互操作入口；所有公开包装必须先建立托管所有权和错误转换。
/// </summary>
internal static unsafe partial class NativeMethods
{
    internal const string LibraryName = "verdandi_cpp";

    /// <summary>读取原生运行库实现的 C ABI 版本。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_c_abi_version")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial uint AbiVersion();

    /// <summary>从严格 JSON 打开根 Client，并通过输出地址转移一个句柄。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_client_open_json")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int ClientOpenJson(NativeBytesView json, nint* output, NativeError* error);

    /// <summary>读取根 Client 是否仍接纳工作。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_client_is_open")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int ClientIsOpen(ClientHandle value);

    /// <summary>执行根 Client PING。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_client_ping")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int ClientPing(ClientHandle value, NativeError* error);

    /// <summary>显式关闭根 Client。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_client_close")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int ClientClose(ClientHandle value, NativeError* error);

    /// <summary>释放根 Client 原生句柄；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_client_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void ClientRelease(nint value);

    /// <summary>读取二进制 Key。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_key_load")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int KeyLoad(ClientHandle client, NativeStringView key, int* found, nint* output, NativeError* error);

    /// <summary>无 TTL 覆盖写入二进制 Key。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_key_store")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int KeyStore(ClientHandle client, NativeStringView key, NativeBytesView value, NativeError* error);

    /// <summary>以精确毫秒 TTL 覆盖写入二进制 Key。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_key_store_ttl")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int KeyStoreTtl(ClientHandle client, NativeStringView key, NativeBytesView value, ulong ttlMilliseconds, NativeError* error);

    /// <summary>删除完整 Key。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_key_erase")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int KeyErase(ClientHandle client, NativeStringView key, int* removed, NativeError* error);

    /// <summary>判断 Key 是否存在。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_key_contains")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int KeyContains(ClientHandle client, NativeStringView key, int* present, NativeError* error);

    /// <summary>为现存 Key 设置精确毫秒 TTL。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_key_expire")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int KeyExpire(ClientHandle client, NativeStringView key, ulong ttlMilliseconds, int* changed, NativeError* error);

    /// <summary>读取完整 Redis Hash。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_hash_load")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int HashLoad(ClientHandle client, NativeStringView key, nint* output, NativeError* error);

    /// <summary>用一次 HSET 写入字段集合。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_hash_store")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int HashStore(ClientHandle client, NativeStringView key, NativeFieldsView value, NativeError* error);

    /// <summary>删除多个 Hash 字段。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_hash_erase")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int HashErase(ClientHandle client, NativeStringView key, NativeStringView* names, nuint count, nuint* removed, NativeError* error);

    /// <summary>判断 Hash 字段是否存在。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_hash_contains")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int HashContains(ClientHandle client, NativeStringView key, NativeStringView name, int* present, NativeError* error);

    /// <summary>读取 Hash 字段数量。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_hash_size")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int HashSize(ClientHandle client, NativeStringView key, nuint* size, NativeError* error);

    /// <summary>读取 Blob 借用字节。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_blob_view")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial NativeBytesView BlobView(BlobHandle value);

    /// <summary>释放 Blob；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_blob_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void BlobRelease(nint value);

    /// <summary>读取拥有型字段集合数量。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_field_set_size")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial nuint FieldSetSize(FieldSetHandle value);

    /// <summary>按顺序读取拥有型字段集合中的一个借用字段。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_field_set_at")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int FieldSetAt(FieldSetHandle value, nuint index, NativeFieldView* output);

    /// <summary>释放拥有型字段集合；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_field_set_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void FieldSetRelease(nint value);

    /// <summary>打开 Registration 子域。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_client_open")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationClientOpen(ClientHandle root, nint* output, NativeError* error);

    /// <summary>读取 Registration Client 是否开放。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_client_is_open")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationClientIsOpen(RegistrationClientHandle value);

    /// <summary>关闭 Registration 子域。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_client_close")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationClientClose(RegistrationClientHandle value, NativeError* error);

    /// <summary>释放 Registration Client；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_client_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void RegistrationClientRelease(nint value);

    /// <summary>本地构造未发布 Registration。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationCreate(RegistrationClientHandle client, NativeRegistrationOptions* options, nint* output, NativeError* error);

    /// <summary>借用 Registration UUID。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_uuid")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial NativeStringView RegistrationUuid(RegistrationHandle value);

    /// <summary>读取 Registration 是否完成首次发布。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_is_published")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationIsPublished(RegistrationHandle value);

    /// <summary>读取 Registration 当前期望 revision。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_revision")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial ulong RegistrationRevision(RegistrationHandle value);

    /// <summary>读取 Registration 最近确认时间戳。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_timestamp")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial ulong RegistrationTimestamp(RegistrationHandle value);

    /// <summary>发布完整 Attr 与 Data。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_publish")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationPublish(RegistrationHandle value, NativeFieldsView attr, NativeFieldsView data, NativeError* error);

    /// <summary>提交完整期望 Data。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_update")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationUpdate(RegistrationHandle value, NativeFieldsView data, NativeError* error);

    /// <summary>只修改应用 Version。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_set_version")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationSetVersion(RegistrationHandle value, ulong version, NativeError* error);

    /// <summary>用一个 revision 同时修改 Version 和 Data。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_update_content")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationUpdateContent(RegistrationHandle value, ulong version, NativeFieldsView data, NativeError* error);

    /// <summary>显式续期 Registration。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_renew")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationRenew(RegistrationHandle value, NativeError* error);

    /// <summary>终止并删除 Registration。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_close")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationClose(RegistrationHandle value, NativeError* error);

    /// <summary>非阻塞读取 Registration 异步诊断。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_try_error")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int RegistrationTryError(RegistrationHandle value, int* available, NativeError* error);

    /// <summary>释放 Registration；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_registration_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void RegistrationRelease(nint value);

    /// <summary>创建并完成 Selector 初始同步。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectorCreate(RegistrationClientHandle client, NativeStringView type, nint* output, NativeError* error);

    /// <summary>读取借用候选数量。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_candidates_size")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial nuint CandidatesSize(nint value);

    /// <summary>读取一个借用候选的元数据。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_candidates_metadata")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CandidatesMetadata(nint value, nuint index, NativeRegistrationMetadata* output);

    /// <summary>遍历一个借用候选的 Attr。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_candidates_visit_attr")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CandidatesVisitAttr(
        nint value,
        nuint index,
        delegate* unmanaged[Cdecl]<nint, NativeStringView, NativeBytesView, int> visitor,
        nint context,
        NativeError* error);

    /// <summary>遍历一个借用候选当前预测优先的 Data。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_candidates_visit_data")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CandidatesVisitData(
        nint value,
        nuint index,
        delegate* unmanaged[Cdecl]<nint, NativeStringView, NativeBytesView, int> visitor,
        nint context,
        NativeError* error);

    /// <summary>在当前 Selector 事务内暂存完整 Data。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_candidates_mutate")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CandidatesMutate(nint value, nuint index, NativeFieldsView data, NativeError* error);

    /// <summary>把一个索引加入当前事务选择结果。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selection_add")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectionAdd(nint value, nuint index, NativeError* error);

    /// <summary>同步执行 One 策略。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_one")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectorOne(
        SelectorHandle value,
        delegate* unmanaged[Cdecl]<nint, nint, nint, NativeError*, int> policy,
        nint context,
        nint* output,
        NativeError* error);

    /// <summary>同步执行 Any 策略。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_any")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectorAny(
        SelectorHandle value,
        delegate* unmanaged[Cdecl]<nint, nint, nint, NativeError*, int> policy,
        nint context,
        nint* output,
        NativeError* error);

    /// <summary>读取脱离候选列表数量。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_candidate_list_size")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial nuint CandidateListSize(CandidateListHandle value);

    /// <summary>读取脱离候选元数据。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_candidate_list_metadata")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CandidateListMetadata(CandidateListHandle value, nuint index, NativeRegistrationMetadata* output);

    /// <summary>遍历脱离候选 Attr。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_candidate_list_visit_attr")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CandidateListVisitAttr(
        CandidateListHandle value,
        nuint index,
        delegate* unmanaged[Cdecl]<nint, NativeStringView, NativeBytesView, int> visitor,
        nint context,
        NativeError* error);

    /// <summary>遍历脱离候选 Data。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_candidate_list_visit_data")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CandidateListVisitData(
        CandidateListHandle value,
        nuint index,
        delegate* unmanaged[Cdecl]<nint, NativeStringView, NativeBytesView, int> visitor,
        nint context,
        NativeError* error);

    /// <summary>释放脱离候选列表；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_candidate_list_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void CandidateListRelease(nint value);

    /// <summary>创建完整脱离 Selector 快照。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_snapshot_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectorSnapshotCreate(SelectorHandle value, nint* output, NativeError* error);

    /// <summary>读取 Selector 快照 generation。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_snapshot_generation")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial ulong SelectorSnapshotGeneration(SelectorSnapshotHandle value);

    /// <summary>读取 Selector 快照同步状态。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_snapshot_is_synchronized")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectorSnapshotIsSynchronized(SelectorSnapshotHandle value);

    /// <summary>读取活动或 retained 快照数量。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_snapshot_size")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial nuint SelectorSnapshotSize(SelectorSnapshotHandle value, int retained);

    /// <summary>读取活动或 retained 快照元数据。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_snapshot_metadata")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectorSnapshotMetadata(
        SelectorSnapshotHandle value,
        int retained,
        nuint index,
        NativeRegistrationMetadata* output);

    /// <summary>读取 retained 候选截止 Redis 毫秒。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_snapshot_retained_until")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial ulong SelectorSnapshotRetainedUntil(SelectorSnapshotHandle value, nuint index);

    /// <summary>遍历活动或 retained 快照 Attr。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_snapshot_visit_attr")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectorSnapshotVisitAttr(
        SelectorSnapshotHandle value,
        int retained,
        nuint index,
        delegate* unmanaged[Cdecl]<nint, NativeStringView, NativeBytesView, int> visitor,
        nint context,
        NativeError* error);

    /// <summary>遍历活动或 retained 快照 Data。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_snapshot_visit_data")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectorSnapshotVisitData(
        SelectorSnapshotHandle value,
        int retained,
        nuint index,
        delegate* unmanaged[Cdecl]<nint, NativeStringView, NativeBytesView, int> visitor,
        nint context,
        NativeError* error);

    /// <summary>释放 Selector 快照；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_snapshot_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void SelectorSnapshotRelease(nint value);

    /// <summary>非阻塞读取 Selector 异步诊断。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_try_error")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectorTryError(SelectorHandle value, int* available, NativeError* error);

    /// <summary>关闭 Selector。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_close")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SelectorClose(SelectorHandle value, NativeError* error);

    /// <summary>释放 Selector；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_selector_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void SelectorRelease(nint value);

    /// <summary>打开 Catalog 子域。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_client_open")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogClientOpen(ClientHandle root, nint* output, NativeError* error);

    /// <summary>读取 Catalog Client 是否开放。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_client_is_open")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogClientIsOpen(CatalogClientHandle value);

    /// <summary>关闭 Catalog 子域。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_client_close")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogClientClose(CatalogClientHandle value, NativeError* error);

    /// <summary>释放 Catalog Client；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_client_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void CatalogClientRelease(nint value);

    /// <summary>创建无任务 Catalog Publisher。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_publisher_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogPublisherCreate(CatalogClientHandle client, nint* output, NativeError* error);

    /// <summary>释放 Catalog Publisher；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_publisher_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void CatalogPublisherRelease(nint value);

    /// <summary>原子覆盖完整 Catalog 记录。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_replace")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogReplace(
        CatalogPublisherHandle publisher,
        NativeCatalogPathView path,
        NativeStringView kind,
        NativeFieldsView fields,
        ulong* revision,
        NativeError* error);

    /// <summary>按精确 base revision 原子 Patch Catalog 字段。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_patch")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogPatch(
        CatalogPublisherHandle publisher,
        NativeCatalogPathView path,
        ulong baseRevision,
        NativeFieldsView fields,
        ulong* revision,
        NativeError* error);

    /// <summary>原子删除完整 Catalog Path。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_erase")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogErase(CatalogPublisherHandle publisher, NativeCatalogPathView path, ulong* revision, NativeError* error);

    /// <summary>创建并完成 Catalog Subscriber 初始同步。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_subscriber_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogSubscriberCreate(
        CatalogClientHandle client,
        NativeCatalogSubscription* subscription,
        nint* output,
        NativeError* error);

    /// <summary>在订阅范围内查找稳定 Entry。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_subscriber_find")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogSubscriberFind(
        CatalogSubscriberHandle subscriber,
        NativeCatalogPathView path,
        int* found,
        nint* output,
        NativeError* error);

    /// <summary>非阻塞读取 Catalog Subscriber 异步诊断。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_subscriber_try_error")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogSubscriberTryError(CatalogSubscriberHandle value, int* available, NativeError* error);

    /// <summary>关闭 Catalog Subscriber。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_subscriber_close")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogSubscriberClose(CatalogSubscriberHandle value, NativeError* error);

    /// <summary>释放 Catalog Subscriber；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_subscriber_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void CatalogSubscriberRelease(nint value);

    /// <summary>借用 Catalog Entry Part。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_entry_part")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial NativeStringView CatalogEntryPart(CatalogEntryHandle value);

    /// <summary>借用 Catalog Entry ID。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_entry_id")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial NativeStringView CatalogEntryId(CatalogEntryHandle value);

    /// <summary>借用 Catalog Entry 稳定状态字符串。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_entry_status")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial nint CatalogEntryStatus(CatalogEntryHandle value);

    /// <summary>读取 Catalog Entry revision。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_entry_revision")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial ulong CatalogEntryRevision(CatalogEntryHandle value);

    /// <summary>读取 Catalog Entry 同步状态。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_entry_is_synchronized")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogEntryIsSynchronized(CatalogEntryHandle value);

    /// <summary>从同一个不可变 Catalog Entry 状态加载完整 Fields。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_entry_load")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int CatalogEntryLoad(
        CatalogEntryHandle value,
        ulong* revision,
        nint* status,
        int* synchronized,
        int* present,
        nint* output,
        NativeError* error);

    /// <summary>释放 Catalog Entry；仅由 SafeHandle 调用。</summary>
    [LibraryImport(LibraryName, EntryPoint = "verdandi_catalog_entry_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void CatalogEntryRelease(nint value);
}

/// <summary>
/// 安装跨平台原生库解析并在第一次打开 Client 前验证 C ABI 版本。
/// </summary>
internal static class NativeRuntime
{
    private const uint RequiredAbiVersion = 1;
    private static readonly object Gate = new();
    private static bool _resolverInstalled;

    /// <summary>
    /// 确保当前进程只安装一次本程序集的解析器，并把加载、位数或 ABI 错误转换为稳定失败。
    /// </summary>
    /// <returns>ABI v1 可调用时成功，否则返回 unavailable/incompatible。</returns>
    internal static Result CheckAbi()
    {
        try
        {
            EnsureResolver();
            var actual = NativeMethods.AbiVersion();
            return actual == RequiredAbiVersion
                ? Result.Success()
                : Result.Failure(new VerdandiError("incompatible", "abi", $"Expected ABI {RequiredAbiVersion}, received {actual}."));
        }
        catch (EntryPointNotFoundException exception)
        {
            return Result.Failure(new VerdandiError("incompatible", "abi", exception.Message));
        }
        catch (BadImageFormatException exception)
        {
            return Result.Failure(new VerdandiError("incompatible", "native_library", exception.Message));
        }
        catch (DllNotFoundException exception)
        {
            return Result.Failure(new VerdandiError("unavailable", "native_library", exception.Message));
        }
        catch (Exception exception)
        {
            return Result.Failure(new VerdandiError("unavailable", "native_library", exception.Message));
        }
    }

    /// <summary>
    /// 在锁内安装程序集级解析器；后续 P/Invoke 走运行库已经缓存的原生模块。
    /// </summary>
    private static void EnsureResolver()
    {
        if (Volatile.Read(ref _resolverInstalled))
        {
            return;
        }

        lock (Gate)
        {
            if (Volatile.Read(ref _resolverInstalled))
            {
                return;
            }

            NativeLibrary.SetDllImportResolver(typeof(NativeMethods).Assembly, Resolve);
            Volatile.Write(ref _resolverInstalled, true);
        }
    }

    /// <summary>
    /// 依次检查显式路径、NuGet RID 目录和应用目录；返回零时交给操作系统默认加载规则。
    /// </summary>
    /// <param name="libraryName">P/Invoke 声明的逻辑库名。</param>
    /// <param name="assembly">发起加载的程序集。</param>
    /// <param name="searchPath">运行库建议的默认搜索规则。</param>
    /// <returns>已加载模块句柄，或零以继续默认解析。</returns>
    private static nint Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        _ = assembly;
        _ = searchPath;
        if (!StringComparer.Ordinal.Equals(libraryName, NativeMethods.LibraryName))
        {
            return nint.Zero;
        }

        var configured = Environment.GetEnvironmentVariable("VERDANDI_NATIVE_LIBRARY");
        if (!string.IsNullOrWhiteSpace(configured))
        {
            return NativeLibrary.Load(configured);
        }

        var fileName = RuntimeInformation.IsOSPlatform(OSPlatform.Windows)
            ? "verdandi_cpp.dll"
            : RuntimeInformation.IsOSPlatform(OSPlatform.OSX)
                ? "libverdandi_cpp.dylib"
                : "libverdandi_cpp.so";
        var ridPath = Path.Combine(AppContext.BaseDirectory, "runtimes", RuntimeInformation.RuntimeIdentifier, "native", fileName);
        if (NativeLibrary.TryLoad(ridPath, out var handle))
        {
            return handle;
        }

        var applicationPath = Path.Combine(AppContext.BaseDirectory, fileName);
        return NativeLibrary.TryLoad(applicationPath, out handle) ? handle : nint.Zero;
    }
}
