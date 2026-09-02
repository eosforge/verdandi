#include "verdandi/configuration.hpp"
#include "verdandi/schema.hpp"

#include <yyjson.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace verdandi {

namespace {

constexpr std::size_t maximum_json_bytes = std::size_t{1'024} * 1'024;

[[nodiscard]] std::string child_path(const std::string_view parent, const std::string_view name) {
    std::string result(parent);
    if (!result.empty()) {
        result.push_back('.');
    }
    result.append(name);
    return result;
}

[[nodiscard]] result<void> read_value(yyjson_val* value, std::string& output, std::string_view) {
    if (!yyjson_is_str(value)) {
        return std::unexpected(error(code::invalid, "json"));
    }
    output.assign(yyjson_get_str(value), yyjson_get_len(value));
    return {};
}

[[nodiscard]] result<void> read_value(yyjson_val* value, std::filesystem::path& output, const std::string_view field) {
    std::string text;
    if (auto status = read_value(value, text, field); !status) {
        return status;
    }
    try {
        const std::u8string utf8(reinterpret_cast<const char8_t*>(text.data()), text.size());
        output = std::filesystem::path(utf8);
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::invalid, std::string(field)).with_detail(exception.what()));
    }
}

[[nodiscard]] result<void> read_value(yyjson_val* value, bool& output, std::string_view) {
    if (!yyjson_is_bool(value)) {
        return std::unexpected(error(code::invalid, "json"));
    }
    output = yyjson_get_bool(value);
    return {};
}

template <class T>
    requires std::unsigned_integral<T> && (!std::same_as<T, bool>)
[[nodiscard]] result<void> read_value(yyjson_val* value, T& output, const std::string_view field) {
    static_assert(std::is_unsigned_v<T>);
    std::uint64_t number{};
    if (yyjson_is_uint(value)) {
        number = yyjson_get_uint(value);
        // Go 和 Rust 的稳定 DTO 使用有符号 64 位整数；超出该解析域属于 JSON 类型错误。
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::unexpected(error(code::invalid, "json"));
        }
    } else if (yyjson_is_sint(value)) {
        const auto signed_number = yyjson_get_sint(value);
        if (signed_number == 0) {
            return std::unexpected(error(code::invalid, "json"));
        }
        if (signed_number < 0) {
            return std::unexpected(error(code::invalid, std::string(field)));
        }
        number = static_cast<std::uint64_t>(signed_number);
    } else {
        return std::unexpected(error(code::invalid, "json"));
    }
    if (number > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        return std::unexpected(error(code::invalid, std::string(field)));
    }
    output = static_cast<T>(number);
    return {};
}

[[nodiscard]] result<void> read_value(yyjson_val* value, std::chrono::milliseconds& output, const std::string_view field) {
    std::uint64_t number{};
    if (auto status = read_value(value, number, field); !status) {
        return status;
    }
    if (number > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count())) {
        return std::unexpected(error(code::invalid, std::string(field)));
    }
    output = std::chrono::milliseconds(number);
    return {};
}

[[nodiscard]] result<void> read_value(yyjson_val* value, std::vector<std::string>& output, const std::string_view field) {
    if (!yyjson_is_arr(value)) {
        return std::unexpected(error(code::invalid, "json"));
    }
    output.clear();
    output.reserve(yyjson_arr_size(value));
    auto iterator = yyjson_arr_iter_with(value);
    while (auto* element = yyjson_arr_iter_next(&iterator)) {
        std::string address;
        if (auto status = read_value(element, address, field); !status) {
            return status;
        }
        output.push_back(std::move(address));
    }
    return {};
}

[[nodiscard]] result<void> read_value(yyjson_val* value, redis_mode& output, const std::string_view field) {
    std::string mode;
    if (auto status = read_value(value, mode, field); !status) {
        return status;
    }
    if (mode == "standalone") {
        output = redis_mode::standalone;
        return {};
    }
    if (mode == "sentinel") {
        output = redis_mode::sentinel;
        return {};
    }
    return std::unexpected(error(code::invalid, std::string(field)));
}

/// 一个编译期 JSON 字段名及其具体解析闭包；闭包类型保持可见，允许编译器完全内联。
template <fixed_string Name, class Callback>
struct object_binding {
    [[no_unique_address]] Callback callback;
    bool seen{false};

    [[nodiscard]] static constexpr std::string_view name() noexcept {
        return Name.view();
    }

    [[nodiscard]] result<void> parse(yyjson_val* value, const std::string_view field) {
        return std::invoke(callback, value, field);
    }
};

/// 把字段名和解析闭包绑定为一个可由折叠表达式展开的条目。
template <fixed_string Name, class Callback>
[[nodiscard]] auto bind_object(Callback&& callback) {
    return object_binding<Name, std::decay_t<Callback>>{std::forward<Callback>(callback)};
}

/// 绑定一个使用其 C++ 原生类型解码器的普通配置字段。
template <fixed_string Name, class T>
[[nodiscard]] auto bind_value(T& output) {
    return bind_object<Name>([&output](yyjson_val* value, const std::string_view field) { return read_value(value, output, field); });
}

/// 绑定一个必须在 JSON 中出现的原生字段，并仅在成功解码后标记存在。
template <fixed_string Name, class Callback>
[[nodiscard]] auto bind_present(bool& present, Callback&& callback) {
    return bind_object<Name>([&present, callback = std::forward<Callback>(callback)](yyjson_val* value, const std::string_view field) mutable -> result<void> {
        auto status = std::invoke(callback, value, field);
        if (status) {
            present = true;
        }
        return status;
    });
}

/// 绑定一个必须在 JSON 中出现的原生字段，并复用其类型解码器。
template <fixed_string Name, class T>
[[nodiscard]] auto bind_required(T& output, bool& present) {
    return bind_present<Name>(present, [&output](yyjson_val* value, const std::string_view field) { return read_value(value, output, field); });
}

/// 严格解析一个对象；字段分派由可变参数和短路折叠表达式在编译期展开。
template <class... Binding>
[[nodiscard]] result<void> parse_object(yyjson_val* value, const std::string_view path, Binding&&... binding) {
    static_assert(sizeof...(Binding) > 0);
    if (!yyjson_is_obj(value)) {
        return std::unexpected(error(code::invalid, "json"));
    }
    auto iterator = yyjson_obj_iter_with(value);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        const auto field = child_path(path, name);
        result<void> status;
        bool duplicate = false;
        const auto dispatch = [&](auto& target) {
            if (name != std::remove_cvref_t<decltype(target)>::name()) {
                return false;
            }
            duplicate = target.seen;
            if (!duplicate) {
                target.seen = true;
                status = target.parse(yyjson_obj_iter_get_val(key), field);
            }
            return true;
        };
        const bool known = (dispatch(binding) || ...);
        if (!known) {
            return std::unexpected(error(code::invalid, "json"));
        }
        if (duplicate) {
            return std::unexpected(error(code::invalid, "json"));
        }
        if (!status) {
            return status;
        }
    }
    return {};
}

[[nodiscard]] result<void> parse_auth(yyjson_val* value, auth_configuration& output, const std::string_view path) {
    return parse_object(value, path, bind_value<"username">(output.username), bind_value<"password">(output.password));
}

[[nodiscard]] result<void> parse_tls(yyjson_val* value, tls_configuration& output, const std::string_view path) {
    return parse_object(value, path, bind_value<"enabled">(output.enabled), bind_value<"system_roots">(output.system_roots),
                        bind_value<"server_name">(output.server_name), bind_value<"ca_file">(output.ca_file), bind_value<"cert_file">(output.cert_file),
                        bind_value<"key_file">(output.key_file));
}

[[nodiscard]] result<void> parse_pool(yyjson_val* value, pool_configuration& output, const std::string_view path) {
    return parse_object(value, path, bind_value<"min_connections">(output.min_connections), bind_value<"max_connections">(output.max_connections),
                        bind_value<"idle_timeout_ms">(output.idle_timeout));
}

[[nodiscard]] result<void> parse_reconnect(yyjson_val* value, reconnect_configuration& output, const std::string_view path) {
    return parse_object(value, path, bind_value<"initial_delay_ms">(output.initial_delay), bind_value<"max_delay_ms">(output.max_delay),
                        bind_value<"multiplier">(output.multiplier), bind_value<"jitter_percent">(output.jitter_percent));
}

[[nodiscard]] result<void> parse_redis_reconnect(yyjson_val* value, redis_reconnect_configuration& output, const std::string_view path) {
    return parse_object(value, path, bind_value<"delay_ms">(output.delay));
}

[[nodiscard]] result<void> parse_redis(yyjson_val* value, redis_configuration& output) {
    bool has_mode = false;
    bool has_addresses = false;
    auto status = parse_object(
        value, "redis", bind_required<"mode">(output.mode, has_mode), bind_required<"addresses">(output.addresses, has_addresses),
        bind_value<"master_name">(output.master_name),
        bind_object<"auth">([&](yyjson_val* member, const std::string_view field) { return parse_auth(member, output.auth, field); }),
        bind_object<"sentinel_auth">([&](yyjson_val* member, const std::string_view field) { return parse_auth(member, output.sentinel_auth, field); }),
        bind_value<"database">(output.database),
        bind_object<"tls">([&](yyjson_val* member, const std::string_view field) { return parse_tls(member, output.tls, field); }),
        bind_value<"timeout_ms">(output.timeout), bind_value<"connect_timeout_ms">(output.connect_timeout),
        bind_object<"pool">([&](yyjson_val* member, const std::string_view field) { return parse_pool(member, output.pool, field); }),
        bind_object<"reconnect">([&](yyjson_val* member, const std::string_view field) { return parse_redis_reconnect(member, output.reconnect, field); }));
    if (!status) {
        return status;
    }
    if (!has_mode) {
        return std::unexpected(error(code::invalid, "redis.mode"));
    }
    if (!has_addresses) {
        return std::unexpected(error(code::invalid, "redis.addresses"));
    }
    return {};
}

[[nodiscard]] result<void> parse_registration_policy(yyjson_val* value, registration_policy_configuration& output, const std::string_view path) {
    return parse_object(value, path, bind_value<"attr_max_fields">(output.attr_max_fields), bind_value<"data_max_fields">(output.data_max_fields),
                        bind_value<"field_name_max_bytes">(output.field_name_max_bytes), bind_value<"attr_value_max_bytes">(output.attr_value_max_bytes),
                        bind_value<"data_value_max_bytes">(output.data_value_max_bytes), bind_value<"record_max_bytes">(output.record_max_bytes),
                        bind_value<"refresh_ms">(output.refresh_interval));
}

[[nodiscard]] result<void> parse_selector(yyjson_val* value, selector_configuration& output, const std::string_view path) {
    return parse_object(
        value, path, bind_value<"scan_page_size">(output.scan_page_size), bind_value<"max_pending_entries">(output.max_pending_entries),
        bind_value<"max_pending_bytes">(output.max_pending_bytes), bind_value<"view_publish_interval_ms">(output.view_publish_interval),
        bind_value<"sync_timeout_ms">(output.sync_timeout), bind_value<"max_active_bytes">(output.max_active_bytes),
        bind_value<"max_retained_bytes">(output.max_retained_bytes), bind_value<"clock_refresh_interval_ms">(output.clock_refresh_interval),
        bind_value<"clock_uncertainty_ms">(output.clock_uncertainty), bind_value<"error_buffer_capacity">(output.error_buffer_capacity),
        bind_object<"recovery">([&](yyjson_val* member, const std::string_view field) { return parse_reconnect(member, output.recovery, field); }));
}

[[nodiscard]] result<void> parse_registration(yyjson_val* value, registration_configuration& output) {
    bool has_zone = false;
    auto status = parse_object(
        value, "registration", bind_required<"zone">(output.zone, has_zone), bind_value<"buffer_capacity">(output.buffer_capacity),
        bind_value<"error_buffer_capacity">(output.error_buffer_capacity), bind_value<"min_renew_interval_ms">(output.min_renew_interval),
        bind_value<"renew_jitter_percent">(output.renew_jitter_percent), bind_value<"policy_refresh_jitter_percent">(output.policy_refresh_jitter_percent),
        bind_object<"policy">([&](yyjson_val* member, const std::string_view field) { return parse_registration_policy(member, output.policy, field); }),
        bind_object<"selector">([&](yyjson_val* member, const std::string_view field) { return parse_selector(member, output.selector, field); }));
    if (!status) {
        return status;
    }
    if (!has_zone) {
        return std::unexpected(error(code::invalid, "registration.zone"));
    }
    return {};
}

[[nodiscard]] result<void> parse_catalog(yyjson_val* value, catalog_configuration& output) {
    bool has_zone = false;
    auto status =
        parse_object(value, "catalog", bind_required<"zone">(output.zone, has_zone), bind_value<"sync_timeout_ms">(output.sync_timeout),
                     bind_value<"scan_page_size">(output.scan_page_size), bind_value<"max_inflight_reads">(output.max_inflight_reads),
                     bind_value<"event_buffer_capacity">(output.event_buffer_capacity), bind_value<"error_buffer_capacity">(output.error_buffer_capacity),
                     bind_value<"max_view_bytes">(output.max_view_bytes), bind_value<"max_record_bytes">(output.max_record_bytes),
                     bind_object<"recovery">([&](yyjson_val* member, const std::string_view field) { return parse_reconnect(member, output.recovery, field); }),
                     bind_object<"local_store_path">([&](yyjson_val* member, const std::string_view field) -> result<void> {
                         if (auto parsed = read_value(member, output.local_store_path, field); !parsed) {
                             return parsed;
                         }
                         // 省略字段表示关闭本地持久化；显式空字符串通常是部署配置错误。
                         if (output.local_store_path.empty()) {
                             return std::unexpected(error(code::invalid, std::string(field)));
                         }
                         return {};
                     }));
    if (!status) {
        return status;
    }
    if (!has_zone) {
        return std::unexpected(error(code::invalid, "catalog.zone"));
    }
    return {};
}

/// 迭代拒绝 JSON null，使“字段省略采用默认值”不会与显式空值产生第四种语义，并避免深层无关输入消耗 C++ 调用栈。
[[nodiscard]] bool contains_null(yyjson_val* value) {
    std::vector<yyjson_val*> pending;
    pending.reserve(32);
    pending.push_back(value);
    while (!pending.empty()) {
        auto* current = pending.back();
        pending.pop_back();
        if (yyjson_is_null(current)) {
            return true;
        }
        if (yyjson_is_arr(current)) {
            auto iterator = yyjson_arr_iter_with(current);
            while (auto* element = yyjson_arr_iter_next(&iterator)) {
                pending.push_back(element);
            }
            continue;
        }
        if (yyjson_is_obj(current)) {
            auto iterator = yyjson_obj_iter_with(current);
            while (auto* key = yyjson_obj_iter_next(&iterator)) {
                pending.push_back(yyjson_obj_iter_get_val(key));
            }
        }
    }
    return false;
}

} // namespace

result<configuration> configuration::from_json(const std::span<const std::byte> source) {
    if (source.empty()) {
        return std::unexpected(error(code::invalid, "json"));
    }
    if (source.size() > maximum_json_bytes) {
        return std::unexpected(error(code::capacity, "json"));
    }
    try {
        std::string input(reinterpret_cast<const char*>(source.data()), source.size());
        yyjson_read_err parse_error{};
        std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(
            yyjson_read_opts(input.data(), input.size(), YYJSON_READ_NOFLAG, nullptr, &parse_error), &yyjson_doc_free);
        if (!document) {
            return std::unexpected(error(code::invalid, "json").with_detail(parse_error.msg == nullptr ? "invalid JSON" : parse_error.msg));
        }

        auto* root = yyjson_doc_get_root(document.get());
        if (contains_null(root)) {
            return std::unexpected(error(code::invalid, "json"));
        }

        configuration output;
        bool has_version = false;
        bool has_redis = false;
        auto status = parse_object(
            root, "", bind_required<"version">(output.version, has_version),
            bind_present<"redis">(has_redis, [&](yyjson_val* member, std::string_view) { return parse_redis(member, output.redis); }),
            bind_present<"registration">(output.registration_enabled,
                                         [&](yyjson_val* member, std::string_view) { return parse_registration(member, output.registration); }),
            bind_present<"catalog">(output.catalog_enabled, [&](yyjson_val* member, std::string_view) { return parse_catalog(member, output.catalog); }));
        if (!status) {
            return std::unexpected(status.error());
        }
        if (!has_version) {
            return std::unexpected(error(code::invalid, "version"));
        }
        if (!has_redis) {
            return std::unexpected(error(code::invalid, "redis.mode"));
        }
        if (auto checked = output.check(); !checked) {
            return std::unexpected(checked.error());
        }
        return output;
    } catch (const std::bad_alloc&) {
        return std::unexpected(error(code::capacity, "json"));
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::invalid, "json").with_detail(exception.what()));
    }
}

result<configuration> configuration::load_json(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(error(code::invalid, "path"));
    }
    std::ifstream input;
    try {
        input.open(path, std::ios::binary);
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::unavailable, "path").with_detail(exception.what()));
    }
    if (!input) {
        return std::unexpected(error(code::unavailable, "path"));
    }

    try {
        std::vector<char> buffer(maximum_json_bytes + 1);
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) {
            return std::unexpected(error(code::invalid, "json"));
        }
        if (static_cast<std::size_t>(count) > maximum_json_bytes) {
            return std::unexpected(error(code::capacity, "json"));
        }
        if (input.bad()) {
            return std::unexpected(error(code::unavailable, "json"));
        }
        const auto* first = reinterpret_cast<const std::byte*>(buffer.data());
        return from_json(std::span(first, static_cast<std::size_t>(count)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(error(code::capacity, "json"));
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::unavailable, "json").with_detail(exception.what()));
    }
}

} // namespace verdandi
