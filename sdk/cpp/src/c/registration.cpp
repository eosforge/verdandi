#include "c/internal.hpp"

extern "C" {

int VERDANDI_C_CALL verdandi_registration_client_open(verdandi_client* root, verdandi_registration_client** output, verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (root == nullptr || output == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, root == nullptr ? "client" : "output"));
        }
        if (!root->configuration.registration_enabled) {
            return std::unexpected(verdandi::error(verdandi::code::missing, "registration"));
        }
        auto client = verdandi::registration::client::open(root->value, root->configuration.registration);
        if (!client) {
            return std::unexpected(client.error());
        }
        *output = new verdandi_registration_client{std::move(*client)};
        return {};
    });
}

int VERDANDI_C_CALL verdandi_registration_client_is_open(const verdandi_registration_client* value) {
    return value != nullptr && value->value.open() ? 1 : 0;
}

int VERDANDI_C_CALL verdandi_registration_client_close(verdandi_registration_client* value, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "registration"));
        }
        return value->value.close();
    });
}

void VERDANDI_C_CALL verdandi_registration_client_release(verdandi_registration_client* value) {
    if (value != nullptr) {
        static_cast<void>(value->value.close());
        delete value;
    }
}

int VERDANDI_C_CALL verdandi_registration_create(verdandi_registration_client* client, const verdandi_registration_options* options,
                                                 verdandi_registration** output, verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (client == nullptr || options == nullptr || output == nullptr) {
            const auto* field = client == nullptr ? "client" : options == nullptr ? "options" : "output";
            return std::unexpected(verdandi::error(verdandi::code::invalid, field));
        }
        auto type = verdandi::c_api::read_text(options->type, "type");
        if (!type) {
            return std::unexpected(type.error());
        }
        auto ttl = verdandi::c_api::read_duration(options->ttl_ms, "ttl");
        if (!ttl) {
            return std::unexpected(ttl.error());
        }
        std::optional<std::chrono::milliseconds> renew_interval;
        if (options->has_renew_interval != 0) {
            auto renew = verdandi::c_api::read_duration(options->renew_interval_ms, "renew_interval");
            if (!renew) {
                return std::unexpected(renew.error());
            }
            renew_interval = *renew;
        }
        verdandi::registration::options native{std::string(*type), *ttl, renew_interval, options->version};
        auto registration = verdandi::registration::registration<verdandi::fields, verdandi::fields>::create(client->value, std::move(native));
        if (!registration) {
            return std::unexpected(registration.error());
        }
        *output = new verdandi_registration{std::move(*registration)};
        return {};
    });
}

verdandi_string_view VERDANDI_C_CALL verdandi_registration_uuid(const verdandi_registration* value) {
    if (value == nullptr) {
        return {};
    }
    const auto uuid = value->value.uuid();
    return {uuid.data(), uuid.size()};
}

int VERDANDI_C_CALL verdandi_registration_is_published(const verdandi_registration* value) {
    return value != nullptr && value->value.published() ? 1 : 0;
}

std::uint64_t VERDANDI_C_CALL verdandi_registration_revision(const verdandi_registration* value) {
    return value == nullptr ? 0 : value->value.revision();
}

std::uint64_t VERDANDI_C_CALL verdandi_registration_timestamp(const verdandi_registration* value) {
    return value == nullptr ? 0 : value->value.timestamp();
}

int VERDANDI_C_CALL verdandi_registration_publish(verdandi_registration* value, const verdandi_fields_view attr, const verdandi_fields_view data,
                                                  verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "registration"));
        }
        auto native_attr = verdandi::c_api::read_fields(attr, "attr");
        if (!native_attr) {
            return std::unexpected(native_attr.error());
        }
        auto native_data = verdandi::c_api::read_fields(data, "data");
        if (!native_data) {
            return std::unexpected(native_data.error());
        }
        return value->value.publish(*native_attr, *native_data);
    });
}

int VERDANDI_C_CALL verdandi_registration_update(verdandi_registration* value, const verdandi_fields_view data, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "registration"));
        }
        auto native = verdandi::c_api::read_fields(data, "data");
        if (!native) {
            return std::unexpected(native.error());
        }
        return value->value.update(*native);
    });
}

int VERDANDI_C_CALL verdandi_registration_set_version(verdandi_registration* value, const std::uint64_t version, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "registration"));
        }
        return value->value.set_version(version);
    });
}

int VERDANDI_C_CALL verdandi_registration_update_content(verdandi_registration* value, const std::uint64_t version, const verdandi_fields_view data,
                                                         verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "registration"));
        }
        auto native = verdandi::c_api::read_fields(data, "data");
        if (!native) {
            return std::unexpected(native.error());
        }
        return value->value.update_content(version, *native);
    });
}

int VERDANDI_C_CALL verdandi_registration_renew(verdandi_registration* value, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "registration"));
        }
        return value->value.renew();
    });
}

int VERDANDI_C_CALL verdandi_registration_close(verdandi_registration* value, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "registration"));
        }
        return value->value.close();
    });
}

int VERDANDI_C_CALL verdandi_registration_try_error(verdandi_registration* value, int* available, verdandi_error* error) {
    if (available != nullptr) {
        *available = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || available == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, value == nullptr ? "registration" : "output"));
        }
        if (auto diagnostic = value->value.try_error()) {
            *available = 1;
            verdandi::c_api::write_error(error, *diagnostic);
        }
        return {};
    });
}

void VERDANDI_C_CALL verdandi_registration_release(verdandi_registration* value) {
    if (value != nullptr) {
        static_cast<void>(value->value.close());
        delete value;
    }
}

} // extern "C"
