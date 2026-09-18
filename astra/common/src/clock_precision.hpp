// 功能: 有界寻找实际读钟跳变, 避免仅依赖系统报告值或在停滞时钟上无限自旋.
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace astra {
// read 返回非负单调纳秒; running 由独立预算/取消源控制, 不能仅以 read 的读数或次数判断超时.
// minimum 为系统报告与表示单位的较大值, 范围 1 ns..20 ms. 采样不足或反序显式抛错.
inline std::uint64_t measure_clock_precision(auto read, auto running, std::uint64_t minimum) {
    if (minimum == 0 || minimum > 20'000'000) {
        throw std::invalid_argument("Invalid minimum clock precision");
    }
    // before 始终保留最近一次读数, 相同读数不算有效样本, 必须等待真正跨过滴答边界.
    std::int64_t before = read();
    if (before < 0) {
        throw std::runtime_error("Negative monotonic clock reading");
    }
    // best 是实际正增量的最小值, edges 只计时钟跳变, 不把相等读数伪装成低成本测量.
    auto best = std::numeric_limits<std::uint64_t>::max();
    unsigned edges = 0;
    // 目标 32 次跳变, 每 128 次读钟检查预算. 128 只控制检查开销, 不限制总采样次数或时钟合法性.
    while (edges < 32 && running()) {
        for (unsigned sample = 0; sample < 128 && edges < 32; ++sample) {
            const std::int64_t after = read();
            if (after < before) {
                throw std::runtime_error("Monotonic clock moved backwards");
            }
            if (after > before) {
                best = std::min(best, static_cast<std::uint64_t>(after - before));
                ++edges;
            }
            before = after;
        }
    }
    // 允许粗时钟在预算内少于 32 次跳变, 但至少需要三个样本; 不再无样本回退到报告精度.
    if (edges < 3 || best > 20'000'000) {
        throw std::runtime_error("Cannot measure supported monotonic clock precision");
    }
    return std::max(minimum, best);
}
} // namespace astra
