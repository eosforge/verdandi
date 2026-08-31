#pragma once

#include "verdandi/configuration.hpp"
#include "verdandi/schema.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace verdandi {

namespace detail {
class driver;
struct driver_access;
} // namespace detail

class key_commands;
class hash_commands;

/// 共享一个 Redis 传输池和 I/O reactor 的轻量拥有型句柄。
///
/// 复制句柄不会新建连接；任一副本执行 `close()` 都会终止同一传输。Registration 与 Catalog
/// 子域拥有独立 Zone 和任务，但复用此处的连接、ACL、TLS、超时与 Sentinel 解析。
class client final {
public:
    client() noexcept = default;
    client(const client&) noexcept = default;
    client(client&&) noexcept = default;
    client& operator=(const client&) noexcept = default;
    client& operator=(client&&) noexcept = default;
    ~client() = default;

    /// 校验 `configuration`、启动私有连接池并在命令超时内完成一次 PING。
    [[nodiscard]] static result<client> open(const redis_configuration& configuration);

    /// 返回根字符串 Key 命令薄封装；默认构造或已关闭 Client 的调用返回 `closed`。
    [[nodiscard]] key_commands key() const noexcept;

    /// 返回根 Redis Hash 命令薄封装；默认构造或已关闭 Client 的调用返回 `closed`。
    [[nodiscard]] hash_commands hash() const noexcept;

    /// 使用根命令超时执行一次 PING，用于显式健康检查。
    [[nodiscard]] result<void> ping() const;

    /// 终止共享传输、全部底层连接和 I/O reactor；幂等且不会删除 Redis 数据。
    [[nodiscard]] result<void> close() const;

    /// 返回 Client 是否仍可接纳新工作；网络暂时中断但正在重连时仍为 true。
    [[nodiscard]] bool open() const noexcept;

    /// 返回规范化后的普通 Redis 命令总等待上限。
    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept;

private:
    explicit client(std::shared_ptr<detail::driver> driver) noexcept;

    std::shared_ptr<detail::driver> driver_;

    friend class key_commands;
    friend class hash_commands;
    friend struct detail::driver_access;
};

/// 根 Client 上的二进制字符串 Key 命令集合。
class key_commands final {
public:
    /// 读取 `key` 的完整二进制值；键不存在返回空 optional。
    [[nodiscard]] result<std::optional<bytes>> load(std::string_view key) const;

    /// 编码 `T` 并读取 `key`；键不存在返回空 optional，解码失败不改变 Redis。
    template <field_scalar T>
    [[nodiscard]] result<std::optional<T>> get(const std::string_view key) const {
        auto loaded = load(key);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (!*loaded) {
            return std::optional<T>{};
        }
        auto decoded = field_codec<T>::decode(**loaded);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        return std::optional<T>(std::move(*decoded));
    }

    /// 无 TTL 覆盖写入 `key` 的二进制 `value`。
    [[nodiscard]] result<void> store(std::string_view key, std::span<const std::byte> value) const;

    /// 编码 `value` 并无 TTL 覆盖写入 `key`。
    template <field_scalar T>
    [[nodiscard]] result<void> set(const std::string_view key, const T& value) const {
        auto encoded = field_codec<T>::encode(value);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return store(key, *encoded);
    }

    /// 以精确毫秒 `ttl` 覆盖写入二进制值；TTL 必须大于零。
    [[nodiscard]] result<void> store(std::string_view key, std::span<const std::byte> value, std::chrono::milliseconds ttl) const;

    /// 编码 `value` 并以精确毫秒 `ttl` 覆盖写入。
    template <field_scalar T>
    [[nodiscard]] result<void> set(const std::string_view key, const T& value, const std::chrono::milliseconds ttl) const {
        auto encoded = field_codec<T>::encode(value);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return store(key, *encoded, ttl);
    }

    /// 删除整个 `key`；返回删除前是否存在。
    [[nodiscard]] result<bool> erase(std::string_view key) const;

    /// 判断 `key` 当前是否存在。
    [[nodiscard]] result<bool> contains(std::string_view key) const;

    /// 为已存在 `key` 设置精确毫秒 TTL；返回是否实际设置。
    [[nodiscard]] result<bool> expire(std::string_view key, std::chrono::milliseconds ttl) const;

private:
    explicit key_commands(std::shared_ptr<detail::driver> driver) noexcept;

    std::shared_ptr<detail::driver> driver_;

    friend class client;
};

/// 根 Client 上的完整二进制 Hash 命令集合。
class hash_commands final {
public:
    /// 读取 `key` 的完整字段集合；不存在的 Hash 返回空 Fields。
    [[nodiscard]] result<fields> load(std::string_view key) const;

    /// 读取完整 Fields 并解码为具有固定 Schema 的 `T`。
    template <field_value T>
    [[nodiscard]] result<T> get(const std::string_view key) const {
        auto loaded = load(key);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        return decode_fields<T>(*loaded);
    }

    /// 用一个 HSET 原子地写入 `value` 中的全部字段；空 Fields 被拒绝。
    [[nodiscard]] result<void> store(std::string_view key, const fields& value) const;

    /// 编码具有固定 Schema 的 `value`，再用一个 HSET 写入全部字段。
    template <field_value T>
    [[nodiscard]] result<void> set(const std::string_view key, const T& value) const {
        auto encoded = encode_fields(value);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return store(key, *encoded);
    }

    /// 删除 `names` 指定的 Hash 字段；返回实际删除数量，空名称集合被拒绝。
    [[nodiscard]] result<std::size_t> erase(std::string_view key, std::span<const std::string_view> names) const;

    /// 判断 `key` 中是否存在 `name` 字段。
    [[nodiscard]] result<bool> contains(std::string_view key, std::string_view name) const;

    /// 返回 `key` 的字段数量；不存在时为零。
    [[nodiscard]] result<std::size_t> size(std::string_view key) const;

private:
    explicit hash_commands(std::shared_ptr<detail::driver> driver) noexcept;

    std::shared_ptr<detail::driver> driver_;

    friend class client;
};

} // namespace verdandi
