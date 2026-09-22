#pragma once
#include "core.hpp"
#include <algorithm>

namespace comet::detail {
// 一个应用 Reader 的私有状态, 网络 RPC 可以延长清理寿命, 不能延长应用自动订阅意图.
template <class Policy>
class Watching : public Activity, public std::enable_shared_from_this<Watching<Policy>> {
    using API = typename Policy::API;         // 对应公开读取句柄, 类型不会进入网络协议.
    using View = typename API::View;          // 完整安装后交付的只读视图.
    using State = typename API::State;        // 各读取域共用相同生命周期状态.
    using Options = typename API::Options;    // 固定范围的本地容量和观察者.
    using Reply = typename Policy::Reply;     // 生成回复只在私有实现可见.
    using Service = typename Policy::Service; // 生成的专用 Watch 服务.
public:
    // Core 已完成工厂参数/额度接纳后创建, 构造自身不发 RPC 或调用用户观察者.
    Watching(std::shared_ptr<Core> core, Scope scope, std::string target, Options options);
    ~Watching();                                        // 原生投影归还受控计费, 所有 RPC/观察者须在最后引用释放前已经完成.
    View load() const;                                  // 加短锁取得当前状态, 不等待网络.
    void close() noexcept;                              // 只在第一次关闭时归还应用拥有数, 在途仍等待 OnDone.
    bool wait(std::chrono::milliseconds timeout) const; // 禁止在 SDK 观察者里阻塞.
    bool closed() const noexcept override;              // Core 工厂计数使用原子标志, 不反向取得 Watching 锁.
    bool finished() const noexcept override;            // 已关闭且完全清理后可以从 Core 弱目录移除, 用户仍可保存最后 View.

    bool streaming() const noexcept override {
        return true;
    } // Watch 不挤占 Beacon 保活额度.

    // 一个 Core 控制轮顺序调用; binding/error 是已经拿出 Core 锁的固定快照, 返回下一实际调度时刻.
    Core::Time poll(Core::Time now, const std::shared_ptr<const Binding>& binding, const std::optional<Error>& error, bool closing) override;

private:
    bool cleaned_{}; // 已处理本地 close 且准备存储归还, 与实际 OnDone 一起构成 wait 完成条件.
    class Stream;    // 有一个 read 在途的真实 Callback RPC, 包括取消后的实际清理状态.
    // 单次状态发布, 不在这个方法里执行用户代码, caller 按通知边界解锁之后调用.
    bool publish(State state, std::optional<Error> error = {});
    // 每次新流冻结请求范围及恢复位置, 旧 Stream 未 OnDone 时不能创建下一条.
    void start(const std::shared_ptr<const Binding>& binding);
    bool reserve(std::size_t requested) noexcept; // 持 Watching 锁调整共享核心计费, 失败保留原额度.
    const std::shared_ptr<Core> core_;            // 活跃业务应用拥有共享核心, 不通过 View 保留这个引用.
    const Scope scope_;                           // 固定外部地址, 网络 request 使用自己的字符串所有权.
    const std::string target_;                    // 空为全分组, 非空精确 Key.
    mutable std::mutex mutex_;                    // 保护投影、句柄状态与通知开始, 不跨用户回调或网络等待.
    mutable std::condition_variable condition_;   // 本地清理 wait 的唯一等待点.
    std::size_t bytes_{};                         // reserve 回调在此 Watching 锁下调用, Core 用原子总额计费.
    std::optional<Policy> projection_;            // close 可以释放网络受控存储, 当前 View 转为应用持有旧内容.
    View view_;                                   // 最后完整安装与最新本地状态, 不引用未收齐候选.
    std::move_only_function<void(View)> changed_; // 创建时固定, close 不在执行中销毁回调对象.
    std::shared_ptr<Stream> stream_;              // 唯一实际 RPC, 取消不提前清除此引用或归还物理额度.
    std::shared_ptr<const Binding> binding_;      // 本次实际请求目标, 与当前 Core 绑定用指针身份比较.
    std::atomic_bool closed_{};                   // 单向应用停止标志, 只归还一次 Core 拥有数.
    std::atomic_bool finished_{};                 // 由控制轮最后发布, 工厂可无锁移除已不需调度的对象.
    bool failed_{};                               // 永久本地/协议失败, 保留旧视图而不无限重拉相同损坏内容.
    bool notifying_{};                            // 用户回调已开始, close 允许它结束, wait 必须等其归还.
    bool dirty_{};                                // 尚需通知的完整状态, 中间状态允许合并, 不建立通知 FIFO.
    unsigned failures_{};                         // 有界恢复退避, 不能因 TCP 成功就清零不停断流的失败.
    Core::Time retry_{};                          // 下一次重连时刻, 没有逐毫秒轮询或每对象 Alarm.
};

template <class Policy>
class Watching<Policy>::Stream final : public grpc::ClientReadReactor<typename Policy::Reply> {
public:
    // owner 在途保活, 调用方把 Stream 放入 Reading 后才调用 start, 不在构造里逃逸 this.
    Stream(std::shared_ptr<Watching> owner, std::shared_ptr<const Binding> binding, proto::comet::v1::WatchRequest request) : owner_(std::move(owner)), binding_(std::move(binding)), request_(std::move(request)), stub_(Service::NewStub(binding_->streams)) {
        if (!binding_->session.empty()) {
            context_.AddMetadata("comet-session-bin", binding_->session);
        }
    }

    ~Stream() override {
        Reply{}.Swap(&page_); // 在网络已实际完成后释放最后一页, 再归还其受控计费.
        static_cast<void>(owner_->core_->resize(bytes_, 0));
    }

    // 成员与请求已稳定拥有, 外部控制路径的读取由唯一 hold 保护.
    void start() {
        stub_->async()->Watch(&context_, &request_, this);
        this->AddHold();
        held_ = true;
        this->StartRead(&page_);
        deadline_ = std::chrono::steady_clock::now() + owner_->core_->options_.timeout;
        this->StartCall();
    }

    // 取消只停止本调用, 在途计数直到 OnDone 后由 Reading 归还.
    void cancel() {
        const std::lock_guard lock(mutex_);
        context_.TryCancel();
        release();
    }

    // 一个完整 Read 的结果只发布给 Core, 不在 gRPC 线程解码安装或调用用户观察者.
    void OnReadDone(bool ok) override {
        const std::lock_guard lock(mutex_);
        if (ok) {
            const auto bytes = page_.SpaceUsedLong(); // 消息解码后才可观察真实拥有空间, 不把它遗漏在所有 Reader 的全局预算之外.
            if (!owner_->core_->resize(0, bytes)) {
                overflow_ = true;
                Reply{}.Swap(&page_);
            } else {
                bytes_ = bytes;
            }
        }
        available_ = ok;
        consumed_ = false;
        ended_ = !ok;
        owner_->core_->wake();
    }

    // context 是本 Stream 自己拥有的客户端对象, 可在 OnDone 后读取其 trailing metadata.
    void OnDone(const grpc::Status& status) override {
        const std::lock_guard lock(mutex_);
        code_ = status.error_code(); // 不复制可能含任意远端正文的 message/details.
        done_ = true;
        owner_->core_->wake();
    }

private:
    friend class Watching;
    Core::Time deadline_ = Core::Time::max(); // 首批/半批无进展期限, 完整就绪的空闲流没有总 deadline.
    bool timed_{};                            // 本地无进展取消, 不能误报为凭据撤销.
    bool consumed_{};                         // 当前完整页已消费, 且下一 Read 尚未发起; 只有控制线程在 mutex_ 内修改.
    bool repair_{};                           // 本流由一次缺 Attr 回退主动取消, OnDone 不误判共享 Session 失效.
    bool recovered_{};                        // 本流是否已经完整安装过一批, 只在首次恢复时更新共享 Core 的退避.
    bool overflow_{};                         // 已解码消息不能纳入全局预算, 控制轮明确报告容量并停止该订阅.
    std::size_t bytes_{};                     // 当前唯一已接收页的拥有空间, 页面释放/实际 OnDone 后才归还.

    // 已持 mutex_, 在收到 EOF 或取消后精确归还一次 hold, 使 OnDone 能够发生.
    void release() {
        if (held_) {
            held_ = false;
            this->RemoveHold();
        }
    }

    const std::shared_ptr<Watching> owner_;             // OnDone 清理前保留私有状态, public Reader close 已另行计数.
    const std::shared_ptr<const Binding> binding_;      // 固定实际 Star/Session, 不在换目标时改投旧请求.
    const proto::comet::v1::WatchRequest request_;      // 只发送一次, 含固定恢复位置和范围.
    std::unique_ptr<typename Service::Stub> stub_;      // 对应这次固定传输.
    grpc::ClientContext context_;                       // 长期 Watch 不误用 3 秒 unary 总 deadline.
    std::mutex mutex_;                                  // 控制路径与两个 gRPC 回调的缓冲所有权边界.
    Reply page_;                                        // 每次最多一份未消费页, 再 StartRead 前先归还它.
    bool held_{};                                       // 唯一控制路径 hold 尚未归还.
    bool available_{};                                  // 有完整页等待消费, 未再次 StartRead 前不可被写入.
    bool ended_{};                                      // 已读 EOF, 等控制路径 RemoveHold.
    bool done_{};                                       // gRPC 最终完成, 可以清除父级 Stream 引用和实际 RPC 额度.
    grpc::StatusCode code_ = grpc::StatusCode::UNKNOWN; // OnDone 前没有真实最终状态.
};

template <class Policy>
Watching<Policy>::Watching(std::shared_ptr<Core> core, Scope scope, std::string target, Options options) : core_(std::move(core)), scope_(std::move(scope)), target_(std::move(target)), projection_(std::in_place, scope_, target_, options.bytes, options.records, [this](std::size_t requested) noexcept { return reserve(requested); }), changed_(std::move(options.changed)) {
    view_ = projection_->view(State::waiting);
}

template <class Policy>
Watching<Policy>::~Watching() = default;

template <class Policy>
bool Watching<Policy>::reserve(std::size_t requested) noexcept {
    if (!core_->resize(bytes_, requested)) {
        return false;
    }
    bytes_ = requested;
    return true;
}

template <class Policy>
typename Watching<Policy>::View Watching<Policy>::load() const {
    const std::lock_guard lock(mutex_);
    auto view = view_; // 有界状态字段拥有副本, 大内容只共享不可变根.
    if (closed() || core_->stopped()) {
        view.state_ = State::closed;
    }
    return view;
}

template <class Policy>
bool Watching<Policy>::closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

template <class Policy>
bool Watching<Policy>::finished() const noexcept {
    return finished_.load(std::memory_order_acquire);
}

template <class Policy>
void Watching<Policy>::close() noexcept {
    bool first; // 与通知开始使用同一短锁, 不持锁调用 Core 或等待网络.
    {
        const std::lock_guard lock(mutex_);
        first = !closed_.exchange(true, std::memory_order_acq_rel);
    }
    if (first) {
        core_->release(); // 内部 RPC 持有的 shared_ptr 不延长应用自动订阅意图.
        core_->wake();
    }
}

template <class Policy>
bool Watching<Policy>::wait(std::chrono::milliseconds timeout) const {
    if (Core::notifying()) {
        throw std::logic_error("Cannot wait inside a Comet callback");
    }
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return cleaned_ && !stream_ && !notifying_; });
}

template <class Policy>
bool Watching<Policy>::publish(State state, std::optional<Error> error) {
    // 相同状态和相同原因无需重复通知, 每个完整数据批次另行标记 dirty.
    const bool changed = view_.state() != state || view_.error().has_value() != error.has_value() || (error && view_.error() && error->code != view_.error()->code);
    view_ = projection_->view(state, std::move(error));
    dirty_ = dirty_ || changed;
    return changed;
}

template <class Policy>
void Watching<Policy>::start(const std::shared_ptr<const Binding>& binding) {

    proto::comet::v1::WatchRequest request; // 只有这个对象自己的完整位置可以用于同范围恢复.
    request.mutable_scope()->set_sector(scope_.sector);
    request.mutable_scope()->set_spectrum(scope_.spectrum);
    request.set_target(target_);
    if (view_.version() && projection_->resume()) {
        request.set_instance(view_.instance());
        request.set_version(*view_.version());
    }
    projection_->begin(binding->instance);
    auto stream = std::make_shared<Stream>(this->shared_from_this(), binding, std::move(request));
    // 两个动作之间尚未发 RPC, 失败不会产生未被计费的请求.
    if (!core_->claim()) {
        retry_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        return;
    }
    binding_ = binding;
    stream_ = std::move(stream);
    stream_->start();
}

template <class Policy>
Core::Time Watching<Policy>::poll(Core::Time now, const std::shared_ptr<const Binding>& binding, const std::optional<Error>& error, bool closing) {

    std::unique_lock lock(mutex_);
    if (closing || core_->stopped()) {
        // 显式 Client 关闭由本轮落实到对象, 实际取消/准备释放不放在应用析构路径.
        if (!closed_.exchange(true, std::memory_order_acq_rel)) {
            lock.unlock();
            core_->release();
            lock.lock();
        }
    }
    const bool stopped = closed();
    if (stopped && !cleaned_) {
        view_.state_ = State::closed;
        view_.error_.reset();
        dirty_ = false;
        projection_.reset(); // view_ 保留的旧内容现由应用拥有, 不继续占受控恢复预算.
        if (stream_) {
            stream_->cancel();
        }
        cleaned_ = true;
    }
    if (stream_ && !stopped && binding_ != binding) {
        projection_->discard();
        stream_->cancel(); // 旧请求仍固定原目标, 不把任何迟到页接入新绑定.
        publish(view_.version() ? State::stale : State::waiting, error);
    }
    if (!stream_ && !stopped && !binding && !failed_) {
        // 首次登录被拒绝时也通知尚未建流的 Reader, 不把它永远留在没有原因的 waiting.
        publish(error ? State::failed : view_.version() ? State::stale
                                                        : State::waiting,
                error);
    }

    if (stream_) {
        auto stream = stream_; // 释放父级引用时保留当前访问, 避免销毁仍被本栈加锁的 mutex.
        std::unique_lock io(stream->mutex_);
        if (stream->done_) {
            io.unlock();
            stream_.reset();
            core_->relinquish();
            if (!stopped && binding_ == binding && !failed_ && !stream->repair_) {
                auto failure = Core::failure(grpc::Status(stream->code_, ""), stream->context_);
                if (failure.code == Error::Code::transport && stream->code_ == grpc::StatusCode::CANCELLED && !binding_->session.empty() && !stream->timed_) {
                    failure.code = Error::Code::session; // 凭据撤销会取消依附的流, 先同端点重认证, 不能仅因此切换 Star.
                }
                projection_->discard();
                // 局部权威落后/历史不足可单对象恢复, 不触发共享 Client 切换风暴.
                failed_ = failure.code == Error::Code::input || failure.code == Error::Code::limit || failure.code == Error::Code::protocol;
                publish(failed_ ? State::failed : view_.version() ? State::stale
                                                                  : State::waiting,
                        failure);
                failures_ = std::min(failures_ + 1, 32U);
                retry_ = now + core_->delay(failures_);
                if (failure.code == Error::Code::session || failure.code == Error::Code::transport) {
                    core_->lost(binding_, failure);
                }
            }
        } else if (!stopped && !stream->timed_ && !stream->available_ && now >= stream->deadline_) {
            stream->timed_ = true;
            io.unlock();
            projection_->discard();
            publish(view_.version() ? State::stale : State::waiting, Error{Error::Code::timeout, Error::Effect::unapplied, {}, {}, {}});
            stream->cancel(); // 同一目标在确认期限内不给首批/下一页, 不永远占住 Watch 名额.
        } else if (stream->overflow_ && !stopped && !failed_) {
            io.unlock();
            projection_->discard();
            failed_ = true;
            publish(State::failed, Error{Error::Code::limit, Error::Effect::unapplied, {}, {}, {}});
            stream->cancel();
        } else if (stream->ended_) {
            stream->release();
        } else if (stream->available_ && !stopped && binding_ == binding && !failed_) {
            auto page = std::move(stream->page_); // 独占当前完整页, 解析期间不继续读入下一页.
            stream->available_ = false;
            stream->consumed_ = true;
            io.unlock();
            try {
                auto accepted = projection_->accept(page);
                if (!accepted) {
                    if (projection_->repair(accepted.error().code)) {
                        stream->repair_ = true;
                        retry_ = now + core_->delay(++failures_);
                        publish(view_.version() ? State::stale : State::waiting, accepted.error());
                    } else {
                        failed_ = true;
                        auto error = accepted.error(); // 缺 Attr 的唯一回退已用完时, 对外明确是协议错误.
                        if (error.code == Error::Code::history) {
                            error.code = Error::Code::protocol;
                        }
                        publish(State::failed, std::move(error));
                    }
                    stream->cancel();
                } else if (*accepted) {
                    stream->deadline_ = Core::Time::max(); // 就绪后无更新是合法状态, 不以数据静默判掉线.
                    view_ = std::move(**accepted);
                    dirty_ = true;
                    failures_ = 0; // 只有完整应用一批才结束连续失败, 不是 TCP 建连成功即清零.
                    if (!stream->recovered_) {
                        stream->recovered_ = true;
                        core_->recovered(binding_);
                    }
                } else {
                    stream->deadline_ = now + core_->options_.timeout;
                }
            } catch (...) {
                projection_->discard();
                failed_ = true;
                publish(State::failed, Error{Error::Code::internal, Error::Effect::unapplied, {}, {}, {}});
                stream->cancel();
            }
            Reply{}.Swap(&page);
            const std::lock_guard consumed(stream->mutex_);
            static_cast<void>(core_->resize(stream->bytes_, 0));
            stream->bytes_ = 0; // 私有解析页已释放, 下一 Read 才能重新占用解码预算.
            // 下一 Read 必须等下面的用户通知返回, 保证单个订阅回调不并发且不引入通知队列.
        }
    }

    if (dirty_ && !stopped && changed_ && core_->notification()) {
        auto notification = view_; // 开始回调前固定本次状态, 用户可以非阻塞关闭或发起其他异步操作.
        notifying_ = true;
        dirty_ = false;
        lock.unlock();
        const bool previous = Core::notifying();
        Core::notify(true);
        try {
            changed_(std::move(notification));
        } catch (...) {
            core_->exception(); // 不回滚已安装数据或重放通知, 只提供有界诊断计数.
        }
        Core::notify(previous);
        lock.lock();
        notifying_ = false;
    }
    if (!changed_) {
        dirty_ = false;
    }
    if (stream_ && !closed() && !failed_ && binding_ == binding && !core_->stopped()) {
        const std::lock_guard io(stream_->mutex_);
        if (!stream_->available_ && !stream_->ended_ && !stream_->done_ && stream_->held_) {
            // 下一个读取由单独标志防止每次 timer pump 都对同一缓冲重复 StartRead.
            if (stream_->consumed_) {
                stream_->consumed_ = false;
                stream_->page_.Clear();
                stream_->StartRead(&stream_->page_);
            }
        }
    }
    if (!stream_ && !closed() && !failed_ && binding && now >= retry_ && !core_->stopped()) {
        start(binding);
    }
    if (cleaned_ && !stream_ && !notifying_) {
        finished_.store(true, std::memory_order_release);
    }
    condition_.notify_all();
    if (closed() || failed_ || stream_ || !binding) {
        return stream_ && !closed() && !failed_ && !stream_->timed_ ? stream_->deadline_ : Core::Time::max();
    }
    return retry_;
}

} // namespace comet::detail
