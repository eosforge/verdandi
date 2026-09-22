#include "catalog_state.hpp"
#include "check.hpp"
#include <iostream>
#include <new>
#include <string>

namespace {
using namespace std::chrono_literals;
using astra::Catalog;
using astra::Clock;
using astra::Scope;
using State = Catalog::State;

// 不透明正文测试值, 空字符串仍有非空所有者.
Catalog::Value bytes(std::string_view value) {
    return std::make_shared<const Catalog::Buffer>(value.begin(), value.end());
}

// 内容版本、来源序列与下游游标各自有语义: 续租只推进来源, 本地过期只推进下游.
void lifecycle() {

    auto now = 1000ms; // 可控纪元时间, 从 1 s 开始.
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope left{"route", "left"};
    const Scope right{"route", "right"};
    const auto value = bytes("body");
    CHECK(state.publish(left, "key", value, 10, 1000));
    CHECK(state.publish(right, "key", bytes(""), 2, 1000));
    const auto frozen = state.capture(left);
    CHECK(frozen && frozen->version() == 1 && state.source().position() == 2);
    now = 1200ms;
    CHECK(state.renew(left, "key", 10, 2000));
    CHECK(state.source().position() == 3 && state.capture(left)->version() == 1);
    CHECK(state.publish(left, "key", bytes("body"), 10, 1000));
    CHECK(state.source().position() == 4 && state.capture(left)->version() == 1);
    const auto conflict = state.publish(left, "key", bytes("different"), 10, 1000);
    const auto obsolete = state.publish(left, "key", value, 9, 1000);
    CHECK(!conflict && conflict.error() == State::Error::conflict);
    CHECK(!obsolete && obsolete.error() == State::Error::version);
    CHECK(state.source().position() == 4);

    now = 2000ms;
    state.tick();
    CHECK(state.source().position() == 4 && state.source().size() == 2); // right 留下版本水位, 不广播删除.
    CHECK(state.capture(right)->size() == 0 && state.capture(right)->version() == 2);
    CHECK(state.capture(left)->size() == 1);
    const auto missing = state.renew(right, "key", 2, 1000);
    CHECK(!missing && missing.error() == State::Error::ended);
    const auto lower = state.publish(right, "key", value, 1, 1000);
    CHECK(!lower && lower.error() == State::Error::version);
    CHECK(state.publish(right, "key", bytes(""), 2, 1000)); // 完整正文允许同版本重建, 水位不能自行续租.
    CHECK(state.source().position() == 5 && state.capture(right)->version() == 3);
    CHECK(state.publish(left, "key", value, 100, 1000)); // 高版本跳号, 相同正文仍是可见版本变化.
    CHECK(state.capture(left)->version() == 2 && state.find(left, "key")->record->version == 100);
    frozen->each([&](const std::string& key, const State::Content& record) { CHECK(key == "key" && record.version == 10 && record.value == value); });
    const auto events = state.events(0);
    CHECK(events && events->size() == 6 && events->at(2).form == State::Source::Form::renew);

    now += 1h;
    state.tick();
    CHECK(state.source().position() == 6 && state.source().size() == 2);
    state.source().each([](const Scope&, const std::string&, const Catalog::Record& record) { CHECK(record.version > 0 && !record.value && !record.deadline); });
    CHECK(state.capture(left)->size() == 0 && state.capture(right)->size() == 0);
}

// 最终采样不能续活已经到期的正文, Pulsar 未同步仍允许已建立的本地业务时钟受理.
void boundary() {

    auto now = 1s;
    auto jump = 0s; // 单次采样后才跳变, 使真正准备与最终提交处于不同时间.
    State state([&] { const auto reading = Clock::Reading{.time = Clock::Time(now), .ready = true, .synchronized = false}; now += std::exchange(jump, 0s); return std::optional(reading); }, {});
    const Scope scope{"time", "boundary"};
    const auto value = bytes("v");
    jump = 2s;
    CHECK(state.publish(scope, "k", value, 1, 1000));
    state.source().each([](const Scope&, const std::string&, const Catalog::Record& record) { CHECK(record.deadline == Clock::Time(4s)); });
    jump = 2s;
    const auto rejected = state.renew(scope, "k", 1, 1000);
    CHECK(!rejected && rejected.error() == State::Error::ended);
    state.tick();
    CHECK(state.source().position() == 1 && state.capture(scope)->size() == 0);
    CHECK(state.publish(scope, "k", value, 1, 1000));
    CHECK(state.source().position() == 2 && state.capture(scope)->version() == 3);
}

// 统一读取入口必须保留输入/时钟/游标错误, 取时抛错后锁和空范围额度仍可正常使用.
void reads() {

    bool ready{};         // 最初没有可用时钟, 各读取不得执行后续范围创建.
    bool fail{};          // 仅一次取时抛出异常, 检验 execute 的锁展开.
    State::Limits limits; // 只允许一个范围, 错误路径偷建目录会使正常发布失败.
    limits.scopes = 1;
    State state([&]() -> std::optional<Clock::Reading> {
        if (std::exchange(fail, false)) {
            throw std::bad_alloc{};
        }
        return Clock::Reading{.time = Clock::Time(1s), .ready = ready};
    },
                limits);
    const Scope pending{"pending", "main"}, active{"active", "main"}; // 错误路径与最终成功路径使用不同范围.
    CHECK(state.capture({}) == std::unexpected(State::Error::input));
    CHECK(state.capture(pending) == std::unexpected(State::Error::clock));
    CHECK(state.find(pending, "key") == std::unexpected(State::Error::clock));
    CHECK(state.changes(pending, 0) == std::unexpected(State::Error::clock));
    CHECK(state.events(0) == std::unexpected(State::Error::clock));
    CHECK(state.deliver(0, 8, 4096) == std::unexpected(State::Error::clock));

    ready = fail = true;
    bool caught{}; // 只有实际捕获取时异常才接受后续恢复检查.
    try {
        static_cast<void>(state.capture(pending));
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    CHECK(caught);
    for (unsigned index = 0; index < 4; ++index) {
        const Scope unknown{"unknown", std::to_string(index)}; // 读取范围数超过写入额度, 不能挤掉真正的业务 Scope.
        const auto view = state.capture(unknown);              // 空范围完整根, 不借用临时 Scene.
        const auto point = state.find(unknown, "key");         // 同边界精确缺项.
        const auto replay = state.changes(unknown, 0);         // 零游标表示已追平合法空基线.
        CHECK(view && view->version() == 0 && view->size() == 0 && view->bytes() == 0);
        CHECK(point && point->version == 0 && !point->record && !point->name);
        CHECK(replay && replay->empty());
        CHECK(state.changes(unknown, 1) == std::unexpected(State::Error::input));
    }
    const auto empty = state.capture(active); // 未创建时的冻结根不能因后来首次写入而发生变化.
    CHECK(empty && state.publish(active, "key", bytes("body"), 1, 1000));
    CHECK(empty->page(0, 1, [](const auto&, const auto&) { CHECK(false); return true; }) == 0);
    CHECK(state.capture(active)->size() == 1 && state.capture(active)->version() == 1);
    CHECK(state.capture(pending)->version() == 0 && !state.find(pending, "key")->record && state.changes(pending, 0)->empty());
    CHECK(state.publish(pending, "key", bytes("body"), 1, 1000) == std::unexpected(State::Error::capacity));
    CHECK(state.changes(active, 2) == std::unexpected(State::Error::input)); // 超前游标不能误映射为写入版本耗尽.
    CHECK(state.events(2) == std::unexpected(State::Error::input));
    CHECK(state.deliver(2, 8, 4096) == std::unexpected(State::Error::input));
}

// 最后一份正文在读取推进到期时回收, 析构回调可以取得域锁, 检验 retired 必须晚于 lock 释放.
void reclaim() {

    auto now = 1s;               // 固定 TTL 从 1 s 到 2 s, 不依赖休眠.
    bool released{}, unlocked{}; // 分别记录实际析构与析构时成功重入, 必须晚于 State 销毁.
    State::Limits limits;        // 禁用两类历史, 排除历史合法保留正文而掩盖回收时机.
    limits.history = limits.source.history = 0;
    const auto state = std::make_shared<State>([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, limits);
    const Scope scope{"expiry", "reclaim"}; // 单范围, 到期后其公开视图必须为空.
    Catalog::Value value(new Catalog::Buffer(16), [owner = std::weak_ptr<State>(state), &released, &unlocked](const Catalog::Buffer* payload) noexcept {
        released = true;
        try {
            const auto live = owner.lock(); // 失败退出销毁 State 时不反向访问已经析构的成员.
            unlocked = live && live->received("absent") == std::unexpected(State::Error::input);
        } catch (...) {
            unlocked = false; // 析构不传播异常, 由外层断言报告未满足回收契约.
        }
        delete payload;
    });
    CHECK(state->publish(scope, "key", value, 1, 1000));
    value.reset(); // 仅生产状态持有正文, 回收不依赖测试释放最后一份外部引用.
    CHECK(!released);

    now = 2s;
    const auto view = state->capture(scope); // 由统一读取入口触发 TTL 删除并在返回前完成锁外回收.
    CHECK(view && view->size() == 0 && released && unlocked);
}

// 失败准备不占空 Scope, 过期水位仍受原生容量保护, 不能通过无限短租约吃掉无限内存.
void capacity() {

    auto now = 1s;
    State::Limits limits;
    limits.scopes = 1;
    limits.source.records = 1;
    limits.projection.bytes = 700;
    limits.history = 0;
    limits.source.history = 0;
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, limits);
    const Scope rejected{"rejected", "scope"};
    const Scope accepted{"accepted", "scope"};
    const auto large = std::make_shared<const Catalog::Buffer>(1024, 1);
    CHECK(!state.publish(rejected, "a", large, 1, 1000));
    CHECK(state.source().position() == 0 && state.source().size() == 0);
    CHECK(state.publish(accepted, "a", bytes("small"), 1, 1000));
    CHECK(state.changes(accepted, 0).error() == State::Error::history && state.events(0).error() == State::Error::history);
    now = 2s;
    state.tick();
    CHECK(state.source().size() == 1 && state.capture(accepted)->size() == 0);
    const auto full = state.publish(accepted, "b", bytes("small"), 1, 1000);
    CHECK(!full && full.error() == State::Error::capacity);
    CHECK(state.source().position() == 1 && state.capture(accepted)->version() == 2);
    CHECK(state.publish(accepted, "a", bytes("new"), 2, 1000));
}
} // namespace

// 原生状态用例不创建网络/数据库, 所有时间边界由测试明确控制.
int main() {
    try {
        lifecycle();
        boundary();
        reads();
        reclaim();
        capacity();
        std::cout << "Catalog state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
