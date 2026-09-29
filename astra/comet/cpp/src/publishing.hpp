#pragma once
#include "core.hpp"
#include <comet/publisher.hpp>

namespace comet::detail {
// 同步调用线程执行 Query/Publish, 控制轮只负责取消; 不保存期望值或安排自动续租.
class Publishing final : public Activity, public std::enable_shared_from_this<Publishing> {
public:
    Publishing(std::shared_ptr<Core> core, Scope scope);                                                                                        // 本地构造, 接纳前不发 RPC.
    ~Publishing() override;                                                                                                                     // 归还最后一批 Key 的版本基线预算, 不持有 Data.
    Result<Publisher::Receipt> update(std::vector<Publisher::Entry> entries, std::chrono::milliseconds ttl, std::chrono::milliseconds timeout); // 同对象至多一个调用.
    void close() noexcept;                                                                                                                      // 立即关闭停止门并取消本次调用, 不等待 Core 控制轮.
    bool wait(std::chrono::milliseconds timeout) const;                                                                                         // 等待实际同步调用结束, 通知内不可等待.
    bool closed() const noexcept override;                                                                                                      // 对象停止门.
    bool finished() const noexcept override;                                                                                                    // 关闭且实际调用已结束.

    bool streaming() const noexcept override {
        return false;
    } // 占写对象名额.

    Core::Time poll(Core::Time now, const std::shared_ptr<const Binding>& binding, const std::optional<Error>& error, bool closing) override; // 只取消已失效绑定上正在执行的那次调用.

private:
    // stop_callback 析构与执行互斥, 不让 close 借用已经退出栈的 context.
    struct Cancel {
        grpc::ClientContext* context; // 仅在对应 Call 存活且回调未注销时有效.

        void operator()() const noexcept {
            context->TryCancel();
        } // 不取得 Core 或 Publishing 锁, 不执行用户代码.
    };

    struct Call {
        explicit Call(Publishing& owner); // 先构造 context, 再注册共享关闭和单次取消.

        grpc::ClientContext context;             // 最后析构, 晚于两个取消回调注销.
        std::stop_callback<Cancel> shutdown;     // Client 关闭直接触发, 不依赖控制轮空闲.
        std::stop_callback<Cancel> cancellation; // 仅属于当前 update, 不会误取消下一次调用.
    };

    // 只跨越一次 update 的资源, 异常、返回和取消路径均精确归还, 无后台重放.
    struct Operation {
        explicit Operation(Publishing& owner) : owner(owner) {} // owner 由调用的强引用保持存活.

        ~Operation();        // 真实 RPC 结束后归还预算和槽位, 失败撤销可复用基线.
        Publishing& owner;   // 本次所属对象, 不独立拥有线程.
        std::size_t bytes{}; // 当前受控编码和正文预算, 零表示尚未取得.
        bool admitted{};     // 已取得显式调用名额.
        bool claimed{};      // 已取得真实 RPC 名额.
        bool confirmed{};    // 收到匹配实例及版本的确认, 才允许后续省略 Query.
    };

    static Error error(Error::Code code, Error::Effect effect = Error::Effect::unapplied);                                                                                // 不泄漏业务正文.
    Result<void> prepare(grpc::ClientContext& context, const std::shared_ptr<const Binding>& binding, Core::Time deadline);                                               // 固定目标及剩余截止.
    void discard() noexcept;                                                                                                                                              // 已无并发 update 时释放 Key 基线及其计费, 不取得 Core 锁.
    Result<std::uint64_t> query(const std::shared_ptr<const Binding>& binding, const std::vector<Publisher::Entry>& entries, Core::Time deadline, std::string& instance); // 单次只读版本基线.

    const std::shared_ptr<Core> core_;                // 所有角色共用的认证与预算.
    const Scope scope_;                               // 固定公开范围.
    mutable std::mutex mutex_;                        // 保护取消源和进行中标志, 不跨网络或用户代码.
    mutable std::condition_variable condition_;       // wait 本地排空通知.
    std::stop_source cancellation_{std::nostopstate}; // 每次 update 独立创建, 旧控制轮只能取消旧状态.
    std::shared_ptr<const Binding> binding_;          // 当前调用固定目标, 结束即释放.
    std::weak_ptr<const Binding> confirmed_;          // 最近成功批次的目标, 不因缓存而保活旧连接.
    std::vector<std::string> keys_;                   // 最近一批排序后的精确 Key, 至多 128 个, 不建无界版本表.
    std::string instance_;                            // 最近成功确认的实例, 匿名接入也需要携带此水位归属.
    std::size_t bytes_{};                             // Key 基线的共享预算, 关闭或销毁时归还.
    std::uint64_t issued_{};                          // 已分配版本最高值, 未知结果也消耗版本, 不缓存 Data.
    bool active_{};                                   // 整个 update 正在执行, 禁止并发覆盖.
    std::atomic_bool closed_{};                       // 单向停止门.
};
} // namespace comet::detail
