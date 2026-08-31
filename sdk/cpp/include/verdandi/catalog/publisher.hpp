#pragma once

#include "verdandi/catalog/client.hpp"
#include "verdandi/schema.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace verdandi::catalog {

/// Catalog 值的规范顶层形状。
enum class kind : std::uint8_t {
    value,
    array,
    map,
};

/// 一次已接纳变更获得的 Redis Zone 全局 revision。
struct mutation_result {
    std::uint64_t revision{};
};

/// 一次严格基于版本的 Array/Map 局部覆盖；v1 不支持字段删除。
struct patch {
    std::uint64_t base_revision{};
    fields set;
};

namespace detail {
[[nodiscard]] result<mutation_result> catalog_replace(const std::shared_ptr<client_core>& owner, const path& target, kind shape, fields value);
[[nodiscard]] result<mutation_result> catalog_patch(const std::shared_ptr<client_core>& owner, const path& target, patch value);
[[nodiscard]] result<mutation_result> catalog_erase(const std::shared_ptr<client_core>& owner, const path& target);
} // namespace detail

/// 绑定 Catalog Client 的无任务轻量写入器。
class publisher final {
public:
    publisher() noexcept = default;

    /// 创建轻量写入视图；不执行 Redis I/O，也无需单独关闭。
    [[nodiscard]] static result<publisher> create(const client& owner);

    /// 原子发布一个完整 Value、Array 或 Map；并发 Replace/Erase 采用 Redis 执行顺序的最晚写入。
    template <structured_value Value>
    [[nodiscard]] result<mutation_result> replace(const path& target, const kind shape, const Value& value) const {
        auto encoded = encode_value(value);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return detail::catalog_replace(core_, target, shape, std::move(*encoded));
    }

    /// 仅在 BaseRevision 精确匹配时原子覆盖 Array/Map 字段。
    [[nodiscard]] result<mutation_result> apply(const path& target, patch value) const;

    /// 原子删除完整 Path 并总是产生一个新 tombstone revision。
    [[nodiscard]] result<mutation_result> erase(const path& target) const;

private:
    explicit publisher(std::shared_ptr<detail::client_core> core) noexcept : core_(std::move(core)) {}

    std::shared_ptr<detail::client_core> core_;
};

} // namespace verdandi::catalog
