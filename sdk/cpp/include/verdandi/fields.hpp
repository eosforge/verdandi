#pragma once

#include "verdandi/error.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace verdandi {

/// Redis 和 Verdandi 协议均不解释的拥有型二进制值。
using bytes = std::vector<std::byte>;

/// 一个完整顶层结构；透明比较器允许用 `string_view` 查找而不分配临时字符串。
using fields = std::map<std::string, bytes, std::less<>>;

/// 为一个 C++ 成员类型提供顶层字段字节编解码。
///
/// 应用可对自定义标量显式特化本模板。编码结果必须拥有自己的字节，解码必须消费完整输入。
template <class T, class Enable = void>
struct field_codec;

template <>
struct field_codec<bytes> {
    /// 返回 `value` 的拥有型副本。
    [[nodiscard]] static result<bytes> encode(std::span<const std::byte> value);

    /// 返回包含 `value` 完整内容的拥有型字节。
    [[nodiscard]] static result<bytes> decode(std::span<const std::byte> value);
};

template <>
struct field_codec<std::string> {
    /// 按原始 UTF-8/二进制字节编码字符串；协议不替应用验证文本语义。
    [[nodiscard]] static result<bytes> encode(std::string_view value);

    /// 用完整字段字节构造字符串，允许其中包含零字节。
    [[nodiscard]] static result<std::string> decode(std::span<const std::byte> value);
};

template <>
struct field_codec<bool> {
    /// 把布尔值编码为稳定文本 `true` 或 `false`。
    [[nodiscard]] static result<bytes> encode(bool value);

    /// 只接受精确的稳定文本 `true` 或 `false`。
    [[nodiscard]] static result<bool> decode(std::span<const std::byte> value);
};

template <class T>
struct field_codec<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    /// 把整数 `value` 编码为无前导零的十进制文本。
    [[nodiscard]] static result<bytes> encode(T value) {
        std::array<char, std::numeric_limits<T>::digits10 + 4> buffer{};
        const auto [end, status] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (status != std::errc{}) {
            return std::unexpected(error(code::invalid, "field"));
        }
        const auto* first = reinterpret_cast<const std::byte*>(buffer.data());
        const auto* last = reinterpret_cast<const std::byte*>(end);
        return bytes(first, last);
    }

    /// 从完整十进制字段解码整数；空值、空白、前导加号和越界值均被拒绝。
    [[nodiscard]] static result<T> decode(std::span<const std::byte> value) {
        if (value.empty()) {
            return std::unexpected(error(code::invalid, "field"));
        }
        const auto* first = reinterpret_cast<const char*>(value.data());
        const auto* last = first + value.size();
        T decoded{};
        const auto [end, status] = std::from_chars(first, last, decoded);
        if (status != std::errc{} || end != last) {
            return std::unexpected(error(code::invalid, "field"));
        }
        if (value.size() > 1 && first[0] == '0') {
            return std::unexpected(error(code::invalid, "field"));
        }
        if constexpr (std::is_signed_v<T>) {
            if (value.size() >= 2 && first[0] == '-' && first[1] == '0') {
                return std::unexpected(error(code::invalid, "field"));
            }
        }
        return decoded;
    }
};

/// 判断 `T` 是否具有返回 Verdandi `result` 的字段级编解码实现。
template <class T>
concept field_scalar = requires(const T& input, std::span<const std::byte> encoded) {
    { field_codec<T>::encode(input) } -> std::same_as<result<bytes>>;
    { field_codec<T>::decode(encoded) } -> std::same_as<result<T>>;
};

} // namespace verdandi
