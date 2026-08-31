#pragma once

#include "verdandi/legacy/error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace verdandi {
namespace legacy {

typedef std::vector<std::uint8_t> bytes;

template <class T>
struct schema;

/// 为一个字段标量提供稳定二进制编码；应用可显式特化自定义标量。
template <class T, class Enable = void>
struct value_codec;

/// 在应用结构与完整顶层 Fields 之间转换；Schema 类型自动获得默认实现。
template <class T, class Enable = void>
struct codec;

namespace detail {

template <class T, bool Signed>
struct integer_traits;

template <class T>
struct integer_traits<T, false> {
    typedef typename std::make_unsigned<T>::type unsigned_type;

    static bool negative(T) {
        return false;
    }
    static unsigned_type magnitude(T value) {
        return static_cast<unsigned_type>(value);
    }
    static unsigned_type limit(bool) {
        return std::numeric_limits<unsigned_type>::max();
    }
    static result<T> finish(unsigned_type value, bool negative) {
        if (negative) {
            return result<T>(error("invalid", "field"));
        }
        return result<T>(static_cast<T>(value));
    }
};

template <class T>
struct integer_traits<T, true> {
    typedef typename std::make_unsigned<T>::type unsigned_type;

    static bool negative(T value) {
        return value < 0;
    }

    static unsigned_type magnitude(T value) {
        const unsigned_type converted = static_cast<unsigned_type>(value);
        return negative(value) ? static_cast<unsigned_type>(unsigned_type(0) - converted) : converted;
    }

    static unsigned_type limit(bool negative_value) {
        const unsigned_type maximum = static_cast<unsigned_type>(std::numeric_limits<T>::max());
        return negative_value ? static_cast<unsigned_type>(maximum + unsigned_type(1)) : maximum;
    }

    static result<T> finish(unsigned_type value, bool negative_value) {
        if (!negative_value) {
            return result<T>(static_cast<T>(value));
        }
        const unsigned_type minimum_magnitude = static_cast<unsigned_type>(std::numeric_limits<T>::max()) + unsigned_type(1);
        if (value == minimum_magnitude) {
            return result<T>(std::numeric_limits<T>::min());
        }
        return result<T>(static_cast<T>(-static_cast<T>(value)));
    }
};

template <class T>
class has_schema {
private:
    template <class U>
    static auto test(int) -> decltype(schema<U>::members(), std::true_type());

    template <class>
    static std::false_type test(...);

public:
    static const bool value = decltype(test<T>(0))::value;
};

} // namespace detail

/// C++11 包装层拥有的完整顶层 Fields；名称有序且每个值均独立拥有。
class fields {
public:
    typedef std::map<std::string, bytes> map_type;

    /// 构造空的拥有型字段集合。
    fields() {}

    /// 返回字段数量。
    std::size_t size() const {
        return values_.size();
    }
    /// 返回字段集合是否为空。
    bool empty() const {
        return values_.empty();
    }

    /// 插入一个原始二进制字段；重复字段返回 contract，避免静默覆盖结构定义。
    result<void> insert(std::string name, bytes value) {
        const std::pair<map_type::iterator, bool> inserted = values_.insert(std::make_pair(std::move(name), std::move(value)));
        if (!inserted.second) {
            return result<void>(error("contract", inserted.first->first));
        }
        return result<void>();
    }

    /// 使用稳定标量 Codec 编码并插入字段。
    template <class T>
    result<void> insert(const std::string& name, const T& value) {
        result<bytes> encoded = value_codec<T>::encode(value);
        if (!encoded) {
            return result<void>(error(encoded.failure().code(), name, encoded.failure().detail()));
        }
        return insert(name, std::move(*encoded));
    }

    /// 取得并解码一个必需字段；缺失和非规范值均带回实际字段名。
    template <class T>
    result<T> get(const std::string& name) const {
        const map_type::const_iterator found = values_.find(name);
        if (found == values_.end()) {
            return result<T>(error("missing", name));
        }
        result<T> decoded = value_codec<T>::decode(found->second);
        if (!decoded) {
            return result<T>(error(decoded.failure().code(), name, decoded.failure().detail()));
        }
        return decoded;
    }

    /// 查找原始字段并返回只读地址；缺失时返回空指针，地址随本对象修改失效。
    const bytes* find(const std::string& name) const {
        const map_type::const_iterator found = values_.find(name);
        return found == values_.end() ? NULL : &found->second;
    }

    /// 返回按名称排序的只读字段 Map；引用随本对象生命周期和修改规则有效。
    const map_type& values() const {
        return values_;
    }

private:
    map_type values_;
};

template <>
struct value_codec<bytes, void> {
    /// 复制并返回完整二进制字段。
    static result<bytes> encode(const bytes& value) {
        return result<bytes>(value);
    }
    /// 复制并返回完整二进制字段。
    static result<bytes> decode(const bytes& value) {
        return result<bytes>(value);
    }
};

template <>
struct value_codec<std::string, void> {
    /// 按原始字节编码字符串，允许空值和内嵌零字节。
    static result<bytes> encode(const std::string& value) {
        if (value.empty()) {
            return result<bytes>(bytes());
        }
        return result<bytes>(bytes(reinterpret_cast<const std::uint8_t*>(value.data()), reinterpret_cast<const std::uint8_t*>(value.data()) + value.size()));
    }

    /// 用完整字段字节构造字符串，协议不替应用验证文本语义。
    static result<std::string> decode(const bytes& value) {
        if (value.empty()) {
            return result<std::string>(std::string());
        }
        return result<std::string>(std::string(reinterpret_cast<const char*>(value.data()), value.size()));
    }
};

template <>
struct value_codec<bool, void> {
    /// 把布尔值编码为稳定文本 `true` 或 `false`。
    static result<bytes> encode(bool value) {
        static const std::uint8_t true_value[] = {'t', 'r', 'u', 'e'};
        static const std::uint8_t false_value[] = {'f', 'a', 'l', 's', 'e'};
        return value ? result<bytes>(bytes(true_value, true_value + sizeof(true_value))) : result<bytes>(bytes(false_value, false_value + sizeof(false_value)));
    }

    /// 只接受精确的稳定文本 `true` 或 `false`。
    static result<bool> decode(const bytes& value) {
        static const std::uint8_t true_value[] = {'t', 'r', 'u', 'e'};
        static const std::uint8_t false_value[] = {'f', 'a', 'l', 's', 'e'};
        if (value.size() == sizeof(true_value) && std::equal(value.begin(), value.end(), true_value)) {
            return result<bool>(true);
        }
        if (value.size() == sizeof(false_value) && std::equal(value.begin(), value.end(), false_value)) {
            return result<bool>(false);
        }
        return result<bool>(error("invalid", "field"));
    }
};

template <class T>
struct value_codec<T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value>::type> {
    typedef detail::integer_traits<T, std::is_signed<T>::value> traits;
    typedef typename traits::unsigned_type unsigned_type;

    /// 把整数编码为无加号、无前导零的规范十进制文本。
    static result<bytes> encode(T value) {
        std::uint8_t buffer[std::numeric_limits<unsigned_type>::digits10 + 3];
        std::uint8_t* cursor = buffer + sizeof(buffer);
        unsigned_type magnitude = traits::magnitude(value);
        do {
            *--cursor = static_cast<std::uint8_t>('0' + magnitude % 10);
            magnitude = static_cast<unsigned_type>(magnitude / 10);
        } while (magnitude != 0);
        if (traits::negative(value)) {
            *--cursor = '-';
        }
        return result<bytes>(bytes(cursor, buffer + sizeof(buffer)));
    }

    /// 解码完整规范十进制字段；拒绝空值、加号、前导零、负零和越界。
    static result<T> decode(const bytes& value) {
        if (value.empty()) {
            return result<T>(error("invalid", "field"));
        }
        std::size_t offset = 0;
        bool negative = false;
        if (value[0] == '-') {
            negative = true;
            offset = 1;
            if (!std::is_signed<T>::value || value.size() == 1) {
                return result<T>(error("invalid", "field"));
            }
        } else if (value[0] == '+') {
            return result<T>(error("invalid", "field"));
        }
        if (value[offset] == '0' && (value.size() - offset != 1 || negative)) {
            return result<T>(error("invalid", "field"));
        }

        const unsigned_type limit = traits::limit(negative);
        unsigned_type decoded = 0;
        for (; offset < value.size(); ++offset) {
            const std::uint8_t current = value[offset];
            if (current < '0' || current > '9') {
                return result<T>(error("invalid", "field"));
            }
            const unsigned_type digit = static_cast<unsigned_type>(current - '0');
            if (decoded > static_cast<unsigned_type>((limit - digit) / 10)) {
                return result<T>(error("invalid", "field"));
            }
            decoded = static_cast<unsigned_type>(decoded * 10 + digit);
        }
        return traits::finish(decoded, negative);
    }
};

/// 一个 C++11 Schema 成员描述；只保存稳定字段名和成员指针。
template <class Owner, class Member>
struct member_field {
    /// C ABI 中使用的稳定字段名；调用方必须提供静态生命周期文本。
    const char* name;
    /// 指向应用结构成员的类型安全指针。
    Member Owner::*pointer;
};

/// 构造一个字段描述；`name` 必须在 Schema 静态对象的完整生命周期内有效。
template <class Owner, class Member>
member_field<Owner, Member> make_field(const char* name, Member Owner::*pointer) {
    member_field<Owner, Member> output = {name, pointer};
    return output;
}

namespace detail {

template <std::size_t Index, std::size_t Size>
struct schema_encoder {
    template <class T, class Tuple>
    static result<void> apply(const T& value, const Tuple& members, fields& output) {
        const typename std::tuple_element<Index, Tuple>::type& member = std::get<Index>(members);
        result<void> inserted = output.insert(member.name, value.*(member.pointer));
        if (!inserted) {
            return inserted;
        }
        return schema_encoder<Index + 1, Size>::apply(value, members, output);
    }
};

template <std::size_t Size>
struct schema_encoder<Size, Size> {
    template <class T, class Tuple>
    static result<void> apply(const T&, const Tuple&, fields&) {
        return result<void>();
    }
};

template <std::size_t Index, std::size_t Size>
struct schema_decoder {
    template <class T, class Tuple>
    static result<void> apply(T& value, const Tuple& members, const fields& input) {
        const typename std::tuple_element<Index, Tuple>::type& member = std::get<Index>(members);
        typedef typename std::remove_reference<decltype(value.*(member.pointer))>::type member_type;
        result<member_type> decoded = input.get<member_type>(member.name);
        if (!decoded) {
            return result<void>(decoded.failure());
        }
        value.*(member.pointer) = std::move(*decoded);
        return schema_decoder<Index + 1, Size>::apply(value, members, input);
    }
};

template <std::size_t Size>
struct schema_decoder<Size, Size> {
    template <class T, class Tuple>
    static result<void> apply(T&, const Tuple&, const fields&) {
        return result<void>();
    }
};

} // namespace detail

template <>
struct codec<fields, void> {
    /// 为原始 Fields 模式返回拥有型副本。
    static result<fields> encode(const fields& value) {
        return result<fields>(value);
    }
    /// 为原始 Fields 模式返回拥有型副本。
    static result<fields> decode(const fields& value) {
        return result<fields>(value);
    }
};

/// 为拥有 Schema 的应用结构提供通用强类型编解码；应用仍可显式特化 Codec。
template <class T>
struct codec<T, typename std::enable_if<detail::has_schema<T>::value>::type> {
    /// 按 Schema 声明顺序编码全部成员；任一字段失败即停止。
    static result<fields> encode(const T& value) {
        fields output;
        const auto& members = schema<T>::members();
        result<void> encoded =
            detail::schema_encoder<0, std::tuple_size<typename std::remove_reference<decltype(members)>::type>::value>::apply(value, members, output);
        if (!encoded) {
            return result<fields>(encoded.failure());
        }
        return result<fields>(std::move(output));
    }

    /// 默认构造 `T` 并解码全部必需成员；缺失或非法字段返回对应错误。
    static result<T> decode(const fields& value) {
        T output = T();
        const auto& members = schema<T>::members();
        result<void> decoded =
            detail::schema_decoder<0, std::tuple_size<typename std::remove_reference<decltype(members)>::type>::value>::apply(output, members, value);
        if (!decoded) {
            return result<T>(decoded.failure());
        }
        return result<T>(std::move(output));
    }
};

namespace detail {

/// 调用应用 Codec 并把异常收敛为稳定 Verdandi 错误。
template <class T>
result<fields> encode_value(const T& value) {
    try {
        return codec<T>::encode(value);
    } catch (const std::bad_alloc& exception) {
        return result<fields>(error("capacity", "codec", exception.what()));
    } catch (const std::exception& exception) {
        return result<fields>(error("unavailable", "codec", exception.what()));
    } catch (...) {
        return result<fields>(error("corrupt", "codec"));
    }
}

/// 调用应用 Codec 并把异常收敛为稳定 Verdandi 错误。
template <class T>
result<T> decode_value(const fields& value) {
    try {
        return codec<T>::decode(value);
    } catch (const std::bad_alloc& exception) {
        return result<T>(error("capacity", "codec", exception.what()));
    } catch (const std::exception& exception) {
        return result<T>(error("unavailable", "codec", exception.what()));
    } catch (...) {
        return result<T>(error("corrupt", "codec"));
    }
}

/// 构造仅在当前 C ABI 调用期间借用字符串的视图。
inline verdandi_string_view native_text(const std::string& value) {
    verdandi_string_view output = {value.data(), value.size()};
    return output;
}

/// 构造仅在当前 C ABI 调用期间借用字节的视图；空值使用空指针。
inline verdandi_bytes_view native_bytes(const bytes& value) {
    verdandi_bytes_view output = {value.empty() ? NULL : &value[0], value.size()};
    return output;
}

/// 在一次 C ABI 调用期间拥有扁平字段视图；底层名称和值仍由 Fields 拥有。
class native_fields {
public:
    /// 为 `value` 的全部字段建立连续 C 视图；本对象不得比 `value` 活得更久。
    explicit native_fields(const fields& value) {
        views_.reserve(value.size());
        for (fields::map_type::const_iterator iterator = value.values().begin(); iterator != value.values().end(); ++iterator) {
            verdandi_field_view field = {native_text(iterator->first), native_bytes(iterator->second)};
            views_.push_back(field);
        }
    }

    /// 返回调用期间借用的连续字段视图。
    verdandi_fields_view view() const {
        verdandi_fields_view output = {views_.empty() ? NULL : &views_[0], views_.size()};
        return output;
    }

private:
    std::vector<verdandi_field_view> views_;
};

struct field_collector {
    /// 已从 C 回调完整复制的字段。
    fields value;
    /// 回调发现的协议或重复字段错误。
    legacy::error failure;
    /// 异常类别：零为无异常，一为容量错误，二为其他异常。
    int exception;

    /// 构造尚未收集字段且没有异常的回调状态。
    field_collector() : exception(0) {}
};

/// C ABI 字段访问回调；在返回前拥有型复制名称和值，任何异常都不跨越 C。
inline int VERDANDI_C_CALL collect_field(void* context, const verdandi_string_view name, const verdandi_bytes_view value) {
    field_collector* collector = static_cast<field_collector*>(context);
    try {
        if (collector == NULL || (name.size != 0U && name.data == NULL) || (value.size != 0U && value.data == NULL)) {
            if (collector != NULL) {
                collector->failure = legacy::error("invalid", "field");
            }
            return 0;
        }
        std::string field_name(name.data == NULL ? "" : name.data, name.size);
        bytes encoded = value.size == 0U ? bytes() : bytes(value.data, value.data + value.size);
        result<void> inserted = collector->value.insert(std::move(field_name), std::move(encoded));
        if (!inserted) {
            collector->failure = inserted.failure();
            return 0;
        }
        return 1;
    } catch (const std::bad_alloc&) {
        if (collector != NULL) {
            collector->exception = 1;
        }
    } catch (...) {
        if (collector != NULL) {
            collector->exception = 2;
        }
    }
    return 0;
}

/// 合并 C 调用状态、回调异常和字段错误，成功时转移完整 Fields。
inline result<fields> collected_fields(const int succeeded, const verdandi_error& native_failure, field_collector& collector) {
    if (collector.exception == 1) {
        return result<fields>(legacy::error("capacity", "callback"));
    }
    if (collector.exception == 2) {
        return result<fields>(legacy::error("unavailable", "callback"));
    }
    if (!collector.failure.empty()) {
        return result<fields>(collector.failure);
    }
    if (succeeded == 0) {
        return result<fields>(legacy::error::from_native(native_failure));
    }
    return result<fields>(std::move(collector.value));
}

/// 把字符串截断复制进固定 C 错误缓冲区并保证零结尾。
inline void copy_error_text(char* output, const std::size_t capacity, const std::string& value) {
    if (output == NULL || capacity == 0U) {
        return;
    }
    const std::size_t count = std::min(capacity - 1U, value.size());
    if (count != 0U) {
        std::memcpy(output, value.data(), count);
    }
    output[count] = '\0';
}

/// 把可空 C 字符串截断复制进固定错误缓冲区并保证零结尾。
inline void copy_error_text(char* output, const std::size_t capacity, const char* value) {
    if (output == NULL || capacity == 0U) {
        return;
    }
    const std::size_t length = value == NULL ? 0U : std::strlen(value);
    const std::size_t count = std::min(capacity - 1U, length);
    if (count != 0U) {
        std::memcpy(output, value, count);
    }
    output[count] = '\0';
}

/// 把拥有型 Legacy 错误复制回 C ABI 回调输出。
inline void write_native_error(verdandi_error* output, const legacy::error& value) {
    if (output == NULL) {
        return;
    }
    verdandi_error_reset(output);
    copy_error_text(output->code, VERDANDI_C_ERROR_CODE_BYTES, value.code());
    copy_error_text(output->field, VERDANDI_C_ERROR_FIELD_BYTES, value.field());
    copy_error_text(output->detail, VERDANDI_C_ERROR_DETAIL_BYTES, value.detail());
    if (value.has_revision()) {
        output->revision = value.revision();
        output->has_revision = 1U;
    }
}

/// 直接把稳定错误三元组复制回 C ABI 回调输出。
inline void write_native_error(verdandi_error* output, const char* code, const char* field, const char* detail) {
    if (output == NULL) {
        return;
    }
    verdandi_error_reset(output);
    copy_error_text(output->code, VERDANDI_C_ERROR_CODE_BYTES, code);
    copy_error_text(output->field, VERDANDI_C_ERROR_FIELD_BYTES, field);
    copy_error_text(output->detail, VERDANDI_C_ERROR_DETAIL_BYTES, detail);
}

} // namespace detail
} // namespace legacy
} // namespace verdandi

/// 使用成员名作为稳定字段名的 C++11 Schema 描述器。
#define VERDANDI_LEGACY_FIELD(Type, Member) ::verdandi::legacy::make_field<Type>(#Member, &Type::Member)
/// 使用显式稳定字段名的 C++11 Schema 描述器。
#define VERDANDI_LEGACY_NAMED_FIELD(Type, Member, Name) ::verdandi::legacy::make_field<Type>(Name, &Type::Member)
/// 为应用类型声明一次静态成员元组；不引入运行时反射或代码生成。
#define VERDANDI_LEGACY_SCHEMA(Type, ...)                                                                                                                      \
    namespace verdandi {                                                                                                                                       \
    namespace legacy {                                                                                                                                         \
    template <>                                                                                                                                                \
    struct schema<Type> {                                                                                                                                      \
        typedef decltype(std::make_tuple(__VA_ARGS__)) members_type;                                                                                           \
        static const members_type& members() {                                                                                                                 \
            static const members_type value = std::make_tuple(__VA_ARGS__);                                                                                    \
            return value;                                                                                                                                      \
        }                                                                                                                                                      \
    };                                                                                                                                                         \
    }                                                                                                                                                          \
    }
