#pragma once
#include "types.hpp"
#include <functional>
#include <future>
#include <span>

namespace comet {
namespace detail {
class Publishing;
}
class Client;

// 固定 Catalog Scope/Key/TTL, 只保存最新完整期望, 内容及版本的持久化由应用负责.
class Publisher {
public:
    // 状态只描述本对象最近观察, 不保证其他 Star 已收到内容.
    enum class Phase {
        waiting,   // 未接纳内容或尚未建立首个确认.
        ready,     // 当前期望在目标确认且本地保守租约仍有效.
        uncertain, // 保留期望但没有当前有效确认, 自动有界恢复.
        failed,    // 当前期望被永久拒绝, 等待合法新 publish 或关闭.
        closed     // 停止发布/续租, 远端由原 TTL 自行清理.
    };

    struct Receipt {
        std::string instance;    // 本次实际确认的 Star, 不宣称全网提交.
        std::uint64_t version{}; // 本次正业务内容版本, 从不由 SDK 自动加号.
    };

    struct State {
        Phase phase = Phase::waiting;     // 初始尚无内容/确认.
        std::optional<Receipt> confirmed; // 最后确认, ready 以外不能据此宣称当前有效.
        std::optional<Error> error;       // 有界原因, 不含业务正文或凭据.
    };

    struct Options {
        std::move_only_function<void(State)> changed; // 锁外串行通知, 应快速返回, 允许非阻塞 close.
    };

    Publisher() = default;                      // 空句柄, 不接入网络.
    Publisher(Publisher&&) noexcept;            // 移交唯一自动发布责任.
    Publisher& operator=(Publisher&&) noexcept; // 先关闭旧对象, 再移交.
    Publisher(const Publisher&) = delete;       // 不复制后台业务拥有权.
    Publisher& operator=(const Publisher&) = delete;
    ~Publisher();                                                                                                                                                  // 非阻塞停止, 不等待网络.
    State state() const;                                                                                                                                           // 按包含系统休眠的本地预算重新判断可用性.
    std::future<Result<Receipt>> publish(std::uint64_t version, std::vector<std::uint8_t> value, std::chrono::milliseconds timeout = std::chrono::seconds(3));     // 转移拥有正文.
    std::future<Result<Receipt>> publish(std::uint64_t version, std::span<const std::uint8_t> value, std::chrono::milliseconds timeout = std::chrono::seconds(3)); // 复制借用正文.
    std::future<Result<Receipt>> publish(std::uint64_t version, Value value, std::chrono::milliseconds timeout = std::chrono::seconds(3));                         // 共享不可变正文.
    void close() noexcept;                                                                                                                                         // 停止后续恢复, 不发送不存在的 Catalog Delete.
    bool wait(std::chrono::milliseconds timeout) const;                                                                                                            // 等待真实 RPC 清理, SDK 回调内禁止阻塞.

private:
    friend class Client;
    explicit Publisher(std::shared_ptr<detail::Publishing> publishing); // 仅工厂完成本地接纳后构造.
    std::shared_ptr<detail::Publishing> publishing_;                    // 旧 future/View 不拥有自动发布意图.
};
} // namespace comet
