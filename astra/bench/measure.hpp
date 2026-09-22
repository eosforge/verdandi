#pragma once
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

// 基准专用统计, 不链接进生产库. 调用方负责隔离场景和说明负载模型.
class Measure {
public:
    using Clock = std::chrono::steady_clock; // 所有延迟来自本进程单调时钟, 不依赖对时时间.

    // 完整解析有界十进制参数, 先验证再缩窄到调用方类型, 拒绝符号、尾部垃圾及溢出.
    static std::uint64_t number(std::string_view text, std::uint64_t minimum, std::uint64_t maximum) {
        std::uint64_t value{};                                                              // 失败时绝不返回这个初始零值.
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value); // 解析不分配, 不借助区域设置.
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < minimum || value > maximum) {
            throw std::invalid_argument("Invalid unsigned benchmark argument");
        }
        return value;
    }

    // 将有界样本排序后输出 JSON. count 是确认成功次数, seconds 包含整个测量窗口.
    // samples 单位纳秒; 空集合表示该阶段没有可用延迟, 不伪造零分位数.
    static void report(std::string_view name, std::vector<std::int64_t> samples, double seconds, std::size_t count) {

        std::ranges::sort(samples); // 排序发生在计时窗口之后, 不计入操作耗时.
        std::cout << "{\"metric\":\"" << name << "\",\"count\":" << count << ",\"seconds\":" << seconds << ",\"operations_per_second\":" << (seconds > 0 ? static_cast<double>(count) / seconds : 0) << ",\"samples\":" << samples.size();

        // percentile/value 分别为固定标签和最近秩百分位, 输出微秒以兼容本地操作及 RPC.
        for (const auto& [percentile, value] : {std::pair{"p50_us", 0.5}, {"p95_us", 0.95}, {"p99_us", 0.99}, {"p999_us", 0.999}}) {
            std::cout << ",\"" << percentile << "\":";
            if (samples.empty()) {
                std::cout << "null";
            } else {
                const auto index = static_cast<std::size_t>(std::ceil(value * static_cast<double>(samples.size()))) - 1; // 非空集合的合法最近秩下标.
                std::cout << static_cast<double>(samples[index]) / 1000.0;
            }
        }
        std::cout << "}" << std::endl;
    }

    // 固定次数的单线程闭环微基准, 保留每次延迟. action 必须验证操作成功并阻止结果被优化掉.
    static void run(std::string_view name, std::size_t count, auto&& action) {

        std::vector<std::int64_t> samples(count); // 预分配全部计时样本, 不让扩容污染热路径.
        const auto begin = Clock::now();          // 吞吐包括采样开销, 不作虚假扣除.
        for (std::size_t index = 0; index < count; ++index) {
            const auto start = Clock::now(); // index 是操作次序; 单次结束时立即采样.
            action(index);
            samples[index] = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        }
        const auto seconds = std::chrono::duration<double>(Clock::now() - begin).count(); // 完整测量窗口秒数.
        report(name, std::move(samples), seconds, count);
    }

    // 失败不计作成功吞吐, 由 main 报错并使整次样本无效.
    static void require(bool condition) {
        if (!condition) {
            throw std::runtime_error("Performance operation failed validation");
        }
    }
};
