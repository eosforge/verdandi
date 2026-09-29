#pragma once
#include "core.hpp"
#include "lifetime.hpp"
#include <comet/beacon.hpp>

namespace comet::detail {
// 同步 Data 与异步租约维护共享一个状态, 恢复仅使用确认缓存, 不保留待重放的业务请求.
class Beaming final : public Activity, public std::enable_shared_from_this<Beaming> {
public:
    Beaming(std::shared_ptr<Core> core, Scope scope, Value attr, Value data, std::chrono::milliseconds ttl, std::chrono::milliseconds beat, Beacon::Options options); // 构造只预留缓存, accept 后才注册.
    ~Beaming() override;                                                                                                                                              // 所有实际 RPC 和采样已结束, 归还固定缓存.
    Result<void> initialize(Core::Time deadline);                                                                                                                     // 工厂唯一同步首次注册, 失败由工厂立即 close.
    Result<Beacon::Receipt> update(Value data, std::chrono::milliseconds timeout, Core::Time started);                                                                // 一个同步调用, 不排队或后台重试.
    Result<void> tick(std::chrono::milliseconds interval, std::move_only_function<Value()> callback);                                                                 // 替换采样器和代次, 空回调解除.
    Result<void> changed(std::move_only_function<void(Beacon::State)> callback);                                                                                      // 替换状态回调并安排当前状态.
    Beacon::State state() const;                                                                                                                                      // 每次重查包含 suspend 的租约预算.
    void close() noexcept;                                                                                                                                            // 取消显式 RPC, 停止自动任务, 不等待用户回调.
    bool wait(std::chrono::milliseconds timeout) const;                                                                                                               // 仅外部线程允许等待本地实际完成.

    bool closed() const noexcept override {
        return closed_.load(std::memory_order_acquire);
    }

    bool finished() const noexcept override {
        return finished_.load(std::memory_order_acquire);
    }

    bool streaming() const noexcept override {
        return false;
    } // 与 Watch 的物理额度分开.

    Core::Time poll(Core::Time now, const std::shared_ptr<const Binding>& binding, const std::optional<Error>& error, bool closing) override; // 唯一控制轮, 不阻塞网络.
private:
    using Notice = std::move_only_function<void(Beacon::State)>; // 活跃通知拥有自己的回调包装.
    using Sample = std::move_only_function<Value()>;             // 采样器仅返回完整拥有式 Data.

    struct Cancel {
        grpc::ClientContext* context; // stop_callback 的寿命严格短于 context.

        void operator()() const noexcept {
            context->TryCancel();
        }
    };

    struct Call {
        grpc::ClientContext context;             // 应用线程中的同步 RPC, 取消不提前结束此存储.
        std::stop_callback<Cancel> shutdown;     // 共享 Client 关闭直接取消, 不依赖控制轮.
        std::stop_callback<Cancel> cancellation; // 本对象关闭或原目标变更直接取消.
        explicit Call(Beaming& owner);           // 取得本次固定的停止令牌.
    };

    struct Operation {
        Beaming& owner;      // active_ 的唯一清理责任.
        bool admitted{};     // 同步调用额度是否取得.
        bool claimed{};      // 实际 unary 槽是否取得.
        std::size_t bytes{}; // 在途请求/回复/输入所有权预算.

        explicit Operation(Beaming& owner) : owner(owner) {} // 不发请求.

        ~Operation(); // 真实同步 RPC 返回后归还, 唤醒维护与 wait.
    };

    template <class Request, class Reply>
    struct Attempt {
        std::shared_ptr<Beaming> owner;                    // 最终 callback 前保活, 不增加应用拥有计数.
        std::shared_ptr<const Binding> binding;            // 实际请求目标不可改投.
        Request request;                                   // 发出后不再修改.
        Reply reply;                                       // done 的 release/acquire 发布完整回复.
        grpc::ClientContext context;                       // 固定有限截止.
        Lifetime::Time sent{};                             // 首次实际发送的含 suspend 时间.
        std::atomic_bool done{};                           // 网络最终完成, 非取消请求.
        grpc::StatusCode code = grpc::StatusCode::UNKNOWN; // 只保存有界错误编号.
        std::size_t bytes{};                               // 实际拥有预算.
        bool claimed{};                                    // 实际自动 RPC 槽位.
        bool priority{};                                   // 续租/注销用保留槽.

        ~Attempt() {
            if (claimed) {
                owner->core_->returning(priority, true);
            }
            static_cast<void>(owner->core_->resize(bytes, 0));
        }
    };

    using Creating = Attempt<proto::comet::v1::CreateRequest, proto::comet::v1::CreateReply>;
    using Renewing = Attempt<proto::comet::v1::RenewRequest, proto::comet::v1::RenewReply>;
    using Removing = Attempt<proto::comet::v1::RemoveRequest, proto::comet::v1::Empty>;
    template <class T>
    std::shared_ptr<T> prepare(const std::shared_ptr<const Binding>& binding, Core::Time deadline, bool priority); // 准备拥有式上下文, 不发请求.
    template <class T>
    bool admit(const std::shared_ptr<T>& call); // 编码完成才计费和取得实际槽.
    template <class T>
    static void complete(const std::shared_ptr<T>& call, const grpc::Status& status) noexcept;                                                                                             // 网络回调只发布最终结果.
    Result<void> begin(Operation& operation, Core::Time deadline, std::size_t bytes, std::shared_ptr<const Binding>& binding);                                                             // 原截止内等待共享认证并冻结目标.
    Result<void> context(grpc::ClientContext& context, const std::shared_ptr<const Binding>& binding, Core::Time deadline);                                                                // 请求前最后确认生命周期和目标.
    Result<Beacon::Receipt> submit(Value data, Core::Time deadline, std::optional<std::uint64_t> sample);                                                                                  // sample 有值时拒绝过时代次.
    void sample() noexcept;                                                                                                                                                                // 有界工作线程执行, 捕获业务异常, 总是归还 sampling_.
    Result<void> install(const proto::comet::v1::CreateReply& reply, const std::shared_ptr<const Binding>& binding, std::uint64_t generation, Lifetime::Time sent, std::size_t& reserved); // 校验成功回复后安装确认缓存.
    void consume(const std::shared_ptr<const Binding>& binding);                                                                                                                           // 控制轮消费自动调用, 不结算应用同步结果.
    void restore(const std::shared_ptr<const Binding>& binding, Core::Time now, Lifetime::Time time);                                                                                      // 同逻辑 ID 和确认 Data, 每次消耗新注册代次.
    void renew(const std::shared_ptr<const Binding>& binding, Core::Time now, Lifetime::Time time);                                                                                        // 新独立 order, 不重算旧回复的租约.
    void remove(const std::shared_ptr<const Binding>& binding);                                                                                                                            // 关闭预算内最多一次尽力注销.
    void failure(const std::shared_ptr<const Binding>& binding, Error error);                                                                                                              // 持锁更新局部状态与共享连接故障.
    void publish(Beacon::Phase phase, std::optional<Error> error = {});                                                                                                                    // 标记通知, 不调用用户.
    bool target(const Binding& binding) const;                                                                                                                                             // 对比已注册 Star 和实际端点, 重认证允许复用.
    static Error error(Error::Code code, Error::Effect effect = Error::Effect::unapplied);                                                                                                 // 有界本地错误.
    const std::shared_ptr<Core> core_;                                                                                                                                                     // 所有对象共享连接和调度.
    const Scope scope_;                                                                                                                                                                    // 固定注册范围.
    const Value attr_;                                                                                                                                                                     // 固定属性, 工厂后永不更改.
    Value data_;                                                                                                                                                                           // 仅最后成功确认的 Data, 初次注册前保存初值.
    const std::chrono::milliseconds ttl_;                                                                                                                                                  // 固定 1 s..10 min.
    const std::chrono::milliseconds beat_;                                                                                                                                                 // 0 < beat < ttl, 实际发送后重新计时.
    mutable std::mutex mutex_;                                                                                                                                                             // 不跨用户代码和同步 RPC.
    mutable std::condition_variable condition_;                                                                                                                                            // 本地清理通知.
    std::size_t bytes_{};                                                                                                                                                                  // 固定 Attr/确认 Data/元数据预算.
    Beacon::State state_;                                                                                                                                                                  // 身份在恢复间保持逻辑 UUID, instance 更新为最近确认目标.
    std::string capability_;                                                                                                                                                               // 首次成功注册签发, 不输出或进入公开状态.
    std::shared_ptr<const Binding> binding_;                                                                                                                                               // 当前已注册的实际目标, 失效时清空.
    std::shared_ptr<const Binding> active_binding_;                                                                                                                                        // 显式同步调用固定目标.
    std::stop_source cancellation_{std::nostopstate};                                                                                                                                      // active_ 期间唯一停止源.
    bool active_{};                                                                                                                                                                        // 同对象只接纳一个同步调用.
    bool initialized_{};                                                                                                                                                                   // 首次注册已成功, 只有此后才允许后台恢复.
    bool permanent_{};                                                                                                                                                                     // 永久错误停止自动恢复, 不通过新 ID 绕过.
    Lifetime lifetime_;                                                                                                                                                                    // 以首次发送起点确认保守租约.
    std::optional<Lifetime::Time> sent_;                                                                                                                                                   // 最近真正发送新 Update/Renew/Create 的时间, 非输入尝试.
    std::uint64_t issued_{};                                                                                                                                                               // 已消耗 Data order, 未知结果也不能复用.
    std::uint64_t confirmed_{};                                                                                                                                                            // 恢复缓存对应的 Data order, 不冒充 issued_.
    std::uint64_t generation_ = 1;                                                                                                                                                         // 首次注册为 1, 每次恢复递增, 耗尽不绕回.
    std::uint64_t renewal_{};                                                                                                                                                              // 当前注册的独立续租序列.
    Core::Time retry_{};                                                                                                                                                                   // 自动维护退避, 初始可以立即执行.
    unsigned failures_{};                                                                                                                                                                  // 有界退避指数.
    std::shared_ptr<Notice> changed_;                                                                                                                                                      // 替换时旧回调仍由正在执行的通知持有.
    bool dirty_{};                                                                                                                                                                         // 合并状态通知, 不积累队列.
    bool notifying_{};                                                                                                                                                                     // 已开始的状态回调可完成, wait 等其结束.
    std::shared_ptr<Sample> sampler_;                                                                                                                                                      // 当前可选采样器.
    std::chrono::milliseconds interval_{};                                                                                                                                                 // 采样周期, 未启用为零.
    Core::Time tick_{};                                                                                                                                                                    // 下一次采样, 不补发错过的周期.
    std::uint64_t sampling_generation_{};                                                                                                                                                  // 采样器替换和显式更新使旧采样失效.
    bool sampling_{};                                                                                                                                                                      // 从排入工作槽到采样/提交结束一直为真.
    std::atomic_bool closed_{};                                                                                                                                                            // 立即关闭门.
    std::atomic_bool finished_{};                                                                                                                                                          // 所有实际 RPC、采样和通知结束.
    std::shared_ptr<Beaming> retained_;                                                                                                                                                    // 关闭期间有界清理保活, 完成释放.
    Core::Time close_at_{};                                                                                                                                                                // 首次 close 固定截止.
    bool removed_{};                                                                                                                                                                       // 最多一次 Remove.
    std::shared_ptr<Creating> creating_;                                                                                                                                                   // 取消后仍等 done, 不另开第二次恢复.
    std::shared_ptr<Renewing> renewing_;                                                                                                                                                   // 与显式 Update 独立, 自动保留槽.
    std::shared_ptr<Removing> removing_;                                                                                                                                                   // 有限清理.
};
} // namespace comet::detail
