#include "broadcast.hpp"
#include "catalog_edition.hpp"
#include "check.hpp"
#include "ephemeris_edition.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <unordered_set>

namespace {
using namespace std::chrono_literals;
using astra::Catalog;
using astra::Clock;
using astra::Ephemeris;
using astra::Scope;

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
    Edition missing(key, typename Projection::Point{.version = 7});
    const auto empty = missing.next("star"); // 精确缺项也保留捕获时的完整游标.
    CHECK(empty.complete() && empty.version() == 7 && empty.changes().empty());

    Edition zero(key, typename Projection::Point{.record = content(bytes("")), .version = 7});
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

    Edition large(key, typename Projection::Point{.record = content(bytes(std::string(1024 * 1024, 'x'))), .version = 8});
    const auto page = large.next("star"); // 合法单条超过软目标, 必须整体放行, 不能形成无进展空页.
    CHECK(page.complete() && page.changes_size() == 1 && page.ByteSizeLong() > 256 * 1024 && page.ByteSizeLong() < 8 * 1024 * 1024);
    Edition oversized(key, typename Projection::Point{.record = content(bytes(std::string(9 * 1024 * 1024, 'x'))), .version = 9});
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
        payload();
        boundaries<Catalog>("key", [](Catalog::Value value) { return Catalog::State::Content{1, std::move(value)}; });
        boundaries<Ephemeris>("00112233-4455-4677-8899-aabbccddeeff", [](Ephemeris::Value value) { return Ephemeris::State::Content{bytes("fixed"), std::move(value)}; });
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
