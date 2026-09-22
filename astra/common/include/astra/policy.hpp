// 本文件提供了统一的网络策略接口(Policy), 用于隔离底层网络行为与高层拓扑管理.
#pragma once

#include <astra/config.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace astra {

// 策略基类: 针对不同节点角色提供抽象统一的生命周期管理接口.
// 仅两个真实角色 (Star/Planet) 共用这个生命周期接口.
// 每个接口实现拥有自身的独立内部索引状态, 保证不执行任何 gRPC 调用或在持锁范围内回调外部不安全代码.
class Policy {
public:
    // 与双向会话数组的索引一致, outbound 为 0, inbound 为 1.
    enum class Direction {
        // 本进程主动发起的会话, 索引为 0, 也是值初始化的默认方向.
        outbound,
        // 本进程接受的远端会话, 索引为 1; Star 与相反方向共享唯一活动会话, Planet 仍仅用既有上游方向.
        inbound
    };

    // 诊断状态快照对象.
    // 诊断来自当前内部数据结构索引, 是某个时刻的快照, 不累计发生过的有损事件.
    // 注意: initialized 为 true 时仅代表底层网络节点配置加载完成, 不代表业务数据已经完全同步.
    struct State {
        // initialized: 本地角色身份及初始名单已成功安装.注意这不代表外部如 Registry/Catalog 等上层业务数据已经同步.
        bool initialized{};

        // members: Star 拓扑视角下已知的 Star 成员数, 这个数量包含当前节点自身;
        // 仅在 Star 角色中具备意义, Planet 角色不使用此计数.
        std::size_t members{};

        // inbound: Star 节点目前已完成逻辑层面安装的 Star 类入站(被动)会话数量,
        // 这个数字中不包含处于半连接或 TLS 握手中的未完成 RPC.
        std::size_t inbound{};

        // outbound: Star 节点已完成安装的 Star 类出站(主动)会话数量;
        // 对于 Planet 角色, 则复用该字段表示指向该 Planet 上游状态的活动连接数.
        std::size_t outbound{};

        // planet_inbound: 统计 Star 节点已安装的 Planet 节点类型入站会话数量,
        // 与一般的 Star 节点间的入站连接数(inbound)分开独立统计, 以利于隔离与限流.
        std::size_t planet_inbound{};

        // candidates: 当前 Planet 角色维持的上游候选数量,
        // 这个数其中包括了正处于网络退避或协议错误被隔离中的候选者, 它并不等于系统当前健康的可用连接数.
        std::size_t candidates{};

        // active_member: Planet 角色当前所依附的那个活动上游节点的独立成员信息快照,
        // 若当前没有任何活动的上游(全线断开或未接入), 则值为空(nullopt).
        std::optional<Member> active_member;
    };

    // 虚析构函数.允许通过基类指针安全地销毁具体的子类策略对象;
    // 调用方在销毁前应当先停止全部的会话推进任务, 析构函数职责仅在于释放自身内存, 并不负责实际取消运行中的 RPC 调用.
    virtual ~Policy() = default;

    // 节点拓扑信息初始化操作.
    // 全名单必须在此先进行一次整体验证然后再一次性安装.refresh 操作仅用于 Planet 角色, 如果刷新失败则应保留已有的有效候选列表不变.
    // local 变量必须已通过准入机制的初步验证, members 切片在调用期间被借用;
    // 成功的话会深拷贝内部所需的特定值; 如果发现身份认证错误, 系统容量超限或检测到重复安装冲突, 则返回相应的底层 Status 错误码.
    // 参数 local: 本节点的 Member 信息.
    // 参数 members: 其他相关成员的列表.
    virtual Result<void> initialize(const Member& local, std::span<const Member> members) = 0;

    // Star 的受信只读名单刷新, 保留已观察的更新身份及缺项成员; 返回须在锁外取消的旧会话.
    // 默认拒绝, 暂停开发的 Planet 继续使用既有候选流程, 不扩展为业务同步节点.
    virtual Result<std::vector<Generation>> refresh(const Member&, std::span<const Member>) {
        return Status::protocol("Role does not support directory refresh");
    }

    // 接受一个来自远端的新连接, 并准备安装相应的会话.
    // 返回需要由上层网络框架去主动断开(取消)的旧代次会话编号列表,
    // 由调用者收到结果后在无锁的安全环境下去完成真实的断开动作, 以此避免把底层具体的 transport 传输类型耦合传入纯逻辑的角色策略层.
    // 参数 remote: 已经通过签名验证的远端成员身份对象.
    // 参数 direction: 表示这条会话是针对本进程的主动(outbound)或是被动(inbound).
    // 参数 generation: 当前新会话的代次 ID, 要求在生命周期内必须唯一.
    // 参数 expected: 对于 outbound 是发起请求时的期望目标成员, 入站请求则传入 nullopt 空值.
    // 验证失败返回错误, 且不会安装该异常会话;
    // 成功后调用方需负责挂载保留此新会话的生命周期直到完成, 同时必须执行返回向量中要求取消的旧代次连接.
    virtual Result<std::vector<Generation>> accept(const Member& remote, Policy::Direction direction, Generation generation, const std::optional<Member>& expected) = 0;

    // 汇报并处理会话断开.
    // 用于精确查找并删除同一对应会话代次的记录, 以防过期的旧完成事件因网络乱序错误删除系统新建立的同源有效条目.
    // 这里的失败报告会影响系统后续的指数退避策略和 Planet 对于候选节点的隔离判断.
    // 该函数仅对成功被 accept 安装且生命周期最终走向结束的有效会话进行调用;
    // 参数 generation: 需要关闭会话的唯一代次编号.
    // 参数 error: 导致断开的错误信息枚举, 如果为空则按普通的底层传输断开处理.
    // 参数 now: 当前时刻, 必须取自严格递增的单调时钟(steady_clock).
    virtual void closed(Generation generation, std::optional<Status::Code> error, Steady::time_point now) = 0;

    // 从控制循环周期性调用, 用于驱动网络连接.
    // 用于取得至多一个已经到达网络重试退避期限或待发起的成员快照,
    // 取出时内部会同步登记处于 pending 等待中状态, 以防止针对该成员造成过量的重复拨号.
    // 参数 now: 当前单调时间戳.
    virtual std::optional<Member> due(Steady::time_point now) = 0;

    // 建流初期网络出错汇报.
    // 当建立底层连接尚未取得对方逻辑身份时就发生的底层失败事件, 同样必须由上层通知以释放内部该目标的 pending 标记, 并根据错误性质安排后续合理的有限退避延时.
    // 参数 target: 是原先从 due 函数返回的目标成员快照对象.
    // 参数 error: 导致拨号失败的原因码, 控制角色内部的具体错误降级或隔离处理方案.
    // 参数 now: 当前单调时间戳, 用以计算下一次允许重试的截止时刻; 过期的陈旧目标调用将不会清除现行活动新目标的状态.
    virtual void failed(const Member& target, Status::Code error, Steady::time_point now) = 0;

    // 检测是否需要执行拓扑候选人的信息刷新.
    // 目前设计上仅针对所有上游候选列表均不可用(耗尽)的 Planet 节点才会返回 true, 触发寻路刷新.在健康连接保持期间该函数不会触发多余的名单更新.
    // 参数 now: 当前的单调时间戳.
    // 返回值若为 true 代表将消费掉本次刷新请求令牌, 并自动推进规划下一次允许进行拉取刷新的最快时间限制, 所以此函数执行并不是一个无副作用的纯查询操作.
    virtual bool needs_refresh(Steady::time_point now) = 0;

    // 获取当前底层网络状态快照.
    // 返回取得具有一致性保障的独立技术指标与计数信息快照, 接口绝不向外界调用者返回或暴露内部共享容器的原始指针或引用.
    virtual Policy::State status() const = 0;
};

// 策略工厂指针类型定义.
// 根据输入的 Config 参数返回具体的策略实例对象(如 Star 或 PlanetUpstream).
using PolicyFactory = std::unique_ptr<Policy> (*)(const Config&);

// 共享的连接候选人替换逻辑规则检查.
// 规定: 来自同一次部署的连接实体不能自行改变身份角色(Member::Role)或者底层监听端点; 而如果存在同一代次
// epoch, 则必须保证新旧成员的每个关键字段的值完全相等无歧义. candidate 和 current 两参数均须在调用该函数前完成预先格式校验; 参数 candidate:
// 期望覆盖当前状态的新成员对象. 参数 current: 被对比的目前正处在系统维护中的现存成员对象.
// 如果出现身份绑定变更, 新记录代次落后于旧记录降代或同世代记录却存在字段值不同步的情况, 均抛出 conflict 并阻止替换.
Result<bool> supersedes(const Member& candidate, const Member& current);

// 计算带有抖动的退避重试延迟.
// 失败等待时间的增长有固定上限(在 config
// 中定义), 当达到最大退避边界时仍会保留随机抖动偏移以防大量重连拥堵, 计算过程并不使用系统当前时间作为不安全的弱随机源. 参数 failures:
// 当前连续发生的失败次数记录, 当其为 0 时也必须与首次失败采用相同的基础退避值起点; 参数 config: 包含关于延迟边界要求的配置, 逻辑上需确保内部满足 0 <
// reconnect_min <= reconnect_max. 参数 salt: 一个用于使不同节点保持错开稳定的去同步时间扰动值, 它的具体返回值一定分布处于配置的 reconnect_min..reconnect_max
// 这个区间内, 且严禁直接充当任何系统的安全级别随机决策依赖.
Milliseconds retry_delay(std::uint32_t failures, const Config& config, std::uint64_t salt);

} // namespace astra
