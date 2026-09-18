#pragma once
#include "identity.hpp"
#include <grpcpp/server_context.h>
#include <semaphore>
#include <vector>

namespace astra {
// Pulsar 签发权威, 持有账号摘要和签名私钥, 不承担成员持久状态.
class Authority {
public:
    // 加载既有 Supervisor 格式的账号和密钥, 验证两个监听端点的证书授权; 失败不返回部分对象.
    static Result<std::unique_ptr<Authority>> load(const std::filesystem::path& directory, const Endpoint& admission, const Endpoint& pulse);
    // 释放时清理内存中的 Ed25519 私钥; 服务和请求必须已退出.
    ~Authority();
    // 验证账号密码与角色. 固定 PBKDF2-SHA256/600000, 最多四次并发 KDF, 超载立即返回.
    grpc::Status authenticate(grpc::ServerContext& context, const proto::orbit::v1::RegistrationRequest& request) const;
    // 对已经校验的 Member 编码并签名; response 由本次调用独占, 异常由 RPC 边界转为固定错误.
    void sign(const proto::orbit::v1::Member& member, proto::orbit::v1::RegistrationResponse& response) const;
    // 借用已校验的 TLS/准入身份, 仅在签发者存活期间有效, 不交出修改权限.
    const Identity& identity() const;
    // 签发公钥摘要, 用于将持久账本绑定到同一权威, 不暴露签名秘密.
    std::string key_id() const;

private:
    // 禁止调用者跳过完整材料校验构造可用签发者.
    Authority() = default;

    // 只保存盐与摘要, 角色位 1=Star, 2=Planet; 不保留明文密码.
    struct Account {
        // 大小写敏感的安全 ASCII 账号名.
        std::string username;
        // 固定 16 字节随机盐.
        std::array<std::uint8_t, 16> salt{};
        // PBKDF2 派生的 32 字节摘要.
        std::array<std::uint8_t, 32> hash{};
        // 启动校验得到的非零角色位集合.
        unsigned roles{};
    };

    // 校验后只读的 TLS 证书和准入公钥.
    std::shared_ptr<Identity> identity_;
    // BoringSSL 的 seed + public key 形式, 固定 64 字节.
    std::array<std::uint8_t, 64> private_key_{};
    // 从已经验证的公钥计算, 不依赖 BoringSSL 私钥缓冲的内部布局.
    Principal key_id_;
    // 最多 64 个账号, 认证路径只读查找.
    std::vector<Account> accounts_;
    // 登记密码计算与 Pulse 完全分离; 不为等待计算的请求建立无界队列.
    mutable std::counting_semaphore<4> passwords_{4};
};
} // namespace astra
