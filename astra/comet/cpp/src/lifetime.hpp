#pragma once
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace comet::detail {
// 本地租约确认预算, 只保存最近成功起点, 不存服务端纪元或续租历史.
class Lifetime {
public:
    using Time = std::int64_t;                 // 本进程 CLOCK_BOOTTIME 毫秒, 非负, 包含系统 suspend.
    static std::optional<Time> now() noexcept; // 系统读时失败返回空, 不伪造时间或退到墙钟.

    explicit Lifetime(std::chrono::milliseconds ttl) : ttl_(ttl >= std::chrono::seconds(1) && ttl <= std::chrono::minutes(10) ? ttl.count() : throw std::invalid_argument("Invalid lease TTL")), period_(ttl_ / 3) {} // 私有边界同样拒绝非法配置.

    void spread(std::size_t seed) noexcept {
        period_ = ttl_ / 3 - ttl_ / 30 + static_cast<Time>(seed % static_cast<std::size_t>(2 * (ttl_ / 30) + 1));
    } // 每 UUID 固定约 +/-10% 周期抖动, 不用于随机身份.

    // 首次发送时间是确认依据, 重试/响应到达不得重新获得完整 TTL.
    bool confirm(Time sent) noexcept {
        if (sent < 0 || sent > std::numeric_limits<Time>::max() - ttl_) {
            return false;
        }
        if (!sent_ || sent > *sent_) {
            sent_ = sent;
        }
        return true;
    }

    bool ready(std::optional<Time> now) const noexcept {
        return now && sent_ && *now >= *sent_ && *now - *sent_ < ttl_;
    } // 负向异常读时同样不假装租约已确认.

    bool due(Time now) const noexcept {
        return !sent_ || now < *sent_ || now - *sent_ >= period_;
    } // 错过多次周期仍只调度当前一次, 不逐拍补发.

    std::chrono::milliseconds delay(Time now) const noexcept {
        return due(now) ? std::chrono::milliseconds::zero() : std::chrono::milliseconds(period_ - (now - *sent_));
    } // 只决定下一次唤醒, 不代表续租已经成功.

    void reset() noexcept {
        sent_.reset();
    } // 新 UUID 必须重新建立确认, 旧预算不可跨身份沿用.

private:
    const Time ttl_;           // 固定请求 TTL, 单位毫秒.
    Time period_;              // 约 TTL/3, 1 秒租约也以数百毫秒调度.
    std::optional<Time> sent_; // 最近完整成功的首次发送起点, 默认没有确认.
};
} // namespace comet::detail
