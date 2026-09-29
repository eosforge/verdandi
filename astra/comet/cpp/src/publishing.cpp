#include "publishing.hpp"
#include <algorithm>
#include <astra/scope.hpp>
#include <grpc/support/time.h>
#include <limits>
#include <stdexcept>

namespace comet::detail {
Publishing::Publishing(std::shared_ptr<Core> core, Scope scope) : core_(std::move(core)), scope_(std::move(scope)) {}

Publishing::~Publishing() {
    discard();
}

Publishing::Call::Call(Publishing& owner) : shutdown(owner.core_->shutdown_.get_token(), Cancel{&context}), cancellation(owner.cancellation_.get_token(), Cancel{&context}) {}

void Publishing::discard() noexcept {
    std::vector<std::string>{}.swap(keys_);
    std::string{}.swap(instance_);
    confirmed_.reset();
    static_cast<void>(core_->resize(bytes_, 0));
    bytes_ = 0;
}

Error Publishing::error(Error::Code code, Error::Effect effect) {
    return Error{code, effect, {}, {}, {}};
}

Publishing::Operation::~Operation() {

    // 当前栈上 RPC 已返回才回收, 不以 TryCancel 作为网络完成依据.
    if (claimed)
        owner.core_->returning(false, false);
    if (admitted)
        owner.core_->settled();
    static_cast<void>(owner.core_->resize(bytes, 0));
    {
        const std::lock_guard lock(owner.mutex_);
        if (!confirmed)
            owner.confirmed_.reset();
        if (owner.closed())
            owner.discard();
        owner.cancellation_ = std::stop_source{std::nostopstate};
        owner.binding_.reset();
        owner.active_ = false;
    }
    owner.condition_.notify_all();
    owner.core_->wake(&owner);
}

Result<void> Publishing::prepare(grpc::ClientContext& context, const std::shared_ptr<const Binding>& binding, Core::Time deadline) {

    // 查询、提交和修复共用一个截止; 新步骤不能因换实例自动改投.
    if (closed() || core_->stopped())
        return std::unexpected(error(Error::Code::closed));
    if (!core_->current(binding))
        return std::unexpected(error(Error::Code::instance));
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count(); // 剩余纳秒, 不重置预算.
    if (remaining <= 0)
        return std::unexpected(error(Error::Code::timeout));
    context.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_nanos(remaining, GPR_TIMESPAN)));
    if (!binding->session.empty())
        context.AddMetadata("comet-session-bin", binding->session);

    if (closed() || core_->stopped())
        return std::unexpected(error(Error::Code::closed));
    if (cancellation_.stop_requested())
        return std::unexpected(error(Error::Code::instance));
    return {};
}

Result<std::uint64_t> Publishing::query(const std::shared_ptr<const Binding>& binding, const std::vector<Publisher::Entry>& entries, Core::Time deadline, std::string& instance) {

    // 请求只携带相关 Key, 不拉取 Scope 全表或正文; 查询无业务副作用.
    proto::comet::v1::CatalogQueryRequest request;
    proto::comet::v1::CatalogQueryReply reply;
    Call call(*this); // 已停止的令牌立即取消, 回调在 context 析构前注销.
    request.set_instance(instance);
    request.mutable_scope()->set_sector(scope_.sector);
    request.mutable_scope()->set_spectrum(scope_.spectrum);
    for (const auto& entry : entries)
        request.add_keys(entry.key);
    if (auto prepared = prepare(call.context, binding, deadline); !prepared)
        return std::unexpected(prepared.error());
    const auto status = binding->catalog->Query(&call.context, request, &reply); // 阻塞应用线程, 不占 Core 控制轮.
    if (!status.ok()) {
        auto failure = Core::failure(status, call.context); // 查询失败不会提交原业务批次.
        failure.effect = Error::Effect::unapplied;
        if (closed() || core_->stopped())
            failure.code = Error::Code::closed;
        if (failure.code == Error::Code::session || failure.code == Error::Code::transport || failure.code == Error::Code::instance)
            core_->lost(binding, failure);
        return std::unexpected(std::move(failure));
    }

    // 不接受缺项、错序、额外 Key 或实例漂移, 防止错误基线驱动后续写入.
    if (!astra::Scope::text(reply.instance(), 128) || (!instance.empty() && reply.instance() != instance) || reply.entries_size() != static_cast<int>(entries.size()))
        return std::unexpected(error(Error::Code::protocol));
    std::uint64_t maximum = issued_; // 未知结果已消耗的版本也不能复用.
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& row = reply.entries(static_cast<int>(index)); // 响应必须与请求逐项对应.
        if (row.key() != entries[index].key)
            return std::unexpected(error(Error::Code::protocol));
        maximum = std::max(maximum, row.version());
    }
    if (maximum == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(error(Error::Code::limit));
    instance = reply.instance();
    return maximum + 1;
}

Result<Publisher::Receipt> Publishing::update(std::vector<Publisher::Entry> entries, std::chrono::milliseconds ttl, std::chrono::milliseconds timeout) {

    // 非法超大 timeout 不参与加法, 合法调用从本地准备开始共用唯一截止.
    if (timeout.count() <= 0 || timeout > std::chrono::minutes(1) || ttl < std::chrono::seconds(1) || ttl > std::chrono::minutes(10) || entries.empty() || entries.size() > 128)
        return std::unexpected(error(Error::Code::input));
    const auto deadline = std::chrono::steady_clock::now() + timeout; // 包括认证和查询在内的绝对单调截止.
    if (closed() || core_->stopped())
        return std::unexpected(error(Error::Code::closed));
    if (Core::notifying())
        return std::unexpected(error(Error::Code::busy));
    std::size_t bytes{}; // 单项先限长, 再累加有界键与正文, 避免溢出.
    std::size_t names{}; // Key 还会出现在查询和本地基线中, 单独计入编码预算.
    for (const auto& entry : entries) {
        if (!astra::Scope::text(entry.key, 1024) || !entry.value || entry.value->size() > 1024 * 1024)
            return std::unexpected(error(Error::Code::input));
        names += entry.key.size();
        bytes += entry.key.size() + entry.value->size();
        if (bytes > 1024 * 1024)
            return std::unexpected(error(Error::Code::input));
    }

    // 消费调用方移入的容器, 原地排序同时规范化 Key 集合并消除逐 Key 树节点分配.
    std::ranges::sort(entries, {}, &Publisher::Entry::key);
    if (std::ranges::adjacent_find(entries, {}, &Publisher::Entry::key) != entries.end())
        return std::unexpected(error(Error::Code::input));

    // 同对象只接纳一个同步调用, 不替换、合并或排队; 强引用跨越整个实际网络寿命.
    const auto owned = shared_from_this();
    std::stop_source cancellation; // 可分配步骤先于 active_ 发布, 异常不能遗留忙碌标志.
    {
        const std::lock_guard lock(mutex_);
        if (closed() || core_->stopped())
            return std::unexpected(error(Error::Code::closed));
        if (active_)
            return std::unexpected(error(Error::Code::busy));
        cancellation_ = std::move(cancellation);
        active_ = true;
    }
    Operation operation(*this); // 从这里开始所有退出路径精确归还已取得的资源.
    if (!core_->admitting())
        return std::unexpected(error(Error::Code::busy));
    operation.admitted = true;
    const auto reserved = 3 * bytes + 3 * names + entries.size() * 768 + 8192; // 正文、请求/回复和下一份 Key 基线的有界保守预算.
    if (!core_->resize(0, reserved))
        return std::unexpected(error(Error::Code::busy));
    operation.bytes = reserved;
    auto selected = core_->acquire(deadline, closed_); // 初次认证也消耗原截止.
    if (!selected)
        return std::unexpected(selected.error());
    const auto binding = *selected; // 整个调用固定目标, 切换只影响后续显式调用.
    {
        const std::lock_guard lock(mutex_);
        binding_ = binding;
    }
    if (!core_->outgoing(false, false))
        return std::unexpected(error(Error::Code::busy));
    operation.claimed = true;

    // 同一绑定和精确 Key 集合可以沿用已确认水位; 换目标、换集合或失败后重新查询.
    const bool initialized = confirmed_.lock() == binding && std::ranges::equal(keys_, entries, {}, {}, &Publisher::Entry::key);
    std::string instance = initialized ? instance_ : binding->instance; // 匿名接入由首次 Query 确认实例.
    std::vector<std::string> keys;                                      // 只在换集合时准备下一份 Key 基线, 不保留业务正文.
    if (!initialized) {
        keys.reserve(entries.size());
        for (const auto& entry : entries)
            keys.push_back(entry.key);
    }

    // 正文只编码一次, 唯一一次冲突修复仅替换版本; 不随 RPC 重试再复制整个批次.
    proto::comet::v1::PublishRequest request;
    request.mutable_scope()->set_sector(scope_.sector);
    request.mutable_scope()->set_spectrum(scope_.spectrum);
    request.set_ttl_ms(static_cast<std::uint32_t>(ttl.count()));
    for (const auto& entry : entries) {
        auto* row = request.add_entries(); // 单一原子批次, 不拆成逐 Key 写入.
        row->set_key(entry.key);
        row->set_value(entry.value->data(), entry.value->size());
    }
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        std::uint64_t version{}; // 仅在明确整批未提交的版本冲突后允许修复一次.
        if (!initialized || attempt != 0) {
            auto queried = query(binding, entries, deadline, instance);
            if (!queried)
                return std::unexpected(queried.error());
            version = *queried;
        } else {
            if (issued_ == std::numeric_limits<std::uint64_t>::max())
                return std::unexpected(error(Error::Code::limit));
            version = issued_ + 1;
        }
        Publisher::Receipt receipt{instance, version}; // 可能分配的回执文本在提交前准备.
        std::size_t retained{};                        // 提交前核对下一份元数据的空间, 不依赖 reserve 的精确容量.
        if (!initialized) {
            retained = keys.capacity() * sizeof(std::string) + instance.capacity() + 1;
            for (const auto& key : keys)
                retained += key.capacity() + 1;
            if (retained > operation.bytes)
                return std::unexpected(error(Error::Code::busy));
        }
        request.set_instance(instance);
        request.set_version(version);
        proto::comet::v1::PublishReply reply;
        Call call(*this); // 两个停止源均直接作用于本 RPC, 不借用对象内裸指针.
        if (auto prepared = prepare(call.context, binding, deadline); !prepared)
            return std::unexpected(prepared.error());
        issued_ = version; // 发送前消耗, 未知结果不能换 Data 重用.
        const auto status = binding->catalog->Publish(&call.context, request, &reply);
        if (status.ok()) {
            if (reply.instance() != instance || reply.version() != version)
                return std::unexpected(error(Error::Code::protocol, Error::Effect::unknown));
            if (!initialized) {
                discard();
                keys_ = std::move(keys);
                instance_ = std::move(instance);
                bytes_ = retained;
                operation.bytes -= bytes_; // 已预留预算转移给有界元数据, 不在提交成功后再争抢额度.
            }
            confirmed_ = binding;
            operation.confirmed = true;
            core_->recovered(binding);
            return receipt;
        }
        auto failure = Core::failure(status, call.context); // 只信任结构化错误的 unapplied 证据.
        if (closed() || core_->stopped())
            failure.code = Error::Code::closed; // 本地取消不触发共享 Client 的网络故障切换.
        if (attempt == 0 && failure.code == Error::Code::version && failure.effect == Error::Effect::unapplied)
            continue;
        if (failure.code == Error::Code::session || failure.code == Error::Code::transport || failure.code == Error::Code::instance)
            core_->lost(binding, failure);
        return std::unexpected(std::move(failure));
    }
    return std::unexpected(error(Error::Code::internal));
}

bool Publishing::closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

bool Publishing::finished() const noexcept {
    const std::lock_guard lock(mutex_);
    return closed() && !active_;
}

void Publishing::close() noexcept {

    bool first;                                      // 应用拥有数只减少一次, 不在持对象锁时进入 Core.
    std::stop_source cancellation{std::nostopstate}; // 快照只指向本次操作, 不持对象锁触发回调.
    {
        const std::lock_guard lock(mutex_);
        first = !closed_.exchange(true, std::memory_order_acq_rel);
        cancellation = cancellation_;
        if (!active_)
            discard();
    }
    cancellation.request_stop();
    if (first)
        core_->release();
    condition_.notify_all();
    core_->wake(this);
}

bool Publishing::wait(std::chrono::milliseconds timeout) const {
    if (Core::notifying())
        throw std::logic_error("Cannot wait inside an SDK callback");
    std::unique_lock lock(mutex_); // 只等待实际调用排空, 不隐式关闭.
    return condition_.wait_until(lock, Core::deadline(timeout), [this] { return closed() && !active_; });
}

Core::Time Publishing::poll(Core::Time, const std::shared_ptr<const Binding>&, const std::optional<Error>&, bool closing) {

    if (closing)
        close();
    std::shared_ptr<const Binding> binding; // 只检查当前实际调用, 不采用控制轮更早取得的绑定快照.
    std::stop_source cancellation{std::nostopstate};
    {
        const std::lock_guard lock(mutex_);
        binding = binding_;
        cancellation = cancellation_;
    }
    if (binding && !core_->current(binding))
        cancellation.request_stop(); // 不持对象锁进入 Core; 即使新调用已开始, 也只取消旧操作.
    return Core::Time::max();
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

Result<Publisher::Receipt> Publisher::update(std::vector<Entry> entries, std::chrono::milliseconds ttl, std::chrono::milliseconds timeout) {
    return publishing_ ? publishing_->update(std::move(entries), ttl, timeout) : Result<Receipt>(std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}}));
}

Result<Publisher::Receipt> Publisher::update(std::string key, std::vector<std::uint8_t> data, std::chrono::milliseconds ttl, std::chrono::milliseconds timeout) {
    std::vector<Entry> entries; // 避免 initializer_list 将已经移入的 Key 再复制一遍.
    entries.push_back({std::move(key), std::make_shared<const std::vector<std::uint8_t>>(std::move(data))});
    return update(std::move(entries), ttl, timeout);
}

void Publisher::close() noexcept {
    if (publishing_)
        publishing_->close();
}

bool Publisher::wait(std::chrono::milliseconds timeout) const {
    return !publishing_ || publishing_->wait(timeout);
}
} // namespace comet
