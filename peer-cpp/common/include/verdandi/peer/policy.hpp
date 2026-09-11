// 许可证: MIT, 详见仓库根目录 LICENSE.
#pragma once

#include <verdandi/peer/config.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace verdandi::peer {

// 诊断来自当前索引, 不累计有损事件. initialized 不表示业务数据已经同步.
struct NetworkStatus {
    bool initialized{};
    std::size_t members{};
    std::size_t inbound{};
    std::size_t outbound{};
    std::size_t planet_inbound{};
    std::size_t candidates{};
    bool upstream{};
    std::optional<Member> active_member;
};

// 拨号请求携带已验证成员快照, 连接完成时必须再次核对它仍属于当前代次.
struct DialTarget {
    Member member;
    bool require_exact_id{};
};

// 仅两个真实角色共用的生命周期接口. 实现拥有自身索引, 不执行 gRPC 或在锁内调用外部代码.
class Policy {
public:
    virtual ~Policy() = default;
    // 全名单先验证再一次安装. refresh 仅用于 Planet, 失败保留已有候选.
    virtual Result<void> initialize(const Member& local, std::span<const Member> members) = 0;
    // 返回应取消的旧会话编号, 由调用者在锁外完成取消, 不把 transport 类型传入角色层.
    virtual Result<std::vector<SessionGeneration>> accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                          const std::optional<DialTarget>& expected) = 0;
    // 精确删除同一会话代次, 旧完成事件不能删除新条目. 失败影响后续退避和 Planet 隔离.
    virtual void closed(SessionGeneration generation, std::optional<ErrorCode> error, Clock::time_point now) = 0;
    // 控制循环调用, 取得本轮可启动的有限拨号, 同时登记 pending, 防止重复发起.
    virtual std::vector<DialTarget> due(Clock::time_point now, std::size_t budget) = 0;
    // 建流尚未取得逻辑身份就失败时, 也必须释放 pending 并安排有限退避.
    virtual void failed(const DialTarget& target, ErrorCode error, Clock::time_point now) = 0;
    // 仅候选耗尽的 Planet 返回 true. 健康连接期间不触发名单刷新.
    virtual bool needs_refresh(Clock::time_point now) = 0;
    // 取得一致的独立计数快照, 不向调用者返回内部容器引用.
    virtual NetworkStatus status() const = 0;
};
using PolicyFactory = std::unique_ptr<Policy> (*)(const Config&);

// 成员替换的共享规则: 同部署不能改变角色或端点, 同 epoch 必须逐字段相等.
Result<bool> supersedes(const Member& candidate, const Member& current);
// 失败次数增长有上限, 达到最大退避仍保留抖动, 不使用时间作为安全随机源.
Milliseconds retry_delay(std::uint32_t failures, const Config& config, std::uint64_t salt);

} // namespace verdandi::peer
