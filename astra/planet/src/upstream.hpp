// 功能: 定义 Planet 节点的有界候选集以及单上游连接策略。
// 该策略负责保存上游节点的失败退避状态、隔离状态以及定时刷新状态。
#pragma once
#include <astra/policy.hpp>

#include <inplace_vector>
#include <mutex>

namespace astra {
// PlanetUpstream 类继承自 Policy 接口，作为 Planet 节点的特定连接策略。
// Planet 节点最多保存 8 个已授权的候选上游（Star 节点），并且在同一时刻，
// 只有完成 Hello 握手的一个上游节点能够对外显示为活跃状态（active）。
class PlanetUpstream final : public Policy {
public:
    // 构造函数: 复制传入的 config 对象中的分组偏好与退避设置。
    // 初始状态下没有候选上游，也没有活动的连接上游，且不主动发起联网请求。
    // 参数:
    // - config: 包含节点网络配置信息的配置对象（如退避重试参数、组别偏好等）。
    explicit PlanetUpstream(const Config& config);

    // 初始化策略: 借用已经完成验签的本地节点信息 local，以及最多 8 个 Star 节点的候选名单 members。
    // 在安装成为独立的候选副本之前，会校验列表的顺序并检查唯一性。
    // 如果是刷新操作，会保留已观察到的具有较新代次（epoch）的成员，以及同代次成员的退避状态。
    // 当存在活动上游或在途拨号时，不会直接替换。遇到错误则保留当前原有状态。
    // 参数:
    // - local: 本地节点的身份和地址信息。
    // - members: 传入的候选上游节点列表。
    // 返回值: Result<void>，成功返回空值，失败返回相应的 Error 错误信息。
    Result<void> initialize(const Member& local, std::span<const Member> members) override;

    // 接受连接请求: 仅接纳参数 expected 所对应的处于 pending 状态的出站（outbound）Star 节点连接。
    // 允许 remote（对端节点）在相同的绑定下进行合法的代次升级（epoch 更新）。
    // 成功连接后，将 generation 记录为当前唯一的活动上游。
    // 参数:
    // - remote: 发起/接受连接的对端节点身份信息。
    // - direction: 连接方向（通常为 outbound）。
    // - generation: 此次连接会话的唯一代次标识。
    // - expected: 预期的目标节点身份（如果存在）。
    // 返回值: 返回一个需要被取消的旧会话 generation 列表。成功则返回空列表，未授权或重复上游则返回 identity 错误，代次冲突则沿用 supersedes 检查的错误。
    Result<std::vector<SessionGeneration>> accept(const Member& remote, Direction direction, SessionGeneration generation,
                                                  const std::optional<Member>& expected) override;

    // 处理会话关闭事件: 仅处理当前处于活动状态的 generation。
    // 根据当前时间 now 判断连接是否达到稳定期，并根据传入的 error 进行退避延迟计算或直接隔离。
    // 旧的（已经替换的）会话完成事件不会导致当前新的活跃上游被关闭。
    // 参数:
    // - generation: 关闭的会话代次标识。
    // - error: 可选的错误码，用于指示断开的具体原因。
    // - now: 触发关闭时的当前系统时钟时间。
    void closed(SessionGeneration generation, std::optional<Error::Code> error, Clock::time_point now) override;

    // 调度拨号尝试: 当没有活动上游，也没有在途上游连接时，根据当前时间 now、同组偏好以及稳定散列顺序，
    // 选择至多一个最优的候选上游，并将其标记为 pending（在途）状态。
    // 参数:
    // - now: 当前时钟时间。
    // 返回值: 如果有合适的拨号目标则返回 Member，否则返回 std::nullopt。
    std::optional<Member> due(Clock::time_point now) override;

    // 处理连接失败: 仅更新那些仍然与 target 身份信息完整匹配的候选节点。
    // 使用传入的 error 判断是否需要隔离该候选节点，使用 now 决定下一次允许重试的截止时间点。
    // 参数:
    // - target: 发生连接失败的目标节点信息。
    // - error: 导致失败的错误码。
    // - now: 失败发生时的系统当前时间。
    void failed(const Member& target, Error::Code error, Clock::time_point now) override;

    // 判断是否需要刷新: 当当前没有活动上游，且所有候选节点的轮询尝试均已耗尽，
    // 并且当前时间 now 已经达到或超过下一次刷新的到期时间时，消费掉刷新请求（返回 true），
    // 同时推进下一次刷新的截止时间。
    // 参数:
    // - now: 当前时钟时间。
    // 返回值: bool 类型，true 表示需要发起刷新名单操作。
    bool needs_refresh(Clock::time_point now) override;

    // 获取当前网络状态: 在互斥锁保护下，返回候选节点的总数以及活动上游的独立状态快照。
    // 被隔离的候选节点依然会被计入候选总数中。
    // 返回值: 包含初始化状态、候选数量、活动节点等信息的 NetworkStatus 结构体。
    NetworkStatus status() const override;

private:
    // Candidate 结构体用于记录和追踪每个候选上游节点的实时状态。
    struct Candidate {
        Member member;                       // 候选节点的身份和地址等核心信息。
        bool pending{};                      // 默认值为 false。表示当前是否已经对该候选节点发起了拨号（在途连接）。
        bool attempted{};                    // 默认值为 false。表示在当前的一轮遍历中，是否已经尝试连接过该候选。
        bool quarantined{};                  // 默认值为 false。表示该节点是否由于严重错误（如身份验证失败）被完全隔离，不再重试。
        std::uint32_t failures{};            // 默认值为 0。记录连续连接失败的次数，用于指数退避算法。
        Clock::time_point next{};            // 默认值为纪元 0。下一次允许发起拨号连接的最早时间点（受退避延迟影响）。
        Clock::time_point connected{};       // 默认值为纪元 0。记录该候选节点成功建立连接的时间点，用于判断连接是否稳定。
    };

    // 记录连接失败状态，更新退避。调用此函数时必须已经持有 mutex_ 互斥锁。
    // 该方法仅负责更新候选节点的状态，不执行实际的网络取消或连接逻辑。
    // 前置要求: candidate 必须属于当前可用索引内，且本地身份 local_ 已安装。
    // 处理规则: 如果遇到 identity, protocol, 或 conflict 错误，则将节点持续隔离（quarantined 置为 true），其他普通失败仅更新尝试次数与退避时间。
    // 参数:
    // - candidate: 要更新状态的候选节点引用。
    // - error: 发生的具体错误码。
    // - now: 当前时钟时间，用于计算 next（下次重试时间）。
    void record_failure(Candidate& candidate, Error::Code error, Clock::time_point now);

    Config config_;                                                  // 保存节点的运行时网络配置参数。
    mutable std::mutex mutex_;                                       // 用于保护内部状态（并发安全）的互斥锁，声明为 mutable 以便在 const 方法中加锁。
    std::optional<Member> local_;                                    // 存储初始化时传入的本地节点身份信息。
    std::inplace_vector<Candidate, 8> candidates_;                   // 预分配最大容量为 8 的连续数组，用于保存候选上游节点的状态。
    std::optional<std::pair<Principal, SessionGeneration>> active_;  // 记录当前成功建立且处于活动状态的上游。保存目标的主体标识 (Principal) 以及对应的会话代次。
    bool refresh_{};                                                 // 默认 false。一个标志位，用于标识是否触发了强制刷新请求。
    Clock::time_point next_refresh_{};                               // 记录下一次允许执行名单刷新的最早时间点。
};

// 工厂函数: 根据已经校验通过的配置 config，创建一个未初始化的 Planet 连接策略。
// 策略对象的独占所有权将直接交由 Runtime 接管。此函数内部不会触发任何拨号或网络连接动作。
// 参数:
// - config: 节点网络运行配置。
// 返回值: 返回一个指向 Policy 基类接口的独占智能指针，实际指向 PlanetUpstream 实例。
std::unique_ptr<Policy> make_planet(const Config& config);
} // namespace astra
