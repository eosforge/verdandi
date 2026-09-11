// 许可证: MIT, 详见仓库根目录 LICENSE.
#include "topology.hpp"

#include <algorithm>
#include <set>

namespace verdandi::peer {
StarTopology::StarTopology(const Config& config) : config_(config) {}

Result<void> StarTopology::initialize(const Member& local, std::span<const Member> members) {
    if (members.empty() || members.size() > config_.max_peers) {
        return std::unexpected(Error{ErrorCode::capacity, "Invalid complete Star list size"});
    }
    std::map<Principal, Entry> prepared;
    std::set<std::string> addresses;
    std::set<Principal> principals;
    std::optional<PeerId> previous;
    bool found = false;
    for (const auto& member : members) {
        if (!validate_member(member) || member.cluster != config_.cluster || member.role != Role::star || (previous && *previous >= member.id) ||
            !principals.insert(member.principal).second || !addresses.insert(member.address.text()).second) {
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
    std::lock_guard lock(mutex_);
    if (local_) {
        return std::unexpected(Error{ErrorCode::conflict, "Star topology already initialized"});
    }
    members_ = std::move(prepared);
    local_ = local;
    return {};
}

Result<std::vector<SessionGeneration>> StarTopology::accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                            const std::optional<DialTarget>& expected) {
    std::lock_guard lock(mutex_);
    if (!local_ || remote.cluster != local_->cluster || remote.id == local_->id || remote.principal == local_->principal || remote.address == local_->address ||
        (remote.role == Role::planet && direction != Direction::inbound)) {
        return std::unexpected(Error{ErrorCode::identity, "Invalid peer relationship"});
    }
    if (expected && (remote != expected->member || direction != Direction::outbound)) {
        return std::unexpected(Error{ErrorCode::identity, "Dial target identity changed"});
    }
    for (const auto& [principal, entry] : members_) {
        if (principal != remote.principal && (entry.member.id == remote.id || entry.member.address == remote.address)) {
            return std::unexpected(Error{ErrorCode::conflict, "Member aliases an existing identity"});
        }
    }
    auto existing = members_.find(remote.principal);
    std::vector<SessionGeneration> cancelled;
    if (existing == members_.end()) {
        const auto count = std::count_if(members_.begin(), members_.end(), [&](const auto& pair) { return pair.second.member.role == remote.role; });
        const auto maximum = config_.max_peers - (remote.role == Role::star ? 1U : 0U);
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
    auto& slot = entry.sessions[static_cast<std::size_t>(direction)];
    if (slot) {
        return std::unexpected(Error{ErrorCode::conflict, "Peer direction is already active"});
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
                    entry.next = now + retry_delay(entry.failures, config_, principal.bytes[0] + local_->id.bytes[0]);
                }
                return;
            }
        }
    }
}

std::vector<DialTarget> StarTopology::due(Clock::time_point now, std::size_t budget) {
    std::lock_guard lock(mutex_);
    std::vector<DialTarget> result;
    if (!local_) {
        return result;
    }
    for (auto& [principal, entry] : members_) {
        if (result.size() == budget) {
            break;
        }
        if (entry.member.role == Role::star && !entry.pending && !entry.sessions[0] && now >= entry.next) {
            result.push_back({entry.member, true});
            entry.pending = true;
        }
    }
    return result;
}

void StarTopology::failed(const DialTarget& target, ErrorCode, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    const auto found = members_.find(target.member.principal);
    if (found != members_.end() && found->second.member == target.member) {
        auto& entry = found->second;
        entry.pending = false;
        entry.failures = std::min(entry.failures + 1, 100U);
        entry.next = now + retry_delay(entry.failures, config_, target.member.principal.bytes[0] + local_->id.bytes[0]);
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
} // namespace verdandi::peer
