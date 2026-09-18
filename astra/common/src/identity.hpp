// 详细说明: 该头文件负责处理与身份验证相关的核心逻辑声明，包括证书加载、凭证生成以及基于 Ed25519 的消息验签。
// 本文件引入的 protobuf 和 gRPC 类型被严格限制在适配层内，不污染上层业务逻辑。
#pragma once
#include <astra/config.hpp>

#include "astra.pb.h"
#include "orbit.pb.h"
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/security/tls_certificate_provider.h>

namespace astra {

// 当前实现只接受 v1. 所有生产消息和推流探针共用此版本, 无旧版本协商路径.
// 详细说明: 版本常量，硬编码为 1。强制要求所有通信对端一致，不提供版本降级。
inline constexpr std::uint32_t protocol_major = 1;

// Orbit 准入用途的唯一签名域, NUL 分隔字节由编码处显式追加.
// 详细说明: 作为签名输入的前缀，用于区分不同作用域的签名，防止重放攻击。
inline constexpr std::string_view admission_signature_domain = "proto.orbit.v1.admission";

// 生成类型只在传输适配层使用, 名单状态转换后拥有独立的 Member 值.
// - value (const proto::orbit::v1::Member&): protobuf 消息，在调用期间被只读借用。
// 返回值: 成功返回拥有数据且通过基础校验的成员结构，失败（无效编码等）返回包含 Status::Code::identity 错误的 unexpected。
// 详细说明: 负责将底层 Protobuf 类型转换为系统内部业务使用的强类型 Member。需要注意此函数不进行验签，只进行格式和边界校验。
Result<Member> decode_member(const proto::orbit::v1::Member& value);

// TLS 和准入材料在启动时加载, 此后只读共享. 不提供通用格式化或完整配置输出.
// 详细说明: 封装了本地密钥、证书等安全材料，支持构造客户端和服务端的 gRPC TLS 凭证，并提供 Ed25519 的验签功能。
class Identity {
public:
    // - directory (const std::filesystem::path&): 存放凭据文件的目录路径。
    // - advertise (const Endpoint&): 需要在 TLS 证书扩展 (SAN) 中进行匹配校验的 IP 端点。
    // 返回值: 成功返回一个在堆上分配且只读共享的 Identity 实例。
    // 详细说明: 从 directory 读取五个有界普通文件（ca.pem, cert.pem, key.pem, admission.pub, login.json），
    // 校验证书链, 私钥和 advertise 的 IP SAN, 成功返回只读共享材料。材料或凭证错误返回 identity 错误，验证资源分配失败可能返回 internal。
    // 不输出文件正文, 不发起网络连接。
    static Result<std::shared_ptr<Identity>> load(const std::filesystem::path& directory, const Endpoint& advertise);

    // Pulsar 服务端复用相同 TLS 和公钥校验, 不读取节点专用的 login.json.
    // directory 必须包含 ca.pem, cert.pem, key.pem 和 admission.pub; advertise 用于核对 IP SAN.
    static Result<std::shared_ptr<Identity>> load_server(const std::filesystem::path& directory, const Endpoint& advertise);

    // - cluster (std::string_view): 已验证的 Galaxy（星系）集群名称。
    // - endpoint (std::string_view): 规范化后的端点字符串形式。
    // 返回值: 返回一个拥有 SHA-256 数据的 Principal 实例，绝不缓存或者借用输入参数。
    // 详细说明: 摘要由账号名, Galaxy 集群名和规范端点以 NUL (0x00) 分隔后通过 SHA-256 计算构成。
    // 注意，密码仅作登录用，不作为身份或签名密钥的一部分。
    Principal principal(std::string_view cluster, std::string_view endpoint) const;

    // - value (std::span<const std::uint8_t>): 待校验及解析的有效负载，上限为 1024 字节。
    // - signature (std::span<const std::uint8_t>): 64 字节的 Ed25519 签名。
    // 返回值: 成功返回独立持有的 Member 对象（不借用 value 的内存缓冲）。失败返回 identity 错误。
    // 详细说明: 对原始 admission 字节使用 Ed25519 验签后再进行 Protobuf 解析。
    // 直接验原字节流，而不将其重新序列化来构造验签输入。
    Result<Member> verify(std::span<const std::uint8_t> value, std::span<const std::uint8_t> signature) const;

    // 返回值: 共享的 ChannelCredentials 实例。
    // 详细说明: 构造共享 TLS 1.3 客户端凭证, 校验服务端证书以及目标主机，并且此凭证不会向对端(服务端)提供客户端自身的证书。
    std::shared_ptr<grpc::ChannelCredentials> channel_credentials() const;

    // 返回值: 共享的 ServerCredentials 实例。如果底层选项创建失败则返回 nullptr。
    // 详细说明: 构造共享 TLS 1.3 服务端凭证，且配置为不主动要求客户端出示客户端 TLS 证书；因为客户端身份由后续的准入 (admission) 协议另外验证。
    std::shared_ptr<grpc::ServerCredentials> server_credentials() const;

    // 返回值: 用户名字符串的只读引用。
    // 详细说明: 只允许准入模块读取账号材料；这些借用引用决不能逃出 Identity 的生命周期（不能存储为长期引用的指针），且不用于通用的诊断输出。
    const std::string& username() const;

    // 返回值: 密码字符串的只读引用。
    // 详细说明: 仅用于生成准入请求时的鉴权字段，严禁记录在日志或以任何形式逃逸出 Identity 生命周期。
    const std::string& password() const;

private:
    // 从 login.json 中读取并保存的用户名 (默认值: 空字符串)。
    std::string username_;
    // 从 login.json 中读取并保存的密码 (默认值: 空字符串)。
    std::string password_;
    // 保存公钥 authority 的缓冲数组，固定长度为 32 字节。用于 Ed25519 签名校验 (默认值: 全 0)。
    std::array<std::uint8_t, 32> authority_{};
    // gRPC 提供的一种在内存中维护证书链以及私钥的提供者组件，管理 TLS 的安全材料上下文。
    std::shared_ptr<grpc::experimental::InMemoryCertificateProvider> provider_;
};
} // namespace astra
