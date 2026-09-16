// 功能: 实现 Star 核心拓扑相关的名单安装、网络连接的验证接纳、代次成员安全替换以及出站重连调度逻辑。
// 注意：所有的接口在执行时，锁内（mutex_）仅仅操作本策略的角色内存状态，不能在锁内持有并释放外部 I/O 资源阻塞线程。
#include "topology.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <ranges>
#include <vector>

namespace astra {
// StarTopology 构造函数实现: 将传入的 config 参数保存起来，用于后续的连接与容量校验逻辑。
StarTopology::StarTopology(const Config& config) : config_(config) {}

// initialize 实现: 完成完整的拓扑列表初始化。
Result<void> StarTopology::initialize(const Member& local, std::span<const Member> members) {
    // 首先进行预检，名单不能为空，并且总数量不能超过系统配置定义的最大成员承载限制。
    if (members.empty() || members.size() > config_.max_members) {
        return Error::capacity("Invalid complete Star list size");
    }
    
    // 使用临时 std::map 容器 prepared 进行验证安装准备。
    // 这样做可以保证“要么全部成功应用，要么失败不改变任何内部状态（强异常安全保证）”。
    // 任何中间抛错或校验失败，都不会遗留下部分损坏的成员索引数据。
    std::map<Principal, Entry> prepared;
    std::optional<Id> previous;
    bool found = false; // 用于追踪我们自身 (local) 是否在传入的拓扑名单中。
    
    // 第一次遍历：检查节点的规范性、角色以及名单的有序性。
    for (const auto& member : members) {
        // 节点合法性判定:
        // 1. member.validate() 是否为结构合法的成员。
        // 2. 所有名单上的节点 cluster 集群标识必须一致。
        // 3. 所有名单上的节点 role 必须全是 Star（Planet 不会出现在拓扑基础名单中，它们是单向上连进来的）。
        // 4. 输入的节点 id 必须单调递增，确保整体有序 (previous && *previous >= member.id)。
        if (!member.validate() || member.galaxy != config_.galaxy || member.role != Role::star || (previous && *previous >= member.id)) {
            return Error::identity("Invalid complete Star list");
        }
        previous = member.id;
        
        // 检查本地身份是否在名单中
        if (member.principal == local.principal) {
            // 虽然 principal 一样，但是里面的具体数据（如地址，代次等）和传给我的 local 不匹配，属于冲突。
            if (member != local) {
                return Error::identity("Complete list conflicts with local identity");
            }
            found = true; // 确认自身身份在名单中。
        } else {
            // 不是自己，加入到临时待安装的 map 中（以 principal 为 key）。
            prepared.emplace(member.principal, Entry{.member = member});
        }
    }
    
    // 如果整个名单遍历完都没发现自己，返回错误。
    if (!found) {
        return Error::identity("Complete list omits local identity");
    }

    // 为了进一步验证没有隐藏的恶意重放或者重复攻击（例如不同 ID 但是 principal 重复），
    // 构造临时的连续数组，提取所有的 principal 并在排序后利用 adjacent_find 查重。
    // 采用线性数组排序查重，避免了为去重去频繁构建临时的树结构或哈希表。
    auto principals = members | std::views::transform(&Member::principal) | std::ranges::to<std::vector>();
    std::ranges::sort(principals);
    if (std::ranges::adjacent_find(principals) != principals.end()) {
        return Error::identity("Duplicate principal in complete Star list");
    }

    // 同样的手法提取出所有的地址 address（通过 address.text()）进行排序查重。
    // 确保整个集群内，没有不同的节点试图绑定到同一个公网地址和端口。
    auto addresses = members | std::views::transform([](const Member& m) { return m.address.text(); }) | std::ranges::to<std::vector>();
    std::ranges::sort(addresses);
    if (std::ranges::adjacent_find(addresses) != addresses.end()) {
        return Error::identity("Duplicate address in complete Star list");
    }
    
    // 所有繁重的、可能失败的校验操作都完成了，现在进入加锁安装阶段。
    std::lock_guard lock(mutex_);
    
    // 如果 local_ 已经有值，说明策略对象已经被初始化过一次了。当前设计不支持在这上面二次初始化覆盖。
    if (local_) {
        return Error::conflict("Star topology already initialized");
    }
    
    // 使用 std::move 完成无拷贝转移赋值。
    members_ = std::move(prepared);
    local_ = local;
    return {};
}

// accept 实现: 接纳网络连接，验证合法性，并将有效会话登记在册。
Result<std::vector<SessionGeneration>> StarTopology::accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                            const std::optional<Member>& expected) {
    std::lock_guard lock(mutex_);
    
    // 基本网络约束条件检验:
    // 1. 本地节点必须已经成功初始化。
    // 2. 来访者 cluster 必须在同一个集群内。
    // 3. 不能是自己连接自己（id，principal，address 均不得与 local_ 相同）。
    // 4. 如果对方是 Planet，它只能充当客户端发起连接，因此只接受 inbound 的请求。
    if (!local_ || remote.galaxy != local_->galaxy || remote.id == local_->id || remote.principal == local_->principal || remote.address == local_->address ||
        (remote.role == Role::planet && direction != Direction::inbound)) {
        return Error::identity("Invalid star relationship");
    }
    
    // 如果是主动发起出站拨号（outbound，带有 expected 参数），必须确认返回连接成功的目标，
    // 其身份完全匹配预期的目标节点（防止中间人欺骗或地址重用导致的意外响应）。
    if (expected && (remote != *expected || direction != Direction::outbound)) {
        return Error::identity("Dial target identity changed");
    }
    
    // 别名检查：遍历现有的已知成员。如果有个连接声称是 A，但使用的却是已登记属于 B 的 id 或是 B 的 address。
    // 即：principal 不一样，但 id 或 address 却“冒名顶替”，这种冲突是被绝对禁止的。
    for (const auto& [principal, entry] : members_) {
        if (principal != remote.principal && (entry.member.id == remote.id || entry.member.address == remote.address)) {
            return Error::conflict("Member aliases an existing identity");
        }
    }
    
    auto existing = members_.find(remote.principal);
    std::vector<SessionGeneration> cancelled; // 收集由于连接替代等原因导致需要主动取消的旧会话代次。
    
    // 如果是一个陌生的 principal。
    if (existing == members_.end()) {
        // 先检查按角色类型是否会超容：例如只能有固定的 Star 节点数，以及特定数量的挂载 Planet 节点。
        const auto count = std::count_if(members_.begin(), members_.end(), [&](const auto& pair) { return pair.second.member.role == remote.role; });
        // max_members 计算时扣除掉自己本身占用的一份额度（仅针对 Star 计算容量）。
        const auto maximum = config_.max_members - (remote.role == Role::star ? 1U : 0U);
        if (static_cast<std::size_t>(count) >= maximum) {
            return Error::capacity("Member capacity reached");
        }
        // 允许通过，创建并插入新的节点记录。
        existing = members_.emplace(remote.principal, Entry{.member = remote}).first;
    } else {
        // 如果是已知的 principal，则进行代次（epoch）校验，看是不是发生了节点合法升级。
        auto replace = supersedes(remote, existing->second.member);
        if (!replace) {
            return std::unexpected(replace.error()); // 严重错误拒绝
        }
        if (*replace) {
            // 返回 true 说明旧成员过时，需要被新代次取代。
            // 此时必须把旧节点上所有方向的活跃会话标记并丢入 cancelled 列表中等待断开清理。
            for (auto session : existing->second.sessions) {
                if (session) {
                    cancelled.push_back(*session);
                }
            }
            // 更新该 principal 对应的节点数据为最新代次的信息。
            existing->second = Entry{.member = remote};
        }
    }
    
    auto& entry = existing->second;
    // 注意这里非常核心的逻辑：每个方向（出站或入站）只能有一个活动的代次。
    // 旧的会话即便由于超时等由于收到网络延迟滞后的包而返回完成（关闭）事件，也会因为传递了旧的 generation 编号被过滤掉，
    // 不会误杀或者覆盖已经安装成功的新会话。
    auto& slot = entry.sessions[static_cast<std::size_t>(direction)];
    if (slot) {
        // 如果该方向上已经有一个仍然合法的活动连接，阻止重复连接。
        return Error::conflict("Star direction is already active");
    }
    
    // 真正地将会话代次安装到槽位中。
    slot = generation;
    
    // 如果这属于一次出站拨号（是我们主动发起的连接成功了）。
    if (direction == Direction::outbound) {
        entry.pending = false;               // 清除拨号状态标志。
        entry.connected = Clock::now();      // 更新连通时间。
    }
    return cancelled; // 返回通知外层清理过期会话的任务列表。
}

// closed 实现: 安全响应底层传输断开的事件。
void StarTopology::closed(SessionGeneration generation, std::optional<Error::Code>, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    for (auto& [principal, entry] : members_) {
        for (std::size_t index = 0; index < entry.sessions.size(); ++index) {
            // 定位到发生断开的特定连接会话。
            if (entry.sessions[index] == generation) {
                // 清空该槽位。
                entry.sessions[index].reset();
                // 如果断开的是出站方向，我们需要承担重新拨号的责任，所以要计算退避策略。
                if (index == static_cast<std::size_t>(Direction::outbound)) {
                    // 判断断开前该连接是否达到了稳定期标准。
                    // 达到了则失败记为 1 (从头开始重新计算退避)；如果没有达到稳定期，则失败次数累加 (上限 100 避溢出)。
                    entry.failures = now - entry.connected >= config_.stable_connection ? 1 : std::min(entry.failures + 1, 100U);
                    // 根据哈希盐与失败次数推算下一次允许尝试拨号的阈值时间。
                    entry.next = now + retry_delay(entry.failures, config_, principal.bytes[0] + std::hash<Id>{}(local_->id));
                }
                return;
            }
        }
    }
}

// due 实现: 负责筛选调度并产生拨号任务目标。
std::optional<Member> StarTopology::due(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (local_) {
        for (auto& [principal, entry] : members_) {
            // 过滤条件:
            // 1. 目标节点是 Star。
            // 2. 当前没有在向它拨号 (非 pending)。
            // 3. 它对应的出站槽位 sessions[0] 为空（说明当前和它没有合法的出站连接）。
            // 4. 重试退避计时器已经到期 (now >= entry.next)。
            if (entry.member.role == Role::star && !entry.pending && !entry.sessions[0] && now >= entry.next) {
                // 处理顺序非常关键：必须先拷贝目标节点信息准备返回。
                auto target = entry.member;
                // 然后再同步将其打上 pending 标记。
                // 这样能确保就算后续外层进行网络 I/O 过程中发生异常失败，我们这里也不会遗留错误的未发起状态（造成无限重复发起）。
                entry.pending = true;
                return target;
            }
        }
    }
    return std::nullopt; // 当前没有符合要求的拨号目标。
}

// failed 实现: 供网络层在尝试拨号立即遭遇失败时（如 TCP 拒接），通知回退。
void StarTopology::failed(const Member& target, Error::Code, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    const auto found = members_.find(target.principal);
    // 找到对应记录，且记录与当前报错的 target 快照相匹配。
    if (found != members_.end() && found->second.member == target) {
        auto& entry = found->second;
        entry.pending = false; // 解除它的锁定状态。
        // 加算退避次数并延迟。与由于网络中途断开导致的退避一视同仁。
        entry.failures = std::min(entry.failures + 1, 100U);
        entry.next = now + retry_delay(entry.failures, config_, target.principal.bytes[0] + std::hash<Id>{}(local_->id));
    }
}

// needs_refresh 实现。
bool StarTopology::needs_refresh(Clock::time_point) {
    // Star 拓扑不需要通过定时周期行为刷新整体节点清单。
    return false;
}

// status 实现: 用于暴露节点内部连接质量状态。
NetworkStatus StarTopology::status() const {
    std::lock_guard lock(mutex_);
    NetworkStatus result;
    result.initialized = local_.has_value();
    result.members = local_ ? 1 : 0; // 如果初始化了自己，那么本身也算是拓扑成员 1。
    
    // 聚合遍历统计。
    for (const auto& [principal, entry] : members_) {
        if (entry.member.role == Role::star) {
            ++result.members; // Star 节点计入总规模。
            result.outbound += entry.sessions[0].has_value(); // 统计成功建立的出站链接数。
            result.inbound += entry.sessions[1].has_value();  // 统计成功建立的入站链接数。
        } else {
            // 针对连入的 Planet 节点单独统计入站数。
            result.planet_inbound += entry.sessions[1].has_value();
        }
    }
    return result;
}

// 工厂方法实现。
std::unique_ptr<Policy> make_star(const Config& config) {
    return std::make_unique<StarTopology>(config);
}
} // namespace astra
