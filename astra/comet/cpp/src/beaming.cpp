#include "beaming.hpp"
#include "selection.hpp"
#include <algorithm>
#include <astra/scope.hpp>
#include <grpc/support/time.h>
#include <utility>

namespace comet::detail {
Beaming::Beaming(std::shared_ptr<Core> core, Scope scope, Value attr, Value data, std::chrono::milliseconds ttl, std::chrono::milliseconds beat, Beacon::Options options) : core_(std::move(core)), scope_(std::move(scope)), attr_(std::move(attr)), data_(std::move(data)), ttl_(ttl), beat_(beat), lifetime_(ttl), changed_(options.changed ? std::make_shared<Notice>(std::move(options.changed)) : nullptr) {
    const auto bytes = attr_->size() + data_->size() + 2048; // 固定身份、能力和共享载荷保守计费.
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

Beaming::Call::Call(Beaming& owner) : shutdown(owner.core_->shutdown_.get_token(), Cancel{&context}), cancellation(owner.cancellation_.get_token(), Cancel{&context}) {}

Beaming::Operation::~Operation() {

    if (claimed) {
        owner.core_->returning(false, false);
    }
    if (admitted) {
        owner.core_->settled();
    }
    static_cast<void>(owner.core_->resize(bytes, 0));
    {
        const std::lock_guard lock(owner.mutex_);
        owner.active_ = false;
        owner.active_binding_.reset();
        owner.cancellation_ = std::stop_source{std::nostopstate};
    }
    owner.condition_.notify_all();
    owner.core_->wake(&owner);
}

Result<void> Beaming::context(grpc::ClientContext& context, const std::shared_ptr<const Binding>& binding, Core::Time deadline) {

    if (closed() || core_->stopped()) {
        return std::unexpected(error(Error::Code::closed));
    }
    if (!core_->current(binding) || cancellation_.stop_requested()) {
        return std::unexpected(error(Error::Code::instance));
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) {
        return std::unexpected(error(Error::Code::timeout));
    }
    if (!binding->session.empty()) {
        context.AddMetadata("comet-session-bin", binding->session);
    }
    context.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_nanos(remaining, GPR_TIMESPAN)));
    return {};
}

Result<void> Beaming::begin(Operation& operation, Core::Time deadline, std::size_t bytes, std::shared_ptr<const Binding>& binding) {

    if (!core_->admitting()) {
        return std::unexpected(error(Error::Code::busy));
    }
    operation.admitted = true;
    if (!core_->resize(0, bytes)) {
        return std::unexpected(error(Error::Code::busy));
    }
    operation.bytes = bytes;
    auto acquired = core_->acquire(deadline, closed_);
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    binding = *acquired;
    {
        const std::lock_guard lock(mutex_);
        active_binding_ = binding;
    }
    if (!core_->outgoing(false, false)) {
        return std::unexpected(error(Error::Code::busy));
    }
    operation.claimed = true;
    return {};
}

Result<void> Beaming::initialize(Core::Time deadline) {

    std::stop_source cancellation; // 所有可分配的停止令牌先于 active_ 发布.
    {
        const std::lock_guard lock(mutex_);
        cancellation_ = std::move(cancellation);
        active_ = true;
    }
    Operation operation(*this);
    std::shared_ptr<const Binding> binding;
    if (auto admitted = begin(operation, deadline, 2 * attr_->size() + 3 * 1024 * 1024 + 8192, binding); !admitted) {
        return admitted;
    }
    proto::comet::v1::CreateRequest request;
    proto::comet::v1::CreateReply reply;
    Call call(*this);
    request.set_instance(binding->instance);
    request.mutable_scope()->set_sector(scope_.sector);
    request.mutable_scope()->set_spectrum(scope_.spectrum);
    request.set_attr(attr_->data(), attr_->size());
    request.set_data(data_->data(), data_->size());
    request.set_ttl_ms(static_cast<std::uint32_t>(ttl_.count()));
    request.set_generation(1);
    if (auto prepared = context(call.context, binding, deadline); !prepared) {
        return prepared;
    }
    const auto sent = Lifetime::now();
    if (!sent) {
        return std::unexpected(error(Error::Code::clock));
    }
    const auto status = binding->ephemeris->Create(&call.context, request, &reply); // 不依赖 Core 消费通知才能返回.
    const std::lock_guard lock(mutex_);
    if (!status.ok()) {
        auto failed = Core::failure(status, call.context);
        if (closed() || core_->stopped()) {
            failed.code = Error::Code::closed;
        }
        failure(binding, failed);
        return std::unexpected(std::move(failed));
    }
    auto result = install(reply, binding, 1, *sent, operation.bytes);
    if (!result) {
        return result;
    }
    if (closed() || core_->stopped()) {
        return std::unexpected(error(Error::Code::closed, Error::Effect::unknown));
    }
    if (!core_->current(binding)) {
        return std::unexpected(error(Error::Code::instance, Error::Effect::unknown));
    }
    initialized_ = true;
    sent_ = *sent;
    core_->wake(this);
    return {};
}

Result<void> Beaming::install(const proto::comet::v1::CreateReply& reply, const std::shared_ptr<const Binding>& binding, std::uint64_t generation, Lifetime::Time sent, std::size_t& reserved) {

    const bool valid = Selection::valid(reply.uuid()) && astra::Scope::text(reply.instance(), 128) && (binding->instance.empty() || binding->instance == reply.instance()) && reply.ttl_ms() == static_cast<std::uint32_t>(ttl_.count()) && reply.generation() == generation && reply.capability().size() == 32 && reply.data().size() <= 1024 * 1024 && reply.order() >= confirmed_ && (state_.identity || reply.order() == 0) && (!state_.identity || (reply.uuid() == state_.identity->uuid && reply.capability() == capability_));
    if (!valid || (reply.order() == confirmed_ && !std::ranges::equal(*data_, reply.data(), [](std::uint8_t left, char right) { return left == static_cast<std::uint8_t>(right); }))) {
        return std::unexpected(error(Error::Code::protocol, Error::Effect::unknown));
    }
    auto data = reply.order() == confirmed_ ? data_ : std::make_shared<const std::vector<std::uint8_t>>(reply.data().begin(), reply.data().end());
    Beacon::Identity identity{scope_, reply.instance(), reply.uuid()};
    auto capability = reply.capability(); // 字符串和载荷先准备, 不能部分修改已确认身份.
    const auto bytes = attr_->size() + data->size() + 2048;
    const auto growth = bytes > bytes_ ? bytes - bytes_ : 0; // 成功后的缓存增长由在途预留转移, 不二次抢额度.
    if (growth > reserved) {
        return std::unexpected(error(Error::Code::internal, Error::Effect::unknown));
    }
    if (growth) {
        reserved -= growth;
    } else {
        static_cast<void>(core_->resize(bytes_, bytes));
    }
    bytes_ = bytes;
    data_ = std::move(data);
    capability_.swap(capability);
    state_.identity = std::move(identity);
    binding_ = binding;
    confirmed_ = reply.order();
    issued_ = std::max(issued_, confirmed_);
    renewal_ = 0;
    static_cast<void>(lifetime_.confirm(sent));
    failures_ = 0;
    dirty_ = true;
    publish(lifetime_.ready(Lifetime::now()) ? Beacon::Phase::ready : Beacon::Phase::uncertain);
    return {};
}

Result<Beacon::Receipt> Beaming::update(Value data, std::chrono::milliseconds timeout, Core::Time started) {
    if (timeout.count() <= 0 || timeout > std::chrono::minutes(1)) {
        return std::unexpected(error(Error::Code::input));
    }
    return submit(std::move(data), started + timeout, {});
}

Result<Beacon::Receipt> Beaming::submit(Value data, Core::Time deadline, std::optional<std::uint64_t> sample) {

    if (!data || data->size() > 1024 * 1024) {
        return std::unexpected(error(Error::Code::input));
    }
    if (closed() || core_->stopped()) {
        return std::unexpected(error(Error::Code::closed));
    }
    if (Core::notifying()) {
        return std::unexpected(error(Error::Code::busy));
    }
    const auto owned = shared_from_this(); // 在途应用调用独立保活, 不依赖句柄继续存在.
    std::stop_source cancellation;
    std::shared_ptr<const Binding> registered;
    std::uint64_t generation{};
    Beacon::Receipt receipt;
    std::string capability;
    {
        const std::lock_guard lock(mutex_);
        if (closed() || core_->stopped()) {
            return std::unexpected(error(Error::Code::closed));
        }
        if (sample && *sample != sampling_generation_) {
            return std::unexpected(error(Error::Code::obsolete));
        }
        if (!initialized_ || active_ || !binding_ || creating_ || permanent_) {
            return std::unexpected(error(Error::Code::busy));
        }
        if (issued_ == UINT64_MAX || (!sample && sampling_generation_ == UINT64_MAX)) {
            permanent_ = true;
            publish(Beacon::Phase::failed, error(Error::Code::limit));
            core_->wake(this);
            return std::unexpected(error(Error::Code::limit));
        }
        registered = binding_;
        receipt = Beacon::Receipt{*state_.identity, issued_ + 1};
        capability = capability_;
        generation = generation_;
        cancellation_ = std::move(cancellation);
        active_ = true;
        if (!sample) {
            ++sampling_generation_;
            tick_ = Core::deadline(interval_);
        }
    }
    Operation operation(*this);
    std::shared_ptr<const Binding> binding;
    if (auto admitted = begin(operation, deadline, data->size() * 2 + 8192, binding); !admitted) {
        return std::unexpected(admitted.error());
    }
    if (binding->endpoint != registered->endpoint || (!binding->instance.empty() && binding->instance != receipt.identity.instance)) {
        return std::unexpected(error(Error::Code::instance));
    }
    proto::comet::v1::UpdateRequest request;
    proto::comet::v1::UpdateReply reply;
    Call call(*this);
    request.set_instance(receipt.identity.instance);
    request.mutable_scope()->set_sector(scope_.sector);
    request.mutable_scope()->set_spectrum(scope_.spectrum);
    request.set_uuid(receipt.identity.uuid);
    request.set_capability(capability);
    request.set_generation(generation);
    request.set_order(receipt.order);
    request.set_data(data->data(), data->size());
    if (auto prepared = context(call.context, binding, deadline); !prepared) {
        return std::unexpected(prepared.error());
    }
    const auto sent = Lifetime::now();
    if (!sent) {
        return std::unexpected(error(Error::Code::clock));
    }
    {
        const std::lock_guard lock(mutex_);
        issued_ = receipt.order; // 未知结果同样消耗版本, 失败内容不保存为恢复缓存.
        sent_ = *sent;
    }
    core_->wake(this);
    const auto status = binding->ephemeris->Update(&call.context, request, &reply);
    const std::lock_guard lock(mutex_);
    if (!status.ok()) {
        auto failed = Core::failure(status, call.context);
        if (closed() || core_->stopped()) {
            failed.code = Error::Code::closed;
        }
        failure(binding, failed);
        return std::unexpected(std::move(failed));
    }
    if (reply.order() != receipt.order) {
        auto failed = error(Error::Code::protocol, Error::Effect::unknown);
        failure(binding, failed);
        return std::unexpected(std::move(failed));
    }
    if (closed() || core_->stopped()) {
        return std::unexpected(error(Error::Code::closed, Error::Effect::unknown));
    }
    if (!core_->current(binding) || generation != generation_ || !binding_) {
        return std::unexpected(error(Error::Code::instance, Error::Effect::unknown));
    }
    const auto bytes = attr_->size() + data->size() + 2048;
    // 在途预留覆盖成功缓存增长, 不允许 RPC 成功后因另一个对象抢预算而丢失确认.
    const auto growth = bytes > bytes_ ? bytes - bytes_ : 0;
    if (growth) {
        operation.bytes -= growth;
        bytes_ += growth;
    } else {
        static_cast<void>(core_->resize(bytes_, bytes));
        bytes_ = bytes;
    }
    data_ = std::move(data);
    confirmed_ = receipt.order;
    static_cast<void>(lifetime_.confirm(*sent));
    publish(lifetime_.ready(Lifetime::now()) ? Beacon::Phase::ready : Beacon::Phase::uncertain);
    return receipt;
}

Beacon::State Beaming::state() const {
    const std::lock_guard lock(mutex_);
    auto state = state_;
    if (closed() || core_->stopped()) {
        state.phase = Beacon::Phase::closed;
    } else if (state.phase == Beacon::Phase::ready && !lifetime_.ready(Lifetime::now())) {
        state.phase = Beacon::Phase::uncertain;
    }
    return state;
}

Result<void> Beaming::changed(std::move_only_function<void(Beacon::State)> callback) {
    auto next = callback ? std::make_shared<Notice>(std::move(callback)) : nullptr;
    {
        const std::lock_guard lock(mutex_);
        if (closed() || core_->stopped()) {
            return std::unexpected(error(Error::Code::closed));
        }
        changed_.swap(next);
        dirty_ = true;
    }
    core_->wake(this);
    return {};
}

Result<void> Beaming::tick(std::chrono::milliseconds interval, std::move_only_function<Value()> callback) {

    if (interval.count() <= 0) {
        return std::unexpected(error(Error::Code::input));
    }
    auto next = callback ? std::make_shared<Sample>(std::move(callback)) : nullptr;
    {
        const std::lock_guard lock(mutex_);
        if (closed() || core_->stopped()) {
            return std::unexpected(error(Error::Code::closed));
        }
        if (sampling_generation_ == UINT64_MAX) {
            return std::unexpected(error(Error::Code::limit));
        }
        sampler_.swap(next);
        interval_ = interval;
        tick_ = Core::deadline(interval);
        ++sampling_generation_;
    }
    core_->wake(this);
    return {};
}

void Beaming::sample() noexcept {

    std::uint64_t generation{}; // 旧采样结束不能重置手动更新后的新期限.
    try {
        std::shared_ptr<Sample> callback;
        {
            const std::lock_guard lock(mutex_);
            if (!closed() && core_->notification()) {
                callback = sampler_;
                generation = sampling_generation_;
            }
        }
        if (callback) {
            Value data;
            const bool previous = Core::notifying();
            Core::notify(true);
            try {
                data = (*callback)();
            } catch (...) {
                callback.reset(); // 捕获析构仍属于回调, 禁止等待正在执行的采样自身.
                Core::notify(previous);
                throw;
            }
            callback.reset();
            Core::notify(previous);
            const auto result = submit(std::move(data), Core::deadline(core_->options_.timeout), generation);
            if (!result && result.error().code != Error::Code::obsolete && result.error().code != Error::Code::closed) {
                const std::lock_guard lock(mutex_);
                if (generation == sampling_generation_) {
                    publish(state_.phase, result.error());
                }
            }
        }
    } catch (...) {
        core_->exception();
        const std::lock_guard lock(mutex_);
        if (generation == sampling_generation_ && !closed()) {
            publish(state_.phase, error(Error::Code::internal));
        }
    }
    {
        const std::lock_guard lock(mutex_);
        sampling_ = false;
        if (generation == sampling_generation_) {
            tick_ = Core::deadline(interval_); // 跳过错过的拍数, 同对象不会并发执行两个采样器.
        }
    }
    condition_.notify_all();
    core_->wake(this);
}

void Beaming::close() noexcept {

    std::stop_source cancellation{std::nostopstate};
    bool first{};
    {
        const std::lock_guard lock(mutex_);
        first = !closed_.exchange(true, std::memory_order_acq_rel);
        if (first) {
            retained_ = shared_from_this();
            close_at_ = std::chrono::steady_clock::now() + core_->options_.timeout;
            cancellation = cancellation_;
        }
    }
    if (first) {
        cancellation.request_stop();
        core_->release();
        core_->wake(this);
    }
}

bool Beaming::wait(std::chrono::milliseconds timeout) const {
    if (Core::notifying()) {
        throw std::logic_error("Cannot wait inside a Comet callback");
    }
    std::unique_lock lock(mutex_);
    return condition_.wait_until(lock, Core::deadline(timeout), [this] { return finished(); });
}

void Beaming::publish(Beacon::Phase phase, std::optional<Error> error) {
    dirty_ = dirty_ || state_.phase != phase || state_.error.has_value() != error.has_value() || (error && state_.error && (error->code != state_.error->code || error->effect != state_.error->effect));
    state_.phase = phase;
    state_.error = std::move(error);
}

bool Beaming::target(const Binding& binding) const {
    return binding_ && binding_->endpoint == binding.endpoint && (binding.instance.empty() || (state_.identity && binding.instance == state_.identity->instance));
}

void Beaming::failure(const std::shared_ptr<const Binding>& binding, Error failed) {

    if (closed()) {
        return;
    }
    if (failed.code == Error::Code::transport || failed.code == Error::Code::session || failed.code == Error::Code::instance) {
        auto shared = failed;
        if (shared.code == Error::Code::instance) {
            shared.code = Error::Code::transport;
        }
        core_->lost(binding, std::move(shared));
    }
    if (failed.code == Error::Code::ended || failed.code == Error::Code::instance) {
        binding_.reset();
        lifetime_.reset();
    }
    permanent_ = failed.code == Error::Code::input || failed.code == Error::Code::limit || failed.code == Error::Code::protocol || failed.code == Error::Code::obsolete || failed.code == Error::Code::conflict;
    failures_ = std::min(failures_ + 1, 32U);
    retry_ = std::chrono::steady_clock::now() + core_->delay(failures_);
    publish(permanent_ ? Beacon::Phase::failed : binding_ ? Beacon::Phase::uncertain
                                                          : Beacon::Phase::recovering,
            std::move(failed));
}

template <class T>
std::shared_ptr<T> Beaming::prepare(const std::shared_ptr<const Binding>& binding, Core::Time deadline, bool priority) {
    if (!binding || deadline <= std::chrono::steady_clock::now()) {
        return {};
    }
    auto call = std::make_shared<T>();
    call->owner = shared_from_this();
    call->binding = binding;
    call->priority = priority;
    if (!binding->session.empty()) {
        call->context.AddMetadata("comet-session-bin", binding->session);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count();
    call->context.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_nanos(std::max(remaining, std::int64_t{}), GPR_TIMESPAN)));
    call->request.mutable_scope()->set_sector(scope_.sector);
    call->request.mutable_scope()->set_spectrum(scope_.spectrum);
    call->request.set_uuid(state_.identity->uuid);
    call->request.set_capability(capability_);
    call->request.set_generation(generation_);
    return call;
}

template <class T>
bool Beaming::admit(const std::shared_ptr<T>& call) {
    const auto bytes = sizeof(T) + call->request.SpaceUsedLong() + (std::same_as<T, Creating> ? 2 * 1024 * 1024 : 2048); // 恢复回执最大 Data 与候选解码都预留.
    if (!core_->resize(0, bytes)) {
        return false;
    }
    call->bytes = bytes;
    if (!core_->outgoing(call->priority, true)) {
        return false;
    }
    call->claimed = true;
    return true;
}

template <class T>
void Beaming::complete(const std::shared_ptr<T>& call, const grpc::Status& status) noexcept {
    call->code = status.error_code();
    call->done.store(true, std::memory_order_release);
    call->owner->core_->wake(call->owner.get());
}

void Beaming::restore(const std::shared_ptr<const Binding>& binding, Core::Time now, Lifetime::Time time) {

    if (generation_ == UINT64_MAX) {
        permanent_ = true;
        publish(Beacon::Phase::failed, error(Error::Code::limit));
        return;
    }
    auto call = prepare<Creating>(binding, now + core_->options_.timeout, false);
    if (!call) {
        return;
    }
    call->request.set_instance(binding->instance);
    call->request.set_generation(generation_ + 1);
    call->request.set_attr(attr_->data(), attr_->size());
    call->request.set_data(data_->data(), data_->size());
    call->request.set_order(confirmed_);
    call->request.set_ttl_ms(static_cast<std::uint32_t>(ttl_.count()));
    if (!admit(call)) {
        retry_ = now + std::chrono::milliseconds(50);
        return;
    }
    std::function<void(const grpc::Status&)> callback = [call](const grpc::Status& status) { complete(call, status); };
    generation_ = call->request.generation();
    call->sent = time;
    sent_ = time;
    creating_ = call;
    publish(Beacon::Phase::recovering);
    binding->ephemeris->async()->Create(&call->context, &call->request, &call->reply, std::move(callback));
}

void Beaming::renew(const std::shared_ptr<const Binding>& binding, Core::Time now, Lifetime::Time time) {

    if (renewal_ == UINT64_MAX) {
        permanent_ = true;
        publish(Beacon::Phase::failed, error(Error::Code::limit));
        return;
    }
    auto call = prepare<Renewing>(binding, now + core_->options_.timeout, true);
    if (!call) {
        return;
    }
    call->request.set_instance(state_.identity->instance);
    call->request.set_order(renewal_ + 1);
    if (!admit(call)) {
        retry_ = now + std::chrono::milliseconds(25);
        return;
    }
    std::function<void(const grpc::Status&)> callback = [call](const grpc::Status& status) { complete(call, status); };
    renewal_ = call->request.order();
    call->sent = time;
    sent_ = time;
    renewing_ = call;
    binding->ephemeris->async()->Renew(&call->context, &call->request, &call->reply, std::move(callback));
}

void Beaming::remove(const std::shared_ptr<const Binding>& binding) {
    auto call = prepare<Removing>(binding, close_at_, true);
    if (!call) {
        removed_ = true;
        return;
    }
    call->request.set_instance(state_.identity->instance);
    if (!admit(call)) {
        return;
    }
    std::function<void(const grpc::Status&)> callback = [call](const grpc::Status& status) { complete(call, status); };
    removed_ = true;
    removing_ = call;
    binding->ephemeris->async()->Remove(&call->context, &call->request, &call->reply, std::move(callback));
}

void Beaming::consume(const std::shared_ptr<const Binding>& binding) {

    if (creating_ && creating_->done.load(std::memory_order_acquire)) {
        auto call = std::exchange(creating_, {});
        if (call->code == grpc::StatusCode::OK && (closed() || (binding && call->binding->endpoint == binding->endpoint && (binding->instance.empty() || binding->instance == call->reply.instance())))) {
            auto result = install(call->reply, call->binding, call->request.generation(), call->sent, call->bytes);
            if (!result) {
                failure(call->binding, result.error());
            }
        } else if (!closed() && call->code != grpc::StatusCode::OK && core_->current(call->binding)) {
            failure(call->binding, Core::failure(grpc::Status(call->code, ""), call->context));
        }
    }
    if (renewing_ && renewing_->done.load(std::memory_order_acquire)) {
        auto call = std::exchange(renewing_, {});
        if (!closed() && binding_ && call->request.generation() == generation_ && target(*call->binding)) {
            if (call->code == grpc::StatusCode::OK && call->reply.order() == call->request.order()) {
                static_cast<void>(lifetime_.confirm(call->sent));
                failures_ = 0;
                publish(lifetime_.ready(Lifetime::now()) ? Beacon::Phase::ready : Beacon::Phase::uncertain);
            } else {
                failure(call->binding, call->code == grpc::StatusCode::OK ? error(Error::Code::protocol, Error::Effect::unknown) : Core::failure(grpc::Status(call->code, ""), call->context));
            }
        }
    }
    if (removing_ && removing_->done.load(std::memory_order_acquire)) {
        removing_.reset();
    }
}

Core::Time Beaming::poll(Core::Time now, const std::shared_ptr<const Binding>& binding, const std::optional<Error>& blocked, bool closing) {

    if ((closing || core_->stopped()) && !closed()) {
        close();
    }
    std::unique_lock lock(mutex_);
    consume(binding);
    if (active_binding_ && !core_->current(active_binding_)) {
        auto cancellation = cancellation_;
        lock.unlock();
        cancellation.request_stop();
        lock.lock(); // 停止回调不在对象锁内调用.
    }
    if (closed()) {
        if (creating_) {
            creating_->context.TryCancel();
        }
        if (renewing_) {
            renewing_->context.TryCancel();
        }
        if (now >= close_at_) {
            removed_ = true;
            if (removing_) {
                removing_->context.TryCancel();
            }
        }
        if (!active_ && !creating_ && !renewing_ && !removed_ && state_.identity) {
            remove(binding && target(*binding) ? binding : binding_);
        }
        if (!active_ && !creating_ && !renewing_ && !removing_ && !sampling_ && !notifying_ && (removed_ || !state_.identity)) {
            publish(Beacon::Phase::closed);
            auto notice = std::move(changed_); // 用户捕获的析构也可能重入, 先移出再解锁释放.
            auto sample = std::move(sampler_);
            lock.unlock();
            notice.reset();
            sample.reset();
            lock.lock();
            finished_.store(true, std::memory_order_release);
            retained_.reset();
            condition_.notify_all();
            return Core::Time::max();
        }
        return now + std::chrono::milliseconds(25);
    }
    if (!initialized_) {
        return now + std::chrono::milliseconds(100);
    }
    if (binding_ && binding && !target(*binding)) {
        binding_.reset();
        lifetime_.reset();
        if (renewing_) {
            renewing_->context.TryCancel();
        }
        publish(Beacon::Phase::recovering);
    }
    if (creating_ && binding && (creating_->binding->endpoint != binding->endpoint || (!binding->instance.empty() && !creating_->binding->instance.empty() && creating_->binding->instance != binding->instance))) {
        creating_->context.TryCancel();
    }
    const auto time = Lifetime::now();
    if (!time) {
        publish(Beacon::Phase::uncertain, error(Error::Code::clock));
    } else if (permanent_) {
        publish(Beacon::Phase::failed, state_.error);
    } else if (!binding) {
        publish(blocked ? Beacon::Phase::failed : Beacon::Phase::uncertain, blocked);
    } else if (!binding_) {
        if (!active_ && !creating_ && !renewing_ && now >= retry_) {
            restore(binding, now, *time);
        }
    } else {
        binding_ = binding;
        const bool due = !sent_ || *time < *sent_ || *time - *sent_ >= beat_.count();
        if (!renewing_ && !creating_ && due && now >= retry_) {
            renew(binding, now, *time);
        }
        if (sampler_ && !sampling_ && !active_ && now >= tick_) {
            auto owner = shared_from_this();
            if (core_->sample([owner] { owner->sample(); })) {
                sampling_ = true;
            } else {
                tick_ = now + std::chrono::milliseconds(50);
            }
        }
        if (!lifetime_.ready(time) && state_.phase == Beacon::Phase::ready) {
            publish(Beacon::Phase::uncertain);
        }
    }
    if (dirty_ && changed_ && !closed() && core_->notification()) {
        auto callback = changed_;
        auto notification = state_;
        dirty_ = false;
        notifying_ = true;
        lock.unlock();
        const bool previous = Core::notifying();
        Core::notify(true);
        try {
            (*callback)(std::move(notification));
        } catch (...) {
            core_->exception();
        }
        callback.reset(); // 回调可在执行中解除自己; 最后一个业务捕获必须在对象锁外释放.
        Core::notify(previous);
        lock.lock();
        notifying_ = false;
    }
    condition_.notify_all();
    auto next = now + std::chrono::seconds(1); // 保证 suspend 返回后观察 BOOTTIME, 不按墙钟延长租约.
    if (binding && time && !permanent_) {
        if (!binding_ && !active_ && !creating_ && !renewing_) {
            next = std::min(next, retry_);
        }
        if (binding_ && !renewing_) {
            const auto delay = sent_ && *time >= *sent_ ? std::max(Lifetime::Time{}, beat_.count() - (*time - *sent_)) : 0;
            next = std::min(next, std::max(retry_, now + std::chrono::milliseconds(delay)));
        }
        if (sampler_ && binding_ && !sampling_ && !active_) {
            next = std::min(next, tick_);
        }
    }
    return std::max(next, now + std::chrono::milliseconds(1));
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

Result<Beacon::Receipt> Beacon::update(std::vector<std::uint8_t> data, std::chrono::milliseconds timeout) {
    const auto started = std::chrono::steady_clock::now(); // 编码/取得共享拥有权也消耗本次总期限.
    if (!beaming_) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    auto owned = data.size() <= 1024 * 1024 ? std::make_shared<const std::vector<std::uint8_t>>(std::move(data)) : Value{};
    return beaming_->update(std::move(owned), timeout, started);
}

Result<Beacon::Receipt> Beacon::update(std::span<const std::uint8_t> data, std::chrono::milliseconds timeout) {
    const auto started = std::chrono::steady_clock::now(); // 借用输入只在本调用内复制, 之后无外部可写别名.
    if (!beaming_) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    auto owned = data.size() <= 1024 * 1024 ? std::make_shared<const std::vector<std::uint8_t>>(data.begin(), data.end()) : Value{};
    return beaming_->update(std::move(owned), timeout, started);
}

Result<Beacon::Receipt> Beacon::update(Value data, std::chrono::milliseconds timeout) {
    return beaming_ ? beaming_->update(std::move(data), timeout, std::chrono::steady_clock::now()) : Result<Receipt>(std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}}));
}

Result<void> Beacon::tick(std::chrono::milliseconds interval, std::move_only_function<Value()> callback) {
    return beaming_ ? beaming_->tick(interval, std::move(callback)) : Result<void>(std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}}));
}

Result<void> Beacon::changed(std::move_only_function<void(State)> callback) {
    return beaming_ ? beaming_->changed(std::move(callback)) : Result<void>(std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}}));
}

void Beacon::destroy() noexcept {
    close();
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
