// 许可证: MIT, 详见仓库根目录 LICENSE.
#include <verdandi/peer/policy.hpp>

#include <algorithm>

namespace verdandi::peer {
Result<bool> supersedes(const Member& candidate, const Member& current) {
    if (candidate.principal != current.principal || candidate.role != current.role || candidate.address != current.address ||
        candidate.cluster != current.cluster || candidate.epoch < current.epoch || (candidate.epoch == current.epoch && candidate != current)) {
        return std::unexpected(Error{ErrorCode::conflict, "Conflicting or stale member incarnation"});
    }
    return candidate.epoch > current.epoch;
}

Milliseconds retry_delay(std::uint32_t failures, const Config& config, std::uint64_t salt) {
    const auto shift = std::min(failures == 0 ? 0 : failures - 1, 16U);
    const auto base = std::min(config.reconnect_min * (1U << shift), config.reconnect_max);
    const auto lower = std::max(base - base / 4, config.reconnect_min);
    const auto upper = std::min(base + base / 4, config.reconnect_max);
    return lower + Milliseconds((salt + failures) % static_cast<std::uint64_t>((upper - lower).count() + 1));
}
} // namespace verdandi::peer
