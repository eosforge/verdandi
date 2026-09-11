#pragma once

#include "verdandi/catalog/publisher.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace verdandi::catalog::detail {

/// 单个 Catalog 完整值或 Patch 的协议字段数上限。
constexpr std::size_t maximum_fields = 65'536;

/// 校验 shape、字段名称、连续数组下标和 maximum_bytes，返回名称与值的精确总字节数。
/// 成功路径不分配临时容器；失败返回稳定字段错误，且不修改借用的 value。
[[nodiscard]] result<std::size_t> validate_catalog_value(kind shape, const fields& value, std::size_t maximum_bytes);

/// 校验非空 Patch 的字段名称、数量和 maximum_bytes；不读取 Redis 或修改 value。
[[nodiscard]] result<void> validate_catalog_patch(const fields& value, std::size_t maximum_bytes);

/// 校验 value 并构造拥有型 Replace 参数；member 必须来自已经验证的 Path。
/// Array 按数值下标排列，Value/Map 按字段字典序排列；返回值不保留任何输入引用。
[[nodiscard]] result<std::vector<std::string>> encode_catalog_replace(std::string_view member, kind shape, const fields& value, std::size_t maximum_bytes);

} // namespace verdandi::catalog::detail
