// 功能: 只读系统物理时间及校准质量, 在独立可取消线程中维护 Pulsar 公共时钟.
#pragma once
#include <astra/clock.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <thread>

namespace astra {
// 从系统对时服务管理的 CLOCK_REALTIME 获取观测. 只读 adjtimex, 不改钟或启动/安装守护程序.
// 未同步, 内核错误, 误差过大或读取失败时返回空/抛异常, 调用方降低时钟质量.
std::optional<ClockEstimate> system_time_sample();

class PhysicalClock {
public:
    // provider 在唯一工作线程调用, 必须有界返回. 默认为只读内核采样, 测试可注入故障/重启观测.
    using Provider = std::function<std::optional<ClockEstimate>()>;
    explicit PhysicalClock(Provider provider = system_time_sample);
    // 停止并 join, 所有借用本对象的 RPC 必须先退出.
    ~PhysicalClock();
    PhysicalClock(const PhysicalClock&) = delete;
    PhysicalClock& operator=(const PhysicalClock&) = delete;
    // 返回连续时间与质量; 未初始化/计时失败返回空, 不执行系统对时查询.
    std::optional<EpochReading> now() const;
    // 真实 BOOTTIME rho, 初始零表示尚未完成标定; acquire 与线程发布配对.
    std::uint64_t precision() const noexcept;

private:
    // 失败降低资格并于下一秒重试; 校正锚点不因参考源暂时失效而丢失.
    void run(std::stop_token stop) noexcept;
    // 只在采样线程使用, 不在每个 gRPC 请求里启动外部命令或执行读盘.
    Provider provider_;
    // Pulsar 最大额外调速 500 ppm, Star 保留更大的追赶余量.
    EpochClock clock_{500};
    // 仅标定线程发布, RPC 只读.
    std::atomic_uint64_t precision_{};
    // 可取消的一秒采样等待, 析构不必等待整个周期.
    std::mutex wait_mutex_;
    std::condition_variable_any condition_;
    // 最后构造/最先停止, 不能越过 provider_/clock_ 的寿命.
    std::jthread worker_;
};
} // namespace astra