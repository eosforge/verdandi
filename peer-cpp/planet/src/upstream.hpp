// 许可证: MIT, 详见仓库根目录 LICENSE.
#pragma once
#include <verdandi/peer/policy.hpp>

#include <inplace_vector>
#include <mutex>

namespace verdandi::peer {
// Planet 保存最多八个授权候选, 只有已完成 Hello 的一个上游能够对外显示为 active.
class PlanetUpstream final : public Policy {
public:
    explicit PlanetUpstream(const Config& config);
    Result<void> initialize(const Member& local, std::span<const Member> members) override;
    Result<std::vector<SessionGeneration>> accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                  const std::optional<DialTarget>& expected) override;
    void closed(SessionGeneration generation, std::optional<ErrorCode> error, Clock::time_point now) override;
    std::vector<DialTarget> due(Clock::time_point now, std::size_t budget) override;
    void failed(const DialTarget& target, ErrorCode error, Clock::time_point now) override;
    bool needs_refresh(Clock::time_point now) override;
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
    void record_failure(Candidate& candidate, ErrorCode error, Clock::time_point now);
    Config config_;
    mutable std::mutex mutex_;
    std::optional<Member> local_;
    std::inplace_vector<Candidate, 8> candidates_;
    std::optional<std::pair<Principal, SessionGeneration>> active_;
    bool refresh_{};
    Clock::time_point next_refresh_{};
};
std::unique_ptr<Policy> make_planet(const Config& config);
} // namespace verdandi::peer
