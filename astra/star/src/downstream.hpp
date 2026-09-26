#pragma once
#include "broadcast.hpp"
#include "gateway.hpp"
#include <astra/profile.hpp>
#include <astra/scope.hpp>

#include "progress.hpp"
#include "watch_kernel.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>

namespace astra {
// 动态域共用的公共下行. gRPC 回调仅提交 I/O 结果, 既有控制循环有界 pump, 不创建每订阅线程.
// State 的提交通知必须连接 changed, 才能在快照发送期间独立保留后缀而不赌全局历史窗口.
template <class Domain>
class Downstream {
    using State = typename Domain::State;            // 领域提交边界, 保持各自独立业务规则.
    using Edition = typename Domain::Edition;        // 领域专属分页, 不使用跨域 oneof.
    using Reply = typename Edition::Reply;           // 此 RPC 唯一生成响应类型.
    using Batch = Broadcast<Edition>;                // 相同订阅区间共享准备及同时在途的消息, 各流仍独立确认.
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
    // 唯一控制线程最多推进 maximum 次, 默认 32; 每次先转交至多一条范围进度, 再处理一个就绪任务. 写超时每秒扫描, 不等待网络.
    void pump(std::chrono::steady_clock::time_point now, std::size_t maximum = 32);
    // 禁止新订阅并唤醒现有流取消; 仍须继续 pump, 直到全部 OnDone 被回收.
    void stop() noexcept;
    // 到期推进失败时结束现有流的追平承诺, 不把部分推进当作完整快照; 后续新流重新尝试捕获.
    void invalidate() noexcept;
    // 已没有本服务拥有的 RPC, 只表示 OnDone 已回收, 不代表整个 Server 停止.
    bool empty() const;

    // 只读分发计数快照, 扫描是实际访问流数, 命中是进入后缀数; 仅用于归因测量, 不改变调度.
    struct Delivery {
        std::uint64_t scans{};   // changed 访问的正文目标数, 初始零, 不含空进度调度.
        std::uint64_t matched{}; // 成功进入合并后缀的次数, 初始零, 超额拒绝不计入.
    };

    // 返回累计扫描与命中; 调用只取 mutex_ 快照, 不推进发送或改变预算.
    Delivery delivery() const;

private:
    // 共享拥有 reactor 的活动流, 实现中严格分开回调状态锁和合并后缀锁.
    class Stream;
    // 立即拒绝的 reactor 不占活动索引, 由自身 OnDone 删除.
    class Rejected;

    // 仅对本范围保留最近一个弱缓存, 不以目标组合/版本数无限增长缓存索引.
    struct Group : Progress<Stream>::Group {
        std::set<Stream*> broadcast;                                   // 全范围流的非拥有视图, 每次提交都遍历.
        std::map<std::string, std::set<Stream*>, std::less<>> precise; // 精确目标到流的非拥有视图, 按变化 Key 直接定位.
        std::weak_ptr<Batch> batch;                                    // 无活动消费者时不保持正文/快照.
    };

    // 复用同目标、同基线版本与同起点的批次; 调用者不持 mutex_, 已完成领域准备.
    std::shared_ptr<Batch> freeze(Stream& stream, Edition edition, std::optional<std::uint64_t> since);
    // 已持 mutex_ 时加入侵入式就绪队列, 每流至多一个位置, 不分配.
    void enqueue(Stream& stream) noexcept;
    // I/O 回调唤醒本流, 只取得 mutex_, 不访问 State/Access.
    void signal(Stream& stream) noexcept;
    // 取得第一批完整投影; 持本流 I/O 锁, 已释放初检许可及全局队列锁; 发送前重新取得最终许可.
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
    // 保护路由、就绪队列、计费及每流后缀; 接受页面后解锁再 StartWrite, 最终认证许可仍覆盖该提交.
    mutable std::mutex mutex_;
    // 地址只保存有活动流的范围, 空范围在最后一条流退出后回收.
    std::map<Scope, Group> streams_;
    Progress<Stream> progress_;  // 只调度有提交的 Scope, 每次 pump 有界轮转, 不读 RPC 的 I/O 状态.
    astra::Watch<Stream> queue_; // 两条侵入式链分别管理就绪与在途, 每页发送不分配索引节点.
    // 活动流总数与全局已占用字节, 都由 mutex_ 保护.
    std::size_t count_{};
    std::size_t bytes_{};
    // changed 累计扫描与后缀接纳, 初始零, 只追加不重置; 测量精确订阅的索引剪枝效果.
    std::uint64_t scans_{};
    std::uint64_t matched_{};
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

        ASTRA_PROFILE_SCOPE("star.downstream.OnWriteDone");
        ASTRA_PROFILE_BEGIN(profile_lock_153, "star.downstream.OnWriteDone.wait.lock");
        const std::lock_guard lock(io);
        ASTRA_PROFILE_END(profile_lock_153);
        ASTRA_PROFILE_CONSUME(profile_write, "star.downstream.write_to_callback");
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
    ASTRA_PROFILE_STAMP(profile_write);                 // io 保护提交到完成回调的间隔, 关闭构建无成员.
    bool writing{};                                     // 一次最多一个 StartWrite, 完成回调才清除.
    bool written{};                                     // 尚未被 pump 消费的成功写入结果.
    bool failed{};                                      // gRPC 取消或失败, 单向置位.
    bool finished{};                                    // Finish 已调用, 等待 OnDone.
    bool done{};                                        // OnDone 已到达, 此后禁止访问 context.
    std::atomic_bool busy{};                            // 仅供低频超时扫描选取正在写的流.
    std::chrono::steady_clock::time_point deadline{};   // 当前页的单调写截止, io 保护.
    std::shared_ptr<const typename Batch::Page> page;   // gRPC 在 writing 时借用, 不提前 Clear/覆盖.
    std::shared_ptr<Batch> batch;                       // 共享固定批次, 不共享 RPC 的完成状态.
    std::size_t offset{};                               // 本流下一个页面序号, 每个新批次从零开始.
    std::optional<Edition> edition;                     // 唯一控制线程拥有的冻结批次, 不在回调改变.
    bool started{};                                     // 已捕获首批基线, 后续只生成 apply.
    std::uint64_t cursor{};                             // 最终完整页实际写成功的位置.
    std::size_t held{};                                 // 当前 edition 保守计费, mutex_ 保护调整.
    std::map<std::string, Event, std::less<>> pending;  // 按 Key 合并未冻结的最终操作.
    std::size_t bytes{};                                // pending 键/节点/载荷计费, mutex_ 保护.
    bool overflow{};                                    // 后缀分配/预算/连续性失败, mutex_ 内单向置位.
    bool queued{};                                      // 在就绪链表中至多一次, mutex_ 保护.
    Stream* next{};                                     // 侵入式就绪后继, 没有独立分配.
    typename astra::Watch<Stream>::Link flight;         // 独立在途双向链, 由拥有者索引锁保护, 初始未入链.
    std::unique_ptr<std::stop_callback<Cancel>> cancel; // 必须在 OnDone 返回前注销上下文借用.
    std::size_t encoded{};                              // 当前在途页逻辑字节, mutex_ 保护计费.
};

template <class Domain>
Downstream<Domain>::Downstream(State& state, Gateway& gateway, std::function<void()> wake) : Downstream(state, gateway, std::move(wake), Limits{}) {}

template <class Domain>
Downstream<Domain>::Downstream(State& state, Gateway& gateway, std::function<void()> wake, Limits limits) : state_(state), gateway_(gateway), wake_(std::move(wake)), limits_(limits) {
    astra::Watch<Stream>::validate(limits_, "Invalid Dynamic watch budget");
    if (!wake_) {
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
    queue_.enqueue(stream);
}

template <class Domain>
void Downstream<Domain>::signal(Stream& stream) noexcept {

    bool scheduled{}; // 新的队列位置需要唤醒; 已排队流由原通知或 pump 的剩余任务通知覆盖.
    {
        const std::lock_guard lock(mutex_);
        scheduled = queue_.enqueue(stream);
    }
    if (scheduled) {
        wake_(); // 不把 Wakeup 的锁竞争延伸到全体订阅的索引锁内.
    }
}

template <class Domain>
grpc::ServerWriteReactor<typename Downstream<Domain>::Reply>* Downstream<Domain>::Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request) {

    ASTRA_PROFILE_SCOPE("star.downstream.Downstream_Domain.Watch");

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
            ASTRA_PROFILE_BEGIN(profile_lock_255, "star.downstream.Downstream_Domain.Watch.wait.lock");
            const std::lock_guard lock(mutex_);
            ASTRA_PROFILE_END(profile_lock_255);
            if (stopped_ || count_ == limits_.streams) {
                return new Rejected(gateway_.error(*context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Dynamic watch capacity unavailable"));
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
        return new Rejected(gateway_.error(*context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Dynamic watch preparation failed"));
    }
}

template <class Domain>
void Downstream<Domain>::changed(const Scope& scope, const Event& change) noexcept {

    ASTRA_PROFILE_SCOPE("star.downstream.Downstream_Domain.changed");

    ASTRA_PROFILE_BEGIN(profile_lock_295, "star.downstream.Downstream_Domain.changed.wait.lock");
    const std::lock_guard lock(mutex_);
    ASTRA_PROFILE_END(profile_lock_295);
    const auto group = streams_.find(scope);
    if (group == streams_.end()) {
        return;
    }
    // 连续性和 reset 归范围所有, 精确目标跳过无关变化也不会丢失完整覆盖证据.
    progress_.publish(group->second, change.version, !change.name);
    const auto visit = [&](Stream& stream) {
        if (stream.overflow) {
            return;
        }
        ++scans_; // 只有实际访问的流才计入; 空闲精确流的跳过由 pump 心跳覆盖, 不再消耗提交锁.
        // 节点与第二份索引键也计入预算, 同 Key 高频变动只保留一个共享最终值.
        const auto cost = change.bytes + change.name->key.size() + 64;
        const auto old = stream.pending.lower_bound(change.name->key); // 一次查找同时确定已有项和新项的插入位置.
        const bool exists = old != stream.pending.end() && old->first == change.name->key;
        const auto previous = exists ? old->second.bytes + old->first.size() + 64 : 0;
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
                    old->second = Edition::merge(old->second, change); // 保留 Ephemeris 完整 Attr 的合并依据, 不重新查找或复制索引键.
                } else {
                    stream.pending.emplace_hint(old, change.name->key, change); // lower_bound 提供准确位置, 只有新键分配节点.
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
    if (change.name) {
        // 只收集合并正文; reset 与无关目标的进度由范围轮转统一通知, 不在提交内全量扫描.
        for (auto* pointer : group->second.broadcast) {
            visit(*pointer);
        }
        if (const auto bucket = group->second.precise.find(change.name->key); bucket != group->second.precise.end()) {
            for (auto* pointer : bucket->second) {
                visit(*pointer);
            }
        }
    }
    wake_();
}

template <class Domain>
typename Downstream<Domain>::Delivery Downstream<Domain>::delivery() const {

    const std::lock_guard lock(mutex_);
    return {.scans = scans_, .matched = matched_};
}

template <class Domain>
std::expected<typename Downstream<Domain>::Edition, grpc::Status> Downstream<Domain>::initial(Stream& stream) {

    ASTRA_PROFILE_SCOPE("star.downstream.Downstream_Domain.initial");

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
std::shared_ptr<typename Downstream<Domain>::Batch> Downstream<Domain>::freeze(Stream& stream, Edition edition, std::optional<std::uint64_t> since) {

    ASTRA_PROFILE_SCOPE("star.downstream.Downstream_Domain.freeze");

    ASTRA_PROFILE_BEGIN(profile_lock_381, "star.downstream.Downstream_Domain.freeze.wait.lock");
    const std::lock_guard lock(mutex_);
    ASTRA_PROFILE_END(profile_lock_381);
    auto& group = streams_.find(stream.scope)->second; // 当前流在 OnDone 回收前始终拥有该范围入口.
    if (auto batch = group.batch.lock(); batch && batch->matches(since, edition.version(), stream.target))
        return batch;

    auto batch = std::make_shared<Batch>(std::move(edition), since, stream.target);
    group.batch = batch;
    return batch;
}

template <class Domain>
void Downstream<Domain>::retire(Stream& stream) {
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

template <class Domain>
void Downstream<Domain>::advance(Stream& stream, std::chrono::steady_clock::time_point now) {

    ASTRA_PROFILE_SCOPE("star.downstream.Downstream_Domain.advance");

    // io 排除 OnDone, 保证下面取得登录许可和发起 gRPC 操作时 context 仍有效.
    ASTRA_PROFILE_BEGIN(profile_lock_414, "star.downstream.Downstream_Domain.advance.wait.io");
    const std::lock_guard io(stream.io);
    ASTRA_PROFILE_END(profile_lock_414);
    if (stream.done) {
        ASTRA_PROFILE_BEGIN(profile_lock_416, "star.downstream.Downstream_Domain.advance.wait.lock");
        const std::lock_guard lock(mutex_);
        ASTRA_PROFILE_END(profile_lock_416);
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
            ASTRA_PROFILE_BEGIN(profile_lock_429, "star.downstream.Downstream_Domain.advance.wait.lock");
            const std::lock_guard lock(mutex_);
            ASTRA_PROFILE_END(profile_lock_429);
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
        ASTRA_PROFILE_BEGIN(profile_lock_445, "star.downstream.Downstream_Domain.advance.wait.lock");
        std::unique_lock lock(mutex_); // 只保护停止/预算状态, 不跨错误编码和 gRPC 调用.
        ASTRA_PROFILE_END(profile_lock_445);
        if (stream.failed || stopped_ || stream.overflow) {
            const bool overflow = stream.overflow; // 解锁前固定拒绝原因, 后续通知仍可修改 overflow.
            lock.unlock();
            stream.finished = true;
            stream.Finish(gateway_.error(stream.context, overflow ? grpc::StatusCode::RESOURCE_EXHAUSTED : grpc::StatusCode::CANCELLED, overflow ? proto::comet::v1::REASON_BUSY : proto::comet::v1::REASON_SESSION, "Dynamic watch cannot continue"));
            return;
        }
    }

    try {
        if (stream.written) {
            stream.written = false;
            if (stream.edition->complete()) {
                stream.cursor = stream.edition->version(); // 只有完整页实际写成功才推进此流位置.
                {
                    ASTRA_PROFILE_BEGIN(profile_lock_461, "star.downstream.Downstream_Domain.advance.wait.lock");
                    const std::lock_guard lock(mutex_);
                    ASTRA_PROFILE_END(profile_lock_461);
                    bytes_ -= stream.held;
                    stream.held = 0;
                }
                stream.edition.reset();
                stream.batch.reset();
                stream.offset = 0;
            }
            stream.page.reset(); // 回收实际页分配, 不让 Clear 保留大容量却计费为零.
            {
                ASTRA_PROFILE_BEGIN(profile_lock_471, "star.downstream.Downstream_Domain.advance.wait.lock");
                const std::lock_guard lock(mutex_);
                ASTRA_PROFILE_END(profile_lock_471);
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
            const auto since = edition->reset() ? std::nullopt : stream.requested; // reset 不借用请求旧游标作为缓存键.
            stream.batch = freeze(stream, std::move(*edition), since);
            stream.edition.emplace(stream.batch->begin());
            stream.started = true;
            ASTRA_PROFILE_BEGIN(profile_lock_488, "star.downstream.Downstream_Domain.advance.wait.lock");
            const std::lock_guard lock(mutex_);
            ASTRA_PROFILE_END(profile_lock_488);
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
            const auto held = stream.edition->bytes(); // 冻结批次保守计费, 累加前先作上溢安全比较, 不以回绕通过预算.
            if (held > limits_.bytes || bytes_ > limits_.bytes - held) {
                stream.overflow = true;
            } else {
                stream.held = held;
                bytes_ += stream.held;
            }
        }

        // 后缀只在索引锁内移交一次, 排序/合并等准备转到锁外, 不阻塞 State 的提交收集器.
        std::map<std::string, Event, std::less<>> pending; // 私有旧后缀, 析构时不持 mutex_.
        std::optional<std::uint64_t> preparing;            // 本次冻结的末游标, 后续通知可以继续追加到新后缀.
        {
            ASTRA_PROFILE_BEGIN(profile_lock_514, "star.downstream.Downstream_Domain.advance.wait.lock");
            std::unique_lock lock(mutex_); // 失败路径先释放范围索引, 再提交 RPC 终止.
            ASTRA_PROFILE_END(profile_lock_514);
            const auto& group = streams_.find(stream.scope)->second; // 索引锁内固定进度与后缀的同一提交边界.
            const auto baseline = stream.edition ? stream.edition->version() : stream.cursor;
            if (stream.overflow || !group.covers(baseline)) {
                lock.unlock();
                stream.finished = true;
                stream.Finish(gateway_.error(stream.context, grpc::StatusCode::OUT_OF_RANGE, proto::comet::v1::REASON_HISTORY, "Dynamic watch requires a new baseline"));
                return;
            }
            if (!stream.edition) {
                const auto frontier = group.version; // 范围水位即全体流的推进目标, 空闲流的心跳版本以此合成.
                if (frontier <= stream.cursor)
                    return;
                preparing = frontier;
                auto cached = group.batch.lock(); // 同区间命中时连事件向量也不重复构造, 复用本锁内已定位的范围.
                if (cached && cached->prefix(stream.cursor, *preparing, stream.target)) {
                    preparing = cached->version(); // 新事件不使公共前缀失效, 更高版本继续保留在当前流后缀.
                    stream.batch = std::move(cached);
                    stream.edition.emplace(stream.batch->begin());
                }
                pending.swap(stream.pending);
                stream.held = std::exchange(stream.bytes, 0); // 私有准备仍计费, 不在锁外暂时归零绕过总预算.
                if (stream.edition) {
                    for (auto entry = pending.begin(); entry != pending.end();) {
                        if (entry->second.version <= *preparing) {
                            ++entry;
                            continue;
                        }
                        const auto cost = entry->second.bytes + entry->first.size() + 64; // 缓存只确认旧前缀, 不丢弃合并后的较新事实.
                        stream.pending.insert(pending.extract(entry++));                  // 节点移交不分配第二份键/容器节点.
                        stream.bytes += cost;
                        stream.held -= cost;
                    }
                }
            }
        }
        if (preparing) {
            if (!stream.edition) {
                std::vector<Event> replay; // 只为未命中的固定区间构造一次连续后缀.
                replay.reserve(pending.size());
                for (auto& [key, change] : pending) {
                    if (change.version > stream.cursor)
                        replay.push_back(std::move(change));
                }
                stream.batch = freeze(stream, Edition(*preparing, std::move(replay), stream.target), stream.cursor);
                stream.edition.emplace(stream.batch->begin());
            }
            pending.clear(); // 归还节点/键发生在事件索引锁外.
            ASTRA_PROFILE_BEGIN(profile_lock_562, "star.downstream.Downstream_Domain.advance.wait.lock");
            const std::lock_guard lock(mutex_);
            ASTRA_PROFILE_END(profile_lock_562);
            bytes_ -= stream.held;
            stream.held = 0;
            const auto held = stream.edition->bytes(); // 私有后缀同样逐流保守计费, 比较时避免无符号回绕.
            if (held > limits_.bytes || bytes_ > limits_.bytes - held) {
                stream.overflow = true;
            } else {
                stream.held = held;
                bytes_ += stream.held; // 即使物理共享也逐流保守计费, 慢订阅不能免费无限积压.
            }
        }
        // 全局锁外编码单页, 每轮只准备这一份有界暂存. Permit 不跨任何网络等待.
        stream.page = stream.batch->next(*stream.edition, stream.offset++, gateway_.instance());
        const auto encoded = stream.page->bytes;       // 单次完整大小计算, 同时计入在途与全局保有预算.
        auto sending = gateway_.enter(stream.context); // 只覆盖最终检查和非阻塞 StartWrite, 不跨编码或网络等待.
        if (!sending) {
            stream.page.reset();
            stream.finished = true;
            stream.Finish(sending.error());
            return;
        }
        ASTRA_PROFILE_BEGIN(profile_lock_583, "star.downstream.Downstream_Domain.advance.wait.lock");
        std::unique_lock lock(mutex_); // 最终接受页面与计费同锁完成, gRPC 提交不占此锁.
        ASTRA_PROFILE_END(profile_lock_583);
        const auto over_total = astra::Watch<Stream>::exceeds(bytes_, encoded, limits_.bytes); // 全局已超限时不再用减法比较, 避免回绕放行.
        const auto over_stream = astra::Watch<Stream>::exceeds(stream.bytes, encoded, limits_.pending);
        if (stream.overflow || stopped_ || !streams_.find(stream.scope)->second.covers(stream.edition->version()) || over_total || over_stream) {
            lock.unlock();
            stream.page.reset(); // 此页尚未进入在途, 不保留未计费的大编码缓冲.
            stream.finished = true;
            stream.Finish(gateway_.error(stream.context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Dynamic send budget unavailable"));
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
        ASTRA_PROFILE_COUNT("star.downstream.submitted_bytes", stream.page->bytes);
        ASTRA_PROFILE_BEGIN(submission, "star.downstream.submit");
        stream.StartWrite(&stream.page->message);
        ASTRA_PROFILE_END(submission);
    } catch (...) {
        stream.finished = true;
        stream.Finish(gateway_.error(stream.context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Dynamic page preparation failed"));
    }
}

template <class Domain>
void Downstream<Domain>::pump(std::chrono::steady_clock::time_point now, std::size_t maximum) {

    ASTRA_PROFILE_SCOPE("star.downstream.Downstream_Domain.pump");

    // 扫描期限仅由串行控制循环读写, 未到期时不取得订阅索引锁.
    if (now >= sweep_) {
        ASTRA_PROFILE_BEGIN(profile_lock_614, "star.downstream.Downstream_Domain.pump.wait.lock");
        const std::lock_guard lock(mutex_);
        ASTRA_PROFILE_END(profile_lock_614);
        queue_.sweep(now, sweep_, wake_);
    }

    // count 同时限制范围通知和实际推进次数, 每次释放索引锁, 不整批占锁扫描 Scope.
    for (std::size_t count = 0; count < maximum; ++count) {
        std::shared_ptr<Stream> stream; // 弹出后保活, OnDone 回收索引不会释放当前栈使用的对象.
        {
            ASTRA_PROFILE_BEGIN(profile_lock_622, "star.downstream.Downstream_Domain.pump.wait.lock");
            const std::lock_guard lock(mutex_);
            ASTRA_PROFILE_END(profile_lock_622);
            stream = queue_.pop(progress_);
            if (!stream) {
                break;
            }
        }
        advance(*stream, now);
    }
    ASTRA_PROFILE_BEGIN(profile_lock_630, "star.downstream.Downstream_Domain.pump.wait.lock");
    const std::lock_guard lock(mutex_);
    ASTRA_PROFILE_END(profile_lock_630);
    if (!queue_.idle() || progress_.pending()) {
        wake_(); // 孤立的一次无关提交也会被排空, 不依赖下一次更新或低频兜底.
    }
}

template <class Domain>
void Downstream<Domain>::stop() noexcept {
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
        for (const auto& [address, stream] : group.streams) {
            static_cast<void>(address);
            stream->overflow = true;
            enqueue(*stream);
        }
    }
    wake_();
}

} // namespace astra
