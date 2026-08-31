#pragma once

#include "verdandi/error.hpp"

#include <compare>
#include <string>
#include <string_view>

namespace verdandi::catalog {

/// Catalog Client Zone 内由 Part 和 ID 组成的不可变身份。
class path final {
public:
    path() noexcept = default;

    /// 校验并构造 Path；Part 最多 64 字节，ID 最多 128 字节。
    [[nodiscard]] static result<path> create(std::string part, std::string id);

    /// 返回不可变 Part。
    [[nodiscard]] std::string_view part() const noexcept;

    /// 返回不可变 ID。
    [[nodiscard]] std::string_view id() const noexcept;

    /// 返回索引使用的规范 `part:id` 文本。
    [[nodiscard]] std::string member() const;

    /// 重新验证两段；默认构造的 Path 无效。
    [[nodiscard]] bool valid() const noexcept;

    auto operator<=>(const path&) const = default;

private:
    path(std::string part, std::string id) : part_(std::move(part)), id_(std::move(id)) {}

    std::string part_;
    std::string id_;
};

} // namespace verdandi::catalog
