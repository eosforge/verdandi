#include "check.hpp"
#include "clock_filter.hpp"
#include "clock_precision.hpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <thread>

using namespace astra;
using namespace std::chrono_literals;

namespace {

// 注入粗时钟而非依赖宿主精度, 前 4096 次完全相等也必须等待真实边沿并覆盖虚假的 1 ns 报告.
static void test_precision_probe() {

    for (const std::int64_t quantum : {1'000'000, 15'000'000}) {
        // reads 为合成读钟次数, 从零开始, 由读回调递增并供独立预算回调检查.
        std::uint64_t reads = 0;
        // measured 是粗粒度合成时钟的实测步长, 应等于 quantum, 不能采信虚假的 1 ns 下限.
        const auto measured = measure_clock_precision([&] { return static_cast<std::int64_t>(reads++ / 4096) * quantum; }, [&] { return reads < 200'000; }, 1);
        CHECK(measured == static_cast<std::uint64_t>(quantum) && reads > 4096);
    }

    // 快时钟读成本小于系统报告时, 仍不能缩小系统给出的分辨率下限.
    std::int64_t fast = 0;
    CHECK(measure_clock_precision([&] { return fast += 20; }, [&] { return fast < 100'000; }, 1000) == 1000);

    // 模拟每次读取仅经过 2 ns, 而对外时钟以 15.6 ms 跳变. 观察窗口由独立经过时间限制.
    // 总读取次数超过旧的 2^22 上限仍应成功; 不依赖当前测试机器实际 CPU 速度或休眠.
    std::int64_t elapsed = 0;
    // coarse 检查快读钟与粗粒度输出组合, 总读取次数可超过旧的固定循环上限.
    const auto coarse = measure_clock_precision([&] { return (elapsed += 2) / 15'600'000 * 15'600'000; }, [&] { return elapsed < 64'000'000; }, 1);
    CHECK(coarse == 15'600'000 && elapsed / 2 > (1U << 22));

    // 完全停滞和只有两次跳变都不能静默信任报告值. 测试预算独立于被测读数, 不等待停滞时钟自行超时.
    for (const bool partly_advancing : {false, true}) {
        // reads 为合成读钟次数, 从零开始, 由读回调递增并供独立预算回调检查.
        std::uint64_t reads = 0;
        // rejected 初始为 false, 只在捕获预期异常时设置, 防止无异常的静默回退通过用例.
        bool rejected = false;
        try {
            static_cast<void>(measure_clock_precision(
                [&] {
                    // sample 保存递增前的读序号, 用于生成停滞或仅两次跳变的非法观测.
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
    // rejected 初始为 false, 只在捕获预期异常时设置, 防止无异常的静默回退通过用例.
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

// 把 value 纳秒包装为本进程经过时间坐标, 不读取真实时钟.
static ElapsedTime local(std::chrono::nanoseconds value) {
    return ElapsedTime(value);
}

// 把 value 纳秒包装为 Unix 坐标, 保持测试中的两个参考系显式分离.
static Clock::Time epoch(std::chrono::nanoseconds value) {
    return Clock::Time(value);
}

// 构造 at 时刻的 unix_time 观测, uncertainty 默认为零纳秒, 往返延迟固定为零.
static Clock::Estimate observation(std::chrono::nanoseconds at, std::chrono::nanoseconds unix_time, std::uint64_t uncertainty = 0) {
    return {epoch(unix_time), local(at), uncertainty, 0};
}

// 检查四时间戳筛选,最少样本数,量化容忍和误差门槛, 全部使用确定性输入.
static void test_filter() {

    // filter 使用默认 1 ns 本机精度, 初始无样本, 少于三个有效观测时不能返回结果.
    Filter filter;
    CHECK(!filter.result());
    CHECK(filter.observe(100, 1110, 1120, local(130ns), 1, 0, true));
    CHECK(filter.observe(200, 1202, 1204, local(206ns), 1, 5, true));
    CHECK(!filter.result());
    CHECK(filter.observe(300, 1310, 1320, local(330ns), 1, 0, true));
    // best 应选取有效样本中的最小网络延迟观测, 保留对应接收时刻和完整误差.
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

    // coarse 使用 15 ms 本机量化精度, 验证负网络延迟只在明确误差范围内被容忍.
    Filter coarse(15'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(coarse.observe(100, 1000, 21000, local(100ns), 1, 0, true));
    }
    CHECK(coarse.result()->rtt_ns == 15'000'000 && coarse.result()->uncertainty_ns == 22'500'002);
    CHECK(!coarse.observe(100, 0, 30'000'003, local(100ns), 1, 0, true));
    CHECK(!coarse.observe(100, 1000, 21000, local(100ns), 0, 0, true));
    CHECK(!coarse.observe(100, 1000, 21000, local(100ns), 20'000'001, 0, true));
    // 200 ms 往返期间同时包含两端频率误差和服务端调速, 不能机械复用旧 500 ppm 预算.
    Filter drifting;
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(drifting.observe(0, 1000, 100'101'000, local(100ms), 1, 0, true));
    }
    CHECK(drifting.result()->rtt_ns == 1 && drifting.result()->uncertainty_ns == 150'004);
}

// 检查双向调速的连续性,步长无关性和观测替换, 不依赖真实时间经过.
static void test_continuity() {

    for (const auto sign : {-1, 1}) {
        // clock 是当前场景独立的默认调速时钟, 初始尚无纪元锚点.
        Clock clock;
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
        // small 高频小步读取, large 一次跨越相同时间, 两者必须消化相同整数调速预算.
        Clock small, large;
        for (auto* clock : {&small, &large}) {
            CHECK(clock->publish(observation(0ns, 100s), local(0ns)));
            CHECK(clock->publish(observation(1ns, 100s + 1ns + sign * 10ms), local(1ns)));
        }
        for (std::int64_t ns = 2; ns <= 10001; ++ns) {
            CHECK(small.now(local(std::chrono::nanoseconds(ns))));
        }
        CHECK(small.now(local(10001ns))->time == large.now(local(10001ns))->time);
    }

    // replacement 连续接收新观测, 验证新样本替换偏差而不直接跳变公开时间.
    Clock replacement;
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

// 覆盖资格过期,参考恢复,计数耗尽和反序失效, 确认坏样本不会恢复永久设施故障.
static void test_quality_and_limits() {

    // clock 是当前场景独立的默认调速时钟, 初始尚无纪元锚点.
    Clock clock;
    CHECK(!clock.publish(observation(0ns, -1ns), local(0ns)));
    CHECK(!clock.publish(observation(0ns, 100s, 500'000'001), local(0ns)));
    CHECK(!clock.publish(observation(0ns, 100s), local(6s)));
    CHECK(clock.publish(observation(0ns, 100s), local(0ns)));
    CHECK(clock.now(local(5s))->ready);
    CHECK(!clock.now(local(5s + 1ns))->ready);
    CHECK(clock.now(local(6s))->time == epoch(106s));
    CHECK(!clock.now(local(6s))->deadline_after(1s));
    CHECK(clock.publish(observation(6s, 106s), local(6s)));
    // ready 为恢复后的同一读数, 用它同时检查合法和负 TTL 的截止计算.
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
    Clock boundary;
    CHECK(boundary.publish(observation(0ns, 100s, 500'000'000), local(0ns)));
    CHECK(boundary.now(local(0ns))->ready);
    CHECK(!boundary.now(local(1ns))->ready);
    // relaxed 使用 400 ms 误差观测, 确认低于当前 500 ms 门槛可正常就绪.
    Clock relaxed;
    CHECK(relaxed.publish(observation(0ns, 100s, 400'000'000), local(0ns)));
    CHECK(relaxed.now(local(0ns))->ready);

    // suspended 一次推进一周经过时间, 验证走时继续但新租约资格过期.
    Clock suspended;
    CHECK(suspended.publish(observation(0ns, 100s), local(0ns)));
    CHECK(suspended.now(local(24h * 7))->time == epoch(100s + 24h * 7));
    CHECK(!suspended.now(local(24h * 7))->ready);
    // overflow 从纳秒坐标上界前一刻开始, 覆盖截止和走时加法耗尽.
    Clock overflow;
    CHECK(overflow.publish({Clock::Time::max() - 1ns, local(0ns), 0, 0}, local(0ns)));
    CHECK(!overflow.now(local(0ns))->deadline_after(2ns));
    CHECK(overflow.now(local(1ns))->time == Clock::Time::max());
    CHECK(!overflow.now(local(2ns)));
    CHECK(!overflow.publish(observation(3ns, 100s), local(3ns)));
    // backwards 接收反序本地计数, 验证永久设施故障不被当作正常参考更新.
    Clock backwards;
    CHECK(backwards.publish(observation(1s, 100s), local(1s)));
    CHECK(!backwards.now(local(0ns)));
    for (const auto rate : {0U, 1001U}) {
        // rejected 初始为 false, 只在捕获预期异常时设置, 防止无异常的静默回退通过用例.
        bool rejected = false;
        try {
            Clock invalid(rate);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

// 并发发布和读取同一时钟, 检查读者看到的公开 Unix 时间从不倒退.
static void test_concurrent_reading() {

    // clock 是当前场景独立的默认调速时钟, 初始尚无纪元锚点.
    Clock clock;
    // origin 记录真实 BOOTTIME 起点, 合成 Unix 观测由同一原点外推.
    const auto origin = Clock::Elapsed::now();
    CHECK(clock.publish({epoch(1'800'000'000s), origin, 0, 0}));
    // finished 初始 false, 发布线程结束后置 true, 读者据此停止循环.
    std::atomic_bool finished{};
    // writer 借用 clock/origin/finished, 显式 join 后这些栈对象才允许析构.
    std::jthread writer([&] {
        for (unsigned i = 0; i < 1000; ++i) {
            // sample 是本次真实采样计数, 可由其他读者先行推进, publish 负责消费时外推.
            const auto sample = Clock::Elapsed::now();
            static_cast<void>(clock.publish({epoch(1'800'000'000s) + (sample - origin), sample, 0, 0}));
        }
        finished.store(true);
    });
    // previous 保存上次公开时间, 初始 Unix 零点, 用于逐次单调性断言.
    auto previous = Clock::Time{};
    do {
        // now 是当前并发读数, 必须存在且不早于 previous.
        const auto now = clock.now();
        CHECK(now && now->time >= previous);
        previous = now->time;
    } while (!finished.load());
    writer.join();
}

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
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
