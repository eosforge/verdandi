#include "readout.hpp"
#include "watch_kernel.hpp"
#include <algorithm>
#include <astra/profile.hpp>
#include <atomic>
#include <stdexcept>
#include <utility>

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

        ASTRA_PROFILE_SCOPE("star.readout.OnWriteDone");
        ASTRA_PROFILE_BEGIN(profile_lock_42, "star.readout.OnWriteDone.wait.lock");
        const std::lock_guard lock(io);
        ASTRA_PROFILE_END(profile_lock_42);
        ASTRA_PROFILE_CONSUME(profile_write, "star.readout.write_to_callback");
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
    ASTRA_PROFILE_STAMP(profile_write);                          // io 保护提交到完成回调的间隔, 关闭构建无成员.
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
    bool overflow{};                                             // 后缀分配/预算/连续性失败, mutex_ 内单向置位.
    bool queued{};                                               // 在就绪链表中至多一次, mutex_ 保护.
    Stream* next{};                                              // 侵入式就绪后继, 没有独立分配.
    typename astra::Watch<Stream>::Link flight;                  // 独立在途双向链, 由拥有者索引锁保护, 初始未入链.
    std::unique_ptr<std::stop_callback<Cancel>> cancel;          // 必须在 OnDone 返回前注销上下文借用.
    std::size_t encoded{};                                       // 当前在途页逻辑字节, mutex_ 保护计费.
};

Readout::Readout(Library& library, Gateway& gateway, std::function<void()> wake) : Readout(library, gateway, std::move(wake), Limits{}) {}

Readout::Readout(Library& library, Gateway& gateway, std::function<void()> wake, Limits limits) : library_(library), gateway_(gateway), wake_(std::move(wake)), limits_(limits) {
    astra::Watch<Stream>::validate(limits_, "Invalid Almanac watch budget");
    if (!wake_) {
        throw std::invalid_argument("Invalid Almanac watch budget");
    }
}

Readout::~Readout() = default;

void Readout::enqueue(Stream& stream) noexcept {
    queue_.enqueue(stream);
}

void Readout::signal(Stream& stream) noexcept {

    bool scheduled{}; // 已排队流会消费最新 I/O 状态, 重复回调不再重复唤醒控制轮.
    {
        const std::lock_guard lock(mutex_);
        scheduled = queue_.enqueue(stream);
    }
    if (scheduled) {
        wake_(); // 索引锁外通知, 与动态域保持同一调度规则.
    }
}

grpc::ServerWriteReactor<proto::comet::v1::AlmanacWatchReply>* Readout::Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request) {

    ASTRA_PROFILE_SCOPE("star.readout.Readout.Watch");

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
            ASTRA_PROFILE_BEGIN(profile_lock_130, "star.readout.Readout.Watch.wait.lock");
            const std::lock_guard lock(mutex_);
            ASTRA_PROFILE_END(profile_lock_130);
            if (stopped_ || count_ == limits_.streams) {
                return new Rejected(gateway_.error(*context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Almanac watch capacity unavailable"));
            }
            // 先观察提交再捕获根, 期间发生的提交稍后按版本剪去, 不丢失快照后的变化.
            const auto [group, created] = streams_.try_emplace(scope);
            try {
                group->second.streams.emplace(stream.get(), stream);
                if (stream->target.empty()) {
                    group->second.broadcast.insert(stream.get()); // 全范围流每次提交都遍历, 只记非拥有指针.
                } else {
                    group->second.precise[stream->target].insert(stream.get()); // 同目标共享一个集合, 不按流数复制 Key.
                }
            } catch (...) {
                progress_.erase(group->second, stream.get()); // 拥有者回滚, 视图必须同步清除, 不留幽灵流.
                group->second.broadcast.erase(stream.get());
                if (const auto bucket = group->second.precise.find(stream->target); bucket != group->second.precise.end()) {
                    bucket->second.erase(stream.get());
                    if (bucket->second.empty()) {
                        group->second.precise.erase(bucket);
                    }
                }
                if (created || group->second.streams.empty()) {
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

    ASTRA_PROFILE_SCOPE("star.readout.Readout.changed");

    ASTRA_PROFILE_BEGIN(profile_lock_169, "star.readout.Readout.changed.wait.lock");
    const std::lock_guard lock(mutex_);
    ASTRA_PROFILE_END(profile_lock_169);
    const auto group = streams_.find(scope);
    if (group == streams_.end()) {
        return;
    }
    // 连续性和 reset 归范围所有, 精确目标跳过无关变化也不会丢失完整覆盖证据.
    progress_.publish(group->second, change.version, !change.key);
    const auto visit = [&](Stream& stream) {
        if (stream.overflow) {
            return;
        }
        ++scans_; // 只有实际访问的流才计入; 空闲精确流的跳过由 pump 心跳覆盖, 不再消耗提交锁.
        // 节点与第二份索引键也计入预算, 同 Key 高频变动只保留一个共享最终值.
        const auto cost = change.bytes() + change.key->size() + 64;
        const auto old = stream.pending.lower_bound(*change.key); // 同次查找复用现有条目或准确插入位置.
        const bool exists = old != stream.pending.end() && old->first == *change.key;
        const auto previous = exists ? old->second.bytes() + old->first.size() + 64 : 0;
        if (previous > stream.bytes || previous > bytes_) {
            stream.overflow = true; // 内部账本不一致直接拒绝, 不归零后继续做会回绕的减法.
            enqueue(stream);
            return;
        }
        const auto held = stream.bytes - previous; // 替换后仍保留的其他 Key 字节, 已验证减法合法.
        const auto total = bytes_ - previous;      // 全局保有量减去同一份旧记录, 与最终记账一致.
        if (held > limits_.pending || stream.encoded > limits_.pending - held || cost > limits_.pending - held - stream.encoded || total > limits_.bytes || cost > limits_.bytes - total) {
            stream.overflow = true;
        } else {
            try {
                if (exists) {
                    old->second = change; // 只替换不可变事件引用, 不再次查找和复制键.
                } else {
                    stream.pending.emplace_hint(old, *change.key, change);
                }
                stream.bytes = held + cost;
                bytes_ = total + cost;
                ++matched_; // 只有真正进入后缀的目标才算命中, 溢出与过滤都不计入.
            } catch (...) {
                stream.overflow = true;
            }
        }
        enqueue(stream);
    };
    if (change.key) {
        // 只收集合并正文; reset 与无关目标的进度由范围轮转统一通知, 不在提交内全量扫描.
        for (auto* pointer : group->second.broadcast) {
            visit(*pointer);
        }
        if (const auto bucket = group->second.precise.find(*change.key); bucket != group->second.precise.end()) {
            for (auto* pointer : bucket->second) {
                visit(*pointer);
            }
        }
    }
    wake_();
}

Readout::Delivery Readout::delivery() const {

    const std::lock_guard lock(mutex_);
    return {.scans = scans_, .matched = matched_};
}

std::expected<Edition, grpc::Status> Readout::initial(Stream& stream) {

    ASTRA_PROFILE_SCOPE("star.readout.Readout.initial");

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
    queue_.settle(stream);
    const auto group = streams_.find(stream.scope);
    progress_.erase(group->second, &stream);
    group->second.broadcast.erase(&stream); // 非拥有视图与拥有者同锁同步清除, 不留悬空指针.
    if (const auto bucket = group->second.precise.find(stream.target); bucket != group->second.precise.end()) {
        bucket->second.erase(&stream);
        if (bucket->second.empty()) {
            group->second.precise.erase(bucket); // 空目标集合及时回收, 不随历史目标数增长.
        }
    }
    if (group->second.streams.empty()) {
        streams_.erase(group);
    }
    --count_;
}

void Readout::advance(Stream& stream, std::chrono::steady_clock::time_point now) {

    ASTRA_PROFILE_SCOPE("star.readout.Readout.advance");

    // io 排除 OnDone, 保证下面取得登录许可和发起 gRPC 操作时 context 仍有效.
    ASTRA_PROFILE_BEGIN(profile_lock_273, "star.readout.Readout.advance.wait.io");
    const std::lock_guard io(stream.io);
    ASTRA_PROFILE_END(profile_lock_273);
    if (stream.done) {
        ASTRA_PROFILE_BEGIN(profile_lock_275, "star.readout.Readout.advance.wait.lock");
        const std::lock_guard lock(mutex_);
        ASTRA_PROFILE_END(profile_lock_275);
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
            ASTRA_PROFILE_BEGIN(profile_lock_288, "star.readout.Readout.advance.wait.lock");
            const std::lock_guard lock(mutex_);
            ASTRA_PROFILE_END(profile_lock_288);
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
        ASTRA_PROFILE_BEGIN(profile_lock_304, "star.readout.Readout.advance.wait.lock");
        std::unique_lock lock(mutex_); // 只保护停止/预算状态, 不跨错误编码和 gRPC 调用.
        ASTRA_PROFILE_END(profile_lock_304);
        if (stream.failed || stopped_ || stream.overflow) {
            const bool overflow = stream.overflow; // 解锁前固定拒绝原因, 后续通知仍可修改 overflow.
            lock.unlock();
            stream.finished = true;
            stream.Finish(gateway_.error(stream.context, overflow ? grpc::StatusCode::RESOURCE_EXHAUSTED : grpc::StatusCode::CANCELLED, overflow ? proto::comet::v1::REASON_BUSY : proto::comet::v1::REASON_SESSION, "Almanac watch cannot continue"));
            return;
        }
    }

    try {
        if (stream.written) {
            stream.written = false;
            if (stream.edition->complete()) {
                stream.cursor = stream.edition->version(); // 只有完整页实际写成功才推进此流位置.
                {
                    ASTRA_PROFILE_BEGIN(profile_lock_320, "star.readout.Readout.advance.wait.lock");
                    const std::lock_guard lock(mutex_);
                    ASTRA_PROFILE_END(profile_lock_320);
                    bytes_ -= stream.held;
                    stream.held = 0;
                }
                stream.edition.reset();
            }
            proto::comet::v1::AlmanacWatchReply{}.Swap(&stream.page); // 回收实际页分配, 不让 Clear 保留大容量却计费为零.
            {
                ASTRA_PROFILE_BEGIN(profile_lock_328, "star.readout.Readout.advance.wait.lock");
                const std::lock_guard lock(mutex_);
                ASTRA_PROFILE_END(profile_lock_328);
                bytes_ -= stream.encoded;
                stream.encoded = 0;
                queue_.settle(stream);
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
            ASTRA_PROFILE_BEGIN(profile_lock_343, "star.readout.Readout.advance.wait.lock");
            const std::lock_guard lock(mutex_);
            ASTRA_PROFILE_END(profile_lock_343);
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
            const auto held = stream.edition->bytes(); // 冻结批次保守计费, 累加前先作上溢安全比较, 不以回绕通过预算.
            if (held > limits_.bytes || bytes_ > limits_.bytes - held) {
                stream.overflow = true;
            } else {
                stream.held = held;
                bytes_ += stream.held;
            }
        }

        // 只在索引锁内移交后缀和末游标, 大向量准备与旧节点销毁不阻塞写入收集器.
        std::map<std::string, Almanac::Change, std::less<>> pending; // 移出的后缀仍由 held 计费, 析构发生在 mutex_ 外.
        std::optional<std::uint64_t> preparing;                      // 有值时本轮需要构造新的固定 apply, 零也是合法版本.
        {
            ASTRA_PROFILE_BEGIN(profile_lock_369, "star.readout.Readout.advance.wait.lock");
            std::unique_lock lock(mutex_); // 失败路径先释放范围索引, 再提交 RPC 终止.
            ASTRA_PROFILE_END(profile_lock_369);
            const auto& group = streams_.find(stream.scope)->second; // 索引锁内固定进度与后缀的同一提交边界.
            const auto baseline = stream.edition ? stream.edition->version() : stream.cursor;
            if (stream.overflow || !group.covers(baseline)) {
                lock.unlock();
                stream.finished = true;
                stream.Finish(gateway_.error(stream.context, grpc::StatusCode::OUT_OF_RANGE, proto::comet::v1::REASON_HISTORY, "Almanac watch requires a new baseline"));
                return;
            }
            if (!stream.edition) {
                if (group.version <= stream.cursor) {
                    return;
                }
                preparing = group.version;
                pending.swap(stream.pending);
                stream.held = std::exchange(stream.bytes, 0); // 后续通知进入新后缀, 原有准备仍占总预算.
            }
        }
        if (preparing) {
            Almanac::Replay replay{*preparing, {}}; // 不回查有限历史, 精确目标未命中也发出完整的空 apply.
            replay.changes.reserve(pending.size());
            for (auto& [key, change] : pending) {
                if (change.version > stream.cursor) {
                    replay.changes.push_back(std::move(change));
                }
            }
            stream.edition.emplace(std::move(replay), stream.target);
            pending.clear(); // 归还旧索引节点不持事件锁.
            ASTRA_PROFILE_BEGIN(profile_lock_397, "star.readout.Readout.advance.wait.lock");
            const std::lock_guard lock(mutex_);
            ASTRA_PROFILE_END(profile_lock_397);
            bytes_ -= stream.held;
            stream.held = 0;
            const auto held = stream.edition->bytes(); // 只用已校验的减法判断剩余额度, 不靠累加后的回绕比较.
            if (held > limits_.bytes || bytes_ > limits_.bytes - held) {
                stream.overflow = true;
            } else {
                stream.held = held;
                bytes_ += held;
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
        ASTRA_PROFILE_BEGIN(profile_lock_418, "star.readout.Readout.advance.wait.lock");
        std::unique_lock lock(mutex_); // 最终接受页面与计费同锁完成, gRPC 提交不占此锁.
        ASTRA_PROFILE_END(profile_lock_418);
        const auto over_total = astra::Watch<Stream>::exceeds(bytes_, encoded, limits_.bytes); // 全局已超限时不再用减法比较, 避免回绕放行.
        const auto over_stream = astra::Watch<Stream>::exceeds(stream.bytes, encoded, limits_.pending);
        if (stream.overflow || stopped_ || !streams_.find(stream.scope)->second.covers(stream.edition->version()) || over_total || over_stream) {
            lock.unlock();
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
        queue_.write(stream);
        // 页面至此已接受并计入在途. io 和 sending 仍保护缓冲寿命与认证边界;
        // 后续停止/断档按在途处理, 不能解锁后再读取索引字段或撤回已接受页面.
        lock.unlock();
        ASTRA_PROFILE_MARK(stream.profile_write);
        ASTRA_PROFILE_COUNT("star.readout.submitted_bytes", stream.encoded);
        ASTRA_PROFILE_BEGIN(submission, "star.readout.submit");
        stream.StartWrite(&stream.page);
        ASTRA_PROFILE_END(submission);
    } catch (...) {
        stream.finished = true;
        stream.Finish(gateway_.error(stream.context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Almanac page preparation failed"));
    }
}

void Readout::pump(std::chrono::steady_clock::time_point now, std::size_t maximum) {

    ASTRA_PROFILE_SCOPE("star.readout.Readout.pump");

    // 扫描期限仅由串行控制循环读写, 未到期时不取得订阅索引锁.
    if (now >= sweep_) {
        ASTRA_PROFILE_BEGIN(profile_lock_448, "star.readout.Readout.pump.wait.lock");
        const std::lock_guard lock(mutex_);
        ASTRA_PROFILE_END(profile_lock_448);
        queue_.sweep(now, sweep_, wake_);
    }

    // count 同时限制范围通知和实际推进次数, 每次释放索引锁, 不整批占锁扫描 Scope.
    for (std::size_t count = 0; count < maximum; ++count) {
        std::shared_ptr<Stream> stream; // 弹出后保活, OnDone 回收索引不会释放当前栈使用的对象.
        {
            ASTRA_PROFILE_BEGIN(profile_lock_456, "star.readout.Readout.pump.wait.lock");
            const std::lock_guard lock(mutex_);
            ASTRA_PROFILE_END(profile_lock_456);
            stream = queue_.pop(progress_);
            if (!stream) {
                break;
            }
        }
        advance(*stream, now);
    }
    ASTRA_PROFILE_BEGIN(profile_lock_464, "star.readout.Readout.pump.wait.lock");
    const std::lock_guard lock(mutex_);
    ASTRA_PROFILE_END(profile_lock_464);
    if (!queue_.idle() || progress_.pending()) {
        wake_(); // 孤立的一次无关提交也会被排空, 不依赖下一次更新或低频兜底.
    }
}

void Readout::stop() noexcept {
    const std::lock_guard lock(mutex_);
    stopped_ = true;
    for (const auto& [scope, group] : streams_) {
        static_cast<void>(scope);
        for (const auto& [address, stream] : group.streams) {
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
