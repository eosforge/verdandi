// 功能: 实现角色共用的成员代次替换判断和有界重连退避, 不持有网络资源.
// 该文件提供与会话策略相关的独立纯函数计算，如判断一个成员节点的候选是否优于当前节点（处理重启代次），以及如何进行指数退避。
#include <astra/policy.hpp>

#include <algorithm>

namespace astra {
// supersedes: 判断一个新候选成员状态是否能取代（覆盖）现有的成员状态。
// 参数 candidate: 收到的新成员信息。
// 参数 current: 系统当前记录的该成员信息。
// 返回值: Result<bool>。如果候选完全合法并能替换则返回 true；如果是同一代次但不需要替换则返回 false；如果是冲突或非法旧代次则返回 Error。
Result<bool> supersedes(const Member& candidate, const Member& current) {
    // 固定部署绑定不可变化, 同代次要求完整值一致; 只有严格升代才允许替换会话身份.
    // 确保主体的核心身份信息（主体标识，角色，监听地址，集群ID）没有发生改变。
    // 如果发生改变，或是新代次 (epoch) 小于当前代次，或者两者 epoch 相同但其他属性不同，均视为冲突。
    if (candidate.principal != current.principal || candidate.role != current.role || candidate.address != current.address ||
        candidate.galaxy != current.galaxy || candidate.epoch < current.epoch || (candidate.epoch == current.epoch && candidate != current)) {
        return Error::conflict("Conflicting or stale member incarnation");
    }
    // 只有在代次严格大于时才认定可以覆盖。
    return candidate.epoch > current.epoch;
}

// retry_delay: 根据当前连续失败次数、系统配置的界限以及随机盐值，计算下一次重试应该延迟的时间（加入抖动）。
// 参数 failures: 连续失败次数。
// 参数 config: 系统配置，包含重试的最小(reconnect_min)和最大(reconnect_max)延迟设定。
// 参数 salt: 随机盐值（通常基于节点ID等信息哈希得出），用于把抖动平滑并分散，避免羊群效应。
// 返回值: 需要延迟的毫秒数 (Milliseconds)。
Milliseconds retry_delay(std::uint32_t failures, const Config& config, std::uint64_t salt) {
    // 先截断指数再限幅, 抖动区间也限制在配置上下界, 避免持续失败时所有连接固定同一重试周期.
    // shift 计算 2 的指数幂次数，上限为 16 以防止溢出。
    const auto shift = std::min(failures == 0 ? 0 : failures - 1, 16U);
    // base 为基础退避时间，等于 reconnect_min 乘以 2 的 shift 次方，但不超过 reconnect_max。
    const auto base = std::min(config.reconnect_min * (1U << shift), config.reconnect_max);
    // lower 设定抖动区间的下界，即 base 减去其四分之一（不得低于配置下限）。
    const auto lower = std::max(base - base / 4, config.reconnect_min);
    // upper 设定抖动区间的上界，即 base 加上其四分之一（不得高于配置上限）。
    const auto upper = std::min(base + base / 4, config.reconnect_max);
    // 返回 lower 加上基于盐值和失败次数的抖动偏移量。
    return lower + Milliseconds((salt + failures) % static_cast<std::uint64_t>((upper - lower).count() + 1));
}
} // namespace astra
