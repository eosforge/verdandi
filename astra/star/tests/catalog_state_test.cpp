#include "catalog_state.hpp"
#include "check.hpp"
#include "exports.hpp"
#include <array>
#include <atomic>
#include <iostream>
#include <latch>
#include <new>
#include <string>
#include <thread>

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

// 版本查询读取合并水位, 不创建不存在的范围, 不受正文过期或来源差异影响.
void versions() {

    auto now = 1s; // 确定性的可控业务时间, 不依赖测试机墙钟.
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope scope{"query", "main"};                                       // 只捕获这个范围的相关 Key.
    const std::array<std::string_view, 3> keys{"local", "remote", "missing"}; // 同步查询只借用文本, 保留输入顺序.
    CHECK(state.versions(scope, keys).value() == std::vector<std::uint64_t>({0, 0, 0}));
    CHECK(state.source().size() == 0);
    CHECK(state.publish(scope, "local", bytes("local"), 7, 1000));
    CHECK(state.admit("remote"));
    CHECK(state.apply("remote", 1, scope, "remote", Catalog::Record{19, bytes("remote"), Clock::Time(2s)}, State::Source::Form::record));
    CHECK(state.versions(scope, keys).value() == std::vector<std::uint64_t>({7, 19, 0}));

    // 即使所有正文都已清理, SDK 初始化仍能取到两种来源的最高已知版本.
    now = 3s;
    state.tick();
    CHECK(state.capture(scope)->size() == 0);
    CHECK(state.versions(scope, keys).value() == std::vector<std::uint64_t>({7, 19, 0}));
    const std::array<std::string_view, 2> duplicates{"local", "local"}; // 重复 Key 不是合法有界查询.
    CHECK(state.versions(scope, duplicates).error() == State::Error::input);
    CHECK(state.versions(scope, {}).error() == State::Error::input);
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

    std::chrono::nanoseconds now = 1s; // 可调整读数, 验证拒绝的采样不会污染后续单调检查.
    bool available = true;             // false 表示采样器没有返回读数.
    bool ready{};                      // 最初没有可用时钟, 各读取不得执行后续范围创建.
    bool fail{};                       // 仅一次取时抛出异常, 检验 execute 的锁展开.
    State::Limits limits;              // 只允许一个范围, 错误路径偷建目录会使正常发布失败.
    limits.scopes = 1;
    State state([&]() -> std::optional<Clock::Reading> {
        if (std::exchange(fail, false)) {
            throw std::bad_alloc{};
        }
        if (!available)
            return std::nullopt;
        return Clock::Reading{.time = Clock::Time(now), .ready = ready};
    },
                limits);
    const Scope pending{"pending", "main"}, active{"active", "main"}; // 错误路径与最终成功路径使用不同范围.
    CHECK(state.capture({}) == std::unexpected(State::Error::input));
    CHECK(state.capture(pending) == std::unexpected(State::Error::clock));
    CHECK(state.find(pending, "key") == std::unexpected(State::Error::clock));
    CHECK(state.changes(pending, 0) == std::unexpected(State::Error::clock));
    CHECK(state.events(0) && state.events(0)->empty()); // 无时钟也可导出已有空来源, 不隐式清理.
    CHECK(state.deliver(0, 8, 4096));                   // 来源恢复不依赖公共读取的时钟资格.

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

    // 回退读数不能降低已确认的 1 s 水位; 未就绪的未来读数同样不能将水位推高.
    now = 500ms;
    CHECK(state.capture(pending) == std::unexpected(State::Error::clock));
    now = 750ms;
    CHECK(state.capture(pending) == std::unexpected(State::Error::clock));
    now = 10s;
    ready = false;
    CHECK(state.capture(pending) == std::unexpected(State::Error::clock));
    ready = true;
    now = 1s;
    available = false;
    CHECK(state.capture(pending) == std::unexpected(State::Error::clock));
    available = true;

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

// 批次裁剪发送历史时, 已离开当前根的最后一份旧载荷也必须在锁外释放.
void batch_reclaim() {

    bool released{}, unlocked{}; // 析构时反向取得域锁, 检查旧历史的实际释放边界.
    State::Limits limits;
    limits.history = 0; // 不让公开投影历史掩盖来源历史的最后引用.
    limits.source.history = 2;
    const auto state = std::make_shared<State>([] { return std::optional(Clock::Reading{.time = Clock::Time(1s), .ready = true}); }, limits);
    const Scope scope{"batch", "reclaim"};
    Catalog::Value previous(new Catalog::Buffer(16), [owner = std::weak_ptr<State>(state), &released, &unlocked](const Catalog::Buffer* payload) noexcept {
        released = true;
        try {
            const auto live = owner.lock(); // State 析构时不能再反向访问状态.
            unlocked = live && live->received("absent") == std::unexpected(State::Error::input);
        } catch (...) {
            unlocked = false;
        }
        delete payload;
    });
    CHECK(state->publish(scope, "a", previous, 1, 1000));
    CHECK(state->publish(scope, "a", bytes("new"), 2, 1000));
    previous.reset();
    CHECK(!released); // 只剩来源历史保有版本 1, 当前根已经是版本 2.

    const std::vector<State::Entry> entries{{"a", bytes("a")}, {"b", bytes("b")}};
    CHECK(state->publish(scope, entries, 3, 1000));
    CHECK(released && unlocked && state->events(2)->size() == 2);
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

// 批次提交、续租、失败与历史游标都必须使用完整边界, 并发快照不能混合版本.
void batches() {

    auto now = 1s; // 固定时钟隔离到期语义, 只检查本次批提交边界.
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope scope{"batch", "scope"};
    const std::vector<State::Entry> entries{{"a", bytes("a")}, {"b", bytes("b")}};
    CHECK(state.publish(scope, entries, 1, 1000));
    const auto frozen = state.capture(scope);
    const auto source = state.source(); // 来源根与公开根均应冻结整批.
    CHECK(frozen->size() == 2 && frozen->version() == 2 && source.position() == 2);
    std::atomic_bool valid{true};
    std::latch observed{1}; // 写入前至少完成一次读, 避免线程未调度导致空验收.
    std::jthread reader([&](std::stop_token stop) {
        bool first = true; // 只有读取线程递减 latch, 后续轮次保持无等待.
        while (!stop.stop_requested()) {
            const auto view = state.capture(scope);
            std::optional<std::uint64_t> version; // 同份视图中每条业务版本都应相同.
            view->each([&](const auto&, const auto& row) { if (version && *version != row.version) valid.store(false); version = row.version; });
            if (view->size() != 2)
                valid.store(false);
            if (first) {
                first = false;
                observed.count_down();
            }
        }
    });
    observed.wait();
    for (std::uint64_t version = 2; version <= 32; ++version)
        CHECK(state.publish(scope, entries, version, 1000));
    reader.request_stop();
    reader.join();
    CHECK(valid.load());
    frozen->each([](const auto&, const auto& row) { CHECK(row.version == 1); });
    CHECK(state.changes(scope, 1).error() == State::Error::history); // 半批游标不能作为已安装基线.
    CHECK(state.changes(scope, 62)->size() == 2);
    const std::vector<State::Entry> conflict{{"new", bytes("ok")}, {"b", bytes("conflict")}};
    CHECK(state.publish(scope, conflict, 32, 1000).error() == State::Error::conflict);
    CHECK(!state.find(scope, "new")->record && state.source().position() == 64);
    const std::vector<State::Entry> renewal{{"a", {}}, {"missing", {}}};
    CHECK(state.publish(scope, renewal, 32, 2000, true).error() == State::Error::ended);
    CHECK(state.source().position() == 64);
    const std::vector<State::Entry> complete{{"a", {}}, {"b", {}}};
    CHECK(state.publish(scope, complete, 32, 2000, true));
    CHECK(state.source().position() == 66 && state.capture(scope)->version() == 64);
}
} // namespace

// 原生状态用例不创建网络/数据库, 所有时间边界由测试明确控制.
int main() {
    try {
        versions();
        exports<State>([](State& state, const Scope& scope) { CHECK(state.publish(scope, "key", bytes("value"), 1, 1000)); return std::string("key"); });
        batches();
        lifecycle();
        boundary();
        reads();
        reclaim();
        batch_reclaim();
        capacity();
        std::cout << "Catalog state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
