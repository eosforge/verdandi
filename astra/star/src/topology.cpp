// 注意: 所有的接口在执行时, 锁内(mutex_)仅仅操作本策略的角色内存状态, 不能在锁内持有并释放外部 I/O 资源阻塞线程.
#include "topology.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <ranges>
#include <type_traits>
#include <vector>

namespace astra {
// Star 构造函数实现: 将传入的 config 参数保存起来, 用于后续的连接与容量校验逻辑.
Star::Star(const Config& config) : config_(config) {}

Result<std::vector<Generation>> Star::refresh(const Member& local, std::span<const Member> members) {

    // candidate 复用已有完整名单校验, 不触发网络或修改当前会话.
    Star candidate(config_);
    if (auto initialized = candidate.initialize(local, members); !initialized) {
        return std::unexpected(initialized.error());
    }
    const std::lock_guard lock(mutex_); // 名单与已验签握手在同一身份边界合并.
    if (!local_ || *local_ != local) {
        return Status::identity("Directory replaced local process");
    }
    auto prepared = members_;          // 控制面低频复制保留会话/退避, 不是热路径消息复制.
    std::vector<Generation> cancelled; // 所有可失败分配均先于最终 swap.
    for (auto& [principal, incoming] : candidate.members_) {
        const auto old = prepared.find(principal);
        if (old == prepared.end()) {
            prepared.emplace(principal, std::move(incoming));
            continue;
        }
        if (incoming.member.epoch < old->second.member.epoch) {
            continue;
        }
        const auto replace = supersedes(incoming.member, old->second.member);
        if (!replace) {
            return std::unexpected(replace.error());
        }
        if (*replace) {
            for (const auto generation : old->second.sessions) {
                if (generation) {
                    cancelled.push_back(*generation);
                }
            }
            old->second = std::move(incoming);
        }
    }

    // 合并后的缺项旧成员仍会占用身份/地址, 不能只检查当前返回列表中的别名.
    std::map<Id, Principal> ids;
    std::map<Endpoint, Principal> addresses;
    std::size_t stars = 1; // 包含本进程, Planet 继续占独立角色额度.
    for (const auto& [principal, entry] : prepared) {
        if (entry.member.role == Member::Role::star) {
            ++stars;
        }
        if (entry.member.id == local.id || entry.member.address == local.address || !ids.emplace(entry.member.id, principal).second || !addresses.emplace(entry.member.address, principal).second) {
            return Status::identity("Merged directory aliases existing member");
        }
    }
    if (stars > config_.max_members) {
        return Status::capacity("Merged Star directory exceeds capacity");
    }
    members_.swap(prepared);
    return cancelled;
}

// initialize 实现: 完成完整的拓扑列表初始化.
Result<void> Star::initialize(const Member& local, std::span<const Member> members) {

    // 首先进行预检, 名单不能为空, 并且总数量不能超过系统配置定义的最大成员承载限制.
    if (members.empty() || members.size() > config_.max_members) {
        return Status::capacity("Invalid complete Star list size");
    }

    // 使用临时 std::map 容器 prepared 进行验证安装准备.
    // 这样做可以保证"要么全部成功应用, 要么失败不改变任何内部状态(强异常安全保证)".
    // 任何中间抛错或校验失败, 都不会遗留下部分损坏的成员索引数据.
    std::map<Principal, Entry> prepared;
    // previous 借用前一成员的 ID, members 在本次调用内有效, 无需逐项复制字符串.
    std::optional<std::string_view> previous;
    bool found = false; // 用于追踪我们自身 (local) 是否在传入的拓扑名单中.

    // 第一次遍历: 检查节点的规范性, 角色以及名单的有序性.
    for (const auto& member : members) {
        // 节点合法性判定:
        // 1. member.validate() 是否为结构合法的成员.
        // 2. 所有名单上的节点 cluster 集群标识必须一致.
        // 3. 所有名单上的节点 role 必须全是 Star(Planet 不会出现在拓扑基础名单中, 它们是单向上连进来的).
        // 4. 输入的节点 id 必须单调递增, 确保整体有序 (previous && *previous >= member.id).
        if (!member.validate() || member.galaxy != config_.galaxy || member.role != Member::Role::star || (previous && *previous >= member.id)) {
            return Status::identity("Invalid complete Star list");
        }
        previous = member.id;

        // 检查本地身份是否在名单中
        if (member.principal == local.principal) {
            // 虽然 principal 一样, 但是里面的具体数据(如地址, 代次等)和传给我的 local 不匹配, 属于冲突.
            if (member != local) {
                return Status::identity("Complete list conflicts with local identity");
            }
            found = true; // 确认自身身份在名单中.
        } else {
            // 不是自己, 加入到临时待安装的 map 中(以 principal 为 key).
            if (!prepared.emplace(member.principal, Entry{.member = member}).second) {
                return Status::identity("Duplicate principal in complete Star list");
            }
        }
    }

    // 如果整个名单遍历完都没发现自己, 返回错误.
    if (!found) {
        return Status::identity("Complete list omits local identity");
    }

    // Principal 已在建表时查重; 本地身份若重复, 会先被完整值比较或严格 ID 顺序拒绝.
    // 提取地址 address(通过 address.text())进行排序查重.
    // 确保整个集群内, 没有不同的节点试图绑定到同一个公网地址和端口.
    auto addresses = members | std::views::transform([](const Member& m) { return m.address.text(); }) | std::ranges::to<std::vector>();
    std::ranges::sort(addresses);
    if (std::ranges::adjacent_find(addresses) != addresses.end()) {
        return Status::identity("Duplicate address in complete Star list");
    }

    // identity 预先拥有本地身份, 字符串分配失败发生在提交之前, 不留下只有名单的半初始化状态.
    std::optional<Member> identity{local};
    static_assert(std::is_nothrow_move_assignable_v<decltype(identity)>);

    // 所有繁重的, 可能失败的校验操作都完成了, 现在进入加锁安装阶段.
    std::lock_guard lock(mutex_);

    // 如果 local_ 已经有值, 说明策略对象已经被初始化过一次了.当前设计不支持在这上面二次初始化覆盖.
    if (local_) {
        return Status::conflict("Star topology already initialized");
    }

    // 两个已准备好的状态只执行无分配提交, 不在修改 members_ 后再复制 local 的字符串.
    members_.swap(prepared);
    local_ = std::move(identity);
    return {};
}

// accept 实现: 接纳网络连接, 验证合法性, 并将有效会话登记在册.
Result<std::vector<Generation>> Star::accept(const Member& remote, Policy::Direction direction, Generation generation, const std::optional<Member>& expected) {

    // lock 只保护角色内存状态, 返回后由 Runtime 处理实际网络操作.
    std::lock_guard lock(mutex_);

    // 基本网络约束条件检验:
    // 1. 本地节点必须已经成功初始化.
    // 2. 来访者 cluster 必须在同一个集群内.
    // 3. 不能是自己连接自己(id, principal, address 均不得与 local_ 相同).
    // 4. 对等入口只接受 Star 或既有 Planet; Polaris/Astrolabe 的有效准入也不能进入本策略.
    // 5. Planet 只能充当客户端发起连接, 因此只接受 inbound 的请求.
    if (!local_ || (remote.role != Member::Role::star && remote.role != Member::Role::planet) || remote.galaxy != local_->galaxy || remote.id == local_->id || remote.principal == local_->principal || remote.address == local_->address || (remote.role == Member::Role::planet && direction != Policy::Direction::inbound)) {
        return Status::identity("Invalid star relationship");
    }

    // 如果是主动发起出站拨号(outbound, 带有 expected 参数), 必须确认返回连接成功的目标,
    // 其身份完全匹配预期的目标节点(防止中间人欺骗或地址重用导致的意外响应).
    if (expected && (remote != *expected || direction != Policy::Direction::outbound)) {
        return Status::identity("Dial target identity changed");
    }

    // 别名检查: 遍历现有的已知成员.如果有个连接声称是 A, 但使用的却是已登记属于 B 的 id 或是 B 的 address.
    // 即: principal 不一样, 但 id 或 address 却"冒名顶替", 这种冲突是被绝对禁止的.
    for (const auto& [principal, entry] : members_) {
        if (principal != remote.principal && (entry.member.id == remote.id || entry.member.address == remote.address)) {
            return Status::conflict("Member aliases an existing identity");
        }
    }

    // existing 为按部署主体定位的成员迭代器, 在 mutex_ 内保持有效.
    auto existing = members_.find(remote.principal);
    std::vector<Generation> cancelled; // 收集由于连接替代等原因导致需要主动取消的旧会话代次.

    // 如果是一个陌生的 principal.
    if (existing == members_.end()) {
        // 先检查按角色类型是否会超容: 例如只能有固定的 Star 节点数, 以及特定数量的挂载 Planet 节点.
        const auto count = std::count_if(members_.begin(), members_.end(), [&](const auto& pair) { return pair.second.member.role == remote.role; });
        // max_members 计算时扣除掉自己本身占用的一份额度(仅针对 Star 计算容量).
        const auto maximum = config_.max_members - (remote.role == Member::Role::star ? 1U : 0U);
        if (static_cast<std::size_t>(count) >= maximum) {
            return Status::capacity("Member capacity reached");
        }

        // 允许通过, 创建并插入新的节点记录.
        existing = members_.emplace(remote.principal, Entry{.member = remote}).first;
    } else {
        // 如果是已知的 principal, 则进行代次(epoch)校验, 看是不是发生了节点合法升级.
        auto replace = supersedes(remote, existing->second.member);
        if (!replace) {
            return std::unexpected(replace.error()); // 严重错误拒绝
        }
        if (*replace) {
            // 返回 true 说明旧成员过时, 需要被新代次取代.
            // 此时必须把旧节点上所有方向的活跃会话标记并丢入 cancelled 列表中等待断开清理.
            for (auto session : existing->second.sessions) {
                if (session) {
                    cancelled.push_back(*session);
                }
            }

            // 更新该 principal 对应的节点数据为最新代次的信息.
            existing->second = Entry{.member = remote};
        }
    }

    // 已通过身份/部署检查才裁决重复流. 同方向不抢占; 相反方向统一按发起方 ID 排序.
    auto& entry = existing->second;
    const auto index = static_cast<std::size_t>(direction);
    auto& slot = entry.sessions[index];
    auto& opposite = entry.sessions[1 - index];
    if (slot) {
        return Status::conflict("Star session initiator is already active");
    }
    if (remote.role == Member::Role::star && opposite) {
        const auto& candidate = direction == Policy::Direction::outbound ? local_->id : remote.id;
        const auto& current = direction == Policy::Direction::outbound ? remote.id : local_->id;
        const bool preferred = std::lexicographical_compare(candidate.begin(), candidate.end(), current.begin(), current.end(), [](char left, char right) { return static_cast<unsigned char>(left) < static_cast<unsigned char>(right); });
        if (!preferred) {
            return Status::conflict("Opposite Star session has the preferred initiator");
        }
        cancelled.push_back(*opposite); // 先完成可能分配, 再无异常移交唯一活动槽.
        opposite.reset();
    }
    slot = generation;
    if (direction == Policy::Direction::outbound) {
        entry.pending = false;
    }
    entry.connected = Steady::now(); // 两种方向断开都可重拨, 不只给出站计稳定时间.
    return cancelled;                // 返回通知外层清理过期会话的任务列表.
}

// closed 实现: 安全响应底层传输断开的事件.
void Star::closed(Generation generation, std::optional<Status::Code>, Steady::time_point now) {

    // lock 只保护角色内存状态, 返回后由 Runtime 处理实际网络操作.
    std::lock_guard lock(mutex_);
    for (auto& [principal, entry] : members_) {
        for (std::size_t index = 0; index < entry.sessions.size(); ++index) {
            // 定位到发生断开的特定连接会话.
            if (entry.sessions[index] == generation) {
                // 清空该槽位.
                entry.sessions[index].reset();
                // 当前有效流不分方向都可触发本端重拨, 败选旧流的完成因 generation 不同不会走到这里.
                entry.failures = now - entry.connected >= config_.stable_connection ? 1 : std::min(entry.failures + 1, 100U);
                entry.next = now + retry_delay(entry.failures, config_, principal.bytes[0] + std::hash<Id>{}(local_->id));
                return;
            }
        }
    }
}

// due 实现: 负责筛选调度并产生拨号任务目标.
std::optional<Member> Star::due(Steady::time_point now) {

    // lock 只保护角色内存状态, 返回后由 Runtime 处理实际网络操作.
    std::lock_guard lock(mutex_);
    if (local_) {
        for (auto& [principal, entry] : members_) {
            // 过滤条件:
            // 1. 目标节点是 Star.
            // 2. 当前没有在向它拨号 (非 pending).
            // 3. 任一方向都没有已接纳流, 不为补齐反向槽位额外拨号.
            // 4. 重试退避计时器已经到期 (now >= entry.next).
            if (entry.member.role == Member::Role::star && !entry.pending && !entry.sessions[0] && !entry.sessions[1] && now >= entry.next) {
                // 处理顺序非常关键: 必须先拷贝目标节点信息准备返回.
                auto target = entry.member;
                // 然后再同步将其打上 pending 标记.
                // 这样能确保就算后续外层进行网络 I/O 过程中发生异常失败, 我们这里也不会遗留错误的未发起状态(造成无限重复发起).
                entry.pending = true;
                return target;
            }
        }
    }
    return std::nullopt; // 当前没有符合要求的拨号目标.
}

// failed 实现: 供网络层在尝试拨号立即遭遇失败时(如 TCP 拒接), 通知回退.
void Star::failed(const Member& target, Status::Code, Steady::time_point now) {

    // lock 只保护角色内存状态, 返回后由 Runtime 处理实际网络操作.
    std::lock_guard lock(mutex_);
    // found 按主体定位拨号记录, 还需完整匹配 target 才能修改退避.
    const auto found = members_.find(target.principal);
    // 找到对应记录, 且记录与当前报错的 target 快照相匹配.
    if (found != members_.end() && found->second.member == target) {
        // entry 仍对应本次失败的目标快照, 只修改出站调度状态.
        auto& entry = found->second;
        entry.pending = false; // 解除它的锁定状态.
        if (entry.sessions[0] || entry.sessions[1]) {
            return; // 失败的重复拨号不能把已经接纳的健康流记成失联/不稳定.
        }
        // 加算退避次数并延迟.与由于网络中途断开导致的退避一视同仁.
        entry.failures = std::min(entry.failures + 1, 100U);
        entry.next = now + retry_delay(entry.failures, config_, target.principal.bytes[0] + std::hash<Id>{}(local_->id));
    }
}

// needs_refresh 实现.
bool Star::needs_refresh(Steady::time_point) {
    // Star 拓扑不需要通过定时周期行为刷新整体节点清单.
    return false;
}

// status 实现: 用于暴露节点内部连接质量状态.
Policy::State Star::status() const {

    // lock 只保护角色内存状态, 返回后由 Runtime 处理实际网络操作.
    std::lock_guard lock(mutex_);
    // result 初始各计数为零, 在持锁期间汇总并按值返回, 不借用成员表.
    Policy::State result;
    result.initialized = local_.has_value();
    result.members = local_ ? 1 : 0; // 如果初始化了自己, 那么本身也算是拓扑成员 1.

    // 聚合遍历统计.
    for (const auto& [principal, entry] : members_) {
        if (entry.member.role == Member::Role::star) {
            ++result.members;                                 // Star 节点计入总规模.
            result.outbound += entry.sessions[0].has_value(); // 统计成功建立的出站链接数.
            result.inbound += entry.sessions[1].has_value();  // 统计成功建立的入站链接数.
        } else {
            // 针对连入的 Planet 节点单独统计入站数.
            result.planet_inbound += entry.sessions[1].has_value();
        }
    }
    return result;
}

// 工厂方法实现.
std::unique_ptr<Policy> make_star(const Config& config) {
    return std::make_unique<Star>(config);
}
} // namespace astra
