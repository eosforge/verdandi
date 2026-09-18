#pragma once
#include <astra/types.hpp>
#include <mutex>
#include <optional>
#include <stop_token>

namespace astra {
// Pulsar/Star 共用的新有限租约误差门槛, 含上游, 网络, 残余校正与样本年龄, 单位 ns.
inline constexpr std::uint64_t clock_uncertainty_limit_ns = 500'000'000;

// 统一业务纪元时钟: 对外提供单调不减的 Unix 纳秒, 读取不分配, 不写盘, 不调用 RPC.
class Clock {
public:
    // chrono 坐标标签, 不提供 now(), 防止未校准墙钟直接进入业务数据.
    struct Origin;
    using Time = std::chrono::time_point<Origin, std::chrono::nanoseconds>;

    // Linux BOOTTIME 计入 suspend, 只用于进程内采样/外推, 不作为业务截止.
    struct Elapsed {
        using duration = std::chrono::nanoseconds;
        using rep = duration::rep;
        using period = duration::period;
        using time_point = std::chrono::time_point<Elapsed>;
        static constexpr bool is_steady = true;
        // 内核读取失败或计数越界时抛异常, 不用零值伪装成功.
        static time_point now();
    };

    using ElapsedTime = Elapsed::time_point;

    // 一次有效观测的目标时间, 不直接覆盖已公开的业务时钟.
    struct Estimate {
        // sampled 时刻对应的非负 Unix 纳秒估计.
        Time time{};
        // 同进程 BOOTTIME 接收时刻, 用于拒绝旧观测和计算年龄.
        ElapsedTime sampled{};
        // 已包含上游, 网络和量化误差, 不包含本机尚未吸收的偏差.
        std::uint64_t uncertainty_ns{};
        // 最近选中样本的往返延迟, 本机物理参考采样为零.
        std::uint64_t rtt_ns{};
    };

    // 从时钟读数生成有限期限的失败分类, 不与网络 Status 混用.
    enum class Error {
        // 来源未就绪或质量不足, 不能受理新的有限期限.
        clock_unready,
        // TTL 为负或调用方提供了非法时间读数.
        invalid_time,
        // 相加超出固定纳秒坐标的可表示范围.
        exhausted
    };

    struct Reading {
        // 连续单调不减的 Unix 时间, 失联后仍继续前进.
        Time time{};
        // 上游估计, 年龄漂移及未消化偏差之和, 不是硬实时保证.
        std::uint64_t uncertainty_ns{};
        // 最近样本的往返时长, 仅用于诊断.
        std::uint64_t rtt_ns{};
        // 最后有效观测时刻, 用于区分重连后的新鲜样本.
        ElapsedTime sampled{};
        // 来源可信, 样本不超过 5 s 且总误差不超过 500 ms 才接受新有限期限.
        bool ready{};
        // ready 且 ttl 非负, 加法不溢出时返回一次性绝对截止. 错误不可隐式转成 Store 的无限期空值.
        std::expected<Time, Error> deadline_after(std::chrono::nanoseconds ttl) const noexcept;
    };

    // Star 默认 1000 ppm, 为跟踪 Pulsar 的 500 ppm 调速保留余量; 有效范围 1..1000.
    explicit Clock(std::uint32_t slew_ppm = 1000);
    // 未初始化/设施故障返回空; 失联只降低 ready, 不清除锚点.
    std::optional<Clock::Reading> now() const;
    // 测试/确定性驱动入口, local 必须顺序提交, 反序视为计时设施故障.
    std::optional<Clock::Reading> now(ElapsedTime local) const;
    // 校验并外推至消费时刻, 替换残余偏差; 无效/过期/重复观测不改变原模型.
    bool publish(const Clock::Estimate& estimate);
    bool publish(const Clock::Estimate& estimate, ElapsedTime local);
    // 撤销新租约资格, 保留已有走时; 后续有效观测可恢复资格.
    void revoke();

private:
    // 以下操作都由 mutex_ 保护, 算术耗尽/本地反序故障保持至进程重启.
    bool advance(ElapsedTime local) const;
    std::optional<Clock::Reading> read(ElapsedTime local) const;
    bool observe(const Clock::Estimate& estimate, ElapsedTime local);
    // 默认入口在取得锁之后采样, 避免并发读取制造伪反序.
    mutable std::mutex mutex_;
    // 最大额外调速, 不代表底层振荡器准确度.
    const std::uint32_t slew_ppm_;
    // 空表示从未初始化公共纪元锚点.
    std::optional<Clock::Estimate> estimate_;
    // 与 epoch_ 成对的本地经过时间锚点.
    mutable ElapsedTime local_{};
    // 已公开时间, observe 不跳跃替换.
    mutable Time epoch_{};
    // 尚未吸收的有符号偏差, 正值加速, 负值减速.
    mutable std::int64_t debt_{};
    // ppm 除法余数, 防止频繁读取吃掉预算, 不积累闲置额度.
    mutable std::uint64_t fraction_{};
    // 来源资格与是否存在时间锚点分离.
    bool trusted_{};
    // 本地计数反序/耗尽无法由网络样本修复.
    mutable bool failed_{};
};

using ElapsedTime = Clock::ElapsedTime;

// 返回固定纳秒单位的本地采样计数.
std::int64_t elapsed_ns(ElapsedTime local) noexcept;
// 测量实际 BOOTTIME 的精度/读钟成本. 内核定时器限制 200 ms, stop 可取消.
// 失败抛异常, 只在专用采样线程执行, 不阻塞 Runtime 控制循环.
std::uint64_t elapsed_precision_ns(std::stop_token stop = {});
} // namespace astra
