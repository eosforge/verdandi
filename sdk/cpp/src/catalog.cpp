#include "internal/catalog.hpp"
#include "internal/catalog_checkpoint.hpp"
#include "internal/catalog_subscriber.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace verdandi::catalog {

namespace {

[[nodiscard]] bool valid_segment(const std::string_view value, const std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    return std::ranges::all_of(value, [](const char character) {
        const bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        return letter || (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
    });
}

} // namespace

result<path> path::create(std::string part, std::string id) {
    if (!valid_segment(part, 64)) {
        return std::unexpected(error(code::invalid, "part"));
    }
    if (!valid_segment(id, 128)) {
        return std::unexpected(error(code::invalid, "id"));
    }
    return path(std::move(part), std::move(id));
}

std::string_view path::part() const noexcept {
    return part_;
}

std::string_view path::id() const noexcept {
    return id_;
}

std::string path::member() const {
    return part_ + ':' + id_;
}

bool path::valid() const noexcept {
    return valid_segment(part_, 64) && valid_segment(id_, 128);
}

result<client> client::open(const verdandi::client& transport, const catalog_configuration& configuration) {
    auto driver = verdandi::detail::driver_access::get(transport);
    auto core = detail::client_core::open(std::move(driver), configuration);
    if (!core) {
        return std::unexpected(core.error());
    }
    return client(std::move(*core));
}

result<void> client::close() const {
    return core_ ? core_->close() : result<void>{};
}

bool client::open() const noexcept {
    return core_ && core_->open();
}

result<publisher> publisher::create(const client& owner) {
    if (!owner.core_ || !owner.core_->open()) {
        return std::unexpected(error(code::closed));
    }
    return publisher(owner.core_);
}

result<mutation_result> publisher::apply(const path& target, patch value) const {
    return detail::catalog_patch(core_, target, std::move(value));
}

result<mutation_result> publisher::erase(const path& target) const {
    return detail::catalog_erase(core_, target);
}

} // namespace verdandi::catalog

namespace verdandi::catalog::detail {

namespace {

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

[[nodiscard]] bool valid_field_name(const std::string_view value) noexcept {
    return !value.empty() && value.front() != '@' && valid_utf8(value);
}

[[nodiscard]] std::string kind_name(const kind value) {
    switch (value) {
    case kind::value:
        return "value";
    case kind::array:
        return "array";
    case kind::map:
        return "map";
    }
    return {};
}

[[nodiscard]] result<std::size_t> array_index(const std::string_view name, const std::size_t count) {
    if (count == 0 || name.empty() || (name.size() > 1 && name.front() == '0')) {
        return std::unexpected(error(code::contract, "array"));
    }
    std::size_t output{};
    const auto [end, status] = std::from_chars(name.data(), name.data() + name.size(), output);
    if (status != std::errc{} || end != name.data() + name.size() || output >= count) {
        return std::unexpected(error(code::contract, "array"));
    }
    return output;
}

struct validated_value {
    std::vector<std::string_view> names;
    std::size_t bytes{};
};

/// 校验完整 Catalog 形状和编码字节，并返回协议确定顺序。
[[nodiscard]] result<validated_value> validate_value(const kind shape, const fields& value, const std::size_t maximum_bytes) {
    if (kind_name(shape).empty()) {
        return std::unexpected(error(code::invalid, "kind"));
    }
    if (value.size() > maximum_fields) {
        return std::unexpected(error(code::capacity, "fields"));
    }
    validated_value output;
    output.names.reserve(value.size());
    if (shape == kind::value) {
        const auto iterator = value.find("value");
        if (value.size() != 1 || iterator == value.end()) {
            return std::unexpected(error(code::contract, "value"));
        }
    }
    std::size_t expected_index{};
    for (const auto& [name, bytes] : value) {
        if (!valid_field_name(name)) {
            return std::unexpected(error(code::invalid, name));
        }
        if (shape == kind::array) {
            auto index = array_index(name, value.size());
            if (!index || *index != expected_index++) {
                return std::unexpected(error(code::contract, "array"));
            }
        }
        if (name.size() > maximum_bytes - std::min(output.bytes, maximum_bytes) ||
            bytes.size() > maximum_bytes - std::min(output.bytes + name.size(), maximum_bytes)) {
            return std::unexpected(error(code::capacity, "value"));
        }
        output.bytes += name.size() + bytes.size();
        output.names.push_back(name);
    }
    return output;
}

[[nodiscard]] result<std::vector<std::string_view>> validate_patch(const fields& value, const std::size_t maximum_bytes) {
    if (value.empty()) {
        return std::unexpected(error(code::invalid, "patch"));
    }
    if (value.size() > maximum_fields) {
        return std::unexpected(error(code::capacity, "fields"));
    }
    std::vector<std::string_view> output;
    output.reserve(value.size());
    std::size_t bytes{};
    for (const auto& [name, field] : value) {
        if (!valid_field_name(name)) {
            return std::unexpected(error(code::invalid, name));
        }
        if (name.size() > maximum_bytes - std::min(bytes, maximum_bytes) || field.size() > maximum_bytes - std::min(bytes + name.size(), maximum_bytes)) {
            return std::unexpected(error(code::capacity, "patch"));
        }
        bytes += name.size() + field.size();
        output.push_back(name);
    }
    return output;
}

[[nodiscard]] std::string binary_string(const bytes& value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::string prefix(const std::string_view zone) {
    return "verdandi:catalog:" + std::string(zone);
}

[[nodiscard]] std::string meta_key(const std::string_view zone) {
    return prefix(zone) + ":@meta";
}

[[nodiscard]] std::string live_key(const std::string_view zone) {
    return prefix(zone) + ":@live";
}

[[nodiscard]] std::string deleted_key(const std::string_view zone) {
    return prefix(zone) + ":@deleted";
}

[[nodiscard]] std::string deleted_time_key(const std::string_view zone) {
    return prefix(zone) + ":@deleted_time";
}

[[nodiscard]] std::string value_key(const std::string_view zone, const path& target) {
    return prefix(zone) + ':' + std::string(target.part()) + ':' + std::string(target.id());
}

[[nodiscard]] std::string field_revisions_key(const std::string_view zone, const path& target) {
    return value_key(zone, target) + ":@field_revisions";
}

[[nodiscard]] std::vector<std::string> mutation_keys(const std::string_view zone, const path& target) {
    return {meta_key(zone), live_key(zone), deleted_key(zone), deleted_time_key(zone), value_key(zone, target), field_revisions_key(zone, target)};
}

[[nodiscard]] result<mutation_result> parse_mutation_reply(const verdandi::detail::response& response) {
    auto pairs = verdandi::detail::named_pairs(response);
    if (!pairs) {
        return std::unexpected(pairs.error());
    }
    std::map<std::string_view, std::string_view, std::less<>> values;
    for (const auto& pair : *pairs) {
        values.emplace(pair);
    }
    const auto result_value = values.find("&result");
    if (result_value == values.end()) {
        return std::unexpected(error(code::corrupt, "&result"));
    }
    if (result_value->second == "error") {
        const auto status = values.find("&status");
        if (status == values.end()) {
            return std::unexpected(error(code::corrupt, "&status"));
        }
        auto category = parse_code(status->second);
        if (!category || *category == code::deadline || *category == code::ambiguous || *category == code::closed) {
            return std::unexpected(error(code::protocol, "&status"));
        }
        std::string field;
        if (const auto found = values.find("&field"); found != values.end()) {
            field.assign(found->second);
        }
        error output(*category, std::move(field));
        if (const auto found = values.find("@revision"); found != values.end()) {
            auto revision = verdandi::detail::parse_unsigned(found->second, "@revision", true);
            if (!revision) {
                return std::unexpected(revision.error());
            }
            output = output.with_revision(*revision);
        }
        return std::unexpected(std::move(output));
    }
    if (result_value->second != "ok") {
        return std::unexpected(error(code::corrupt, "&result"));
    }
    const auto revision_value = values.find("@revision");
    if (revision_value == values.end()) {
        return std::unexpected(error(code::corrupt, "@revision"));
    }
    auto revision = verdandi::detail::parse_unsigned(revision_value->second, "@revision");
    if (!revision) {
        return std::unexpected(revision.error());
    }
    return mutation_result{*revision};
}

[[nodiscard]] result<std::uint64_t> parse_nonnegative(const verdandi::detail::response& response, const std::string_view field, const std::uint64_t maximum) {
    auto text = response.text();
    auto value = text ? verdandi::detail::parse_unsigned(*text, field, true) : result<std::uint64_t>(std::unexpected(error(code::corrupt, std::string(field))));
    if (!value || *value > maximum) {
        return std::unexpected(value ? error(code::corrupt, std::string(field)) : value.error());
    }
    return value;
}

} // namespace

result<std::size_t> validate_catalog_value(const kind shape, const fields& value, const std::size_t maximum_bytes) {
    auto validated = validate_value(shape, value, maximum_bytes);
    if (!validated) {
        return std::unexpected(validated.error());
    }
    return validated->bytes;
}

result<void> validate_catalog_patch(const fields& value, const std::size_t maximum_bytes) {
    auto validated = validate_patch(value, maximum_bytes);
    if (!validated) {
        return std::unexpected(validated.error());
    }
    return {};
}

std::optional<kind> parse_catalog_kind(const std::string_view value) noexcept {
    if (value == "value") {
        return kind::value;
    }
    if (value == "array") {
        return kind::array;
    }
    if (value == "map") {
        return kind::map;
    }
    return std::nullopt;
}

std::string catalog_prefix(const std::string_view zone) {
    return prefix(zone);
}

std::string catalog_meta_key(const std::string_view zone) {
    return meta_key(zone);
}

std::string catalog_live_key(const std::string_view zone) {
    return live_key(zone);
}

std::string catalog_deleted_key(const std::string_view zone) {
    return deleted_key(zone);
}

std::string catalog_deleted_time_key(const std::string_view zone) {
    return deleted_time_key(zone);
}

std::string catalog_value_key(const std::string_view zone, const path& target) {
    return value_key(zone, target);
}

std::string catalog_field_revisions_key(const std::string_view zone, const path& target) {
    return field_revisions_key(zone, target);
}

std::vector<std::string> catalog_read_keys(const std::string_view zone, const path& target) {
    return {live_key(zone), deleted_key(zone), deleted_time_key(zone), value_key(zone, target), field_revisions_key(zone, target)};
}

client_core::client_core(std::shared_ptr<verdandi::detail::driver> transport, catalog_configuration configuration)
    : transport_(std::move(transport)), configuration_(std::move(configuration)),
      read_script_(verdandi::detail::catalog_script(verdandi::detail::catalog_operation::read)),
      replace_script_(verdandi::detail::catalog_script(verdandi::detail::catalog_operation::replace)),
      patch_script_(verdandi::detail::catalog_script(verdandi::detail::catalog_operation::patch)),
      delete_script_(verdandi::detail::catalog_script(verdandi::detail::catalog_operation::delete_value)) {}

client_core::~client_core() {
    static_cast<void>(close());
}

result<std::shared_ptr<client_core>> client_core::open(std::shared_ptr<verdandi::detail::driver> transport, const catalog_configuration& configuration) {
    if (!transport || !transport->open()) {
        return std::unexpected(error(code::closed, "redis"));
    }
    if (auto status = configuration.check(); !status) {
        return std::unexpected(status.error());
    }
    auto output = std::shared_ptr<client_core>(new client_core(std::move(transport), configuration));
    if (auto status = output->bootstrap(); !status) {
        return std::unexpected(status.error());
    }
    return output;
}

result<void> client_core::bootstrap() {
    verdandi::detail::command info("INFO");
    info.add("SERVER");
    auto server = transport_->execute(info);
    if (!server) {
        return std::unexpected(server.error());
    }
    auto text = server->text();
    const auto marker = text ? text->find("redis_version:") : std::string_view::npos;
    if (!text || marker == std::string_view::npos) {
        return std::unexpected(error(code::corrupt, "redis_version"));
    }
    const auto version = text->substr(marker + 14);
    std::uint32_t major{};
    const auto [end, status] = std::from_chars(version.data(), version.data() + version.size(), major);
    if (status != std::errc{} || end == version.data() || major < 8) {
        return std::unexpected(error(code::protocol, "redis_version"));
    }
    if (!configuration_.local_store_path.empty()) {
        auto store = checkpoint_store::open(configuration_.local_store_path, transport_->timeout());
        if (!store) {
            return std::unexpected(store.error());
        }
        store_ = std::move(*store);
    }
    return {};
}

result<void> client_core::close() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return {};
    }
    std::vector<std::shared_ptr<subscriber_core>> children;
    {
        std::lock_guard lock(children_mutex_);
        for (const auto& weak : children_) {
            if (auto value = weak.lock()) {
                children.push_back(std::move(value));
            }
        }
        children_.clear();
    }
    std::optional<error> failure;
    for (const auto& child : children) {
        if (auto status = child->close(); !status && !failure) {
            failure = status.error();
        }
    }
    std::unique_lock operations(operations_);
    store_.reset();
    if (failure) {
        return std::unexpected(*failure);
    }
    return {};
}

bool client_core::open() const noexcept {
    return !closed_.load(std::memory_order_acquire) && transport_ && transport_->open();
}

const catalog_configuration& client_core::configuration() const noexcept {
    return configuration_;
}

std::shared_ptr<verdandi::detail::driver> client_core::transport() const noexcept {
    return transport_;
}

verdandi::detail::script& client_core::read_script() noexcept {
    return read_script_;
}

verdandi::detail::script& client_core::replace_script() noexcept {
    return replace_script_;
}

verdandi::detail::script& client_core::patch_script() noexcept {
    return patch_script_;
}

verdandi::detail::script& client_core::delete_script() noexcept {
    return delete_script_;
}

std::shared_mutex& client_core::operations() noexcept {
    return operations_;
}

std::shared_ptr<checkpoint_store> client_core::store() const noexcept {
    return store_;
}

void client_core::add(const std::shared_ptr<subscriber_core>& value) {
    std::lock_guard lock(children_mutex_);
    children_.erase(std::remove_if(children_.begin(), children_.end(), [](const auto& child) { return child.expired(); }), children_.end());
    children_.push_back(value);
}

result<mutation_result> catalog_replace(const std::shared_ptr<client_core>& owner, const path& target, const kind shape, fields value) {
    if (!owner || !owner->open()) {
        return std::unexpected(error(code::closed));
    }
    if (!target.valid()) {
        return std::unexpected(error(code::invalid, "path"));
    }
    auto validated = validate_value(shape, value, owner->configuration().max_record_bytes);
    if (!validated) {
        return std::unexpected(validated.error());
    }
    std::vector<std::string> arguments;
    arguments.reserve(4 + validated->names.size() * 2);
    arguments.push_back(target.member());
    arguments.push_back(kind_name(shape));
    arguments.push_back(std::to_string(validated->bytes));
    arguments.push_back(std::to_string(validated->names.size()));
    for (const auto name : validated->names) {
        arguments.emplace_back(name);
        arguments.push_back(binary_string(value.find(name)->second));
    }
    auto keys = mutation_keys(owner->configuration().zone, target);
    std::shared_lock operation(owner->operations());
    if (!owner->open()) {
        return std::unexpected(error(code::closed));
    }
    auto response = owner->replace_script().run(*owner->transport(), keys, arguments, true);
    return response ? parse_mutation_reply(*response) : result<mutation_result>(std::unexpected(response.error()));
}

result<mutation_result> catalog_patch(const std::shared_ptr<client_core>& owner, const path& target, patch value) {
    if (!owner || !owner->open()) {
        return std::unexpected(error(code::closed));
    }
    if (!target.valid()) {
        return std::unexpected(error(code::invalid, "path"));
    }
    if (value.base_revision == 0 || value.base_revision > maximum_revision) {
        return std::unexpected(error(code::invalid, "@base_revision"));
    }
    auto names = validate_patch(value.set, owner->configuration().max_record_bytes);
    if (!names) {
        return std::unexpected(names.error());
    }
    std::shared_lock operation(owner->operations());
    if (!owner->open()) {
        return std::unexpected(error(code::closed));
    }
    verdandi::detail::command read("HMGET");
    read.add(value_key(owner->configuration().zone, target)).add("@revision").add("@kind").add("@encoded_bytes");
    for (const auto name : *names) {
        read.add(name);
    }
    auto header = owner->transport()->execute(read);
    if (!header) {
        return std::unexpected(header.error());
    }
    if (header->type != verdandi::detail::response::kind::array || header->children.size() != names->size() + 3 ||
        header->children[0].type == verdandi::detail::response::kind::null || header->children[1].type == verdandi::detail::response::kind::null ||
        header->children[2].type == verdandi::detail::response::kind::null) {
        return std::unexpected(error(code::corrupt, "catalog_header"));
    }
    auto revision = parse_nonnegative(header->children[0], "@revision", maximum_revision);
    if (!revision) {
        return std::unexpected(revision.error());
    }
    if (*revision != value.base_revision) {
        return std::unexpected(error(code::stale, "@base_revision").with_revision(*revision));
    }
    auto shape_text = header->children[1].text();
    const bool array = shape_text && *shape_text == "array";
    if (!shape_text || (*shape_text != "array" && *shape_text != "map")) {
        return std::unexpected(error(code::transition, "@kind").with_revision(*revision));
    }
    auto projected = parse_nonnegative(header->children[2], "@encoded_bytes", owner->configuration().max_record_bytes);
    if (!projected) {
        return std::unexpected(projected.error());
    }
    for (std::size_t index = 0; index < names->size(); ++index) {
        const auto& old = header->children[index + 3];
        const auto found = value.set.find((*names)[index]);
        if (old.type == verdandi::detail::response::kind::null) {
            if (array) {
                return std::unexpected(error(code::transition, std::string((*names)[index])).with_revision(*revision));
            }
            if ((*names)[index].size() > owner->configuration().max_record_bytes - *projected ||
                found->second.size() > owner->configuration().max_record_bytes - *projected - (*names)[index].size()) {
                return std::unexpected(error(code::capacity, "value").with_revision(*revision));
            }
            *projected += (*names)[index].size() + found->second.size();
        } else {
            auto previous = old.text();
            if (!previous || previous->size() > *projected) {
                return std::unexpected(error(code::corrupt, std::string((*names)[index])));
            }
            *projected = *projected - previous->size() + found->second.size();
            if (*projected > owner->configuration().max_record_bytes) {
                return std::unexpected(error(code::capacity, "value").with_revision(*revision));
            }
        }
    }
    std::vector<std::string> arguments;
    arguments.reserve(4 + names->size() * 2);
    arguments.push_back(target.member());
    arguments.push_back(std::to_string(value.base_revision));
    arguments.push_back(std::to_string(*projected));
    arguments.push_back(std::to_string(names->size()));
    for (const auto name : *names) {
        arguments.emplace_back(name);
        arguments.push_back(binary_string(value.set.find(name)->second));
    }
    auto keys = mutation_keys(owner->configuration().zone, target);
    auto response = owner->patch_script().run(*owner->transport(), keys, arguments, true);
    return response ? parse_mutation_reply(*response) : result<mutation_result>(std::unexpected(response.error()));
}

result<mutation_result> catalog_erase(const std::shared_ptr<client_core>& owner, const path& target) {
    if (!owner || !owner->open()) {
        return std::unexpected(error(code::closed));
    }
    if (!target.valid()) {
        return std::unexpected(error(code::invalid, "path"));
    }
    auto keys = mutation_keys(owner->configuration().zone, target);
    const std::vector<std::string> arguments{target.member()};
    std::shared_lock operation(owner->operations());
    if (!owner->open()) {
        return std::unexpected(error(code::closed));
    }
    auto response = owner->delete_script().run(*owner->transport(), keys, arguments, true);
    return response ? parse_mutation_reply(*response) : result<mutation_result>(std::unexpected(response.error()));
}

} // namespace verdandi::catalog::detail
