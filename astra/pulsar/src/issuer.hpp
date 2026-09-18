#pragma once
#include "authority.hpp"
#include "ledger.hpp"
#include "orbit.grpc.pb.h"

namespace astra {
class Issuer final : public proto::orbit::v1::Admission::Service {
public:
    Issuer(std::string galaxy, std::string pulse_endpoint, const PulsarAuthority& authority, MembershipLedger& ledger);
    // TLS 账号验证后登记; 所有异常在 RPC 边界转换为固定状态, 不泄露密码或本地文件路径.
    grpc::Status Register(grpc::ServerContext* context, const proto::orbit::v1::RegistrationRequest* request,
                          proto::orbit::v1::RegistrationResponse* response) override;

private:
    // 只允许登记到启动配置的 Galaxy.
    const std::string galaxy_;
    // 回包告诉 Star 去独立的 Pulse 端口采样, 不挤占登记连接.
    const std::string pulse_endpoint_;
    // 只读密码与签名材料.
    const PulsarAuthority& authority_;
    // 唯一持久成员存储.
    MembershipLedger& ledger_;
};
} // namespace astra
