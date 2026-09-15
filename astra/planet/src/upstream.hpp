// 功能: 定义 Planet 的有界候选集及单上游策略, 保存失败退避, 隔离和刷新状态.
#pragma once
#include <astra/policy.hpp>

#include <inplace_vector>
#include <mutex>

namespace astra {
// Planet 保存最多八个授权候选, 只有已完成 Hello 的一个上游能够对外显示为 active.
class PlanetUpstream final : public Policy {
public:
    // 复制 config 的分组偏好与退避设置, 初始无候选或活动上游, 不联网.
    explicit PlanetUpstream(const Config& config);
    // 借用已验签 local 和最多 8 个 Star 候选, 校验顺序与唯一性后安装独立副本.
    // 刷新保留已观察的较新代次和同代次退避, 活动上游或在途拨号存在时不替换; 错误保留当前状态.
    Result<void> initialize(const Member& local, std::span<const Member> members) override;
    // 仅接纳 expected 对应的 pending 出站 Star, remote 允许同绑定下合法升代, generation 记录为唯一活动上游.
    // 成功返回空取消列表, 未授权或重复上游返回 identity, 代次冲突沿用 supersedes 错误.
    Result<std::vector<SessionGeneration>> accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                  const std::optional<Member>& expected) override;
    // 仅处理当前活动 generation, 以 now 判断稳定期并按 error 退避或隔离, 旧会话完成不会关闭新上游.
    void closed(SessionGeneration generation, std::optional<ErrorCode> error, Clock::time_point now) override;
    // 无活动或在途上游时, 按 now, 同组偏好和稳定散列顺序选择至多一个候选并标记 pending.
    std::optional<Member> due(Clock::time_point now) override;
    // 仅更新仍与 target 完整匹配的候选, 使用 error 决定隔离, now 决定下次重试截止.
    void failed(const Member& target, ErrorCode error, Clock::time_point now) override;
    // 无活动上游, 候选轮次耗尽且 now 到期时消费刷新请求, 同时推进下一次刷新截止.
    bool needs_refresh(Clock::time_point now) override;
    // 锁内返回候选总数和活动上游的独立快照, 被隔离候选仍计入候选总数.
    NetworkStatus status() const override;

private:
    struct Candidate {
        Member member;
        bool pending{};
        bool attempted{};
        bool quarantined{};
        std::uint32_t failures{};
        Clock::time_point next{};
        Clock::time_point connected{};
    };
    // 调用时已持有 mutex_, 只更新候选状态, 不执行取消或联网.
    // candidate 必须属于当前索引且 local_ 已安装; identity/protocol/conflict 持续隔离, 其他失败只更新退避.
    void record_failure(Candidate& candidate, ErrorCode error, Clock::time_point now);
    Config config_;
    mutable std::mutex mutex_;
    std::optional<Member> local_;
    std::inplace_vector<Candidate, 8> candidates_;
    std::optional<std::pair<Principal, SessionGeneration>> active_;
    bool refresh_{};
    Clock::time_point next_refresh_{};
};
// 从已校验 config 创建未初始化的 Planet 策略, Runtime 接管独占所有权, 不发起拨号.
std::unique_ptr<Policy> make_planet(const Config& config);
} // namespace astra
