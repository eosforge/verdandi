#include "gateway.hpp"
#include <astra/types.hpp>
#include <stdexcept>

namespace astra {
class Gateway::Rejected final : public grpc::ServerWriteReactor<proto::comet::v1::SessionReply> {
public:
    // 立即提交确定失败, 不占 Access 令牌或成功会话槽.
    explicit Rejected(grpc::Status status) {
        Finish(status);
    }

    // gRPC 归还最后一次借用后释放, 不主动结束其他 RPC.
    void OnDone() override {
        delete this;
    }
};

class Gateway::Stream final : public grpc::ServerWriteReactor<proto::comet::v1::SessionReply> {
public:
    // Session 已装入 Access, 所有会失败的响应准备都在 StartWrite 之前完成.
    Stream(Gateway& owner, grpc::CallbackServerContext& context, std::shared_ptr<Access::Session> session) : owner_(owner), session_(std::move(session)), revoked_(session_->stopped(), Cancel{&context}), stopped_(owner.stop_.get_token(), Cancel{&context}) {
        reply_.set_instance(owner.instance_);
        reply_.set_session(session_->token());
    }

    // 只由 handler 在成功创建 reactor 后调用一次, 之后缓冲直到 OnWriteDone 不可修改.
    void start() noexcept {
        StartWrite(&reply_);
    }

    // 不再下发第二份确认, 成功后空闲等待真实取消; Finish 必须等唯一在途 Write 归还.
    void OnWriteDone(bool ok) override {

        if (!ok) {
            owner_.access_.close(session_);
        }
        const std::lock_guard lock(mutex_);
        writing_ = false;
        if (!ok) {
            cancelled_ = true;
        }
        finish();
    }

    // 先撤销令牌授权再安排 Finish, 清理动作不撤回先前已提交的业务结果.
    void OnCancel() override {

        owner_.access_.close(session_);
        const std::lock_guard lock(mutex_);
        cancelled_ = true;
        finish();
    }

    // 全部 reaction 与在途写结束后归还容量, 自身回调注销也在 context 的最终寿命内完成.
    void OnDone() override {
        owner_.access_.close(session_);
        owner_.slots_.release();
        delete this;
    }

private:
    // 停止回调只通知传输层, 不在凭据撤销调用栈内访问 Gateway 索引或等待网络.
    struct Cancel {
        // 由 gRPC 持有至 Stream::OnDone 返回的上下文, 回调先于其寿命结束注销.
        grpc::CallbackServerContext* context;

        // 可以与 OnWriteDone 并发, TryCancel 不代替最终 Finish/OnDone.
        void operator()() const noexcept {
            context->TryCancel();
        }
    };

    // 在本流 mutex_ 内决定唯一 Finish, 不在 Write 未归还时复用发送端.
    void finish() {
        if (cancelled_ && !writing_ && !finished_) {
            finished_ = true;
            Finish(grpc::Status(grpc::StatusCode::CANCELLED, "Business session ended"));
        }
    }

    // Gateway 活过服务端排空, 不采用捕获析构中服务的异步工作队列.
    Gateway& owner_;
    // 令牌授权及取消状态, 真实 OnDone 后由 Access 与本对象共同释放.
    std::shared_ptr<Access::Session> session_;
    // 两类取消源分别是凭据撤销和服务器关闭, 不创建额外线程.
    std::stop_callback<Cancel> revoked_;
    // 服务端停止通知, 与上面的单会话撤销互相独立.
    std::stop_callback<Cancel> stopped_;
    // 首条确认的稳定缓冲, 写完成后保持不变至流析构.
    proto::comet::v1::SessionReply reply_;
    // 只保护三个反应状态, 不与 Access 或 Library 同时持有.
    std::mutex mutex_;
    // 初始即预约唯一写, gRPC 在 handler 返回后才能投递对应 reaction.
    bool writing_ = true;
    // 收到取消或发送失败后为 true, 不回退到活动状态.
    bool cancelled_{};
    // Finish 只发一次, OnDone 是真正的最终释放边界.
    bool finished_{};
};

Gateway::Gateway(Access& access, std::string instance, bool auth, std::ptrdiff_t maximum) : access_(access), instance_(std::move(instance)), auth_(auth), slots_(maximum >= 1 && maximum <= 65536 ? maximum : 1) {
    if (!Member::valid_id(instance_) || maximum < 1 || maximum > 65536) {
        throw std::invalid_argument("Invalid business gateway configuration");
    }
}

void Gateway::ready() noexcept {
    ready_.store(true, std::memory_order_release);
}

void Gateway::stop() noexcept {
    stop_.request_stop();
}

const std::string& Gateway::instance() const noexcept {
    return instance_;
}

grpc::Status Gateway::error(grpc::CallbackServerContext& context, grpc::StatusCode code, proto::comet::v1::Reason reason, std::string_view message, std::optional<std::uint64_t> version) const {

    proto::comet::v1::Failure failure; // 所有字段来自固定服务状态, 不回显外部请求正文.
    failure.set_reason(reason);
    failure.set_effect(proto::comet::v1::EFFECT_UNAPPLIED);
    failure.set_instance(instance_);
    failure.set_message(message);
    if (version) {
        failure.set_version(*version);
    }
    context.AddTrailingMetadata("comet-error-bin", failure.SerializeAsString());
    return {code, std::string(message)};
}

grpc::ServerWriteReactor<proto::comet::v1::SessionReply>* Gateway::Session(grpc::CallbackServerContext* context, const proto::comet::v1::SessionRequest* request) {

    if (!ready_.load(std::memory_order_acquire) || stop_.stop_requested()) {
        return new Rejected(error(*context, grpc::StatusCode::UNAVAILABLE, proto::comet::v1::REASON_BUSY, "Star is not accepting business sessions"));
    }
    if (!auth_) {
        return new Rejected(error(*context, grpc::StatusCode::FAILED_PRECONDITION, proto::comet::v1::REASON_INPUT, "Business authentication is disabled"));
    }
    if (!slots_.try_acquire()) {
        return new Rejected(error(*context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Business session capacity exceeded"));
    }
    // owns 仅用于 handler 异常清理, 一旦交给 Stream 就只在其 OnDone 归还槽位.
    std::shared_ptr<Access::Session> owns;
    bool reserved = true; // 错误回执本身若分配失败, catch 不得第二次归还信号量.
    try {
        const auto& secret = request->secret(); // 借用本次请求字节, 不缓存 protobuf 可写内存.
        auto session = access_.open(request->key(), {reinterpret_cast<const std::uint8_t*>(secret.data()), secret.size()});
        if (!session) {
            slots_.release();
            reserved = false;
            const auto code = session.error() == Access::Error::denied ? grpc::StatusCode::UNAUTHENTICATED : grpc::StatusCode::RESOURCE_EXHAUSTED;
            const auto reason = session.error() == Access::Error::denied ? proto::comet::v1::REASON_SESSION : proto::comet::v1::REASON_BUSY;
            return new Rejected(error(*context, code, reason, "Business login rejected"));
        }
        owns = std::move(*session);
        auto stream = std::make_unique<Stream>(*this, *context, owns); // 构造失败仍由 handler 归还授权和槽位.
        reserved = false;
        stream->start();
        return stream.release();
    } catch (...) {
        access_.close(owns);
        if (reserved) {
            slots_.release();
        }
        return new Rejected(error(*context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Business login preparation failed"));
    }
}

std::expected<std::optional<Access::Permit>, grpc::Status> Gateway::enter(grpc::CallbackServerContext& context) const {

    if (!ready_.load(std::memory_order_acquire) || stop_.stop_requested()) {
        return std::unexpected(error(context, grpc::StatusCode::UNAVAILABLE, proto::comet::v1::REASON_BUSY, "Star is not accepting business requests"));
    }
    if (!auth_) {
        return std::optional<Access::Permit>{};
    }
    // metadata 查找不创建临时正文; 只接受恰好一个原始令牌, 不兼容文本或 Base64 别名.
    const auto range = context.client_metadata().equal_range("comet-session-bin");
    if (range.first == range.second || std::next(range.first) != range.second || range.first->second.size() != 32) {
        return std::unexpected(error(context, grpc::StatusCode::UNAUTHENTICATED, proto::comet::v1::REASON_SESSION, "Invalid business session"));
    }
    const auto& token = range.first->second;
    auto permit = access_.enter(std::string_view(token.data(), token.size()));
    if (!permit) {
        return std::unexpected(error(context, grpc::StatusCode::UNAUTHENTICATED, proto::comet::v1::REASON_SESSION, "Business session expired"));
    }
    return std::optional<Access::Permit>(std::move(*permit));
}
} // namespace astra
