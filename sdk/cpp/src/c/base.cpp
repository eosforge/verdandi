#include "c/internal.hpp"

#include <cstring>

namespace verdandi::c_api {

namespace {

template <std::size_t Size>
void copy_text(char (&output)[Size], const std::string_view value) noexcept {
    static_assert(Size > 0);
    const auto count = std::min(value.size(), Size - 1);
    if (count != 0) {
        std::memcpy(output, value.data(), count);
    }
    output[count] = '\0';
}

template <std::size_t Size>
[[nodiscard]] std::string_view bounded_text(const char (&value)[Size]) noexcept {
    const auto end = std::find(value, value + Size, '\0');
    return {value, static_cast<std::size_t>(end - value)};
}

} // namespace

void clear_error(verdandi_error* output) noexcept {
    if (output != nullptr) {
        *output = {};
    }
}

void write_error(verdandi_error* output, const error& value) noexcept {
    if (output == nullptr) {
        return;
    }
    *output = {};
    copy_text(output->code, to_string(value.category()));
    copy_text(output->field, value.field());
    copy_text(output->detail, value.detail());
    if (const auto revision = value.revision()) {
        output->revision = *revision;
        output->has_revision = 1;
    }
}

error read_callback_error(const verdandi_error& value) {
    const auto parsed = parse_code(bounded_text(value.code));
    auto output = error(parsed.value_or(code::contract), std::string(bounded_text(value.field)));
    if (value.has_revision != 0) {
        output = output.with_revision(value.revision);
    }
    const auto detail = bounded_text(value.detail);
    if (!detail.empty()) {
        output = output.with_detail(std::string(detail));
    }
    return output;
}

result<std::string_view> read_text(const verdandi_string_view value, const std::string_view field) {
    if (value.size != 0 && value.data == nullptr) {
        return std::unexpected(error(code::invalid, std::string(field)));
    }
    return std::string_view(value.data == nullptr ? "" : value.data, value.size);
}

result<std::span<const std::byte>> read_bytes(const verdandi_bytes_view value, const std::string_view field) {
    if (value.size != 0 && value.data == nullptr) {
        return std::unexpected(error(code::invalid, std::string(field)));
    }
    const auto* data = reinterpret_cast<const std::byte*>(value.data);
    return std::span<const std::byte>(data, value.size);
}

result<fields> read_fields(const verdandi_fields_view value, const std::string_view field) {
    if (value.size != 0 && value.data == nullptr) {
        return std::unexpected(error(code::invalid, std::string(field)));
    }
    fields output;
    for (std::size_t index = 0; index < value.size; ++index) {
        auto name = read_text(value.data[index].name, field);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto encoded = read_bytes(value.data[index].value, field);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        const auto [iterator, inserted] = output.emplace(std::string(*name), bytes(encoded->begin(), encoded->end()));
        static_cast<void>(iterator);
        if (!inserted) {
            return std::unexpected(error(code::contract, std::string(field)));
        }
    }
    return output;
}

result<std::chrono::milliseconds> read_duration(const std::uint64_t value, const std::string_view field) {
    using representation = std::chrono::milliseconds::rep;
    if (value > static_cast<std::uint64_t>(std::numeric_limits<representation>::max())) {
        return std::unexpected(error(code::invalid, std::string(field)));
    }
    return std::chrono::milliseconds(static_cast<representation>(value));
}

result<void> visit_fields(const fields& value, const verdandi_field_visitor visitor, void* context) {
    if (visitor == nullptr) {
        return std::unexpected(error(code::invalid, "visitor"));
    }
    for (const auto& [name, encoded] : value) {
        const auto field_name = verdandi_string_view{name.data(), name.size()};
        const auto field_value = verdandi_bytes_view{reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()};
        if (visitor(context, field_name, field_value) == 0) {
            return std::unexpected(error(code::contract, "callback"));
        }
    }
    return {};
}

void write_metadata(const registration::metadata& value, verdandi_registration_metadata* output) noexcept {
    if (output == nullptr) {
        return;
    }
    *output = {verdandi_string_view{value.uuid.data(), value.uuid.size()}, value.revision, value.timestamp, value.ttl, value.version};
}

const char* catalog_status(const catalog::status value) noexcept {
    using enum catalog::status;
    switch (value) {
    case synchronizing:
        return "synchronizing";
    case present:
        return "present";
    case absent:
        return "absent";
    case deleted:
        return "deleted";
    case unavailable:
        return "unavailable";
    case closed:
        return "closed";
    }
    return "unavailable";
}

} // namespace verdandi::c_api

extern "C" {

std::uint32_t VERDANDI_C_CALL verdandi_c_abi_version(void) {
    return VERDANDI_C_ABI_VERSION;
}

void VERDANDI_C_CALL verdandi_error_reset(verdandi_error* value) {
    verdandi::c_api::clear_error(value);
}

verdandi_bytes_view VERDANDI_C_CALL verdandi_blob_view(const verdandi_blob* value) {
    if (value == nullptr) {
        return {};
    }
    return {reinterpret_cast<const std::uint8_t*>(value->value.data()), value->value.size()};
}

void VERDANDI_C_CALL verdandi_blob_release(verdandi_blob* value) {
    delete value;
}

std::size_t VERDANDI_C_CALL verdandi_field_set_size(const verdandi_field_set* value) {
    return value == nullptr ? 0 : value->ordered.size();
}

int VERDANDI_C_CALL verdandi_field_set_at(const verdandi_field_set* value, const std::size_t index, verdandi_field_view* output) {
    if (output != nullptr) {
        *output = {};
    }
    if (value == nullptr || output == nullptr || index >= value->ordered.size()) {
        return 0;
    }
    const auto& [name, encoded] = *value->ordered[index];
    *output = {{name.data(), name.size()}, {reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()}};
    return 1;
}

int VERDANDI_C_CALL verdandi_field_set_visit(const verdandi_field_set* value, const verdandi_field_visitor visitor, void* context, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "fields"));
        }
        return verdandi::c_api::visit_fields(value->value, visitor, context);
    });
}

void VERDANDI_C_CALL verdandi_field_set_release(verdandi_field_set* value) {
    delete value;
}

} // extern "C"
