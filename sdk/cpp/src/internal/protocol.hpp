#pragma once

#include <cstdint>
#include <string_view>

namespace verdandi::detail {

enum class registration_operation : std::uint8_t {
    register_value,
    update,
    renew,
    unregister,
};

enum class catalog_operation : std::uint8_t {
    read,
    replace,
    patch,
    delete_value,
};

/// 返回构建时嵌入、与规范 Lua 产物逐字节一致的 Registration 脚本。
[[nodiscard]] std::string_view registration_script(registration_operation operation) noexcept;

/// 返回构建时嵌入、与规范 Lua 产物逐字节一致的 Catalog 脚本。
[[nodiscard]] std::string_view catalog_script(catalog_operation operation) noexcept;

} // namespace verdandi::detail
