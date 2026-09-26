#include "broadcast.hpp"
#include "catalog_edition.hpp"
#include "check.hpp"
#include "ephemeris_edition.hpp"
#include "progress.hpp"
#include "watch_kernel.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <unordered_set>

namespace {
using namespace std::chrono_literals;
using astra::Catalog;
using astra::Clock;
using astra::Ephemeris;
using astra::Scope;

// 只提供调度所需字段, 不依赖 gRPC; 拥有者必须在两个侵入式索引清除后才释放对象.
struct Pending : std::enable_shared_from_this<Pending> {
    bool queued{};                      // 就绪标志, 初始未入队.
    Pending* next{};                    // 就绪后继, 与在途链独立.
    std::atomic_bool busy{true};        // 模拟未完成的网络写入, false 时扫描不能再次唤醒.
    astra::Watch<Pending>::Link flight; // 在途链, 初始为空.
};

// 重复入队、任意摘除、超时扫描及取消后的残留就绪通知互不破坏生命周期.
void scheduling() {

    astra::Watch<Pending> queue;              // 本例单线程模拟拥有者索引锁.
    auto first = std::make_shared<Pending>(); // 三个独立拥有节点, 用于覆盖头/中间/尾删除.
    auto middle = std::make_shared<Pending>();
    auto last = std::make_shared<Pending>();
    const auto now = std::chrono::steady_clock::now(); // 固定时间基线, 测试不实际休眠.
    auto deadline = now;                               // 首次扫描立即到期.
    unsigned wakes{};                                  // 扫描要求调度的次数, 无就绪流时不得增加.
    auto wake = [&] { ++wakes; };                      // 不重入 queue, 与生产唤醒约束一致.
    static_assert(noexcept(queue.write(*first)) && noexcept(queue.settle(*first)));
    CHECK(queue.idle() && !queue.pop());

    for (const auto& stream : {first, middle, last}) {
        CHECK(queue.enqueue(*stream));  // 新位置需要通知控制线程.
        CHECK(!queue.enqueue(*stream)); // 同位置重复回调只更新状态, 不重复唤醒.
        queue.write(*stream);
        queue.write(*stream);
    }
    CHECK(queue.pop() == first && queue.pop() == middle && queue.pop() == last && queue.idle());
    CHECK(queue.enqueue(*first)); // 已被弹出后到达的新回调必须重新排队, 不能因历史通知而漏掉.
    CHECK(!queue.enqueue(*first) && queue.pop() == first && queue.idle());
    queue.settle(*middle); // 移除中间节点, 其余两个仍可扫描.
    queue.settle(*middle); // 幂等清理不误摘除邻居.
    queue.sweep(now, deadline, wake);
    CHECK(wakes == 1 && queue.pop() == last && queue.pop() == first && queue.idle());
    queue.sweep(now, deadline, wake);
    CHECK(wakes == 1 && queue.idle());

    queue.settle(*last);  // 摘除链头.
    queue.settle(*first); // 摘除最后一项.
    queue.sweep(now + 1s, deadline, wake);
    CHECK(wakes == 1 && queue.idle());
    queue.write(*middle); // 已清理节点允许再利用.
    middle->busy.store(false);
    queue.sweep(now + 2s, deadline, wake);
    CHECK(wakes == 1 && queue.idle());
    middle->busy.store(true);
    queue.sweep(now + 3s, deadline, wake);
    queue.settle(*middle);   // OnDone 先摘除在途, 就绪通知仍保留供拥有者收尾.
    auto held = queue.pop(); // pop 返回强引用, 不借用将被删除的拥有索引.
    middle.reset();
    CHECK(held && !held->queued && !held->next && !held->flight.previous && !held->flight.next);
    CHECK(queue.idle());
    CHECK(!astra::Watch<Pending>::exceeds(3, 4, 7));
    CHECK(astra::Watch<Pending>::exceeds(8, 0, 7));
    CHECK(astra::Watch<Pending>::exceeds(1, std::numeric_limits<std::size_t>::max(), 7));
}

// 范围通知与就绪弹出共用一次有界调度, 覆盖空队列、重复通知、FIFO、公平性和取消后的寿命.
void dispatch() {

    astra::Watch<Pending> queue;               // 模拟同一索引锁下的就绪队列, 不启动线程或网络.
    astra::Progress<Pending> progress;         // 范围通知器, 每次 pop 最多消费一个进度位置.
    astra::Progress<Pending>::Group one, two;  // 两个独立范围, 地址在整个用例中保持稳定.
    auto first = std::make_shared<Pending>();  // 第一个范围的订阅.
    auto second = std::make_shared<Pending>(); // 第二个范围的订阅.
    one.streams.emplace(first.get(), first);
    two.streams.emplace(second.get(), second);
    static_assert(noexcept(queue.pop(progress)));
    CHECK(!queue.pop(progress));

    // 没有正文命中时仅产生范围进度, 必须在当前调度返回流, 不能留给下一轮.
    progress.publish(one, 1, false);
    CHECK(queue.idle() && progress.pending());
    CHECK(queue.pop(progress) == first);
    CHECK(queue.idle() && !progress.pending());

    // 正文与进度指向同一已就绪流时合并, 不在完成一次推进后再排一次相同通知.
    queue.enqueue(*first);
    progress.publish(one, 2, false);
    CHECK(queue.pop(progress) == first);
    CHECK(!queue.pop(progress) && !first->queued);

    // 已就绪的其他流保持 FIFO 优先级. 进度仍只消费一项, 新通知不会插到队首.
    queue.enqueue(*second);
    progress.publish(one, 3, false);
    CHECK(queue.pop(progress) == second);
    CHECK(first->queued && !progress.pending());
    CHECK(queue.pop(progress) == first && queue.idle());

    // 一次额度只推进一个范围, 新提交排到另一个待处理范围之后, 不能独占控制轮.
    progress.publish(one, 4, false);
    progress.publish(two, 1, false);
    CHECK(queue.pop(progress) == first && progress.pending());
    progress.publish(one, 5, false);
    CHECK(queue.pop(progress) == second && progress.pending());
    CHECK(queue.pop(progress) == first && !progress.pending());

    // 尚未转交的已取消流可直接摘除; 已弹出流由强引用保活, 不借用随后的索引删除.
    progress.publish(two, 2, false);
    progress.erase(two, second.get());
    CHECK(!queue.pop(progress));
    progress.publish(one, 6, true);
    auto held = queue.pop(progress);         // 模拟 advance 持有的局部强引用.
    std::weak_ptr<Pending> lifetime = first; // 只观察寿命, 不延长已取消对象的存活.
    progress.erase(one, first.get());
    first.reset();
    CHECK(held && !lifetime.expired() && queue.idle() && !progress.pending());
    CHECK(!held->queued && !held->next);
    held.reset();
    CHECK(lifetime.expired());
}

// Scope 轮转只依赖拥有索引, 不借用 RPC 状态; 覆盖慢范围、公平调度、取消及遍历期间的新提交.
void progress() {

    using Queue = astra::Progress<unsigned>; // 测试载荷只标识订阅, 不提供 writing/done, 防止通知器耦合 I/O 状态.
    Queue queue;                             // 所有操作模拟在同一个事件索引锁内执行.
    Queue::Group zero;                       // 完整空范围的版本零不能被当作未观察到通知.
    queue.publish(zero, 0, true);
    CHECK(zero.covers(0) && !queue.pending());
    queue.publish(zero, 1, false);
    CHECK(zero.covers(0) && !queue.pending());

    Queue::Group large, small; // 地址稳定的两个范围, 独立版本不会互相污染.
    for (unsigned index = 0; index < 3; ++index) {
        auto stream = std::make_shared<unsigned>(index); // 同范围三个独立消费者.
        large.streams.emplace(stream.get(), stream);
    }
    auto solitary = std::make_shared<unsigned>(9); // 小范围不能被大范围的剩余订阅饿死.
    small.streams.emplace(solitary.get(), solitary);
    queue.publish(large, 10, false);
    queue.publish(small, 40, false);
    CHECK(!large.covers(8) && large.covers(9) && small.covers(39));
    auto* first = queue.take(); // 指针排序只决定本范围顺序, 测试不依赖分配器返回地址.
    CHECK(first && large.streams.contains(first));
    CHECK(queue.take() == solitary.get());

    queue.publish(large, 11, false);        // 已被调度的第一个消费者必须还能获知后来的提交.
    std::map<unsigned*, unsigned> observed; // 按实际拥有指针统计, 非本范围的重复出队也会失败.
    for (unsigned remaining = 8; queue.pending(); --remaining) {
        CHECK(remaining != 0); // 队列必须自行排空, 不依赖下一次业务写入.
        auto* stream = queue.take();
        CHECK(stream && large.streams.contains(stream));
        ++observed[stream];
    }
    CHECK(observed.size() == 3 && observed[first] == 1 && queue.take() == nullptr);
    CHECK(large.covers(10));

    queue.publish(large, 12, false);
    queue.publish(small, 41, false);
    queue.erase(large, large.streams.begin()->first); // 删除扫描将要访问的节点, 迭代器必须安全推进.
    queue.erase(small, solitary.get());               // 删除队尾最后一条流, Scope 可立即被拥有者销毁.
    while (!large.streams.empty()) {
        queue.erase(large, large.streams.begin()->first);
    }
    CHECK(!queue.pending() && queue.take() == nullptr);

    queue.publish(small, 43, false); // 缺 42, 不得把目标过滤误认为拥有完整历史.
    CHECK(!small.covers(41) && small.covers(43));
    queue.publish(small, 44, true); // 全量替换边界之后的基线才可以继续 apply.
    CHECK(!small.covers(43) && small.covers(44));
    queue.publish(small, UINT64_MAX, true);
    CHECK(small.covers(UINT64_MAX) && !small.covers(UINT64_MAX - 1));
    CHECK(!queue.pending()); // 空范围只更新标量, 不入通知队列.
}

// 业务缓冲保留不可变所有权, 空载荷仍是合法记录.
Catalog::Value bytes(std::string_view value) {
    return std::make_shared<const Catalog::Buffer>(value.begin(), value.end());
}

// 真实领域分页覆盖整 Scope、多页、快慢读者、复制位置和页回收, 不将相同正文误认为相同网络确认.
template <typename Domain>
void pages(auto&& write) {

    using Edition = typename Domain::Edition;
    using Batch = astra::Broadcast<Edition>;
    typename Domain::State state([] { return std::optional(Clock::Reading{.time = Clock::Time(1s), .ready = true}); }, {});
    const Scope scope{"shared", "main"}; // 600 行强制产生三页, 让多个在途页同时被快慢流交错访问.
    for (unsigned index = 0; index < 600; ++index)
        CHECK(write(state, scope, index));
    auto view = state.capture(scope);
    CHECK(view && view->size() == 600);
    Batch batch(Edition(std::move(*view)), std::nullopt, "");
    CHECK(batch.matches(std::nullopt, 600, "") && !batch.matches(0, 600, "") && !batch.matches(std::nullopt, 599, "") && !batch.matches(std::nullopt, 600, "other"));
    auto first = batch.begin(), second = batch.begin(), slow = batch.begin(); // 三个独立位置, 数据来源共享.
    auto one = batch.next(first, 0, "star");
    auto same = batch.next(second, 0, "star");
    CHECK(one == same && one->message.changes_size() == 256 && !first.complete() && !second.complete());
    CHECK(one->bytes == one->message.ByteSizeLong());
    auto middle = batch.next(first, 1, "star"); // 页 0 的发送者尚未完成, 快流已经进入第二页.
    CHECK(middle->message.changes_size() == 256 && !first.complete());
    auto end = batch.next(first, 2, "star");
    CHECK(first.complete() && !second.complete() && end->message.complete() && end->message.changes_size() == 88);
    auto lagged = batch.next(slow, 0, "star"); // 页 1/2 的出现不能覆盖仍在使用的页 0.
    CHECK(lagged == one);
    CHECK(batch.next(second, 1, "star") == middle && !second.complete());
    auto second_end = batch.next(second, 2, "star");
    CHECK(second.complete() && second_end == end);
    const auto encoded = end->message.SerializeAsString(); // 仅测试保留正文副本, 不持有缓存页.
    std::weak_ptr<const typename Batch::Page> released = second_end;
    second_end.reset();
    end.reset();
    CHECK(released.expired()); // 弱缓存不独占持有整个编码页, 取消/完成的消息可以立即释放.
    CHECK(batch.next(slow, 1, "star") == middle);
    const auto rebuilt = batch.next(slow, 2, "star"); // 页槽还在但正文已释放, 从该流自己的位置重新构造.
    CHECK(rebuilt->message.SerializeAsString() == encoded && slow.complete());
    auto retry = batch.begin(); // 已经发送结束不改变固定起点, 新消费者仍能读取原基线.
    auto replay = batch.next(retry, 0, "star");
    CHECK(replay == one && !retry.complete());
    CHECK(batch.misses() == 4 && batch.hits() == 6); // 三页首次构造加一次释放后重建, 其余六次均为弱缓存命中.
}

// 2 KiB 二进制正文按字节预算分页, 缓存命中不能截断零字节、漏项或串用其他页内容.
void payload() {

    using Edition = Catalog::Edition;
    Catalog::State state([] { return std::optional(Clock::Reading{.time = Clock::Time(1s), .ready = true}); }, {});
    const Scope scope{"payload", "main"}; // 384 项超过一页字节上限, 不依赖 256 条数量上限触发分页.
    std::string value(2048, '\0');        // 包含零字节及高位字节, 不能按文本终止符截取.
    for (std::size_t index = 0; index < value.size(); ++index)
        value[index] = static_cast<char>(index % 256);
    const auto content = bytes(value); // 各 Key 共享不可变输入, 实际 protobuf 每项仍完整编码 2048 字节.
    for (unsigned index = 0; index < 384; ++index)
        CHECK(state.publish(scope, std::to_string(index), content, 1, 1000));
    auto captured = state.capture(scope);
    CHECK(captured && captured->size() == 384);
    astra::Broadcast<Edition> batch(Edition(std::move(*captured)), std::nullopt, "");
    auto first = batch.begin(), second = batch.begin(); // 两条流独立采纳完整位置, 一条返回不代表另一条网络完成.
    std::unordered_set<std::string> seen;               // 验证所有键恰好一次, 不以消息数量代替内容核对.
    std::size_t offset{};
    while (!first.complete()) {
        const auto page = batch.next(first, offset, "star");
        CHECK(page == batch.next(second, offset++, "star"));
        CHECK(page->bytes <= 256 * 1024 && page->message.changes_size() > 0);
        for (const auto& change : page->message.changes())
            CHECK(change.has_value() && change.value() == value && change.version() == 1 && seen.insert(change.key()).second);
    }
    CHECK(offset >= 4 && seen.size() == 384 && second.complete());
}

// 相同机制在两种真实策略下运行: 空目标, 复制位置, 软预算大行, 硬预算失败及坏批次都不混淆领域.
template <typename Domain>
void boundaries(std::string key, auto&& content) {

    using Edition = typename Domain::Edition;              // 仍通过领域入口验证, 不向测试暴露私有 Encoding.
    using Projection = typename Domain::State::Projection; // 缺项和完整点查采用真实公开结构.
    Edition missing(key, typename Projection::Point{.name = {}, .record = {}, .version = 7});
    const auto empty = missing.next("star"); // 精确缺项也保留捕获时的完整游标.
    CHECK(empty.complete() && empty.version() == 7 && empty.changes().empty());

    Edition zero(key, typename Projection::Point{.name = {}, .record = content(bytes("")), .version = 7});
    auto copy = zero; // 冻结来源共享, 页位置按值复制, 失败不消费原对象.
    bool invalid{};
    try {
        static_cast<void>(zero.next(""));
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    CHECK(invalid && !zero.complete());
    const auto present = zero.next("star");
    CHECK(present.complete() && present.changes_size() == 1 && !copy.complete());
    CHECK(present.SerializeAsString() == copy.next("star").SerializeAsString());

    Edition large(key, typename Projection::Point{.name = {}, .record = content(bytes(std::string(1024 * 1024, 'x'))), .version = 8});
    const auto page = large.next("star"); // 合法单条超过软目标, 必须整体放行, 不能形成无进展空页.
    CHECK(page.complete() && page.changes_size() == 1 && page.ByteSizeLong() > 256 * 1024 && page.ByteSizeLong() < 8 * 1024 * 1024);
    Edition oversized(key, typename Projection::Point{.name = {}, .record = content(bytes(std::string(9 * 1024 * 1024, 'x'))), .version = 9});
    bool exceeded{}; // 人工破坏上层单正文限制, 分页硬预算仍须兜底并保持位置未完成.
    try {
        static_cast<void>(oversized.next("star"));
    } catch (const std::length_error&) {
        exceeded = true;
    }
    CHECK(exceeded && !oversized.complete() && oversized.version() == 9);

    bool rejected{}; // 缺名称的非法事件不能靠精确过滤消失, 必须先校验整批.
    try {
        Edition damaged(1, std::vector<typename Projection::Event>(1), key);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

// 共享后缀压缩只统一顺序/遍历, 不把 Catalog 的末值规则误用于 Ephemeris 的 Attr 基线.
void collapse() {

    const Scope scope{"collapse", "main"}; // 两域地址相同, 存储和内容游标仍各自独立.
    Catalog::State catalog([] { return std::optional(Clock::Reading{.time = Clock::Time(1s), .ready = true}); }, {});
    CHECK(catalog.publish(scope, "key", bytes("old"), 10, 1000));
    CHECK(catalog.publish(scope, "other", bytes("separate"), 3, 1000));
    CHECK(catalog.publish(scope, "key", bytes("new"), 20, 1000));
    auto changes = catalog.changes(scope, 0); // 故意逆序输入, 后缀按 Key/游标恢复顺序后再折叠.
    CHECK(changes && changes->size() == 3);
    std::ranges::reverse(*changes);
    Catalog::Edition exact(3, *changes, "key"), unrelated(3, *changes, "missing");
    const auto value = exact.next("star");
    CHECK(value.complete() && value.version() == 3 && value.changes_size() == 1);
    CHECK(value.changes(0).key() == "key" && value.changes(0).version() == 20 && value.changes(0).value() == "new");
    const auto empty = unrelated.next("star");
    CHECK(empty.complete() && empty.version() == 3 && empty.changes().empty());

    Ephemeris::State ephemeris([] { return std::optional(Clock::Reading{.time = Clock::Time(1s), .ready = true}); }, {});
    const auto record = ephemeris.create(scope, bytes("fixed"), bytes("old"), 1000);
    CHECK(record && ephemeris.update(scope, record->uuid, bytes("new"), 1));
    auto events = ephemeris.changes(scope, 0);
    CHECK(events && events->size() == 2);
    std::ranges::reverse(*events);
    Ephemeris::Edition full(2, std::move(*events)); // Create+Data 仍须输出完整 Attr/Data.
    CHECK(ephemeris.remove(scope, record->uuid));   // 构造后的删除不能污染先前已冻结内容.
    const auto captured = full.next("star");
    CHECK(captured.changes_size() == 1 && captured.changes(0).has_record());
    CHECK(captured.changes(0).record().attr() == "fixed" && captured.changes(0).record().data() == "new");
    events = ephemeris.changes(scope, 0);
    CHECK(events && events->size() == 3);
    Ephemeris::Edition removed(3, std::move(*events));
    const auto erased = removed.next("star");
    CHECK(erased.complete() && erased.version() == 3 && erased.changes_size() == 1 && erased.changes(0).has_erase());
}

// Ephemeris 的相同 UUID 在不同游标区间需要不同 action, 不能用同一末版本就复用 Data-only.
void delta() {

    Ephemeris::State state([] { return std::optional(Clock::Reading{.time = Clock::Time(1s), .ready = true}); }, {});
    const Scope scope{"delta", "main"};
    const auto record = state.create(scope, bytes("fixed"), bytes("old"), 1000);
    CHECK(record && state.update(scope, record->uuid, bytes("new"), 1));
    auto all = state.changes(scope, 0), changes = state.changes(scope, 1);
    CHECK(all && changes);
    astra::Broadcast<Ephemeris::Edition> baseline(Ephemeris::Edition(2, std::move(*all)), 0, "");
    astra::Broadcast<Ephemeris::Edition> update(Ephemeris::Edition(2, std::move(*changes)), 1, "");
    CHECK(!baseline.matches(1, 2, "") && update.matches(1, 2, ""));
    CHECK(update.prefix(1, 3, "") && !update.prefix(0, 3, "") && !update.prefix(2, 3, "") && !update.prefix(1, 1, "") && !update.prefix(1, 3, record->uuid));
    auto first = baseline.begin(), second = update.begin(), other = update.begin();
    const auto full = baseline.next(first, 0, "star");
    const auto data = update.next(second, 0, "star");
    CHECK(full->message.changes(0).has_record() && data->message.changes(0).has_data());
    CHECK(data == update.next(other, 0, "star"));

    // 完成后误用同一位置仍须报错, 不因缓存命中绕过 Edition 的重复消费保护.
    bool rejected{};
    try {
        static_cast<void>(update.next(second, 1, "star"));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    CHECK(rejected);
}

// 精确未知目标生成合法空 reset, 不和全范围或另一个精确目标共享缓存.
void empty() {

    using Edition = Catalog::Edition;
    astra::Broadcast<Edition> batch(Edition("missing", {}), std::nullopt, "missing");
    auto first = batch.begin(), second = batch.begin();
    bool rejected{}; // 非连续页号不能扩容成稀疏缓存, 失败不能提前消费合法的第一页.
    try {
        static_cast<void>(batch.next(first, std::numeric_limits<std::size_t>::max(), "star"));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    CHECK(rejected && !first.complete());
    const auto page = batch.next(first, 0, "star");
    CHECK(page->message.complete() && page->message.changes_size() == 0 && first.complete());
    CHECK(batch.next(second, 0, "star") == page && second.complete());
    CHECK(!batch.matches(std::nullopt, 0, "") && !batch.matches(std::nullopt, 0, "another"));
}
} // namespace

// 纯组件测试不启动网络, 真实流的取消/慢消费者/在途页由既有 RPC 回归继续覆盖.
int main() {

    try {
        pages<Catalog>([](auto& state, const Scope& scope, unsigned index) { return state.publish(scope, std::to_string(index), bytes("value"), 1, 1000); });
        pages<Ephemeris>([](auto& state, const Scope& scope, unsigned) { return state.create(scope, bytes("attr"), bytes("data"), 1000); });
        scheduling();
        dispatch();
        progress();
        payload();
        boundaries<Catalog>("key", [](Catalog::Value value) { return Catalog::State::Content{1, std::move(value)}; });
        boundaries<Ephemeris>(std::string("\x00\x11\x22\x33\x44\x55\x46\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff", 16), [](Ephemeris::Value value) { return Ephemeris::State::Content{bytes("fixed"), std::move(value)}; });
        collapse();
        delta();
        empty();
        std::cout << "Shared publication: ok\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
