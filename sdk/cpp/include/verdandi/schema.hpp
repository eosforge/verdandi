#pragma once

#include "verdandi/fields.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <functional>
#include <new>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace verdandi {

/// 可作为 C++20/23 非类型模板参数的编译期字段名。
template <std::size_t Size>
struct fixed_string {
    std::array<char, Size> value{};

    /// 从包含结尾零字节的字符串字面量构造稳定字段名。
    consteval fixed_string(const char (&source)[Size]) {
        std::copy_n(source, Size, value.begin());
    }

    /// 返回不包含结尾零字节的字段名视图。
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {value.data(), Size - 1};
    }
};

template <std::size_t Left, std::size_t Right>
[[nodiscard]] consteval bool operator==(const fixed_string<Left>& lhs, const fixed_string<Right>& rhs) noexcept {
    return lhs.view() == rhs.view();
}

/// 一个编译期字段名和成员指针描述符；运行期编码不进行成员名查找。
template <class Owner, class Member, fixed_string Name>
struct member_descriptor {
    using owner_type = Owner;
    using member_type = Member;

    Member Owner::*pointer;

    /// 返回该成员的稳定顶层 wire 名称。
    [[nodiscard]] static constexpr std::string_view name() noexcept {
        return Name.view();
    }
};

/// 构造一个由 `Name` 和 `pointer` 完整确定的成员描述符。
template <class Owner, fixed_string Name, class Member>
[[nodiscard]] consteval auto make_field(Member Owner::*pointer) noexcept {
    return member_descriptor<Owner, Member, Name>{pointer};
}

/// 应用结构的 Schema 特化点；使用 `VERDANDI_SCHEMA` 声明。
template <class T>
struct schema;

namespace detail {

[[nodiscard]] consteval bool valid_wire_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64 || value.front() == '@' || value.front() == '.' || value.front() == '&') {
        return false;
    }
    for (const char raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        if (character < 0x21U || character > 0x7eU) {
            return false;
        }
    }
    return true;
}

template <std::size_t Index, class Tuple, std::size_t... Rest>
[[nodiscard]] consteval bool name_differs_from_rest(const Tuple& value, std::index_sequence<Rest...>) noexcept {
    return ((std::get<Index>(value).name() != std::get<Index + 1 + Rest>(value).name()) && ...);
}

template <class Tuple, std::size_t... Index>
[[nodiscard]] consteval bool unique_names(const Tuple& value, std::index_sequence<Index...>) noexcept {
    return (name_differs_from_rest<Index>(value, std::make_index_sequence<std::tuple_size_v<Tuple> - Index - 1>{}) && ...);
}

template <class Tuple, std::size_t... Index>
[[nodiscard]] consteval bool valid_names(const Tuple& value, std::index_sequence<Index...>) noexcept {
    return (valid_wire_name(std::get<Index>(value).name()) && ...);
}

template <class T, class Tuple, std::size_t... Index>
[[nodiscard]] consteval bool matching_owner(const Tuple&, std::index_sequence<Index...>) noexcept {
    return (std::same_as<typename std::tuple_element_t<Index, Tuple>::owner_type, T> && ...);
}

template <class T>
concept described = requires { schema<T>::members; };

/// 在头文件边界内调用应用编解码或策略，并把所有异常统一转换为 Verdandi 结果。
/// 返回类型由具体回调静态推导；该高阶函数会被内联，不引入类型擦除或运行期分派。
template <class Callback>
[[nodiscard]] auto invoke_application(const std::string_view field, Callback&& callback) -> std::invoke_result_t<Callback&&> {
    try {
        return std::invoke(std::forward<Callback>(callback));
    } catch (const std::bad_alloc& exception) {
        return std::unexpected(error(code::unavailable, std::string(field)).with_detail(exception.what()));
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::contract, std::string(field)).with_detail(exception.what()));
    } catch (...) {
        return std::unexpected(error(code::contract, std::string(field)));
    }
}

/// 通过 `std::apply` 和折叠表达式静态展开 Schema 成员；回调类型不会被擦除。
template <class T, class Callback>
constexpr void for_each_schema_member(Callback&& callback) {
    std::apply([&](const auto&... member) { (std::invoke(callback, member), ...); }, schema<T>::members);
}

} // namespace detail

/// 编译期验证 `T` 的字段数量、wire 名称、唯一性及成员归属。
template <class T>
[[nodiscard]] consteval bool valid_schema() noexcept {
    if constexpr (!detail::described<T>) {
        return false;
    } else {
        constexpr auto& members = schema<T>::members;
        using tuple_type = std::remove_cvref_t<decltype(members)>;
        constexpr auto count = std::tuple_size_v<tuple_type>;
        if constexpr (count == 0 || count > 128) {
            return false;
        } else {
            return detail::valid_names(members, std::make_index_sequence<count>{}) && detail::unique_names(members, std::make_index_sequence<count - 1>{}) &&
                   detail::matching_owner<T>(members, std::make_index_sequence<count>{});
        }
    }
}

/// 具有已验证 Schema 且可默认构造的强类型顶层字段结构。
template <class T>
concept field_value = detail::described<T> && std::default_initializable<T> && valid_schema<T>();

/// 公共领域 API 接受的完整结构：编译期 Schema 类型或原始 Fields。
template <class T>
concept structured_value = std::same_as<std::remove_cvref_t<T>, fields> || field_value<std::remove_cvref_t<T>>;

/// 将 `value` 的全部 Schema 成员编码为拥有型 Fields；任一成员失败时不返回部分结果。
template <field_value T>
[[nodiscard]] result<fields> encode_fields(const T& value) {
    fields encoded;
    std::optional<error> failure;
    detail::for_each_schema_member<T>([&](const auto& member) {
        if (failure) {
            return;
        }
        using member_type = typename std::remove_cvref_t<decltype(member)>::member_type;
        static_assert(field_scalar<member_type>, "Every schema member requires a verdandi::field_codec specialization");
        auto converted = detail::invoke_application(member.name(), [&] { return field_codec<member_type>::encode(value.*(member.pointer)); });
        if (!converted) {
            failure = converted.error();
            return;
        }
        encoded.emplace(std::string(member.name()), std::move(*converted));
    });
    if (failure) {
        return std::unexpected(std::move(*failure));
    }
    return encoded;
}

/// 从完整 `source` 解码 `T`；缺失、额外或失败的成员均拒绝整次投影。
template <field_value T>
[[nodiscard]] result<T> decode_fields(const fields& source) {
    constexpr auto expected = std::tuple_size_v<std::remove_cvref_t<decltype(schema<T>::members)>>;
    if (source.size() != expected) {
        return std::unexpected(error(code::contract, "fields"));
    }
    T decoded{};
    std::optional<error> failure;
    detail::for_each_schema_member<T>([&](const auto& member) {
        if (failure) {
            return;
        }
        const auto iterator = source.find(member.name());
        if (iterator == source.end()) {
            failure = error(code::contract, std::string(member.name()));
            return;
        }
        using member_type = typename std::remove_cvref_t<decltype(member)>::member_type;
        static_assert(field_scalar<member_type>, "Every schema member requires a verdandi::field_codec specialization");
        auto converted = detail::invoke_application(member.name(), [&] { return field_codec<member_type>::decode(iterator->second); });
        if (!converted) {
            failure = converted.error();
            return;
        }
        decoded.*(member.pointer) = std::move(*converted);
    });
    if (failure) {
        return std::unexpected(std::move(*failure));
    }
    return decoded;
}

/// 在领域边界把 Schema 类型编码为完整 Fields；原始 Fields 取得独立拥有型副本。
template <structured_value T>
[[nodiscard]] result<fields> encode_value(const T& value) {
    if constexpr (std::same_as<std::remove_cvref_t<T>, fields>) {
        return value;
    } else {
        return encode_fields(value);
    }
}

/// 在领域边界把完整 Fields 解码为 Schema 类型；原始 Fields 取得独立拥有型副本。
template <structured_value T>
[[nodiscard]] result<std::remove_cvref_t<T>> decode_value(const fields& source) {
    if constexpr (std::same_as<std::remove_cvref_t<T>, fields>) {
        return source;
    } else {
        return decode_fields<std::remove_cvref_t<T>>(source);
    }
}

} // namespace verdandi

/// 声明 `TYPE::MEMBER` 使用其 C++ 标识符文本作为 wire 字段名。
#define VERDANDI_FIELD(TYPE, MEMBER) ::verdandi::make_field<TYPE, #MEMBER>(&TYPE::MEMBER)

/// 声明 `TYPE::MEMBER` 使用显式 `NAME` 字符串字面量作为 wire 字段名。
#define VERDANDI_NAMED_FIELD(TYPE, MEMBER, NAME) ::verdandi::make_field<TYPE, NAME>(&TYPE::MEMBER)

/// 为 `TYPE` 声明编译期 Schema；字段描述符按传入顺序展开并在编译期验证。
#define VERDANDI_SCHEMA(TYPE, ...)                                                                                                                             \
    template <>                                                                                                                                                \
    struct verdandi::schema<TYPE> {                                                                                                                            \
        static constexpr auto members = std::tuple{__VA_ARGS__};                                                                                               \
        static_assert(::verdandi::valid_schema<TYPE>(), "Invalid Verdandi schema");                                                                            \
    }
