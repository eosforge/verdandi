#include "check.hpp"
#include "physical_clock.hpp"
#include <atomic>
#include <iostream>

using namespace astra;
using namespace std::chrono_literals;

static void wait_until(auto&& predicate) {
    const auto end = Clock::now() + 10s;
    while (Clock::now() < end) {
        if (predicate()) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Physical reference did not reach expected state");
}

int main() {
    try {
        // 模式仅在测试回调内部使用, 生产没有跳过系统质量检查的 CLI 或环境开关.
        enum class Mode {
            // 无有效时间来源, 冷启动不得初始化.
            missing,
            // 返回连续的合成 Unix 物理时间.
            valid,
            // 模拟墙钟向前跳变十秒.
            jumped,
            // 模拟墙钟回拨十秒, 已发布时间不能回退.
            reversed,
            // 模拟系统读取异常.
            failed,
            // 误差超过允许启动门槛.
            uncertain
        };
        std::atomic mode{Mode::missing};
        const auto reference = [&]() -> std::optional<EpochClock::Estimate> {
            const auto current = mode.load();
            if (current == Mode::missing) {
                return std::nullopt;
            }
            if (current == Mode::failed) {
                throw std::runtime_error("Injected reference failure");
            }
            const auto local = EpochClock::Elapsed::now();
            const auto correction = current == Mode::jumped ? 10s : current == Mode::reversed ? -10s : 0s;
            const auto time = EpochClock::Time(1'800'000'000s) + local.time_since_epoch() + correction;
            // 正常样本使用 400 ms 误差, 验证新门槛确实应用到物理参考层; 超限仅增加到 500 ms + 1 ns.
            return EpochClock::Estimate{time, local, current == Mode::uncertain ? 500'000'001U : 400'000'000U, 0};
        };
        {
            PhysicalClock source(reference);
            wait_until([&] { return source.precision() != 0; });
            CHECK(!source.now());
            mode.store(Mode::valid);
            wait_until([&] {
                const auto value = source.now();
                return value && value->ready;
            });
            const auto initial = *source.now();
            CHECK(initial.time >= EpochClock::Time(1'800'000'000s));
            mode.store(Mode::failed);
            wait_until([&] {
                const auto value = source.now();
                return value && !value->ready;
            });
            CHECK(source.now()->time >= initial.time);
            mode.store(Mode::valid);
            wait_until([&] { return source.now()->ready; });
            for (const auto jumped : {Mode::jumped, Mode::reversed}) {
                const auto before = *source.now();
                const auto started = EpochClock::Elapsed::now();
                mode.store(jumped);
                wait_until([&] { return !source.now()->ready; });
                const auto after = *source.now();
                const auto elapsed = EpochClock::Elapsed::now() - started;
                CHECK(after.time >= before.time && after.uncertainty_ns >= 9'000'000'000ULL);
                // 允许调度抖动, 但输出差必须接近本地经过时间, 不直接跨越十秒校正值.
                CHECK(after.time - before.time < elapsed + 100ms);
                CHECK(!after.deadline_after(1s));
                mode.store(Mode::valid);
                wait_until([&] { return source.now()->ready; });
            }
            mode.store(Mode::uncertain);
            wait_until([&] { return !source.now()->ready; });
            CHECK(source.now()->time >= initial.time);
        }
        // 空 Provider 是明确配置错误, 不能静默运行一个永远无参考源的线程.
        bool rejected = false;
        try {
            PhysicalClock invalid(PhysicalClock::Provider{});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
        // 新 Pulsar 初始化依旧采用同一 Unix 基准, 不使用随机 era 或从进程零点重新编号.
        mode.store(Mode::valid);
        const auto start = EpochClock::Elapsed::now();
        PhysicalClock restarted(reference);
        wait_until([&] {
            const auto value = restarted.now();
            return value && value->ready;
        });
        const auto now = *restarted.now();
        const auto end = EpochClock::Elapsed::now();
        CHECK(now.time >= EpochClock::Time(1'800'000'000s) + start.time_since_epoch() - 20ms);
        CHECK(now.time <= EpochClock::Time(1'800'000'000s) + end.time_since_epoch() + 20ms);
        std::cout << "PASS physical reference readiness, fault recovery, slew and stable Unix restart\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
