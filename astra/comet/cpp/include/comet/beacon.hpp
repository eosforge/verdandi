#pragma once
#include "types.hpp"
#include <functional>
#include <span>

namespace comet {
namespace detail {
class Beaming;
}
class Client;

// 一个自动注册生命周期, 固定 Attr/TTL, 共享 Client, 不接管应用传入的旧 UUID.
class Beacon {
public:
    struct Identity {
        Scope scope;                                      // 本对象固定分组, 不含内部 __ 范围.
        std::string instance;                             // 接纳此 UUID 的 Star, 切换实例按同一逻辑 UUID 恢复注册.
        std::string uuid;                                 // Star 签发的 16 字节原始 UUIDv4, 可含 NUL, 未确认时不提供 Identity.
        bool operator==(const Identity&) const = default; // 只比较身份, 不包含 Session 或 TCP 连接.
    };

    struct Receipt {
        Identity identity;     // 本次 Data 实际请求的固定身份, 不随之后重注册改变.
        std::uint64_t order{}; // 成功的正 Data 顺序, 不表示续租或集群持久成功.
    };
    enum class Phase {
        recovering, // 正在恢复同一逻辑 ID, 不接纳新 Data 更新.
        waiting,    // 尚未确认 UUID, 创建或重注册仍在推进.
        ready,      // 已确认身份, 本地保守租约预算仍有余量.
        uncertain,  // 原身份仍可能存活, 但本地预算或网络不足以确认.
        failed,     // 配置/协议等永久问题使自动创建暂停.
        closed      // 应用已停止自动恢复, 远端删除仍是有界尽力动作.
    };

    struct State {
        Phase phase = Phase::waiting;     // 取快照时的状态, 不声称实时远端存活.
        std::optional<Identity> identity; // 只保存已确认的实际身份.
        std::optional<Error> error;       // 有界最近原因, 不包含载荷或登录秘密.
    };

    struct Options {
        std::chrono::milliseconds timeout{3000};      // 首次注册覆盖连接、认证与 RPC 的总期限, 必须在 (0, 1 min].
        std::move_only_function<void(State)> changed; // 身份/状态变化时在 SDK 锁外串行通知, 必须快速返回.
    };

    Beacon() = default;                   // 空句柄不创建网络或自动任务.
    Beacon(Beacon&&) noexcept;            // 单独移交自动续租责任.
    Beacon& operator=(Beacon&&) noexcept; // 非阻塞关闭原对象再移交.
    Beacon(const Beacon&) = delete;       // 同一自动注册意图只有一个应用句柄.
    Beacon& operator=(const Beacon&) = delete;
    ~Beacon();           // 非阻塞幂等 close, 不在析构中等待网络.
    State state() const; // 重查本地休眠可见的租约预算, 不调用 Pulsar 或业务 RPC.
    // 同步提交一个完整 Data, 同对象在途返回 busy; 失败不补发且不覆盖已确认恢复缓存.
    Result<Receipt> update(std::vector<std::uint8_t> data, std::chrono::milliseconds timeout = std::chrono::seconds(3));
    Result<Receipt> update(std::span<const std::uint8_t> data, std::chrono::milliseconds timeout = std::chrono::seconds(3)); // 同步取得拥有副本.
    Result<Receipt> update(Value data, std::chrono::milliseconds timeout = std::chrono::seconds(3));                         // 调用方保证无可写别名, 空指针非法.
    Result<void> tick(std::chrono::milliseconds interval, std::move_only_function<Value()> callback);                        // 设置或替换串行采样, interval > 0, 空 callback 解除, 不补历史 tick.
    Result<void> changed(std::move_only_function<void(State)> callback);                                                     // 替换状态通知, 首次安排当前状态, 空回调解除.
    void destroy() noexcept;                                                                                                 // 立即逻辑关闭, 有界尽力 Remove.
    void close() noexcept;                                                                                                   // 停止续租/重注册, 有界尽力注销, 不保证远端即时删除.
    bool wait(std::chrono::milliseconds timeout) const;                                                                      // 等待本地实际清理, SDK 回调中拒绝阻塞.

private:
    friend class Client;
    explicit Beacon(std::shared_ptr<detail::Beaming> beaming); // 仅由接纳成功的工厂构造.
    std::shared_ptr<detail::Beaming> beaming_;                 // 活动状态与 public 句柄分离, 已交付的身份和载荷不维持自动注册.
};
} // namespace comet
