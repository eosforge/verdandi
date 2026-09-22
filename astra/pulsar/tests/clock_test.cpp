#include "check.hpp"
#include "physical_clock.hpp"
#include <atomic>
#include <iostream>

using namespace astra;
using namespace std::chrono_literals;

namespace {

// 每 10 ms 调用 predicate, 最多等待 10 s, 未达到条件抛出测试失败.
static void wait_until(auto&& predicate) {

    // end 是测试轮询的单调截止, 不使用可能调整的业务时间.
    const auto end = Steady::now() + 10s;
    while (Steady::now() < end) {
        if (predicate()) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Physical reference did not reach expected state");
}

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
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
        // mode 初始无来源, 由主线程切换, 唯一采样线程只读.
        std::atomic mode{Mode::missing};
        // reference 借用 mode 生成确定性观测, 寿命覆盖 Source 工作线程.
        const auto reference = [&]() -> std::optional<Clock::Estimate> {
            // current 固定本次采样所见模式, 避免一条观测混入两种场景.
            const auto current = mode.load();
            if (current == Mode::missing) {
                return std::nullopt;
            }
            if (current == Mode::failed) {
                throw std::runtime_error("Injected reference failure");
            }

            // local 记录本次观测的 BOOTTIME 锚点, 与合成 Unix 时间配对.
            const auto local = Clock::Elapsed::now();
            // correction 在正跳, 回拨和正常模式分别取 +10 s, -10 s 和 0.
            const auto correction = current == Mode::jumped ? 10s : current == Mode::reversed ? -10s
                                                                                              : 0s;
            // time 使用固定 Unix 基点叠加经过时长, 只在测试中合成物理参考.
            const auto time = Clock::Time(1'800'000'000s) + local.time_since_epoch() + correction;
            // 正常样本使用 400 ms 误差, 验证新门槛确实应用到物理参考层; 超限仅增加到 500 ms + 1 ns.
            return Clock::Estimate{time, local, current == Mode::uncertain ? 500'000'001U : 400'000'000U, 0};
        };
        {
            // source 拥有采样线程, 借用 reference 的捕获直到作用域结束.
            Source source(reference);
            wait_until([&] { return source.precision() != 0; });
            CHECK(!source.now());
            mode.store(Mode::valid);
            wait_until([&] {
                // value 独立保存本次读取, 先检查存在再检查是否满足就绪状态.
                const auto value = source.now();
                return value && value->synchronized;
            });
            // initial 记录首次就绪时间, 后续故障和恢复不能回退它.
            const auto initial = *source.now();
            CHECK(initial.time >= Clock::Time(1'800'000'000s));
            mode.store(Mode::failed);
            wait_until([&] {
                // value 独立保存本次读取, 先检查存在再检查是否满足就绪状态.
                const auto value = source.now();
                return value && !value->synchronized;
            });
            CHECK(source.now()->time >= initial.time);
            CHECK(source.now()->ready && source.now()->deadline_after(1s));
            mode.store(Mode::valid);
            wait_until([&] { return source.now()->synchronized; });
            // jumped 依次选择正跳和回拨, 对两种方向检查连续调速.
            for (const auto jumped : {Mode::jumped, Mode::reversed}) {
                // before 是注入跳变前的完整时钟读数.
                const auto before = *source.now();
                // started 记录本机经过时间起点, 用于估算正常推进上界.
                const auto started = Clock::Elapsed::now();
                mode.store(jumped);
                wait_until([&] { return !source.now()->synchronized; });
                // after 是失去参考质量后的连续读数, 本地推进与有限期限能力应保持有效.
                const auto after = *source.now();
                // elapsed 是两次本机采样之间的时长, 不包含人为墙钟跳变.
                const auto elapsed = Clock::Elapsed::now() - started;
                CHECK(after.time >= before.time && after.uncertainty_ns >= 9'000'000'000ULL);
                // 允许调度抖动, 但输出差必须接近本地经过时间, 不直接跨越十秒校正值.
                CHECK(after.time - before.time < elapsed + 100ms);
                CHECK(after.ready && after.deadline_after(1s) == after.time + 1s);
                mode.store(Mode::valid);
                wait_until([&] { return source.now()->synchronized; });
            }
            mode.store(Mode::uncertain);
            wait_until([&] { return !source.now()->synchronized; });
            CHECK(source.now()->time >= initial.time);
            CHECK(source.now()->ready && source.now()->deadline_after(1s));
        }

        // 空 Provider 是明确配置错误, 不能静默运行一个永远无参考源的线程.
        bool rejected = false;
        try {
            // invalid 故意使用空回调, 构造必须直接拒绝.
            Source invalid(Source::Provider{});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
        // 新 Pulsar 初始化依旧采用同一 Unix 基准, 不使用随机 era 或从进程零点重新编号.
        mode.store(Mode::valid);
        // start 为新实例启动前的 BOOTTIME 下界.
        const auto start = Clock::Elapsed::now();
        // restarted 使用相同 Unix 基准启动新实例, 不沿用旧进程对象.
        Source restarted(reference);
        wait_until([&] {
            // value 独立保存本次读取, 先检查存在再检查是否满足就绪状态.
            const auto value = restarted.now();
            return value && value->synchronized;
        });
        // now 是重启实例首次就绪后的读数, 与前后采样区间比较.
        const auto now = *restarted.now();
        // end 为重启读数取得后的 BOOTTIME 上界.
        const auto end = Clock::Elapsed::now();
        CHECK(now.time >= Clock::Time(1'800'000'000s) + start.time_since_epoch() - 20ms);
        CHECK(now.time <= Clock::Time(1'800'000'000s) + end.time_since_epoch() + 20ms);
        std::cout << "PASS physical reference readiness, fault recovery, slew and stable Unix restart\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
