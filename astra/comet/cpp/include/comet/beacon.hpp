#pragma once
#include "types.hpp"
#include <functional>
#include <future>
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
        std::string instance;                             // 接纳此 UUID 的 Star, 切换实例必须重新 Create.
        std::string uuid;                                 // Star 签发的规范小写 UUIDv4, 未确认时不对外提供 Identity.
        bool operator==(const Identity&) const = default; // 只比较身份, 不包含 Session 或 TCP 连接.
    };

    struct Receipt {
        Identity identity;     // 本次 Data 实际请求的固定身份, 不随之后重注册改变.
        std::uint64_t order{}; // 成功的正 Data 顺序, 不表示续租或集群持久成功.
    };
    enum class Phase {
        waiting,   // 尚未确认 UUID, 创建或重注册仍在推进.
        ready,     // 已确认身份, 本地保守租约预算仍有余量.
        uncertain, // 原身份仍可能存活, 但本地预算或网络不足以确认.
        failed,    // 配置/协议等永久问题使自动创建暂停.
        closed     // 应用已停止自动恢复, 远端删除仍是有界尽力动作.
    };

    struct State {
        Phase phase = Phase::waiting;     // 取快照时的状态, 不声称实时远端存活.
        std::optional<Identity> identity; // 只保存已确认的实际身份.
        std::optional<Error> error;       // 有界最近原因, 不包含载荷或登录秘密.
    };

    struct Options {
        std::move_only_function<void(State)> changed; // 身份/状态变化时在 SDK 锁外串行通知, 必须快速返回.
    };

    Beacon() = default;                   // 空句柄不创建网络或自动任务.
    Beacon(Beacon&&) noexcept;            // 单独移交自动续租责任.
    Beacon& operator=(Beacon&&) noexcept; // 非阻塞关闭原对象再移交.
    Beacon(const Beacon&) = delete;       // 同一自动注册意图只有一个应用句柄.
    Beacon& operator=(const Beacon&) = delete;
    ~Beacon();           // 非阻塞幂等 close, 不在析构中等待网络.
    State state() const; // 重查本地休眠可见的租约预算, 不调用 Pulsar 或业务 RPC.
    // 接纳最新 Data, 未发送值可被新值替代; 已发调用只完成一次固定结果, 超时不撤回期望.
    std::future<Result<Receipt>> update(std::vector<std::uint8_t> data, std::chrono::milliseconds timeout = std::chrono::seconds(3));
    std::future<Result<Receipt>> update(std::span<const std::uint8_t> data, std::chrono::milliseconds timeout = std::chrono::seconds(3)); // 同步取得拥有副本.
    std::future<Result<Receipt>> update(Value data, std::chrono::milliseconds timeout = std::chrono::seconds(3));                         // 调用方保证无可写别名, 空指针非法.
    void close() noexcept;                                                                                                                // 停止续租/重注册, 有界尽力注销, 不保证远端即时删除.
    bool wait(std::chrono::milliseconds timeout) const;                                                                                   // 等待本地实际清理, SDK 回调中拒绝阻塞.

private:
    friend class Client;
    explicit Beacon(std::shared_ptr<detail::Beaming> beaming); // 仅由接纳成功的工厂构造.
    std::shared_ptr<detail::Beaming> beaming_;                 // 活动状态与 public 句柄分离, future 不维持自动注册.
};
} // namespace comet
