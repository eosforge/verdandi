// 功能: 确定性校验四时间戳, 连续 Unix 走时和质量门槛, 不依赖真实调度精度.
#include "check.hpp"
#include "clock_filter.hpp"
#include "clock_precision.hpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <thread>

using namespace astra;
using namespace std::chrono_literals;

// 注入粗时钟而非依赖宿主精度, 前 4096 次完全相等也必须等待真实边沿并覆盖虚假的 1 ns 报告.
static void test_precision_probe() {
    for (const std::int64_t quantum : {1'000'000, 15'000'000}) {
        std::uint64_t reads = 0;
        const auto measured = measure_clock_precision([&] { return static_cast<std::int64_t>(reads++ / 4096) * quantum; }, [&] { return reads < 200'000; }, 1);
        CHECK(measured == static_cast<std::uint64_t>(quantum) && reads > 4096);
    }
    // 快时钟读成本小于系统报告时, 仍不能缩小系统给出的分辨率下限.
    std::int64_t fast = 0;
    CHECK(measure_clock_precision([&] { return fast += 20; }, [&] { return fast < 100'000; }, 1000) == 1000);

    // 模拟每次读取仅经过 2 ns, 而对外时钟以 15.6 ms 跳变. 观察窗口由独立经过时间限制.
    // 总读取次数超过旧的 2^22 上限仍应成功; 不依赖当前测试机器实际 CPU 速度或休眠.
    std::int64_t elapsed = 0;
    const auto coarse = measure_clock_precision([&] { return (elapsed += 2) / 15'600'000 * 15'600'000; }, [&] { return elapsed < 64'000'000; }, 1);
    CHECK(coarse == 15'600'000 && elapsed / 2 > (1U << 22));

    // 完全停滞和只有两次跳变都不能静默信任报告值. 测试预算独立于被测读数, 不等待停滞时钟自行超时.
    for (const bool partly_advancing : {false, true}) {
        std::uint64_t reads = 0;
        bool rejected = false;
        try {
            static_cast<void>(measure_clock_precision(
                [&] {
                    const auto sample = reads++;
                    return partly_advancing ? static_cast<std::int64_t>(std::min(sample, std::uint64_t{2})) : std::int64_t{0};
                },
                [&] { return reads < 2048; }, 1));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected && reads >= 2048 && reads <= 2176);
    }
    // 反序立即拒绝, 不让有符号回绕变成一个可接受的大精度数.
    std::int64_t backwards = 100;
    bool rejected = false;
    try {
        static_cast<void>(measure_clock_precision([&] { return --backwards; }, [] { return true; }, 1));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected);

    // 真实取消源在进入观察循环时即生效, 不等待 200 ms. 这里不修改系统时间或时钟配置.
    std::stop_source cancellation;
    cancellation.request_stop();
    rejected = false;
    try {
        static_cast<void>(elapsed_precision_ns(cancellation.get_token()));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected);
}

static ElapsedTime local(std::chrono::nanoseconds value) {
    return ElapsedTime(value);
}
static EpochTime epoch(std::chrono::nanoseconds value) {
    return EpochTime(value);
}
static ClockEstimate observation(std::chrono::nanoseconds at, std::chrono::nanoseconds unix_time, std::uint64_t uncertainty = 0) {
    return {epoch(unix_time), local(at), uncertainty, 0};
}

static void test_filter() {
    ClockFilter filter;
    CHECK(!filter.result());
    CHECK(filter.observe(100, 1110, 1120, local(130ns), 1, 0, true));
    CHECK(filter.observe(200, 1202, 1204, local(206ns), 1, 5, true));
    CHECK(!filter.result());
    CHECK(filter.observe(300, 1310, 1320, local(330ns), 1, 0, true));
    const auto best = filter.result();
    CHECK(best && best->time == epoch(1206ns) && best->rtt_ns == 4 && best->sampled == local(206ns));
    CHECK(best->uncertainty_ns == 11);
    CHECK(!filter.observe(0, 0, 0, local(0ns), 1, 0, false));
    CHECK(!filter.observe(10, 11, 9, local(20ns), 1, 0, true));
    CHECK(!filter.observe(30, 11, 12, local(20ns), 1, 0, true));
    CHECK(!filter.observe(0, 0, 100, local(20ns), 1, 0, true));
    CHECK(!filter.observe(0, 0, 0, local(200'000'001ns), 1, 0, true));
    CHECK(!filter.observe(UINT64_MAX, 0, 0, local(20ns), 1, 0, true));
    CHECK(!filter.observe(0, UINT64_MAX, UINT64_MAX, local(20ns), 1, 0, true));
    CHECK(filter.observe(0, 0, 0, local(20ns), 1, 400'000'000, true));
    CHECK(filter.observe(0, 0, 0, local(20ns), 1, 500'000'000, true));
    CHECK(!filter.observe(0, 0, 0, local(20ns), 1, 500'000'001, true));

    ClockFilter coarse(15'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(coarse.observe(100, 1000, 21000, local(100ns), 1, 0, true));
    }
    CHECK(coarse.result()->rtt_ns == 15'000'000 && coarse.result()->uncertainty_ns == 22'500'002);
    CHECK(!coarse.observe(100, 0, 30'000'003, local(100ns), 1, 0, true));
    CHECK(!coarse.observe(100, 1000, 21000, local(100ns), 0, 0, true));
    CHECK(!coarse.observe(100, 1000, 21000, local(100ns), 20'000'001, 0, true));
    // 200 ms 往返期间同时包含两端频率误差和服务端调速, 不能机械复用旧 500 ppm 预算.
    ClockFilter drifting;
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(drifting.observe(0, 1000, 100'101'000, local(100ms), 1, 0, true));
    }
    CHECK(drifting.result()->rtt_ns == 1 && drifting.result()->uncertainty_ns == 150'004);
}

static void test_continuity() {
    for (const auto sign : {-1, 1}) {
        EpochClock clock;
        CHECK(!clock.now(local(0ns)));
        CHECK(clock.publish(observation(0ns, 100s), local(0ns)));
        CHECK(clock.publish(observation(1s, 101s + sign * 10ms), local(1s)));
        // 接受新观测的同一时刻不跳变, 一秒后仅消化一毫秒.
        CHECK(clock.now(local(1s))->time == epoch(101s));
        CHECK(clock.now(local(2s))->time == epoch(102s + sign * 1ms));
        CHECK(clock.now(local(11s))->time == epoch(111s + sign * 10ms));
        CHECK(clock.now(local(12s))->time == epoch(112s + sign * 10ms));
    }
    // 高频小步与一次大步的整数余数相同, 不依赖读取频率.
    for (const auto sign : {-1, 1}) {
        EpochClock small, large;
        for (auto* clock : {&small, &large}) {
            CHECK(clock->publish(observation(0ns, 100s), local(0ns)));
            CHECK(clock->publish(observation(1ns, 100s + 1ns + sign * 10ms), local(1ns)));
        }
        for (std::int64_t ns = 2; ns <= 10001; ++ns) {
            CHECK(small.now(local(std::chrono::nanoseconds(ns))));
        }
        CHECK(small.now(local(10001ns))->time == large.now(local(10001ns))->time);
    }
    EpochClock replacement;
    CHECK(replacement.publish(observation(0ns, 100s), local(0ns)));
    CHECK(replacement.publish(observation(1s, 101s + 10ms), local(1s)));
    CHECK(replacement.publish(observation(2s, 102s + 10ms), local(2s)));
    CHECK(replacement.now(local(11s))->time == epoch(111s + 10ms));
    // 采样后存在并发读者推进并不使样本作废, 用消费时刻外推而非倒退到旧 T3.
    CHECK(replacement.now(local(12s)));
    CHECK(replacement.publish(observation(11500ms, 111500ms + 10ms), local(12s)));
    CHECK(replacement.now(local(12s))->time == epoch(112s + 10ms));
    CHECK(!replacement.publish(observation(11s, 999s), local(12s)));
    CHECK(replacement.now(local(12s))->time == epoch(112s + 10ms));
}

static void test_quality_and_limits() {
    EpochClock clock;
    CHECK(!clock.publish(observation(0ns, -1ns), local(0ns)));
    CHECK(!clock.publish(observation(0ns, 100s, 500'000'001), local(0ns)));
    CHECK(!clock.publish(observation(0ns, 100s), local(6s)));
    CHECK(clock.publish(observation(0ns, 100s), local(0ns)));
    CHECK(clock.now(local(5s))->ready);
    CHECK(!clock.now(local(5s + 1ns))->ready);
    CHECK(clock.now(local(6s))->time == epoch(106s));
    CHECK(!clock.now(local(6s))->deadline_after(1s));
    CHECK(clock.publish(observation(6s, 106s), local(6s)));
    const auto ready = clock.now(local(6s));
    CHECK(ready->deadline_after(1s) == epoch(107s));
    CHECK(!ready->deadline_after(-1ns));
    clock.revoke();
    CHECK(!clock.now(local(7s))->ready && clock.now(local(7s))->time == epoch(107s));
    // 来源重启即使给出相差很大的新估计, 也只能影响质量和校正速度.
    CHECK(clock.publish(observation(8s, 5000s), local(8s)));
    CHECK(clock.now(local(8s))->time == epoch(108s) && !clock.now(local(8s))->ready);
    CHECK(clock.now(local(9s))->time == epoch(109s + 1ms));
    CHECK(clock.now(local(9s))->uncertainty_ns >= 4'891'000'000'000ULL);

    // 门槛明确锁定为 500 ms. 边界可接受, 再老化一纳秒也不能隐去新增误差.
    EpochClock boundary;
    CHECK(boundary.publish(observation(0ns, 100s, 500'000'000), local(0ns)));
    CHECK(boundary.now(local(0ns))->ready);
    CHECK(!boundary.now(local(1ns))->ready);
    EpochClock relaxed;
    CHECK(relaxed.publish(observation(0ns, 100s, 400'000'000), local(0ns)));
    CHECK(relaxed.now(local(0ns))->ready);

    EpochClock suspended;
    CHECK(suspended.publish(observation(0ns, 100s), local(0ns)));
    CHECK(suspended.now(local(24h * 7))->time == epoch(100s + 24h * 7));
    CHECK(!suspended.now(local(24h * 7))->ready);
    EpochClock overflow;
    CHECK(overflow.publish({EpochTime::max() - 1ns, local(0ns), 0, 0}, local(0ns)));
    CHECK(!overflow.now(local(0ns))->deadline_after(2ns));
    CHECK(overflow.now(local(1ns))->time == EpochTime::max());
    CHECK(!overflow.now(local(2ns)));
    CHECK(!overflow.publish(observation(3ns, 100s), local(3ns)));
    EpochClock backwards;
    CHECK(backwards.publish(observation(1s, 100s), local(1s)));
    CHECK(!backwards.now(local(0ns)));
    for (const auto rate : {0U, 1001U}) {
        bool rejected = false;
        try {
            EpochClock invalid(rate);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

static void test_concurrent_reading() {
    EpochClock clock;
    const auto origin = ElapsedClock::now();
    CHECK(clock.publish({epoch(1'800'000'000s), origin, 0, 0}));
    std::atomic_bool finished{};
    std::jthread writer([&] {
        for (unsigned i = 0; i < 1000; ++i) {
            const auto sample = ElapsedClock::now();
            static_cast<void>(clock.publish({epoch(1'800'000'000s) + (sample - origin), sample, 0, 0}));
        }
        finished.store(true);
    });
    auto previous = EpochTime{};
    do {
        const auto now = clock.now();
        CHECK(now && now->time >= previous);
        previous = now->time;
    } while (!finished.load());
    writer.join();
}

int main() {
    try {
        test_precision_probe();
        test_filter();
        test_continuity();
        test_quality_and_limits();
        test_concurrent_reading();
        std::cout << "PASS Unix clock filtering, continuous slew, holdover, quality and concurrent reading\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
