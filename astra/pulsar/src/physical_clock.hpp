#pragma once
#include <astra/clock.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <thread>

namespace astra {
// 将受系统对时服务约束的物理参考转换成连续的公共纪元时钟.
class Source {
public:
    // 只读 CLOCK_REALTIME 和 adjtimex, 不改系统时钟或管理对时守护进程.
    // 未同步,误差超限或内核查询失败返回空, BOOTTIME 设施失败抛异常.
    static std::optional<Clock::Estimate> sample();

    // provider 在唯一工作线程调用, 必须有界返回. 默认为只读内核采样, 测试可注入故障/重启观测.
    using Provider = std::function<std::optional<Clock::Estimate>()>;
    // 接管 provider 并启动唯一采样线程, 空回调抛 invalid_argument; 外部依赖须活到线程结束.
    explicit Source(Provider provider = sample);
    // 停止并 join, 所有借用本对象的 RPC 必须先退出.
    ~Source();
    // 时钟与采样线程拥有固定对象地址, 不允许复制线程责任.
    Source(const Source&) = delete;
    // 禁止覆盖仍被采样线程或 RPC 借用的时钟对象.
    Source& operator=(const Source&) = delete;
    // 返回连续时间与质量; 未初始化/计时失败返回空, 不执行系统对时查询.
    std::optional<Clock::Reading> now() const;
    // 真实 BOOTTIME rho, 初始零表示尚未完成标定; acquire 与线程发布配对.
    std::uint64_t precision() const noexcept;

private:
    // 失败降低资格并于下一秒重试; 校正锚点不因参考源暂时失效而丢失.
    void run(std::stop_token stop) noexcept;
    // 只在采样线程使用, 不在每个 gRPC 请求里启动外部命令或执行读盘.
    Provider provider_;
    // Pulsar 最大额外调速 500 ppm, Star 保留更大的追赶余量.
    Clock clock_{500};
    // 仅标定线程发布, RPC 只读.
    std::atomic_uint64_t precision_{};
    // 可取消的一秒采样等待, 析构不必等待整个周期.
    std::mutex wait_mutex_;
    // 只作 stop_token 可中断等待, 不承载业务事件计数.
    std::condition_variable_any condition_;
    // 最后构造/最先停止, 不能越过 provider_/clock_ 的寿命.
    std::jthread worker_;
};
} // namespace astra
