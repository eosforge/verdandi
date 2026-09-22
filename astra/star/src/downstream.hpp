#pragma once
#include "gateway.hpp"
#include <astra/scope.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>

namespace astra {
// 动态域共用的公共下行. gRPC 回调仅提交 I/O 结果, 既有控制循环有界 pump, 不创建每订阅线程.
// State 的提交通知必须连接 changed, 才能在快照发送期间独立保留后缀而不赌全局历史窗口.
template <class Domain>
class Downstream {
    using State = typename Domain::State;            // 领域提交边界, 保持各自独立业务规则.
    using Edition = typename Domain::Edition;        // 领域专属分页, 不使用跨域 oneof.
    using Reply = typename Edition::Reply;           // 此 RPC 唯一生成响应类型.
    using Event = typename State::Projection::Event; // 不可变公开变化.
public:
    // 资源都是部署预算, 不固化为协议数字, 不等同于整个进程 RSS.
    struct Limits {
        // 包含接入、发送及取消等待 OnDone 的流, 默认 4096, 范围 1..65536.
        std::size_t streams = 4096;
        // 全部冻结批次和合并后缀的保守逻辑字节, 默认 256 MiB.
        std::size_t bytes = 256 * 1024 * 1024;
        // 每条流的合并后缀上限, 默认 8 MiB; 不把整个快照复制进此队列.
        std::size_t pending = 8 * 1024 * 1024;
        // 一页写入的最大本地单调等待时间, 默认 30 秒, 必须为正.
        std::chrono::milliseconds timeout{30000};
    };

    // state/gateway 活过全部 RPC, wake 只能唤醒共享控制循环; 构造不启动线程或开放端口.
    Downstream(State& state, Gateway& gateway, std::function<void()> wake);
    // 测试与部署可显式降低预算, 非法配置抛 invalid_argument.
    Downstream(State& state, Gateway& gateway, std::function<void()> wake, Limits limits);
    // 先 stop、排空 Server 并 pump 到 empty, 再释放服务; 不析构仍被 gRPC 使用的 reactor.
    ~Downstream();
    // 验证地址和恢复位置, 登录许可与挂入事件索引定序; 禁止读取 __ 内部 Scope.
    grpc::ServerWriteReactor<Reply>* Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request);
    // State 在 域提交锁 内按提交顺序调用; 只合并不可变引用, 错误关闭受影响流而不撤回已提交写入.
    void changed(const Scope& scope, const Event& change) noexcept;
    // 唯一控制线程推进最多 maximum 个就绪任务, 默认 32; 不等待网络, 不遍历空闲订阅.
    void pump(std::chrono::steady_clock::time_point now, std::size_t maximum = 32);
    // 禁止新订阅并唤醒现有流取消; 仍须继续 pump, 直到全部 OnDone 被回收.
    void stop() noexcept;
    // 到期推进失败时结束现有流的追平承诺, 不把部分推进当作完整快照; 后续新流重新尝试捕获.
    void invalidate() noexcept;
    // 已没有本服务拥有的 RPC, 只表示 OnDone 已回收, 不代表整个 Server 停止.
    bool empty() const;

private:
    // 共享拥有 reactor 的活动流, 实现中严格分开回调状态锁和合并后缀锁.
    class Stream;
    // 立即拒绝的 reactor 不占活动索引, 由自身 OnDone 删除.
    class Rejected;
    // 已持 mutex_ 时加入侵入式就绪队列, 每流至多一个位置, 不分配.
    void enqueue(Stream& stream) noexcept;
    // I/O 回调唤醒本流, 只取得 mutex_, 不访问 State/Access.
    void signal(Stream& stream) noexcept;
    // 取得第一批完整投影; 调用时持本流 I/O 锁及认证许可, 不持全局队列锁.
    std::expected<Edition, grpc::Status> initial(Stream& stream);
    // 单任务推进, 当前 reactor 的上下文直到 OnDone 取得同一 I/O 锁前始终有效.
    void advance(Stream& stream, std::chrono::steady_clock::time_point now);
    // 删除已完成索引并归还所有逻辑预算, 调用时持 mutex_, 不等待任何流.
    void retire(Stream& stream);
    // 固定原生路由, 只读取完整已安装状态.
    State& state_;
    // 登录守卫及统一错误映射, 不重新比较 SECRET.
    Gateway& gateway_;
    // 唤醒函数不得阻塞或回调 pump, 不捕获短寿命 RPC.
    const std::function<void()> wake_;
    // 不在运行中变更计费规则.
    const Limits limits_;
    // 保护路由、就绪队列、计费及每流后缀; 不在此锁内编码或等待网络, 最终许可只跨非阻塞提交.
    mutable std::mutex mutex_;
    // 地址只保存有活动流的范围, 空范围在最后一条流退出后回收.
    std::map<Scope, std::map<Stream*, std::shared_ptr<Stream>>> streams_;
    // 就绪链表首尾, 指向 streams_ 仍拥有的对象, 初始空.
    Stream* head_{};
    Stream* tail_{};
    // 活动流总数与全局已占用字节, 都由 mutex_ 保护.
    std::size_t count_{};
    std::size_t bytes_{};
    // stop 可跨线程调用, 在 mutex_ 内单向设为 true.
    bool stopped_{};
    // 仅控制线程使用的低频写超时扫描截止, 不轮询所有空闲订阅的版本.
    std::chrono::steady_clock::time_point sweep_{};
};

template <class Domain>
class Downstream<Domain>::Rejected final : public grpc::ServerWriteReactor<typename Downstream<Domain>::Reply> {
public:
    // 错误已由入口生成, 不保有授权或业务状态.
    explicit Rejected(grpc::Status status) {
        this->Finish(std::move(status));
    }

    // gRPC 最后一次回调是本对象唯一释放点.
    void OnDone() override {
        delete this;
    }
};

template <class Domain>
class Downstream<Domain>::Stream final : public grpc::ServerWriteReactor<typename Downstream<Domain>::Reply>, public std::enable_shared_from_this<Stream> {
public:
    // stop callback 只借用仍处于 RPC 生命周期的上下文, 不调用 Access 或业务代码.
    struct Cancel {
        // 在 stop_callback 析构注销之前由 gRPC 保持存活.
        grpc::CallbackServerContext* context;

        // 仅通知 gRPC 取消, 不在停止源的调用线程释放 Stream.
        void operator()() const noexcept {
            context->TryCancel();
        }
    };

    // 请求小字段在 handler 内复制, 不借用 request 在 OnDone 之后的内存.
    Stream(Downstream<Domain>& owner, grpc::CallbackServerContext& context, const proto::comet::v1::WatchRequest& request, std::stop_token token) : owner(owner), context(context), scope{request.scope().sector(), request.scope().spectrum()}, target(request.target()), instance(request.instance()), requested(request.has_version() ? std::optional(request.version()) : std::nullopt), cancel(std::make_unique<std::stop_callback<Cancel>>(token, Cancel{&context})) {}

    // 默认析构注销停止回调; 拥有者必须在 OnDone 后才释放最后一个引用.
    ~Stream() override = default;

    // 每次 Write 完成只发布结果, 不在 gRPC 回调编码下一页.
    void OnWriteDone(bool ok) override {
        const std::lock_guard lock(io);
        writing = false;
        written = ok;
        failed = failed || !ok;
        busy.store(false, std::memory_order_release);
        owner.signal(*this);
    }

    // 取消可能与完成写入并发, Finish 留给控制线程统一定序.
    void OnCancel() override {
        const std::lock_guard lock(io);
        failed = true;
        owner.signal(*this);
    }

    // 不自行 delete; 控制线程拿到同一 io 锁后只回收, 不能再访问 context.
    void OnDone() override {
        const std::lock_guard lock(io);
        cancel.reset(); // 返回后 context 可立即销毁, 不能把注销推迟到控制线程回收.
        done = true;
        owner.signal(*this);
    }

    Downstream<Domain>& owner;                          // 必须活过 OnDone 和控制线程回收.
    grpc::CallbackServerContext& context;               // io 锁内且 done=false 时才允许访问.
    const Scope scope;                                  // 固定外部地址, 已排除 __.
    const std::string target;                           // 空为全范围, 非空精确 Key.
    const std::string instance;                         // 请求恢复的 Star ID, 不作认证凭据.
    const std::optional<std::uint64_t> requested;       // 未提供和显式零不同.
    std::mutex io;                                      // I/O 回调与 pump 的上下文生命周期边界.
    bool writing{};                                     // 一次最多一个 StartWrite, 完成回调才清除.
    bool written{};                                     // 尚未被 pump 消费的成功写入结果.
    bool failed{};                                      // gRPC 取消或失败, 单向置位.
    bool finished{};                                    // Finish 已调用, 等待 OnDone.
    bool done{};                                        // OnDone 已到达, 此后禁止访问 context.
    std::atomic_bool busy{};                            // 仅供低频超时扫描选取正在写的流.
    std::chrono::steady_clock::time_point deadline{};   // 当前页的单调写截止, io 保护.
    typename Downstream<Domain>::Reply page;            // gRPC 在 writing 时借用, 不提前 Clear/覆盖.
    std::optional<Edition> edition;                     // 唯一控制线程拥有的冻结批次, 不在回调改变.
    bool started{};                                     // 已捕获首批基线, 后续只生成 apply.
    std::uint64_t cursor{};                             // 最终完整页实际写成功的位置.
    std::size_t held{};                                 // 当前 edition 保守计费, mutex_ 保护调整.
    std::map<std::string, Event, std::less<>> pending;  // 按 Key 合并未冻结的最终操作.
    std::size_t bytes{};                                // pending 键/节点/载荷计费, mutex_ 保护.
    std::optional<std::uint64_t> first;                 // 已观察的第一个后缀提交号, 空表示尚无通知.
    std::uint64_t latest{};                             // 已观察的连续后缀最高号, 包括精确目标无关提交.
    std::optional<std::uint64_t> reset;                 // 已观察的全量替换号, 超过已捕获基线时必须重建流.
    bool overflow{};                                    // 后缀分配/预算/连续性失败, mutex_ 内单向置位.
    bool queued{};                                      // 在就绪链表中至多一次, mutex_ 保护.
    Stream* next{};                                     // 侵入式就绪后继, 没有独立分配.
    std::unique_ptr<std::stop_callback<Cancel>> cancel; // 必须在 OnDone 返回前注销上下文借用.
    std::size_t encoded{};                              // 当前在途页逻辑字节, mutex_ 保护计费.
};

template <class Domain>
Downstream<Domain>::Downstream(State& state, Gateway& gateway, std::function<void()> wake) : Downstream(state, gateway, std::move(wake), Limits{}) {}

template <class Domain>
Downstream<Domain>::Downstream(State& state, Gateway& gateway, std::function<void()> wake, Limits limits) : state_(state), gateway_(gateway), wake_(std::move(wake)), limits_(limits) {
    if (!wake_ || limits.streams == 0 || limits.streams > 65536 || limits.bytes == 0 || limits.pending == 0 || limits.pending > limits.bytes || limits.timeout.count() <= 0) {
        throw std::invalid_argument("Invalid Dynamic watch budget");
    }
    state_.notify([](void* context, const Scope& scope, const Event& event) noexcept { static_cast<Downstream*>(context)->changed(scope, event); }, this);
}

template <class Domain>
Downstream<Domain>::~Downstream() {
    state_.notify(nullptr, nullptr); // 停止、排空后解除唯一内部收集器, 不留下指向已析构 Feed 的回调.
}

template <class Domain>
void Downstream<Domain>::enqueue(Stream& stream) noexcept {
    if (!stream.queued) {
        stream.queued = true;
        if (tail_) {
            tail_->next = &stream;
        } else {
            head_ = &stream;
        }
        tail_ = &stream;
    }
}

template <class Domain>
void Downstream<Domain>::signal(Stream& stream) noexcept {
    const std::lock_guard lock(mutex_);
    enqueue(stream);
    wake_();
}

template <class Domain>
grpc::ServerWriteReactor<typename Downstream<Domain>::Reply>* Downstream<Domain>::Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request) {

    try {
        // 许可先于事件索引锁取得, 撤销安装同样遵循 Access -> State -> 事件索引, 不反向加锁.
        auto permit = gateway_.enter(*context);
        if (!permit) {
            return new Rejected(permit.error());
        }
        if (!Scope::text(request->scope().sector(), 128) || !Scope::text(request->scope().spectrum(), 128) || (!request->target().empty() && !Edition::target(request->target())) || (request->has_version() && request->instance().empty()) || (!request->instance().empty() && !Scope::text(request->instance(), 128))) {
            return new Rejected(gateway_.error(*context, grpc::StatusCode::INVALID_ARGUMENT, proto::comet::v1::REASON_INPUT, "Invalid Dynamic watch request"));
        }
        if (request->scope().sector().starts_with("__")) {
            return new Rejected(gateway_.error(*context, grpc::StatusCode::PERMISSION_DENIED, proto::comet::v1::REASON_DENIED, "Internal scopes are not public"));
        }
        const Scope scope{request->scope().sector(), request->scope().spectrum()}; // 通过长度/UTF-8 检查后才复制请求字段.
        const auto token = permit->has_value() ? permit->value().stopped() : std::stop_token{};
        auto stream = std::make_shared<Stream>(*this, *context, *request, token);
        {
            const std::lock_guard lock(mutex_);
            if (stopped_ || count_ == limits_.streams) {
                return new Rejected(gateway_.error(*context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Dynamic watch capacity unavailable"));
            }
            // 先观察提交再捕获根, 期间发生的提交稍后按版本剪去, 不丢失快照后的变化.
            const auto [group, created] = streams_.try_emplace(scope);
            try {
                group->second.emplace(stream.get(), stream);
            } catch (...) {
                if (created) {
                    streams_.erase(group);
                }
                throw;
            }
            ++count_;
            enqueue(*stream);
        }
        wake_();
        return stream.get();
    } catch (...) {
        return new Rejected(gateway_.error(*context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Dynamic watch preparation failed"));
    }
}

template <class Domain>
void Downstream<Domain>::changed(const Scope& scope, const Event& change) noexcept {

    const std::lock_guard lock(mutex_);
    const auto group = streams_.find(scope);
    if (group == streams_.end()) {
        return;
    }
    for (const auto& [address, owner] : group->second) {
        auto& stream = *owner; // 索引持有共享所有权, 不读取 I/O 字段或取得流锁.
        static_cast<void>(address);
        if (stream.overflow) {
            continue;
        }
        if (!stream.first) {
            stream.first = change.version;
        } else if (change.name && (stream.latest == UINT64_MAX || change.version != stream.latest + 1)) {
            stream.overflow = true;
        }
        stream.latest = change.version;
        if (!change.name) {
            stream.reset = change.version;
        } else if (stream.target.empty() || stream.target == change.name->key) {
            // 节点与第二份索引键也计入预算, 同 Key 高频变动只保留一个共享最终值.
            const auto cost = change.bytes + change.name->key.size() + 64;
            const auto old = stream.pending.find(change.name->key);
            const auto previous = old == stream.pending.end() ? 0 : old->second.bytes + old->first.size() + 64;
            if (cost > limits_.pending - (stream.bytes - previous + stream.encoded) || cost > limits_.bytes - (bytes_ - previous)) {
                stream.overflow = true;
            } else {
                try {
                    stream.pending.insert_or_assign(change.name->key, old == stream.pending.end() ? change : Edition::merge(old->second, change));
                    stream.bytes = stream.bytes - previous + cost;
                    bytes_ = bytes_ - previous + cost;
                } catch (...) {
                    stream.overflow = true;
                }
            }
        }
        enqueue(stream);
    }
    wake_();
}

template <class Domain>
std::expected<typename Downstream<Domain>::Edition, grpc::Status> Downstream<Domain>::initial(Stream& stream) {

    // 动态游标只在同一实例内恢复, 不把 Dynamic 跨 Star 的权威版本下限套用到本地投影.
    if (stream.requested && stream.instance == gateway_.instance()) {
        auto replay = state_.changes(stream.scope, *stream.requested);
        if (replay) {
            const auto version = replay->empty() ? *stream.requested : replay->back().version; // 未过滤的完整后缀固定其完成位置.
            return Edition(version, std::move(*replay), stream.target);
        }
        if (replay.error() != State::Error::history && replay.error() != State::Error::input) {
            return std::unexpected(gateway_.error(stream.context, replay.error() == State::Error::input ? grpc::StatusCode::FAILED_PRECONDITION : grpc::StatusCode::UNAVAILABLE, replay.error() == State::Error::clock ? proto::comet::v1::REASON_CLOCK : proto::comet::v1::REASON_HISTORY, "Dynamic cursor is unavailable"));
        }
    }
    if (!stream.target.empty()) {
        auto point = state_.find(stream.scope, stream.target);
        if (point) {
            return Edition(stream.target, std::move(*point));
        }
    } else if (auto view = state_.capture(stream.scope)) {
        return Edition(std::move(*view));
    }
    return std::unexpected(gateway_.error(stream.context, grpc::StatusCode::UNAVAILABLE, proto::comet::v1::REASON_BUSY, "Dynamic baseline is unavailable"));
}

template <class Domain>
void Downstream<Domain>::retire(Stream& stream) {
    bytes_ -= stream.bytes + stream.held + stream.encoded;
    const auto group = streams_.find(stream.scope);
    group->second.erase(&stream);
    if (group->second.empty()) {
        streams_.erase(group);
    }
    --count_;
}

template <class Domain>
void Downstream<Domain>::advance(Stream& stream, std::chrono::steady_clock::time_point now) {

    // io 排除 OnDone, 保证下面取得登录许可和发起 gRPC 操作时 context 仍有效.
    const std::lock_guard io(stream.io);
    if (stream.done) {
        const std::lock_guard lock(mutex_);
        // OnDone 可能在本任务弹出之后再次入队; 留给最后那个位置回收, 不悬挂链表指针.
        if (!stream.queued) {
            retire(stream);
        }
        return;
    }
    if (stream.finished) {
        return;
    }
    if (stream.writing) {
        bool stopping; // 关闭不等待页面的正常超时预算, 立即请求 gRPC 取消.
        {
            const std::lock_guard lock(mutex_);
            stopping = stopped_ || stream.overflow;
        }
        if (stopping || now >= stream.deadline) {
            stream.context.TryCancel();
        }
        return;
    }
    auto permit = gateway_.enter(stream.context); // 不在事件索引锁内获取 Access, 避免与凭据安装反向等待.
    if (!permit) {
        stream.finished = true;
        stream.Finish(permit.error());
        return;
    }
    permit->reset(); // 准备大页不占认证锁, 发送前再取最终许可, 撤销仍可立即使准备失效.
    {
        const std::lock_guard lock(mutex_);
        if (stream.failed || stopped_ || stream.overflow) {
            stream.finished = true;
            stream.Finish(gateway_.error(stream.context, stream.overflow ? grpc::StatusCode::RESOURCE_EXHAUSTED : grpc::StatusCode::CANCELLED, stream.overflow ? proto::comet::v1::REASON_BUSY : proto::comet::v1::REASON_SESSION, "Dynamic watch cannot continue"));
            return;
        }
    }

    try {
        if (stream.written) {
            stream.written = false;
            if (stream.edition->complete()) {
                stream.cursor = stream.edition->version(); // 只有完整页实际写成功才推进此流位置.
                {
                    const std::lock_guard lock(mutex_);
                    bytes_ -= stream.held;
                    stream.held = 0;
                }
                stream.edition.reset();
            }
            typename Downstream<Domain>::Reply{}.Swap(&stream.page); // 回收实际页分配, 不让 Clear 保留大容量却计费为零.
            {
                const std::lock_guard lock(mutex_);
                bytes_ -= stream.encoded;
                stream.encoded = 0;
            }
        }
        if (!stream.started) {
            auto edition = initial(stream);
            if (!edition) {
                stream.finished = true;
                stream.Finish(edition.error());
                return;
            }
            stream.edition.emplace(std::move(*edition));
            stream.started = true;
            const std::lock_guard lock(mutex_);
            // 已被捕获基线覆盖的通知不重复发出, 更高的全量替换则不能伪装为连续 apply.
            const auto baseline = stream.edition->version();
            for (auto entry = stream.pending.begin(); entry != stream.pending.end();) {
                if (entry->second.version <= baseline) {
                    const auto cost = entry->second.bytes + entry->first.size() + 64;
                    stream.bytes -= cost;
                    bytes_ -= cost;
                    entry = stream.pending.erase(entry);
                } else {
                    ++entry;
                }
            }
            if (stream.edition->bytes() > limits_.bytes - bytes_) {
                stream.overflow = true;
            } else {
                stream.held = stream.edition->bytes();
                bytes_ += stream.held;
            }
        }

        {
            const std::lock_guard lock(mutex_);
            const auto baseline = stream.edition ? stream.edition->version() : stream.cursor;
            if (stream.overflow || (stream.reset && *stream.reset > baseline) || (stream.first && baseline != UINT64_MAX && *stream.first > baseline + 1)) {
                stream.finished = true;
                stream.Finish(gateway_.error(stream.context, grpc::StatusCode::OUT_OF_RANGE, proto::comet::v1::REASON_HISTORY, "Dynamic watch requires a new baseline"));
                return;
            }
            if (!stream.edition) {
                if (!stream.first || stream.latest <= stream.cursor) {
                    return;
                }
                std::vector<Event> replay; // 固定完整后缀, 不向已经裁剪的普通历史补查.
                replay.reserve(stream.pending.size());
                for (const auto& [key, change] : stream.pending) {
                    static_cast<void>(key);
                    if (change.version > stream.cursor) {
                        replay.push_back(change);
                    }
                }
                stream.edition.emplace(stream.latest, std::move(replay), stream.target);
                bytes_ -= stream.bytes;
                stream.bytes = 0;
                stream.pending.clear();
                stream.held = stream.edition->bytes();
                bytes_ += stream.held;
            }
        }
        // 全局锁外编码单页, 每轮只准备这一份有界暂存. Permit 不跨任何网络等待.
        stream.page = stream.edition->next(gateway_.instance());
        const auto encoded = stream.page.ByteSizeLong(); // 单次完整大小计算, 同时计入在途与全局保有预算.
        auto sending = gateway_.enter(stream.context);   // 只覆盖最终检查和非阻塞 StartWrite, 不跨编码或网络等待.
        if (!sending) {
            typename Downstream<Domain>::Reply{}.Swap(&stream.page);
            stream.finished = true;
            stream.Finish(sending.error());
            return;
        }
        const std::lock_guard lock(mutex_);
        if (stream.overflow || stopped_ || (stream.reset && *stream.reset > stream.edition->version()) || encoded > limits_.bytes - bytes_ || encoded > limits_.pending - stream.bytes) {
            typename Downstream<Domain>::Reply{}.Swap(&stream.page); // 此页尚未进入在途, 不保留未计费的大编码缓冲.
            stream.finished = true;
            stream.Finish(gateway_.error(stream.context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Dynamic send budget unavailable"));
            return;
        }
        stream.encoded = encoded;
        bytes_ += encoded;
        stream.deadline = now + limits_.timeout;
        stream.writing = true;
        stream.busy.store(true, std::memory_order_release);
        stream.StartWrite(&stream.page);
    } catch (...) {
        stream.finished = true;
        stream.Finish(gateway_.error(stream.context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Dynamic page preparation failed"));
    }
}

template <class Domain>
void Downstream<Domain>::pump(std::chrono::steady_clock::time_point now, std::size_t maximum) {

    if (now >= sweep_) {
        const std::lock_guard lock(mutex_);
        // 只有在途 Write 需要检查 deadline, 空闲 Watch 的数据更新完全由提交通知触发.
        for (const auto& [scope, group] : streams_) {
            static_cast<void>(scope);
            for (const auto& [address, stream] : group) {
                static_cast<void>(address);
                if (stream->busy.load(std::memory_order_acquire)) {
                    enqueue(*stream);
                }
            }
        }
        sweep_ = now + std::chrono::seconds(1);
    }
    for (std::size_t count = 0; count < maximum; ++count) {
        std::shared_ptr<Stream> stream; // 弹出后保活, OnDone 回收索引不会释放当前栈使用的对象.
        {
            const std::lock_guard lock(mutex_);
            if (!head_) {
                break;
            }
            stream = head_->shared_from_this();
            head_ = head_->next;
            if (!head_) {
                tail_ = nullptr;
            }
            stream->queued = false;
            stream->next = nullptr;
        }
        advance(*stream, now);
    }
    const std::lock_guard lock(mutex_);
    if (head_) {
        wake_();
    }
}

template <class Domain>
void Downstream<Domain>::stop() noexcept {
    const std::lock_guard lock(mutex_);
    stopped_ = true;
    for (const auto& [scope, group] : streams_) {
        static_cast<void>(scope);
        for (const auto& [address, stream] : group) {
            static_cast<void>(address);
            enqueue(*stream);
        }
    }
    wake_();
}

template <class Domain>
bool Downstream<Domain>::empty() const {
    const std::lock_guard lock(mutex_);
    return count_ == 0;
}

template <class Domain>
void Downstream<Domain>::invalidate() noexcept {
    const std::lock_guard lock(mutex_); // 不调用原生状态, 避免与提交回调反向加锁.
    for (const auto& [scope, group] : streams_) {
        static_cast<void>(scope);
        for (const auto& [address, stream] : group) {
            static_cast<void>(address);
            stream->overflow = true;
            enqueue(*stream);
        }
    }
    wake_();
}

} // namespace astra
