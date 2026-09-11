#include "verdandi/catalog/path.hpp"

#include <algorithm>
#include <utility>

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

} // namespace verdandi::catalog