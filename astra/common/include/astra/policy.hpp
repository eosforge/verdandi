// 功能: 定义角色策略的连接生命周期契约, 诊断快照及成员替换和重试规则.
#pragma once

#include <astra/config.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace astra {

// 诊断来自当前索引, 不累计有损事件. initialized 不表示业务数据已经同步.
struct NetworkStatus {
    // 本地角色身份及初始名单已安装, 不代表 Registry/Catalog 等业务数据已同步.
    bool initialized{};
    // Star 已知 Star 成员数, 包含自身; Planet 不使用此计数.
    std::size_t members{};
    // Star 已完成逻辑安装的 Star 入站会话数, 不包含握手中的 RPC.
    std::size_t inbound{};
    // Star 已安装的 Star 出站会话数; Planet 用 upstream 表示上游状态.
    std::size_t outbound{};
    // Star 已安装的 Planet 入站会话数, 与 Star 入站数分开统计.
    std::size_t planet_inbound{};
    // Planet 当前候选数量, 包括退避或隔离中的候选, 不等于健康连接数.
    std::size_t candidates{};
    // Planet 当前活动上游的独立成员快照, 无活动上游时为空.
    std::optional<Member> active_member;
};

// 仅两个真实角色共用的生命周期接口. 实现拥有自身索引, 不执行 gRPC 或在锁内调用外部代码.
class Policy {
public:
    // 允许通过基类指针销毁具体策略; 调用方应先停止会话推进, 析构不负责取消 RPC.
    virtual ~Policy() = default;
    // 全名单先验证再一次安装. refresh 仅用于 Planet, 失败保留已有候选.
    // local 已通过准入验证, members 在调用期间借用; 成功复制所需值, 身份, 容量或重复安装失败返回相应错误.
    virtual Result<void> initialize(const Member& local, std::span<const Member> members) = 0;
    // 返回应取消的旧会话编号, 由调用者在锁外完成取消, 不把 transport 类型传入角色层.
    // remote 已验签, direction 相对本进程, generation 必须唯一, expected 为原出站目标或入站空值.
    // 验证失败返回错误且不安装该会话; 成功后调用方负责保留会话到完成并执行返回的旧代次取消.
    virtual Result<std::vector<SessionGeneration>> accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                          const std::optional<Member>& expected) = 0;
    // 精确删除同一会话代次, 旧完成事件不能删除新条目. 失败影响后续退避和 Planet 隔离.
    // 仅对已安装且最终完成的会话调用; error 为空时按传输断开处理, now 必须来自单调时钟.
    virtual void closed(SessionGeneration generation, std::optional<ErrorCode> error, Clock::time_point now) = 0;
    // 控制循环调用, 取得至多一个到期成员快照, 同时登记 pending, 防止重复发起.
    // now 为单调时间, 无可用目标返回 nullopt; 每个返回成员都必须最终经过 accept 或 failed.
    virtual std::optional<Member> due(Clock::time_point now) = 0;
    // 建流尚未取得逻辑身份就失败时, 也必须释放 pending 并安排有限退避.
    // target 是 due 返回的原始快照, error 控制角色的失败处理, now 计算下次截止; 过期目标不能清除新目标状态.
    virtual void failed(const Member& target, ErrorCode error, Clock::time_point now) = 0;
    // 仅候选耗尽的 Planet 返回 true. 健康连接期间不触发名单刷新.
    // now 为单调时间; true 会消费刷新请求并推进下一次允许刷新的期限, 此函数不是无副作用查询.
    virtual bool needs_refresh(Clock::time_point now) = 0;
    // 取得一致的独立计数快照, 不向调用者返回内部容器引用.
    virtual NetworkStatus status() const = 0;
};
using PolicyFactory = std::unique_ptr<Policy> (*)(const Config&);

// 成员替换的共享规则: 同部署不能改变角色或端点, 同 epoch 必须逐字段相等.
// candidate/current 均须预先校验; 更新代次返回 true, 完全相同返回 false, 绑定变化, 降代或同代不同值返回 conflict.
Result<bool> supersedes(const Member& candidate, const Member& current);
// 失败次数增长有上限, 达到最大退避仍保留抖动, 不使用时间作为安全随机源.
// failures 为连续失败次数, 0 与首次失败使用相同基础退避; config 要求 0 < min <= max.
// salt 是稳定的去同步扰动, 返回值处于 reconnect_min..reconnect_max, 不用于任何安全决策.
Milliseconds retry_delay(std::uint32_t failures, const Config& config, std::uint64_t salt);

} // namespace astra
