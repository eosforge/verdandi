// 功能: 定义 Supervisor 准入状态机及异步 RPC 所有权, 保持请求地址和启动幂等键稳定.
#pragma once
#include "identity.hpp"
#include "orbit.grpc.pb.h"

#include <atomic>
#include <functional>

namespace astra {
struct Joined {
    Member local;
    std::vector<Member> members;
    std::shared_ptr<const proto::astra::v1::Hello> hello;
};

// 唯一拥有 Supervisor RPC 的上下文和输入, 控制循环轮询完成, gRPC 回调不修改角色索引.
class Admission {
public:
    // 复制已校验配置, 持有 identity, 进程 ID 由 Supervisor 签发, wake 须可由 gRPC 回调安全调用且不得借用 Runtime.
    // 构造只准备通道和稳定请求缓冲; 使用者必须在 pending 变为 false 后才能销毁对象.
    Admission(const Config& config, std::shared_ptr<Identity> identity, std::function<void()> wake);
    // gRPC 借用成员中的请求地址, Admission 从构造到排空回调都不能搬移.
    Admission(const Admission&) = delete;
    // 禁止复制赋值, 防止替换仍被 gRPC 借用的请求和上下文.
    Admission& operator=(const Admission&) = delete;
    // 禁止移动构造, 保持已提交请求的地址不变.
    Admission(Admission&&) = delete;
    // 禁止移动赋值, 防止在途 RPC 指向旧对象存储.
    Admission& operator=(Admission&&) = delete;
    // 没有在途 RPC 时开始一次尝试, candidate_round 只影响 Planet 候选, 不改变启动请求.
    void begin(std::uint32_t candidate_round);
    // 返回完成的一次结果; nullopt 表示等待. 响应对象在完成回调结束前始终由共享状态持有.
    std::optional<Result<Joined>> poll();
    // 由控制循环永久取消此 Admission, 对在途 RPC 发出 TryCancel; 仍须 poll 到 pending 为 false 后销毁.
    void cancel();
    // 由控制循环查询是否仍有未消费的尝试, 包括连接等待阶段; true 不表示已经提交 Register.
    bool pending() const;

private:
    struct RegistrationCall {
        grpc::ClientContext context;
        proto::orbit::v1::RegistrationResponse response;
        grpc::Status status;
        std::atomic_bool done{false};
    };
    // 只由控制循环切换; 回调仅发布登记调用的完成状态.
    enum class Phase {
        // 当前尝试未启动或结果已消费; 若未取消, begin 可以启动下一次尝试.
        idle,
        // 等待 Supervisor TLS 通道就绪, 同时受连接期限和握手总期限约束.
        connecting,
        // Register RPC 在途, 申请准入或刷新 Planet 候选.
        registration
    };
    // 仅在通道就绪且无登记 RPC 在途时调用, 使用剩余总预算提交 Register 并发布回调完成标志.
    void start_registration();
    // 在 Register 完成后核对容量, 本地部署和已安装身份, 返回独立名单与共享 Hello.
    // response 的准入字节会移入 Hello; 失败返回 capacity 或 identity, 名单角色策略由 initialize 继续校验.
    Result<Joined> validate(proto::orbit::v1::RegistrationResponse& response);
    Config config_;
    std::shared_ptr<Identity> identity_;
    // 首次成功后固定身份, 候选刷新不能默默替换本进程.
    std::optional<Member> local_;
    std::function<void()> wake_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<proto::orbit::v1::Admission::Stub> stub_;
    Phase phase_{};
    bool cancelled_{};
    Clock::time_point deadline_;
    Clock::time_point connect_deadline_;
    proto::orbit::v1::RegistrationRequest request_;
    std::shared_ptr<RegistrationCall> registration_;
};

} // namespace astra
