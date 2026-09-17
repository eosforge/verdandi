// 功能: 验证时间轮精确到期, 侵入式生命周期, 回调改期与异常恢复, 并与简单定时器模型逐拍对照.
#include "check.hpp"
#include "wheel.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <type_traits>

namespace {
// 测试默认五层轮. 每个测试拥有独立轮与节点, 不依赖墙钟或测试执行次序.
using Timer = astra::Wheel<>;

// 通过 requires 表达模板参数可用性, 无效配置必须在实例化前拒绝.
template <std::size_t Levels, std::size_t Bits>
concept ValidWheel = requires { typename astra::Wheel<Levels, Bits>; };

static_assert(!ValidWheel<0, 8> && !ValidWheel<3, 0> && !ValidWheel<1, 9> && !ValidWheel<9, 8>);
static_assert(ValidWheel<1, 1> && ValidWheel<3, 8> && ValidWheel<8, 8> && ValidWheel<64, 1>);
static_assert(Timer::limit == 1'099'511'627'775 && astra::Wheel<8, 8>::limit == UINT64_MAX);
static_assert(!std::is_copy_constructible_v<Timer> && !std::is_move_constructible_v<Timer>);
static_assert(!std::is_copy_assignable_v<Timer> && !std::is_move_assignable_v<Timer>);
static_assert(!std::is_copy_constructible_v<Timer::Node> && !std::is_move_constructible_v<Timer::Node>);
static_assert(!std::is_copy_assignable_v<Timer::Node> && !std::is_move_assignable_v<Timer::Node>);
static_assert(std::is_nothrow_destructible_v<Timer> && std::is_nothrow_destructible_v<Timer::Node>);
static_assert(noexcept(std::declval<Timer&>().schedule(std::declval<Timer::Node&>(), 1)));

// 单个定时器严格在 delay 拍触发, 0 也只能在下一拍触发. initial 可使到期跨槽, 跨层或跨 uint64_t 回绕.
template <typename W> void check_deadline(std::uint64_t initial, std::uint64_t delay) {
    // wheel 使用指定逻辑起点; node 的寿命短于 wheel, 退出时验证自动摘链不访问无效对象.
    W wheel(initial);
    typename W::Node node;
    // effective 是简单参考模型的剩余拍数; calls 检查到期恰好一次.
    const auto effective = std::max(delay, std::uint64_t{1});
    std::size_t calls = 0;
    CHECK(wheel.schedule(node, delay));
    CHECK(node.scheduled());
    for (std::uint64_t elapsed = 1; elapsed <= effective + 1; ++elapsed) {
        wheel.tick([&](auto* value) {
            CHECK(value == &node);
            CHECK(!node.scheduled());
            CHECK(elapsed == effective);
            CHECK(wheel.now() == initial + effective);
            ++calls;
        });
        CHECK(calls == (elapsed >= effective ? 1U : 0U));
    }
}

// 默认配置覆盖层边界; 小配置穷举所有起点和延迟, 包括最大允许延迟与最高层循环.
void test_deadlines() {
    for (const auto delay : {0U, 1U, 2U, 255U, 256U, 257U, 65'535U, 65'536U, 65'537U}) {
        check_deadline<Timer>(0, delay);
        check_deadline<Timer>(253, delay);
        check_deadline<Timer>(UINT64_MAX - 257, delay);
    }
    for (std::uint64_t initial = 0; initial < 64; ++initial) {
        for (std::uint64_t delay = 0; delay <= astra::Wheel<3, 2>::limit; ++delay) {
            check_deadline<astra::Wheel<3, 2>>(initial, delay);
        }
    }
    check_deadline<astra::Wheel<1, 1>>(UINT64_MAX, 1);
    check_deadline<astra::Wheel<8, 8>>(UINT64_MAX - 1, 3);
    check_deadline<astra::Wheel<64, 1>>(UINT64_MAX - 1, 3);
    // 压缩槽宽覆盖五层的全部级联和最大延迟, 避免测试默认第五层时逐拍等待 2^32 次.
    for (std::uint64_t initial = 0; initial < 32; ++initial) {
        for (std::uint64_t delay = 0; delay <= astra::Wheel<5, 1>::limit; ++delay) {
            check_deadline<astra::Wheel<5, 1>>(initial, delay);
        }
    }
    check_deadline<astra::Wheel<5, 2>>(1000, astra::Wheel<5, 2>::limit);
    // 默认槽宽在高位边界与整数回绕附近运行, 保证 countr_zero 触发的多层路径有覆盖.
    for (const auto boundary : {std::uint64_t{1} << 24, std::uint64_t{1} << 32, std::uint64_t{1} << 40}) {
        check_deadline<Timer>(boundary - 257, 1024);
    }
}

// 无效改期保留原计划; 有效改期移除旧计划, cancel 可重复调用且能处理桶头/中间/尾部.
void test_schedule_and_cancel() {
    Timer wheel;
    std::array<Timer::Node, 3> nodes;
    std::size_t calls = 0;
    CHECK(wheel.schedule(nodes[0], 1));
    CHECK(!wheel.schedule(nodes[0], Timer::limit + 1));
    CHECK(nodes[0].scheduled());
    wheel.tick([&](auto*) { ++calls; });
    CHECK(calls == 1);
    CHECK(wheel.schedule(nodes[0], 1));
    CHECK(wheel.schedule(nodes[0], 2));
    wheel.tick([&](auto*) { ++calls; });
    CHECK(calls == 1);
    wheel.tick([&](auto*) { ++calls; });
    CHECK(calls == 2);

    // 每个位置都在三节点同桶状态下独立取消, 不依赖回调顺序.
    for (std::size_t victim = 0; victim < nodes.size(); ++victim) {
        for (auto& node : nodes) {
            CHECK(wheel.schedule(node, 1));
        }
        Timer::cancel(nodes[victim]);
        Timer::cancel(nodes[victim]);
        calls = 0;
        wheel.tick([&](auto* node) {
            CHECK(node != &nodes[victim]);
            ++calls;
        });
        CHECK(calls == 2);
    }
}

// 回调取消另一个已到期节点必须立即生效, 不能沿提前保存的 next 再访问它.
void test_callback_cancel() {
    Timer wheel;
    std::array<Timer::Node, 2> nodes;
    std::size_t calls = 0;
    for (auto& node : nodes) {
        CHECK(wheel.schedule(node, 1));
    }
    wheel.tick([&](auto* current) {
        ++calls;
        // other 无论实际回调顺序如何, 都是当前批次尚未触发的那个节点.
        auto& other = current == &nodes[0] ? nodes[1] : nodes[0];
        Timer::cancel(other);
    });
    CHECK(calls == 1 && !nodes[0].scheduled() && !nodes[1].scheduled());
}

// 当前节点自销毁并释放同批另一节点, 析构取消不得访问已释放前驱. Sanitizer 能直接检查这些路径.
void test_callback_destroy() {
    Timer wheel;
    std::array<std::unique_ptr<Timer::Node>, 2> nodes{std::make_unique<Timer::Node>(), std::make_unique<Timer::Node>()};
    std::size_t calls = 0;
    for (auto& node : nodes) {
        CHECK(wheel.schedule(*node, 1));
    }
    wheel.tick([&](auto* current) {
        ++calls;
        // first 指向当前节点, 先销毁它以暴露其他节点仍引用旧前驱时的悬垂访问.
        const auto first = current == nodes[0].get() ? 0U : 1U;
        nodes[first].reset();
        nodes[1U - first].reset();
    });
    CHECK(calls == 1 && !nodes[0] && !nodes[1]);
}

// 当前节点和同批其他节点都可改到下一拍, 不应在本拍无限循环或丢掉未来调度.
void test_callback_reschedule() {
    Timer wheel;
    std::array<Timer::Node, 2> nodes;
    std::size_t calls = 0;
    for (auto& node : nodes) {
        CHECK(wheel.schedule(node, 1));
    }
    wheel.tick([&](auto*) {
        ++calls;
        for (auto& node : nodes) {
            CHECK(wheel.schedule(node, 0));
        }
    });
    CHECK(calls == 1);
    wheel.tick([&](auto*) { ++calls; });
    CHECK(calls == 3 && !nodes[0].scheduled() && !nodes[1].scheduled());
}

// 回调异常不丢失剩余队列, 下次 tick 先恢复原拍再推进; 已抛异常的回调不自动重试.
void test_callback_exception() {
    Timer wheel;
    std::array<Timer::Node, 3> nodes;
    Timer::Node future;
    std::size_t calls = 0;
    Timer::Node* consumed = nullptr;
    for (auto& node : nodes) {
        CHECK(wheel.schedule(node, 1));
    }
    CHECK(wheel.schedule(future, 2));
    try {
        wheel.tick([&](auto* node) {
            consumed = node;
            ++calls;
            throw 17;
        });
        CHECK(false);
    } catch (int value) {
        CHECK(value == 17);
    }
    CHECK(calls == 1 && wheel.now() == 1 && consumed != nullptr && !consumed->scheduled());
    // 仍挂在 ready_ 的节点可从 tick 外取消, 此时其前驱必须指向有效地址.
    auto& cancelled = consumed == &nodes[0] ? nodes[1] : nodes[0];
    Timer::cancel(cancelled);
    wheel.tick([&](auto* node) {
        CHECK(node != consumed && node != &cancelled);
        CHECK(wheel.now() == (node == &future ? 2U : 1U));
        ++calls;
    });
    CHECK(calls == 3 && wheel.now() == 2);
    for (const auto& node : nodes) {
        CHECK(!node.scheduled());
    }

    // 连续失败也不能让逻辑时钟越过尚未处理的同拍节点, 最后一次恢复应正常推进.
    for (auto& node : nodes) {
        CHECK(wheel.schedule(node, 1));
    }
    calls = 0;
    for (std::size_t attempt = 0; attempt < nodes.size(); ++attempt) {
        try {
            wheel.tick([&](auto*) {
                ++calls;
                throw 19;
            });
            CHECK(false);
        } catch (int value) {
            CHECK(value == 19);
        }
        CHECK(calls == attempt + 1 && wheel.now() == 3);
    }
    wheel.tick([](auto*) { CHECK(false); });
    CHECK(wheel.now() == 4);
}

// 同轮递归 tick 明确失败且不污染外层标记; 后续普通 tick 仍可使用.
void test_reentrant_tick() {
    Timer wheel;
    Timer::Node node;
    CHECK(wheel.schedule(node, 1));
    bool rejected = false;
    wheel.tick([&](auto*) {
        try {
            wheel.tick([](auto*) {});
        } catch (const std::logic_error&) {
            rejected = true;
        }
        CHECK(wheel.now() == 1);
        CHECK(wheel.schedule(node, 1));
    });
    CHECK(rejected);
    std::size_t calls = 0;
    wheel.tick([&](auto*) { ++calls; });
    CHECK(calls == 1 && wheel.now() == 2);
}

// 节点先销毁自动摘链, Wheel 先销毁清空外部钩子, 同配置的两个 Wheel 可显式转移一个节点.
void test_lifetime_and_transfer() {
    Timer::Node survivor;
    {
        Timer wheel;
        CHECK(wheel.schedule(survivor, 256));
        {
            Timer::Node temporary;
            CHECK(wheel.schedule(temporary, 1));
        }
        wheel.tick([](auto*) { CHECK(false); });
    }
    CHECK(!survivor.scheduled());
    Timer::cancel(survivor);

    Timer first;
    Timer second(100);
    CHECK(first.schedule(survivor, 1));
    CHECK(second.schedule(survivor, 1));
    first.tick([](auto*) { CHECK(false); });
    std::size_t calls = 0;
    // 仅可移动的回调能直接调用, 验证没有 std::function 包装和隐藏复制.
    second.tick([state = std::make_unique<int>(7), &calls](auto*) mutable {
        CHECK(*state == 7);
        ++calls;
    });
    CHECK(calls == 1 && !survivor.scheduled());

    // Wheel 在异常后仍能安全析构 ready_ 中未触发的节点.
    std::array<Timer::Node, 3> pending;
    {
        Timer wheel;
        for (auto& node : pending) {
            CHECK(wheel.schedule(node, 1));
        }
        try {
            wheel.tick([](auto*) { throw 1; });
        } catch (int) {}
    }
    for (const auto& node : pending) {
        CHECK(!node.scheduled());
    }
}

// 使用与时间轮无关的逐拍倒计时模型, 对随机调度/取消/改期结果逐节点比较; seed 固定以便复现.
template <typename W> void test_model(std::uint64_t initial, std::uint64_t seed) {
    // Item 的 id 是测试标识, 业务对象可用同样方式嵌入 Node, 不增加生产钩子的大小.
    struct Item : W::Node {
        std::size_t id{};
    };
    W wheel(initial);
    std::array<Item, 64> items;
    // remaining 是独立模型, nullopt 表示未调度. seen 标记本拍是否已回调, 用于发现重复触发.
    std::array<std::optional<std::uint64_t>, 64> remaining{};
    std::array<bool, 64> seen{};
    std::mt19937_64 random(seed);
    const auto limit = std::min(W::limit, std::uint64_t{1000});
    std::uint64_t elapsed = 0;
    for (std::size_t id = 0; id < items.size(); ++id) {
        items[id].id = id;
    }
    // advance 先推进倒计时模型, 再对实际回调及所有节点的挂链状态做独立检查.
    auto advance = [&] {
        seen.fill(false);
        ++elapsed;
        for (auto& counter : remaining) {
            if (counter) {
                --*counter;
            }
        }
        wheel.tick([&](auto* node) {
            const auto id = static_cast<Item*>(node)->id;
            CHECK(remaining[id] && *remaining[id] == 0 && !seen[id]);
            seen[id] = true;
        });
        CHECK(wheel.now() == initial + elapsed);
        for (std::size_t id = 0; id < items.size(); ++id) {
            CHECK(seen[id] == (remaining[id] && *remaining[id] == 0));
            if (seen[id]) {
                remaining[id].reset();
            }
            CHECK(items[id].scheduled() == remaining[id].has_value());
        }
    };
    for (std::size_t step = 0; step < 30'000; ++step) {
        const auto id = static_cast<std::size_t>(random() % items.size());
        const auto operation = random() % 4;
        if (operation < 2) {
            const auto delay = random() % (limit + 1);
            CHECK(wheel.schedule(items[id], delay));
            remaining[id] = std::max(delay, std::uint64_t{1});
        } else if (operation == 2) {
            W::cancel(items[id]);
            remaining[id].reset();
        } else {
            advance();
        }
    }
    for (std::uint64_t step = 0; step <= limit; ++step) {
        advance();
    }
    for (const auto& counter : remaining) {
        CHECK(!counter);
    }
}
} // namespace

// 独立进程入口, 所有断言在 Release 中仍生效. 测试只推进逻辑时钟, 不启动服务或休眠.
int main() {
    try {
        test_deadlines();
        test_schedule_and_cancel();
        test_callback_cancel();
        test_callback_destroy();
        test_callback_reschedule();
        test_callback_exception();
        test_reentrant_tick();
        test_lifetime_and_transfer();
        test_model<astra::Wheel<1, 4>>(0, 11);
        test_model<astra::Wheel<3, 2>>(61, 22);
        test_model<Timer>(65'530, 33);
        test_model<Timer>(UINT64_MAX - 1024, 44);
        test_model<astra::Wheel<8, 8>>(UINT64_MAX - 1024, 55);
        test_model<astra::Wheel<64, 1>>(UINT64_MAX - 1024, 66);
        std::cout << "Wheel boundary, lifetime, callback and 180000 model operations passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
