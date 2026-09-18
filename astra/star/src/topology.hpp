// 该策略负责根据成员角色限制网络并发容量, 并且能够隔离和管理旧代次(epoch)的会话完成事件, 确保安全平滑升级.
#pragma once
#include <astra/policy.hpp>

#include <array>
#include <map>
#include <mutex>

namespace astra {
// Star 类继承自 Policy 接口.作为 Star 节点的核心网络拓扑策略.
// 该类独占持有所有的已知合法网络成员名单, 并且维护出站(outbound)和入站(inbound)两个方向的会话索引.
// 内部使用了互斥锁(mutex_)保护, 该锁仅覆盖内存状态的修改.
// 所有实际的网络 I/O 资源取消或关闭操作, 均由调用方在锁外执行以避免死锁.
class Star final : public Policy {
public:
    // 构造函数: 复制传入的 config 配置中的网络容量限制和退避参数设置.
    // 初始状态下没有本地身份(未初始化 local_), 且不主动向外发起网络连接拨号.
    // - config: 节点网络运行相关配置.
    explicit Star(const Config& config);

    // 初始化策略: 借用已经完成签名验证的本地节点身份 local 以及一份完整的 Star 集群成员名单 members.
    // 在一次性安装名单之前, 会验证传入名单的顺序合法性, 主体唯一性, 并确保自身存在于名单中.
    // - local: 本地 Star 节点的身份信息.
    // - members: 传入的当前最新完整成员拓扑名单.
    // 返回值:
    // - 成功返回空.
    // - 如果超出了容量限制返回 Status::Code::capacity 错误.
    // - 如果名单内容(如顺序/去重)存在问题, 返回 Status::Code::identity.
    // - 如果尝试多次初始化, 返回 Status::Code::conflict.任何失败都会保留原有的旧状态不变.
    Result<void> initialize(const Member& local, std::span<const Member> members) override;

    // 接受连接请求: 按照 Policy 的统一契约接纳已经完成验签的对端节点 remote.
    // 约束: 如果是主动出站连接(outbound), 那么 remote 必须精确匹配传入的 expected 目标;
    // 如果对方角色是 Planet, 那么只允许入站连接(inbound).
    // 会将会话的唯一标识 generation 安装到成员对应方向的槽位(slot)中.
    // 如果检测到是同节点合法的代次(epoch)提升, 策略会通过返回值给出一组待断开(取消)的旧版本会话.
    // - remote: 对端节点的身份信息.
    // - direction: 连接方向(出站或入站).
    // - generation: 本次新连接的会话代次.
    // - expected: 预期的目标节点(通常在出站拨号时存在).
    // 返回值: 成功时返回包含了需要被取消的过期 Generation 列表(如果是平滑替换);
    // 发生身份, 冲突或超出容量错误时, 直接返回 error.
    Result<std::vector<Generation>> accept(const Member& remote, Policy::Direction direction, Generation generation, const std::optional<Member>& expected) override;

    // 处理会话关闭事件: 仅当传入的 generation 与当前槽位内保存的代次匹配时, 才将该槽位释放(清空).
    // 如果是出站(outbound)连接断开, 会根据 now 当前时间和稳定连接阈值来动态更新重试退避时间.
    // 不同于 Planet, Star 节点不会依据发生的具体 error 类型来"隔离(quarantine)"集群内正式成员,
    // 已获准成员在断线后继续按退避重连, 这里不实现共识或自动删除成员.
    // - generation: 宣告关闭的会话代次.
    // - error: 触发关闭的错误码.
    // - now: 当前系统时钟时间.
    void closed(Generation generation, std::optional<Status::Code> error, Steady::time_point now) override;

    // 调度出站拨号: 当系统时钟到达 now 且存在退避到期且其出站连接槽位未被占用的 Star 成员时,
    // 策略会选择一个成员快照返回以供外层发起拨号.
    // 同时同步将该成员置为 pending 状态, 防止被后续调用重复选中.
    // - now: 当前时钟时间.
    // 返回值: 需要连接的目标成员(如果有).
    std::optional<Member> due(Steady::time_point now) override;

    // 处理拨号失败回调: 仅当传入的 target 与当前拓扑名单中保存的完整成员信息值相符时,
    // 才释放对应的 pending 拨号锁定状态, 并利用 now 计算下一次的重试退避期限.
    // - target: 发生连接失败的目标节点快照.
    // - error: 错误码.如前所述, Star 不依靠该 error 区分致命故障并进行本地名单层面的隔离.
    // - now: 当前系统时钟时间.
    void failed(const Member& target, Status::Code error, Steady::time_point now) override;

    // 判断是否需要刷新: 与 Planet 能够动态刷新候选单上游不同, Star 的初始完整名单一旦安装成功,
    // 不会通过此周期接口主动从 Supervisor 拉取刷新请求; 新身份由已验签的入站会话接纳.
    // 所以在此方法中忽略 now 并始终固定返回 false.
    bool needs_refresh(Steady::time_point now) override;

    // 获取当前网络状态: 在互斥锁内部安全地读取成员总数以及各种已安装完成的会话统计数目.
    // 构造并返回一个独立的 Policy::State 状态快照.请注意, 该统计结果不包含处于前期握手 RPC 阶段的半开连接.
    Policy::State status() const override;

private:
    // Entry 结构体记录 Star 拓扑网络中每个已知节点的具体运行时连接状态.
    struct Entry {
        Member member; // 节点的身份, 地址等元数据.

        // 记录节点会话代次槽位.
        // std::array 的大小固定为 2.
        // 根据约定: 槽位 0 代表出站方向 (outbound), 槽位 1 代表入站方向 (inbound).
        // 这一布局必须与 Policy::Direction 枚举的值及其顺序保持严格一致.
        std::array<std::optional<Generation>, 2> sessions{};

        bool pending{};                 // 默认值为 false.是否正在向该节点发起出站拨号(等待握手结果).
        std::uint32_t failures{};       // 默认值为 0.累计的出站连接连续失败次数.
        Steady::time_point next{};      // 默认值为纪元 0.下一次允许向其发起出站重试的到期时间.
        Steady::time_point connected{}; // 默认值为纪元 0.当前出站连接成功建立的时间, 用于判定连接稳定性.
    };

    Config config_;               // Star 的运行时配置信息(包含最大节点数, 重连退避参数等).
    mutable std::mutex mutex_;    // 保护以下可变状态的并发同步锁.
    std::optional<Member> local_; // 本地 Star 节点的身份, 未初始化前为 nullopt.

    // 首版设计采用标准的 std::map 结构(红黑树), 以 Principal 作为键, Entry 为值, 保持了一个确定的成员字典排序,
    // 这有助于实现可预测的确定性拨号遍历顺序.
    // 由于节点总数受 config_.max_members 部署容量限制(通常不大), 这样的开销是可以接受的.
    // 另外强调: 绝不在高频的心跳关键路径中进行此 map 的整体深拷贝.
    std::map<Principal, Entry> members_;
};

// 工厂函数: 根据校验无误的 config 创建并返回一个尚未初始化的 Star 策略实例.
// 返回的智能指针(Policy 类型的所有权)将被转移交给 Runtime, 该工厂函数不主动持有 config 配置的长期借用.
// - config: 节点网络运行配置.
std::unique_ptr<Policy> make_star(const Config& config);
} // namespace astra
