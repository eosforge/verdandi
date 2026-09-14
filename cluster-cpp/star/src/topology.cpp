// 功能: 实现 Star 名单安装, 连接接纳, 成员替换和重连调度, 锁内只操作角色状态.
#include "topology.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <ranges>
#include <vector>

namespace verdandi::cluster {
StarTopology::StarTopology(const Config& config) : config_(config) {}

Result<void> StarTopology::initialize(const Member& local, std::span<const Member> members) {
    if (members.empty() || members.size() > config_.max_members) {
        return std::unexpected(Error{ErrorCode::capacity, "Invalid complete Star list size"});
    }
    // 在临时容器完成整份名单校验, 任何失败都不会留下部分成员索引, 验证成功后再加锁安装.
    std::map<Principal, Entry> prepared;
    std::optional<Id> previous;
    bool found = false;
    for (const auto& member : members) {
        if (!validate_member(member) || member.cluster != config_.cluster || member.role != Role::star || (previous && *previous >= member.id)) {
            return std::unexpected(Error{ErrorCode::identity, "Invalid complete Star list"});
        }
        previous = member.id;
        if (member.principal == local.principal) {
            if (member != local) {
                return std::unexpected(Error{ErrorCode::identity, "Complete list conflicts with local identity"});
            }
            found = true;
        } else {
            prepared.emplace(member.principal, Entry{.member = member});
        }
    }
    if (!found) {
        return std::unexpected(Error{ErrorCode::identity, "Complete list omits local identity"});
    }

    // 用临时连续数组排序检查重复项, 避免再为去重索引逐项分配树节点. 性能收益需独立测量.
    auto principals = members | std::views::transform(&Member::principal) | std::ranges::to<std::vector>();
    std::ranges::sort(principals);
    if (std::ranges::adjacent_find(principals) != principals.end()) {
        return std::unexpected(Error{ErrorCode::identity, "Duplicate principal in complete Star list"});
    }

    auto addresses = members | std::views::transform([](const Member& m) { return m.address.text(); }) | std::ranges::to<std::vector>();
    std::ranges::sort(addresses);
    if (std::ranges::adjacent_find(addresses) != addresses.end()) {
        return std::unexpected(Error{ErrorCode::identity, "Duplicate address in complete Star list"});
    }
    std::lock_guard lock(mutex_);
    if (local_) {
        return std::unexpected(Error{ErrorCode::conflict, "Star topology already initialized"});
    }
    members_ = std::move(prepared);
    local_ = local;
    return {};
}

Result<std::vector<SessionGeneration>> StarTopology::accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                            const std::optional<Member>& expected) {
    std::lock_guard lock(mutex_);
    if (!local_ || remote.cluster != local_->cluster || remote.id == local_->id || remote.principal == local_->principal || remote.address == local_->address ||
        (remote.role == Role::planet && direction != Direction::inbound)) {
        return std::unexpected(Error{ErrorCode::identity, "Invalid star relationship"});
    }
    if (expected && (remote != *expected || direction != Direction::outbound)) {
        return std::unexpected(Error{ErrorCode::identity, "Dial target identity changed"});
    }
    for (const auto& [principal, entry] : members_) {
        if (principal != remote.principal && (entry.member.id == remote.id || entry.member.address == remote.address)) {
            return std::unexpected(Error{ErrorCode::conflict, "Member aliases an existing identity"});
        }
    }
    // 新身份先检查按角色容量, 已知身份先检查代次; 仅合法替换才重置会话槽并返回旧代次取消列表.
    auto existing = members_.find(remote.principal);
    std::vector<SessionGeneration> cancelled;
    if (existing == members_.end()) {
        const auto count = std::count_if(members_.begin(), members_.end(), [&](const auto& pair) { return pair.second.member.role == remote.role; });
        const auto maximum = config_.max_members - (remote.role == Role::star ? 1U : 0U);
        if (static_cast<std::size_t>(count) >= maximum) {
            return std::unexpected(Error{ErrorCode::capacity, "Member capacity reached"});
        }
        existing = members_.emplace(remote.principal, Entry{.member = remote}).first;
    } else {
        auto replace = supersedes(remote, existing->second.member);
        if (!replace) {
            return std::unexpected(replace.error());
        }
        if (*replace) {
            for (auto session : existing->second.sessions) {
                if (session) {
                    cancelled.push_back(*session);
                }
            }
            existing->second = Entry{.member = remote};
        }
    }
    auto& entry = existing->second;
    // 每个方向只能安装一个代次, 旧会话的完成通过编号过滤, 不能覆盖或移除新会话.
    auto& slot = entry.sessions[static_cast<std::size_t>(direction)];
    if (slot) {
        return std::unexpected(Error{ErrorCode::conflict, "Star direction is already active"});
    }
    slot = generation;
    if (direction == Direction::outbound) {
        entry.pending = false;
        entry.connected = Clock::now();
    }
    return cancelled;
}

void StarTopology::closed(SessionGeneration generation, std::optional<ErrorCode>, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    for (auto& [principal, entry] : members_) {
        for (std::size_t index = 0; index < entry.sessions.size(); ++index) {
            if (entry.sessions[index] == generation) {
                entry.sessions[index].reset();
                if (index == static_cast<std::size_t>(Direction::outbound)) {
                    entry.failures = now - entry.connected >= config_.stable_connection ? 1 : std::min(entry.failures + 1, 100U);
                    entry.next = now + retry_delay(entry.failures, config_, principal.bytes[0] + std::hash<Id>{}(local_->id));
                }
                return;
            }
        }
    }
}

std::optional<Member> StarTopology::due(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (local_) {
        for (auto& [principal, entry] : members_) {
            if (entry.member.role == Role::star && !entry.pending && !entry.sessions[0] && now >= entry.next) {
                // 先准备拥有的副本再标记 pending, 复制失败不会遗留未发起的拨号.
                auto target = entry.member;
                entry.pending = true;
                return target;
            }
        }
    }
    return std::nullopt;
}

void StarTopology::failed(const Member& target, ErrorCode, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    const auto found = members_.find(target.principal);
    if (found != members_.end() && found->second.member == target) {
        auto& entry = found->second;
        entry.pending = false;
        entry.failures = std::min(entry.failures + 1, 100U);
        entry.next = now + retry_delay(entry.failures, config_, target.principal.bytes[0] + std::hash<Id>{}(local_->id));
    }
}

bool StarTopology::needs_refresh(Clock::time_point) {
    return false;
}

NetworkStatus StarTopology::status() const {
    std::lock_guard lock(mutex_);
    NetworkStatus result;
    result.initialized = local_.has_value();
    result.members = local_ ? 1 : 0;
    for (const auto& [principal, entry] : members_) {
        if (entry.member.role == Role::star) {
            ++result.members;
            result.outbound += entry.sessions[0].has_value();
            result.inbound += entry.sessions[1].has_value();
        } else {
            result.planet_inbound += entry.sessions[1].has_value();
        }
    }
    return result;
}

std::unique_ptr<Policy> make_star(const Config& config) {
    return std::make_unique<StarTopology>(config);
}
} // namespace verdandi::cluster
