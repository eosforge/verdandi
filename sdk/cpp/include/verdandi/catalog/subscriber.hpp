#pragma once

#include "verdandi/catalog/client.hpp"
#include "verdandi/catalog/publisher.hpp"
#include "verdandi/schema.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace verdandi::catalog {

/// Entry 的权威同步状态。
enum class status : std::uint8_t {
    synchronizing,
    present,
    absent,
    deleted,
    unavailable,
    closed,
};

/// 选择整个 Zone、若干 Part、若干精确 Path，或它们的非空组合。
struct subscription {
    bool zone{false};
    std::vector<std::string> parts;
    std::vector<path> paths;
};

namespace detail {
struct entry_state {
    std::uint64_t revision{};
    std::uint64_t replace_revision{};
    status state{status::synchronizing};
    kind shape{kind::value};
    std::size_t encoded_bytes{};
    fields value;
};

class subscriber_core;
} // namespace detail

/// 一次从稳定 Entry 原子状态脱离的强类型读取。
template <structured_value Value>
struct snapshot {
    std::uint64_t revision{};
    status state{status::synchronizing};
    bool synchronized{false};
    std::optional<Value> value;
};

/// 在 Subscriber 生命周期内始终绑定同一 Path 的稳定本地对象。
class entry final {
public:
    entry(const entry&) = delete;
    entry& operator=(const entry&) = delete;

    /// 返回不可变 Path。
    [[nodiscard]] const path& target() const noexcept;

    /// 返回当前本地状态。
    [[nodiscard]] status state() const noexcept;

    /// 返回最后已知完整 revision；尚无完整结果时为零。
    [[nodiscard]] std::uint64_t revision() const noexcept;

    /// 报告状态是否为 Present、Absent 或 Deleted。
    [[nodiscard]] bool synchronized() const noexcept;

    /// 从同一个不可变状态解码完整强类型值；不执行 Redis 或磁盘 I/O。
    template <structured_value Value>
    [[nodiscard]] result<snapshot<Value>> load() const {
        auto current = state_.load(std::memory_order_acquire);
        if (!current) {
            return std::unexpected(error(code::unavailable, "entry"));
        }
        snapshot<Value> output{current->revision, current->state, synchronized_state(current->state), std::nullopt};
        if (!current->value.empty() || current->state == status::present) {
            auto decoded = decode_value<Value>(current->value);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            output.value = std::move(*decoded);
        }
        return output;
    }

private:
    entry(path target, status initial);

    [[nodiscard]] static bool synchronized_state(status value) noexcept;

    path target_;
    std::atomic<std::shared_ptr<const detail::entry_state>> state_;

    friend class detail::subscriber_core;
};

namespace detail {
[[nodiscard]] result<std::shared_ptr<subscriber_core>> create_subscriber(const std::shared_ptr<client_core>& owner, subscription value);
[[nodiscard]] std::shared_ptr<entry> subscriber_find(const std::shared_ptr<subscriber_core>& value, const path& target);
[[nodiscard]] result<void> subscriber_close(const std::shared_ptr<subscriber_core>& value);
[[nodiscard]] std::optional<error> subscriber_error(const std::shared_ptr<subscriber_core>& value);
} // namespace detail

/// 一个常驻 Pub/Sub 监听任务和至多一个临时权威同步任务组成的本地 Catalog 视图。
class subscriber final {
public:
    subscriber() noexcept = default;
    subscriber(const subscriber&) = delete;
    subscriber& operator=(const subscriber&) = delete;
    subscriber(subscriber&&) = delete;
    subscriber& operator=(subscriber&&) = delete;
    ~subscriber();

    /// 完成订阅确认、权威同步和同连接栅栏后返回。
    [[nodiscard]] static result<std::unique_ptr<subscriber>> create(const client& owner, subscription value);

    /// 返回覆盖范围内稳定 Entry；范围外、无效 Path 或关闭后返回空指针。
    [[nodiscard]] std::shared_ptr<entry> find(const path& target) const;

    /// 非阻塞取得一条同步、恢复、检查点或协议诊断。
    [[nodiscard]] std::optional<error> try_error();

    /// 关闭常驻监听和当前临时同步任务；不删除 Redis Catalog 数据。
    [[nodiscard]] result<void> close();

private:
    explicit subscriber(std::shared_ptr<detail::subscriber_core> core) noexcept : core_(std::move(core)) {}

    std::shared_ptr<detail::subscriber_core> core_;
};

} // namespace verdandi::catalog
