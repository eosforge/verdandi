// 许可证: MIT, 详见仓库根目录 LICENSE.
#include "admission.hpp"

#include <grpcpp/create_channel.h>
#include <limits>

namespace verdandi::peer {
namespace {
// gRPC 接受 system_clock 截止, 剩余预算始终从单调时钟计算, 不在每次 RPC 重置总期限.
auto rpc_deadline(Clock::time_point deadline) {
    return std::chrono::system_clock::now() + std::max(deadline - Clock::now(), Clock::duration::zero());
}
} // namespace

Error rpc_error(const grpc::Status& status) {
    ErrorCode code;
    switch (status.error_code()) {
    case grpc::StatusCode::UNAUTHENTICATED:
    case grpc::StatusCode::PERMISSION_DENIED:
        code = ErrorCode::identity;
        break;
    case grpc::StatusCode::ABORTED:
    case grpc::StatusCode::ALREADY_EXISTS:
        code = ErrorCode::conflict;
        break;
    case grpc::StatusCode::CANCELLED:
        code = ErrorCode::cancelled;
        break;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
        code = ErrorCode::capacity;
        break;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
        code = ErrorCode::timeout;
        break;
    case grpc::StatusCode::UNKNOWN:
    case grpc::StatusCode::UNAVAILABLE:
        code = ErrorCode::transport;
        break;
    default:
        code = ErrorCode::protocol;
        break;
    }
    return {code, "gRPC operation failed"};
}

Admission::Admission(const Config& config, std::shared_ptr<Identity> identity, PeerId id, std::function<void()> wake)
    : config_(config), identity_(std::move(identity)), id_(id), wake_(std::move(wake)) {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(2 * 1024 * 1024);
    args.SetMaxSendMessageSize(4096);
    args.SetInt("grpc.enable_retries", 0);
    channel_ = grpc::CreateCustomChannel(config_.supervisor, identity_->channel_credentials(), args);
    stub_ = wire::Admission::NewStub(channel_);
    login_.set_username(identity_->username());
    login_.set_password(identity_->password());
    login_.set_cluster_id(config_.cluster);
    login_.set_advertise(config_.advertise.text());
    request_.set_username(identity_->username());
    request_.set_password(identity_->password());
    request_.set_cluster_id(config_.cluster);
    request_.set_advertise(config_.advertise.text());
    request_.set_peer_id(id_.text());
    request_.set_role(config_.role == Role::star ? wire::NODE_ROLE_STAR : wire::NODE_ROLE_PLANET);
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
    request_.set_expected_epoch(*expected_epoch_);
    registration_ = std::make_shared<Call<wire::RegistrationResponse>>();
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
    if (phase_ == Phase::connecting) {
        if (cancelled_ || Clock::now() >= connect_deadline_) {
            phase_ = Phase::idle;
            return std::unexpected(Error{cancelled_ ? ErrorCode::cancelled : ErrorCode::timeout, "Supervisor connection did not become ready"});
        }
        if (channel_->GetState(true) != GRPC_CHANNEL_READY) {
            return std::nullopt;
        }
        if (expected_epoch_) {
            start_registration();
        } else {
            challenge_ = std::make_shared<Call<wire::RegistrationChallenge>>();
            challenge_->context.set_deadline(rpc_deadline(deadline_));
            phase_ = Phase::challenge;
            stub_->async()->Challenge(&challenge_->context, &login_, &challenge_->response, [call = challenge_, wake = wake_](grpc::Status status) {
                call->status = std::move(status);
                call->done.store(true, std::memory_order_release);
                wake();
            });
        }
    }
    if (phase_ == Phase::challenge && challenge_->done.load(std::memory_order_acquire)) {
        if (!challenge_->status.ok() || cancelled_) {
            phase_ = Phase::idle;
            return std::unexpected(cancelled_ ? Error{ErrorCode::cancelled, "Admission cancelled"} : rpc_error(challenge_->status));
        }
        if (challenge_->response.cluster_id() != config_.cluster || challenge_->response.expected_epoch() == std::numeric_limits<std::uint64_t>::max()) {
            phase_ = Phase::idle;
            return std::unexpected(Error{ErrorCode::protocol, "Invalid Supervisor baseline"});
        }
        // 此赋值仅发生一次. Register 响应丢失时继续携带原基线, 不追赶新进程的 epoch.
        expected_epoch_ = challenge_->response.expected_epoch();
        start_registration();
    }
    if (phase_ == Phase::registration && registration_->done.load(std::memory_order_acquire)) {
        phase_ = Phase::idle;
        if (!registration_->status.ok() || cancelled_) {
            return std::unexpected(cancelled_ ? Error{ErrorCode::cancelled, "Admission cancelled"} : rpc_error(registration_->status));
        }
        auto result = validate(registration_->response);
        registration_.reset();
        challenge_.reset();
        return result;
    }
    return std::nullopt;
}

Result<Joined> Admission::validate(wire::RegistrationResponse& response) const {
    const auto count = static_cast<std::size_t>(response.members_size());
    if (count > 4096 || (config_.role == Role::planet && count > 8) || (config_.role == Role::star && count > config_.max_peers)) {
        return std::unexpected(Error{ErrorCode::capacity, "Admission list exceeds role capacity"});
    }
    auto local = identity_->verify(response.admission(), response.signature());
    if (!local || local->cluster != config_.cluster || local->group != config_.group || local->id != id_ || local->role != config_.role ||
        local->address != config_.advertise || local->principal != identity_->principal(config_.cluster, config_.advertise.text()) ||
        local->epoch.value != *expected_epoch_ + 1) {
        return std::unexpected(Error{ErrorCode::identity, "Admission credential does not match this process"});
    }
    std::vector<Member> members;
    members.reserve(count);
    for (const auto& encoded : response.members()) {
        auto member = decode_member(encoded);
        if (!member) {
            return std::unexpected(member.error());
        }
        members.push_back(std::move(*member));
    }
    auto hello = std::make_shared<wire::Hello>();
    hello->set_protocol_major(4);
    hello->set_protocol_minor(0);
    hello->set_max_frame_bytes(config_.max_frame_bytes);
    hello->set_admission(std::move(*response.mutable_admission()));
    hello->set_admission_signature(std::move(*response.mutable_signature()));
    return Joined{std::move(*local), std::move(members), std::move(hello)};
}

void Admission::cancel() {
    cancelled_ = true;
    if (phase_ == Phase::challenge) {
        challenge_->context.TryCancel();
    }
    if (phase_ == Phase::registration) {
        registration_->context.TryCancel();
    }
}

bool Admission::pending() const {
    return phase_ != Phase::idle;
}
} // namespace verdandi::peer
