#include "internal/selector.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace verdandi::registration::detail {

namespace {

constexpr std::size_t max_event_bytes = 128ULL * 1'024ULL;

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index{};
    while (index < value.size()) {
        const auto lead = static_cast<unsigned char>(value[index++]);
        if (lead <= 0x7fU) {
            continue;
        }
        std::size_t continuation{};
        std::uint32_t codepoint{};
        if ((lead & 0xe0U) == 0xc0U) {
            continuation = 1;
            codepoint = lead & 0x1fU;
        } else if ((lead & 0xf0U) == 0xe0U) {
            continuation = 2;
            codepoint = lead & 0x0fU;
        } else if ((lead & 0xf8U) == 0xf0U) {
            continuation = 3;
            codepoint = lead & 0x07U;
        } else {
            return false;
        }
        if (continuation > value.size() - index) {
            return false;
        }
        for (std::size_t count = 0; count < continuation; ++count) {
            const auto next = static_cast<unsigned char>(value[index++]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        const auto minimum = continuation == 1 ? 0x80U : continuation == 2 ? 0x800U : 0x10000U;
        if (codepoint < minimum || codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_uuid(const std::string_view value) noexcept {
    return value.size() == 32 &&
           std::ranges::all_of(value, [](const char character) { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); });
}

[[nodiscard]] std::size_t decimal_digits(std::uint64_t value) noexcept {
    std::size_t output{1};
    while (value >= 10) {
        value /= 10;
        ++output;
    }
    return output;
}

[[nodiscard]] std::size_t record_size(const selector_record& value) noexcept {
    std::size_t output = 5 + value.meta.uuid.size() + 9 + decimal_digits(value.meta.revision) + 10 + decimal_digits(value.meta.timestamp) + 4 +
                         decimal_digits(value.meta.ttl) + 8 + decimal_digits(value.meta.version);
    for (const auto& [name, field] : value.attr) {
        output += 1 + name.size() + field.size();
    }
    for (const auto& [name, field] : value.data) {
        output += name.size() + field.size();
    }
    return output;
}

[[nodiscard]] bytes copy_bytes(const std::string_view value) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

class messagepack_cursor final {
public:
    explicit messagepack_cursor(const std::string_view source) noexcept : source_(source) {}

    /// 读取顶层数组长度；本协议不接受 Map 或嵌套容器。
    [[nodiscard]] result<std::size_t> array_size() {
        auto marker = take_byte();
        if (!marker) {
            return std::unexpected(marker.error());
        }
        if ((*marker & 0xf0U) == 0x90U) {
            return static_cast<std::size_t>(*marker & 0x0fU);
        }
        if (*marker == 0xdcU) {
            auto encoded = take(2);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            return static_cast<std::size_t>(read_big<std::uint16_t>(*encoded));
        }
        if (*marker == 0xddU) {
            auto encoded = take(4);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            const auto value = read_big<std::uint32_t>(*encoded);
            if (value > max_event_bytes) {
                return std::unexpected(error(code::capacity, "event"));
            }
            return static_cast<std::size_t>(value);
        }
        return std::unexpected(error(code::corrupt, "event"));
    }

    /// 读取 String 或 Binary 标量，并返回只在游标存活期间有效的窗口。
    [[nodiscard]] result<std::string_view> binary() {
        auto marker = take_byte();
        if (!marker) {
            return std::unexpected(marker.error());
        }
        std::size_t length{};
        if ((*marker & 0xe0U) == 0xa0U) {
            length = static_cast<std::size_t>(*marker & 0x1fU);
        } else {
            std::size_t width{};
            switch (*marker) {
            case 0xc4U:
            case 0xd9U:
                width = 1;
                break;
            case 0xc5U:
            case 0xdaU:
                width = 2;
                break;
            case 0xc6U:
            case 0xdbU:
                width = 4;
                break;
            default:
                return std::unexpected(error(code::corrupt, "event"));
            }
            auto encoded = take(width);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            if (width == 1) {
                length = static_cast<unsigned char>((*encoded)[0]);
            } else if (width == 2) {
                length = read_big<std::uint16_t>(*encoded);
            } else {
                const auto value = read_big<std::uint32_t>(*encoded);
                if (value > max_event_bytes) {
                    return std::unexpected(error(code::capacity, "event"));
                }
                length = static_cast<std::size_t>(value);
            }
        }
        if (length > max_event_bytes) {
            return std::unexpected(error(code::capacity, "event"));
        }
        return take(length);
    }

    /// 读取一个正 MessagePack 整数并约束到协议安全整数范围。
    [[nodiscard]] result<std::uint64_t> positive_integer() {
        auto marker = take_byte();
        if (!marker) {
            return std::unexpected(marker.error());
        }
        std::uint64_t value{};
        if (*marker <= 0x7fU) {
            value = *marker;
        } else {
            std::size_t width{};
            bool signed_value{false};
            switch (*marker) {
            case 0xccU:
                width = 1;
                break;
            case 0xcdU:
                width = 2;
                break;
            case 0xceU:
                width = 4;
                break;
            case 0xcfU:
                width = 8;
                break;
            case 0xd0U:
                width = 1;
                signed_value = true;
                break;
            case 0xd1U:
                width = 2;
                signed_value = true;
                break;
            case 0xd2U:
                width = 4;
                signed_value = true;
                break;
            case 0xd3U:
                width = 8;
                signed_value = true;
                break;
            default:
                return std::unexpected(error(code::invalid, "event_integer"));
            }
            auto encoded = take(width);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            value = read_unsigned(*encoded);
            if (signed_value) {
                const auto sign_bit = std::uint64_t{1} << (width * 8U - 1U);
                if ((value & sign_bit) != 0) {
                    return std::unexpected(error(code::invalid, "event_integer"));
                }
            }
        }
        if (value == 0 || value > safe_integer_max) {
            return std::unexpected(error(code::invalid, "event_integer"));
        }
        return value;
    }

    /// 跳过一个未知控制字段的有界标量，拒绝任意嵌套对象。
    [[nodiscard]] result<void> skip_scalar() {
        auto marker = take_byte();
        if (!marker) {
            return std::unexpected(marker.error());
        }
        if (*marker <= 0x7fU || *marker >= 0xe0U || *marker == 0xc0U || *marker == 0xc2U || *marker == 0xc3U) {
            return {};
        }
        if ((*marker & 0xe0U) == 0xa0U) {
            return discard(static_cast<std::size_t>(*marker & 0x1fU));
        }
        std::size_t width{};
        switch (*marker) {
        case 0xc4U:
        case 0xd9U:
            width = 1;
            break;
        case 0xc5U:
        case 0xdaU:
            width = 2;
            break;
        case 0xc6U:
        case 0xdbU:
            width = 4;
            break;
        case 0xcaU:
        case 0xceU:
        case 0xd2U:
            return discard(4);
        case 0xcbU:
        case 0xcfU:
        case 0xd3U:
            return discard(8);
        case 0xccU:
        case 0xd0U:
            return discard(1);
        case 0xcdU:
        case 0xd1U:
            return discard(2);
        default:
            return std::unexpected(error(code::corrupt, "event_scalar"));
        }
        auto encoded = take(width);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        std::size_t length{};
        if (width == 1) {
            length = static_cast<unsigned char>((*encoded)[0]);
        } else if (width == 2) {
            length = read_big<std::uint16_t>(*encoded);
        } else {
            const auto value = read_big<std::uint32_t>(*encoded);
            if (value > max_event_bytes) {
                return std::unexpected(error(code::capacity, "event_scalar"));
            }
            length = static_cast<std::size_t>(value);
        }
        return discard(length);
    }

    [[nodiscard]] bool done() const noexcept {
        return offset_ == source_.size();
    }

private:
    template <class T>
    [[nodiscard]] static T read_big(const std::string_view value) noexcept {
        T output{};
        for (const char raw : value) {
            const auto character = static_cast<unsigned char>(raw);
            output = static_cast<T>((output << 8U) | character);
        }
        return output;
    }

    [[nodiscard]] static std::uint64_t read_unsigned(const std::string_view value) noexcept {
        std::uint64_t output{};
        for (const char raw : value) {
            const auto character = static_cast<unsigned char>(raw);
            output = (output << 8U) | character;
        }
        return output;
    }

    [[nodiscard]] result<unsigned char> take_byte() {
        if (offset_ >= source_.size()) {
            return std::unexpected(error(code::corrupt, "event"));
        }
        return static_cast<unsigned char>(source_[offset_++]);
    }

    [[nodiscard]] result<std::string_view> take(const std::size_t length) {
        if (length > source_.size() - offset_) {
            return std::unexpected(error(code::corrupt, "event"));
        }
        const auto output = source_.substr(offset_, length);
        offset_ += length;
        return output;
    }

    [[nodiscard]] result<void> discard(const std::size_t length) {
        auto value = take(length);
        if (!value) {
            return std::unexpected(value.error());
        }
        return {};
    }

    std::string_view source_;
    std::size_t offset_{};
};

enum class event_kind : std::uint8_t {
    register_value,
    update,
    renew,
    unregister,
};

struct registration_event {
    event_kind kind{event_kind::register_value};
    std::string uuid;
    std::uint64_t revision{};
    std::uint64_t base_revision{};
    std::uint64_t timestamp{};
    std::uint64_t ttl{};
    std::uint64_t version{};
    bool has_version{false};
    fields attr;
    fields data;
    std::size_t size{};
};

/// 单遍解码受限 MessagePack 交替数组，并在分配应用字段前执行协议上限检查。
[[nodiscard]] result<registration_event> decode_event(const std::string_view payload, const policy& limits) {
    if (payload.empty() || payload.size() > max_event_bytes) {
        return std::unexpected(error(code::capacity, "event"));
    }
    messagepack_cursor cursor(payload);
    auto elements = cursor.array_size();
    if (!elements || *elements % 2 != 0 || *elements > (128ULL + 128ULL + 7ULL) * 2ULL) {
        return std::unexpected(elements ? error(code::capacity, "event") : elements.error());
    }

    registration_event output;
    output.size = payload.size();
    std::unordered_set<std::string> seen;
    seen.reserve(*elements / 2);
    std::string kind;
    bool protocol_seen{false};
    bool kind_seen{false};
    bool uuid_seen{false};
    for (std::size_t index = 0; index < *elements; index += 2) {
        auto encoded_name = cursor.binary();
        if (!encoded_name || encoded_name->empty() || !valid_utf8(*encoded_name)) {
            return std::unexpected(error(code::corrupt, "event"));
        }
        std::string name(*encoded_name);
        if (!seen.emplace(name).second) {
            return std::unexpected(error(code::contract, std::move(name)));
        }
        if (name == "&protocol") {
            auto value = cursor.binary();
            if (!value || *value != "v1") {
                return std::unexpected(error(code::protocol, "&protocol"));
            }
            protocol_seen = true;
        } else if (name == "&kind") {
            auto value = cursor.binary();
            if (!value) {
                return std::unexpected(error(code::corrupt, "&kind"));
            }
            kind.assign(*value);
            kind_seen = true;
        } else if (name == "@uuid") {
            auto value = cursor.binary();
            if (!value || !valid_uuid(*value)) {
                return std::unexpected(error(code::invalid, "@uuid"));
            }
            output.uuid.assign(*value);
            uuid_seen = true;
        } else if (name == "@revision") {
            auto value = cursor.positive_integer();
            if (!value) {
                return std::unexpected(error(code::invalid, "@revision"));
            }
            output.revision = *value;
        } else if (name == "@timestamp") {
            auto value = cursor.positive_integer();
            if (!value) {
                return std::unexpected(error(code::invalid, "@timestamp"));
            }
            output.timestamp = *value;
        } else if (name == "@ttl") {
            auto value = cursor.positive_integer();
            if (!value) {
                return std::unexpected(error(code::invalid, "@ttl"));
            }
            output.ttl = *value;
        } else if (name == "@version") {
            auto value = cursor.positive_integer();
            if (!value) {
                return std::unexpected(error(code::invalid, "@version"));
            }
            output.version = *value;
            output.has_version = true;
        } else if (name.front() == '&' || name.front() == '@') {
            if (auto status = cursor.skip_scalar(); !status) {
                return std::unexpected(status.error());
            }
        } else {
            auto value = cursor.binary();
            if (!value) {
                return std::unexpected(error(code::corrupt, name));
            }
            const bool attribute = name.front() == '.';
            std::string field = attribute ? name.substr(1) : name;
            const auto value_limit = attribute ? limits.attr_value_max_bytes : limits.data_value_max_bytes;
            if (field.empty() || field.size() > limits.field_name_max_bytes || !valid_utf8(field) || field.front() == '&' || field.front() == '@' ||
                field.front() == '.' || value->size() > value_limit) {
                return std::unexpected(error(value->size() > value_limit ? code::capacity : code::invalid, std::move(field)));
            }
            auto& destination = attribute ? output.attr : output.data;
            if (!destination.emplace(std::move(field), copy_bytes(*value)).second) {
                return std::unexpected(error(code::contract, name));
            }
        }
    }
    if (!cursor.done() || !protocol_seen || !kind_seen || !uuid_seen) {
        return std::unexpected(error(code::corrupt, "event"));
    }

    if (kind == "register") {
        output.kind = event_kind::register_value;
        if (output.revision == 0 || output.timestamp == 0 || output.ttl == 0 || !output.has_version) {
            return std::unexpected(error(code::contract, "register"));
        }
        if (auto status = validate_record(output.uuid, output.revision, output.ttl, output.version, output.attr, output.data, limits); !status) {
            return std::unexpected(status.error());
        }
    } else if (kind == "update") {
        output.kind = event_kind::update;
        if (output.revision == 0 || output.timestamp == 0 || !output.attr.empty() || (!output.has_version && output.data.empty())) {
            return std::unexpected(error(code::contract, "update"));
        }
        output.base_revision = output.revision - 1;
    } else if (kind == "renew") {
        output.kind = event_kind::renew;
        if (output.revision == 0 || output.timestamp == 0 || output.ttl != 0 || output.has_version || !output.attr.empty() || !output.data.empty()) {
            return std::unexpected(error(code::contract, "renew"));
        }
    } else if (kind == "unregister") {
        output.kind = event_kind::unregister;
        if (output.revision != 0 || output.timestamp != 0 || output.ttl != 0 || output.has_version || !output.attr.empty() || !output.data.empty()) {
            return std::unexpected(error(code::contract, "unregister"));
        }
    } else {
        return std::unexpected(error(code::invalid, "&kind"));
    }
    return output;
}

class pending_events final {
public:
    pending_events(const std::size_t entry_limit, const std::size_t byte_limit) : entry_limit_(entry_limit), byte_limit_(byte_limit) {}

    /// 按 UUID 合并连续 Update/Renew，保证高频单 Registration 不会线性占用同步缓存。
    [[nodiscard]] result<void> add(registration_event value) {
        const auto size = value.size;
        auto [iterator, inserted] = values_.try_emplace(value.uuid);
        if (inserted) {
            if (values_.size() > entry_limit_) {
                values_.erase(iterator);
                return std::unexpected(error(code::capacity, "selector_pending"));
            }
        }
        auto& sequence = iterator->second;
        if (value.kind == event_kind::register_value || value.kind == event_kind::unregister) {
            for (const auto& previous : sequence) {
                bytes_ -= previous.size;
            }
            sequence.clear();
            sequence.push_back(std::move(value));
            bytes_ += size;
        } else if (!sequence.empty() && merge(sequence.back(), value)) {
            bytes_ -= sequence.back().size;
            sequence.back().size = std::max(sequence.back().size, size);
            bytes_ += sequence.back().size;
        } else {
            sequence.push_back(std::move(value));
            bytes_ += size;
        }
        if (bytes_ > byte_limit_) {
            return std::unexpected(error(code::capacity, "selector_pending"));
        }
        return {};
    }

    [[nodiscard]] std::vector<registration_event> drain() {
        std::vector<registration_event> output;
        for (auto& [uuid, sequence] : values_) {
            static_cast<void>(uuid);
            for (auto& value : sequence) {
                output.push_back(std::move(value));
            }
        }
        values_.clear();
        bytes_ = 0;
        return output;
    }

private:
    [[nodiscard]] static bool merge(registration_event& current, const registration_event& next) {
        if (next.kind == event_kind::renew && current.revision == next.revision &&
            (current.kind == event_kind::register_value || current.kind == event_kind::update || current.kind == event_kind::renew)) {
            current.timestamp = std::max(current.timestamp, next.timestamp);
            return true;
        }
        if (next.kind != event_kind::update) {
            return false;
        }
        if (current.kind == event_kind::register_value && next.revision > current.revision) {
            for (const auto& [name, value] : next.data) {
                current.data[name] = value;
            }
            current.revision = next.revision;
            current.timestamp = next.timestamp;
            if (next.has_version) {
                current.version = next.version;
            }
            return true;
        }
        if (current.kind == event_kind::update && next.base_revision <= current.revision && next.revision > current.revision) {
            for (const auto& [name, value] : next.data) {
                current.data[name] = value;
            }
            current.revision = next.revision;
            current.timestamp = next.timestamp;
            if (next.has_version) {
                current.version = next.version;
                current.has_version = true;
            }
            return true;
        }
        return false;
    }

    std::map<std::string, std::vector<registration_event>, std::less<>> values_;
    std::size_t entry_limit_;
    std::size_t byte_limit_;
    std::size_t bytes_{};
};

struct redis_clock {
    std::chrono::steady_clock::time_point anchor{};
    std::uint64_t upper{};

    [[nodiscard]] std::uint64_t now() const noexcept {
        if (anchor == std::chrono::steady_clock::time_point{}) {
            return safe_integer_max;
        }
        const auto elapsed = std::chrono::steady_clock::now() - anchor;
        const auto whole = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        const auto rounded = whole + (elapsed > whole ? std::chrono::milliseconds{1} : std::chrono::milliseconds::zero());
        const auto increment = static_cast<std::uint64_t>(std::max<std::int64_t>(0, rounded.count()));
        return upper > safe_integer_max - increment ? safe_integer_max : upper + increment;
    }
};

/// 用 Redis TIME 和完整往返时间构造保守服务端时间上界。
[[nodiscard]] result<redis_clock> calibrate_clock(const std::shared_ptr<client_core>& owner) {
    const auto started = std::chrono::steady_clock::now();
    verdandi::detail::command command("TIME");
    auto response = owner->transport()->execute(command);
    const auto finished = std::chrono::steady_clock::now();
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->type != verdandi::detail::response::kind::array || response->children.size() != 2) {
        return std::unexpected(error(code::corrupt, "TIME"));
    }
    auto seconds = response->children[0].text();
    auto microseconds = response->children[1].text();
    if (!seconds || !microseconds) {
        return std::unexpected(error(code::corrupt, "TIME"));
    }
    auto parsed_seconds = verdandi::detail::parse_unsigned(*seconds, "TIME.seconds");
    auto parsed_microseconds = verdandi::detail::parse_unsigned(*microseconds, "TIME.microseconds", true);
    if (!parsed_seconds || !parsed_microseconds || *parsed_microseconds >= 1'000'000 || *parsed_seconds > safe_integer_max / 1'000) {
        return std::unexpected(error(code::corrupt, "TIME"));
    }
    const auto elapsed = finished - started;
    const auto whole = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    auto margin =
        whole + (elapsed > whole ? std::chrono::milliseconds{1} : std::chrono::milliseconds::zero()) + owner->configuration().selector.clock_uncertainty;
    const auto server = *parsed_seconds * 1'000 + *parsed_microseconds / 1'000;
    const auto margin_value = static_cast<std::uint64_t>(margin.count());
    if (server > safe_integer_max - margin_value) {
        return std::unexpected(error(code::capacity, "redis_clock"));
    }
    return redis_clock{finished, server + margin_value};
}

struct deadline_entry {
    std::uint64_t value{};
    std::string uuid;

    [[nodiscard]] bool operator>(const deadline_entry& other) const noexcept {
        return value > other.value;
    }
};

struct selector_state {
    std::unordered_map<std::string, std::shared_ptr<const selector_record>> active;
    std::unordered_map<std::string, retained_record> retained;
    std::priority_queue<deadline_entry, std::vector<deadline_entry>, std::greater<>> active_deadlines;
    std::priority_queue<deadline_entry, std::vector<deadline_entry>, std::greater<>> retained_deadlines;
    std::size_t active_bytes{};
    std::size_t retained_bytes{};
};

[[nodiscard]] std::shared_ptr<const selector_record> find_record(const selector_state& state, const std::string_view uuid) {
    if (const auto active = state.active.find(std::string(uuid)); active != state.active.end()) {
        return active->second;
    }
    if (const auto retained = state.retained.find(std::string(uuid)); retained != state.retained.end()) {
        return retained->second.record;
    }
    return {};
}

void remove_retained(selector_state& state, const std::string_view uuid) {
    const auto iterator = state.retained.find(std::string(uuid));
    if (iterator == state.retained.end()) {
        return;
    }
    state.retained_bytes -= iterator->second.record->size;
    state.retained.erase(iterator);
}

void remove_record(selector_state& state, const std::string_view uuid) {
    const auto iterator = state.active.find(std::string(uuid));
    if (iterator != state.active.end()) {
        state.active_bytes -= iterator->second->size;
        state.active.erase(iterator);
    }
    remove_retained(state, uuid);
}

/// 安装活动记录并更新惰性截止堆；旧堆项由过期处理时丢弃。
[[nodiscard]] result<void> set_active(selector_state& state, std::shared_ptr<const selector_record> record, const selector_configuration& configuration) {
    std::size_t previous{};
    if (const auto iterator = state.active.find(record->meta.uuid); iterator != state.active.end()) {
        previous = iterator->second->size;
    }
    const auto next = state.active_bytes - previous + record->size;
    if (next > configuration.max_active_bytes) {
        return std::unexpected(error(code::capacity, "selector_view"));
    }
    remove_retained(state, record->meta.uuid);
    state.active_bytes = next;
    state.active.insert_or_assign(record->meta.uuid, record);
    state.active_deadlines.push({record->deadline, record->meta.uuid});
    return {};
}

/// 把记录移动到不可选择 retained 视图，并在超限时优先驱逐最早截止项。
void set_retained(selector_state& state, const std::shared_ptr<const selector_record>& record, std::uint64_t until, const std::uint64_t now,
                  const selector_configuration& configuration) {
    if (const auto active = state.active.find(record->meta.uuid); active != state.active.end()) {
        state.active_bytes -= active->second->size;
        state.active.erase(active);
    }
    remove_retained(state, record->meta.uuid);
    if (configuration.max_retained_bytes == 0 || until <= now) {
        return;
    }
    state.retained.insert_or_assign(record->meta.uuid, retained_record{record, until});
    state.retained_bytes += record->size;
    state.retained_deadlines.push({until, record->meta.uuid});
    while (state.retained_bytes > configuration.max_retained_bytes && !state.retained_deadlines.empty()) {
        const auto earliest = state.retained_deadlines.top();
        state.retained_deadlines.pop();
        const auto iterator = state.retained.find(earliest.uuid);
        if (iterator != state.retained.end() && iterator->second.until == earliest.value) {
            state.retained_bytes -= iterator->second.record->size;
            state.retained.erase(iterator);
        }
    }
}

void retain(selector_state& state, const std::shared_ptr<const selector_record>& record, const std::uint64_t now, const selector_configuration& configuration) {
    const auto until = record->deadline > safe_integer_max - record->meta.ttl ? safe_integer_max : record->deadline + record->meta.ttl;
    set_retained(state, record, until, now, configuration);
}

/// 驱动活动租约和 retained 第二截止；只处理仍与 Map 当前值匹配的惰性堆项。
[[nodiscard]] bool expire(selector_state& state, const std::uint64_t now, const selector_configuration& configuration) {
    bool changed{false};
    while (!state.active_deadlines.empty() && state.active_deadlines.top().value <= now) {
        const auto earliest = state.active_deadlines.top();
        state.active_deadlines.pop();
        const auto iterator = state.active.find(earliest.uuid);
        if (iterator != state.active.end() && iterator->second->deadline == earliest.value) {
            auto record = iterator->second;
            retain(state, record, now, configuration);
            changed = true;
        }
    }
    while (!state.retained_deadlines.empty() && state.retained_deadlines.top().value <= now) {
        const auto earliest = state.retained_deadlines.top();
        state.retained_deadlines.pop();
        const auto iterator = state.retained.find(earliest.uuid);
        if (iterator != state.retained.end() && iterator->second.until == earliest.value) {
            state.retained_bytes -= iterator->second.record->size;
            state.retained.erase(iterator);
            changed = true;
        }
    }
    return changed;
}

[[nodiscard]] selector_state recovery_state(const selector_state& previous, const std::uint64_t now, const selector_configuration& configuration) {
    selector_state output;
    output.active.reserve(previous.active.size());
    output.retained.reserve(previous.active.size() + previous.retained.size());
    for (const auto& [uuid, value] : previous.retained) {
        static_cast<void>(uuid);
        set_retained(output, value.record, value.until, now, configuration);
    }
    for (const auto& [uuid, value] : previous.active) {
        static_cast<void>(uuid);
        retain(output, value, now, configuration);
    }
    return output;
}

[[nodiscard]] result<std::shared_ptr<const selector_record>> parse_record(const std::string_view uuid, const verdandi::detail::response& response,
                                                                          const policy& limits, const projector& project) {
    auto pairs = verdandi::detail::named_pairs(response);
    if (!pairs) {
        return std::unexpected(pairs.error());
    }
    if (pairs->empty()) {
        return std::shared_ptr<const selector_record>{};
    }
    auto record = std::make_shared<selector_record>();
    record->meta.uuid = uuid;
    bool uuid_seen{false};
    bool revision_seen{false};
    bool timestamp_seen{false};
    bool ttl_seen{false};
    bool version_seen{false};
    for (const auto& [name, value] : *pairs) {
        if (name == "@uuid") {
            if (uuid_seen || value != uuid) {
                return std::unexpected(error(code::corrupt, "@uuid"));
            }
            uuid_seen = true;
        } else if (name == "@revision") {
            auto parsed = verdandi::detail::parse_unsigned(value, name);
            if (!parsed || revision_seen) {
                return std::unexpected(error(code::corrupt, "@revision"));
            }
            record->meta.revision = *parsed;
            revision_seen = true;
        } else if (name == "@timestamp") {
            auto parsed = verdandi::detail::parse_unsigned(value, name);
            if (!parsed || timestamp_seen) {
                return std::unexpected(error(code::corrupt, "@timestamp"));
            }
            record->meta.timestamp = *parsed;
            timestamp_seen = true;
        } else if (name == "@ttl") {
            auto parsed = verdandi::detail::parse_unsigned(value, name);
            if (!parsed || ttl_seen) {
                return std::unexpected(error(code::corrupt, "@ttl"));
            }
            record->meta.ttl = *parsed;
            ttl_seen = true;
        } else if (name == "@version") {
            auto parsed = verdandi::detail::parse_unsigned(value, name);
            if (!parsed || version_seen) {
                return std::unexpected(error(code::corrupt, "@version"));
            }
            record->meta.version = *parsed;
            version_seen = true;
        } else if (!name.empty() && name.front() == '.') {
            if (!record->attr.emplace(std::string(name.substr(1)), copy_bytes(value)).second) {
                return std::unexpected(error(code::corrupt, std::string(name)));
            }
        } else {
            if (name.empty() || name.front() == '@' || name.front() == '&') {
                return std::unexpected(error(code::corrupt, std::string(name)));
            }
            if (!record->data.emplace(std::string(name), copy_bytes(value)).second) {
                return std::unexpected(error(code::corrupt, std::string(name)));
            }
        }
    }
    if (!uuid_seen || !revision_seen || !timestamp_seen || !ttl_seen || !version_seen ||
        !validate_record(uuid, record->meta.revision, record->meta.ttl, record->meta.version, record->attr, record->data, limits)) {
        return std::unexpected(error(code::corrupt, "registration"));
    }
    if (record->meta.timestamp > hash_field_deadline_max - record->meta.ttl) {
        return std::unexpected(error(code::corrupt, "@timestamp"));
    }
    record->deadline = record->meta.timestamp + record->meta.ttl;
    auto attr = project.attr(record->attr);
    if (!attr) {
        return std::unexpected(attr.error());
    }
    auto data = project.data(record->data);
    if (!data) {
        return std::unexpected(data.error());
    }
    record->projected_attr = std::move(*attr);
    record->projected_data = std::move(*data);
    record->size = record_size(*record);
    return std::shared_ptr<const selector_record>(std::move(record));
}

[[nodiscard]] std::string registry_key(const std::string_view zone, const std::string_view type) {
    return "verdandi:registry:" + std::string(zone) + ':' + std::string(type);
}

[[nodiscard]] std::string registration_key(const std::string_view zone, const std::string_view type, const std::string_view uuid) {
    return "verdandi:registration:" + std::string(zone) + ':' + std::string(type) + ':' + std::string(uuid);
}

struct fence_gate {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::uint64_t observed{};

    void observe(const std::uint64_t identifier) {
        std::lock_guard lock(mutex);
        observed = std::max(observed, identifier);
        changed.notify_all();
    }

    [[nodiscard]] bool wait(const std::stop_token& stop, const std::uint64_t identifier) {
        std::unique_lock lock(mutex);
        return changed.wait(lock, stop, [&] { return observed >= identifier; });
    }
};

struct sync_output {
    selector_state state;
    redis_clock clock;
};

/// 临时任务执行完整 HSCAN + 分页 Pipeline HGETALL；它只构造私有快照，不修改监听任务状态。
[[nodiscard]] result<sync_output> synchronize(const std::stop_token& stop, const std::shared_ptr<client_core>& owner, const selector_options& options,
                                              const projector& project, const selector_state& previous,
                                              const std::shared_ptr<verdandi::detail::subscription>& subscription, const std::shared_ptr<fence_gate>& gate) {
    auto clock = calibrate_clock(owner);
    if (!clock) {
        return std::unexpected(clock.error());
    }
    const auto limits = owner->limits();
    if (!limits) {
        return std::unexpected(error(code::unavailable, "registration_policy"));
    }
    const auto& configuration = owner->configuration().selector;
    auto state = recovery_state(previous, clock->now(), configuration);
    const auto registry = registry_key(owner->configuration().zone, options.type);
    std::uint64_t cursor{};
    do {
        if (stop.stop_requested()) {
            return std::unexpected(error(code::closed));
        }
        verdandi::detail::command scan("HSCAN");
        scan.add(registry).add(cursor).add("COUNT").add(static_cast<std::uint64_t>(configuration.scan_page_size));
        auto page = owner->transport()->execute(scan);
        if (!page) {
            return std::unexpected(page.error());
        }
        if (page->type != verdandi::detail::response::kind::array || page->children.size() != 2) {
            return std::unexpected(error(code::corrupt, "registry"));
        }
        auto cursor_text = page->children[0].text();
        auto next = cursor_text ? verdandi::detail::parse_unsigned(*cursor_text, "HSCAN.cursor", true)
                                : result<std::uint64_t>(std::unexpected(error(code::corrupt, "HSCAN.cursor")));
        auto hints = verdandi::detail::named_pairs(page->children[1]);
        if (!next || !hints) {
            return std::unexpected(!next ? next.error() : hints.error());
        }
        std::vector<std::pair<std::string, std::uint64_t>> entries;
        entries.reserve(hints->size());
        std::vector<verdandi::detail::command> reads;
        reads.reserve(hints->size());
        for (const auto& [uuid, revision_text] : *hints) {
            auto revision = verdandi::detail::parse_unsigned(revision_text, "registry");
            if (!valid_uuid(uuid) || !revision) {
                return std::unexpected(error(code::corrupt, "registry"));
            }
            entries.emplace_back(uuid, *revision);
            verdandi::detail::command read("HGETALL");
            read.add(registration_key(owner->configuration().zone, options.type, uuid));
            reads.push_back(std::move(read));
        }
        if (!reads.empty()) {
            auto records = owner->transport()->execute(reads);
            if (!records) {
                return std::unexpected(records.error());
            }
            for (std::size_t index = 0; index < records->size(); ++index) {
                auto record = parse_record(entries[index].first, (*records)[index], *limits, project);
                if (!record) {
                    return std::unexpected(record.error());
                }
                if (!*record) {
                    continue;
                }
                if ((*record)->meta.revision < entries[index].second) {
                    return std::unexpected(error(code::transition, "@revision", (*record)->meta.revision, {}));
                }
                if ((*record)->deadline <= clock->now()) {
                    retain(state, *record, clock->now(), configuration);
                } else if (auto status = set_active(state, *record, configuration); !status) {
                    return std::unexpected(status.error());
                }
            }
        }
        cursor = *next;
    } while (cursor != 0);

    auto fence = subscription->fence();
    if (!fence) {
        return std::unexpected(fence.error());
    }
    if (!gate->wait(stop, *fence)) {
        return std::unexpected(error(code::closed));
    }
    static_cast<void>(expire(state, clock->now(), configuration));
    return sync_output{std::move(state), *clock};
}

struct active_sync {
    std::shared_ptr<fence_gate> gate{std::make_shared<fence_gate>()};
    std::future<result<sync_output>> completed;
    std::jthread worker;
};

[[nodiscard]] std::unique_ptr<active_sync> start_sync(const std::shared_ptr<client_core>& owner, const selector_options& options, const projector& project,
                                                      const selector_state& previous, const std::shared_ptr<verdandi::detail::subscription>& subscription) {
    auto output = std::make_unique<active_sync>();
    auto promise = std::make_shared<std::promise<result<sync_output>>>();
    output->completed = promise->get_future();
    const auto gate = output->gate;
    output->worker = std::jthread([owner, options, project, previous, subscription, gate, promise](const std::stop_token& stop) {
        try {
            promise->set_value(synchronize(stop, owner, options, project, previous, subscription, gate));
        } catch (const std::exception& exception) {
            promise->set_value(std::unexpected(error(code::unavailable, "selector_sync").with_detail(exception.what())));
        } catch (...) {
            promise->set_value(std::unexpected(error(code::unavailable, "selector_sync")));
        }
    });
    return output;
}

struct apply_result {
    bool changed{false};
    bool repair{false};
};

[[nodiscard]] result<std::shared_ptr<const selector_record>> record_from_event(const registration_event& event, const policy& limits,
                                                                               const projector& project) {
    if (event.timestamp > hash_field_deadline_max - event.ttl) {
        return std::unexpected(error(code::corrupt, "@timestamp"));
    }
    auto output = std::make_shared<selector_record>();
    output->meta = {event.uuid, event.revision, event.timestamp, event.ttl, event.version};
    output->attr = event.attr;
    output->data = event.data;
    output->deadline = event.timestamp + event.ttl;
    auto attr = project.attr(output->attr);
    if (!attr) {
        return std::unexpected(attr.error());
    }
    auto data = project.data(output->data);
    if (!data) {
        return std::unexpected(data.error());
    }
    output->projected_attr = std::move(*attr);
    output->projected_data = std::move(*data);
    output->size = record_size(*output);
    if (auto status = validate_record(event.uuid, event.revision, event.ttl, event.version, output->attr, output->data, limits); !status) {
        return std::unexpected(status.error());
    }
    return std::shared_ptr<const selector_record>(std::move(output));
}

/// 在监听任务独占状态下应用一条事件；revision 间隙只请求权威重同步，不猜测缺失内容。
[[nodiscard]] result<apply_result> apply_event(selector_state& state, const registration_event& event, const policy& limits, const projector& project,
                                               const redis_clock& clock, const selector_configuration& configuration) {
    const auto current = find_record(state, event.uuid);
    if (event.kind == event_kind::unregister) {
        if (!current) {
            return apply_result{};
        }
        remove_record(state, event.uuid);
        return apply_result{true, false};
    }
    if (event.kind == event_kind::register_value) {
        if (current && event.revision < current->meta.revision) {
            return apply_result{};
        }
        auto next = record_from_event(event, limits, project);
        if (!next) {
            return std::unexpected(next.error());
        }
        if (current && event.revision == current->meta.revision &&
            (current->meta.version != (*next)->meta.version || current->meta.ttl != (*next)->meta.ttl || current->attr != (*next)->attr ||
             current->data != (*next)->data)) {
            return apply_result{false, true};
        }
        if ((*next)->deadline <= clock.now()) {
            retain(state, *next, clock.now(), configuration);
        } else if (auto status = set_active(state, *next, configuration); !status) {
            return std::unexpected(status.error());
        }
        return apply_result{true, false};
    }
    if (!current) {
        return apply_result{false, true};
    }
    if (event.kind == event_kind::renew) {
        if (event.revision < current->meta.revision || (event.revision == current->meta.revision && event.timestamp <= current->meta.timestamp)) {
            return apply_result{};
        }
        if (event.revision != current->meta.revision || event.timestamp > hash_field_deadline_max - current->meta.ttl) {
            return apply_result{false, true};
        }
        auto next = std::make_shared<selector_record>(*current);
        next->meta.timestamp = event.timestamp;
        next->deadline = event.timestamp + next->meta.ttl;
        next->size = record_size(*next);
        if (next->deadline <= clock.now()) {
            retain(state, next, clock.now(), configuration);
        } else if (auto status = set_active(state, next, configuration); !status) {
            return std::unexpected(status.error());
        }
        return apply_result{true, false};
    }
    if (event.revision <= current->meta.revision) {
        return apply_result{};
    }
    if (current->meta.revision < event.base_revision) {
        return apply_result{false, true};
    }
    auto next = std::make_shared<selector_record>(*current);
    next->data = current->data;
    for (const auto& [name, value] : event.data) {
        const auto field = next->data.find(name);
        if (field == next->data.end()) {
            return apply_result{false, true};
        }
        field->second = value;
    }
    next->meta.revision = event.revision;
    next->meta.timestamp = event.timestamp;
    if (event.has_version) {
        next->meta.version = event.version;
    }
    if (event.timestamp > hash_field_deadline_max - next->meta.ttl) {
        return std::unexpected(error(code::corrupt, "@timestamp"));
    }
    next->deadline = event.timestamp + next->meta.ttl;
    auto projected = project.data(next->data);
    if (!projected) {
        return std::unexpected(projected.error());
    }
    next->projected_data = std::move(*projected);
    next->size = record_size(*next);
    if (next->size > limits.record_max_bytes) {
        return std::unexpected(error(code::capacity, "registration"));
    }
    if (next->deadline <= clock.now()) {
        retain(state, next, clock.now(), configuration);
    } else if (auto status = set_active(state, next, configuration); !status) {
        return std::unexpected(status.error());
    }
    return apply_result{true, false};
}

[[nodiscard]] result<apply_result> apply_events(selector_state& state, const std::vector<registration_event>& events, const policy& limits,
                                                const projector& project, const redis_clock& clock, const selector_configuration& configuration) {
    apply_result output;
    for (const auto& event : events) {
        auto applied = apply_event(state, event, limits, project, clock, configuration);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        output.changed = output.changed || applied->changed;
        output.repair = output.repair || applied->repair;
    }
    return output;
}

[[nodiscard]] std::shared_ptr<const selector_view> make_view(const selector_state& state, const std::uint64_t generation, const bool synchronized) {
    auto output = std::make_shared<selector_view>();
    output->generation = generation;
    output->synchronized = synchronized;
    for (const auto& [uuid, record] : state.active) {
        output->records.emplace(uuid, record);
        output->ordered.push_back(record);
    }
    std::ranges::sort(output->ordered, {}, [](const auto& value) -> const std::string& { return value->meta.uuid; });
    for (const auto& [uuid, record] : state.retained) {
        output->retained.emplace(uuid, record);
        output->ordered_retained.push_back(record);
    }
    std::ranges::sort(output->ordered_retained, {}, [](const auto& value) -> const std::string& { return value.record->meta.uuid; });
    return output;
}

[[nodiscard]] std::chrono::milliseconds retry_delay(const reconnect_configuration& configuration, const std::size_t failures) {
    std::int64_t value = configuration.initial_delay.count();
    for (std::size_t index = 0; index < failures && value < configuration.max_delay.count(); ++index) {
        if (value > configuration.max_delay.count() / static_cast<std::int64_t>(configuration.multiplier)) {
            value = configuration.max_delay.count();
            break;
        }
        value *= configuration.multiplier;
    }
    value = std::min(value, configuration.max_delay.count());
    const auto span = value * configuration.jitter_percent / 100;
    if (span == 0) {
        return std::chrono::milliseconds{value};
    }
    thread_local std::mt19937_64 generator(std::random_device{}());
    std::uniform_int_distribution<std::int64_t> distribution(0, span);
    return std::chrono::milliseconds{value - span + distribution(generator)};
}

[[nodiscard]] bool wait_stop(const std::stop_token& stop, const std::chrono::milliseconds delay) {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unique_lock lock(mutex);
    return !changed.wait_for(lock, stop, delay, [] { return false; });
}

} // namespace

selector_core::selector_core(std::shared_ptr<client_core> owner, selector_options options, projector project)
    : owner_(std::move(owner)), options_(std::move(options)), project_(project), view_(make_view({}, 0, false)) {}

selector_core::~selector_core() {
    static_cast<void>(close());
}

result<std::shared_ptr<selector_core>> selector_core::create(const std::shared_ptr<client_core>& owner, selector_options options, projector project) {
    if (!owner || !owner->open()) {
        return std::unexpected(error(code::closed));
    }
    if (!valid_type(options.type)) {
        return std::unexpected(error(code::invalid, "type"));
    }
    if (!project.attr || !project.data) {
        return std::unexpected(error(code::invalid, "projector"));
    }
    auto output = std::shared_ptr<selector_core>(new selector_core(owner, std::move(options), project));
    owner->add(output);
    auto ready = std::make_shared<std::promise<result<void>>>();
    auto completed = ready->get_future();
    {
        std::lock_guard lock(output->lifecycle_mutex_);
        output->listener_ = std::jthread([output, ready](const std::stop_token& stop) { output->run(stop, ready); });
    }
    const auto maximum = owner->configuration().selector.sync_timeout + owner->transport()->timeout() + std::chrono::milliseconds{250};
    if (completed.wait_for(maximum) != std::future_status::ready) {
        auto diagnostic = output->try_error();
        static_cast<void>(output->close());
        if (diagnostic) {
            return std::unexpected(std::move(*diagnostic));
        }
        return std::unexpected(error(code::deadline, "selector"));
    }
    auto status = completed.get();
    if (!status) {
        static_cast<void>(output->close());
        return std::unexpected(status.error());
    }
    return output;
}

std::shared_ptr<const selector_view> selector_core::current_view() const noexcept {
    return view_.load(std::memory_order_acquire);
}

result<void> selector_core::validate_data(const fields& data) const {
    const auto limits = owner_ ? owner_->limits() : nullptr;
    if (!limits) {
        return std::unexpected(error(code::closed));
    }
    return validate_record("00000000000000000000000000000000", 1, 1, 1, {}, data, *limits);
}

std::chrono::milliseconds selector_core::wait() const noexcept {
    return owner_ ? owner_->configuration().selector.sync_timeout : std::chrono::milliseconds{1};
}

result<void> selector_core::close() {
    std::lock_guard lock(lifecycle_mutex_);
    closed_.store(true, std::memory_order_release);
    if (listener_.joinable()) {
        listener_.request_stop();
        listener_.join();
    }
    if (final_error_) {
        return std::unexpected(*final_error_);
    }
    return {};
}

std::optional<error> selector_core::try_error() {
    std::lock_guard lock(errors_mutex_);
    if (errors_.empty()) {
        return std::nullopt;
    }
    auto output = std::move(errors_.front());
    errors_.pop_front();
    return output;
}

void selector_core::report(error value) {
    std::lock_guard lock(errors_mutex_);
    const auto capacity = owner_->configuration().selector.error_buffer_capacity;
    if (errors_.size() == capacity) {
        errors_.pop_front();
    }
    errors_.push_back(std::move(value));
}

void selector_core::run(const std::stop_token& stop, const std::shared_ptr<std::promise<result<void>>>& ready) {
    bool ready_sent{false};
    auto finish_ready = [&](result<void> value) {
        if (!ready_sent) {
            ready->set_value(std::move(value));
            ready_sent = true;
        }
    };
    selector_state state;
    redis_clock clock;
    bool clock_valid{false};
    std::uint64_t generation{};
    std::size_t failures{};

    try {
        while (!stop.stop_requested() && owner_->open()) {
            const auto channel = registry_key(owner_->configuration().zone, options_.type);
            auto opened = owner_->transport()->subscribe({channel}, {}, owner_->configuration().selector.max_pending_entries + 16);
            if (!opened) {
                report(opened.error());
                if (!wait_stop(stop, retry_delay(owner_->configuration().selector.recovery, failures++))) {
                    break;
                }
                continue;
            }
            auto subscription = *opened;
            bool confirmed{false};
            const auto confirmation_deadline = std::chrono::steady_clock::now() + owner_->configuration().selector.sync_timeout;
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < confirmation_deadline) {
                auto item = subscription->next(stop, std::chrono::milliseconds{100});
                if (item.type == verdandi::detail::subscription_item::kind::reconnected) {
                    confirmed = true;
                    break;
                }
                if (item.type == verdandi::detail::subscription_item::kind::failure && item.failure) {
                    report(*item.failure);
                    break;
                }
                if (item.type == verdandi::detail::subscription_item::kind::closed) {
                    break;
                }
            }
            if (!confirmed) {
                static_cast<void>(subscription->close());
                if (!wait_stop(stop, retry_delay(owner_->configuration().selector.recovery, failures++))) {
                    break;
                }
                continue;
            }

            view_.store(make_view(state, generation, false), std::memory_order_release);
            pending_events pending(owner_->configuration().selector.max_pending_entries, owner_->configuration().selector.max_pending_bytes);
            auto synchronization = start_sync(owner_, options_, project_, state, subscription);
            bool synchronized{false};
            bool generation_failed{false};
            auto clock_refresh = std::chrono::steady_clock::time_point::max();
            auto publish_at = std::chrono::steady_clock::time_point::max();
            bool dirty{false};

            while (!stop.stop_requested() && owner_->open() && !generation_failed) {
                if (synchronization && synchronization->completed.wait_for(std::chrono::milliseconds::zero()) == std::future_status::ready) {
                    auto result = synchronization->completed.get();
                    synchronization->worker.join();
                    synchronization.reset();
                    if (!result) {
                        report(result.error());
                        break;
                    }
                    state = std::move(result->state);
                    clock = result->clock;
                    clock_valid = true;
                    const auto limits = owner_->limits();
                    if (!limits) {
                        break;
                    }
                    auto applied = apply_events(state, pending.drain(), *limits, project_, clock, owner_->configuration().selector);
                    if (!applied) {
                        report(applied.error());
                        break;
                    }
                    if (applied->repair) {
                        synchronization = start_sync(owner_, options_, project_, state, subscription);
                        continue;
                    }
                    static_cast<void>(expire(state, clock.now(), owner_->configuration().selector));
                    ++generation;
                    synchronized = true;
                    failures = 0;
                    clock_refresh = std::chrono::steady_clock::now() + owner_->configuration().selector.clock_refresh_interval;
                    publish_at = std::chrono::steady_clock::time_point::max();
                    dirty = false;
                    view_.store(make_view(state, generation, true), std::memory_order_release);
                    finish_ready({});
                }

                const auto now = std::chrono::steady_clock::now();
                if (synchronized && now >= clock_refresh) {
                    auto calibrated = calibrate_clock(owner_);
                    if (!calibrated) {
                        report(calibrated.error());
                        break;
                    }
                    clock = *calibrated;
                    clock_refresh = now + owner_->configuration().selector.clock_refresh_interval;
                    dirty = expire(state, clock.now(), owner_->configuration().selector) || dirty;
                }
                if (synchronized && clock_valid) {
                    dirty = expire(state, clock.now(), owner_->configuration().selector) || dirty;
                }
                if (dirty && (owner_->configuration().selector.view_publish_interval == std::chrono::milliseconds::zero() || now >= publish_at)) {
                    view_.store(make_view(state, generation, true), std::memory_order_release);
                    dirty = false;
                    publish_at = std::chrono::steady_clock::time_point::max();
                }

                auto item = subscription->next(stop, synchronization ? std::chrono::milliseconds{10} : std::chrono::milliseconds{50});
                switch (item.type) {
                case verdandi::detail::subscription_item::kind::message: {
                    const auto limits = owner_->limits();
                    auto event = limits ? decode_event(item.payload, *limits)
                                        : result<registration_event>(std::unexpected(error(code::unavailable, "registration_policy")));
                    if (!event) {
                        report(event.error());
                        generation_failed = true;
                        break;
                    }
                    if (synchronization || !synchronized) {
                        if (auto status = pending.add(std::move(*event)); !status) {
                            report(status.error());
                            generation_failed = true;
                        }
                        break;
                    }
                    auto applied = apply_event(state, *event, *limits, project_, clock, owner_->configuration().selector);
                    if (!applied) {
                        report(applied.error());
                        generation_failed = true;
                        break;
                    }
                    if (applied->repair) {
                        synchronized = false;
                        view_.store(make_view(state, generation, false), std::memory_order_release);
                        if (auto status = pending.add(std::move(*event)); !status) {
                            report(status.error());
                            generation_failed = true;
                            break;
                        }
                        synchronization = start_sync(owner_, options_, project_, state, subscription);
                    } else if (applied->changed) {
                        dirty = true;
                        if (publish_at == std::chrono::steady_clock::time_point::max()) {
                            publish_at = std::chrono::steady_clock::now() + owner_->configuration().selector.view_publish_interval;
                        }
                    }
                    break;
                }
                case verdandi::detail::subscription_item::kind::fence:
                    if (synchronization) {
                        synchronization->gate->observe(item.fence_id);
                    }
                    break;
                case verdandi::detail::subscription_item::kind::idle:
                    break;
                case verdandi::detail::subscription_item::kind::failure:
                    if (item.failure) {
                        report(*item.failure);
                    }
                    generation_failed = true;
                    break;
                case verdandi::detail::subscription_item::kind::lagged:
                case verdandi::detail::subscription_item::kind::closed:
                case verdandi::detail::subscription_item::kind::reconnected:
                    generation_failed = true;
                    break;
                }
            }
            if (synchronization) {
                synchronization->worker.request_stop();
                static_cast<void>(subscription->close());
                synchronization->worker.join();
                synchronization.reset();
            } else {
                static_cast<void>(subscription->close());
            }
            if (clock_valid) {
                static_cast<void>(expire(state, clock.now(), owner_->configuration().selector));
            }
            view_.store(make_view(state, generation, false), std::memory_order_release);
            if (!stop.stop_requested() && owner_->open() && !wait_stop(stop, retry_delay(owner_->configuration().selector.recovery, failures++))) {
                break;
            }
        }
    } catch (const std::exception& exception) {
        final_error_ = error(code::unavailable, "selector").with_detail(exception.what());
    } catch (...) {
        final_error_ = error(code::unavailable, "selector");
    }
    view_.store(make_view({}, 0, false), std::memory_order_release);
    if (!ready_sent) {
        finish_ready(std::unexpected(final_error_.value_or(error(code::closed))));
    }
}

result<std::shared_ptr<selector_core>> create_selector(const std::shared_ptr<client_core>& owner, selector_options options, projector project) {
    return selector_core::create(owner, std::move(options), project);
}

std::shared_ptr<const selector_view> selector_current_view(const std::shared_ptr<selector_core>& value) noexcept {
    return value ? value->current_view() : nullptr;
}

result<void> selector_validate_data(const std::shared_ptr<selector_core>& value, const fields& data) {
    return value ? value->validate_data(data) : result<void>(std::unexpected(error(code::closed)));
}

std::chrono::milliseconds selector_wait(const std::shared_ptr<selector_core>& value) noexcept {
    return value ? value->wait() : std::chrono::milliseconds{1};
}

result<void> selector_close(const std::shared_ptr<selector_core>& value) {
    return value ? value->close() : result<void>{};
}

std::optional<error> selector_error(const std::shared_ptr<selector_core>& value) {
    return value ? value->try_error() : std::optional<error>{};
}

} // namespace verdandi::registration::detail
