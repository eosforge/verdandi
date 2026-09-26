#pragma once
#include "wheel.hpp"
#include <algorithm>
#include <astra/clock.hpp>
#include <astra/profile.hpp>
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace astra {
// 单个动态来源组的期限调度器, 首个有限租约时才创建. 全部操作由所属域的同一提交保护串行调用.
// 只保存组级时间锚点, 不在每条记录增加另一份 deadline; 真实截止始终由原生记录提供.
class Agenda {
    // 四层各 256 槽, 10 ms 拍覆盖约 497 天; 更远的合法绝对截止在触发时重新安排.
    using Timer = Wheel<4, 8>;

public:
    // 钩子嵌入稳定地址的原生记录句柄, 不拥有载荷, 移动/复制约束沿用 Wheel::Node.
    using Node = Timer::Node;
    // 固定正拍长, 避免配置为零引入除零; 小于一拍的余量保留到下一次推进.
    static constexpr auto interval = std::chrono::milliseconds(10);

    // initial 为当前已建立的非负 Unix 纳秒, 不从 1970 年起补拍; 非法时间抛 invalid_argument.
    explicit Agenda(Clock::Time initial) : time_(initial) {
        if (initial.time_since_epoch().count() < 0) {
            throw std::invalid_argument("Agenda requires nonnegative Unix time");
        }
    }

    // 组级已推进边界, 不代表墙钟或当前瞬间; 只有完整拍改变此值.
    Clock::Time time() const noexcept {
        return time_ + (timer_.now() == tick_ ? std::chrono::milliseconds::zero() : interval);
    }

    // 下一次可能完成整拍的绝对边界, 供外层跳过尚不能推进的全部轮; 极值饱和, 不溢出时间类型.
    Clock::Time next() const noexcept {
        const auto current = time(); // 当前轮的真实相位, 不假定全部来源都对齐整十毫秒.
        return Clock::Time::max() - current < interval ? Clock::Time::max() : current + interval;
    }

    // 按当前边界安排有限 deadline, 已过期记录安排下一拍, 不在写入中同步调用业务.
    // 无分配且不抛错; 调用者先推进到最终受理时间, 再提交原生 deadline 和此钩子.
    void set(Node& node, Clock::Time deadline) noexcept {

        const auto boundary = time();                                                                                              // 回调期间 Wheel 可能已推进, 不能用上一拍锚点再次计算延期.
        const auto remaining = deadline > boundary ? static_cast<std::uint64_t>((deadline - boundary).count()) : 0;                // 剩余纳秒, 非负.
        constexpr auto width = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count()); // 每拍纳秒, 严格为正.
        const auto delay = remaining / width + static_cast<std::uint64_t>(remaining % width != 0);                                 // 向上取整, 加法不先扩大 remaining.
        static_cast<void>(timer_.schedule(node, std::min(delay, Timer::limit)));
    }

    // 撤销调度但不销毁节点; 已摘下的钩子同样允许调用.
    static void erase(Node& node) noexcept {
        Timer::cancel(node);
    }

    // 完整推进到 now 前的最后一拍. fire(node, boundary) 必须重新检查真实截止, 不能把分段唤醒当作到期.
    // fire 成功可销毁节点; 抛错前必须保持当前节点有效且业务未提交, 本方法重排它并原样传播异常.
    // 先前已完整提交的到期不回滚. 失败时调用方必须停止受影响视图的追平承诺, 不伪装整组推进成功.
    template <typename F>
    void advance(Clock::Time now, F&& fire) {

        ASTRA_PROFILE_SCOPE("star.agenda.advance");

        if (now < time_) {
            throw std::invalid_argument("Agenda time cannot move backwards");
        }
        const auto pending = static_cast<std::uint64_t>((now - time_) / interval); // 待补完整拍数, 不截断且不丢亚拍余量.
        for (std::uint64_t step = 0; step < pending; ++step) {
            try {
                timer_.tick([&](Node* node) {
                    const auto boundary = time(); // 区分前次异常遗留的 ready 队列与本次新拍.
                    try {
                        fire(node, boundary);
                    } catch (...) {
                        static_cast<void>(timer_.schedule(*node, 1)); // 当前记录未提交, 无分配重排, 下一拍仍会被处理.
                        throw;
                    }
                });
            } catch (...) {
                time_ = time(); // Wheel 已走一拍时同步锚点, 不重复补拍或把失败当作未推进.
                tick_ = timer_.now();
                throw;
            }
            time_ = time();
            tick_ = timer_.now();
        }
    }

private:
    // 实际组级拍边界, 与 timer_ 已完成拍数保持一致; 不保存每个记录的剩余 TTL.
    Clock::Time time_;
    // 与 time_ 同步的 Wheel 位置, 在回调中识别已推进的一拍, 不增加逐记录时间字段.
    std::uint64_t tick_{};
    // 稳定地址的组级时间轮, 与 Node 任意先后销毁均由 Wheel 解除钩子.
    Timer timer_;
};
} // namespace astra
