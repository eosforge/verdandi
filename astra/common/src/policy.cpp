// 功能: 实现角色共用的成员代次替换判断和有界重连退避, 不持有网络资源.
#include <astra/policy.hpp>

#include <algorithm>

namespace astra {
Result<bool> supersedes(const Member& candidate, const Member& current) {
    // 固定部署绑定不可变化, 同代次要求完整值一致; 只有严格升代才允许替换会话身份.
    if (candidate.principal != current.principal || candidate.role != current.role || candidate.address != current.address ||
        candidate.cluster != current.cluster || candidate.epoch < current.epoch || (candidate.epoch == current.epoch && candidate != current)) {
        return std::unexpected(Error{ErrorCode::conflict, "Conflicting or stale member incarnation"});
    }
    return candidate.epoch > current.epoch;
}

Milliseconds retry_delay(std::uint32_t failures, const Config& config, std::uint64_t salt) {
    // 先截断指数再限幅, 抖动区间也限制在配置上下界, 避免持续失败时所有连接固定同一重试周期.
    const auto shift = std::min(failures == 0 ? 0 : failures - 1, 16U);
    const auto base = std::min(config.reconnect_min * (1U << shift), config.reconnect_max);
    const auto lower = std::max(base - base / 4, config.reconnect_min);
    const auto upper = std::min(base + base / 4, config.reconnect_max);
    return lower + Milliseconds((salt + failures) % static_cast<std::uint64_t>((upper - lower).count() + 1));
}
} // namespace astra
