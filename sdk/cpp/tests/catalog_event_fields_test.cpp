#include "internal/catalog_event_fields.hpp"
#include "internal/catalog_value.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using verdandi::bytes;
using verdandi::fields;
using verdandi::catalog::kind;
using verdandi::catalog::detail::encode_catalog_replace;
using verdandi::catalog::detail::event_cursor;
using verdandi::catalog::detail::event_fields;
using wire_fields = std::vector<std::pair<std::string, std::string>>;

int failures{};
std::size_t checks{};

void check(const bool passed, const std::string_view message) {
    ++checks;
    if (!passed) {
        ++failures;
        std::cerr << message << '\n';
    }
}

void append_u32(std::string& output, const std::uint32_t value) {
    for (const unsigned shift : {24U, 16U, 8U, 0U}) {
        output.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

std::string array_header(const std::size_t count) {
    std::string output;
    if (count < 16) {
        output.push_back(static_cast<char>(0x90U | count));
    } else if (count <= 65'535) {
        output.push_back(static_cast<char>(0xdc));
        output.push_back(static_cast<char>((count >> 8U) & 0xffU));
        output.push_back(static_cast<char>(count & 0xffU));
    } else {
        output.push_back(static_cast<char>(0xdd));
        append_u32(output, static_cast<std::uint32_t>(count));
    }
    return output;
}

void append_text(std::string& output, const std::string_view value) {
    if (value.size() < 32) {
        output.push_back(static_cast<char>(0xa0U | value.size()));
    } else {
        output.push_back(static_cast<char>(0xdb));
        append_u32(output, static_cast<std::uint32_t>(value.size()));
    }
    output.append(value);
}

std::string pack(const wire_fields& value) {
    auto output = array_header(value.size() * 2);
    for (const auto& [name, field] : value) {
        append_text(output, name);
        output.push_back(static_cast<char>(0xc6));
        append_u32(output, static_cast<std::uint32_t>(field.size()));
        output.append(field);
    }
    return output;
}

wire_fields array_value(const std::size_t count) {
    wire_fields output;
    for (std::size_t index = 0; index < count; ++index) {
        output.emplace_back(std::to_string(index), std::string("v\0\xff", 3));
    }
    return output;
}

bool accepted(const std::string_view payload, const bool array_replace) {
    event_cursor cursor(payload);
    const auto decoded = event_fields(cursor, array_replace);
    return decoded.has_value() && cursor.done();
}

void test_numeric_array_notifications() {
    for (const std::size_t count : {0U, 1U, 10U, 11U, 12U, 100U, 101U, 1024U, 65'536U}) {
        const auto value = array_value(count);
        auto payload = pack(value);
        event_cursor cursor(payload);
        const auto decoded = event_fields(cursor, true);
        check(decoded.has_value() && cursor.done(), "a numeric Array Replace notification was rejected: " + std::to_string(count));
        if (!decoded) {
            continue;
        }
        check(decoded->size() == count, "Array notification field count changed");
        payload.assign(payload.size(), '\0');
        for (const auto& [name, field] : value) {
            const auto found = decoded->find(name);
            const auto* data = reinterpret_cast<const std::byte*>(field.data());
            check(found != decoded->end() && found->second == bytes(data, data + field.size()), "Array field bytes or detached ownership changed");
        }
    }

    auto lexical = array_value(12);
    std::ranges::sort(lexical);
    check(!accepted(pack(lexical), true), "lexical order was accepted for Array Replace");
    check(accepted(pack(lexical), false), "lexical Map field order was rejected");
    check(!accepted(pack(array_value(12)), false), "numeric order was accepted for a lexical Map notification");

    check(accepted(pack({{"10", "ten"}, {"2", "two"}}), false), "lexical Array Patch order was rejected");
    check(!accepted(pack({{"2", "two"}, {"10", "ten"}}), false), "numeric Array Patch order was accepted");
    check(accepted(pack({{"value", ""}}), false), "empty scalar field was rejected");
}

void test_malformed_fields() {
    for (const std::string name : {"", "00", "01", "+1", "-1", " 1", "12", "99999999999999999999999999999999999", "\xe4\xb8\x80"}) {
        auto value = array_value(12);
        value[1].first = name;
        check(!accepted(pack(value), true), "a noncanonical or out-of-range array index was accepted");
    }
    auto duplicate = array_value(12);
    duplicate[1].first = "0";
    check(!accepted(pack(duplicate), true), "a duplicate array index was accepted");
    check(!accepted(pack({{"a", "one"}, {"a", "two"}}), false), "duplicate Map fields were accepted");
    check(!accepted(pack({{"", "empty"}}), false), "an empty Map field name was accepted");

    const auto payload = pack(array_value(12));
    for (std::size_t length = 0; length < payload.size(); ++length) {
        check(!accepted(std::string_view(payload).substr(0, length), true), "truncated Array fields were accepted");
    }
    check(!accepted(array_header(1), true), "an odd field element count was accepted");
    check(!accepted(array_header(65'537 * 2), true), "a field count above the protocol limit was accepted");
    check(!accepted(std::string(1, static_cast<char>(0xc0)), true), "a non-array MessagePack marker was accepted");
}

void test_publisher_notification_boundary() {
    fields original;
    for (std::size_t index = 0; index < 12; ++index) {
        original.emplace(std::to_string(index), bytes{std::byte{'x'}, std::byte{0}, std::byte{255}});
    }
    const auto arguments = encode_catalog_replace("routing:array", kind::array, original, 4096);
    check(arguments.has_value(), "Array publisher argument encoding failed");
    if (!arguments) {
        return;
    }
    wire_fields notification;
    for (std::size_t index = 0; index < original.size(); ++index) {
        check((*arguments)[4 + index * 2] == std::to_string(index), "publisher arguments do not follow numeric order");
        notification.emplace_back((*arguments)[4 + index * 2], (*arguments)[5 + index * 2]);
    }
    const auto payload = pack(notification);
    event_cursor cursor(payload);
    const auto decoded = event_fields(cursor, true);
    check(decoded && cursor.done() && *decoded == original, "publisher Array fields cannot be consumed as a Replace notification");
}

} // namespace

int main() {
    test_numeric_array_notifications();
    test_malformed_fields();
    test_publisher_notification_boundary();
    if (failures != 0) {
        std::cerr << failures << " of " << checks << " Catalog notification checks failed\n";
        return 1;
    }
    std::cout << checks << " Catalog notification order, framing, and ownership checks passed\n";
    return 0;
}
