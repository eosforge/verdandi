// 功能: 声明只读身份材料, TLS 凭证和准入验签接口, 将生成消息隔离在传输层.
#pragma once
#include <verdandi/cluster/config.hpp>

#include "cluster.pb.h"
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/security/tls_certificate_provider.h>

namespace verdandi::cluster {
namespace wire = ::verdandi::cluster::v1;

// 生成类型只在传输适配层使用, 名单状态转换后拥有独立的 Member 值.
// value 在调用期间借用; 成功返回拥有数据且通过基础校验的成员, 无效编码返回 identity, 此函数不验签.
Result<Member> decode_member(const wire::RegistrationResponse::Member& value);

// TLS 和准入材料在启动时加载, 此后只读共享. 不提供通用格式化或完整配置输出.
class Identity {
public:
    // 从 directory 读取五个有界普通文件, 校验证书链, 私钥和 advertise 的 IP SAN, 成功返回只读共享材料.
    // 材料或凭证错误返回 identity, 验证资源分配失败可能返回 internal; 不输出文件正文, 不发起网络连接.
    static Result<std::shared_ptr<Identity>> load(const std::filesystem::path& directory, const Endpoint& advertise);
    // 摘要由账号名, Galaxy 和规范端点以 NUL 分隔构成, 密码不作为身份或签名密钥.
    // cluster 和 endpoint 应为已验证的 Galaxy 名称及规范端点文本; 返回拥有的 SHA-256 摘要, 不缓存借用输入.
    Principal principal(std::string_view cluster, std::string_view endpoint) const;
    // 对原始 admission 字节验签后解析, 不重新序列化构造验签输入.
    // payload 最多 1024 字节, signature 必须为 64 字节 Ed25519 签名; 失败返回 identity, 成功成员不借用凭证缓冲.
    Result<Member> verify(std::span<const std::uint8_t> payload, std::span<const std::uint8_t> signature) const;
    // 构造共享 TLS 1.3 客户端凭证, 校验服务端证书及目标主机, 不向对端提供客户端证书.
    std::shared_ptr<grpc::ChannelCredentials> channel_credentials() const;
    // 构造共享 TLS 1.3 服务端凭证, 创建选项失败返回 nullptr; 客户端身份由后续准入协议验证.
    std::shared_ptr<grpc::ServerCredentials> server_credentials() const;
    // 只允许准入模块读取账号材料; 这些借用不逃出 Identity 生命周期.
    // 返回登录账号的只读借用, 必须在 Identity 存活期间使用, 不用于通用诊断输出.
    const std::string& username() const;
    // 返回登录密码的只读借用, 仅用于准入请求, 不记录日志或逃逸 Identity 生命周期.
    const std::string& password() const;

private:
    std::string username_;
    std::string password_;
    std::array<std::uint8_t, 32> authority_{};
    std::shared_ptr<grpc::experimental::InMemoryCertificateProvider> provider_;
};
} // namespace verdandi::cluster
