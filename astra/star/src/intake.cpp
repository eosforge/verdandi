#include "intake.hpp"
#include <astra/policy.hpp>
#include <grpcpp/create_channel.h>

namespace astra {
Intake::Intake(std::shared_ptr<Identity> identity, const proto::astra::v1::Hello& local, Member target, Library& output, Access& access, std::function<void()> wake) : identity_(std::move(identity)), target_(std::move(target)), receiver_(output, access), wake_(std::move(wake)) {

    // arguments 限定编码预算及传输探活. 首帧声明同步入口能力, 不沿用旧控制流的 4 KiB 实际限制.
    grpc::ChannelArguments arguments;
    arguments.SetMaxReceiveMessageSize(8 * 1024 * 1024);
    arguments.SetMaxSendMessageSize(8 * 1024 * 1024);
    arguments.SetInt("grpc.enable_retries", 0);
    arguments.SetInt("grpc.keepalive_time_ms", 30000);
    arguments.SetInt("grpc.keepalive_timeout_ms", 10000);
    channel_ = grpc::CreateCustomChannel(target_.address.text(), identity_->channel_credentials(), arguments);
    stub_ = proto::polaris::v1::Almanac::NewStub(channel_);
    *output_.mutable_hello() = local;
    output_.mutable_hello()->set_max_frame_bytes(8 * 1024 * 1024);

    // 先完成所有可抛分配, 最后绑定 reactor. 控制循环接管所有权后才 StartCall.
    stub_->async()->Open(&context_, this);
    AddHold();
}

bool Intake::done() const noexcept {
    return done_.load(std::memory_order_acquire);
}

bool Intake::ready() const noexcept {
    return authenticated_ && !error_ && receiver_.ready();
}

const Member& Intake::target() const noexcept {
    return target_;
}

std::optional<Status::Code> Intake::error() const noexcept {
    return error_;
}

void Intake::OnReadDone(bool ok) {
    read_.store(ok ? 1 : -1, std::memory_order_release);
    wake_();
}

void Intake::OnWriteDone(bool ok) {
    write_.store(ok ? 1 : -1, std::memory_order_release);
    wake_();
}

void Intake::OnDone(const grpc::Status&) {

    // wake 先转为独立局部所有权, 完成发布后外部可能立即回收 reactor, 不再读取成员.
    auto wake = std::move(wake_);
    done_.store(true, std::memory_order_release);
    wake();
}

void Intake::cancel(Status::Code cause) {

    if (error_) {
        return;
    }
    error_ = cause;
    if (!started_) {
        started_ = true;
        StartCall(); // 未开始的绑定也必须进入完成流程, 不能直接销毁 context.
    }
    context_.TryCancel();
    RemoveHold(); // 此后只等待 OnDone, 不再发起任何外部读写.
}

Result<void> Intake::receive() {

    if (authenticated_) {
        return receiver_.receive(input_);
    }
    if (!input_.has_hello() || input_.hello().protocol_major() != protocol_major || input_.hello().max_frame_bytes() < 1024 || input_.hello().max_frame_bytes() > 8 * 1024 * 1024) {
        return Status::protocol("Invalid Polaris Hello");
    }
    const auto& hello = input_.hello(); // 验原始签名材料, 不重编码再比较签名.
    auto remote = identity_->verify(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(hello.admission().data()), hello.admission().size()), std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(hello.admission_signature().data()), hello.admission_signature().size()));
    if (!remote || remote->role != Member::Role::polaris || remote->galaxy != target_.galaxy || remote->principal != target_.principal || remote->address != target_.address) {
        return Status::identity("Polaris admission does not match deployment");
    }
    if (auto replacement = supersedes(*remote, target_); !replacement) {
        return std::unexpected(replacement.error());
    }
    target_ = std::move(*remote);
    maximum_ = hello.max_frame_bytes();
    authenticated_ = true;
    return {};
}

void Intake::pump(Steady::time_point now) {

    if (done() || error_) {
        return;
    }
    try {
        if (!started_) {
            started_ = true;
            writing_ = true;
            deadline_ = now + std::chrono::seconds(30);
            StartRead(&input_);
            StartWrite(&output_);
            StartCall();
        }
        if ((!authenticated_ && now >= handshake_) || (writing_ && now >= deadline_) || now - received_ >= std::chrono::seconds(75)) {
            cancel(Status::Code::timeout);
            return;
        }

        // received/sent 以 acquire 取得缓冲所有权, 失败即取消, 不把 EOF 当作快照完成.
        const auto received = read_.exchange(0, std::memory_order_acq_rel);
        const auto sent = write_.exchange(0, std::memory_order_acq_rel);
        if (received < 0 || sent < 0) {
            cancel(Status::Code::transport);
            return;
        }
        if (sent > 0) {
            writing_ = false;
        }
        if (received > 0) {
            if (auto result = receive(); !result) {
                cancel(result.error().code);
                return;
            }
            received_ = now;
            input_.Clear();
            StartRead(&input_);
        }

        if (authenticated_ && !writing_) {
            if (auto packet = receiver_.next(maximum_)) {
                output_ = std::move(*packet); // 仅写完成后移动新帧, 没有每条提交独立线程或队列.
                writing_ = true;
                deadline_ = now + std::chrono::seconds(30);
                StartWrite(&output_);
            }
        }
    } catch (...) {
        cancel(Status::Code::internal); // 所有分配/解析错误只终止本流, 已完整安装底稿保持.
    }
}
} // namespace astra
