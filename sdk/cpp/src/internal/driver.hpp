#pragma once

#include "verdandi/client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace verdandi::detail {

/// 一个拥有全部二进制参数的 Redis 命令；参数顺序保持调用方给出的协议 ABI。
struct command {
    std::vector<std::string> arguments;

    /// 用非空 Redis `name` 创建命令。
    explicit command(std::string_view name);

    /// 追加一个可包含零字节的字符串参数并返回自身。
    command& add(std::string_view value);

    /// 追加一个可包含零字节的二进制参数并返回自身。
    command& add(std::span<const std::byte> value);

    /// 追加规范十进制无符号整数并返回自身。
    command& add(std::uint64_t value);
};

/// Verdandi 需要的 RESP3 拥有型树节点；完全隔离 Boost.Redis 类型。
struct response {
    enum class kind : std::uint8_t {
        null,
        string,
        number,
        boolean,
        array,
        map,
        set,
        push,
    };

    kind type{kind::null};
    std::string value;
    std::vector<response> children;

    /// 返回字符串、数字或布尔叶节点的原始 RESP 文本；类型不匹配时返回 `corrupt`。
    [[nodiscard]] result<std::string_view> text() const;
};

/// 专用 Pub/Sub 连接传给域监听任务的有界队列项。
struct subscription_item {
    enum class kind : std::uint8_t {
        message,
        reconnected,
        lagged,
        fence,
        idle,
        failure,
        closed,
    };

    kind type{kind::message};
    std::string channel;
    std::string payload;
    std::optional<std::string> pattern;
    std::uint64_t fence_id{};
    std::optional<error> failure;
};

class subscription;

/// 根 Client 的私有 Boost.Redis 连接池、I/O reactor 与专用订阅连接工厂。
class driver final : public std::enable_shared_from_this<driver> {
public:
    driver(const driver&) = delete;
    driver& operator=(const driver&) = delete;
    ~driver() noexcept;

    /// 校验并构造驱动、启动最少连接数和 reactor，再以 PING 证明可用。
    [[nodiscard]] static result<std::shared_ptr<driver>> open(const redis_configuration& configuration);

    /// 执行一个命令并返回唯一 RESP 根；`mutation` 使不确定传输失败保守映射为 `ambiguous`。
    [[nodiscard]] result<response> execute(const command& value, bool mutation = false);

    /// 在一次 Boost.Redis request 中发送多个命令并按原顺序返回 RESP 根。
    [[nodiscard]] result<std::vector<response>> execute(std::span<const command> values, bool mutation = false);

    /// 创建一个使用同一拓扑/ACL/TLS 但独占物理连接的 Pub/Sub 订阅。
    [[nodiscard]] result<std::shared_ptr<subscription>> subscribe(std::vector<std::string> channels, std::vector<std::string> patterns, std::size_t capacity);

    /// 终止订阅、连接池和 reactor 并汇合线程；幂等。
    [[nodiscard]] result<void> close() noexcept;

    /// 返回驱动是否仍接纳新请求。
    [[nodiscard]] bool open() const noexcept;

    /// 返回普通命令超时。
    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept;

    /// 返回只读原生配置，供子域构造独立连接或恢复退避。
    [[nodiscard]] const redis_configuration& configuration() const noexcept;

private:
    struct implementation;

    explicit driver(std::unique_ptr<implementation> implementation) noexcept;

    std::unique_ptr<implementation> implementation_;
};

/// 一个自动重订阅的专用 Pub/Sub 连接及其有界交接队列。
class subscription final : public std::enable_shared_from_this<subscription> {
public:
    subscription(const subscription&) = delete;
    subscription& operator=(const subscription&) = delete;
    ~subscription() noexcept;

    /// 等待下一条消息、重连、丢失、栅栏、错误或关闭项；`stop` 可取消本地等待。
    [[nodiscard]] subscription_item next(const std::stop_token& stop);

    /// 在 `wait` 内没有队列项时返回 idle，使领域监听任务可以驱动租约和时钟工作。
    [[nodiscard]] subscription_item next(const std::stop_token& stop, std::chrono::milliseconds wait);

    /// 在订阅连接执行有界 PING，并把同序栅栏项排到此前已解析消息之后。
    [[nodiscard]] result<std::uint64_t> fence();

    /// 取消底层连接、唤醒监听任务并等待接收 coroutine 退出；幂等。
    [[nodiscard]] result<void> close() noexcept;

private:
    struct implementation;

    explicit subscription(std::shared_ptr<driver> owner, std::unique_ptr<implementation> implementation) noexcept;

    // 订阅持有根驱动，保证 implementation_ 中的 io_context 引用始终有效。
    std::shared_ptr<driver> owner_;
    std::unique_ptr<implementation> implementation_;

    friend class driver;
};

/// 只允许 Verdandi 子域从公共 Client 取得私有 driver，不扩大应用可见 API。
struct driver_access {
    /// 返回 `value` 共享的 driver；默认 Client 返回空指针。
    [[nodiscard]] static std::shared_ptr<driver> get(const client& value) noexcept;
};

/// 把 RESP 十进制 `value` 严格解析为安全 revision；零是否允许由调用方决定。
[[nodiscard]] result<std::uint64_t> parse_unsigned(std::string_view value, std::string_view field, bool allow_zero = false);

/// 从交替 key/value 数组建立名称到文本值的只读视图；重复、奇数或嵌套值返回 `corrupt`。
[[nodiscard]] result<std::vector<std::pair<std::string_view, std::string_view>>> named_pairs(const response& value);

} // namespace verdandi::detail
