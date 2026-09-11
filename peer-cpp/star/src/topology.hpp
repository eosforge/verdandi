// 许可证: MIT, 详见仓库根目录 LICENSE.
#pragma once
#include <verdandi/peer/policy.hpp>

#include <array>
#include <map>
#include <mutex>

namespace verdandi::peer {
// Star 独占全部已知成员和两个方向的会话索引. 锁只覆盖内存修改, 所有取消在调用方执行.
class StarTopology final : public Policy {
public:
    explicit StarTopology(const Config& config);
    Result<void> initialize(const Member& local, std::span<const Member> members) override;
    Result<std::vector<SessionGeneration>> accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                  const std::optional<DialTarget>& expected) override;
    void closed(SessionGeneration generation, std::optional<ErrorCode> error, Clock::time_point now) override;
    std::vector<DialTarget> due(Clock::time_point now, std::size_t budget) override;
    void failed(const DialTarget& target, ErrorCode error, Clock::time_point now) override;
    bool needs_refresh(Clock::time_point now) override;
    NetworkStatus status() const override;

private:
    struct Entry {
        Member member;
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
std::unique_ptr<Policy> make_star(const Config& config);
} // namespace verdandi::peer
