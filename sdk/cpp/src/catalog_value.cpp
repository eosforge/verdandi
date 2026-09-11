#include "internal/catalog_value.hpp"
#include "verdandi/detail/utf8.hpp"

#include <charconv>
#include <cstdint>

namespace verdandi::catalog::detail {

namespace {

using verdandi::detail::valid_utf8;

/// 返回已知 Catalog 形状的固定协议名；未知枚举返回空视图，不分配字符串。
[[nodiscard]] std::string_view kind_name(const kind value) noexcept {
    switch (value) {
    case kind::value:
        return "value";
    case kind::array:
        return "array";
    case kind::map:
        return "map";
    }
    return {};
}

/// 解析 [0,count) 内的规范十进制下标；空值、前导零、非法字符和越界返回 contract。
[[nodiscard]] result<std::size_t> array_index(const std::string_view name, const std::size_t count) {
    if (count == 0 || name.empty() || (name.size() > 1 && name.front() == '0')) {
        return std::unexpected(error(code::contract, "array"));
    }
    std::size_t output{};
    const auto [end, status] = std::from_chars(name.data(), name.data() + name.size(), output);
    if (status != std::errc{} || end != name.data() + name.size() || output >= count) {
        return std::unexpected(error(code::contract, "array"));
    }
    return output;
}

/// 单遍校验字段和容量；array 表示还须校验下标，capacity_field 保留 Value/Patch 的错误上下文。
/// 仅借用 value；累计大小始终不超过 maximum_bytes，因此减法检查不会下溢。
[[nodiscard]] result<std::size_t> validate_fields(const fields& value, const std::size_t maximum_bytes, const bool array,
                                                  const std::string_view capacity_field) {
    std::size_t size{};
    for (const auto& [name, field] : value) {
        // fields 的键唯一，规范下标又均落在 [0,N)；N 个这样的键必然恰好覆盖整个数组。
        // std::map 的字典序不是数值顺序，不能要求遍历中的下标逐次递增。
        if (array && !array_index(name, value.size())) {
            return std::unexpected(error(code::contract, "array"));
        }
        if (name.empty() || name.front() == '@' || !valid_utf8(name)) {
            return std::unexpected(error(code::invalid, name));
        }
        if (name.size() > maximum_bytes - size || field.size() > maximum_bytes - size - name.size()) {
            return std::unexpected(error(code::capacity, std::string(capacity_field)));
        }
        size += name.size() + field.size();
    }
    return size;
}

} // namespace

result<std::size_t> validate_catalog_value(const kind shape, const fields& value, const std::size_t maximum_bytes) {
    if (kind_name(shape).empty()) {
        return std::unexpected(error(code::invalid, "kind"));
    }
    if (value.size() > maximum_fields) {
        return std::unexpected(error(code::capacity, "fields"));
    }
    if (shape == kind::value && (value.size() != 1 || value.find("value") == value.end())) {
        return std::unexpected(error(code::contract, "value"));
    }
    return validate_fields(value, maximum_bytes, shape == kind::array, "value");
}

result<void> validate_catalog_patch(const fields& value, const std::size_t maximum_bytes) {
    if (value.empty()) {
        return std::unexpected(error(code::invalid, "patch"));
    }
    if (value.size() > maximum_fields) {
        return std::unexpected(error(code::capacity, "fields"));
    }
    auto size = validate_fields(value, maximum_bytes, false, "patch");
    if (!size) {
        return std::unexpected(size.error());
    }
    return {};
}

result<std::vector<std::string>> encode_catalog_replace(const std::string_view member, const kind shape, const fields& value, const std::size_t maximum_bytes) {
    auto size = validate_catalog_value(shape, value, maximum_bytes);
    if (!size) {
        return std::unexpected(size.error());
    }
    std::vector<std::string> arguments(4 + value.size() * 2);
    arguments[0] = member;
    arguments[1] = kind_name(shape);
    arguments[2] = std::to_string(*size);
    arguments[3] = std::to_string(value.size());
    std::size_t next{};
    for (const auto& [name, field] : value) {
        // 输入已完整校验且在此只读；数组下标必然有效并唯一。直接填入最终参数位置，
        // 无需临时名称数组、额外排序或再次按名称查找 map。
        const auto index = shape == kind::array ? *array_index(name, value.size()) : next++;
        arguments[4 + index * 2] = name;
        if (!field.empty()) {
            arguments[5 + index * 2].assign(reinterpret_cast<const char*>(field.data()), field.size());
        }
    }
    return arguments;
}

} // namespace verdandi::catalog::detail
