#include "readout.hpp"
#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace astra {
class Readout::Rejected final : public grpc::ServerWriteReactor<proto::comet::v1::AlmanacWatchReply> {
public:
    // 错误已由入口生成, 不保有授权或业务状态.
    explicit Rejected(grpc::Status status) {
        Finish(std::move(status));
    }

    // gRPC 最后一次回调是本对象唯一释放点.
    void OnDone() override {
        delete this;
    }
};

class Readout::Stream final : public grpc::ServerWriteReactor<proto::comet::v1::AlmanacWatchReply>, public std::enable_shared_from_this<Stream> {
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
    Stream(Readout& owner, grpc::CallbackServerContext& context, const proto::comet::v1::WatchRequest& request, std::stop_token token) : owner(owner), context(context), scope{request.scope().sector(), request.scope().spectrum()}, target(request.target()), instance(request.instance()), requested(request.has_version() ? std::optional(request.version()) : std::nullopt), cancel(std::make_unique<std::stop_callback<Cancel>>(token, Cancel{&context})) {}

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

    Readout& owner;                                              // 必须活过 OnDone 和控制线程回收.
    grpc::CallbackServerContext& context;                        // io 锁内且 done=false 时才允许访问.
    const Scope scope;                                           // 固定外部地址, 已排除 __.
    const std::string target;                                    // 空为全范围, 非空精确 Key.
    const std::string instance;                                  // 请求恢复的 Star ID, 不作认证凭据.
    const std::optional<std::uint64_t> requested;                // 未提供和显式零不同.
    std::mutex io;                                               // I/O 回调与 pump 的上下文生命周期边界.
    bool writing{};                                              // 一次最多一个 StartWrite, 完成回调才清除.
    bool written{};                                              // 尚未被 pump 消费的成功写入结果.
    bool failed{};                                               // gRPC 取消或失败, 单向置位.
    bool finished{};                                             // Finish 已调用, 等待 OnDone.
    bool done{};                                                 // OnDone 已到达, 此后禁止访问 context.
    std::atomic_bool busy{};                                     // 仅供低频超时扫描选取正在写的流.
    std::chrono::steady_clock::time_point deadline{};            // 当前页的单调写截止, io 保护.
    proto::comet::v1::AlmanacWatchReply page;                    // gRPC 在 writing 时借用, 不提前 Clear/覆盖.
    std::optional<Edition> edition;                              // 唯一控制线程拥有的冻结批次, 不在回调改变.
    bool started{};                                              // 已捕获首批基线, 后续只生成 apply.
    std::uint64_t cursor{};                                      // 最终完整页实际写成功的位置.
    std::size_t held{};                                          // 当前 edition 保守计费, mutex_ 保护调整.
    std::map<std::string, Almanac::Change, std::less<>> pending; // 按 Key 合并未冻结的最终操作.
    std::size_t bytes{};                                         // pending 键/节点/载荷计费, mutex_ 保护.
    std::optional<std::uint64_t> first;                          // 已观察的第一个后缀提交号, 空表示尚无通知.
    std::uint64_t latest{};                                      // 已观察的连续后缀最高号, 包括精确目标无关提交.
    std::optional<std::uint64_t> reset;                          // 已观察的全量替换号, 超过已捕获基线时必须重建流.
    bool overflow{};                                             // 后缀分配/预算/连续性失败, mutex_ 内单向置位.
    bool queued{};                                               // 在就绪链表中至多一次, mutex_ 保护.
    Stream* next{};                                              // 侵入式就绪后继, 没有独立分配.
    std::unique_ptr<std::stop_callback<Cancel>> cancel;          // 必须在 OnDone 返回前注销上下文借用.
    std::size_t encoded{};                                       // 当前在途页逻辑字节, mutex_ 保护计费.
};

Readout::Readout(Library& library, Gateway& gateway, std::function<void()> wake) : Readout(library, gateway, std::move(wake), Limits{}) {}

Readout::Readout(Library& library, Gateway& gateway, std::function<void()> wake, Limits limits) : library_(library), gateway_(gateway), wake_(std::move(wake)), limits_(limits) {
    if (!wake_ || limits.streams == 0 || limits.streams > 65536 || limits.bytes == 0 || limits.pending == 0 || limits.pending > limits.bytes || limits.timeout.count() <= 0) {
        throw std::invalid_argument("Invalid Almanac watch budget");
    }
}

Readout::~Readout() = default;

void Readout::enqueue(Stream& stream) noexcept {
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

void Readout::signal(Stream& stream) noexcept {
    const std::lock_guard lock(mutex_);
    enqueue(stream);
    wake_();
}

grpc::ServerWriteReactor<proto::comet::v1::AlmanacWatchReply>* Readout::Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request) {

    try {
        // 许可先于事件索引锁取得, 撤销安装同样遵循 Access -> Library -> 事件索引, 不反向加锁.
        auto permit = gateway_.enter(*context);
        if (!permit) {
            return new Rejected(permit.error());
        }
        if (!Scope::text(request->scope().sector(), 128) || !Scope::text(request->scope().spectrum(), 128) || request->scope().sector().starts_with("__") || (!request->target().empty() && !Scope::text(request->target(), 1024)) || (request->has_version() && request->instance().empty()) || (!request->instance().empty() && !Scope::text(request->instance(), 128))) {
            return new Rejected(gateway_.error(*context, grpc::StatusCode::INVALID_ARGUMENT, proto::comet::v1::REASON_INPUT, "Invalid Almanac watch request"));
        }
        const Scope scope{request->scope().sector(), request->scope().spectrum()}; // 静态边界确认后再复制有界请求字段.
        const auto token = permit->has_value() ? permit->value().stopped() : std::stop_token{};
        auto stream = std::make_shared<Stream>(*this, *context, *request, token);
        {
            const std::lock_guard lock(mutex_);
            if (stopped_ || count_ == limits_.streams) {
                return new Rejected(gateway_.error(*context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Almanac watch capacity unavailable"));
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
        return new Rejected(gateway_.error(*context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Almanac watch preparation failed"));
    }
}

void Readout::changed(const Scope& scope, const Almanac::Change& change) noexcept {

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
        } else if (change.key && (stream.latest == UINT64_MAX || change.version != stream.latest + 1)) {
            stream.overflow = true;
        }
        stream.latest = change.version;
        if (!change.key) {
            stream.reset = change.version;
        } else if (stream.target.empty() || stream.target == *change.key) {
            // 节点与第二份索引键也计入预算, 同 Key 高频变动只保留一个共享最终值.
            const auto cost = change.bytes() + change.key->size() + 64;
            const auto old = stream.pending.find(*change.key);
            const auto previous = old == stream.pending.end() ? 0 : old->second.bytes() + old->first.size() + 64;
            if (cost > limits_.pending - (stream.bytes - previous + stream.encoded) || cost > limits_.bytes - (bytes_ - previous)) {
                stream.overflow = true;
            } else {
                try {
                    stream.pending.insert_or_assign(*change.key, change);
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

std::expected<Edition, grpc::Status> Readout::initial(Stream& stream) {

    const auto book = library_.find(stream.scope); // 共享分组所有权, 不跨网络持路由锁.
    const auto version = book ? book->usage().version : std::optional<std::uint64_t>{0};
    if (!version || (stream.requested && *version < *stream.requested)) {
        return std::unexpected(gateway_.error(stream.context, grpc::StatusCode::UNAVAILABLE, proto::comet::v1::REASON_VERSION, "Almanac authority version is behind", version));
    }
    if (book && stream.requested && stream.instance == gateway_.instance()) {
        if (auto replay = book->replay(*stream.requested)) {
            return Edition(std::move(*replay), stream.target);
        }
    }
    if (!book) {
        return Edition();
    }
    if (!stream.target.empty()) {
        auto point = book->find(stream.target);
        if (point) {
            return Edition(stream.target, std::move(*point));
        }
    } else if (auto view = book->view()) {
        return Edition(std::move(*view));
    }
    return std::unexpected(gateway_.error(stream.context, grpc::StatusCode::UNAVAILABLE, proto::comet::v1::REASON_BUSY, "Almanac baseline is unavailable"));
}

void Readout::retire(Stream& stream) {
    bytes_ -= stream.bytes + stream.held + stream.encoded;
    const auto group = streams_.find(stream.scope);
    group->second.erase(&stream);
    if (group->second.empty()) {
        streams_.erase(group);
    }
    --count_;
}

void Readout::advance(Stream& stream, std::chrono::steady_clock::time_point now) {

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
            stream.Finish(gateway_.error(stream.context, stream.overflow ? grpc::StatusCode::RESOURCE_EXHAUSTED : grpc::StatusCode::CANCELLED, stream.overflow ? proto::comet::v1::REASON_BUSY : proto::comet::v1::REASON_SESSION, "Almanac watch cannot continue"));
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
            proto::comet::v1::AlmanacWatchReply{}.Swap(&stream.page); // 回收实际页分配, 不让 Clear 保留大容量却计费为零.
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
                    const auto cost = entry->second.bytes() + entry->first.size() + 64;
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
                stream.Finish(gateway_.error(stream.context, grpc::StatusCode::OUT_OF_RANGE, proto::comet::v1::REASON_HISTORY, "Almanac watch requires a new baseline"));
                return;
            }
            if (!stream.edition) {
                if (!stream.first || stream.latest <= stream.cursor) {
                    return;
                }
                Almanac::Replay replay{stream.latest, {}}; // 有界合并后缀转移到固定 apply, 不回查已经淘汰的历史.
                replay.changes.reserve(stream.pending.size());
                for (const auto& [key, change] : stream.pending) {
                    static_cast<void>(key);
                    if (change.version > stream.cursor) {
                        replay.changes.push_back(change);
                    }
                }
                stream.edition.emplace(std::move(replay), stream.target);
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
            proto::comet::v1::AlmanacWatchReply{}.Swap(&stream.page);
            stream.finished = true;
            stream.Finish(sending.error());
            return;
        }
        const std::lock_guard lock(mutex_);
        if (stream.overflow || stopped_ || (stream.reset && *stream.reset > stream.edition->version()) || encoded > limits_.bytes - bytes_ || encoded > limits_.pending - stream.bytes) {
            proto::comet::v1::AlmanacWatchReply{}.Swap(&stream.page); // 此页尚未进入在途, 不保留未计费的大编码缓冲.
            stream.finished = true;
            stream.Finish(gateway_.error(stream.context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Almanac send budget unavailable"));
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
        stream.Finish(gateway_.error(stream.context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Almanac page preparation failed"));
    }
}

void Readout::pump(std::chrono::steady_clock::time_point now, std::size_t maximum) {

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

void Readout::stop() noexcept {
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

bool Readout::empty() const {
    const std::lock_guard lock(mutex_);
    return count_ == 0;
}
} // namespace astra
