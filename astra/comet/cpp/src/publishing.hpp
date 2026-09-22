#pragma once
#include "core.hpp"
#include "lifetime.hpp"
#include <comet/publisher.hpp>
#include <variant>

namespace comet::detail {
// 一 Key 一期望, 一个实际发布或续租尝试; 新正文优先, Publish 本身确认一次保活.
class Publishing final : public Activity, public std::enable_shared_from_this<Publishing> {
public:
    Publishing(std::shared_ptr<Core> core, Scope scope, std::string key, std::chrono::milliseconds ttl, Publisher::Options options); // 工厂接纳前不发 RPC.
    ~Publishing() override;                                                                                                          // 已无网络引用, 归还当前期望预算.
    Publisher::State state() const;                                                                                                  // 只取短锁/本地计时, 不发网络请求.
    std::future<Result<Publisher::Receipt>> publish(std::uint64_t version, Value value, std::chrono::milliseconds timeout);          // 高版本替换未发期望.
    void close() noexcept;                                                                                                           // 非阻塞关闭与应用拥有数只改变一次.
    bool wait(std::chrono::milliseconds timeout) const;                                                                              // 不允许在 SDK 回调内等待.
    bool closed() const noexcept override;
    bool finished() const noexcept override;

    bool streaming() const noexcept override {
        return false;
    } // 与 Watch 分开计量实际请求.

    Core::Time poll(Core::Time now, const std::shared_ptr<const Binding>& binding, const std::optional<Error>& blocked, bool closing) override; // 唯一控制轮推进.

private:
    struct Pending {
        std::shared_ptr<Core> admission; // 显式调用总额度的拥有者, 结算后置空.

        ~Pending() {
            if (admission) {
                admission->settled();
            }
        } // 未结算候选异常析构也精确归还.

        std::uint64_t version{};                                        // 应用已接纳的正内容版本, 不在后台更改.
        Value value;                                                    // 固定完整正文, 重试共享.
        std::optional<std::promise<Result<Publisher::Receipt>>> result; // 显式调用只完成一次.
        Core::Time deadline{};                                          // 从接纳起算, 超时不撤销对象期望.
    };

    // 请求/回复同一类型组内成对, 不为每次续租构造大型 Publish 消息.
    struct Publish {
        proto::comet::v1::PublishRequest request;
        proto::comet::v1::PublishReply reply;
    };

    struct Renew {
        proto::comet::v1::CatalogRenewRequest request;
        proto::comet::v1::Empty reply;
    };

    struct Call {
        std::shared_ptr<Publishing> owner;                 // 最后 callback 返回前保活业务状态.
        std::shared_ptr<const Binding> binding;            // 固定实际目标和 Session, 不随 Core 切换修改.
        std::shared_ptr<Pending> pending;                  // 本次固定的版本/正文/结果, 不借最新期望.
        std::variant<Publish, Renew> message;              // 实际一次请求的类型安全消息.
        grpc::ClientContext context;                       // 有限尝试期限, 取消后仍等待最终完成.
        std::atomic_bool done{};                           // release/acquire 发布回复/metadata.
        grpc::StatusCode code = grpc::StatusCode::UNKNOWN; // 不保存任意远端正文.
        Lifetime::Time sent{};                             // 本次首次发送的包含休眠起点, 不从回应到达起算 TTL.
        std::size_t bytes{};                               // 原期望保有/编码副本的保守计费.
        bool claimed{};                                    // 实际 RPC 槽已取得, 最后引用释放时才归还.
        bool automatic{};                                  // 实际容量类别, 析构按原类别归还.
        bool renewal{};                                    // 对应 variant 活动类型, 决定容量组和成功解析.
        ~Call();                                           // 归还计费和槽位, 不在 gRPC 活动期间释放请求.
    };

    static Error error(Error::Code code, Error::Effect effect = Error::Effect::unapplied);                       // 受限本地失败.
    static void settle(const std::shared_ptr<Pending>& pending, Result<Publisher::Receipt> result);              // 精确完成一次 future.
    static void complete(const std::shared_ptr<Call>& call, const grpc::Status& status) noexcept;                // callback 仅发布结果.
    bool target(const Binding& binding) const;                                                                   // 同 Star 重新认证保持内容身份, 换节点重发完整正文.
    void consume(const std::shared_ptr<const Binding>& binding, Core::Time now);                                 // 已结束请求才能检查回复/错误.
    void send(const std::shared_ptr<const Binding>& binding, Core::Time now, Lifetime::Time time, bool renewal); // 只发当前固定期望.
    void report(Publisher::Phase phase, std::optional<Error> error = {});                                        // 持锁标记通知, 不回调用户.
    void notify(std::unique_lock<std::mutex>& lock);                                                             // 解锁调用应用, 异常只累计诊断.

    const std::shared_ptr<Core> core_;                        // 与全部业务共享连接、预算和单控制循环.
    const Scope scope_;                                       // 固定外部范围.
    const std::string key_;                                   // 1..1024 字节完整 UTF-8, 无路径解释.
    const std::chrono::milliseconds ttl_;                     // 固定 1s..10m, 不进入远端时钟参考系.
    mutable std::mutex mutex_;                                // 只保护当前期望/确认/通知, 不跨用户回调.
    mutable std::condition_variable condition_;               // 本地清理等待.
    std::shared_ptr<Pending> wanted_;                         // 未首次 publish 之前为空, 不自动发布空值.
    std::shared_ptr<Call> call_;                              // 唯一实际尝试, 取消后保留到最终 callback.
    std::shared_ptr<const Binding> binding_;                  // 最近完整发布所确认的目标, 不保存旧目标列表.
    Lifetime lifetime_;                                       // 当前期望的保守本地有效预算.
    Publisher::State state_;                                  // 最后公开状态, 初始 waiting.
    std::move_only_function<void(Publisher::State)> changed_; // 构造后固定, 锁外通知.
    std::size_t bytes_{};                                     // 当前期望/元数据计费, 在途独立计费.
    bool applied_{};                                          // 最新期望已在目标完整 Publish 成功.
    bool rejected_{};                                         // 此期望永久拒绝, 等待新的合法显式调用.
    bool dirty_{};                                            // 合并通知, 不建 FIFO.
    bool notifying_{};                                        // 当前用户回调仍在执行, wait 必须等待.
    std::atomic_bool closed_{};                               // 单向停止门.
    std::atomic_bool finished_{};                             // 实际尝试和本地资源已清理.
    unsigned failures_{};                                     // 有界退避指数, 完整确认后归零.
    Core::Time retry_{};                                      // 允许下一次实际发送的单调时间.
};
} // namespace comet::detail
