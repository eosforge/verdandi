// 许可证: MIT, 详见仓库根目录 LICENSE.
#pragma once
#include "admission.grpc.pb.h"
#include "identity.hpp"

#include <atomic>
#include <functional>

namespace verdandi::peer {
struct Joined {
    Member local;
    std::vector<Member> members;
    std::shared_ptr<const wire::Hello> hello;
};

// 唯一拥有 Supervisor RPC 的上下文和输入, 控制循环轮询完成, gRPC 回调不修改角色索引.
class Admission {
public:
    Admission(const Config& config, std::shared_ptr<Identity> identity, PeerId id, std::function<void()> wake);
    // 没有在途 RPC 时开始一次尝试, candidate_round 只影响 Planet 候选, 不改变首次 CAS 基线.
    void begin(std::uint32_t candidate_round);
    // 返回完成的一次结果; nullopt 表示等待. 响应对象在完成回调结束前始终由共享状态持有.
    std::optional<Result<Joined>> poll();
    void cancel();
    bool pending() const;

private:
    template <class Response> struct Call {
        grpc::ClientContext context;
        Response response;
        grpc::Status status;
        std::atomic_bool done{false};
    };
    enum class Phase { idle, connecting, challenge, registration };
    void start_registration();
    Result<Joined> validate(wire::RegistrationResponse& response) const;
    Config config_;
    std::shared_ptr<Identity> identity_;
    PeerId id_;
    std::function<void()> wake_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<wire::Admission::Stub> stub_;
    Phase phase_{};
    bool cancelled_{};
    std::optional<std::uint64_t> expected_epoch_;
    Clock::time_point deadline_;
    Clock::time_point connect_deadline_;
    wire::LoginRequest login_;
    wire::RegistrationRequest request_;
    std::shared_ptr<Call<wire::RegistrationChallenge>> challenge_;
    std::shared_ptr<Call<wire::RegistrationResponse>> registration_;
};

// 只转换 gRPC 稳定状态码, 远端 message/details 不进入诊断, 暂态传输失败不能隔离身份.
Error rpc_error(const grpc::Status& status);
} // namespace verdandi::peer
