#include "c/internal.hpp"

namespace {

[[nodiscard]] verdandi::result<std::string_view> require_key(verdandi_client* client, const verdandi_string_view key) {
    if (client == nullptr) {
        return std::unexpected(verdandi::error(verdandi::code::invalid, "client"));
    }
    return verdandi::c_api::read_text(key, "key");
}

} // namespace

extern "C" {

int VERDANDI_C_CALL verdandi_configuration_validate_json(const verdandi_bytes_view json, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        auto source = verdandi::c_api::read_bytes(json, "json");
        if (!source) {
            return std::unexpected(source.error());
        }
        auto configuration = verdandi::configuration::from_json(*source);
        if (!configuration) {
            return std::unexpected(configuration.error());
        }
        return {};
    });
}

int VERDANDI_C_CALL verdandi_client_open_json(const verdandi_bytes_view json, verdandi_client** output, verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (output == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "output"));
        }
        auto source = verdandi::c_api::read_bytes(json, "json");
        if (!source) {
            return std::unexpected(source.error());
        }
        auto configuration = verdandi::configuration::from_json(*source);
        if (!configuration) {
            return std::unexpected(configuration.error());
        }
        auto client = verdandi::client::open(configuration->redis);
        if (!client) {
            return std::unexpected(client.error());
        }
        *output = new verdandi_client{std::move(*configuration), std::move(*client)};
        return {};
    });
}

int VERDANDI_C_CALL verdandi_client_is_open(const verdandi_client* value) {
    return value != nullptr && value->value.open() ? 1 : 0;
}

int VERDANDI_C_CALL verdandi_client_ping(verdandi_client* value, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "client"));
        }
        return value->value.ping();
    });
}

int VERDANDI_C_CALL verdandi_client_close(verdandi_client* value, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "client"));
        }
        return value->value.close();
    });
}

void VERDANDI_C_CALL verdandi_client_release(verdandi_client* value) {
    if (value != nullptr) {
        static_cast<void>(value->value.close());
        delete value;
    }
}

int VERDANDI_C_CALL verdandi_key_load(verdandi_client* client, const verdandi_string_view key, int* found, verdandi_blob** output, verdandi_error* error) {
    if (found != nullptr) {
        *found = 0;
    }
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (found == nullptr || output == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "output"));
        }
        auto name = require_key(client, key);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto loaded = client->value.key().load(*name);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (*loaded) {
            *output = new verdandi_blob{std::move(**loaded)};
            *found = 1;
        }
        return {};
    });
}

int VERDANDI_C_CALL verdandi_key_store(verdandi_client* client, const verdandi_string_view key, const verdandi_bytes_view value, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        auto name = require_key(client, key);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto encoded = verdandi::c_api::read_bytes(value, "value");
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return client->value.key().store(*name, *encoded);
    });
}

int VERDANDI_C_CALL verdandi_key_store_ttl(verdandi_client* client, const verdandi_string_view key, const verdandi_bytes_view value, const std::uint64_t ttl_ms,
                                           verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        auto name = require_key(client, key);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto encoded = verdandi::c_api::read_bytes(value, "value");
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        auto ttl = verdandi::c_api::read_duration(ttl_ms, "ttl");
        if (!ttl) {
            return std::unexpected(ttl.error());
        }
        return client->value.key().store(*name, *encoded, *ttl);
    });
}

int VERDANDI_C_CALL verdandi_key_erase(verdandi_client* client, const verdandi_string_view key, int* removed, verdandi_error* error) {
    if (removed != nullptr) {
        *removed = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (removed == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "output"));
        }
        auto name = require_key(client, key);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto result = client->value.key().erase(*name);
        if (!result) {
            return std::unexpected(result.error());
        }
        *removed = *result ? 1 : 0;
        return {};
    });
}

int VERDANDI_C_CALL verdandi_key_contains(verdandi_client* client, const verdandi_string_view key, int* present, verdandi_error* error) {
    if (present != nullptr) {
        *present = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (present == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "output"));
        }
        auto name = require_key(client, key);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto result = client->value.key().contains(*name);
        if (!result) {
            return std::unexpected(result.error());
        }
        *present = *result ? 1 : 0;
        return {};
    });
}

int VERDANDI_C_CALL verdandi_key_expire(verdandi_client* client, const verdandi_string_view key, const std::uint64_t ttl_ms, int* changed,
                                        verdandi_error* error) {
    if (changed != nullptr) {
        *changed = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (changed == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "output"));
        }
        auto name = require_key(client, key);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto ttl = verdandi::c_api::read_duration(ttl_ms, "ttl");
        if (!ttl) {
            return std::unexpected(ttl.error());
        }
        auto result = client->value.key().expire(*name, *ttl);
        if (!result) {
            return std::unexpected(result.error());
        }
        *changed = *result ? 1 : 0;
        return {};
    });
}

int VERDANDI_C_CALL verdandi_hash_load(verdandi_client* client, const verdandi_string_view key, verdandi_field_set** output, verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (output == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "output"));
        }
        auto name = require_key(client, key);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto loaded = client->value.hash().load(*name);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        *output = new verdandi_field_set(std::move(*loaded));
        return {};
    });
}

int VERDANDI_C_CALL verdandi_hash_store(verdandi_client* client, const verdandi_string_view key, const verdandi_fields_view value, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        auto name = require_key(client, key);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto fields = verdandi::c_api::read_fields(value, "fields");
        if (!fields) {
            return std::unexpected(fields.error());
        }
        return client->value.hash().store(*name, *fields);
    });
}

int VERDANDI_C_CALL verdandi_hash_erase(verdandi_client* client, const verdandi_string_view key, const verdandi_string_view* names, const std::size_t count,
                                        std::size_t* removed, verdandi_error* error) {
    if (removed != nullptr) {
        *removed = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (removed == nullptr || (count != 0 && names == nullptr)) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "output"));
        }
        auto key_name = require_key(client, key);
        if (!key_name) {
            return std::unexpected(key_name.error());
        }
        std::vector<std::string_view> native_names;
        native_names.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto name = verdandi::c_api::read_text(names[index], "name");
            if (!name) {
                return std::unexpected(name.error());
            }
            native_names.push_back(*name);
        }
        auto result = client->value.hash().erase(*key_name, native_names);
        if (!result) {
            return std::unexpected(result.error());
        }
        *removed = *result;
        return {};
    });
}

int VERDANDI_C_CALL verdandi_hash_contains(verdandi_client* client, const verdandi_string_view key, const verdandi_string_view name, int* present,
                                           verdandi_error* error) {
    if (present != nullptr) {
        *present = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (present == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "output"));
        }
        auto key_name = require_key(client, key);
        if (!key_name) {
            return std::unexpected(key_name.error());
        }
        auto field_name = verdandi::c_api::read_text(name, "name");
        if (!field_name) {
            return std::unexpected(field_name.error());
        }
        auto result = client->value.hash().contains(*key_name, *field_name);
        if (!result) {
            return std::unexpected(result.error());
        }
        *present = *result ? 1 : 0;
        return {};
    });
}

int VERDANDI_C_CALL verdandi_hash_size(verdandi_client* client, const verdandi_string_view key, std::size_t* size, verdandi_error* error) {
    if (size != nullptr) {
        *size = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (size == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "output"));
        }
        auto name = require_key(client, key);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto result = client->value.hash().size(*name);
        if (!result) {
            return std::unexpected(result.error());
        }
        *size = *result;
        return {};
    });
}

} // extern "C"
