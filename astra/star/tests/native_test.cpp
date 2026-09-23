#include "agenda.hpp"
#include "catalog.hpp"
#include "check.hpp"
#include "ephemeris.hpp"
#include "pages.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace std::chrono_literals;
using astra::Agenda;
using astra::Catalog;
using astra::Clock;
using astra::Ephemeris;

// 确定性绝对时间, 不读取系统墙钟或启动 Pulsar; 单位纳秒, 默认 1 s.
Clock::Reading clock(std::chrono::nanoseconds time = 1s) {
    return Clock::Reading{.time = Clock::Time(time), .ready = true, .synchronized = false};
}

// 拥有不可变字节, 空文本仍构造非空 Value, 不与水位混淆.
Catalog::Value bytes(std::string_view value) {
    return std::make_shared<const Catalog::Buffer>(value.begin(), value.end());
}

// 判断 expected 的明确错误, 不通过异常或消息文本推测提交结果.
template <typename T, typename E>
void error(const std::expected<T, E>& result, E expected) {
    CHECK(!result && result.error() == expected);
}

// 原生注册只保留固定 Attr、动态 Data 和单期限, 两种 order 不互相阻塞.
void ephemeris() {

    const auto attr = bytes("fixed");   // 不变属性, 所有后续候选必须保留同一共享引用.
    const auto data = bytes("initial"); // 初始正文, 未受理候选不能改坏它.
    auto initial = Ephemeris::create(attr, data, 1000, clock());
    CHECK(initial && initial->deadline == Clock::Time(2s) && initial->update == 0 && initial->renewal == 0);
    CHECK(initial->attr == attr && initial->data == data && initial->ttl == 1000);

    const auto changed = Ephemeris::update(*initial, bytes("next"), 17, clock(1200ms));
    CHECK(changed && changed->changed && changed->visible);
    CHECK(changed->record.deadline == initial->deadline && changed->record.attr == attr && changed->record.update == 17 && changed->record.renewal == 0);
    CHECK(initial->data == data && initial->update == 0); // 返回候选本身不提交状态.
    const auto repeated = Ephemeris::update(changed->record, bytes("next"), 17, clock(1300ms));
    CHECK(repeated && !repeated->changed && !repeated->visible);
    error(Ephemeris::update(changed->record, bytes("old"), 16, clock()), Ephemeris::Error::obsolete);
    error(Ephemeris::update(changed->record, bytes("different"), 17, clock()), Ephemeris::Error::conflict);
    const auto same = Ephemeris::update(changed->record, bytes("next"), 18, clock());
    CHECK(same && same->changed && !same->visible && same->record.data == changed->record.data);

    const auto renewed = Ephemeris::renew(changed->record, 4, clock(1500ms));
    CHECK(renewed && renewed->changed && !renewed->visible && renewed->record.deadline == Clock::Time(2500ms));
    CHECK(renewed->record.update == 17 && renewed->record.renewal == 4 && renewed->record.ttl == 1000);
    const auto retry = Ephemeris::renew(renewed->record, 4, clock(2000ms));
    CHECK(retry && !retry->changed && retry->record.deadline == renewed->record.deadline);
    error(Ephemeris::renew(renewed->record, 3, clock()), Ephemeris::Error::obsolete);
    error(Ephemeris::renew(renewed->record, 4, clock(2500ms)), Ephemeris::Error::ended);
    error(Ephemeris::update(renewed->record, bytes("next"), 17, clock(2500ms)), Ephemeris::Error::ended);

    const auto empty = Ephemeris::create(bytes(""), bytes(""), 600000, clock());
    CHECK(empty && empty->attr && empty->data && empty->attr->empty() && empty->data->empty());
    for (const auto ttl : {0U, 999U, 600001U, UINT32_MAX}) {
        error(Ephemeris::create(attr, data, ttl, clock()), Ephemeris::Error::input);
    }
    error(Ephemeris::create(nullptr, data, 1000, clock()), Ephemeris::Error::input);
    error(Ephemeris::update(*initial, data, 0, clock()), Ephemeris::Error::input);
    error(Ephemeris::renew(*initial, 0, clock()), Ephemeris::Error::input);
    error(Ephemeris::create(attr, data, 1000, Clock::Reading{}), Ephemeris::Error::clock);
    error(Ephemeris::renew(*initial, 1, Clock::Reading{}), Ephemeris::Error::clock);
    error(Ephemeris::create(attr, data, 1000, clock(-1ns)), Ephemeris::Error::input);

    auto maximum = *initial; // 极大顺序仍可确认, 不通过 +1 回绕计算合法性.
    maximum.update = UINT64_MAX;
    maximum.renewal = UINT64_MAX;
    CHECK(Ephemeris::update(maximum, data, UINT64_MAX, clock()));
    CHECK(Ephemeris::renew(maximum, UINT64_MAX, clock()));
    error(Ephemeris::update(maximum, data, UINT64_MAX - 1, clock()), Ephemeris::Error::obsolete);
    const auto edge = clock(std::chrono::nanoseconds(INT64_MAX) - 500ms); // 有限截止不足 1 s 的空间.
    error(Ephemeris::create(attr, data, 1000, edge), Ephemeris::Error::exhausted);
    maximum.deadline = Clock::Time(std::chrono::nanoseconds(INT64_MAX));
    CHECK(Ephemeris::renew(maximum, UINT64_MAX, edge)); // 同号重试不执行一次无意义的溢出相加.
    maximum.renewal = UINT64_MAX - 1;
    error(Ephemeris::renew(maximum, UINT64_MAX, edge), Ephemeris::Error::exhausted);
}

// 字符串契约含版本和 variant, 生成只发生在创建阶段, 后续检查不格式化.
void uuid() {

    const std::string valid = "00112233-4455-4677-8899-aabbccddeeff";
    CHECK(Ephemeris::valid(valid));
    for (const auto text : {"", "00112233445546778899aabbccddeeff", "00112233-4455-4677-8899-AABBCCDDEEFF", "00112233-4455-1677-8899-aabbccddeeff", "00112233-4455-4677-7899-aabbccddeeff", "00112233-4455-4677-c899-aabbccddeeff"}) {
        CHECK(!Ephemeris::valid(text));
    }
    std::set<std::string> generated; // 有界样本只检验格式和碰撞检查路径, 不声称证明随机源统计性质.
    for (unsigned sample = 0; sample != 64; ++sample) {
        auto value = Ephemeris::uuid();
        CHECK(Ephemeris::valid(value));
        CHECK(generated.emplace(std::move(value)).second);
    }
}

// 业务版本与来源期限独立, 不把远端的更晚截止借给本机新来源.
void catalog() {

    const auto value = bytes("one"); // 版本 1 的唯一正文.
    const auto initial = Catalog::publish(nullptr, nullptr, value, 1, 1000, clock());
    CHECK(initial && initial->deadline == Clock::Time(2s));
    const auto first = Catalog::merge(nullptr, *initial, Clock::Time(1s));
    CHECK(first && first->visible && first->record.version == 1);

    const Catalog::Record distant{1, value, Clock::Time(100s)}; // 其他来源持有更长期限.
    const auto own = Catalog::publish(nullptr, &distant, bytes("one"), 1, 1000, clock());
    CHECK(own && own->value == value && own->deadline == Clock::Time(2s));
    error(Catalog::renew(nullptr, &distant, 1, 1000, clock()), Catalog::Error::ended);
    const auto joined = Catalog::merge(&distant, *own, Clock::Time(1s));
    CHECK(joined && !joined->visible && joined->record.deadline == distant.deadline);
    const auto shorter = Catalog::renew(&distant, &distant, 1, 1000, clock());
    CHECK(shorter && shorter->deadline == distant.deadline); // 同来源同版本不得缩短已确认截止.

    const Catalog::Record floor{8, nullptr, std::nullopt}; // 更高事实即使只剩水位也阻止回退.
    error(Catalog::publish(&*initial, &floor, value, 1, 1000, clock()), Catalog::Error::version);
    error(Catalog::renew(&*initial, &floor, 1, 1000, clock()), Catalog::Error::version);
    const auto hidden = Catalog::merge(&distant, floor, Clock::Time(1s));
    CHECK(hidden && hidden->visible && hidden->record.version == 8 && !hidden->record.value);
    const auto old = Catalog::merge(&hidden->record, distant, Clock::Time(1s));
    CHECK(old && !old->visible && old->record.version == 8 && !old->record.value);
    const auto recovered = Catalog::publish(&floor, &floor, bytes("eight"), 8, 1000, clock());
    CHECK(recovered && recovered->value && recovered->deadline == Clock::Time(2s));
    const auto visible = Catalog::merge(&floor, *recovered, Clock::Time(1s));
    CHECK(visible && visible->visible && visible->record.version == 8);

    const Catalog::Record equal{1, nullptr, std::nullopt}; // 等版本水位/源端过期不能撤销另一份有效正文.
    const auto kept = Catalog::merge(&distant, equal, Clock::Time(2s));
    CHECK(kept && !kept->visible && kept->record.value == value && kept->record.deadline == distant.deadline);
    const auto expired = Catalog::merge(&*initial, equal, Clock::Time(2s));
    CHECK(expired && expired->visible && !expired->record.value && expired->record.version == 1);
    error(Catalog::renew(&*initial, &distant, 1, 1000, clock(2s)), Catalog::Error::ended);
    const auto released = Catalog::expire(*initial, Clock::Time(2s));
    CHECK(released.version == 1 && !released.value && !released.deadline);
    CHECK(Catalog::expire(distant, Clock::Time(2s)).deadline == distant.deadline);

    error(Catalog::publish(&distant, &distant, bytes("other"), 1, 1000, clock()), Catalog::Error::conflict);
    const Catalog::Record conflict{1, bytes("other"), Clock::Time(10s)};
    error(Catalog::merge(&distant, conflict, Clock::Time(2s)), Catalog::Error::conflict);
    const auto jump = Catalog::publish(&distant, &distant, value, 99, 1000, clock());
    CHECK(jump && jump->deadline == Clock::Time(2s)); // 高版本不继承旧版本的 100 s 截止.
    const auto newer = Catalog::merge(&distant, *jump, Clock::Time(1s));
    CHECK(newer && newer->visible); // 内容版本属于可见内容, 相同字节也必须通知.
    const auto timed = Catalog::merge(&distant, *jump, Clock::Time(2s));
    CHECK(timed && timed->visible && !timed->record.value && timed->record.version == 99);

    for (const auto ttl : {0U, 999U, 600001U, UINT32_MAX}) {
        error(Catalog::publish(nullptr, nullptr, value, 1, ttl, clock()), Catalog::Error::input);
        error(Catalog::renew(&distant, &distant, 1, ttl, clock()), Catalog::Error::input);
    }
    error(Catalog::publish(nullptr, nullptr, nullptr, 1, 1000, clock()), Catalog::Error::input);
    error(Catalog::publish(nullptr, nullptr, value, 0, 1000, clock()), Catalog::Error::input);
    error(Catalog::publish(nullptr, nullptr, value, 1, 1000, Clock::Reading{}), Catalog::Error::clock);
    error(Catalog::renew(&distant, &distant, 1, 1000, Clock::Reading{}), Catalog::Error::clock);
    error(Catalog::merge(nullptr, {}, Clock::Time(1s)), Catalog::Error::input);
    error(Catalog::merge(nullptr, Catalog::Record{1, value, std::nullopt}, Clock::Time(1s)), Catalog::Error::input);
    const auto top = Catalog::publish(nullptr, nullptr, bytes(""), UINT64_MAX, 600000, clock());
    CHECK(top && top->value && top->value->empty());
    CHECK(Catalog::renew(&*top, &*top, UINT64_MAX, 1000, clock()));
}

// 多来源到达顺序不改变最高版本或同版本最大截止, 不需要扫描所有来源重选低版本.
void convergence() {

    const auto value = bytes("shared");
    std::array<Catalog::Record, 4> facts{{{2, value, Clock::Time(20s)}, {7, value, Clock::Time(5s)}, {7, value, Clock::Time(10s)}, {7, nullptr, std::nullopt}}};
    std::array<unsigned, 4> order{0, 1, 2, 3}; // 全部 24 种顺序, 显式覆盖水位先到和后到.
    do {
        std::optional<Catalog::Record> current;
        for (const auto item : order) {
            const auto next = Catalog::merge(current ? &*current : nullptr, facts[item], Clock::Time(1s));
            CHECK(next);
            current = next->record;
        }
        CHECK(current && current->version == 7 && current->value == value && current->deadline == Clock::Time(10s));
    } while (std::ranges::next_permutation(order).found);
}

// 原生页不把 attr/data 塞入二进制胶水, 续租路径复制保留旧快照的截止与内容.
void pages() {

    astra::Pages<Ephemeris::Record> registrations; // 同线程读写, 本例不存在跨线程读完成同步.
    const auto key = std::make_shared<const std::string>("00112233-4455-4677-8899-aabbccddeeff");
    const auto initial = Ephemeris::create(bytes("attr"), bytes("data"), 1000, clock());
    CHECK(initial);
    registrations.prepare(0);
    registrations.set(0, key, *initial);
    const auto frozen = registrations.capture();
    const auto renewed = Ephemeris::renew(*registrations.at(0), 1, clock(1500ms));
    CHECK(renewed);
    registrations.prepare(0);
    registrations.set(0, nullptr, renewed->record);
    CHECK(registrations.at(0)->deadline == Clock::Time(2500ms));
    CHECK(registrations.at(1) == nullptr && registrations.at(UINT64_MAX) == nullptr);
    frozen.each([&](const std::string& name, const Ephemeris::Record& record) {
        CHECK(name == *key && record.deadline == Clock::Time(2s) && record.renewal == 0);
        CHECK(record.attr == initial->attr && record.data == initial->data);
    });

    astra::Pages<Catalog::Record> catalog; // 水位没有 Value, 仍然是一条有效原生记录.
    catalog.prepare(0);
    catalog.set(0, std::make_shared<const std::string>("key"), {5, nullptr, std::nullopt});
    const auto watermarks = catalog.capture();
    CHECK(watermarks.size() == 1 && catalog.at(0)->version == 5);
    catalog.prepare(0);
    catalog.erase(0);
    CHECK(catalog.at(0) == nullptr && catalog.capture().size() == 0 && watermarks.size() == 1);
    watermarks.each([](const std::string& name, const Catalog::Record& record) { CHECK(name == "key" && record.version == 5 && !record.value); });
}

// 调度只驱动边界, 不保存逐记录第二期限; 所有时间均由测试明确推进, 不真实休眠.
void agenda() {

    Agenda wheel(Clock::Time(1s));
    CHECK(wheel.next() == Clock::Time(1010ms));
    Agenda phase(Clock::Time(1003ms)); // 不同来源独立相位, 最早维护边界不能截成整十毫秒.
    CHECK(phase.next() == Clock::Time(1013ms));
    Agenda limit(Clock::Time::max() - 1ms); // 剩余不足一拍时饱和, 不发生有符号溢出.
    CHECK(limit.next() == Clock::Time::max());
    Agenda::Node node; // 稳定地址的钩子, 由被测 Agenda 排程.
    unsigned fired{};  // 实际到期回调计数, 不把空拍当作完成.
    wheel.set(node, Clock::Time(1015ms));
    wheel.advance(Clock::Time(1019ms), [&](auto*, auto) { ++fired; });
    CHECK(fired == 0 && wheel.time() == Clock::Time(1010ms));
    wheel.advance(Clock::Time(1020ms), [&](auto*, auto boundary) { CHECK(boundary == Clock::Time(1020ms)); ++fired; });
    CHECK(fired == 1 && !node.scheduled());

    wheel.set(node, Clock::Time(1030ms));
    wheel.advance(Clock::Time(1040ms), [&](auto* current, auto boundary) {
        ++fired;
        CHECK(boundary == Clock::Time(1030ms) || boundary == Clock::Time(1040ms));
        if (boundary == Clock::Time(1030ms)) {
            wheel.set(*current, Clock::Time(1040ms)); // 回调内重排必须从当前拍算, 不能多迟一拍.
        }
    });
    CHECK(fired == 3 && !node.scheduled());

    wheel.set(node, Clock::Time(1050ms));
    bool failed{};
    try {
        wheel.advance(Clock::Time(1060ms), [](auto*, auto) { throw std::bad_alloc{}; });
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    CHECK(failed && node.scheduled() && wheel.time() == Clock::Time(1050ms) && wheel.next() == Clock::Time(1060ms));
    wheel.advance(Clock::Time(1060ms), [&](auto*, auto boundary) { CHECK(boundary == Clock::Time(1060ms)); ++fired; });
    CHECK(fired == 4 && !node.scheduled());
    wheel.advance(Clock::Time(3601s + 3ms), [](auto*, auto) { CHECK(false); });
    CHECK(wheel.time() == Clock::Time(3601s)); // 一小时完整追赶, 不在 1024 拍处截断.
    wheel.set(node, Clock::Time(3602s));
    wheel.advance(Clock::Time(3602s), [&](auto*, auto boundary) { CHECK(boundary == Clock::Time(3602s)); ++fired; });
    CHECK(fired == 5 && !node.scheduled());

    wheel.set(node, Clock::Time(3603s));
    Agenda::erase(node);
    wheel.advance(Clock::Time(3603s), [](auto*, auto) { CHECK(false); });
    bool reversed{};
    try {
        wheel.advance(Clock::Time(3602s), [](auto*, auto) {});
    } catch (const std::invalid_argument&) {
        reversed = true;
    }
    CHECK(reversed && wheel.time() == Clock::Time(3603s));
}
} // namespace

// 所有用例为离线确定性边界检查; UUID 用例仅调用系统随机源, 不启动服务或触发依赖下载.
int main() {

    try {
        ephemeris();
        uuid();
        catalog();
        convergence();
        pages();
        agenda();
        std::cout << "native rules: ok\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
