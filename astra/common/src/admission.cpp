// 功能: 推进连接和单次登记阶段, 校验准入结果, 在统一截止内处理重试与取消.
#include "admission.hpp"
#include "rpc_status.hpp"

#include <algorithm>
#include <utility>

#include <grpcpp/create_channel.h>
#include <openssl/rand.h>

namespace astra {
namespace {
// gRPC 接受 system_clock 截止, 剩余预算始终从单调时钟计算, 不在每次 RPC 重置总期限.
auto rpc_deadline(Clock::time_point deadline) {
    return std::chrono::system_clock::now() + std::max(deadline - Clock::now(), Clock::duration::zero());
}
} // namespace

Admission::Admission(const Config& config, std::shared_ptr<Identity> identity, std::function<void()> wake)
    : config_(config), identity_(std::move(identity)), wake_(std::move(wake)) {
    // 禁用 gRPC 内建重试, 使应用层复用启动幂等键并拥有完整尝试截止与失败分类.
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(2 * 1024 * 1024);
    args.SetMaxSendMessageSize(4096);
    args.SetInt("grpc.enable_retries", 0);
    channel_ = grpc::CreateCustomChannel(config_.supervisor, identity_->channel_credentials(), args);
    stub_ = proto::orbit::v1::Admission::NewStub(channel_);
    // 随机值只用于幂等, 不成为 Star 身份. 整个进程的重试和候选刷新始终复用它.
    std::string request_id(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(request_id.data()), request_id.size()) != 1) {
        throw std::runtime_error("Startup request entropy unavailable");
    }
    request_.set_request_id(std::move(request_id));
    request_.set_username(identity_->username());
    request_.set_password(identity_->password());
    request_.set_cluster_id(config_.cluster);
    request_.set_advertise(config_.advertise.text());
    request_.set_role(config_.role == Role::star ? proto::orbit::v1::ROLE_STAR : proto::orbit::v1::ROLE_PLANET);
    request_.set_group(config_.group);
}

void Admission::begin(std::uint32_t candidate_round) {
    if (phase_ != Phase::idle || cancelled_) {
        return;
    }
    request_.set_candidate_round(candidate_round);
    deadline_ = Clock::now() + config_.handshake_timeout;
    connect_deadline_ = std::min(deadline_, Clock::now() + config_.connect_timeout);
    phase_ = Phase::connecting;
    channel_->GetState(true);
}

void Admission::start_registration() {
    registration_ = std::make_shared<RegistrationCall>();
    registration_->context.set_deadline(rpc_deadline(deadline_));
    phase_ = Phase::registration;
    stub_->async()->Register(&registration_->context, &request_, &registration_->response, [call = registration_, wake = wake_](grpc::Status status) {
        call->status = std::move(status);
        call->done.store(true, std::memory_order_release);
        wake();
    });
}

std::optional<Result<Joined>> Admission::poll() {
    if (phase_ == Phase::idle) {
        return std::nullopt;
    }
    // 通道就绪后只发一次登记 RPC, 连接等待和登记共享同一次尝试的总期限.
    if (phase_ == Phase::connecting) {
        if (cancelled_ || Clock::now() >= connect_deadline_) {
            phase_ = Phase::idle;
            return std::unexpected(Error{cancelled_ ? ErrorCode::cancelled : ErrorCode::timeout, "Supervisor connection did not become ready"});
        }
        if (channel_->GetState(true) != GRPC_CHANNEL_READY) {
            return std::nullopt;
        }
        start_registration();
    }
    if (phase_ == Phase::registration && registration_->done.load(std::memory_order_acquire)) {
        phase_ = Phase::idle;
        // 成功和失败都消费当前调用. 回调仍持有自己的 shared_ptr, 发布完成后只执行独立唤醒.
        auto completed = std::exchange(registration_, {});
        if (!completed->status.ok() || cancelled_) {
            return std::unexpected(cancelled_ ? Error{ErrorCode::cancelled, "Admission cancelled"} : rpc_error(completed->status));
        }
        return validate(completed->response);
    }
    return std::nullopt;
}

Result<Joined> Admission::validate(proto::orbit::v1::RegistrationResponse& response) {
    const auto count = static_cast<std::size_t>(response.members_size());
    if (count > 4096 || (config_.role == Role::planet && count > 8) || (config_.role == Role::star && count > config_.max_members)) {
        return std::unexpected(Error{ErrorCode::capacity, "Admission list exceeds role capacity"});
    }
    // 对原始准入字节验签并核对部署. 首次接受 Supervisor 签发的身份, 后续刷新不得更换它.
    auto local =
        identity_->verify(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(response.admission().data()), response.admission().size()),
                          std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(response.signature().data()), response.signature().size()));
    if (!local || local->cluster != config_.cluster || local->group != config_.group || local->role != config_.role || local->address != config_.advertise ||
        local->principal != identity_->principal(config_.cluster, config_.advertise.text()) || (local_ && *local != *local_)) {
        return std::unexpected(Error{ErrorCode::identity, "Admission credential does not match this process"});
    }
    // 名单转换为拥有的成员值, 生成消息只留在适配层; 完整角色关系由后续 Policy 初始化检查.
    std::vector<Member> members;
    members.reserve(count);
    for (const auto& encoded : response.members()) {
        auto member = decode_member(encoded);
        if (!member) {
            return std::unexpected(member.error());
        }
        members.push_back(std::move(*member));
    }
    local_ = *local;
    auto hello = std::make_shared<proto::astra::v1::Hello>();
    hello->set_protocol_major(protocol_major);
    hello->set_protocol_minor(0);
    hello->set_max_frame_bytes(config_.max_frame_bytes);
    hello->set_admission(std::move(*response.mutable_admission()));
    hello->set_admission_signature(std::move(*response.mutable_signature()));
    return Joined{std::move(*local), std::move(members), std::move(hello)};
}

void Admission::cancel() {
    cancelled_ = true;
    if (phase_ == Phase::registration) {
        registration_->context.TryCancel();
    }
}

bool Admission::pending() const {
    return phase_ != Phase::idle;
}
} // namespace astra
