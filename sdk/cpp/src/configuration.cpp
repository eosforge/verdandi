#include "verdandi/configuration.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>

namespace verdandi {

namespace {

template <class T>
[[nodiscard]] result<void> check_range(const T value, const T minimum, const T maximum, const std::string_view field) {
    if (value < minimum || value > maximum) {
        return std::unexpected(error(code::invalid, std::string(field)));
    }
    return {};
}

/// 与 Go strings.TrimSpace 和 Rust str::trim 对齐的 Unicode White_Space 集合。
[[nodiscard]] bool unicode_whitespace(const std::uint32_t value) noexcept {
    return (value >= 0x09U && value <= 0x0dU) || value == 0x20U || value == 0x85U || value == 0xa0U || value == 0x1680U ||
           (value >= 0x2000U && value <= 0x200aU) || value == 0x2028U || value == 0x2029U || value == 0x202fU || value == 0x205fU || value == 0x3000U;
}

/// 校验标准 UTF-8，拒绝截断、过长编码、代理项和超过 Unicode 上限的码点，并可返回首尾码点。
[[nodiscard]] bool valid_utf8(const std::string_view value, std::uint32_t* first_codepoint = nullptr, std::uint32_t* last_codepoint = nullptr) noexcept {
    bool has_first = false;
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
        if (index + width > value.size()) {
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
        if (!has_first && first_codepoint != nullptr) {
            *first_codepoint = codepoint;
        }
        has_first = true;
        if (last_codepoint != nullptr) {
            *last_codepoint = codepoint;
        }
        index += width;
    }
    return true;
}

[[nodiscard]] bool valid_zone(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= 32 && std::ranges::all_of(value, [](const char raw) {
        const auto character = static_cast<unsigned char>(raw);
        return (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z')) ||
               (character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('z'));
    });
}

[[nodiscard]] bool canonical_text(const std::string_view value) noexcept {
    std::uint32_t first{};
    std::uint32_t last{};
    if (value.empty() || !valid_utf8(value, &first, &last) || unicode_whitespace(first) || unicode_whitespace(last)) {
        return false;
    }
    return value.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool valid_endpoint(const std::string_view value) noexcept {
    if (!canonical_text(value)) {
        return false;
    }
    std::string_view host;
    std::string_view port;
    if (value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string_view::npos || close == 1 || close + 2 > value.size() || value[close + 1] != ':') {
            return false;
        }
        host = value.substr(1, close - 1);
        port = value.substr(close + 2);
    } else {
        const auto separator = value.rfind(':');
        if (separator == std::string_view::npos || separator == 0 || value.find(':') != separator) {
            return false;
        }
        host = value.substr(0, separator);
        port = value.substr(separator + 1);
    }
    if (host.empty() || port.empty()) {
        return false;
    }
    if (std::ranges::any_of(host, [](const char raw) {
            const auto character = static_cast<unsigned char>(raw);
            return character <= static_cast<unsigned char>(' ') || character == 0x7fU;
        })) {
        return false;
    }
    std::uint32_t number{};
    const auto [end, status] = std::from_chars(port.data(), port.data() + port.size(), number);
    return status == std::errc{} && end == port.data() + port.size() && number >= 1 && number <= 65'535;
}

[[nodiscard]] result<void> check_path(const std::filesystem::path& value, const std::string_view field, const bool allow_empty) {
    try {
        const auto native = value.generic_u8string();
        const std::string_view text(reinterpret_cast<const char*>(native.data()), native.size());
        if (text.empty()) {
            if (allow_empty) {
                return {};
            }
            return std::unexpected(error(code::invalid, std::string(field)));
        }
        if (!valid_utf8(text) || text.size() > 4'096 || text.find('\0') != std::string::npos) {
            return std::unexpected(error(code::invalid, std::string(field)));
        }
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::invalid, std::string(field)).with_detail(exception.what()));
    }
}

} // namespace

result<void> tls_configuration::check() const {
    if (!server_name.empty() && (!canonical_text(server_name) || server_name.size() > 253 || std::ranges::any_of(server_name, [](const char raw) {
            switch (raw) {
            case ' ':
            case '\t':
            case '\n':
            case '\r':
            case '\v':
            case '\f':
                return true;
            default:
                return false;
            }
        }))) {
        return std::unexpected(error(code::invalid, "redis.tls.server_name"));
    }
    if (const auto status = check_path(ca_file, "redis.tls.ca_file", true); !status) {
        return status;
    }
    if (const auto status = check_path(cert_file, "redis.tls.cert_file", true); !status) {
        return status;
    }
    if (const auto status = check_path(key_file, "redis.tls.key_file", true); !status) {
        return status;
    }
    if (!enabled) {
        if (!system_roots || !server_name.empty() || !ca_file.empty() || !cert_file.empty() || !key_file.empty()) {
            return std::unexpected(error(code::invalid, "redis.tls"));
        }
        return {};
    }
    if (!system_roots && ca_file.empty()) {
        return std::unexpected(error(code::invalid, "redis.tls.ca_file"));
    }
    if (cert_file.empty() != key_file.empty()) {
        return std::unexpected(error(code::invalid, "redis.tls.cert_file"));
    }
    return {};
}

result<void> pool_configuration::check() const {
    if (const auto status = check_range(min_connections, std::size_t{1}, std::size_t{1'024}, "redis.pool.min_connections"); !status) {
        return status;
    }
    if (const auto status = check_range(max_connections, std::size_t{1}, std::size_t{1'024}, "redis.pool.max_connections"); !status) {
        return status;
    }
    if (max_connections < min_connections) {
        return std::unexpected(error(code::invalid, "redis.pool.min_connections"));
    }
    return check_range(idle_timeout, std::chrono::milliseconds{1'000}, std::chrono::milliseconds{3'600'000}, "redis.pool.idle_timeout_ms");
}

result<void> redis_reconnect_configuration::check() const {
    return check_range(delay, std::chrono::milliseconds{10}, std::chrono::milliseconds{30'000}, "redis.reconnect.delay_ms");
}

result<void> reconnect_configuration::check(const std::string_view prefix) const {
    const auto field = [&](const std::string_view suffix) {
        std::string value(prefix);
        value.push_back('.');
        value.append(suffix);
        return value;
    };
    if (const auto status = check_range(initial_delay, std::chrono::milliseconds{10}, std::chrono::milliseconds{5'000}, field("initial_delay_ms")); !status) {
        return status;
    }
    if (const auto status = check_range(max_delay, std::chrono::milliseconds{100}, std::chrono::milliseconds{30'000}, field("max_delay_ms")); !status) {
        return status;
    }
    if (max_delay < initial_delay) {
        return std::unexpected(error(code::invalid, field("initial_delay_ms")));
    }
    if (const auto status = check_range(multiplier, std::uint32_t{1}, std::uint32_t{8}, field("multiplier")); !status) {
        return status;
    }
    return check_range(jitter_percent, std::uint8_t{0}, std::uint8_t{50}, field("jitter_percent"));
}

result<void> redis_configuration::check() const {
    if (addresses.empty()) {
        return std::unexpected(error(code::invalid, "redis.addresses"));
    }
    if (!std::ranges::all_of(addresses, valid_endpoint)) {
        return std::unexpected(error(code::invalid, "redis.addresses"));
    }
    switch (mode) {
    case redis_mode::standalone:
        if (addresses.size() != 1) {
            return std::unexpected(error(code::invalid, "redis.addresses"));
        }
        if (!master_name.empty()) {
            return std::unexpected(error(code::invalid, "redis.master_name"));
        }
        if (!sentinel_auth.username.empty() || !sentinel_auth.password.empty()) {
            return std::unexpected(error(code::invalid, "redis.sentinel_auth"));
        }
        break;
    case redis_mode::sentinel:
        if (!canonical_text(master_name)) {
            return std::unexpected(error(code::invalid, "redis.master_name"));
        }
        if (tls.enabled && tls.server_name.empty()) {
            return std::unexpected(error(code::invalid, "redis.tls.server_name"));
        }
        break;
    default:
        return std::unexpected(error(code::invalid, "redis.mode"));
    }
    if (database > 255) {
        return std::unexpected(error(code::invalid, "redis.database"));
    }
    if (const auto status = tls.check(); !status) {
        return status;
    }
    if (const auto status = check_range(timeout, std::chrono::milliseconds{10}, std::chrono::milliseconds{15'000}, "redis.timeout_ms"); !status) {
        return status;
    }
    if (const auto status = check_range(connect_timeout, std::chrono::milliseconds{20}, std::chrono::milliseconds{30'000}, "redis.connect_timeout_ms");
        !status) {
        return status;
    }
    if (const auto status = pool.check(); !status) {
        return status;
    }
    return reconnect.check();
}

result<void> registration_policy_configuration::check() const {
    if (const auto status = check_range(attr_max_fields, std::size_t{1}, std::size_t{128}, "registration.policy.attr_max_fields"); !status) {
        return status;
    }
    if (const auto status = check_range(data_max_fields, std::size_t{1}, std::size_t{128}, "registration.policy.data_max_fields"); !status) {
        return status;
    }
    if (const auto status = check_range(field_name_max_bytes, std::size_t{1}, std::size_t{64}, "registration.policy.field_name_max_bytes"); !status) {
        return status;
    }
    if (const auto status = check_range(attr_value_max_bytes, std::size_t{1}, std::size_t{16'384}, "registration.policy.attr_value_max_bytes"); !status) {
        return status;
    }
    if (const auto status = check_range(data_value_max_bytes, std::size_t{1}, std::size_t{16'384}, "registration.policy.data_value_max_bytes"); !status) {
        return status;
    }
    if (const auto status = check_range(record_max_bytes, std::size_t{1}, std::size_t{65'536}, "registration.policy.record_max_bytes"); !status) {
        return status;
    }
    return check_range(refresh_interval, std::chrono::milliseconds{1'000}, std::chrono::milliseconds{86'400'000}, "registration.policy.refresh_ms");
}

result<void> selector_configuration::check() const {
    if (const auto status = check_range(scan_page_size, std::size_t{1}, std::size_t{1'024}, "registration.selector.scan_page_size"); !status) {
        return status;
    }
    if (const auto status = check_range(max_pending_entries, std::size_t{1}, std::size_t{65'536}, "registration.selector.max_pending_entries"); !status) {
        return status;
    }
    if (const auto status = check_range(max_pending_bytes, std::size_t{1}, std::size_t{1'073'741'824}, "registration.selector.max_pending_bytes"); !status) {
        return status;
    }
    if (const auto status = check_range(view_publish_interval, std::chrono::milliseconds{0}, std::chrono::milliseconds{1'000},
                                        "registration.selector.view_publish_interval_ms");
        !status) {
        return status;
    }
    if (const auto status =
            check_range(sync_timeout, std::chrono::milliseconds{100}, std::chrono::milliseconds{3'600'000}, "registration.selector.sync_timeout_ms");
        !status) {
        return status;
    }
    if (const auto status = check_range(max_active_bytes, std::size_t{1}, std::size_t{1'073'741'824}, "registration.selector.max_active_bytes"); !status) {
        return status;
    }
    if (const auto status = check_range(max_retained_bytes, std::size_t{0}, std::size_t{1'073'741'824}, "registration.selector.max_retained_bytes"); !status) {
        return status;
    }
    if (const auto status = check_range(clock_refresh_interval, std::chrono::milliseconds{1'000}, std::chrono::milliseconds{3'600'000},
                                        "registration.selector.clock_refresh_interval_ms");
        !status) {
        return status;
    }
    if (const auto status =
            check_range(clock_uncertainty, std::chrono::milliseconds{0}, std::chrono::milliseconds{1'000}, "registration.selector.clock_uncertainty_ms");
        !status) {
        return status;
    }
    if (const auto status = check_range(error_buffer_capacity, std::size_t{1}, std::size_t{1'024}, "registration.selector.error_buffer_capacity"); !status) {
        return status;
    }
    return recovery.check("registration.selector.recovery");
}

result<void> registration_configuration::check() const {
    if (!valid_zone(zone)) {
        return std::unexpected(error(code::invalid, "registration.zone"));
    }
    if (const auto status = check_range(buffer_capacity, std::size_t{1}, std::size_t{256}, "registration.buffer_capacity"); !status) {
        return status;
    }
    if (const auto status = check_range(error_buffer_capacity, std::size_t{1}, std::size_t{1'024}, "registration.error_buffer_capacity"); !status) {
        return status;
    }
    if (const auto status =
            check_range(min_renew_interval, std::chrono::milliseconds{10}, std::chrono::milliseconds{60'000}, "registration.min_renew_interval_ms");
        !status) {
        return status;
    }
    if (const auto status = check_range(renew_jitter_percent, std::uint8_t{0}, std::uint8_t{50}, "registration.renew_jitter_percent"); !status) {
        return status;
    }
    if (const auto status = check_range(policy_refresh_jitter_percent, std::uint8_t{0}, std::uint8_t{50}, "registration.policy_refresh_jitter_percent");
        !status) {
        return status;
    }
    if (const auto status = policy.check(); !status) {
        return status;
    }
    return selector.check();
}

result<void> catalog_configuration::check() const {
    if (!valid_zone(zone)) {
        return std::unexpected(error(code::invalid, "catalog.zone"));
    }
    if (const auto status = check_range(sync_timeout, std::chrono::milliseconds{100}, std::chrono::milliseconds{3'600'000}, "catalog.sync_timeout_ms");
        !status) {
        return status;
    }
    if (const auto status = check_range(scan_page_size, std::size_t{1}, std::size_t{4'096}, "catalog.scan_page_size"); !status) {
        return status;
    }
    if (const auto status = check_range(max_inflight_reads, std::size_t{1}, std::size_t{256}, "catalog.max_inflight_reads"); !status) {
        return status;
    }
    if (const auto status = check_range(event_buffer_capacity, std::size_t{1}, std::size_t{65'536}, "catalog.event_buffer_capacity"); !status) {
        return status;
    }
    if (const auto status = check_range(error_buffer_capacity, std::size_t{1}, std::size_t{4'096}, "catalog.error_buffer_capacity"); !status) {
        return status;
    }
    if (const auto status = check_range(max_view_bytes, std::uint64_t{0}, std::uint64_t{68'719'476'736}, "catalog.max_view_bytes"); !status) {
        return status;
    }
    if (const auto status = check_range(max_record_bytes, std::size_t{1}, std::size_t{4'194'304}, "catalog.max_record_bytes"); !status) {
        return status;
    }
    if (const auto status = recovery.check("catalog.recovery"); !status) {
        return status;
    }
    return check_path(local_store_path, "catalog.local_store_path", true);
}

result<void> configuration::check() const {
    if (version != "v1") {
        return std::unexpected(error(code::invalid, "version"));
    }
    if (const auto status = redis.check(); !status) {
        return status;
    }
    if (registration_enabled) {
        if (const auto status = registration.check(); !status) {
            return status;
        }
    }
    if (catalog_enabled) {
        if (const auto status = catalog.check(); !status) {
            return status;
        }
    }
    return {};
}

} // namespace verdandi
