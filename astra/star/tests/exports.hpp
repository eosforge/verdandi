#pragma once
#include "check.hpp"
#include <atomic>
#include <future>
#include <semaphore>
#include <thread>

namespace {
// 两个动态域共用锁边界验证; write 创建一条记录, 不模拟内部来源或公开投影.
template <typename State>
void exports(auto&& write) {

    using namespace std::chrono_literals;
    using astra::Clock;
    const astra::Scope scope{"export", "boundary"}; // 一个公开范围, 两次读取使用同一个真实 State.
    std::atomic_bool pause{}, available{true};      // pause 只阻塞一次取时; available 模拟时钟暂时不可取.
    std::atomic_uint samples{};                     // 导出路径不得隐式取时或触发 GC.
    std::binary_semaphore entered{0}, released{0};  // 分别通知已持有域锁和允许退出取时.
    State state([&]() -> std::optional<Clock::Reading> {
        ++samples;
        if (pause.exchange(false)) {
            entered.release();
            static_cast<void>(released.try_acquire_for(3s)); // 异常路径也有界退出, 不让测试线程永久占锁.
        }
        return available ? std::optional(Clock::Reading{.time = Clock::Time(1s), .ready = true}) : std::nullopt;
    },
                {});
    const auto key = write(state, scope); // 发布返回 Key, 注册返回服务端分配的 UUID.
    CHECK(!key.empty());
    const auto position = state.source().position(); // 一条本机事实, 不依赖具体业务返回值.
    CHECK(position == 1);

    // 暂停独占维护的取时, 此时域写锁仍被持有. 不能只挡住共享读者, 否则错误共用域读锁的导出也可能通过.
    pause = true;
    auto maintenance = std::async(std::launch::async, [&] { state.tick(); }); // 已有有限租约, tick 必须进入取时, 不走空域早退.
    entered.acquire();
    auto exported = std::async(std::launch::async, [&] {
        const auto view = state.source();
        const auto events = state.events(0);
        const auto delivery = state.deliver(0, 8, 4096);
        const auto point = state.resolve(scope, key);
        return view.position() == position && events && events->size() == 1 && delivery && point && point->record;
    });
    const bool independent = exported.wait_for(1s) == std::future_status::ready; // 有界检测, 失败也先解除阻塞再断言.
    released.release();
    maintenance.get(); // 解除阻塞后等待维护完成, 异常在当前测试线程传播.
    CHECK(exported.get() && independent);

    // 来源事实已经包含绝对期限, 导出不依赖当前计时可用性; 公开视图仍明确拒绝时钟错误.
    available = false;
    const auto before = samples.load();
    CHECK(state.source().position() == position && state.events(0) && state.deliver(0, 8, 4096));
    CHECK(state.resolve(scope, key)->record && samples.load() == before);
    CHECK(state.capture(scope) == std::unexpected(State::Error::clock));
}
} // namespace
