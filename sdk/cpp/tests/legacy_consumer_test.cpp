#include "verdandi/legacy.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
static_assert(_MSVC_LANG >= 201103L, "The legacy facade requires C++11");
static_assert(_MSVC_LANG < 202002L, "The legacy facade must not inherit the C++23 requirement");
#else
static_assert(__cplusplus >= 201103L, "The legacy facade requires C++11");
static_assert(__cplusplus < 202002L, "The legacy facade must not inherit the C++23 requirement");
#endif

struct test_attr {
    std::string region;
};

struct test_data {
    std::int64_t power;
    bool ready;

    test_data() : power(0), ready(false) {}
};

VERDANDI_LEGACY_SCHEMA(test_attr, VERDANDI_LEGACY_FIELD(test_attr, region));
VERDANDI_LEGACY_SCHEMA(test_data, VERDANDI_LEGACY_FIELD(test_data, power), VERDANDI_LEGACY_FIELD(test_data, ready));

static_assert(!std::is_copy_assignable<verdandi::legacy::result<test_data>>::value, "A value-bearing Legacy result must not expose unsafe Union reassignment");
static_assert(!std::is_move_assignable<verdandi::legacy::result<test_data>>::value, "A value-bearing Legacy result must not expose unsafe Union reassignment");

struct one_policy {
    one_policy() {}
    one_policy(const one_policy&) = delete;
    one_policy& operator=(const one_policy&) = delete;

    verdandi::legacy::result<verdandi::legacy::optional<verdandi::legacy::choice>> operator()(verdandi::legacy::candidates<test_attr, test_data>&) const {
        return verdandi::legacy::optional<verdandi::legacy::choice>();
    }
};

struct any_policy {
    verdandi::legacy::result<std::vector<verdandi::legacy::choice>> operator()(verdandi::legacy::candidates<test_attr, test_data>&) const {
        return std::vector<verdandi::legacy::choice>();
    }
};

struct raw_one_policy {
    verdandi::legacy::result<verdandi::legacy::optional<verdandi::legacy::choice>>
    operator()(verdandi::legacy::candidates<verdandi::legacy::fields, verdandi::legacy::fields>&) const {
        return verdandi::legacy::optional<verdandi::legacy::choice>();
    }
};

int main() {
    using namespace verdandi::legacy;

    test_attr attr;
    attr.region = "cn-east";
    test_data data;
    data.power = std::numeric_limits<std::int64_t>::min();
    data.ready = true;

    result<fields> encoded_attr = codec<test_attr>::encode(attr);
    result<fields> encoded_data = codec<test_data>::encode(data);
    if (!encoded_attr || !encoded_data || encoded_attr->size() != 1U || encoded_data->size() != 2U) {
        return 1;
    }
    result<test_data> decoded = codec<test_data>::decode(*encoded_data);
    if (!decoded || decoded->power != data.power || !decoded->ready) {
        return 2;
    }

    bytes negative_zero;
    negative_zero.push_back('-');
    negative_zero.push_back('0');
    if (value_codec<std::int64_t>::decode(negative_zero)) {
        return 3;
    }

    result<bytes> maximum_unsigned = value_codec<std::uint64_t>::encode(std::numeric_limits<std::uint64_t>::max());
    if (!maximum_unsigned || value_codec<std::uint64_t>::decode(*maximum_unsigned).value() != std::numeric_limits<std::uint64_t>::max()) {
        return 4;
    }
    bytes leading_zero;
    leading_zero.push_back('0');
    leading_zero.push_back('1');
    bytes positive_sign;
    positive_sign.push_back('+');
    positive_sign.push_back('1');
    bytes overflow = *maximum_unsigned;
    overflow.push_back('0');
    if (value_codec<std::uint64_t>::decode(leading_zero) || value_codec<std::uint64_t>::decode(positive_sign) || value_codec<std::uint64_t>::decode(overflow)) {
        return 5;
    }

    fields duplicate;
    if (!duplicate.insert("power", static_cast<std::int64_t>(1)) || duplicate.insert("power", static_cast<std::int64_t>(2))) {
        return 6;
    }
    result<test_data> missing = codec<test_data>::decode(duplicate);
    if (missing || missing.failure().code() != "missing" || missing.failure().field() != "ready") {
        return 7;
    }

    optional<std::string> optional_value;
    optional_value.emplace("first");
    optional<std::string> optional_copy(optional_value);
    optional_value = optional<std::string>("second");
    if (!optional_copy || *optional_copy != "first" || !optional_value || *optional_value != "second") {
        return 8;
    }

    result<void> successful;
    result<void> failed(error("invalid", "test"));
    result<void> copied_failure(failed);
    failed = successful;
    if (!successful || !failed || copied_failure || copied_failure.failure().field() != "test") {
        return 9;
    }

    result<client> root = client::open(std::string());
    if (root || root.failure().code() != "capacity") {
        return 10;
    }

    registration_client registration_domain;
    registration_options options;
    options.type = "Proxy";
    options.ttl = std::chrono::milliseconds(15000);
    result<registration<test_attr, test_data>> registration_value = registration<test_attr, test_data>::create(registration_domain, options);
    if (registration_value || registration_value.failure().code() != "invalid") {
        return 11;
    }
    registration<fields, fields> raw_registration;
    if (raw_registration.publish(duplicate, duplicate)) {
        return 12;
    }

    selector<test_attr, test_data> selector_value;
    if (selector_value.one(one_policy()) || selector_value.any(any_policy()) || selector_value.snapshot()) {
        return 13;
    }
    selector<fields, fields> raw_selector;
    if (raw_selector.one(raw_one_policy())) {
        return 14;
    }

    catalog_publisher publisher;
    catalog_path path("routing", "primary");
    if (publisher.replace(path, catalog_kind::map, data) || publisher.replace(path, catalog_kind::map, duplicate) || publisher.patch(path, 1U, data) ||
        publisher.erase(path)) {
        return 15;
    }

    catalog_entry entry;
    if (entry.load<test_data>() || entry.load<fields>()) {
        return 16;
    }
    return 0;
}
