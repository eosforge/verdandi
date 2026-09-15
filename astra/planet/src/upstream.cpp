// 功能: 实现 Planet 候选校验, 择优拨号和故障切换, 健康上游存在时保持连接稳定.
#include "upstream.hpp"

#include <algorithm>
#include <functional>
#include <tuple>

namespace astra {
namespace {
// 只用于同组候选的本进程排序, 不参与签名或授权. 完整 id 作为盐, 避免所有 Planet 总选名单首项.
std::uint64_t candidate_rank(const Principal& principal, const Id& process) {
    std::uint64_t value = 14695981039346656037ULL;
    for (char byte : process) {
        value = (value ^ static_cast<unsigned char>(byte)) * 1099511628211ULL;
    }
    for (auto byte : principal.bytes) {
        value = (value ^ byte) * 1099511628211ULL;
    }
    return value;
}
} // namespace

PlanetUpstream::PlanetUpstream(const Config& config) : config_(config) {}

Result<void> PlanetUpstream::initialize(const Member& local, std::span<const Member> members) {
    if (members.size() > candidates_.capacity()) {
        return std::unexpected(Error{ErrorCode::capacity, "Too many Planet candidates"});
    }
    std::inplace_vector<Candidate, 8> prepared;
    std::optional<std::pair<bool, Id>> previous;
    for (const auto& member : members) {
        const auto order = std::pair{member.group != config_.group, member.id};
        // 最多八个候选, 直接复用已准备的内嵌容器检查重复, 不额外创建三棵临时集合和地址编码副本.
        // 两侧地址都通过规范化校验, 值比较与原来的规范字符串比较等价.
        if (!validate_member(member) || member.role != Role::star || member.cluster != config_.cluster || member.principal == local.principal ||
            member.id == local.id || member.address == local.address || (previous && *previous >= order) ||
            std::ranges::any_of(prepared, [&](const Candidate& candidate) {
                const auto& known = candidate.member;
                return known.principal == member.principal || known.id == member.id || known.address == member.address;
            })) {
            return std::unexpected(Error{ErrorCode::identity, "Invalid Planet candidate list"});
        }
        previous = order;
        prepared.push_back(Candidate{.member = member});
    }
    std::lock_guard lock(mutex_);
    if (local_ && *local_ != local) {
        return std::unexpected(Error{ErrorCode::identity, "Refresh changed local identity"});
    }
    // 刷新晚于一次成功握手时, 保留当前健康上游及其观察到的代次, 不主动迁移.
    if (active_ || std::ranges::any_of(candidates_, [](const Candidate& c) { return c.pending; })) {
        return {};
    }
    // 刷新只替换经验证的新代次, 同代次保留原失败预算和隔离, 避免名单刷新绕过退避.
    for (auto& candidate : prepared) {
        for (const auto& old : candidates_) {
            if (old.member.principal != candidate.member.principal) {
                continue;
            }
            if (candidate.member.epoch < old.member.epoch) {
                candidate = old;
            } else {
                auto replace = supersedes(candidate.member, old.member);
                if (!replace) {
                    return std::unexpected(replace.error());
                }
                if (!*replace) {
                    candidate = old;
                }
            }
            candidate.attempted = false;
        }
    }
    candidates_ = std::move(prepared);
    local_ = local;
    return {};
}

Result<std::vector<SessionGeneration>> PlanetUpstream::accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                              const std::optional<Member>& expected) {
    std::lock_guard lock(mutex_);
    if (!local_ || active_ || direction != Direction::outbound || !expected || remote.role != Role::star || remote.cluster != local_->cluster) {
        return std::unexpected(Error{ErrorCode::identity, "Planet requires one authorized outbound Star"});
    }
    for (auto& candidate : candidates_) {
        if (candidate.member != *expected || !candidate.pending) {
            continue;
        }
        auto replace = supersedes(remote, candidate.member);
        if (!replace) {
            return std::unexpected(replace.error());
        }
        candidate.member = remote;
        candidate.pending = false;
        candidate.connected = Clock::now();
        active_ = std::pair{remote.principal, generation};
        return std::vector<SessionGeneration>{};
    }
    return std::unexpected(Error{ErrorCode::identity, "Candidate no longer authorized"});
}

void PlanetUpstream::record_failure(Candidate& candidate, ErrorCode error, Clock::time_point now) {
    // 身份, 协议或绑定冲突不当作普通断网反复尝试, 保持隔离直到合法新成员代次替换候选.
    candidate.pending = false;
    candidate.attempted = true;
    candidate.quarantined = candidate.quarantined || error == ErrorCode::identity || error == ErrorCode::protocol || error == ErrorCode::conflict;
    candidate.failures = std::min(candidate.failures + 1, 100U);
    candidate.next = now + retry_delay(candidate.failures, config_, candidate.member.principal.bytes[0] + std::hash<Id>{}(local_->id));
}

void PlanetUpstream::closed(SessionGeneration generation, std::optional<ErrorCode> error, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (!active_ || active_->second != generation) {
        return;
    }
    for (auto& candidate : candidates_) {
        if (candidate.member.principal == active_->first) {
            if (now - candidate.connected >= config_.stable_connection) {
                candidate.failures = 0;
            }
            record_failure(candidate, error.value_or(ErrorCode::transport), now);
            break;
        }
    }
    active_.reset();
}

std::optional<Member> PlanetUpstream::due(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (!local_ || active_ || std::ranges::any_of(candidates_, [](const Candidate& c) { return c.pending; })) {
        return {};
    }
    // 每轮每候选最多一次. Supervisor 离线时也开始下一轮, 不把刷新成功当作重试前提.
    if (std::ranges::all_of(candidates_, [](const Candidate& c) { return c.attempted || c.quarantined; })) {
        refresh_ = true;
        for (auto& candidate : candidates_) {
            candidate.attempted = false;
        }
    }
    // 已签名的新代次可能改变 group/id. 从当前可用候选选最优项, 不依赖旧名单的存储次序.
    const auto eligible = [now](const Candidate& c) { return !c.quarantined && !c.attempted && now >= c.next; };
    const auto next = std::ranges::min_element(candidates_, {}, [&](const Candidate& c) {
        return std::tuple{!eligible(c), c.member.group != config_.group, candidate_rank(c.member.principal, local_->id), c.member.id};
    });
    if (next != candidates_.end() && eligible(*next)) {
        auto target = next->member;
        next->pending = true;
        next->attempted = true;
        return target;
    }
    return {};
}

void PlanetUpstream::failed(const Member& target, ErrorCode error, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    for (auto& candidate : candidates_) {
        if (candidate.member == target) {
            record_failure(candidate, error, now);
            return;
        }
    }
}

bool PlanetUpstream::needs_refresh(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (!local_ || active_ || !refresh_ || now < next_refresh_) {
        return false;
    }
    refresh_ = false;
    next_refresh_ = now + config_.reconnect_max;
    return true;
}

NetworkStatus PlanetUpstream::status() const {
    std::lock_guard lock(mutex_);
    NetworkStatus result;
    result.initialized = local_.has_value();
    result.candidates = candidates_.size();
    if (active_) {
        for (const auto& candidate : candidates_) {
            if (candidate.member.principal == active_->first) {
                result.active_member = candidate.member;
                break;
            }
        }
    }
    return result;
}

std::unique_ptr<Policy> make_planet(const Config& config) {
    return std::make_unique<PlanetUpstream>(config);
}
} // namespace astra
