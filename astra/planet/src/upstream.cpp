// 功能: 实现 Planet 的候选节点校验逻辑、择优拨号调度以及故障切换机制。
// 在存在健康可靠的活动上游时，该策略会努力保持当前网络连接的稳定。
#include "upstream.hpp"

#include <algorithm>
#include <functional>
#include <tuple>

namespace astra {
namespace {
// candidate_rank 函数: 仅用于计算同组候选节点在本进程内部的排序权重，并不参与实际的网络签名或身份授权。
// 使用完整的进程 ID 作为盐（salt）进行哈希运算，这是为了避免集群中所有的 Planet 节点计算出相同的候选排序，
// 从而导致大家集中连接（或者说“总选”）同一个候选节点作为首选，实现连接负载均衡。
// 参数:
// - principal: 候选节点的主体标识（公钥信息等）。
// - process: 当前进程的唯一 ID。
// 返回值: 作为一个 std::uint64_t 类型的无符号 64 位整数返回，用于排序。
std::uint64_t candidate_rank(const Principal& principal, const Id& process) {
    // 初始常量值采用 FNV-1a 哈希算法的 64-bit 偏移量基准。
    std::uint64_t value = 14695981039346656037ULL;
    // 遍历当前进程 ID 的每一个字节进行哈希计算。
    for (char byte : process) {
        value = (value ^ static_cast<unsigned char>(byte)) * 1099511628211ULL;
    }
    // 遍历候选节点主体标识的每一个字节，融合进最终的哈希值中。
    for (auto byte : principal.bytes) {
        value = (value ^ byte) * 1099511628211ULL;
    }
    return value;
}
} // namespace

// PlanetUpstream 构造函数实现: 初始化时保存传入的网络配置参数。
PlanetUpstream::PlanetUpstream(const Config& config) : config_(config) {}

// initialize 实现: 对传入的候选上游列表进行深度验证与初始化。
Result<void> PlanetUpstream::initialize(const Member& local, std::span<const Member> members) {
    // 检查候选列表的规模是否超出了策略允许的最大容量（通常是 8 个）。
    if (members.size() > candidates_.capacity()) {
        return Status::capacity("Too many Planet candidates");
    }

    // 使用预先准备好的内嵌向量 prepared 来暂存新候选，保证如果中间发生错误，不会影响当前正在使用的 candidates_。
    std::inplace_vector<Candidate, 8> prepared;
    // 用于验证传入 members 的有序性，保存上一个验证过的节点的特征。
    std::optional<std::pair<bool, Id>> previous;

    // 遍历并验证每一个候选成员。
    for (const auto& member : members) {
        // 构建节点的排序键值，首先看是否与自己属于不同的组（不同组优先，或者同组优先等偏好），然后是节点 Id。
        const auto order = std::pair{member.group != config_.group, member.id};

        // 验证候选成员:
        // 1. member.validate() 是否为结构合法的成员。
        // 2. 候选的角色必须是 Star 节点（Planet 只能连 Star）。
        // 3. 集群标识 cluster 必须和本地配置一致。
        // 4. 候选成员的 principal、id、address 均不能与本地 local 节点相同（不能自己连自己）。
        // 5. 必须按照预先定义的排序规则进行升序排列，(previous && *previous >= order) 确保了输入是有序的。
        // 6. 检查重复性: prepared 中不能已经存在拥有相同 principal, id 或 address 的候选节点。
        // 两侧地址都通过规范化校验，直接的值比较与原来的规范字符串比较等价。
        if (!member.validate() || member.role != Member::Role::star || member.galaxy != config_.galaxy || member.principal == local.principal ||
            member.id == local.id || member.address == local.address || (previous && *previous >= order) ||
            std::ranges::any_of(prepared, [&](const Candidate& candidate) {
                const auto& known = candidate.member;
                return known.principal == member.principal || known.id == member.id || known.address == member.address;
            })) {
            return Status::identity("Invalid Planet candidate list");
        }
        previous = order;
        // 验证通过，放入暂存向量。
        prepared.push_back(Candidate{.member = member});
    }

    // 准备工作完成，下面准备修改对象状态，需要加互斥锁保护。
    std::lock_guard lock(mutex_);

    // 如果之前已经初始化过 local_，并且与本次传入的 local 不同，说明身份发生了改变，这在刷新操作中是不被允许的。
    if (local_ && *local_ != local) {
        return Status::identity("Refresh changed local identity");
    }

    // 如果当前的刷新操作晚于了一次成功的连接握手（已经存在 active_ 健康上游），
    // 或者是当前存在处于 pending（拨号中）状态的候选节点，
    // 为了网络稳定性，主动放弃此次迁移和替换候选列表。
    if (active_ || std::ranges::any_of(candidates_, [](const Candidate& c) { return c.pending; })) {
        return {};
    }

    // 如果可以替换列表，遍历新的 prepared 列表与旧的 candidates_ 列表。
    // 目的是：刷新只替换经验证的新代次(epoch)；如果是同代次的成员，保留其在旧列表中的失败预算（failures）和隔离状态，
    // 防止通过反复刷新名单来恶意或者意外绕过重试退避机制。
    for (auto& candidate : prepared) {
        for (const auto& old : candidates_) {
            if (old.member.principal != candidate.member.principal) {
                continue; // 不是同一个主体，跳过
            }
            if (candidate.member.epoch < old.member.epoch) {
                // 如果传入的代次反而变小了（过期名单），则直接保留旧的节点状态。
                candidate = old;
            } else {
                // 调用 supersedes 检查新代次是否能够合法覆盖旧代次。
                auto replace = supersedes(candidate.member, old.member);
                if (!replace) {
                    // 如果检查出严重错误（如签名验证失败），直接中止初始化。
                    return std::unexpected(replace.error());
                }
                if (!*replace) {
                    // 如果判定不需要覆盖，也保留旧的状态。
                    candidate = old;
                }
            }
            // 每次刷新重置 attempted 为 false，以允许在下一轮拨号时重新尝试。
            candidate.attempted = false;
        }
    }

    // 替换为新的状态。
    candidates_ = std::move(prepared);
    local_ = local;
    return {};
}

// accept 实现: 接受一个远程节点的连接，只有在验证身份匹配后才会建立会话。
Result<std::vector<Generation>> PlanetUpstream::accept(const Member& remote, Policy::Direction direction, Generation generation,
                                                              const std::optional<Member>& expected) {
    std::lock_guard lock(mutex_);
    // Planet 的安全和角色约束检查:
    // 1. 本地信息必须已初始化。
    // 2. 当前不能已有活跃的上游（active_ 必须为空）。
    // 3. 连接方向必须是主动出站拨号（outbound）。
    // 4. expected 参数不能为 std::nullopt，即我们必须知道正在预期哪一个节点的连接回调。
    // 5. 远程角色必须是 Star，且集群一致。
    if (!local_ || active_ || direction != Policy::Direction::outbound || !expected || remote.role != Member::Role::star || remote.galaxy != local_->galaxy) {
        return Status::identity("Planet requires one authorized outbound Star");
    }

    // 在候选列表中寻找匹配预期并且处于 pending 状态的目标。
    for (auto& candidate : candidates_) {
        // 如果候选者与 expected 不符，或者当前没有在向它发起拨号，跳过。
        if (candidate.member != *expected || !candidate.pending) {
            continue;
        }
        // 验证远程传递过来的 remote 信息是否可以合法覆盖我们原先保存的 member 状态（可能发生合法的 epoch 代次提升）。
        auto replace = supersedes(remote, candidate.member);
        if (!replace) {
            return std::unexpected(replace.error());
        }

        // 接受连接，更新候选节点的状态。
        candidate.member = remote;
        candidate.pending = false;                         // 连接已完成，不再是 pending 状态。
        candidate.connected = Steady::now();                // 记录成功连接的时间戳。
        active_ = std::pair{remote.principal, generation}; // 设置为当前活跃上游，记录标识与代次。

        // 由于 Planet 采用单上游策略，且当前限制不允许并发活跃，所以不需要返回需要取消的旧会话，返回空列表。
        return std::vector<Generation>{};
    }
    // 未找到匹配的合法候选上游。
    return Status::identity("Candidate no longer authorized");
}

// record_failure 实现: 专门用于处理和记录失败的私有函数。
void PlanetUpstream::record_failure(Candidate& candidate, Status::Code error, Steady::time_point now) {
    // 结束在途状态，并标记本轮已尝试过。
    candidate.pending = false;
    candidate.attempted = true;

    // 如果遇到的是身份认证失败、底层协议错误或配置绑定冲突等致命错误，
    // 则直接将其状态置为隔离 (quarantined = true)，不再作为普通网络错误进行退避重试。
    // 除非后续收到合法新成员代次替换了该候选。
    candidate.quarantined = candidate.quarantined || error == Status::Code::identity || error == Status::Code::protocol || error == Status::Code::conflict;

    // 累加连续失败次数，但最多限制在 100 次，避免整数溢出或产生无限大退避时间。
    candidate.failures = std::min(candidate.failures + 1, 100U);

    // 计算下一次允许拨号重试的时间。
    // 通过 principal.bytes[0] + 本地节点 id 的哈希作为额外抖动量，防止大量节点在同一瞬间发起重连，造成惊群效应。
    candidate.next = now + retry_delay(candidate.failures, config_, candidate.member.principal.bytes[0] + std::hash<Id>{}(local_->id));
}

// closed 实现: 响应连接断开事件。
void PlanetUpstream::closed(Generation generation, std::optional<Status::Code> error, Steady::time_point now) {
    std::lock_guard lock(mutex_);
    // 如果当前并没有活跃的连接，或者当前活跃的代次与要关闭的代次不符，则说明可能是过期的关闭事件，直接忽略。
    if (!active_ || active_->second != generation) {
        return;
    }

    // 遍历寻找当前活跃上游对应的候选实体。
    for (auto& candidate : candidates_) {
        if (candidate.member.principal == active_->first) {
            // 如果这个连接的持续时间超过了配置中定义的 stable_connection 稳定期阈值，
            // 那么认为之前的失败预算可以清零，当前属于一个全新的稳定连接的首次断开。
            if (now - candidate.connected >= config_.stable_connection) {
                candidate.failures = 0;
            }
            // 记录这次断线导致的失败，如果是正常的底层断开，默认记作 transport (传输错误)。
            record_failure(candidate, error.value_or(Status::Code::transport), now);
            break;
        }
    }
    // 连接已关闭，清空 active_ 状态。
    active_.reset();
}

// due 实现: 定时调度触发器，用于挑选下一个要拨号的候选节点。
std::optional<Member> PlanetUpstream::due(Steady::time_point now) {
    std::lock_guard lock(mutex_);
    // 如果尚未初始化，或者已经存在活跃上游，或者已经有在途 (pending) 连接，就什么都不做，直接返回。
    if (!local_ || active_ || std::ranges::any_of(candidates_, [](const Candidate& c) { return c.pending; })) {
        return {};
    }

    // 每轮遍历中，每个候选最多只尝试拨号一次。
    // 当所有非隔离的候选节点都已经被尝试过了 (attempted == true 或者是 quarantined)，
    // 这意味着当前一轮的所有尝试均已耗尽。
    if (std::ranges::all_of(candidates_, [](const Candidate& c) { return c.attempted || c.quarantined; })) {
        // 标记需要向外请求刷新上游名单（如果 Pulsar 掉线，这一步可能收不到响应）。
        refresh_ = true;
        // 重置所有候选节点的 attempted 标志，从而开启新一轮的尝试循环。
        for (auto& candidate : candidates_) {
            candidate.attempted = false;
        }
    }

    // 定义过滤条件：没有被隔离、本轮未尝试过、并且达到了退避机制设定的下一次可重试时间 (next <= now)。
    const auto eligible = [now](const Candidate& c) { return !c.quarantined && !c.attempted && now >= c.next; };

    // 在所有候选中寻找“最优”的一个节点进行连接尝试。
    // 使用 std::ranges::min_element 根据 std::tuple 的字典序规则比较：
    // 第一优先级：!eligible(c)（false<true，所以可用的节点优先）。
    // 第二优先级：节点所属组与本地配置组的不同程度（尽量选择同组节点，如果配置偏好同组的话，这里 c.member.group != config_.group
    // 表示不同组的权重较高/被排在后面）。 第三优先级：通过 candidate_rank 计算出来的进程特定哈希值，实现负载均衡。 第四优先级：作为保底，使用节点的 id 排序。
    const auto next = std::ranges::min_element(candidates_, {}, [&](const Candidate& c) {
        return std::tuple{!eligible(c), c.member.group != config_.group, candidate_rank(c.member.principal, local_->id), c.member.id};
    });

    // 如果找到了满足条件的候选节点。
    if (next != candidates_.end() && eligible(*next)) {
        auto target = next->member;
        next->pending = true;   // 标记为正在途连接。
        next->attempted = true; // 标记本轮已经尝试过。
        return target;
    }
    return {};
}

// failed 实现: 用于从外部接收到某个目标的拨号失败事件回调。
void PlanetUpstream::failed(const Member& target, Status::Code error, Steady::time_point now) {
    std::lock_guard lock(mutex_);
    for (auto& candidate : candidates_) {
        // 如果找到了对应的目标节点实体，调用 record_failure 记录失败状态并退出。
        if (candidate.member == target) {
            record_failure(candidate, error, now);
            return;
        }
    }
}

// needs_refresh 实现: 询问系统是否需要外部触发刷新操作。
bool PlanetUpstream::needs_refresh(Steady::time_point now) {
    std::lock_guard lock(mutex_);
    // 如果尚未初始化，或者当前存在健康的活动上游，或者 due 中没有发出 refresh_ 请求，
    // 或者现在还没到下一次允许强制刷新的时间点 next_refresh_，则不执行刷新。
    if (!local_ || active_ || !refresh_ || now < next_refresh_) {
        return false;
    }
    // 消费掉刷新请求（将标志位重置）。
    refresh_ = false;
    // 推进下一次允许刷新的最小时间间隔，防止网络恶化时产生刷单风暴（flood）。
    next_refresh_ = now + config_.reconnect_max;
    return true;
}

// status 实现: 获取网络连接和候选情况的综合报告。
Policy::State PlanetUpstream::status() const {
    std::lock_guard lock(mutex_);
    Policy::State result;
    // 返回初始化状态和现有的候选节点总数。
    result.initialized = local_.has_value();
    result.candidates = candidates_.size();

    // 如果存在活跃连接，去候选列表中查出对应的完整成员信息。
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

// 工厂方法实现。
std::unique_ptr<Policy> make_planet(const Config& config) {
    return std::make_unique<PlanetUpstream>(config);
}
} // namespace astra
