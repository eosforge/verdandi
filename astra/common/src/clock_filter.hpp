#pragma once
#include <astra/clock.hpp>

namespace astra {
// 单批 NTP 四时间戳过滤器, 从有效观测中选取最低往返延迟样本.
class Filter {
public:
    // local_precision 为本机 BOOTTIME 的 rho, 1 ns..20 ms.
    explicit Filter(std::uint64_t local_precision = 1);
    // t0/t3 属于本机 BOOTTIME, t1/t2 属于 Unix 时间, 一个批次仅对应一个 RPC.
    // 拒绝未同步来源, 反序, 越界和大于 200 ms 往返, 容忍量化/调速预算内的负 delay.
    bool observe(std::uint64_t t0, std::uint64_t t1, std::uint64_t t2, ElapsedTime t3, std::uint64_t precision, std::uint64_t uncertainty, bool synchronized);
    // 至少三个有效样本才返回最小 delay 观测, 仍保留全部精度预算.
    std::optional<Clock::Estimate> result() const;

private:
    // 构造时校验, 不由网络数据扩张.
    std::uint64_t local_precision_;
    // 本批最小 delay 观测, 固定大小且不分配.
    std::optional<Clock::Estimate> best_;
    // 有效样本计数, 饱和于八.
    unsigned count_{};
};
} // namespace astra
