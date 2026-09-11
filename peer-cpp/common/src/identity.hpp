// 许可证: MIT, 详见仓库根目录 LICENSE.
#pragma once
#include <verdandi/peer/config.hpp>

#include "peer.pb.h"
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/security/tls_certificate_provider.h>

namespace verdandi::peer {
namespace wire = ::verdandi::peer::v1;

// 生成类型只在传输适配层使用, 名单状态转换后拥有独立的 Member 值.
Result<Member> decode_member(const wire::RegistrationResponse::Member& value);

// TLS 和准入材料在启动时加载, 此后只读共享. 不提供通用格式化或完整配置输出.
class Identity {
public:
    static Result<std::shared_ptr<Identity>> load(const std::filesystem::path& directory, const Endpoint& advertise);
    // 用系统随机源创建 UUIDv4, 失败停止启动, 重连不得重新生成.
    static Result<PeerId> new_id();
    // 摘要含 NUL 分隔, 仅账号名与规范端点参与, 密码不作为身份或签名密钥.
    Principal principal(std::string_view cluster, std::string_view endpoint) const;
    // 对原始 admission 字节验签后解析, 不重新序列化构造验签输入.
    Result<Member> verify(std::string_view payload, std::string_view signature) const;
    std::shared_ptr<grpc::ChannelCredentials> channel_credentials() const;
    std::shared_ptr<grpc::ServerCredentials> server_credentials() const;
    // 只允许准入模块读取账号材料; 这些借用不逃出 Identity 生命周期.
    const std::string& username() const;
    const std::string& password() const;

private:
    std::string username_;
    std::string password_;
    std::array<std::uint8_t, 32> authority_{};
    std::shared_ptr<grpc::experimental::InMemoryCertificateProvider> provider_;
};
} // namespace verdandi::peer
