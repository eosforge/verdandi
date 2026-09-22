#include "check.hpp"
#include "library.hpp"
#include <atomic>
#include <iostream>
#include <thread>

namespace {
// 初始化 scope 的一份完整底稿, value 是唯一 Key 的内容, 非零版本才能包含数据.
void install(astra::Library& library, const astra::Scope& scope, std::uint64_t version, astra::Almanac::Buffer value) {

    auto draft = library.prepare(scope, version); // 私有候选在 set/reset 完成前不可见.
    CHECK(draft);
    if (version != 0) {
        CHECK(draft->set("key", std::move(value)));
    }
    CHECK(library.reset(std::move(*draft)) == true);
}

// 校验 UTF-8 地址和精确双层索引, 斜线不是路径分隔, __ 仅为入口保留标志.
void addresses() {

    CHECK((astra::Scope{"区域", "服务"}.valid()));
    CHECK((astra::Scope{"__auth", "comet"}.internal()));
    for (const auto value : {"", "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8", "\x80"}) {
        CHECK(!astra::Scope::text(value, 128));
    }
    CHECK(!astra::Scope::text(std::string_view("a\0b", 3), 128));
    CHECK(astra::Scope::text("\xf0\x9f\x8c\x9f", 4));

    astra::Library library; // 两组拼接后文本相同, 两级地址仍必须独立.
    const astra::Scope a{"a/b", "c"}, b{"a", "b/c"};
    install(library, a, 1, {1});
    install(library, b, 1, {2});
    CHECK(library.find(a)->find("key")->value->front() == 1);
    CHECK(library.find(b)->find("key")->value->front() == 2);
    CHECK(library.positions().size() == 2);
}

// 未完成候选不占路由, 内容和历史各有总预算; 历史耗尽不拒绝合法权威内容.
void budgets() {

    astra::Library::Limits limits; // 极小容量让边界确定可复现, 不依赖系统内存压力.
    limits.scopes = 2;
    limits.bytes = 8;
    limits.history = 0;
    limits.scope.bytes = 8;
    astra::Library library(limits);
    const astra::Scope a{"a", "s"}, b{"b", "s"};
    {
        auto abandoned = library.prepare(a, 4); // 离开作用域模拟断流, 不得留下虚假空 Scope.
        CHECK(abandoned && abandoned->set("key", {1}));
    }
    CHECK(!library.find(a) && library.positions().empty());
    install(library, a, 1, {1});
    install(library, b, 1, {2});
    CHECK(library.apply(a, 2, "key", astra::Almanac::Buffer{1, 2}) == std::unexpected(astra::Almanac::Error::capacity));
    CHECK(library.find(a)->usage().version == 1);
    CHECK(library.apply(a, 2, "key", std::nullopt) == true);
    CHECK(library.apply(b, 2, "key", astra::Almanac::Buffer{2, 3, 4}) == true);
    CHECK(library.find(b)->replay(1) == std::unexpected(astra::Almanac::Error::history));
    CHECK(!library.prepare({"c", "s"}, 0));
    CHECK(library.positions().size() == 2); // 空分组保留版本, 不借删除内容归还部署槽位.
}

// 完整读视图跨覆盖稳定, 独立读者不持有路由或安装锁执行自己的遍历.
void readers() {

    astra::Library library;
    const astra::Scope scope{"a", "s"};
    install(library, scope, 1, {1});
    const auto original = library.find(scope)->view(); // 保留旧根, 后续覆盖只能改变新版本.
    CHECK(original);
    std::atomic_bool done{}; // writer 完成后通知读线程, 不参与业务同步.
    std::jthread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            const auto book = library.find(scope);
            CHECK(book && book->find("key")->value->size() == 1);
        }
    });
    for (std::uint64_t version = 2; version <= 100; ++version) {
        CHECK(library.apply(scope, version, "key", astra::Almanac::Buffer{static_cast<std::uint8_t>(version)}) == true);
    }
    done.store(true, std::memory_order_release);
    reader.join();
    original->each([](const auto& key, const auto& value) { CHECK(key == "key" && value->front() == 1); });
    CHECK(library.positions().front().version == 100);
}

// 活动通知来自实际提交, 零历史仍有完整不可变值; 重放与失败不发第二次通知.
void notifications() {

    using astra::Almanac;                // 本用例局部使用原生变更/载荷类型, 不扩展其他翻译单元的命名空间.
    std::vector<Almanac::Change> events; // 保留独立共享正文, 检查后续覆盖不会改写先前通知.
    events.reserve(8);                   // 回调声明 noexcept, 本例提前准备足够的有界事件空间.
    astra::Library::Limits limits;
    limits.scope.history = 0;
    astra::Library library(limits, [&](const astra::Scope& scope, const Almanac::Change& change) noexcept {
        if (scope.sector != "routes" || scope.spectrum != "main") {
            std::terminate();
        }
        events.push_back(change);
    });
    auto draft = library.prepare({"routes", "main"}, 1);
    CHECK(draft && draft->set("key", {1}));
    CHECK(library.reset(std::move(*draft)));
    CHECK(events.size() == 1 && !events[0].key && events[0].version == 1);
    CHECK(library.apply({"routes", "main"}, 2, "key", Almanac::Buffer{2}));
    CHECK(events.size() == 2 && *events[1].key == "key" && *events[1].value == Almanac::Buffer{2});
    CHECK(!*library.apply({"routes", "main"}, 2, "key", Almanac::Buffer{2}));
    CHECK(!library.apply({"routes", "main"}, 4, "key", Almanac::Buffer{4}));
    CHECK(events.size() == 2);
    CHECK(library.apply({"routes", "main"}, 3, "key", std::nullopt));
    CHECK(events.size() == 3 && !events[2].value && *events[1].value == Almanac::Buffer{2});
    CHECK(!library.find({"routes", "main"})->replay(1));
}
} // namespace

// 所有断言在 Release 也生效, 用例不联网或安装任何依赖.
int main() {
    addresses();
    budgets();
    readers();
    notifications();
    std::cout << "Almanac library checks passed\n";
}
