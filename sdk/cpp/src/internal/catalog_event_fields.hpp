#pragma once

#include "verdandi/fields.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace verdandi::catalog::detail {

/// 顺序读取 Catalog MessagePack 通知；所有视图只在输入缓冲区存活期间有效。
class event_cursor final {
public:
    explicit event_cursor(const std::string_view source) noexcept : source_(source) {}

    [[nodiscard]] result<std::size_t> array_size() {
        auto marker = byte();
        if (!marker) {
            return std::unexpected(marker.error());
        }
        if ((*marker & 0xf0U) == 0x90U) {
            return static_cast<std::size_t>(*marker & 0x0fU);
        }
        std::size_t width{};
        if (*marker == 0xdcU) {
            width = 2;
        } else if (*marker == 0xddU) {
            width = 4;
        } else {
            return std::unexpected(error(code::corrupt, "notification"));
        }
        auto encoded = take(width);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        std::uint64_t output{};
        for (const char raw : *encoded) {
            output = (output << 8U) | static_cast<unsigned char>(raw);
        }
        if (output > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(error(code::capacity, "notification"));
        }
        return static_cast<std::size_t>(output);
    }

    [[nodiscard]] result<std::string_view> binary() {
        auto marker = byte();
        if (!marker) {
            return std::unexpected(marker.error());
        }
        std::size_t length{};
        if ((*marker & 0xe0U) == 0xa0U) {
            length = static_cast<std::size_t>(*marker & 0x1fU);
        } else {
            std::size_t width{};
            if (*marker == 0xc4U || *marker == 0xd9U) {
                width = 1;
            } else if (*marker == 0xc5U || *marker == 0xdaU) {
                width = 2;
            } else if (*marker == 0xc6U || *marker == 0xdbU) {
                width = 4;
            } else {
                return std::unexpected(error(code::corrupt, "notification"));
            }
            auto encoded = take(width);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            std::uint64_t decoded{};
            for (const char raw : *encoded) {
                decoded = (decoded << 8U) | static_cast<unsigned char>(raw);
            }
            if (decoded > source_.size()) {
                return std::unexpected(error(code::capacity, "notification"));
            }
            length = static_cast<std::size_t>(decoded);
        }
        return take(length);
    }

    [[nodiscard]] bool done() const noexcept {
        return offset_ == source_.size();
    }

private:
    [[nodiscard]] result<unsigned char> byte() {
        if (offset_ >= source_.size()) {
            return std::unexpected(error(code::corrupt, "notification"));
        }
        return static_cast<unsigned char>(source_[offset_++]);
    }

    [[nodiscard]] result<std::string_view> take(const std::size_t length) {
        if (length > source_.size() - offset_) {
            return std::unexpected(error(code::corrupt, "notification"));
        }
        const auto output = source_.substr(offset_, length);
        offset_ += length;
        return output;
    }

    std::string_view source_;
    std::size_t offset_{};
};

/// 解码交替字段名与值；Array Replace 按连续数值下标，其余通知按字典序验证。
/// 返回字段拥有输入数据的副本，且不读取 Redis 或加载外部运行时依赖。
[[nodiscard]] result<fields> event_fields(event_cursor& cursor, bool array_replace);

} // namespace verdandi::catalog::detail
