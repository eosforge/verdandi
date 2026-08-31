#include "verdandi/catalog/path.hpp"
#include "verdandi/configuration.hpp"
#include "verdandi/schema.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <yyjson.h>

namespace {

struct attr {
    std::string region;
    std::int64_t shard{};

    friend bool operator==(const attr&, const attr&) = default;
};

struct data {
    std::int64_t power{};
    bool ready{};

    friend bool operator==(const data&, const data&) = default;
};

struct throwing_scalar {};

struct throwing_record {
    throwing_scalar value;
};

} // namespace

template <>
struct verdandi::field_codec<throwing_scalar> {
    static verdandi::result<verdandi::bytes> encode(const throwing_scalar&) {
        throw std::runtime_error("encode failure");
    }

    static verdandi::result<throwing_scalar> decode(std::span<const std::byte>) {
        throw std::runtime_error("decode failure");
    }
};

VERDANDI_SCHEMA(attr, VERDANDI_FIELD(attr, region), VERDANDI_FIELD(attr, shard));
VERDANDI_SCHEMA(data, VERDANDI_FIELD(data, power), VERDANDI_NAMED_FIELD(data, ready, "available"));
VERDANDI_SCHEMA(throwing_record, VERDANDI_FIELD(throwing_record, value));

namespace {

int test_schema() {
    const attr input{"east", 7};
    const auto encoded = verdandi::encode_fields(input);
    if (!encoded || encoded->size() != 2 || encoded->find("region") == encoded->end() || encoded->find("shard") == encoded->end()) {
        std::cerr << "schema encoding failed\n";
        return 1;
    }
    const auto decoded = verdandi::decode_fields<attr>(*encoded);
    if (!decoded || *decoded != input) {
        std::cerr << "schema round trip failed\n";
        return 1;
    }
    const data state{42, true};
    const auto state_fields = verdandi::encode_fields(state);
    if (!state_fields || state_fields->find("available") == state_fields->end()) {
        std::cerr << "named field encoding failed\n";
        return 1;
    }
    auto extra = *encoded;
    extra.emplace("extra", verdandi::bytes{});
    if (verdandi::decode_fields<attr>(extra)) {
        std::cerr << "extra field was accepted\n";
        return 1;
    }
    const auto throwing_encode = verdandi::encode_fields(throwing_record{});
    if (throwing_encode || throwing_encode.error().category() != verdandi::code::contract || throwing_encode.error().field() != "value") {
        std::cerr << "codec exception escaped encoding\n";
        return 1;
    }
    verdandi::fields throwing_fields;
    throwing_fields.emplace("value", verdandi::bytes{});
    const auto throwing_decode = verdandi::decode_fields<throwing_record>(throwing_fields);
    if (throwing_decode || throwing_decode.error().category() != verdandi::code::contract || throwing_decode.error().field() != "value") {
        std::cerr << "codec exception escaped decoding\n";
        return 1;
    }
    const std::array leading_zero{std::byte{'0'}, std::byte{'1'}};
    if (verdandi::field_codec<std::int64_t>::decode(leading_zero)) {
        std::cerr << "non-canonical integer was accepted\n";
        return 1;
    }
    const std::array negative_zero{std::byte{'-'}, std::byte{'0'}};
    if (verdandi::field_codec<std::int64_t>::decode(negative_zero)) {
        std::cerr << "negative zero was accepted\n";
        return 1;
    }
    return 0;
}

int test_catalog_path() {
    const auto valid = verdandi::catalog::path::create("routing", "primary-1");
    if (!valid || valid->member() != "routing:primary-1") {
        std::cerr << "Catalog path construction failed\n";
        return 1;
    }
    if (verdandi::catalog::path::create("bad:part", "primary") || verdandi::catalog::path::create("routing", "")) {
        std::cerr << "invalid Catalog path was accepted\n";
        return 1;
    }
    return 0;
}

verdandi::result<verdandi::configuration> parse(const std::string_view source) {
    const auto* first = reinterpret_cast<const std::byte*>(source.data());
    return verdandi::configuration::from_json(std::span(first, source.size()));
}

struct json_document_deleter {
    void operator()(yyjson_doc* document) const noexcept {
        yyjson_doc_free(document);
    }
};

struct json_text_deleter {
    void operator()(char* text) const noexcept {
        std::free(text);
    }
};

int test_configuration_conformance() {
    std::ifstream input(VERDANDI_CONFIGURATION_CONFORMANCE, std::ios::binary);
    const std::string source{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    std::unique_ptr<yyjson_doc, json_document_deleter> document{yyjson_read(source.data(), source.size(), YYJSON_READ_NOFLAG)};
    auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
    auto* cases = root ? yyjson_obj_get(root, "cases") : nullptr;
    if (!input.is_open() || input.bad() || !yyjson_is_arr(cases)) {
        std::cerr << "configuration conformance corpus is invalid\n";
        return 1;
    }

    yyjson_arr_iter iterator = yyjson_arr_iter_with(cases);
    while (auto* test_case = yyjson_arr_iter_next(&iterator)) {
        auto* name_value = yyjson_obj_get(test_case, "name");
        auto* valid_value = yyjson_obj_get(test_case, "valid");
        auto* field_value = yyjson_obj_get(test_case, "field");
        auto* configuration_value = yyjson_obj_get(test_case, "document");
        if (!yyjson_is_str(name_value) || !yyjson_is_bool(valid_value) || !yyjson_is_obj(configuration_value) ||
            (field_value != nullptr && !yyjson_is_str(field_value))) {
            std::cerr << "configuration conformance case shape is invalid\n";
            return 1;
        }

        std::size_t size{};
        std::unique_ptr<char, json_text_deleter> serialized{yyjson_val_write(configuration_value, YYJSON_WRITE_NOFLAG, &size)};
        if (!serialized) {
            std::cerr << "configuration conformance case cannot be serialized\n";
            return 1;
        }

        const std::string_view name{yyjson_get_str(name_value), yyjson_get_len(name_value)};
        const bool expected_valid = yyjson_get_bool(valid_value);
        const auto result = parse(std::string_view{serialized.get(), size});
        if (static_cast<bool>(result) != expected_valid) {
            std::cerr << "configuration conformance result differs for " << name << '\n';
            return 1;
        }
        if (!result && field_value != nullptr) {
            const std::string_view expected_field{yyjson_get_str(field_value), yyjson_get_len(field_value)};
            if (result.error().field() != expected_field) {
                std::cerr << "configuration conformance field differs for " << name << '\n';
                return 1;
            }
        }
    }
    return 0;
}

int test_configuration() {
    const auto loaded = verdandi::configuration::load_json(std::filesystem::path(VERDANDI_CONFIGURATION_EXAMPLE));
    if (!loaded || loaded->redis.addresses.size() != 1 || !loaded->registration_enabled || !loaded->catalog_enabled ||
        loaded->registration.policy.attr_max_fields != 16 || loaded->catalog.max_record_bytes != 524'288) {
        std::cerr << "shared configuration example failed\n";
        return 1;
    }
    constexpr std::string_view minimal =
        R"({"version":"v1","redis":{"mode":"standalone","addresses":["127.0.0.1:6379"]},"catalog":{"zone":"Alpha","max_view_bytes":0}})";
    const auto valid = parse(minimal);
    if (!valid || !valid->catalog_enabled || valid->catalog.max_view_bytes != 0 || valid->registration_enabled) {
        std::cerr << "minimal configuration failed\n";
        return 1;
    }
    constexpr std::string_view duplicate = R"({"version":"v1","version":"v1","redis":{"mode":"standalone","addresses":["127.0.0.1:6379"]}})";
    const auto duplicate_result = parse(duplicate);
    if (duplicate_result || duplicate_result.error().field() != "json") {
        std::cerr << "duplicate field was accepted\n";
        return 1;
    }
    constexpr std::string_view unknown = R"({"version":"v1","redis":{"mode":"standalone","addresses":["127.0.0.1:6379"],"retry":1}})";
    const auto unknown_result = parse(unknown);
    if (unknown_result || unknown_result.error().field() != "json") {
        std::cerr << "unknown field was accepted\n";
        return 1;
    }
    constexpr std::string_view fractional = R"({"version":"v1","redis":{"mode":"standalone","addresses":["127.0.0.1:6379"],"timeout_ms":10.5}})";
    if (parse(fractional)) {
        std::cerr << "fractional number was accepted\n";
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    if (const auto status = test_schema(); status != 0) {
        return status;
    }
    if (const auto status = test_catalog_path(); status != 0) {
        return status;
    }
    if (const auto status = test_configuration(); status != 0) {
        return status;
    }
    return test_configuration_conformance();
}
