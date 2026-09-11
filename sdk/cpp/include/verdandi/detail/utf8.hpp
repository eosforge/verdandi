#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace verdandi::detail {

/// 编译期与运行期共用的 UTF-8 校验；可返回首尾码点，拒绝截断、过长编码及非法 Unicode。
[[nodiscard]] constexpr bool valid_utf8(const std::string_view value, std::uint32_t* first_codepoint = nullptr,
                                        std::uint32_t* last_codepoint = nullptr) noexcept {
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t width{1};
        std::uint32_t codepoint{first};
        if (first <= 0x7fU) {
            // ASCII 已经是完整码点。
        } else if (first >= 0xc2U && first <= 0xdfU) {
            width = 2;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            width = 3;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            width = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (width > value.size() - index) {
            return false;
        }
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if ((width == 3 && codepoint < 0x800U) || (width == 4 && codepoint < 0x1'0000U) || (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10'ffffU) {
            return false;
        }
        if (index == 0 && first_codepoint != nullptr) {
            *first_codepoint = codepoint;
        }
        if (last_codepoint != nullptr) {
            *last_codepoint = codepoint;
        }
        index += width;
    }
    return true;
}

} // namespace verdandi::detail
