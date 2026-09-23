#pragma once
#include "comet.grpc.pb.h"
#include <atomic>
#include <comet/client.hpp>
#include <condition_variable>
#include <grpcpp/alarm.h>
#include <mutex>

namespace comet::detail {

// 一次已确认的公共接入绑定. 在途请求保有旧绑定, 不随 Client 换端点被自动改投.
struct Binding {
    std::string endpoint;                                         // 本次明确选择的 Star 地址, 不使用跨 Star round-robin.
    std::string instance;                                         // 登录确认的 Star ID, 匿名在首次合法业务响应前可未知.
    std::string session;                                          // 32 字节二进制令牌, 匿名时为空, 不输出日志.
    std::shared_ptr<grpc::Channel> streams;                       // Session/Watch 的有界长期流 Channel.
    std::shared_ptr<grpc::Channel> unary;                         // 使用独立 subchannel pool, 不与长期流争 HTTP/2 流名额.
    std::shared_ptr<proto::comet::v1::Catalog::Stub> catalog;     // 同一目标所有 Publisher 共用.
    std::shared_ptr<proto::comet::v1::Ephemeris::Stub> ephemeris; // 全部 Beacon 共用此固定传输 Stub, 不按心跳重建.
};

// 共享 Client 中实际存活的业务对象, 只表达清理与一次推进, 不提供任务队列或执行器.
class Activity {
public:
    virtual ~Activity() = default;                                                                                                                                                                     // gRPC 实际结束后才释放具体对象.
    virtual bool closed() const noexcept = 0;                                                                                                                                                          // 应用已停止拥有自动恢复意图.
    virtual bool finished() const noexcept = 0;                                                                                                                                                        // 本地清理和实际 RPC 都已经完成.
    virtual bool streaming() const noexcept = 0;                                                                                                                                                       // Watch 与自动写对象分别计量应用和实际 RPC 容量.
    virtual std::chrono::steady_clock::time_point poll(std::chrono::steady_clock::time_point now, const std::shared_ptr<const Binding>& binding, const std::optional<Error>& error, bool closing) = 0; // 唯一控制轮在 Core 锁外调用.

private:
    friend class Core;
    std::chrono::steady_clock::time_point next_{}; // 仅 Core 控制轮访问, 初始零确保首轮推进; 网络事件不改期限.
    std::weak_ptr<Activity> self_;                 // 接纳后才设置, 就绪队列借用同一控制块; 目录移除时清空, 拒绝迟到唤醒.
    bool ready_{};                                 // Core 锁下保证就绪队列每对象至多一项, 消费前清除, poll 中的新事件留到下一轮.
};

// Client 私有共享核心. 使用一个 gRPC Alarm 和现有 callback 线程, 不创建每对象定时线程或 Task 框架.
class Core : public std::enable_shared_from_this<Core> {
public:
    using Time = std::chrono::steady_clock::time_point; // 只用于本地调度, Comet 不读取 Pulsar.
    // 本地配置及信任材料校验, 不发出业务 RPC; 预期失败通过 Error 返回.
    static Result<std::shared_ptr<Core>> prepare(Client::Options options);
    // 首个应用 Owner 已准备后才调用, 只安排异步处理, 不等待连接.
    void start();
    // 新业务句柄接纳, 只有成功创建之后才增加应用拥有数和对象额度.
    Result<std::shared_ptr<Reading>> reader(Scope scope, std::string target, Reader::Options options);
    Result<std::shared_ptr<Subscribing>> subscriber(Scope scope, std::string target, Subscriber::Options options); // 固定 Catalog 范围/Key, 共用 Watch 容量.
    // Ephemeris 使用同一共享连接/预算, 具体投影只接受完整注册或已知 UUID 的 Data.
    Result<std::shared_ptr<Observing>> observer(Scope scope, std::string target, Observer::Options options);
    Result<std::shared_ptr<Publishing>> publisher(Scope scope, std::string key, std::chrono::milliseconds ttl, Publisher::Options options); // 固定 Catalog Key 的自动期望.
    Result<std::shared_ptr<Beaming>> beacon(Scope scope, Value attr, Value data, std::chrono::milliseconds ttl, Beacon::Options options);   // 本地验证和接纳后才调度.
    // 只替换今后认证的 SECRET, 当前已确认 Session 不因此失效; 未确认尝试需要取消后重登.
    Result<void> secret(std::vector<std::uint8_t> value);
    // 应用 Owner/业务句柄最后关闭时减少拥有数, 零时自动关闭内部生命周期.
    void release() noexcept;
    // 显式关闭所有业务和 Session, 不同步等待任意用户回调或网络.
    void close() noexcept;
    // 仅在外部线程等待所有本地工作完成, SDK 回调中调用抛 logic_error.
    bool wait(std::chrono::milliseconds timeout) const;
    // 网络或本地状态变化只唤醒当前 Alarm, 不并发启动第二个控制轮.
    void wake(Activity* activity = nullptr) noexcept; // 指定对象只唤醒自身; 空指针用于共享身份/容量/关闭变化.

    // 显式 Client 关闭立即阻止新通知开始, 不等待控制轮逐对象处理.
    bool stopped() const noexcept {
        return stopped_.load(std::memory_order_acquire);
    }

    // 仅当前共享会话/传输失败触发 Client 恢复, 局部版本落后或容量错误不切换全部对象.
    void lost(const std::shared_ptr<const Binding>& binding, Error error);
    // Watch 实际在途额度与应用句柄数分别核算, OnDone 之前不归还.
    bool claim() noexcept;
    void relinquish() noexcept; // 与每次成功 claim 精确配对一次.
    // Reading 受控内存从 previous 调整到 requested, 失败不改变计费; 不取得业务状态锁.
    bool resize(std::size_t previous, std::size_t requested) noexcept;
    // 用户回调边界标志, 同一线程的 SDK 等待必须被拒绝; 不强制回调来自固定线程.
    static bool notifying() noexcept;
    static void notify(bool active) noexcept;  // 由实际回调入口保存并恢复旧值.
    bool notification() const noexcept;        // 与显式关闭定序的通知开始点, 调用方已持 Reading 短锁.
    void exception() noexcept;                 // 只累计观察者异常数量, 不记录应用数据或异常正文.
    std::uint64_t exceptions() const noexcept; // 当前共享 Client 的观察者异常诊断计数.

private:
    std::atomic_bool stopped_{}; // 跨对象共享的停止门, close 单向发布, 不由晚到成功清除.
    friend class ::comet::Client;
    friend class Beaming;
    friend class Publishing;
    template <class Policy>
    friend class Watching;
    // 对象已在锁外准备且尚未对外发布, 成功才接纳一次应用拥有数.
    Result<void> accept(const std::shared_ptr<Activity>& activity);
    bool outgoing(bool priority, bool automatic) noexcept;  // 显式请求至多 256, 自动恢复 48, 续租/清理独占 16 个槽.
    void returning(bool priority, bool automatic) noexcept; // 实际最终 callback 对象释放时精确归还.
    bool admitting() noexcept;                              // 同时持有尚未完成 future 的显式调用至多 256, 含尚未发送.
    void settled() noexcept;                                // future 完成或候选回滚时归还一次.
    class Login;                                            // 单条真实 Session RPC, 在确认后继续读取结束/非法第二次确认.
    // 构造不安排计时器, 由 prepare 创建, 避免构造中 shared_from_this 或泄露半初始化对象.
    Core(Client::Options options, std::shared_ptr<grpc::ChannelCredentials> credentials, Value secret);
    // Alarm 的唯一推进入口, 先拿出共享状态再调用 Reading, 不在 Core 锁下调用业务对象/用户回调.
    void tick();
    Time session(Time now);                                        // 消费唯一 Session 的结果, 确认与关闭都不占用网络等待线程.
    Time dial(Time now);                                           // 仅在前一次 Session 真正完成后发起下一次, 返回实际重试期限.
    void recovered(const std::shared_ptr<const Binding>& binding); // 完整业务恢复才清除共享失败退避.
    // Set/Cancel 必须在 mutex_ 下严格定序, Alarm 本身不提供这些操作的线程安全.
    void schedule(Time time);
    Time connecting_until_{}; // 当前唯一端点的就绪截止, 与长期 Session 的确认后寿命分开.
    // 单个失败计数对应有界退避, 不因 TCP 短暂成功立刻清零持续失败.
    std::chrono::milliseconds delay(unsigned failures) const noexcept;
    // 冻结当前选定端点的两种 Channel, 普通重认证可以复用同端点底层连接.
    std::shared_ptr<Binding> connect() const;
    // 生成的失败细节只解析白名单字段, 不回显远端任意文本或未知结果为成功.
    static Error failure(const grpc::Status& status, const grpc::ClientContext& context);
    const Client::Options options_;                               // 固定端点/APIKEY/TLS/容量, SECRET 从单独不可变值读取.
    const std::shared_ptr<grpc::ChannelCredentials> credentials_; // 独立客户端信任, 不包含服务端私钥.
    mutable std::mutex mutex_;                                    // 保护生命周期与计时器, 不跨 gRPC 网络等待或用户代码.
    mutable std::condition_variable condition_;                   // 本地清理等待, 不用于延长操作 deadline.
    grpc::Alarm alarm_;                                           // 一个共享计时资源, 闭包归还前 Core 保持存活.
    bool scheduled_{};                                            // Alarm 已 Set, 触发/取消回调取得后清除.
    bool running_{};                                              // 已有控制轮执行, wake 只合并通知.
    bool awakened_{};                                             // 控制轮执行期间的新事件, 下一轮立即重新检查.
    bool refresh_ = true;                                         // 共享绑定/额度变化要求全部对象复核, 普通单对象完成不设置它.
    bool closing_{};                                              // 单向停止接纳, 不由 secret 或晚到成功复活.
    bool complete_{};                                             // 所有真实 RPC 和本地通知结束, wait 的完成条件.
    std::size_t owners_ = 1;                                      // 初始 Client 应用 Owner, 内部 shared_ptr 不增加此数.
    std::vector<std::weak_ptr<Activity>> readers_;                // 不与具体业务对象 -> Core 形成引用环, 仅在控制轮作类型擦除.
    std::vector<std::weak_ptr<Activity>> ready_;                  // 定向事件队列, 接纳时预留目录容量, noexcept wake 不分配; 不延长对象寿命.
    Time due_ = Time::max();                                      // 仅控制轮访问的活动期限保守最小值, 只允许早醒; 到期或共享变化才扫描目录.
    std::vector<std::shared_ptr<Activity>> polling_;              // 唯一控制轮复用容量, 每轮含异常退出均清空强引用, 不跨轮形成拥有环.
    std::shared_ptr<Login> login_;                                // 包括取消尚未 OnDone 的旧会话, 不提前开始第二次认证.
    std::shared_ptr<Binding> connecting_;                         // 当前端点的未确认传输, 不直接交给认证业务.
    std::shared_ptr<const Binding> binding_;                      // 最后完整认证的不可变绑定, 失效后立即撤销新请求使用.
    Value secret_;                                                // 当前完整 SECRET, 新值不改写旧 Login 已复制的请求.
    std::optional<Error> blocked_;                                // 明确认证/协议错误时暂停, 用户更新 SECRET 可解除认证暂停.
    std::size_t endpoint_{};                                      // 当前顺序端点索引, 不在多个 Star 间逐 RPC 负载均衡.
    unsigned failures_{};                                         // 连续连接/认证传输失败次数, 有界累积.
    Time retry_{};                                                // 下一次准入尝试的单调期限.
    std::atomic_size_t calls_{};                                  // 实际未 OnDone 的 Watch 数, 独立于应用对象额度.
    std::atomic_size_t unary_{};                                  // 实际未释放普通写请求, 默认零.
    std::atomic_size_t admitted_{};                               // 待发和在途显式调用总数, 不含已完成 future 的后台恢复.
    std::atomic_size_t recovery_{};                               // 自动 Create/Publish/Data 实际在途, 至多 48.
    std::atomic_size_t maintenance_{};                            // 实际未释放 Renew/Remove, 默认零.
    std::atomic_size_t bytes_{};                                  // Reading 受控存储总量, 应用额外持有的旧 View 不可强制回收.
    std::atomic_uint64_t exceptions_{};                           // 用户观察者抛出的异常, 饱和累计, 默认 0.
};
} // namespace comet::detail
