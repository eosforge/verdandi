using Verdandi.Internal;

namespace Verdandi;

/// <summary>
/// 查询当前加载的原生 Verdandi 运行库能力；结果只描述代码，不探测 Redis、证书、ACL 或网络状态。
/// </summary>
public static class Runtime
{
    /// <summary>严格 v1 JSON 配置解析和离线校验。</summary>
    public const string ConfigurationJson = "configuration.json";

    /// <summary>根 Client 及其生命周期。</summary>
    public const string Client = "client";

    /// <summary>有界 Redis Key/Hash 原始命令。</summary>
    public const string RedisCommands = "redis.commands";

    /// <summary>使用固定证书身份的 Redis Sentinel TLS。</summary>
    public const string RedisSentinelTls = "redis.sentinel_tls";

    /// <summary>Registration 生命周期能力。</summary>
    public const string Registration = "registration";

    /// <summary>Selector 同步和本地选择能力。</summary>
    public const string Selector = "selector";

    /// <summary>Catalog Publisher/Subscriber 能力。</summary>
    public const string Catalog = "catalog";

    /// <summary>
    /// 查询一项稳定字符串能力。未知或空名称成功返回 false；原生库无法加载或不含查询入口时返回明确错误。
    /// </summary>
    /// <param name="capability">已公布能力名称；UTF-8 编码长度最多 128 字节。</param>
    public static Result<bool> Supports(string capability)
    {
        var abi = NativeRuntime.CheckAbi();
        if (!abi.IsSuccess)
        {
            return Result<bool>.Failure(abi.Error!);
        }

        var encoded = Interop.EncodeUtf8(capability, "capability", 128);
        if (!encoded.IsSuccess)
        {
            return Result<bool>.Failure(encoded.Error!);
        }

        using var value = new NativeBufferLease(encoded.Value);
        try
        {
            return Result<bool>.Success(NativeMethods.HasCapability(value.StringView) != 0);
        }
        catch (EntryPointNotFoundException exception)
        {
            return Result<bool>.Failure(new VerdandiError("incompatible", "native_library", exception.Message));
        }
    }
}
