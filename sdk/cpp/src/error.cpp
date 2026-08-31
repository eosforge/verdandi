#include "verdandi/error.hpp"

#include "verdandi/fields.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace verdandi {

namespace {

constexpr std::size_t max_detail_bytes = 512;

void truncate_detail(std::string& value) {
    if (value.size() > max_detail_bytes) {
        value.resize(max_detail_bytes);
    }
}

} // namespace

std::optional<code> parse_code(const std::string_view value) noexcept {
    using enum code;
    constexpr std::array values{invalid,    protocol,  contract, target,      capacity, missing,   stale,
                                transition, immutable, corrupt,  unavailable, deadline, ambiguous, closed};
    const auto iterator = std::ranges::find(values, value, [](const code item) { return to_string(item); });
    if (iterator == values.end()) {
        return std::nullopt;
    }
    return *iterator;
}

error::error(const code value) noexcept : category_(value) {}

error::error(const code value, std::string field) : category_(value), field_(std::move(field)) {}

error::error(const code value, std::string field, const std::uint64_t revision, std::string detail)
    : category_(value), field_(std::move(field)), revision_(revision == 0 ? std::nullopt : std::optional(revision)), detail_(std::move(detail)) {
    truncate_detail(detail_);
}

code error::category() const noexcept {
    return category_;
}

std::string_view error::field() const noexcept {
    return field_;
}

std::optional<std::uint64_t> error::revision() const noexcept {
    return revision_;
}

std::string_view error::detail() const noexcept {
    return detail_;
}

std::string error::message() const {
    std::string value = "verdandi: ";
    value.append(to_string(category_));
    if (!field_.empty()) {
        value.append(": field ").append(field_);
    }
    if (revision_) {
        value.append(": revision ").append(std::to_string(*revision_));
    }
    if (!detail_.empty()) {
        value.append(": ").append(detail_);
    }
    return value;
}

error error::with_revision(const std::uint64_t value) const {
    auto copy = *this;
    copy.revision_ = value == 0 ? std::nullopt : std::optional(value);
    return copy;
}

error error::with_detail(std::string value) const {
    auto copy = *this;
    truncate_detail(value);
    copy.detail_ = std::move(value);
    return copy;
}

result<bytes> field_codec<bytes>::encode(const std::span<const std::byte> value) {
    return bytes(value.begin(), value.end());
}

result<bytes> field_codec<bytes>::decode(const std::span<const std::byte> value) {
    return bytes(value.begin(), value.end());
}

result<bytes> field_codec<std::string>::encode(const std::string_view value) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return bytes(first, first + value.size());
}

result<std::string> field_codec<std::string>::decode(const std::span<const std::byte> value) {
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

result<bytes> field_codec<bool>::encode(const bool value) {
    return field_codec<std::string>::encode(value ? "true" : "false");
}

result<bool> field_codec<bool>::decode(const std::span<const std::byte> value) {
    const std::string_view text(reinterpret_cast<const char*>(value.data()), value.size());
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    return std::unexpected(error(code::invalid, "field"));
}

} // namespace verdandi
