#include "internal/catalog_value.hpp"

#include <cstddef>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

using verdandi::bytes;
using verdandi::code;
using verdandi::fields;
using verdandi::catalog::kind;
using verdandi::catalog::detail::encode_catalog_replace;
using verdandi::catalog::detail::maximum_fields;
using verdandi::catalog::detail::validate_catalog_patch;
using verdandi::catalog::detail::validate_catalog_value;

constexpr std::size_t maximum_bytes = 4 * 1024 * 1024;
int failures{};

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

template <class T>
void check_error(const verdandi::result<T>& result, const code category, const std::string_view field, const std::string_view message) {
    check(!result && result.error().category() == category && result.error().field() == field, message);
}

fields array_value(const std::size_t count) {
    fields output;
    for (std::size_t index = 0; index < count; ++index) {
        output.emplace(std::to_string(index), bytes{std::byte{'x'}});
    }
    return output;
}

void test_array_order() {
    for (const std::size_t count : {0U, 1U, 10U, 11U, 12U, 100U, 101U, 1024U, 65536U}) {
        const auto value = array_value(count);
        std::size_t expected_bytes{};
        for (std::size_t index = 0; index < count; ++index) {
            expected_bytes += std::to_string(index).size() + 1;
        }
        const auto size = validate_catalog_value(kind::array, value, maximum_bytes);
        check(size && *size == expected_bytes, "a contiguous array was rejected or miscounted");
        const auto encoded = encode_catalog_replace("routing:array", kind::array, value, maximum_bytes);
        if (!encoded) {
            check(false, "array Replace encoding failed");
            continue;
        }
        check(encoded->size() == 4 + count * 2, "Replace argument count is incorrect");
        check((*encoded)[0] == "routing:array" && (*encoded)[1] == "array" && (*encoded)[2] == std::to_string(expected_bytes) &&
                  (*encoded)[3] == std::to_string(count),
              "Replace controls are incorrect");
        for (std::size_t index = 0; index < count; ++index) {
            check((*encoded)[4 + index * 2] == std::to_string(index) && (*encoded)[5 + index * 2] == "x", "array fields were not encoded in numeric order");
        }
    }

    for (const std::string invalid : {"01", "-1", "+1", "1.0", "1e0", " 1", "18446744073709551616"}) {
        auto value = array_value(12);
        value.erase("10");
        value.emplace(invalid, bytes{});
        check_error(validate_catalog_value(kind::array, value, maximum_bytes), code::contract, "array", "a non-canonical array index was accepted");
    }
    auto hole = array_value(12);
    hole.erase("10");
    check_error(validate_catalog_value(kind::array, hole, maximum_bytes), code::contract, "array", "an array hole was accepted");
    hole.erase("0");
    check_error(validate_catalog_value(kind::array, hole, maximum_bytes), code::contract, "array", "an array without index zero was accepted");
    check_error(encode_catalog_replace("routing:array", kind::array, hole, maximum_bytes), code::contract, "array", "an invalid array was encoded");
}

void test_shapes_and_limits() {
    check_error(validate_catalog_value(static_cast<kind>(255), {}, maximum_bytes), code::invalid, "kind", "an unknown kind was accepted");
    check_error(validate_catalog_value(kind::value, {}, maximum_bytes), code::contract, "value", "an empty scalar shape was accepted");
    check_error(validate_catalog_value(kind::value, {{"other", {}}}, maximum_bytes), code::contract, "value", "an incorrect scalar field was accepted");
    check_error(validate_catalog_value(kind::value, {{"value", {}}, {"other", {}}}, maximum_bytes), code::contract, "value",
                "extra scalar fields were accepted");

    const auto empty_scalar = validate_catalog_value(kind::value, {{"value", {}}}, 5);
    check(empty_scalar && *empty_scalar == 5, "an empty scalar payload was rejected");
    const auto empty_map = validate_catalog_value(kind::map, {}, 0);
    check(empty_map && *empty_map == 0, "an empty map was rejected");
    check_error(validate_catalog_patch({}, maximum_bytes), code::invalid, "patch", "an empty Patch was accepted");

    fields value{{"key", bytes{std::byte{0}, std::byte{255}}}};
    const auto exact = validate_catalog_value(kind::map, value, 5);
    check(exact && *exact == 5, "exact byte capacity was rejected");
    check(validate_catalog_patch(value, 5).has_value(), "exact Patch capacity was rejected");
    for (const std::size_t limit : {0U, 2U, 3U, 4U}) {
        check_error(validate_catalog_value(kind::map, value, limit), code::capacity, "value", "an oversized value was accepted");
        check_error(validate_catalog_patch(value, limit), code::capacity, "patch", "an oversized Patch was accepted");
    }
    check(validate_catalog_value(kind::map, value, std::numeric_limits<std::size_t>::max()).has_value(), "size_t maximum capacity failed");
    value.emplace("next", bytes{});
    check_error(validate_catalog_value(kind::map, value, 8), code::capacity, "value", "cumulative byte capacity was not enforced");
    check_error(validate_catalog_patch(value, 8), code::capacity, "patch", "cumulative Patch capacity was not enforced");

    fields large{{"value", bytes(maximum_bytes - 5, std::byte{'x'})}};
    check(validate_catalog_value(kind::value, large, maximum_bytes).has_value(), "the exact 4 MiB scalar boundary was rejected");
    large.begin()->second.push_back(std::byte{'x'});
    check_error(validate_catalog_value(kind::value, large, maximum_bytes), code::capacity, "value", "a scalar above 4 MiB was accepted");

    auto full = array_value(maximum_fields);
    check(validate_catalog_patch(full, maximum_bytes).has_value(), "the maximum Patch field count was rejected");
    full.emplace(std::to_string(maximum_fields), bytes{});
    check_error(validate_catalog_value(kind::array, full, maximum_bytes), code::capacity, "fields", "too many array fields were accepted");
    check_error(validate_catalog_patch(full, maximum_bytes), code::capacity, "fields", "too many Patch fields were accepted");
}

void test_field_names() {
    for (const std::string name : {"", "@revision", "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80", "\xe4\xb8"}) {
        const fields value{{name, {}}};
        check_error(validate_catalog_value(kind::map, value, maximum_bytes), code::invalid, name, "an invalid field name was accepted");
        check_error(validate_catalog_value(kind::array, value, maximum_bytes), code::contract, "array", "an invalid array index used the map error category");
        check_error(validate_catalog_patch(value, maximum_bytes), code::invalid, name, "an invalid Patch field name was accepted");
    }
    for (const std::string name : {"\xe4\xb8\xad", "\xf4\x8f\xbf\xbf", ".attribute", "&control"}) {
        const fields value{{name, {}}};
        check(validate_catalog_value(kind::map, value, maximum_bytes).has_value(), "a valid Catalog field name was rejected");
        check(validate_catalog_patch(value, maximum_bytes).has_value(), "a valid Catalog Patch name was rejected");
    }
}

void test_encoding_ownership() {
    std::string member = "routing:map";
    const std::string binary_name("a\0b", 3);
    const std::string binary_value("\0\xff", 2);
    fields value{{"z", {}}, {binary_name, bytes{std::byte{0}, std::byte{255}}}};
    const auto encoded = encode_catalog_replace(member, kind::map, value, maximum_bytes);
    if (!encoded) {
        check(false, "binary Map encoding failed");
        return;
    }
    member.assign("changed");
    value.clear();
    const std::vector<std::string> expected{"routing:map", "map", "6", "2", binary_name, binary_value, "z", ""};
    check(*encoded == expected, "Map argument order, binary bytes, or detached ownership changed");

    const auto scalar = encode_catalog_replace("routing:value", kind::value, {{"value", {}}}, maximum_bytes);
    check(scalar && *scalar == std::vector<std::string>{"routing:value", "value", "5", "1", "value", ""}, "empty scalar argument encoding failed");
    const auto empty_map = encode_catalog_replace("routing:map", kind::map, {}, 0);
    check(empty_map && *empty_map == std::vector<std::string>{"routing:map", "map", "0", "0"}, "empty Map argument encoding failed");
}

} // namespace

int main() {
    test_array_order();
    test_shapes_and_limits();
    test_field_names();
    test_encoding_ownership();
    if (failures != 0) {
        std::cerr << failures << " Catalog value checks failed\n";
        return 1;
    }
    std::cout << "Catalog shape, bounds, Replace ABI, and ownership checks passed\n";
    return 0;
}
