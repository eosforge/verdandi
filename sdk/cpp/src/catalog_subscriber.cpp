#include "internal/catalog_subscriber.hpp"
#include "internal/catalog_checkpoint.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace verdandi::catalog {

entry::entry(path target, const status initial)
    : target_(std::move(target)), state_(std::make_shared<const detail::entry_state>(detail::entry_state{0, 0, initial, kind::value, 0, {}})) {}

const path& entry::target() const noexcept {
    return target_;
}

status entry::state() const noexcept {
    const auto current = state_.load(std::memory_order_acquire);
    return current ? current->state : status::synchronizing;
}

std::uint64_t entry::revision() const noexcept {
    const auto current = state_.load(std::memory_order_acquire);
    return current ? current->revision : 0;
}

bool entry::synchronized() const noexcept {
    return synchronized_state(state());
}

bool entry::synchronized_state(const status value) noexcept {
    return value == status::present || value == status::absent || value == status::deleted;
}

subscriber::~subscriber() {
    if (core_) {
        static_cast<void>(close());
    }
}

result<std::unique_ptr<subscriber>> subscriber::create(const client& owner, subscription value) {
    if (!owner.core_) {
        return std::unexpected(error(code::closed));
    }
    auto core = detail::create_subscriber(owner.core_, std::move(value));
    if (!core) {
        return std::unexpected(core.error());
    }
    return std::unique_ptr<subscriber>(new subscriber(std::move(*core)));
}

std::shared_ptr<entry> subscriber::find(const path& target) const {
    return detail::subscriber_find(core_, target);
}

std::optional<error> subscriber::try_error() {
    return detail::subscriber_error(core_);
}

result<void> subscriber::close() {
    return detail::subscriber_close(core_);
}

} // namespace verdandi::catalog

namespace verdandi::catalog::detail {

namespace {

struct normalized_subscription {
    bool zone{false};
    std::set<std::string, std::less<>> parts;
    std::set<path> paths;
    std::vector<std::string> channels;
    std::vector<std::string> patterns;
    std::string checkpoint_scope;

    [[nodiscard]] bool covers(const path& target) const {
        return target.valid() && (zone || parts.contains(target.part()) || paths.contains(target));
    }

    [[nodiscard]] bool broad() const noexcept {
        return zone || !parts.empty();
    }
};

[[nodiscard]] result<std::string> checkpoint_scope(const normalized_subscription& value) {
    using context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    context digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        return std::unexpected(error(code::unavailable, "local_store_path"));
    }
    const auto update = [&](const std::string_view bytes) { return bytes.empty() || EVP_DigestUpdate(digest.get(), bytes.data(), bytes.size()) == 1; };
    if (value.zone) {
        if (!update("zone\n")) {
            return std::unexpected(error(code::unavailable, "local_store_path"));
        }
    } else {
        for (const auto& part : value.parts) {
            if (!update(std::string_view("part\0", 5)) || !update(part) || !update("\n")) {
                return std::unexpected(error(code::unavailable, "local_store_path"));
            }
        }
        for (const auto& target : value.paths) {
            const auto member = target.member();
            if (!update(std::string_view("path\0", 5)) || !update(member) || !update("\n")) {
                return std::unexpected(error(code::unavailable, "local_store_path"));
            }
        }
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
    unsigned int size{};
    if (EVP_DigestFinal_ex(digest.get(), output.data(), &size) != 1 || size != 32) {
        return std::unexpected(error(code::unavailable, "local_store_path"));
    }
    return std::string(reinterpret_cast<const char*>(output.data()), size);
}

[[nodiscard]] bool valid_part(const std::string_view value) {
    auto probe = path::create(std::string(value), "probe");
    return probe.has_value();
}

[[nodiscard]] result<normalized_subscription> normalize(const std::string_view zone, subscription value) {
    normalized_subscription output;
    output.zone = value.zone;
    for (auto& part : value.parts) {
        if (!valid_part(part)) {
            return std::unexpected(error(code::invalid, "part"));
        }
        output.parts.insert(std::move(part));
    }
    for (auto& target : value.paths) {
        if (!target.valid()) {
            return std::unexpected(error(code::invalid, "path"));
        }
        output.paths.insert(std::move(target));
    }
    if (!output.zone && output.parts.empty() && output.paths.empty()) {
        return std::unexpected(error(code::invalid, "subscription"));
    }
    const auto prefix = catalog_prefix(zone);
    if (output.zone) {
        output.parts.clear();
        output.paths.clear();
        output.patterns.push_back(prefix + ":*");
    } else {
        for (const auto& part : output.parts) {
            std::string pattern;
            pattern.reserve(prefix.size() + part.size() + 3);
            pattern.append(prefix);
            pattern.push_back(':');
            pattern.append(part);
            pattern.append(":*");
            output.patterns.push_back(std::move(pattern));
        }
        for (auto iterator = output.paths.begin(); iterator != output.paths.end();) {
            if (output.parts.contains(iterator->part())) {
                iterator = output.paths.erase(iterator);
            } else {
                output.channels.push_back(catalog_value_key(zone, *iterator));
                ++iterator;
            }
        }
    }
    auto scope = checkpoint_scope(output);
    if (!scope) {
        return std::unexpected(scope.error());
    }
    output.checkpoint_scope = std::move(*scope);
    return output;
}

[[nodiscard]] result<path> path_from_member(const std::string_view member) {
    const auto separator = member.find(':');
    if (separator == std::string_view::npos || member.find(':', separator + 1) != std::string_view::npos) {
        return std::unexpected(error(code::invalid, "path"));
    }
    return path::create(std::string(member.substr(0, separator)), std::string(member.substr(separator + 1)));
}

[[nodiscard]] result<path> path_from_channel(const std::string_view zone, const std::string_view channel) {
    const auto prefix = catalog_prefix(zone) + ':';
    if (!channel.starts_with(prefix)) {
        return std::unexpected(error(code::target, "channel"));
    }
    return path_from_member(channel.substr(prefix.size()));
}

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

enum class event_kind : std::uint8_t {
    replace,
    patch,
    erase,
};

struct catalog_event {
    event_kind operation{event_kind::replace};
    path target;
    std::uint64_t revision{};
    std::uint64_t base_revision{};
    kind shape{kind::value};
    std::size_t encoded_bytes{};
    fields value;
    std::size_t payload_bytes{};
};

[[nodiscard]] result<std::uint64_t> event_revision(event_cursor& cursor, const std::string_view field) {
    auto encoded = cursor.binary();
    if (!encoded) {
        return std::unexpected(error(code::corrupt, std::string(field)));
    }
    return verdandi::detail::parse_unsigned(*encoded, field);
}

[[nodiscard]] result<std::size_t> event_size(event_cursor& cursor, const std::size_t maximum) {
    auto encoded = cursor.binary();
    if (!encoded) {
        return std::unexpected(error(code::corrupt, "@encoded_bytes"));
    }
    std::size_t output{};
    const auto [end, status] = std::from_chars(encoded->data(), encoded->data() + encoded->size(), output);
    if (status != std::errc{} || end != encoded->data() + encoded->size() || (encoded->size() > 1 && encoded->front() == '0') || output > maximum) {
        return std::unexpected(error(code::corrupt, "@encoded_bytes"));
    }
    return output;
}

[[nodiscard]] bytes copy_bytes(const std::string_view value) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

[[nodiscard]] result<fields> event_fields(event_cursor& cursor) {
    auto elements = cursor.array_size();
    if (!elements || *elements % 2 != 0 || *elements / 2 > maximum_fields) {
        return std::unexpected(error(code::corrupt, "fields"));
    }
    fields output;
    std::string previous;
    for (std::size_t index = 0; index < *elements; index += 2) {
        auto name = cursor.binary();
        auto value = cursor.binary();
        if (!name || !value || name->empty() || (!previous.empty() && previous >= *name)) {
            return std::unexpected(error(code::corrupt, "fields"));
        }
        previous.assign(*name);
        output.emplace(previous, copy_bytes(*value));
    }
    return output;
}

/// 解码 Catalog 固定位置 MessagePack 通知，并严格匹配实际频道 Path。
[[nodiscard]] result<catalog_event> decode_event(const std::string_view payload, const path& expected, const std::size_t maximum_bytes) {
    const auto field_allowance = std::min(maximum_bytes, maximum_fields) * 10ULL;
    if (payload.empty() || payload.size() > maximum_bytes + field_allowance + 1'024ULL) {
        return std::unexpected(error(code::capacity, "notification"));
    }
    event_cursor cursor(payload);
    auto count = cursor.array_size();
    if (!count || *count < 4) {
        return std::unexpected(error(code::corrupt, "notification"));
    }
    auto protocol = cursor.binary();
    auto operation = cursor.binary();
    auto member = cursor.binary();
    if (!protocol || *protocol != "v1") {
        return std::unexpected(error(code::protocol, "protocol"));
    }
    if (!operation || !member || *member != expected.member()) {
        return std::unexpected(error(code::target, "path"));
    }
    catalog_event output;
    output.target = expected;
    output.payload_bytes = payload.size();
    if (*operation == "replace") {
        if (*count != 7) {
            return std::unexpected(error(code::corrupt, "notification"));
        }
        output.operation = event_kind::replace;
        auto revision = event_revision(cursor, "@revision");
        auto shape = cursor.binary();
        auto encoded = event_size(cursor, maximum_bytes);
        auto value = event_fields(cursor);
        auto parsed_shape = shape ? parse_catalog_kind(*shape) : std::nullopt;
        if (!revision || !parsed_shape || !encoded || !value) {
            return std::unexpected(error(code::corrupt, "notification"));
        }
        output.revision = *revision;
        output.shape = *parsed_shape;
        output.encoded_bytes = *encoded;
        output.value = std::move(*value);
        auto actual = validate_catalog_value(output.shape, output.value, maximum_bytes);
        if (!actual || *actual != output.encoded_bytes) {
            return std::unexpected(error(code::corrupt, "@encoded_bytes"));
        }
    } else if (*operation == "patch") {
        if (*count != 8) {
            return std::unexpected(error(code::corrupt, "notification"));
        }
        output.operation = event_kind::patch;
        auto base = event_revision(cursor, "@base_revision");
        auto revision = event_revision(cursor, "@revision");
        auto shape = cursor.binary();
        auto encoded = event_size(cursor, maximum_bytes);
        auto value = event_fields(cursor);
        if (!base || !revision || *revision <= *base || !shape || !encoded || !value || !validate_catalog_patch(*value, maximum_bytes)) {
            return std::unexpected(error(code::corrupt, "notification"));
        }
        auto parsed_shape = parse_catalog_kind(*shape);
        if (!parsed_shape || *parsed_shape == kind::value) {
            return std::unexpected(error(code::corrupt, "notification"));
        }
        const auto event_shape = *parsed_shape;
        output.base_revision = *base;
        output.revision = *revision;
        output.shape = event_shape;
        output.encoded_bytes = *encoded;
        output.value = std::move(*value);
    } else if (*operation == "delete") {
        if (*count != 4) {
            return std::unexpected(error(code::corrupt, "notification"));
        }
        output.operation = event_kind::erase;
        auto revision = event_revision(cursor, "@revision");
        if (!revision) {
            return std::unexpected(revision.error());
        }
        output.revision = *revision;
    } else {
        return std::unexpected(error(code::protocol, "operation"));
    }
    if (!cursor.done()) {
        return std::unexpected(error(code::corrupt, "notification"));
    }
    return output;
}

class pending_events final {
public:
    explicit pending_events(const std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] result<void> add(catalog_event value) {
        auto [iterator, inserted] = values_.try_emplace(value.target);
        if (inserted && values_.size() > capacity_) {
            values_.erase(iterator);
            return std::unexpected(error(code::capacity, "catalog_events"));
        }
        auto& current = iterator->second;
        if (!current || value.operation == event_kind::replace || value.operation == event_kind::erase) {
            current = std::move(value);
            return {};
        }
        if (value.operation == event_kind::patch && current->operation == event_kind::replace && value.base_revision == current->revision) {
            for (const auto& [name, field] : value.value) {
                current->value[name] = field;
            }
            current->revision = value.revision;
            current->encoded_bytes = value.encoded_bytes;
            current->payload_bytes = std::max(current->payload_bytes, value.payload_bytes);
            return {};
        }
        if (value.operation == event_kind::patch && current->operation == event_kind::patch && value.base_revision == current->revision) {
            for (const auto& [name, field] : value.value) {
                current->value[name] = field;
            }
            current->revision = value.revision;
            current->encoded_bytes = value.encoded_bytes;
            current->payload_bytes = std::max(current->payload_bytes, value.payload_bytes);
            return {};
        }
        current = std::move(value);
        return {};
    }

    [[nodiscard]] std::vector<catalog_event> drain() {
        std::vector<catalog_event> output;
        output.reserve(values_.size());
        for (auto& [target, value] : values_) {
            static_cast<void>(target);
            if (value) {
                output.push_back(std::move(*value));
            }
        }
        values_.clear();
        return output;
    }

private:
    std::map<path, std::optional<catalog_event>> values_;
    std::size_t capacity_;
};

struct zone_metadata {
    std::uint64_t revision{};
    std::uint64_t floor{};
};

[[nodiscard]] result<zone_metadata> read_metadata(const std::shared_ptr<client_core>& owner) {
    verdandi::detail::command read("HGETALL");
    read.add(catalog_meta_key(owner->configuration().zone));
    auto response = owner->transport()->execute(read);
    if (!response) {
        return std::unexpected(response.error());
    }
    auto pairs = verdandi::detail::named_pairs(*response);
    if (!pairs) {
        return std::unexpected(pairs.error());
    }
    if (pairs->empty()) {
        return zone_metadata{};
    }
    if (pairs->size() != 2) {
        return std::unexpected(error(code::corrupt, "catalog_meta"));
    }
    std::map<std::string_view, std::string_view, std::less<>> values;
    for (const auto& pair : *pairs) {
        values.emplace(pair);
    }
    const auto revision_value = values.find("@revision");
    const auto floor_value = values.find("@floor_revision");
    if (revision_value == values.end() || floor_value == values.end()) {
        return std::unexpected(error(code::corrupt, "catalog_meta"));
    }
    auto revision = verdandi::detail::parse_unsigned(revision_value->second, "@revision", true);
    auto floor = verdandi::detail::parse_unsigned(floor_value->second, "@floor_revision", true);
    if (!revision || !floor || *floor > *revision) {
        return std::unexpected(error(code::corrupt, "catalog_meta"));
    }
    return zone_metadata{*revision, *floor};
}

[[nodiscard]] result<void> collect_index(const std::shared_ptr<client_core>& owner, const normalized_subscription& scope, const std::string& key,
                                         std::set<path>& output) {
    std::uint64_t cursor{};
    do {
        verdandi::detail::command scan("ZSCAN");
        scan.add(key).add(cursor).add("COUNT").add(static_cast<std::uint64_t>(owner->configuration().scan_page_size));
        auto response = owner->transport()->execute(scan);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (response->type != verdandi::detail::response::kind::array || response->children.size() != 2) {
            return std::unexpected(error(code::corrupt, "catalog_index"));
        }
        auto cursor_text = response->children[0].text();
        auto next = cursor_text ? verdandi::detail::parse_unsigned(*cursor_text, "ZSCAN.cursor", true)
                                : result<std::uint64_t>(std::unexpected(error(code::corrupt, "ZSCAN.cursor")));
        auto pairs = verdandi::detail::named_pairs(response->children[1]);
        if (!next || !pairs) {
            return std::unexpected(!next ? next.error() : pairs.error());
        }
        for (const auto& [member, score] : *pairs) {
            auto revision = verdandi::detail::parse_unsigned(score, "catalog_index");
            auto target = path_from_member(member);
            if (!revision || !target) {
                return std::unexpected(error(code::corrupt, "catalog_index"));
            }
            if (scope.covers(*target)) {
                output.insert(std::move(*target));
            }
        }
        cursor = *next;
    } while (cursor != 0);
    return {};
}

[[nodiscard]] result<void> collect_changed(const std::shared_ptr<client_core>& owner, const normalized_subscription& scope, const std::string& key,
                                           std::uint64_t first_revision, const std::uint64_t last_revision, std::set<path>& output) {
    while (first_revision < last_revision) {
        verdandi::detail::command range("ZRANGEBYSCORE");
        range.add(key)
            .add('(' + std::to_string(first_revision))
            .add(last_revision)
            .add("WITHSCORES")
            .add("LIMIT")
            .add(0)
            .add(static_cast<std::uint64_t>(owner->configuration().scan_page_size));
        auto response = owner->transport()->execute(range);
        if (!response) {
            return std::unexpected(response.error());
        }
        auto pairs = verdandi::detail::named_pairs(*response);
        if (!pairs) {
            return std::unexpected(pairs.error());
        }
        if (pairs->empty()) {
            break;
        }
        for (const auto& [member, score] : *pairs) {
            auto revision = verdandi::detail::parse_unsigned(score, "catalog_index");
            auto target = path_from_member(member);
            if (!revision || !target || *revision <= first_revision || *revision > last_revision) {
                return std::unexpected(error(code::corrupt, "catalog_index"));
            }
            first_revision = *revision;
            if (scope.covers(*target)) {
                output.insert(std::move(*target));
            }
        }
        if (pairs->size() < owner->configuration().scan_page_size) {
            break;
        }
    }
    return {};
}

[[nodiscard]] result<std::map<std::string_view, const verdandi::detail::response*, std::less<>>> response_fields(const verdandi::detail::response& response) {
    if (response.type != verdandi::detail::response::kind::array || response.children.empty() || response.children.size() % 2 != 0) {
        return std::unexpected(error(code::corrupt, "read_reply"));
    }
    std::map<std::string_view, const verdandi::detail::response*, std::less<>> output;
    for (std::size_t index = 0; index < response.children.size(); index += 2) {
        auto name = response.children[index].text();
        if (!name || !output.emplace(*name, &response.children[index + 1]).second) {
            return std::unexpected(error(code::corrupt, "read_reply"));
        }
    }
    return output;
}

[[nodiscard]] result<std::uint64_t> reply_revision(const std::map<std::string_view, const verdandi::detail::response*, std::less<>>& values,
                                                   const bool allow_zero) {
    const auto found = values.find("@revision");
    if (found == values.end()) {
        return std::unexpected(error(code::corrupt, "@revision"));
    }
    auto text = found->second->text();
    return text ? verdandi::detail::parse_unsigned(*text, "@revision", allow_zero) : result<std::uint64_t>(std::unexpected(error(code::corrupt, "@revision")));
}

[[nodiscard]] result<std::shared_ptr<const entry_state>> parse_read(const verdandi::detail::response& response, const std::size_t maximum_bytes) {
    auto values = response_fields(response);
    if (!values) {
        return std::unexpected(values.error());
    }
    const auto result_value = values->find("&result");
    const auto status_value = values->find("&status");
    if (result_value == values->end() || status_value == values->end()) {
        return std::unexpected(error(code::corrupt, "read_reply"));
    }
    auto result_text = result_value->second->text();
    auto status_text = status_value->second->text();
    if (!result_text || !status_text) {
        return std::unexpected(error(code::corrupt, "read_reply"));
    }
    if (*result_text == "error") {
        const auto code_value = values->find("&status");
        auto category_text = code_value != values->end() ? code_value->second->text() : result<std::string_view>(std::unexpected(error(code::corrupt)));
        auto category = category_text ? parse_code(*category_text) : std::nullopt;
        return std::unexpected(error(category.value_or(code::protocol), "catalog_read"));
    }
    if (*result_text != "ok") {
        return std::unexpected(error(code::corrupt, "&result"));
    }
    auto revision = reply_revision(*values, true);
    if (!revision) {
        return std::unexpected(revision.error());
    }
    if (*status_text == "absent") {
        if (*revision != 0) {
            return std::unexpected(error(code::corrupt, "read_reply"));
        }
        return std::make_shared<const entry_state>(entry_state{0, 0, status::absent, kind::value, 0, {}});
    }
    if (*status_text == "deleted") {
        if (*revision == 0) {
            return std::unexpected(error(code::corrupt, "read_reply"));
        }
        return std::make_shared<const entry_state>(entry_state{*revision, 0, status::deleted, kind::value, 0, {}});
    }
    if (*status_text != "present" || *revision == 0) {
        return std::unexpected(error(code::corrupt, "&status"));
    }
    const auto mode_value = values->find("&mode");
    const auto replace_value = values->find("@replace_revision");
    const auto kind_value = values->find("@kind");
    const auto bytes_value = values->find("@encoded_bytes");
    const auto fields_value = values->find("&fields");
    if (mode_value == values->end() || replace_value == values->end() || kind_value == values->end() || bytes_value == values->end() ||
        fields_value == values->end()) {
        return std::unexpected(error(code::corrupt, "read_reply"));
    }
    auto mode = mode_value->second->text();
    auto replace_text = replace_value->second->text();
    auto kind_text = kind_value->second->text();
    auto bytes_text = bytes_value->second->text();
    auto replace_revision = replace_text ? verdandi::detail::parse_unsigned(*replace_text, "@replace_revision")
                                         : result<std::uint64_t>(std::unexpected(error(code::corrupt, "@replace_revision")));
    if (!bytes_text) {
        return std::unexpected(error(code::corrupt, "@encoded_bytes"));
    }
    if (!kind_text) {
        return std::unexpected(error(code::corrupt, "read_reply"));
    }
    auto shape = parse_catalog_kind(*kind_text);
    if (!shape) {
        return std::unexpected(error(code::corrupt, "read_reply"));
    }
    const auto entry_shape = *shape;
    std::size_t encoded_bytes{};
    const auto [end, conversion] = std::from_chars(bytes_text->data(), bytes_text->data() + bytes_text->size(), encoded_bytes);
    if (!mode || *mode != "replace" || !replace_revision || *replace_revision > *revision || conversion != std::errc{} ||
        end != bytes_text->data() + bytes_text->size() || encoded_bytes > maximum_bytes ||
        fields_value->second->type != verdandi::detail::response::kind::array || fields_value->second->children.size() % 2 != 0 ||
        fields_value->second->children.size() / 2 > maximum_fields) {
        return std::unexpected(error(code::corrupt, "read_reply"));
    }
    fields value;
    for (std::size_t index = 0; index < fields_value->second->children.size(); index += 2) {
        auto name = fields_value->second->children[index].text();
        auto field = fields_value->second->children[index + 1].text();
        if (!name || !field || !value.emplace(std::string(*name), copy_bytes(*field)).second) {
            return std::unexpected(error(code::corrupt, "fields"));
        }
    }
    auto actual = validate_catalog_value(entry_shape, value, maximum_bytes);
    if (!actual || *actual != encoded_bytes) {
        return std::unexpected(error(code::corrupt, "@encoded_bytes"));
    }
    return std::make_shared<const entry_state>(entry_state{*revision, *replace_revision, status::present, entry_shape, encoded_bytes, std::move(value)});
}

struct fence_gate {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::uint64_t observed{};

    void observe(const std::uint64_t value) {
        std::lock_guard lock(mutex);
        observed = std::max(observed, value);
        changed.notify_all();
    }

    [[nodiscard]] bool wait(const std::stop_token& stop, const std::uint64_t value) {
        std::unique_lock lock(mutex);
        return changed.wait(lock, stop, [&] { return observed >= value; });
    }
};

struct sync_output {
    std::map<path, std::shared_ptr<const entry_state>> states;
    std::uint64_t cursor{};
    bool full{false};
};

[[nodiscard]] result<sync_output> synchronize(const std::stop_token& stop, const std::shared_ptr<client_core>& owner, const normalized_subscription& scope,
                                              std::vector<path> exact, const std::vector<path>& existing, const std::uint64_t cursor,
                                              const std::shared_ptr<verdandi::detail::subscription>& subscription, const std::shared_ptr<fence_gate>& gate) {
    std::shared_lock operation(owner->operations());
    if (!owner->open() || stop.stop_requested()) {
        return std::unexpected(error(code::closed));
    }
    auto metadata = read_metadata(owner);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }
    std::set<path> paths(exact.begin(), exact.end());
    bool full{false};
    if (exact.empty()) {
        for (const auto& target : scope.paths) {
            paths.insert(target);
        }
        if (scope.broad()) {
            full = cursor == 0 || cursor > metadata->revision || cursor < metadata->floor;
            if (full) {
                if (auto status = collect_index(owner, scope, catalog_live_key(owner->configuration().zone), paths); !status) {
                    return std::unexpected(status.error());
                }
                if (auto status = collect_index(owner, scope, catalog_deleted_key(owner->configuration().zone), paths); !status) {
                    return std::unexpected(status.error());
                }
                for (const auto& target : existing) {
                    if (scope.covers(target)) {
                        paths.insert(target);
                    }
                }
            } else {
                if (auto status = collect_changed(owner, scope, catalog_live_key(owner->configuration().zone), cursor, metadata->revision, paths); !status) {
                    return std::unexpected(status.error());
                }
                if (auto status = collect_changed(owner, scope, catalog_deleted_key(owner->configuration().zone), cursor, metadata->revision, paths); !status) {
                    return std::unexpected(status.error());
                }
            }
        } else {
            full = true;
        }
    }
    sync_output output;
    output.cursor = metadata->revision;
    output.full = full;
    std::vector<path> ordered(paths.begin(), paths.end());
    std::uint64_t maximum{};
    for (std::size_t first = 0; first < ordered.size(); first += owner->configuration().max_inflight_reads) {
        if (stop.stop_requested()) {
            return std::unexpected(error(code::closed));
        }
        const auto last = std::min(first + owner->configuration().max_inflight_reads, ordered.size());
        std::vector<verdandi::detail::script_call> calls;
        calls.reserve(last - first);
        for (std::size_t index = first; index < last; ++index) {
            calls.push_back({catalog_read_keys(owner->configuration().zone, ordered[index]), {ordered[index].member(), "0"}});
        }
        auto responses = owner->read_script().run(*owner->transport(), calls, false);
        if (!responses) {
            return std::unexpected(responses.error());
        }
        for (std::size_t index = 0; index < responses->size(); ++index) {
            auto state = parse_read((*responses)[index], owner->configuration().max_record_bytes);
            if (!state) {
                return std::unexpected(state.error());
            }
            maximum = std::max(maximum, (*state)->revision);
            output.states.emplace(ordered[first + index], std::move(*state));
        }
    }
    auto fence = subscription->fence();
    if (!fence) {
        return std::unexpected(fence.error());
    }
    if (!gate->wait(stop, *fence)) {
        return std::unexpected(error(code::closed));
    }
    auto after = read_metadata(owner);
    if (!after) {
        return std::unexpected(after.error());
    }
    if (metadata->revision < after->floor) {
        return std::unexpected(error(code::transition, "@floor_revision").with_revision(after->floor));
    }
    if (maximum > after->revision) {
        return std::unexpected(error(code::corrupt, "@revision").with_revision(maximum));
    }
    return output;
}

struct active_sync {
    std::shared_ptr<fence_gate> gate{std::make_shared<fence_gate>()};
    std::future<result<sync_output>> completed;
    std::jthread worker;
    bool exact{false};
};

[[nodiscard]] std::unique_ptr<active_sync> start_sync(const std::shared_ptr<client_core>& owner, const normalized_subscription& scope, std::vector<path> exact,
                                                      std::vector<path> existing, const std::uint64_t cursor,
                                                      const std::shared_ptr<verdandi::detail::subscription>& subscription) {
    auto output = std::make_unique<active_sync>();
    output->exact = !exact.empty();
    auto promise = std::make_shared<std::promise<result<sync_output>>>();
    output->completed = promise->get_future();
    const auto gate = output->gate;
    output->worker = std::jthread(
        [owner, scope, exact = std::move(exact), existing = std::move(existing), cursor, subscription, gate, promise](const std::stop_token& stop) mutable {
            try {
                promise->set_value(synchronize(stop, owner, scope, std::move(exact), existing, cursor, subscription, gate));
            } catch (const std::exception& exception) {
                promise->set_value(std::unexpected(error(code::unavailable, "catalog_sync").with_detail(exception.what())));
            } catch (...) {
                promise->set_value(std::unexpected(error(code::unavailable, "catalog_sync")));
            }
        });
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

struct subscriber_core::implementation {
    implementation(std::shared_ptr<client_core> source, normalized_subscription subscription) : owner(std::move(source)), scope(std::move(subscription)) {}

    [[nodiscard]] std::shared_ptr<entry> get_or_create(const path& target, const status initial) {
        std::lock_guard lock(entries_mutex);
        auto [iterator, inserted] = entries.try_emplace(target);
        if (inserted) {
            iterator->second = std::shared_ptr<entry>(new entry(target, initial));
        }
        return iterator->second;
    }

    [[nodiscard]] std::vector<path> entry_paths() const {
        std::lock_guard lock(entries_mutex);
        std::vector<path> output;
        output.reserve(entries.size());
        for (const auto& [target, value] : entries) {
            static_cast<void>(value);
            output.push_back(target);
        }
        return output;
    }

    [[nodiscard]] result<void> install(const path& target, std::shared_ptr<const entry_state> state) {
        const auto entry = get_or_create(target, status::synchronizing);
        const auto previous = entry->state_.load(std::memory_order_acquire);
        const auto previous_bytes = previous && previous->replace_revision != 0 ? previous->encoded_bytes : 0;
        const auto next_bytes = state->replace_revision != 0 ? state->encoded_bytes : 0;
        const auto projected = view_bytes - previous_bytes + next_bytes;
        if (owner->configuration().max_view_bytes != 0 && projected > owner->configuration().max_view_bytes) {
            return std::unexpected(error(code::capacity, "catalog_view"));
        }
        view_bytes = projected;
        entry->state_.store(std::move(state), std::memory_order_release);
        return {};
    }

    void mark_all(const status value) {
        scope_state.store(value, std::memory_order_release);
        std::vector<std::shared_ptr<entry>> values;
        {
            std::lock_guard lock(entries_mutex);
            for (const auto& [target, entry] : entries) {
                static_cast<void>(target);
                values.push_back(entry);
            }
        }
        for (const auto& entry : values) {
            auto current = entry->state_.load(std::memory_order_acquire);
            if (!current || current->state == status::closed) {
                continue;
            }
            auto next = std::make_shared<entry_state>(*current);
            next->state = value;
            entry->state_.store(std::move(next), std::memory_order_release);
        }
    }

    void align() {
        scope_state.store(status::present, std::memory_order_release);
        std::vector<std::shared_ptr<entry>> values;
        {
            std::lock_guard lock(entries_mutex);
            for (const auto& [target, entry] : entries) {
                static_cast<void>(target);
                values.push_back(entry);
            }
        }
        for (const auto& entry : values) {
            auto current = entry->state_.load(std::memory_order_acquire);
            if (!current) {
                continue;
            }
            auto next = std::make_shared<entry_state>(*current);
            if (next->replace_revision != 0) {
                next->state = status::present;
            } else if (next->revision != 0) {
                next->state = status::deleted;
            } else {
                next->state = status::absent;
            }
            entry->state_.store(std::move(next), std::memory_order_release);
        }
    }

    void report(error value) {
        std::lock_guard lock(errors_mutex);
        if (errors.size() == owner->configuration().error_buffer_capacity) {
            errors.pop_front();
        }
        errors.push_back(std::move(value));
    }

    void report_store(error value) {
        if (auto store = owner->store()) {
            store->disable();
        }
        if (!store_error_reported) {
            store_error_reported = true;
            report(std::move(value));
        }
    }

    void restore() {
        auto store = owner->store();
        if (!store) {
            return;
        }
        auto restored = store->load(owner->configuration().zone, scope.checkpoint_scope, owner->configuration().max_record_bytes);
        if (!restored) {
            report_store(restored.error());
            return;
        }
        std::uint64_t total{};
        for (const auto& [target, state] : restored->entries) {
            if (!scope.covers(target) || state->encoded_bytes > std::numeric_limits<std::uint64_t>::max() - total) {
                report_store(error(code::corrupt, "local_store_path"));
                return;
            }
            total += state->encoded_bytes;
        }
        if (owner->configuration().max_view_bytes != 0 && total > owner->configuration().max_view_bytes) {
            report_store(error(code::capacity, "max_view_bytes"));
            return;
        }
        cursor = restored->cursor;
        view_bytes = total;
        for (auto& [target, state] : restored->entries) {
            auto unavailable = std::make_shared<entry_state>(*state);
            unavailable->state = status::synchronizing;
            get_or_create(target, status::synchronizing)->state_.store(std::move(unavailable), std::memory_order_release);
        }
    }

    void persist(const std::set<path>& targets, const std::uint64_t observed_cursor) {
        auto store = owner->store();
        if (!store || store->disabled()) {
            return;
        }
        std::vector<checkpoint_entry> values;
        values.reserve(targets.size());
        {
            std::lock_guard lock(entries_mutex);
            for (const auto& target : targets) {
                const auto found = entries.find(target);
                if (found == entries.end()) {
                    continue;
                }
                auto state = found->second->state_.load(std::memory_order_acquire);
                if (state && (state->state == status::present || state->state == status::absent || state->state == status::deleted)) {
                    values.push_back({target, std::move(state)});
                }
            }
        }
        if (auto saved = store->save(owner->configuration().zone, scope.checkpoint_scope, values, observed_cursor, owner->configuration().max_record_bytes);
            !saved) {
            report_store(saved.error());
        }
    }

    [[nodiscard]] result<bool> apply(catalog_event event) {
        const auto entry = get_or_create(event.target, status::synchronizing);
        const auto current = entry->state_.load(std::memory_order_acquire);
        if (current && event.revision <= current->revision) {
            return false;
        }
        if (event.operation == event_kind::erase) {
            auto next = std::make_shared<const entry_state>(entry_state{event.revision, 0, status::deleted, kind::value, 0, {}});
            if (auto installed = install(event.target, std::move(next)); !installed) {
                return std::unexpected(installed.error());
            }
            return true;
        }
        if (event.operation == event_kind::replace) {
            auto next = std::make_shared<const entry_state>(
                entry_state{event.revision, event.revision, status::present, event.shape, event.encoded_bytes, std::move(event.value)});
            if (auto installed = install(event.target, std::move(next)); !installed) {
                return std::unexpected(installed.error());
            }
            return true;
        }
        if (!current || current->state != status::present || current->revision != event.base_revision || current->shape != event.shape) {
            return std::unexpected(error(code::stale, "@base_revision").with_revision(current ? current->revision : 0));
        }
        auto next = std::make_shared<entry_state>(*current);
        next->revision = event.revision;
        next->state = status::present;
        for (auto& [name, field] : event.value) {
            next->value[name] = std::move(field);
        }
        auto actual = validate_catalog_value(next->shape, next->value, owner->configuration().max_record_bytes);
        if (!actual || *actual != event.encoded_bytes) {
            return std::unexpected(error(code::corrupt, "@encoded_bytes"));
        }
        next->encoded_bytes = event.encoded_bytes;
        if (auto installed = install(event.target, std::move(next)); !installed) {
            return std::unexpected(installed.error());
        }
        return true;
    }

    void run(const std::stop_token& stop, const std::shared_ptr<std::promise<result<void>>>& ready) {
        bool ready_sent{false};
        const auto finish_ready = [&](result<void> value) {
            if (!ready_sent) {
                ready->set_value(std::move(value));
                ready_sent = true;
            }
        };
        std::size_t failures{};
        try {
            while (!stop.stop_requested() && owner->open()) {
                auto opened = owner->transport()->subscribe(scope.channels, scope.patterns, owner->configuration().event_buffer_capacity + 16);
                if (!opened) {
                    report(opened.error());
                    if (!wait_stop(stop, retry_delay(owner->configuration().recovery, failures++))) {
                        break;
                    }
                    continue;
                }
                auto subscription = *opened;
                bool confirmed{false};
                const auto confirmation_deadline = std::chrono::steady_clock::now() + owner->configuration().sync_timeout;
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
                    if (!wait_stop(stop, retry_delay(owner->configuration().recovery, failures++))) {
                        break;
                    }
                    continue;
                }

                mark_all(status::synchronizing);
                pending_events pending(owner->configuration().event_buffer_capacity);
                auto synchronization = start_sync(owner, scope, {}, entry_paths(), cursor, subscription);
                bool generation_failed{false};
                bool aligned{false};
                std::set<path> changed;
                std::uint64_t observed_cursor = cursor;
                while (!stop.stop_requested() && owner->open() && !generation_failed) {
                    if (synchronization && synchronization->completed.wait_for(std::chrono::milliseconds::zero()) == std::future_status::ready) {
                        const bool exact = synchronization->exact;
                        auto result = synchronization->completed.get();
                        synchronization->worker.join();
                        synchronization.reset();
                        if (!result) {
                            report(result.error());
                            break;
                        }
                        observed_cursor = std::max(observed_cursor, result->cursor);
                        for (auto& [target, state] : result->states) {
                            if (auto installed = install(target, state); !installed) {
                                report(installed.error());
                                generation_failed = true;
                                break;
                            }
                            changed.insert(target);
                        }
                        if (generation_failed) {
                            break;
                        }
                        std::set<path> repairs;
                        for (auto& event : pending.drain()) {
                            observed_cursor = std::max(observed_cursor, event.revision);
                            auto applied = apply(event);
                            if (!applied) {
                                if (applied.error().category() == code::stale) {
                                    repairs.insert(event.target);
                                    continue;
                                }
                                report(applied.error());
                                generation_failed = true;
                                break;
                            }
                            if (*applied) {
                                changed.insert(event.target);
                            }
                        }
                        if (generation_failed) {
                            break;
                        }
                        if (!repairs.empty()) {
                            std::vector<path> paths(repairs.begin(), repairs.end());
                            for (const auto& target : paths) {
                                auto entry = get_or_create(target, status::synchronizing);
                                auto current = entry->state_.load(std::memory_order_acquire);
                                auto next = std::make_shared<entry_state>(*current);
                                next->state = status::synchronizing;
                                entry->state_.store(std::move(next), std::memory_order_release);
                            }
                            synchronization = start_sync(owner, scope, std::move(paths), {}, cursor, subscription);
                            continue;
                        }
                        if (!exact) {
                            aligned = true;
                        }
                        if (aligned) {
                            cursor = std::max(cursor, observed_cursor);
                            persist(changed, cursor);
                            changed.clear();
                            align();
                            failures = 0;
                            finish_ready({});
                        }
                    }

                    auto item = subscription->next(stop, synchronization ? std::chrono::milliseconds{10} : std::chrono::milliseconds{100});
                    switch (item.type) {
                    case verdandi::detail::subscription_item::kind::message: {
                        auto target = path_from_channel(owner->configuration().zone, item.channel);
                        auto event = target && scope.covers(*target) ? decode_event(item.payload, *target, owner->configuration().max_record_bytes)
                                                                     : result<catalog_event>(std::unexpected(error(code::target, "channel")));
                        if (!event) {
                            report(event.error());
                            generation_failed = true;
                            break;
                        }
                        if (synchronization || !aligned) {
                            if (auto status = pending.add(std::move(*event)); !status) {
                                report(status.error());
                                generation_failed = true;
                            }
                            break;
                        }
                        auto applied = apply(*event);
                        if (!applied && applied.error().category() == code::stale) {
                            auto entry = get_or_create(event->target, status::synchronizing);
                            auto current = entry->state_.load(std::memory_order_acquire);
                            auto next = std::make_shared<entry_state>(*current);
                            next->state = status::synchronizing;
                            entry->state_.store(std::move(next), std::memory_order_release);
                            if (auto status = pending.add(std::move(*event)); !status) {
                                report(status.error());
                                generation_failed = true;
                                break;
                            }
                            synchronization = start_sync(owner, scope, {entry->target()}, {}, cursor, subscription);
                        } else if (!applied) {
                            report(applied.error());
                            generation_failed = true;
                        } else {
                            cursor = std::max(cursor, event->revision);
                            if (*applied) {
                                const std::set<path> target{event->target};
                                persist(target, cursor);
                            } else {
                                persist({}, cursor);
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
                } else {
                    static_cast<void>(subscription->close());
                }
                mark_all(status::unavailable);
                if (!stop.stop_requested() && owner->open() && !wait_stop(stop, retry_delay(owner->configuration().recovery, failures++))) {
                    break;
                }
            }
        } catch (const std::exception& exception) {
            final_error = error(code::unavailable, "catalog_subscriber").with_detail(exception.what());
        } catch (...) {
            final_error = error(code::unavailable, "catalog_subscriber");
        }
        mark_all(status::closed);
        if (!ready_sent) {
            finish_ready(std::unexpected(final_error.value_or(error(code::closed))));
        }
    }

    std::shared_ptr<client_core> owner;
    normalized_subscription scope;
    mutable std::mutex entries_mutex;
    std::map<path, std::shared_ptr<entry>> entries;
    std::uint64_t cursor{};
    std::uint64_t view_bytes{};
    std::atomic<status> scope_state{status::synchronizing};
    std::atomic_bool closed{false};
    std::mutex lifecycle_mutex;
    std::jthread listener;
    std::mutex errors_mutex;
    std::deque<error> errors;
    bool store_error_reported{false};
    std::optional<error> final_error;
};

subscriber_core::subscriber_core(std::unique_ptr<implementation> implementation) noexcept : implementation_(std::move(implementation)) {}

subscriber_core::~subscriber_core() {
    static_cast<void>(close());
}

result<std::shared_ptr<subscriber_core>> subscriber_core::create(const std::shared_ptr<client_core>& owner, subscription value) {
    if (!owner || !owner->open()) {
        return std::unexpected(error(code::closed));
    }
    auto normalized = normalize(owner->configuration().zone, std::move(value));
    if (!normalized) {
        return std::unexpected(normalized.error());
    }
    auto core = std::shared_ptr<subscriber_core>(new subscriber_core(std::make_unique<implementation>(owner, std::move(*normalized))));
    owner->add(core);
    core->implementation_->restore();
    for (const auto& target : core->implementation_->scope.paths) {
        static_cast<void>(core->implementation_->get_or_create(target, status::synchronizing));
    }
    auto ready = std::make_shared<std::promise<result<void>>>();
    auto completed = ready->get_future();
    {
        std::lock_guard lock(core->implementation_->lifecycle_mutex);
        core->implementation_->listener = std::jthread([core, ready](const std::stop_token& stop) { core->implementation_->run(stop, ready); });
    }
    const auto maximum = owner->configuration().sync_timeout + owner->transport()->timeout() + std::chrono::milliseconds{250};
    if (completed.wait_for(maximum) != std::future_status::ready) {
        auto diagnostic = core->try_error();
        static_cast<void>(core->close());
        return std::unexpected(diagnostic.value_or(error(code::deadline, "catalog_subscriber")));
    }
    auto status = completed.get();
    if (!status) {
        static_cast<void>(core->close());
        return std::unexpected(status.error());
    }
    return core;
}

std::shared_ptr<entry> subscriber_core::find(const path& target) {
    if (!implementation_ || implementation_->closed.load(std::memory_order_acquire) || !implementation_->scope.covers(target)) {
        return {};
    }
    auto initial = implementation_->scope_state.load(std::memory_order_acquire);
    if (initial == status::present) {
        initial = status::absent;
    }
    return implementation_->get_or_create(target, initial);
}

result<void> subscriber_core::close() {
    if (!implementation_) {
        return {};
    }
    auto& state = *implementation_;
    std::lock_guard lock(state.lifecycle_mutex);
    state.closed.store(true, std::memory_order_release);
    if (state.listener.joinable()) {
        state.listener.request_stop();
        state.listener.join();
    }
    if (state.final_error) {
        return std::unexpected(*state.final_error);
    }
    return {};
}

std::optional<error> subscriber_core::try_error() {
    if (!implementation_) {
        return std::nullopt;
    }
    std::lock_guard lock(implementation_->errors_mutex);
    if (implementation_->errors.empty()) {
        return std::nullopt;
    }
    auto output = std::move(implementation_->errors.front());
    implementation_->errors.pop_front();
    return output;
}

result<std::shared_ptr<subscriber_core>> create_subscriber(const std::shared_ptr<client_core>& owner, subscription value) {
    return subscriber_core::create(owner, std::move(value));
}

std::shared_ptr<entry> subscriber_find(const std::shared_ptr<subscriber_core>& value, const path& target) {
    return value ? value->find(target) : nullptr;
}

result<void> subscriber_close(const std::shared_ptr<subscriber_core>& value) {
    return value ? value->close() : result<void>{};
}

std::optional<error> subscriber_error(const std::shared_ptr<subscriber_core>& value) {
    return value ? value->try_error() : std::optional<error>{};
}

} // namespace verdandi::catalog::detail
