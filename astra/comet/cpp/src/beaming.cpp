#include "beaming.hpp"
#include "selection.hpp"
#include <astra/scope.hpp>
#include <grpc/support/time.h>
#include <utility>

namespace comet::detail {
Beaming::Beaming(std::shared_ptr<Core> core, Scope scope, Value attr, Value data, std::chrono::milliseconds ttl, Beacon::Options options) : core_(std::move(core)), scope_(std::move(scope)), attr_(std::move(attr)), ttl_(ttl), wanted_(std::make_shared<Pending>(nullptr, std::move(data))), lifetime_(ttl), changed_(std::move(options.changed)) {
    const auto bytes = attr_->size() + wanted_->data->size() + 1024; // 固定状态和载荷对象的保守计费, 在工厂接纳之前可整体回滚.
    if (!core_->resize(0, bytes)) {
        throw std::length_error("Beacon capacity unavailable");
    }
    bytes_ = bytes;
}

Beaming::~Beaming() {
    static_cast<void>(core_->resize(bytes_, 0));
}

Error Beaming::error(Error::Code code, Error::Effect effect) {
    return Error{code, effect, {}, {}, {}};
}

void Beaming::settle(const std::shared_ptr<Pending>& pending, Result<Beacon::Receipt> result) {
    if (pending && pending->result) {
        pending->result->set_value(std::move(result));
        if (pending->admission) {
            pending->admission->settled();
            pending->admission.reset();
        }
        pending->result.reset(); // 成功和失败都永久结束本次结果, 后台恢复不能重写.
    }
}

Beacon::State Beaming::state() const {
    const std::lock_guard lock(mutex_);
    auto result = state_; // 有界身份/诊断包装, 不复制 Attr/Data.
    if (closed() || core_->stopped()) {
        result.phase = Beacon::Phase::closed;
    } else if (result.phase == Beacon::Phase::ready && !lifetime_.ready(Lifetime::now())) {
        result.phase = Beacon::Phase::uncertain; // 休眠醒来即不再沿用旧确认, 无须等待控制轮先运行.
    }
    return result;
}

std::future<Result<Beacon::Receipt>> Beaming::update(Value data, std::chrono::milliseconds timeout) {

    auto pending = std::make_shared<Pending>(nullptr, std::move(data)); // 先准备结果通道, 失败不替换已接纳期望.
    pending->result.emplace();
    auto future = pending->result->get_future();
    if (!pending->data || pending->data->size() > 1024 * 1024 || timeout.count() <= 0 || timeout > std::chrono::minutes(1)) {
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
        const bool replacing = wanted_->result && wanted_->admission && (!updating_ || updating_->pending != wanted_); // 替代一个未发送调用可移交其现有额度.
        if (!replacing && !core_->admitting()) {
            settle(pending, std::unexpected(error(Error::Code::busy)));
            return future;
        }
        if (!replacing) {
            pending->admission = core_;
        } // 新调用先取得额度, 拒绝不修改原期望.
        const auto bytes = attr_->size() + pending->data->size() + 1024;
        if (!core_->resize(bytes_, bytes)) {
            settle(pending, std::unexpected(error(Error::Code::busy)));
            return future;
        }
        bytes_ = bytes;
        if (replacing) {
            pending->admission = std::move(wanted_->admission);
        } // 所有资源准备成功后才转移, 旧 future 仍明确结算为替代.
        if (!updating_ || updating_->pending != wanted_) {
            settle(wanted_, std::unexpected(error(Error::Code::obsolete))); // 只有未发出的 Update 可以被合并替代.
        }
        wanted_ = std::move(pending);
        applied_ = false;
        rejected_ = false;
        data_at_ = Core::Time{};
    }
    core_->wake();
    return future;
}

bool Beaming::closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

bool Beaming::finished() const noexcept {
    return finished_.load(std::memory_order_acquire);
}

void Beaming::close() noexcept {
    bool first; // 与通知开始使用同一短锁, 不反向持锁释放 Core 应用拥有数.
    {
        const std::lock_guard lock(mutex_);
        first = !closed_.exchange(true, std::memory_order_acq_rel);
        if (first) {
            retained_ = shared_from_this(); // public 析构后仍能完成有限 Remove, 不变成永久后台拥有者.
            close_at_ = std::chrono::steady_clock::now() + core_->options_.timeout;
        }
    }
    if (first) {
        core_->release();
        core_->wake();
    }
}

bool Beaming::wait(std::chrono::milliseconds timeout) const {
    if (Core::notifying()) {
        throw std::logic_error("Cannot wait inside a Comet callback");
    }
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return finished() && !notifying_; });
}

template <class Call>
std::shared_ptr<Call> Beaming::prepare(const std::shared_ptr<const Binding>& binding, Core::Time deadline, bool priority) {
    if (!binding || deadline <= std::chrono::steady_clock::now()) {
        return {};
    }
    auto call = std::make_shared<Call>(); // 构造成功之前不接纳任何实际 RPC 槽位.
    call->owner = shared_from_this();
    call->binding = binding;
    call->identity = state_.identity;
    call->priority = priority;
    if (!binding->session.empty()) {
        call->context.AddMetadata("comet-session-bin", binding->session);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count();
    call->context.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_nanos(std::max(remaining, std::int64_t{0}), GPR_TIMESPAN)));
    call->request.mutable_scope()->set_sector(scope_.sector);
    call->request.mutable_scope()->set_spectrum(scope_.spectrum);
    return call;
}

template <class Call>
bool Beaming::admit(const std::shared_ptr<Call>& call) {
    const auto bytes = sizeof(Call) + call->request.SpaceUsedLong() + (call->pending ? call->pending->data->size() : 0); // 在途旧期望与编码副本同时计费.
    if (!core_->resize(0, bytes)) {
        return false;
    }
    call->bytes = bytes;
    call->automatic = call->priority || !call->pending || !call->pending->result;
    if (!core_->outgoing(call->priority, call->automatic)) {
        return false;
    }
    call->claimed = true;
    return true;
}

template <class Call>
void Beaming::complete(const std::shared_ptr<Call>& call, const grpc::Status& status) noexcept {
    call->code = status.error_code(); // 不复制任意远端 message/details, 控制轮再解析白名单 metadata.
    call->done.store(true, std::memory_order_release);
    call->owner->core_->wake();
}

template <class Call>
Error Beaming::failure(const std::shared_ptr<Call>& call) {
    return Core::failure(grpc::Status(call->code, ""), call->context);
}

bool Beaming::same(const std::optional<Beacon::Identity>& identity) const {
    return identity && state_.identity && *identity == *state_.identity;
}

bool Beaming::target(const Binding& binding) const {
    return identity_ && identity_->endpoint == binding.endpoint && (binding.instance.empty() || (state_.identity && binding.instance == state_.identity->instance));
}

void Beaming::publish(Beacon::Phase phase, std::optional<Error> error) {
    dirty_ = dirty_ || state_.phase != phase || state_.error.has_value() != error.has_value() || (error && state_.error && error->code != state_.error->code);
    state_.phase = phase;
    state_.error = std::move(error);
}

void Beaming::forget() {
    if (updating_) {
        updating_->context.TryCancel();
    }
    if (renewing_) {
        renewing_->context.TryCancel();
    }
    state_.identity.reset();
    identity_.reset();
    lifetime_.reset();
    renewal_sent_.reset();
    update_ = renewal_ = 0;
    wanted_->order = 0;
    applied_ = false;
    dirty_ = true;
}

void Beaming::failed(const std::shared_ptr<const Binding>& binding, Error failure, bool creation, const std::shared_ptr<Pending>& pending) {

    if (failure.code == Error::Code::transport || failure.code == Error::Code::session || failure.code == Error::Code::instance) {
        auto shared = failure;
        if (shared.code == Error::Code::instance) {
            shared.code = Error::Code::transport; // 重新取得实际实例, 匿名配置也不会卡在旧身份.
        }
        core_->lost(binding, std::move(shared));
    }
    if (failure.code == Error::Code::ended || failure.code == Error::Code::instance) {
        forget();
    }
    const bool permanent = failure.code == Error::Code::input || failure.code == Error::Code::limit || failure.code == Error::Code::protocol || failure.code == Error::Code::obsolete || failure.code == Error::Code::conflict;
    if ((creation && permanent) || failure.code == Error::Code::protocol) {
        permanent_ = true;
    } else if (pending && pending == wanted_ && permanent) {
        rejected_ = true; // 此值不能靠反复 Create 绕过拒绝, 但原 UUID 仍正常续租.
    }
    publish(permanent_ ? Beacon::Phase::failed : state_.identity ? Beacon::Phase::uncertain
                                                                 : Beacon::Phase::waiting,
            std::move(failure));
}

void Beaming::consume(const std::shared_ptr<const Binding>& binding, Core::Time now) {

    const auto retry = [&] { failures_ = std::min(failures_ + 1, 32U); return now + core_->delay(failures_); }; // 只有完整确认才归零失败轮次.
    if (creating_ && creating_->done.load(std::memory_order_acquire)) {
        auto call = std::exchange(creating_, {}); // 当前调用拥有副本, 整段回复检查结束后才回收.
        const bool valid = call->code == grpc::StatusCode::OK && Selection::valid(call->reply.uuid()) && astra::Scope::text(call->reply.instance(), 128) && (call->binding->instance.empty() || call->binding->instance == call->reply.instance()) && call->reply.ttl_ms() == static_cast<std::uint32_t>(ttl_.count());
        if (valid && (closed() || !binding || (binding->endpoint == call->binding->endpoint && (binding->instance.empty() || binding->instance == call->reply.instance())))) {
            state_.identity = Beacon::Identity{scope_, call->reply.instance(), call->reply.uuid()};
            identity_ = call->binding;
            dirty_ = true;
            lifetime_.spread(std::hash<std::string>{}(call->reply.uuid()));
            static_cast<void>(lifetime_.confirm(call->sent));
            applied_ = wanted_ == call->pending && !wanted_->result; // 显式 Update 即便已被 Create 携带, 仍需真正的正 order 回执.
            failures_ = 0;
            publish(lifetime_.ready(Lifetime::now()) ? Beacon::Phase::ready : Beacon::Phase::uncertain);
            core_->recovered(call->binding);
        } else if (!valid && !closed()) {
            failed(call->binding, call->code == grpc::StatusCode::OK ? error(Error::Code::protocol, Error::Effect::unknown) : failure(call), true);
            create_at_ = retry();
        } // 已切换目标时旧 Create 成功只作已结束尝试, 未受管理的旧 UUID 交给原 TTL 清理.
    }
    if (updating_ && updating_->done.load(std::memory_order_acquire)) {
        auto call = std::exchange(updating_, {});
        if (call->code == grpc::StatusCode::OK && call->reply.order() == call->request.order()) {
            settle(call->pending, Beacon::Receipt{*call->identity, call->request.order()});
            if (same(call->identity)) {
                applied_ = wanted_ == call->pending;
                failures_ = 0;
                if (applied_) {
                    publish(lifetime_.ready(Lifetime::now()) ? Beacon::Phase::ready : Beacon::Phase::uncertain);
                }
            }
        } else {
            auto result = call->code == grpc::StatusCode::OK ? error(Error::Code::protocol, Error::Effect::unknown) : failure(call);
            settle(call->pending, std::unexpected(result));
            if (same(call->identity) && !closed()) {
                failed(call->binding, result, false, call->pending);
                data_at_ = retry();
            }
        }
    }
    if (renewing_ && renewing_->done.load(std::memory_order_acquire)) {
        auto call = std::exchange(renewing_, {});
        if (same(call->identity)) {
            if (call->code == grpc::StatusCode::OK && call->reply.order() == call->request.order()) {
                static_cast<void>(lifetime_.confirm(call->sent));
                renewal_sent_.reset();
                failures_ = 0;
                publish(lifetime_.ready(Lifetime::now()) ? Beacon::Phase::ready : Beacon::Phase::uncertain, rejected_ ? state_.error : std::optional<Error>{});
                core_->recovered(call->binding);
            } else if (!closed()) {
                failed(call->binding, call->code == grpc::StatusCode::OK ? error(Error::Code::protocol, Error::Effect::unknown) : failure(call), false);
                renew_at_ = retry();
            }
        }
    }
    if (removing_ && removing_->done.load(std::memory_order_acquire)) {
        removing_.reset(); // 尽力清理不重试也不把超时包装成远端已经删除.
    }
}

void Beaming::create(const std::shared_ptr<const Binding>& binding, Core::Time now, Lifetime::Time time) {

    auto call = prepare<Creating>(binding, now + core_->options_.timeout, false);
    if (!call) {
        return;
    }
    call->pending = wanted_;
    call->sent = time; // 在编码准备前记录只会使预算更保守, 不从回复时间起算.
    call->request.set_instance(binding->instance);
    call->request.set_attr(attr_->data(), attr_->size());
    call->request.set_data(wanted_->data->data(), wanted_->data->size());
    call->request.set_ttl_ms(static_cast<std::uint32_t>(ttl_.count()));
    if (!admit(call)) {
        create_at_ = now + std::chrono::milliseconds(50);
        return;
    }
    // 完成可分配的回调包装后才发布在途责任, 未开始的 RPC 不能留下无法完成的持有环.
    std::function<void(const grpc::Status&)> callback = [call](const grpc::Status& status) { complete(call, status); };
    creating_ = call;
    binding->ephemeris->async()->Create(&call->context, &call->request, &call->reply, std::move(callback));
}

void Beaming::renew(const std::shared_ptr<const Binding>& binding, Core::Time now, Lifetime::Time time) {

    // 原重试预算已经耗尽时用新 order 重新请求实际租约, 不从旧重复确认制造新 TTL.
    if (renewal_sent_ && (time < *renewal_sent_ || time - *renewal_sent_ >= ttl_.count())) {
        renewal_sent_.reset();
    }
    if (!renewal_sent_) {
        if (renewal_ == UINT64_MAX) {
            permanent_ = true;
            publish(Beacon::Phase::failed, error(Error::Code::limit));
            return;
        }
    }
    auto call = prepare<Renewing>(binding, now + core_->options_.timeout, true);
    if (!call) {
        return;
    }
    call->sent = renewal_sent_.value_or(time);
    call->request.set_instance(state_.identity->instance);
    call->request.set_uuid(state_.identity->uuid);
    call->request.set_order(renewal_sent_ ? renewal_ : renewal_ + 1);
    if (!admit(call)) {
        renew_at_ = now + std::chrono::milliseconds(25);
        return;
    }
    renewal_ = call->request.order();
    renewal_sent_ = call->sent; // 实际接纳发送之后才固定起点, 资源等待不冒充一次续租尝试.
    // 完成可分配的回调包装后才发布在途责任, 未开始的 RPC 不能留下无法完成的持有环.
    std::function<void(const grpc::Status&)> callback = [call](const grpc::Status& status) { complete(call, status); };
    renewing_ = call;
    binding->ephemeris->async()->Renew(&call->context, &call->request, &call->reply, std::move(callback));
}

void Beaming::send(const std::shared_ptr<const Binding>& binding, Core::Time now) {

    if (wanted_->order == 0) {
        if (update_ == UINT64_MAX) {
            rejected_ = true;
            settle(wanted_, std::unexpected(error(Error::Code::limit)));
            return;
        }
    }
    auto call = prepare<Updating>(binding, wanted_->result ? wanted_->deadline : now + core_->options_.timeout, false);
    if (!call) {
        return;
    }
    call->pending = wanted_;
    call->request.set_instance(state_.identity->instance);
    call->request.set_uuid(state_.identity->uuid);
    call->request.set_order(wanted_->order != 0 ? wanted_->order : update_ + 1);
    call->request.set_data(wanted_->data->data(), wanted_->data->size());
    if (!admit(call)) {
        data_at_ = now + std::chrono::milliseconds(50);
        return;
    }
    wanted_->order = call->request.order();
    update_ = wanted_->order; // 首次实际发送才分配 order, 重试保留同值.
    // 完成可分配的回调包装后才发布在途责任, 未开始的 RPC 不能留下无法完成的持有环.
    std::function<void(const grpc::Status&)> callback = [call](const grpc::Status& status) { complete(call, status); };
    updating_ = call;
    binding->ephemeris->async()->Update(&call->context, &call->request, &call->reply, std::move(callback));
}

void Beaming::remove(const std::shared_ptr<const Binding>& binding) {
    auto call = prepare<Removing>(binding, close_at_, true);
    if (!call) {
        removed_ = true;
        return;
    }
    call->request.set_instance(state_.identity->instance);
    call->request.set_uuid(state_.identity->uuid);
    if (!admit(call)) {
        return; // 保留额度暂满时仅在已经固定的关闭预算内等待.
    }
    removed_ = true;
    // 完成可分配的回调包装后才发布在途责任, 未开始的 RPC 不能留下无法完成的持有环.
    std::function<void(const grpc::Status&)> callback = [call](const grpc::Status& status) { complete(call, status); };
    removing_ = call;
    binding->ephemeris->async()->Remove(&call->context, &call->request, &call->reply, std::move(callback));
}

void Beaming::notify(std::unique_lock<std::mutex>& lock) {
    if (!dirty_ || !changed_ || closed() || !core_->notification()) {
        return;
    }
    auto snapshot = state_; // 快照在解锁之前稳定拥有, 用户回调不能借内部可写状态.
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

Core::Time Beaming::poll(Core::Time now, const std::shared_ptr<const Binding>& binding, const std::optional<Error>& blocked, bool closing) {

    if ((closing || core_->stopped()) && !closed()) {
        close(); // 此时尚未取得对象锁, 关闭不会反向取得自身锁.
    }
    std::unique_lock lock(mutex_);
    consume(binding, now);
    if (wanted_->result && (!updating_ || updating_->pending != wanted_) && now >= wanted_->deadline) {
        settle(wanted_, std::unexpected(error(Error::Code::timeout))); // 明确这个 Update 尚未发出, 最新期望仍保留.
    }
    if (closed()) {
        publish(Beacon::Phase::closed);
        if (creating_) {
            creating_->context.TryCancel();
        }
        if (updating_) {
            updating_->context.TryCancel();
        }
        if (renewing_) {
            renewing_->context.TryCancel();
        }
        if (!updating_ || updating_->pending != wanted_) {
            settle(wanted_, std::unexpected(error(Error::Code::closed)));
        }
        if (now >= close_at_) {
            removed_ = true;
            if (removing_) {
                removing_->context.TryCancel();
            }
        }
        if (!creating_ && !updating_ && !renewing_ && !removed_ && state_.identity) {
            remove(binding && target(*binding) ? binding : identity_);
        }
        if (!creating_ && !updating_ && !renewing_ && !removing_ && (removed_ || !state_.identity)) {
            wanted_->data.reset();
            attr_.reset();
            static_cast<void>(core_->resize(bytes_, 0));
            bytes_ = 0;
            finished_.store(true, std::memory_order_release);
            retained_.reset(); // Core 的本轮快照仍持有本对象, 不在这条语句自我销毁.
            condition_.notify_all();
            return Core::Time::max();
        }
        return now + std::chrono::milliseconds(25); // 截止已过时只等真实 callback, 不向过去时间连续安排 Alarm.
    }

    if (state_.identity && binding && !target(*binding)) {
        forget(); // 只有换 Star/实例才换 UUID, 同端点重新认证保留尚未结束的注册.
    }
    if (creating_ && binding && (creating_->binding->endpoint != binding->endpoint || (!binding->instance.empty() && !creating_->binding->instance.empty() && creating_->binding->instance != binding->instance))) {
        creating_->context.TryCancel();
    }
    const auto time = Lifetime::now(); // BOOTTIME 用于预算, RPC/退避仍使用参数 now 的 steady_clock.
    if (!time) {
        publish(state_.identity ? Beacon::Phase::uncertain : Beacon::Phase::waiting, error(Error::Code::clock));
    } else if (permanent_) {
        publish(Beacon::Phase::failed, state_.error);
    } else if (!binding) {
        publish(blocked ? Beacon::Phase::failed : state_.identity ? Beacon::Phase::uncertain
                                                                  : Beacon::Phase::waiting,
                blocked);
    } else {
        if (!state_.identity && !creating_ && !updating_ && !renewing_ && !rejected_ && now >= create_at_) {
            create(binding, now, *time);
        }
        if (state_.identity) {
            identity_ = binding; // 已经由 target 证明是同 Star, 关闭时采用最新有效 Session.
            if (!renewing_ && now >= renew_at_ && (renewal_sent_ || lifetime_.due(*time))) {
                renew(binding, now, *time); // 保活先接纳, 普通 Data 不能占用其保留容量.
            }
            if (!updating_ && !applied_ && !rejected_ && now >= data_at_) {
                send(binding, now);
            }
            publish(lifetime_.ready(time) ? Beacon::Phase::ready : Beacon::Phase::uncertain, state_.error);
        }
    }
    notify(lock);
    condition_.notify_all();

    // 阻塞在 RPC 时由其 callback 唤醒; 本地预算/显式未发期限仍有独立的有限唤醒点.
    auto next = now + std::chrono::seconds(1);
    if (wanted_->result && (!updating_ || updating_->pending != wanted_)) {
        next = std::min(next, wanted_->deadline);
    }
    if (!permanent_ && binding && time) {
        if (!state_.identity && !creating_ && !updating_ && !renewing_ && !rejected_) {
            next = std::min(next, create_at_);
        }
        if (state_.identity && !renewing_) {
            next = std::min(next, std::max(renew_at_, now + lifetime_.delay(*time)));
        }
        if (state_.identity && !updating_ && !applied_ && !rejected_) {
            next = std::min(next, data_at_);
        }
    }
    return std::max(next, now + std::chrono::milliseconds(1)); // 资源空窗至少让出调度线程, 不同步自旋.
}
} // namespace comet::detail

namespace comet {
Beacon::Beacon(std::shared_ptr<detail::Beaming> beaming) : beaming_(std::move(beaming)) {}

Beacon::Beacon(Beacon&&) noexcept = default;

Beacon& Beacon::operator=(Beacon&& other) noexcept {
    if (this != &other) {
        close();
        beaming_ = std::move(other.beaming_);
    }
    return *this;
}

Beacon::~Beacon() {
    close();
}

Beacon::State Beacon::state() const {
    return beaming_ ? beaming_->state() : State{Phase::closed, {}, {}};
}

std::future<Result<Beacon::Receipt>> Beacon::update(std::vector<std::uint8_t> data, std::chrono::milliseconds timeout) {
    if (data.size() > 1024 * 1024) {
        return update(Value{}, timeout);
    }
    return update(std::make_shared<const std::vector<std::uint8_t>>(std::move(data)), timeout);
}

std::future<Result<Beacon::Receipt>> Beacon::update(std::span<const std::uint8_t> data, std::chrono::milliseconds timeout) {
    // 和 Publisher 一样在复制 span 前拒绝超限, 不因重载不同绕过准备内存边界.
    if (data.size() > 1024 * 1024) {
        return update(Value{}, timeout);
    }
    return update(std::vector<std::uint8_t>(data.begin(), data.end()), timeout);
}

std::future<Result<Beacon::Receipt>> Beacon::update(Value data, std::chrono::milliseconds timeout) {
    if (beaming_) {
        return beaming_->update(std::move(data), timeout);
    }
    std::promise<Result<Receipt>> result; // 空句柄同样返回已经明确失败的标准 future.
    auto future = result.get_future();
    result.set_value(std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}}));
    return future;
}

void Beacon::close() noexcept {
    if (beaming_) {
        beaming_->close();
    }
}

bool Beacon::wait(std::chrono::milliseconds timeout) const {
    return !beaming_ || beaming_->wait(timeout);
}
} // namespace comet
