#pragma once
#include "dispatch.hpp"
#include "grpc_session.hpp"
#include "landing.hpp"
#include <deque>

namespace astra {
// 两个动态来源域共用一条已鉴权逻辑流. 只由 Runtime 控制线程调用, 回调线程不接触复制状态.
class Exchange final : public Session::Data {
public:
    using Packet = proto::astra::v1::SessionPacket; // 单一 gRPC 流消息, 保活由 Session 优先调度.

    // 跨所有流的恢复缓冲预算, 只在控制线程计数; 不代替各域原生数据及 gRPC 解码预算.
    struct Budget {
        std::size_t used{};                       // 已保留的编码积压和完整候选工作区, 初始零.
        std::size_t maximum = 256 * 1024 * 1024;  // 本进程复制恢复额外逻辑空间上限.
        bool acquire(std::size_t bytes) noexcept; // 容量不足不改变计数.
        void release(std::size_t bytes) noexcept; // 仅归还本所有者已经取得的额度.
    };

    // peer 已由 Runtime 完成身份/代次准入并为两域 admit, capacity 为真实协商包上限.
    Exchange(Catalog::State& catalog, Ephemeris::State& ephemeris, std::string peer, Budget& budget, std::size_t capacity, Steady::time_point now);
    // 停止后释放私有半份快照/积压, 已安装来源保留到原 TTL.
    ~Exchange() override;
    Result<void> receive(const Packet& packet, Steady::time_point now) override; // 有界结构验证和完整前缀安装.
    Result<const Packet*> prepare(Steady::time_point now) override;              // 控制消息优先, 两域公平取尾部.
    Packet take() override;                                                      // 紧接 StartWrite 移交唯一包, 不建立第二份发送队列.
    bool ready() const noexcept override;                                        // 两个域的首轮来源目标都已安装.

private:
    // 两种来源的公共恢复机制, 原生字段合并仍由 State 各自实现, 不混成统一 KV 模型.
    template <typename Domain>
    class Pipe {
    public:
        using State = typename Domain::State;
        using Changes = std::conditional_t<std::same_as<Domain, Catalog>, proto::astra::v1::CatalogChanges, proto::astra::v1::EphemerisChanges>;
        using Snapshot = typename Landing<Domain>::Page;
        using Delta = std::conditional_t<std::same_as<Domain, Catalog>, proto::astra::v1::CatalogDelta, proto::astra::v1::EphemerisDelta>;
        static constexpr auto domain = std::same_as<Domain, Catalog> ? proto::astra::v1::DOMAIN_CATALOG : proto::astra::v1::DOMAIN_EPHEMERIS; // 明确线协议域.

        // 状态与预算均比逻辑流活得更久, peer 由外层 Exchange 持有固定字符串.
        Pipe(State& state, const std::string& peer, Budget& budget, std::size_t capacity, Steady::time_point now);
        ~Pipe();                                                                                // 归还本域全部候选/积压额度.
        Result<void> changes(const Changes& changes, Steady::time_point now);                   // 整包先验证, 逐事实安装前缀.
        Result<void> snapshot(const Snapshot& page, Steady::time_point now);                    // complete 才安装原生组/水位/投影.
        Result<void> repaired(const proto::astra::v1::Repaired& reply, Steady::time_point now); // 关联核对, R 不跳过其他目标.
        Result<void> repair(const proto::astra::v1::Repair& request, Packet& response);         // 从本机原生来源回答精确查询.
        Result<const Packet*> prepare(Steady::time_point now);                                  // 恢复请求/ACK/回补请求优先于发送源尾部.
        Packet take();                                                                          // 交给 gRPC 时才推进 Dispatch 的已发送位置.
        Result<void> resume(std::uint64_t position);                                            // 本流每域只接受一次恢复起点.
        bool acknowledge(std::uint64_t position);                                               // ACK 只确认已经送出的完整边界.
        bool ready() const noexcept;                                                            // 已到达首次声明的源头, 后续新写入不撤销启动完成事实.
        bool expired(Steady::time_point now) const noexcept;                                    // 优先响应对端也不能跳过本域恢复截止.

    private:
        // 队列只有正在等待精确回补时才累积, 持有包及处理偏移, 不复制成逐 Key 工作流.
        struct Pending {
            Changes message;     // 不可变来源包, 不能提前释放尚未处理的原始提交.
            int offset{};        // 包内下一条提交, 范围 0..entries_size.
            std::size_t bytes{}; // 已取得的预算, 完整消费或断流时归还.
        };

        // 将原生拒绝映射成固定流错误, 不输出业务正文/远端错误信息.
        static Status failure(typename State::Error error);
        static const std::string& key(const Delta& delta);                // Catalog Key / Ephemeris UUID, 不做文本转换.
        Result<void> drain(Steady::time_point now);                       // 只推进已具备完整字段的连续前缀.
        Result<bool> apply(const Delta& delta, Steady::time_point now);   // false 表示已排入唯一回补等待.
        Result<void> missing(const Delta& delta, Steady::time_point now); // 同域只留一个当前回补, 其余事实受队列预算约束.
        void clear() noexcept;                                            // 放弃私有接收工作, 不改变已确认来源根.
        void acknowledge();                                               // 合并最新 ACK, 不为每条事实排一条回执.

        State& state_;                                   // 真实原生提交器, 不持有网络对象.
        const std::string& peer_;                        // 已鉴权来源 ID, 外层保证固定寿命.
        Budget& budget_;                                 // 全 Runtime 共用的恢复空间预算.
        Dispatch<Domain> dispatch_;                      // 只发送自身事实, 不转发已接收副本.
        std::deque<Pending> pending_;                    // 最大 64 包且计费不超过 8 MiB.
        std::size_t bytes_{};                            // 本队列已计费字节.
        std::optional<Landing<Domain>> landing_;         // 私有全量候选, 每域至多一个.
        std::size_t workspace_{};                        // 全量候选预留 64 MiB, 防止多流同时准备失控.
        std::optional<proto::astra::v1::Repair> repair_; // 当前阻塞目标及触发位置, 不是全局任务 ID.
        std::optional<Packet> packet_;                   // 本域至多一个控制包; 数据包留在 Dispatch 内.
        std::optional<std::uint64_t> acknowledgement_;   // 最新已安装连续位置, 重复通知合并.
        std::optional<std::uint64_t> target_;            // 首个数据包声明的来源目标, 用于启动屏障.
        std::uint64_t received_{};                       // 已安装连续位置, 与目标回补 R 明确分开.
        std::uint64_t seen_{};                           // 收到的完整前缀/完整基线, 包积压时可以领先 received_.
        Steady::time_point deadline_;                    // 首轮、半份快照或回补的 30 s 无进展期限.
        bool resume_ = true;                             // 首次发出本域恢复请求后归零.
        bool opened_{};                                  // 已收到对端恢复请求, Dispatch 才可发送数据.
        bool requested_{};                               // 当前 repair 已交给 gRPC, 不每轮重复发请求.
        bool dispatched_{};                              // prepare 当前借用来自 Dispatch, take 按此归还.
    };

    // 回补响应数量及字节双重有界, 高优先级控制包仍遵守唯一在途写.
    struct Response {
        Packet packet;       // 本机来源捕获边界生成的完整响应.
        std::size_t bytes{}; // 持有期间占用共享预算.
    };

    std::string peer_;               // 在两个 Pipe 之前声明, 析构顺序保证它最后释放.
    Budget& budget_;                 // Runtime 持有, 比所有 Session 活得更久.
    const std::size_t capacity_;     // 协商硬上限, 默认最大 8 MiB.
    Pipe<Catalog> catalog_;          // 独立域位置/基线/修复状态.
    Pipe<Ephemeris> ephemeris_;      // 不与 Catalog 共用来源序号.
    std::deque<Response> responses_; // 最大 8 条、合计 16 MiB, 不建立每 Key 永久缓存.
    std::size_t response_bytes_{};   // 当前响应计费.
    unsigned turn_{};                // 两域轮转 0/1, 默认 Catalog 先选.
    unsigned selected_{};            // 0 表示无包, 1/2 为域, 3 为回补响应.
};
} // namespace astra
