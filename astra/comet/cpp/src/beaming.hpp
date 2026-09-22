#pragma once
#include "core.hpp"
#include "lifetime.hpp"
#include <comet/beacon.hpp>

namespace comet::detail {
// Beacon 的唯一期望/身份所有者, Create 与已注册操作互斥, Data/Renew 独立在途.
class Beaming final : public Activity, public std::enable_shared_from_this<Beaming> {
public:
    Beaming(std::shared_ptr<Core> core, Scope scope, Value attr, Value data, std::chrono::milliseconds ttl, Beacon::Options options);         // 不发 RPC, 工厂接纳成功才调度.
    ~Beaming() override;                                                                                                                      // 实际操作已结束, 归还本对象期望载荷额度.
    Beacon::State state() const;                                                                                                              // 重新观察包含休眠的本地预算, 不延长租约.
    std::future<Result<Beacon::Receipt>> update(Value data, std::chrono::milliseconds timeout);                                               // 一个最新待发值, 原在途尝试不改写.
    void close() noexcept;                                                                                                                    // 异步注销意图与 Core 应用拥有计数只改变一次.
    bool wait(std::chrono::milliseconds timeout) const;                                                                                       // 不允许 SDK 回调内阻塞.
    bool closed() const noexcept override;                                                                                                    // Core 目录不反向取得本对象锁.
    bool finished() const noexcept override;                                                                                                  // 包含取消后的真实回调和 Remove 清理.
    Core::Time poll(Core::Time now, const std::shared_ptr<const Binding>& binding, const std::optional<Error>& error, bool closing) override; // Core 锁外的唯一推进.

    bool streaming() const noexcept override {
        return false;
    } // 与 Watch 分开预留应用及实际 RPC 额度.

private:
    // 每个显式调用的 future 与持久期望分开, 请求发送后不能被 later update 改写.
    struct Pending {
        std::shared_ptr<Core> admission; // 显式调用总额度的拥有者, 结算后置空.

        ~Pending() {
            if (admission) {
                admission->settled();
            }
        } // 未结算候选异常析构也精确归还.

        Value data;                                                  // 不可变期望内容, 即使本次 future 超时仍可用于后台恢复.
        std::optional<std::promise<Result<Beacon::Receipt>>> result; // 有值时尚未结算, 移除后永久不再次完成.
        Core::Time deadline{};                                       // 从接纳开始的单调期限, 不因建连/认证重置.
        std::uint64_t order{};                                       // 实际首次发送才分配, 同身份重试保持原值.
    };

    // 具体生成类型只在私有实现实例化, 请求/回复/上下文均由本次尝试拥有.
    template <class Request, class Reply>
    struct Attempt {
        std::shared_ptr<Beaming> owner;                    // 至最终回调返回保活, 不延长应用的自动注册意图.
        std::shared_ptr<const Binding> binding;            // 固定实际地址/Session, 不随 Core 切换修改.
        std::optional<Beacon::Identity> identity;          // 非 Create 操作的完整固定身份.
        std::shared_ptr<Pending> pending;                  // Create/Data 实际捕获的期望, 不借用后来的最新值.
        Request request;                                   // 整个 RPC 期间不可修改.
        Reply reply;                                       // gRPC 只写这一份, done 的发布后才能读取.
        grpc::ClientContext context;                       // 有限期限, 不在此调用等待网络.
        Lifetime::Time sent{};                             // Create/新 Renew 的首次发送本地起点, 重试不更新.
        std::atomic_bool done{};                           // release/acquire 发布最终回复与 trailing metadata.
        grpc::StatusCode code = grpc::StatusCode::UNKNOWN; // 不保存任意远端错误正文.
        std::size_t bytes{};                               // 此次 Protobuf/额外期望引用的保守计费.
        bool claimed{};                                    // 实际在途额度是否接纳, 析构只归还一次.
        bool automatic{};                                  // 实际发送时固定, 不随 pending 的 future 完成而改变.
        bool priority{};                                   // Renew/Remove 使用独立保留槽, 不被普通请求占满.

        ~Attempt() { // 最后 callback/控制引用释放后才归还实际内存和槽位.
            if (claimed) {
                owner->core_->returning(priority, automatic);
            }
            static_cast<void>(owner->core_->resize(bytes, 0));
        }
    };

    using Creating = Attempt<proto::comet::v1::CreateRequest, proto::comet::v1::CreateReply>; // UUID 确认之前唯一请求.
    using Updating = Attempt<proto::comet::v1::UpdateRequest, proto::comet::v1::UpdateReply>; // 每 UUID 正常一个 Data 请求.
    using Renewing = Attempt<proto::comet::v1::RenewRequest, proto::comet::v1::RenewReply>;   // 独立续租, 不排在 Data 后面.
    using Removing = Attempt<proto::comet::v1::RemoveRequest, proto::comet::v1::Empty>;       // 关闭时最多一个有界注销.

    template <class Call>
    std::shared_ptr<Call> prepare(const std::shared_ptr<const Binding>& binding, Core::Time deadline, bool priority); // 只准备, 无槽/内存时返回空且不发请求.
    template <class Call>
    bool admit(const std::shared_ptr<Call>& call); // 消息已构造后计费并取得在途槽.
    template <class Call>
    static void complete(const std::shared_ptr<Call>& call, const grpc::Status& status) noexcept; // 最终 callback 只发布 I/O, 不调用应用.
    template <class Call>
    static Error failure(const std::shared_ptr<Call>& call);                                                                              // OnDone 后读取当前尝试的生成错误细节.
    static void settle(const std::shared_ptr<Pending>& pending, Result<Beacon::Receipt> result);                                          // 精确结算一次 future, 不改写最新期望.
    static Error error(Error::Code code, Error::Effect effect = Error::Effect::unapplied);                                                // 构造不带敏感正文的本地原因.
    bool same(const std::optional<Beacon::Identity>& identity) const;                                                                     // 防止旧 UUID 的成功改变新注册.
    bool target(const Binding& binding) const;                                                                                            // 匿名绑定未获实例时仍按固定实际端点判断.
    void forget();                                                                                                                        // 新身份开始前取消旧 Data/Renew, 保留最新期望, 不延长旧租约.
    void failed(const std::shared_ptr<const Binding>& binding, Error error, bool creation, const std::shared_ptr<Pending>& pending = {}); // 原因归属/共享重连与永久拒绝分开.
    void consume(const std::shared_ptr<const Binding>& binding, Core::Time now);                                                          // 消费实际结束的四类请求, 不提前回收取消中的对象.
    void create(const std::shared_ptr<const Binding>& binding, Core::Time now, Lifetime::Time time);                                      // 固定本次 Attr/Data/TTL, 不查询旧创建结果.
    void renew(const std::shared_ptr<const Binding>& binding, Core::Time now, Lifetime::Time time);                                       // 相同 order 重试保留首次发送起点.
    void send(const std::shared_ptr<const Binding>& binding, Core::Time now);                                                             // 一个当前 Data 尝试, 后来值仅替换待发期望.
    void remove(const std::shared_ptr<const Binding>& binding);                                                                           // 关闭时只处理当前已确认身份, 不补发旧未知注册.
    void publish(Beacon::Phase phase, std::optional<Error> error = {});                                                                   // 只标记通知, 持锁不调用应用.
    void notify(std::unique_lock<std::mutex>& lock);                                                                                      // 取稳定状态后解锁调用, close 可重入.

    const std::shared_ptr<Core> core_;                     // 所有业务对象共享同一个控制循环/连接.
    const Scope scope_;                                    // 固定外部范围, 构造后不变.
    Value attr_;                                           // 固定属性, 不被 Data update 改写.
    const std::chrono::milliseconds ttl_;                  // 固定 1s..10m, 改 TTL 需要新 Beacon.
    mutable std::mutex mutex_;                             // 期望/身份/通知的短锁, 不跨网络或用户代码.
    mutable std::condition_variable condition_;            // 本地清理等待点, 不触发网络动作.
    std::shared_ptr<Pending> wanted_;                      // 当前唯一期望, 不创建发布 FIFO.
    bool applied_{};                                       // 最新期望已在当前 UUID 明确确认, 后台成功不重写旧 future.
    bool rejected_{};                                      // 当前 Data 永久拒绝, 新合法 update 可解除, 不停止原 UUID 续租.
    bool permanent_{};                                     // TTL/创建输入或协议永久错误, 不反复请求新 UUID.
    std::size_t bytes_{};                                  // 固定 Attr 与当前期望的受控拥有量.
    std::shared_ptr<const Binding> identity_;              // 当前已确认 UUID 所属端点, 用于同 Star 恢复和关闭.
    Lifetime lifetime_;                                    // 最近确认的保守本地预算, 使用 CLOCK_BOOTTIME.
    Beacon::State state_;                                  // 最后状态包装, 对外读取时重新核对预算.
    std::move_only_function<void(Beacon::State)> changed_; // 创建时固定, 不在通知执行时销毁.
    bool dirty_{};                                         // 合并状态通知, 不建立回调队列.
    bool notifying_{};                                     // 当前应用回调已开始, wait 必须等待它完成.
    std::atomic_bool closed_{};                            // 应用关闭门, 单向变化.
    std::atomic_bool finished_{};                          // 所有实际 RPC/本地清理结束后发布.
    std::shared_ptr<Beaming> retained_;                    // 应用析构后仅为有界清理保活, 完成即解除自持有.
    std::shared_ptr<Creating> creating_;                   // 取消至 done 之前也占用唯一 Create 位置.
    std::shared_ptr<Updating> updating_;                   // 不可修改旧请求的内容/顺序.
    std::shared_ptr<Renewing> renewing_;                   // 与 Data 独立的保活请求.
    std::shared_ptr<Removing> removing_;                   // 关闭时唯一尽力请求.
    std::uint64_t update_{};                               // 当前 UUID 已分配的最高 Data order, 耗尽时明确停止该值.
    std::uint64_t renewal_{};                              // 当前 UUID 已分配的最高 Renew order.
    std::optional<Lifetime::Time> renewal_sent_;           // 当前未确认续租 order 的首次发送起点.
    Core::Time create_at_{};                               // 下一次允许创建, 0 表示立即可试.
    Core::Time data_at_{};                                 // Data 单独退避, 不延迟保活.
    Core::Time renew_at_{};                                // 续租单独退避, 不被 Data 永久拒绝暂停.
    Core::Time close_at_{};                                // 关闭开始即固定期限, 不因重认证重置.
    bool removed_{};                                       // 尽力 Remove 只发送一次, 不无限重试清理.
    unsigned failures_{};                                  // 有界重试退避, 成功完整确认后再归零.
};
} // namespace comet::detail
