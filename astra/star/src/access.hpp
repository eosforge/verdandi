#pragma once
#include "almanac.hpp"
#include <functional>
#include <map>
#include <shared_mutex>
#include <span>
#include <stop_token>
#include <unordered_map>

namespace astra {
// 业务登录索引与逻辑会话. 只由 Receiver 在内部凭据提交边界更新, 不维护权限或空闲扫描.
// 锁顺序为 Access -> Library -> Almanac; 业务写入必须持有 Permit 直到实际提交完成.
class Access {
    // 凭据快照的共同有效性, 完整替换时 O(1) 使所有旧会话失效, 取消通知在锁外传播.
    struct Group;
    // 一个 APIKEY 的已解析 SECRET 和共同有效性, 同值连续 Set 保留原对象.
    struct Account;

public:
    // 登录失败分类, 不携带 APIKEY、SECRET 或随机会话正文.
    enum class Error {
        // 凭据或令牌不合法、已删除、已轮换或对应流已结束.
        denied,
        // 活动令牌达到配置容量, 不替换已有会话.
        capacity,
        // 系统随机源失败或有界碰撞重试耗尽.
        random
    };

    // 一条真实 Session 流持有的会话, token 只用于后续 RPC metadata, 不能独立延长流寿命.
    class Session {
    public:
        // 解除内部停止转发, 可在 Gateway 的 OnDone 之后销毁, 不访问 Access 所有者.
        ~Session() = default;
        // 返回固定 32 字节令牌的借用视图, 仅在本对象存活时有效, 不作文本编码.
        std::string_view token() const noexcept;
        // 供实际流注册取消回调; 轮换、快照替换和 close 都会请求停止, 回调必须短小且不抛异常.
        std::stop_token stopped() const noexcept;

    private:
        friend class Access;

        // 将一项内部失效广播转换为本会话的取消信号, 不捕获 Session 地址.
        struct Relay {
            // 单个 Session 的共享停止状态, 按值保持寿命, 不拥有服务对象.
            std::stop_source target;
            // 只传播取消, 不取得 Access 或业务存储锁.
            void operator()() const noexcept;
        };

        // 只由 Access 创建, account/group 必须来自同一已安装索引, token 已由系统随机源生成.
        Session(std::string token, std::shared_ptr<Account> account, std::shared_ptr<Group> group);
        // 原始随机令牌, 安装后不可修改.
        const std::string token_;
        // 保持该 APIKEY 当时的凭据版本, 不借用索引节点.
        const std::shared_ptr<Account> account_;
        // 保持创建时的完整快照有效性, 不使用公开认证代次.
        const std::shared_ptr<Group> group_;
        // 本逻辑流的取消状态, 内部失效广播和显式 close 共同触发.
        std::stop_source stop_;
        // Account 失效转发, 析构先于 account_, 避免悬垂访问.
        std::stop_callback<Relay> account_stop_;
        // 完整快照失效转发, 析构先于 group_.
        std::stop_callback<Relay> group_stop_;
    };

    // 一次业务最终校验与提交的共享锁守卫. 不可保留到网络等待或外部回调期间.
    class Permit {
    public:
        // 只转移同一次提交许可, 不复制锁所有权.
        Permit(Permit&&) noexcept = default;
        // 禁止复制共享锁守卫.
        Permit(const Permit&) = delete;
        // 会话关闭/凭据撤销的通知令牌, 可在许可释放后保有; 不延长授权有效期.
        std::stop_token stopped() const noexcept;

    private:
        friend class Access;
        // 成功校验后接管已持有的共享锁, 锁解开后不再保证后续业务提交有效.
        explicit Permit(std::shared_lock<std::shared_mutex> lock, std::stop_token stopped);
        // 覆盖业务提交边界, 与凭据替换和 Session 结束的独占锁定序.
        std::shared_lock<std::shared_mutex> lock_;
        // 对应实际逻辑 Session, 没有空闲扫描或额外租期.
        std::stop_token stopped_;
    };

    // 完整凭据索引的锁外准备对象, 与 Receiver 的私有 Almanac Draft 同时丢弃或安装.
    class Draft {
    public:
        // 创建空的新有效性分组, 不改变服务当前会话.
        Draft();
        // 单个恢复任务可以移交准备所有权.
        Draft(Draft&&) noexcept = default;
        // 不复制可写凭据索引.
        Draft(const Draft&) = delete;
        // 接管已解析 SECRET, key 为 1..128 UTF-8 字节, secret 为 1..4096 字节, 重复项拒绝.
        bool set(std::string key, Almanac::Buffer secret);

    private:
        friend class Access;
        // 准备时独占, 成功安装后不再使用该 Draft.
        std::shared_ptr<Group> group_;
        // 解析后的登录查找索引, 至多 65536 项; 载荷上限仍由 Almanac 的完整准备预算约束.
        std::map<std::string, std::shared_ptr<Account>, std::less<>> accounts_;
    };

    // 安装回调只运行一次, 返回 true 表示新提交, false 表示重放; 异常或错误不能已提交半份状态.
    using Commit = std::move_only_function<std::expected<bool, Almanac::Error>()>;
    // 创建空凭据表, maximum 为 1..65536 个活动 Session, 默认 4096; 不创建线程或监听.
    explicit Access(std::size_t maximum = 4096);
    // 拥有者先排空业务调用; 析构使仍被外部持有的 Session 失效并停止其通知源.
    ~Access();
    // 接管完整索引, 与 commit 中的 Almanac 安装定序; 新版本撤销全部旧会话, 重放不撤销.
    std::expected<bool, Almanac::Error> reset(Draft&& draft, Commit commit);
    // 连续单 Key 凭据提交, secret 为空表示 Delete; 同值 Set 不撤销, 分配均先于 commit.
    std::expected<bool, Almanac::Error> apply(std::string key, std::optional<Almanac::Buffer> secret, Commit commit);
    // 登录只比较一次 SECRET; 随机令牌有界重试, 成功后由实际 RPC 的 OnDone 调 close.
    std::expected<std::shared_ptr<Session>, Error> open(std::string_view key, std::span<const std::uint8_t> secret);
    // 验证令牌及当前凭据有效性, 成功返回覆盖最终提交的守卫; 不重新比较 SECRET.
    std::expected<Permit, Error> enter(std::string_view token) const;
    // 逻辑流结束时先移除授权再在锁外发停止通知; 重复关闭无副作用, 不影响新会话.
    void close(const std::shared_ptr<Session>& session);

private:
    // 透明令牌散列, 查找借用的 32 字节 metadata 时不创建临时字符串.
    struct Hash {
        // 开启 std::unordered_map 的异构查找.
        using is_transparent = void;
        // 同一字节内容采用统一 string_view 散列, 不作编码转换.
        std::size_t operator()(std::string_view value) const noexcept;
    };

    // 键值的共同边界检查, 只用于登录/凭据准备, 不进入已登录 RPC 热路径.
    static bool valid(std::string_view key, std::span<const std::uint8_t> secret) noexcept;
    // 长度相同的 SECRET 采用 BoringSSL 常数时间比较, 不引入系统 OpenSSL.
    static bool equal(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) noexcept;
    // 活动会话上限, 尚在 gRPC 认证阶段的额外占位由 Gateway 同步限制.
    const std::size_t maximum_;
    // 只保护索引与授权状态, 取消广播与资源回收必须在锁外执行.
    mutable std::shared_mutex mutex_;
    // 当前完整凭据快照组, 初始为空但有效, 无 APIKEY 可登录.
    std::shared_ptr<Group> group_;
    // 只存已验证的凭据, 与内部 Almanac 在同一提交边界更新.
    std::map<std::string, std::shared_ptr<Account>, std::less<>> accounts_;
    // 令牌到会话的直接索引; 已撤销但未 OnDone 的条目仍计入容量, 防止取消积压绕过限额.
    std::unordered_map<std::string, std::shared_ptr<Session>, Hash, std::equal_to<>> sessions_;
};
} // namespace astra
