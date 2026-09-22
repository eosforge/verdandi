#include "publishing.hpp"
#include <algorithm>
#include <astra/scope.hpp>
#include <grpc/support/time.h>

namespace comet::detail {
Publishing::Publishing(std::shared_ptr<Core> core, Scope scope, std::string key, std::chrono::milliseconds ttl, Publisher::Options options) : core_(std::move(core)), scope_(std::move(scope)), key_(std::move(key)), ttl_(ttl), lifetime_(ttl), changed_(std::move(options.changed)) {
    lifetime_.spread(std::hash<std::string>{}(key_)); // 固定 Key 抖动避免同批对象每次一起续租.
}

Publishing::~Publishing() {
    static_cast<void>(core_->resize(bytes_, 0));
}

Publishing::Call::~Call() {
    if (owner) {
        if (claimed) {
            owner->core_->returning(renewal, automatic);
        }
        static_cast<void>(owner->core_->resize(bytes, 0));
    }
}

Error Publishing::error(Error::Code code, Error::Effect effect) {
    return Error{code, effect, {}, {}, {}};
}

void Publishing::settle(const std::shared_ptr<Pending>& pending, Result<Publisher::Receipt> result) {
    if (pending && pending->result) {
        pending->result->set_value(std::move(result));
        if (pending->admission) {
            pending->admission->settled();
            pending->admission.reset();
        }
        pending->result.reset();
    }
}

std::future<Result<Publisher::Receipt>> Publishing::publish(std::uint64_t version, Value value, std::chrono::milliseconds timeout) {

    auto pending = std::make_shared<Pending>(nullptr, version, std::move(value)); // 先准备结果, 失败不替换旧期望.
    pending->result.emplace();
    auto future = pending->result->get_future();
    if (!version || !pending->value || pending->value->size() > 1024 * 1024 || timeout.count() <= 0 || timeout > std::chrono::minutes(1)) {
        settle(pending, std::unexpected(error(Error::Code::input)));
        return future;
    }
    pending->deadline = std::chrono::steady_clock::now() + timeout;
    {
        const std::lock_guard lock(mutex_);
        if (closed() || core_->stopped()) {
            settle(pending, std::unexpected(error(Error::Code::closed)));
            return future;
        }
        if (wanted_ && version < wanted_->version) {
            settle(pending, std::unexpected(error(Error::Code::version)));
            return future;
        }
        if (wanted_ && version == wanted_->version) {
            if (pending->value != wanted_->value && *pending->value != *wanted_->value) {
                settle(pending, std::unexpected(error(Error::Code::conflict)));
                return future;
            }
            pending->value = wanted_->value; // 同版本同内容只复用已有不可变载荷.
        }
        const bool replacing = wanted_ && wanted_->result && wanted_->admission && (!call_ || call_->pending != wanted_); // 替代一个未发送调用可移交其现有额度.
        if (!replacing && !core_->admitting()) {
            settle(pending, std::unexpected(error(Error::Code::busy)));
            return future;
        }
        if (!replacing) {
            pending->admission = core_;
        } // 新调用先取得额度, 拒绝不修改原期望.
        const auto bytes = pending->value->size() + 1024;
        if (!core_->resize(bytes_, bytes)) {
            settle(pending, std::unexpected(error(Error::Code::busy)));
            return future;
        }
        bytes_ = bytes;
        if (replacing) {
            pending->admission = std::move(wanted_->admission);
        } // 所有资源准备成功后才转移, 旧 future 仍明确结算为替代.
        if (!call_ || call_->pending != wanted_) {
            settle(wanted_, std::unexpected(error(Error::Code::obsolete)));
        }
        wanted_ = std::move(pending);
        applied_ = false;
        rejected_ = false;
        retry_ = {};
        report(Publisher::Phase::waiting);
    }
    core_->wake();
    return future;
}

Publisher::State Publishing::state() const {
    const std::lock_guard lock(mutex_);
    auto state = state_; // 不让读状态反向修改已发布快照.
    if (closed() || core_->stopped()) {
        state.phase = Publisher::Phase::closed;
    } else if (state.phase == Publisher::Phase::ready && !lifetime_.ready(Lifetime::now())) {
        state.phase = Publisher::Phase::uncertain;
    }
    return state;
}

bool Publishing::closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

bool Publishing::finished() const noexcept {
    return finished_.load(std::memory_order_acquire);
}

void Publishing::close() noexcept {
    bool first; // 请求结果与停止门一起定序, 对象析构前完成未发送的 future.
    {
        const std::lock_guard lock(mutex_);
        first = !closed_.exchange(true, std::memory_order_acq_rel);
        if (first) {
            if (!call_ || call_->pending != wanted_) {
                settle(wanted_, std::unexpected(error(Error::Code::closed)));
            }
            if (call_) {
                call_->context.TryCancel();
            }
        }
    }
    if (first) {
        core_->release();
        core_->wake();
    }
}

bool Publishing::wait(std::chrono::milliseconds timeout) const {
    if (Core::notifying()) {
        throw std::logic_error("Cannot wait inside a Comet callback");
    }
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return finished() && !notifying_; });
}

bool Publishing::target(const Binding& binding) const {
    return binding_ && state_.confirmed && binding_->endpoint == binding.endpoint && (binding.instance.empty() || binding.instance == state_.confirmed->instance);
}

void Publishing::report(Publisher::Phase phase, std::optional<Error> failure) {
    dirty_ = dirty_ || state_.phase != phase || state_.error.has_value() != failure.has_value() || (failure && state_.error && failure->code != state_.error->code);
    state_.phase = phase;
    state_.error = std::move(failure);
}

void Publishing::complete(const std::shared_ptr<Call>& call, const grpc::Status& status) noexcept {
    call->code = status.error_code();
    call->done.store(true, std::memory_order_release);
    call->owner->core_->wake();
}

void Publishing::consume(const std::shared_ptr<const Binding>& binding, Core::Time now) {

    if (!call_ || !call_->done.load(std::memory_order_acquire)) {
        return;
    }
    auto call = std::exchange(call_, {}); // 在整段检查期间保有回复/metadata, 最后 callback 引用独立存活.
    std::optional<Error> failure;
    std::optional<Publisher::Receipt> receipt;
    if (call->code != grpc::StatusCode::OK) {
        failure = Core::failure(grpc::Status(call->code, ""), call->context);
    } else if (!call->renewal) {
        const auto& reply = std::get<Publish>(call->message).reply;
        if (!astra::Scope::text(reply.instance(), 128) || (!call->binding->instance.empty() && reply.instance() != call->binding->instance) || reply.version() != call->pending->version) {
            failure = error(Error::Code::protocol, Error::Effect::unknown);
        } else {
            receipt = Publisher::Receipt{reply.instance(), reply.version()};
            settle(call->pending, *receipt); // 明确回执属于原调用, 不被较新期望覆盖.
        }
    }
    if (failure && !call->renewal) {
        settle(call->pending, std::unexpected(*failure));
    }
    const auto instance = receipt ? std::string_view(receipt->instance) : std::visit([](const auto& message) { return std::string_view(message.request.instance()); }, call->message); // 失败尝试没有成功回执, 仍按自己的消息类型取固定身份.
    const bool current = wanted_ == call->pending && (!binding || (binding->endpoint == call->binding->endpoint && (binding->instance.empty() || instance.empty() || binding->instance == instance)));
    if (closed()) {
        return;
    } // 结算完成后关闭状态绝不被晚到成功复活.

    if (failure && (failure->code == Error::Code::session || failure->code == Error::Code::transport || failure->code == Error::Code::instance)) {
        auto shared = *failure;
        if (shared.code == Error::Code::instance) {
            shared.code = Error::Code::transport;
        }
        core_->lost(call->binding, std::move(shared)); // 共享 Core 自行拒绝不是当前绑定的旧故障.
    }
    if (!current) {
        return;
    } // 被更新替换/换 Star 的结果只结算自己的 future.
    if (!failure) {
        if (receipt) {
            state_.confirmed = std::move(receipt);
            binding_ = call->binding;
            applied_ = true;
            dirty_ = true;
        }
        static_cast<void>(lifetime_.confirm(call->sent));
        failures_ = 0;
        retry_ = {};
        report(lifetime_.ready(Lifetime::now()) ? Publisher::Phase::ready : Publisher::Phase::uncertain);
        core_->recovered(call->binding);
    } else {
        rejected_ = failure->code == Error::Code::input || failure->code == Error::Code::version || failure->code == Error::Code::conflict || failure->code == Error::Code::obsolete || failure->code == Error::Code::limit || failure->code == Error::Code::protocol;
        if (failure->code == Error::Code::ended || failure->code == Error::Code::instance || !call->renewal) {
            applied_ = false;
        }
        failures_ = std::min(failures_ + 1, 32U);
        retry_ = now + core_->delay(failures_);
        report(rejected_ ? Publisher::Phase::failed : Publisher::Phase::uncertain, *failure);
    }
}

void Publishing::send(const std::shared_ptr<const Binding>& binding, Core::Time now, Lifetime::Time time, bool renewal) {

    auto call = std::make_shared<Call>(); // 只在有内容且允许发送时构造, 不为等待状态预分配 RPC.
    call->owner = shared_from_this();
    call->binding = binding;
    call->pending = wanted_;
    call->renewal = renewal;
    call->sent = time;
    if (!binding->session.empty()) {
        call->context.AddMetadata("comet-session-bin", binding->session);
    }
    const auto deadline = !renewal && wanted_->result ? wanted_->deadline : now + core_->options_.timeout;
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count();
    call->context.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_nanos(std::max(remaining, std::int64_t{0}), GPR_TIMESPAN)));
    if (renewal) {
        call->message.emplace<Renew>();
    }
    const auto size = std::visit([&](auto& message) {
        auto& request = message.request; // 两种请求共享固定字段, 正文只在完整 Publish 填写.
        request.set_instance(renewal ? state_.confirmed->instance : binding->instance);
        request.mutable_scope()->set_sector(scope_.sector);
        request.mutable_scope()->set_spectrum(scope_.spectrum);
        request.set_key(key_);
        request.set_version(wanted_->version);
        request.set_ttl_ms(static_cast<std::uint32_t>(ttl_.count()));
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(message)>, Publish>) {
            request.set_value(wanted_->value->data(), wanted_->value->size());
        }
        return request.SpaceUsedLong();
    },
                                 call->message);
    const auto bytes = sizeof(Call) + size + wanted_->value->size();
    if (!core_->resize(0, bytes)) {
        retry_ = now + std::chrono::milliseconds(50);
        return;
    }
    call->bytes = bytes;
    call->automatic = renewal || !wanted_->result;
    if (!core_->outgoing(renewal, call->automatic)) {
        retry_ = now + std::chrono::milliseconds(50);
        return;
    }
    call->claimed = true;
    // std::function 转换本身可能分配, 必须在发布 call_ 之前完成, 否则未开始的调用会永久占住在途槽.
    std::function<void(const grpc::Status&)> callback = [call](const grpc::Status& status) { complete(call, status); };
    call_ = call;
    std::visit([&](auto& message) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(message)>, Publish>) {
            binding->catalog->async()->Publish(&call->context, &message.request, &message.reply, std::move(callback));
        } else {
            binding->catalog->async()->Renew(&call->context, &message.request, &message.reply, std::move(callback));
        }
    },
               call->message);
}

void Publishing::notify(std::unique_lock<std::mutex>& lock) {
    if (!dirty_ || !changed_ || closed() || !core_->notification()) {
        return;
    }
    auto snapshot = state_; // 快照先完整准备, 回调期间可以重入 publish/close.
    dirty_ = false;
    notifying_ = true;
    lock.unlock();
    const bool previous = Core::notifying();
    Core::notify(true);
    try {
        changed_(std::move(snapshot));
    } catch (...) {
        core_->exception();
    }
    Core::notify(previous);
    lock.lock();
    notifying_ = false;
}

Core::Time Publishing::poll(Core::Time now, const std::shared_ptr<const Binding>& binding, const std::optional<Error>& blocked, bool closing) {

    if ((closing || core_->stopped()) && !closed()) {
        close();
    }
    std::unique_lock lock(mutex_);
    consume(binding, now);
    if (wanted_ && wanted_->result && (!call_ || call_->pending != wanted_) && now >= wanted_->deadline) {
        settle(wanted_, std::unexpected(error(Error::Code::timeout)));
    }
    if (closed()) {
        report(Publisher::Phase::closed);
        if (call_) {
            call_->context.TryCancel();
            return now + std::chrono::milliseconds(25);
        }
        settle(wanted_, std::unexpected(error(Error::Code::closed)));
        wanted_.reset();
        static_cast<void>(core_->resize(bytes_, 0));
        bytes_ = 0;
        finished_.store(true, std::memory_order_release);
        condition_.notify_all();
        return Core::Time::max();
    }
    const auto time = Lifetime::now();
    if (binding && binding_ && !target(*binding)) {
        binding_.reset();
        lifetime_.reset();
        applied_ = false;
        if (call_) {
            call_->context.TryCancel();
        }
    }
    if (rejected_) {
        report(Publisher::Phase::failed, state_.error);
    } else if (!binding) {
        report(blocked ? Publisher::Phase::failed : wanted_ ? Publisher::Phase::uncertain
                                                            : Publisher::Phase::waiting,
               blocked);
    } else if (!time) {
        report(Publisher::Phase::uncertain, error(Error::Code::clock));
    } else if (wanted_) {
        if (!call_ && now >= retry_ && (!applied_ || lifetime_.due(*time))) {
            send(binding, now, *time, applied_); // 有待发正文优先完整 Publish, 无需另发一次保活.
        }
        report(applied_ && lifetime_.ready(time) ? Publisher::Phase::ready : Publisher::Phase::uncertain, state_.error);
    }
    notify(lock);
    condition_.notify_all();

    auto next = now + std::chrono::seconds(1); // 本地预算检查, 不伪装成保活 RPC.
    if (wanted_ && wanted_->result && (!call_ || call_->pending != wanted_)) {
        next = std::min(next, wanted_->deadline);
    }
    if (wanted_ && !call_ && binding && time && !rejected_) {
        next = std::min(next, std::max(retry_, applied_ ? now + lifetime_.delay(*time) : now));
    }
    return std::max(next, now + std::chrono::milliseconds(1));
}
} // namespace comet::detail

namespace comet {
Publisher::Publisher(std::shared_ptr<detail::Publishing> publishing) : publishing_(std::move(publishing)) {}

Publisher::Publisher(Publisher&&) noexcept = default;

Publisher& Publisher::operator=(Publisher&& other) noexcept {
    if (this != &other) {
        close();
        publishing_ = std::move(other.publishing_);
    }
    return *this;
}

Publisher::~Publisher() {
    close();
}

Publisher::State Publisher::state() const {
    return publishing_ ? publishing_->state() : State{Phase::closed, {}, {}};
}

std::future<Result<Publisher::Receipt>> Publisher::publish(std::uint64_t version, std::vector<std::uint8_t> value, std::chrono::milliseconds timeout) {
    if (value.size() > 1024 * 1024) {
        return publish(version, Value{}, timeout);
    }
    return publish(version, std::make_shared<const std::vector<std::uint8_t>>(std::move(value)), timeout);
}

std::future<Result<Publisher::Receipt>> Publisher::publish(std::uint64_t version, std::span<const std::uint8_t> value, std::chrono::milliseconds timeout) {
    // 借用输入先验限, 拒绝超限时不复制潜在巨大的调用方缓冲.
    if (value.size() > 1024 * 1024) {
        return publish(version, Value{}, timeout);
    }
    return publish(version, std::vector<std::uint8_t>(value.begin(), value.end()), timeout);
}

std::future<Result<Publisher::Receipt>> Publisher::publish(std::uint64_t version, Value value, std::chrono::milliseconds timeout) {
    if (publishing_) {
        return publishing_->publish(version, std::move(value), timeout);
    }
    std::promise<Result<Receipt>> result;
    auto future = result.get_future();
    result.set_value(std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}}));
    return future;
}

void Publisher::close() noexcept {
    if (publishing_) {
        publishing_->close();
    }
}

bool Publisher::wait(std::chrono::milliseconds timeout) const {
    return !publishing_ || publishing_->wait(timeout);
}
} // namespace comet
