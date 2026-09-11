#include "internal/catalog_event_fields.hpp"
#include "internal/catalog_value.hpp"

#include <charconv>
#include <string>

namespace verdandi::catalog::detail {

namespace {

[[nodiscard]] bytes copy_bytes(const std::string_view value) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

} // namespace

[[nodiscard]] result<fields> event_fields(event_cursor& cursor, const bool array_replace) {
    auto elements = cursor.array_size();
    if (!elements || *elements % 2 != 0 || *elements / 2 > maximum_fields) {
        return std::unexpected(error(code::corrupt, "fields"));
    }
    fields output;
    std::string previous;
    for (std::size_t index = 0; index < *elements; index += 2) {
        auto name = cursor.binary();
        auto value = cursor.binary();
        if (!name || !value || name->empty()) {
            return std::unexpected(error(code::corrupt, "fields"));
        }
        // Replace 的完整数组按数值下标发布；Map、Value 和稀疏 Patch 保持字典序。
        // 解析后对照当前位置，同时拒绝前导零，不能复用 map 的字符串顺序判断。
        if (array_replace) {
            std::size_t decoded{};
            const auto [end, conversion] = std::from_chars(name->data(), name->data() + name->size(), decoded);
            if (conversion != std::errc{} || end != name->data() + name->size() || decoded != index / 2 || (name->size() > 1 && name->front() == '0')) {
                return std::unexpected(error(code::corrupt, "fields"));
            }
        } else if (!previous.empty() && previous >= *name) {
            return std::unexpected(error(code::corrupt, "fields"));
        }
        previous.assign(*name);
        output.emplace(previous, copy_bytes(*value));
    }
    return output;
}

} // namespace verdandi::catalog::detail
