using Verdandi.Internal;

namespace Verdandi;

/// <summary>
/// 提供跨语言 v1 JSON 的离线校验；解析和规则由同一 C++ 核心执行，不会访问 Redis 或读取 TLS 文件。
/// </summary>
public static unsafe class Configuration
{
    private const int MaximumJsonBytes = 1024 * 1024;

    /// <summary>
    /// 严格校验配置版本、UTF-8、字段集合、默认值范围及字段关系，但不建立连接或创建任何长期对象。
    /// </summary>
    /// <param name="json">不超过 1 MiB 的完整 v1 JSON 文本。</param>
    /// <returns>配置可被当前运行库接受时成功，否则返回稳定的 code/field。</returns>
    public static Result Validate(string json)
    {
        var abi = NativeRuntime.CheckAbi();
        if (!abi.IsSuccess)
        {
            return abi;
        }

        var encoded = Encode(json);
        if (!encoded.IsSuccess)
        {
            return Result.Failure(encoded.Error!);
        }

        using var source = new NativeBufferLease(encoded.Value);
        NativeError error = default;
        try
        {
            return Interop.Status(NativeMethods.ConfigurationValidateJson(source.BytesView, &error), error);
        }
        catch (EntryPointNotFoundException exception)
        {
            // ABI v1 允许向后兼容地增加符号；旧运行库缺少离线校验入口时返回明确兼容性错误。
            return Result.Failure(new VerdandiError("incompatible", "native_library", exception.Message));
        }
    }

    /// <summary>在分配字节数组前完成严格 UTF-16 和 1 MiB 编码长度检查。</summary>
    internal static Result<byte[]> Encode(string json) => Interop.EncodeUtf8(json, "json", MaximumJsonBytes);
}
