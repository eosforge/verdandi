#include "physical_clock.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <sys/timex.h>
#include <time.h>

namespace astra {
// Source::sample 单次物理参考采样, 内核未同步或窗口超限返回空.
// 夹取 adjtimex 与墙钟, 误差累计内核估计与调度窗口, 超限即拒绝.
std::optional<Clock::Estimate> Source::sample() {

    // 夹住质量查询和墙钟采样, 将可能的调度延迟计入误差. BOOTTIME 暂停时间也参与年龄判断.
    const auto before = Clock::Elapsed::now();
    // state 为只读 adjtimex 查询缓冲, modes 为零, 不提交任何系统时钟修改.
    timex state{};
    // status 接收内核对时状态; 错误及未同步状态不能作为物理参考.
    const auto status = ::adjtimex(&state);
    // wall 接收绝对 Unix 秒与纳秒, 校验非负和乘加边界后才能使用.
    timespec wall{};
    if (status < 0 || status == TIME_ERROR || (state.status & (STA_UNSYNC | STA_CLOCKERR)) != 0 || state.maxerror < 0 || state.esterror < 0 || ::clock_gettime(CLOCK_REALTIME, &wall) != 0) {
        return std::nullopt;
    }

    // after 为夹取窗口终点, 与 before 同属包含系统挂起时间的 BOOTTIME.
    const auto after = Clock::Elapsed::now();
    // maximum 为有符号纳秒的可表示上界, 防止秒转纳秒溢出.
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (after < before || after - before > std::chrono::milliseconds(20) || wall.tv_sec < 0 || wall.tv_nsec < 0 || wall.tv_nsec >= 1'000'000'000 || wall.tv_sec > (maximum - wall.tv_nsec) / 1'000'000'000) {
        return std::nullopt;
    }

    // 内核误差字段固定为微秒; offset 由 STA_NANO 决定单位. 先限制范围再换算, 不溢出取绝对值.
    const auto error_us = std::max(state.maxerror, state.esterror);
    // offset_limit 按内核 STA_NANO 标志换算容许偏差, 与 state.offset 使用同一单位.
    const auto offset_limit = static_cast<long>(clock_uncertainty_limit_ns / ((state.status & STA_NANO) != 0 ? 1U : 1000U));
    if (error_us > static_cast<long>(clock_uncertainty_limit_ns / 1000) || state.offset < -offset_limit || state.offset > offset_limit) {
        return std::nullopt;
    }

    // offset 是已限制幅度的偏差绝对值, 转无符号前不可能触及有符号最小值.
    const auto offset = static_cast<std::uint64_t>(state.offset < 0 ? -state.offset : state.offset);
    // uncertainty 累计内核估计误差,残余偏差和完整采样窗口, 单位统一为 ns.
    const auto uncertainty = static_cast<std::uint64_t>(error_us) * 1000 + offset * ((state.status & STA_NANO) != 0 ? 1U : 1000U) + static_cast<std::uint64_t>((after - before).count());
    if (uncertainty > clock_uncertainty_limit_ns) {
        return std::nullopt;
    }

    // 采样墙钟位于 before..after; 全窗口已计入误差, 不假定系统调用在区间正中间完成.
    return Clock::Estimate{Clock::Time(std::chrono::nanoseconds(wall.tv_sec * 1'000'000'000 + wall.tv_nsec)), after, uncertainty, 0};
}

// Source 构造即启动采样线程, provider 为空直接抛异常.
// provider 为参考采样函数, 线程退出由析构负责.
Source::Source(Provider provider) : provider_(std::move(provider)) {

    if (!provider_) {
        throw std::invalid_argument("Physical clock requires a reference provider");
    }
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

// Source 析构请求线程停止并等待退出, 先停采样再释放成员.
// 不抛异常, 连接停止后成员才可安全销毁.
Source::~Source() {

    worker_.request_stop();
    if (worker_.joinable()) {
        worker_.join();
    }
}

// Source::now 返回当前时钟读数, 未校准返回空.
// 复用内部时钟的短锁推进, 不触发系统采样或等待下一次对时.
std::optional<Clock::Reading> Source::now() const {
    return clock_.now();
}

// Source::precision 返回已测本地分辨率 (纳秒), 对时应答用它表示采样误差边界.
// 不抛异常, 原子读取采样线程发布的结果.
std::uint64_t Source::precision() const noexcept {
    return precision_.load(std::memory_order_acquire);
}

// Source::run 采样线程主循环, 按固定周期采样并老化时钟, 停止即退出.
// stop 为停止令牌; 不抛异常. 采样失败撤销参考同步标记, 已校准时钟仍按 holdover 规则推进.
void Source::run(std::stop_token stop) noexcept {

    try {
        while (!stop.stop_requested()) {
            try {
                if (precision() == 0) {
                    precision_.store(elapsed_precision_ns(stop), std::memory_order_release);
                }
                if (stop.stop_requested()) {
                    break;
                }

                // sample 独立拥有本次参考观测, 空表示不可用, 不清除已建立的连续时间锚点.
                auto sample = provider_();
                if (!stop.stop_requested() && sample) {
                    // 加入本地读钟量化, provider 的时间精度不因纳秒编码而被夸大.
                    if (sample->uncertainty_ns > clock_uncertainty_limit_ns) {
                        sample.reset();
                    } else {
                        sample->uncertainty_ns += precision();
                    }
                }
                if (stop.stop_requested() || !sample || !clock_.publish(*sample)) {
                    clock_.revoke();
                }
            } catch (...) {
                clock_.revoke();
            }

            // lock 配合可取消等待, 析构请求停止后无需等完整采样周期.
            std::unique_lock lock(wait_mutex_);
            condition_.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
        }
    } catch (...) {
        // 等待设施故障也撤销资格, 不让后台异常 terminate 整个进程.
    }
    clock_.revoke();
}
} // namespace astra
