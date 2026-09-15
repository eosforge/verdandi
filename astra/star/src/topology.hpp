// 功能: 定义 Star 的成员索引及双向会话策略, 按角色限制容量并隔离旧代次完成事件.
#pragma once
#include <astra/policy.hpp>

#include <array>
#include <map>
#include <mutex>

namespace astra {
// Star 独占全部已知成员和两个方向的会话索引. 锁只覆盖内存修改, 所有取消在调用方执行.
class StarTopology final : public Policy {
public:
    // 复制 config 的容量和退避设置, 初始无本地身份, 不发起网络连接.
    explicit StarTopology(const Config& config);
    // 借用已验签 local 和完整 Star 名单, 验证顺序, 唯一性及自身存在后一次安装.
    // 容量错误返回 capacity, 名单错误返回 identity, 重复初始化返回 conflict, 失败保留旧状态.
    Result<void> initialize(const Member& local, std::span<const Member> members) override;
    // 按 Policy 契约接纳已验签 remote, 出站必须精确匹配 expected, Planet 只允许入站.
    // generation 安装到对应方向槽位; 合法新成员代次返回待取消的旧会话, 身份, 冲突或容量错误直接返回.
    Result<std::vector<SessionGeneration>> accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                  const std::optional<Member>& expected) override;
    // 仅移除匹配 generation 的槽位; 出站断开依据 now 和稳定阈值更新退避, Star 不按 error 隔离成员.
    void closed(SessionGeneration generation, std::optional<ErrorCode> error, Clock::time_point now) override;
    // 在 now 到期且未占用的 Star 出站槽中选一个成员快照, 同步置 pending 防止重复拨号.
    std::optional<Member> due(Clock::time_point now) override;
    // 仅当 target 仍是当前完整成员值时释放 pending 并以 now 计算退避, Star 不使用 error 区分隔离.
    void failed(const Member& target, ErrorCode error, Clock::time_point now) override;
    // Star 初始完整名单安装后不通过此接口刷新, 忽略 now 并始终返回 false.
    bool needs_refresh(Clock::time_point now) override;
    // 锁内读取成员和已安装会话数, 返回独立快照, 不包含握手中的 RPC.
    NetworkStatus status() const override;

private:
    struct Entry {
        Member member;
        // 槽位 0 为 outbound, 槽位 1 为 inbound, 与 Direction 的枚举顺序一致.
        std::array<std::optional<SessionGeneration>, 2> sessions{};
        bool pending{};
        std::uint32_t failures{};
        Clock::time_point next{};
        Clock::time_point connected{};
    };
    Config config_;
    mutable std::mutex mutex_;
    std::optional<Member> local_;
    // 首版以有序表保持确定的拨号遍历, 限制为部署容量, 不在心跳路径复制此表.
    std::map<Principal, Entry> members_;
};
// 从已校验 config 创建未初始化的 Star 策略, 所有权交给 Runtime, 不持有配置借用.
std::unique_ptr<Policy> make_star(const Config& config);
} // namespace astra
