// 独立进程使用线程局部分配暂停, 不将测试钩子或替换 new 链接进生产服务.
#include "catalog_state.hpp"
#include "check.hpp"
#include "ephemeris_state.hpp"
#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <new>
#include <semaphore>
#include <utility>

namespace {
using namespace std::chrono_literals;
using astra::Catalog;
using astra::Clock;
using astra::Ephemeris;
using astra::Scope;

// 仅暂停当前测试的一次真实分配, 三秒后自行释放, 断言失败也不留下永久阻塞线程.
struct Pause {
    std::binary_semaphore entered{0};  // 工作线程确认进入锁外原生准备.
    std::binary_semaphore released{0}; // 测试主线程允许准备继续.
};

thread_local Pause* armed{};                // 安装线程首次取时后才启用分配暂停, 初始关闭.
thread_local Pause* allocation{};           // 下次普通 new 的一次性暂停, 清除后不递归拦截标准库.
std::atomic<std::int64_t> epoch{1000};      // 测试业务毫秒时间, 每个顺序场景开始时重置.
thread_local std::ptrdiff_t remaining = -1; // -1 关闭故障注入, 零使下一次分配抛错, 正数倒计到该位置.
thread_local bool injected{};               // 只记录本线程确实触发的 bad_alloc, 不将其他失败当作覆盖.

// 标准抛出式分配, size=0 同样提供可 free 的存储, 保留 new_handler 行为.
[[gnu::noinline]] void* allocate(std::size_t size) {

    if (auto* pause = std::exchange(allocation, nullptr)) {
        pause->entered.release();
        static_cast<void>(pause->released.try_acquire_for(3s));
    }
    if (remaining == 0) {
        remaining = -1;
        injected = true;
        throw std::bad_alloc{};
    }
    if (remaining > 0)
        --remaining;
    for (;;) {
        if (void* result = std::malloc(size ? size : 1))
            return result;
        if (auto handler = std::get_new_handler())
            handler();
        else
            throw std::bad_alloc{};
    }
}

// 保持替换分配域一致, nullptr 可释放, 不在回收时触发暂停.
[[gnu::noinline]] void release(void* value) noexcept {
    std::free(value);
}

// 两个领域共用同一个真实调度边界, fixed 时钟不触发无关大批到期.
auto time() {
    if (auto* pause = std::exchange(armed, nullptr))
        allocation = pause;
    return std::optional(Clock::Reading{.time = Clock::Time(std::chrono::milliseconds(epoch.load())), .ready = true});
}

// 冻结测试载荷, 名称与数据都使用真实容器, 不模拟 Source/Scene 的内部行为.
Catalog::Value bytes(std::string_view text) {
    return std::make_shared<const Catalog::Buffer>(text.begin(), text.end());
}

// restore 在分配处暂停期间, 本地写入必须完成; 同来源查询等待时也不能重新占住域锁.
template <typename Domain>
void concurrent(typename Domain::Record incoming, std::string key, auto&& write, bool revoke) {

    using State = typename Domain::State;
    epoch = 1000;
    typename State::Limits limits;
    limits.replicas = 1; // 退役空来源应能回收, 否则会永久占满本例唯一的准入位置.
    State state(time, limits);
    const Scope remote{"remote", "main"}, local{"local", "main"}; // 不同 Scope, 两边仍使用同一业务域.
    CHECK(write(state, local));
    CHECK(state.admit("peer"));
    auto draft = state.prepare("peer", 1);
    CHECK(draft && draft->set(remote, key, incoming));
    auto recovery = state.restore("peer", std::move(*draft));
    CHECK(recovery);
    Pause pause;
    auto installer = std::async(std::launch::async, [task = std::move(*recovery), &pause]() mutable {
        armed = &pause;
        auto result = task.step();
        armed = allocation = nullptr; // 测试线程退出之前必须关闭钩子.
        return result;
    });
    const bool entered = pause.entered.try_acquire_for(1s);
    std::promise<void> querying; // 确认等待者已经开始, 不使用任意 sleep 推测来源查询时序.
    auto queried = querying.get_future();
    auto reader = std::async(std::launch::async, [&] {
        querying.set_value();
        return state.received("peer");
    });
    queried.wait();
    auto writer = std::async(std::launch::async, [&] {
        if (revoke)
            state.retire("peer"); // 身份冻结不能等整份原生准备结束, 最终安装必须重新核实.
        return static_cast<bool>(write(state, local));
    });
    const bool independent = writer.wait_for(1s) == std::future_status::ready;
    pause.released.release(); // 所有断言之前解除阻塞, 不依赖 CHECK 异常替我们清理线程.
    const auto installed = installer.get();
    const auto written = writer.get();
    const auto position = reader.get();
    CHECK(entered && independent && written && position && *position == 0);
    if (revoke) {
        state.tick(); // 固定业务时间没有推进, 仍必须完成准备期间延期的退役回收.
        CHECK(state.admit("replacement"));
        CHECK(!installed && !state.find(remote, key)->record); // 禁止已撤销来源通过迟到候选发布.
    } else {
        CHECK(installed == false && state.find(remote, key)->record); // 仅 Scope 安装, 还没执行全来源最终确认.
        auto next = state.prepare("peer", 1);                         // 无遗留 editing_/来源锁, 同位置合法候选可以重新建立.
        CHECK(next);
    }
}

// 旧租约在锁外准备期间到期, 公开读取必须等到完整到期/安装边界, 不能发布已经失效的旧内容.
template <typename Domain>
void expiration(typename Domain::Record incoming, std::string key) {

    using State = typename Domain::State;
    epoch = 1000;
    State state(time, {});
    const Scope scope{"expiry", "main"}; // 此范围只接收远端事实, 无本机来源期限干扰.
    CHECK(state.admit("peer") && state.apply("peer", 1, scope, key, incoming, State::Source::Form::record));
    auto draft = state.prepare("peer", 2);
    CHECK(draft && draft->set(scope, key, incoming));
    auto recovery = state.restore("peer", std::move(*draft));
    CHECK(recovery);
    Pause pause;
    auto installer = std::async(std::launch::async, [task = std::move(*recovery), &pause]() mutable {
        armed = &pause;
        auto result = task.step();
        armed = allocation = nullptr;
        return result;
    });
    const bool entered = pause.entered.try_acquire_for(1s);
    epoch = 5000; // 原期限固定 4 s, 不能因为网络恢复而变成当前时刻再加 TTL.
    auto reader = std::async(std::launch::async, [&] { return state.find(scope, key); });
    const bool waiting = reader.wait_for(100ms) == std::future_status::timeout;
    pause.released.release();
    const auto installed = installer.get();
    const auto point = reader.get();
    CHECK(entered && waiting && installed == false && point && !point->record);
}

// 逐分配点覆盖锁外原生准备及锁内最终投影准备, 失败不得留下 editing、目录、钩子或来源锁.
template <typename Domain>
void rollback(typename Domain::Record old, typename Domain::Record newer, std::string key) {

    using State = typename Domain::State;
    epoch = 1000;
    bool completed{}; // 至少跑到一次所有准备分配均成功, 不凭固定循环数假装穷尽实际分配路径.
    for (std::ptrdiff_t point = 0; point < 512; ++point) {
        State state(time, {});
        const Scope scope{"rollback", "main"};
        CHECK(state.admit("peer") && state.apply("peer", 1, scope, key, old, State::Source::Form::record));
        auto frozen = state.capture(scope);
        auto draft = state.prepare("peer", 2);
        CHECK(frozen && draft && draft->set(scope, key, newer));
        std::expected<void, typename State::Error> result;
        bool failed{};
        injected = false;
        remaining = point;
        try {
            result = state.replace("peer", std::move(*draft));
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        remaining = -1; // 断言、复查及下一次恢复绝不在分配注入区执行.
        if (!injected) {
            CHECK(!failed && result && state.received("peer") == 2);
            completed = true;
            break;
        }
        CHECK(failed && state.received("peer") == 1 && state.capture(scope)->version() == frozen->version());
        auto retry = state.prepare("peer", 2);
        CHECK(retry && retry->set(scope, key, newer) && state.replace("peer", std::move(*retry)));
        CHECK(state.received("peer") == 2 && state.find(scope, key)->record);
    }
    CHECK(completed);
}

// 同 Key 的本地新版本在远端准备期间提交, 最终合并必须使用新水位, 不能用解锁前的旧结果覆盖.
void merging() {

    epoch = 1000;
    Catalog::State state(time, {});
    const Scope scope{"shared", "main"};
    CHECK(state.admit("peer"));
    auto draft = state.prepare("peer", 1);
    CHECK(draft && draft->set(scope, "key", {1, bytes("remote"), Clock::Time(4s)}));
    auto recovery = state.restore("peer", std::move(*draft));
    CHECK(recovery);
    Pause pause;
    auto installer = std::async(std::launch::async, [task = std::move(*recovery), &pause]() mutable {
        armed = &pause;
        auto result = task.step();
        armed = allocation = nullptr;
        return result;
    });
    const bool entered = pause.entered.try_acquire_for(1s);
    auto writer = std::async(std::launch::async, [&] { return state.publish(scope, "key", bytes("local"), 2, 1000); });
    const bool independent = writer.wait_for(1s) == std::future_status::ready;
    pause.released.release();
    const auto installed = installer.get();
    const auto written = writer.get();
    const auto visible = state.find(scope, "key");
    CHECK(entered && independent && installed == false && written && visible && visible->record);
    CHECK(visible->record->version == 2 && *visible->record->value == *bytes("local"));
    const auto remote = state.replica("peer", scope, "key"); // 来源事实仍正确保存版本 1, 仅公共视图取版本 2.
    CHECK(remote && *remote && (*remote)->version == 1);
}

// 两个独立来源的原生准备可以同时到达真实分配屏障, 不仅是不阻塞本地 SDK 写入.
template <typename Domain>
void parallel(typename Domain::Record record, std::string first, std::string second) {

    using State = typename Domain::State;
    epoch = 1000;
    State state(time, {});
    const Scope scope{"parallel", "main"};
    CHECK(state.admit("a") && state.admit("b"));
    auto a = state.prepare("a", 1), b = state.prepare("b", 1);
    CHECK(a && b && a->set(scope, first, record) && b->set(scope, second, record));
    auto left = state.restore("a", std::move(*a)), right = state.restore("b", std::move(*b));
    CHECK(left && right);
    Pause one, two; // 两个线程各自仅暂停一次, 不共享线程局部分配钩子.
    auto launch = [](typename State::Recovery task, Pause& pause) {
        return std::async(std::launch::async, [task = std::move(task), &pause]() mutable {
            armed = &pause;
            auto result = task.step();
            armed = allocation = nullptr;
            return result;
        });
    };
    auto first_task = launch(std::move(*left), one);
    const bool first_entered = one.entered.try_acquire_for(1s);
    auto second_task = launch(std::move(*right), two);
    const bool second_entered = two.entered.try_acquire_for(1s);
    one.released.release();
    two.released.release();
    const auto first_result = first_task.get(), second_result = second_task.get();
    CHECK(first_entered && second_entered && first_result == false && second_result == false);
    CHECK(state.find(scope, first)->record && state.find(scope, second)->record);
}

// 私有准备抛异常时必须先重取域锁, 同来源责任释放后其他调用可以继续取得锁.
void exception() {

    struct Replica {
        std::mutex mutex;
    }; // 仅测试锁守卫本身, 不扩大 State 的生产测试接口.

    std::shared_mutex domain;
    const auto replica = std::make_shared<Replica>();
    std::unique_lock locked(domain);
    {
        astra::Borrowing borrowed(replica, locked);
        bool failed{};
        try {
            astra::Borrowing<Replica>::outside(locked, [] { throw std::bad_alloc{}; });
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        CHECK(failed && locked.owns_lock());
    }
    CHECK(replica->mutex.try_lock());
    replica->mutex.unlock();
}
} // namespace

// 以下替换均使用同一个分配域, 不接管超对齐分配.
void* operator new(std::size_t size) {
    return allocate(size);
}

void* operator new[](std::size_t size) {
    return allocate(size);
}

void operator delete(void* value) noexcept {
    release(value);
}

void operator delete[](void* value) noexcept {
    release(value);
}

void operator delete(void* value, std::size_t) noexcept {
    release(value);
}

void operator delete[](void* value, std::size_t) noexcept {
    release(value);
}

// 有限锁交错验证, 不测性能, 不使用全局睡眠或生产故障注入开关.
int main() {

    try {
        exception();
        merging();
        parallel<Catalog>({1, bytes("remote"), Clock::Time(4s)}, "a", "b");
        parallel<Ephemeris>({bytes("attr"), bytes("data"), Clock::Time(4s), 0, 0, 1000}, Ephemeris::uuid(), Ephemeris::uuid());
        for (const bool revoke : {false, true}) {
            const Catalog::Record catalog{1, bytes("remote"), Clock::Time(4s)};
            concurrent<Catalog>(catalog, "remote", [](auto& state, const Scope& scope) { return state.publish(scope, "own", bytes("own"), 1, 1000); }, revoke);
            const Ephemeris::Record ephemeris{bytes("attr"), bytes("data"), Clock::Time(4s), 0, 0, 1000};
            concurrent<Ephemeris>(ephemeris, Ephemeris::uuid(), [](auto& state, const Scope& scope) { return state.create(scope, bytes("attr"), bytes("own"), 1000); }, revoke);
        }
        expiration<Catalog>({1, bytes("remote"), Clock::Time(4s)}, "remote");
        expiration<Ephemeris>({bytes("attr"), bytes("data"), Clock::Time(4s), 0, 0, 1000}, Ephemeris::uuid());
        rollback<Catalog>({1, bytes("old"), Clock::Time(4s)}, {2, bytes("new"), Clock::Time(5s)}, "key");
        rollback<Ephemeris>({bytes("attr"), bytes("old"), Clock::Time(4s), 0, 0, 1000}, {bytes("attr"), bytes("new"), Clock::Time(5s), 1, 1, 1000}, Ephemeris::uuid());
        std::cout << "Replica preparation: ok\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
