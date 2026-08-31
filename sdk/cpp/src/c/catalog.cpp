#include "c/internal.hpp"

namespace {

[[nodiscard]] verdandi::result<verdandi::catalog::path> read_path(const verdandi_catalog_path_view value) {
    auto part = verdandi::c_api::read_text(value.part, "part");
    if (!part) {
        return std::unexpected(part.error());
    }
    auto id = verdandi::c_api::read_text(value.id, "id");
    if (!id) {
        return std::unexpected(id.error());
    }
    return verdandi::catalog::path::create(std::string(*part), std::string(*id));
}

[[nodiscard]] verdandi::result<verdandi::catalog::kind> read_kind(const verdandi_string_view value) {
    auto name = verdandi::c_api::read_text(value, "kind");
    if (!name) {
        return std::unexpected(name.error());
    }
    if (*name == "value") {
        return verdandi::catalog::kind::value;
    }
    if (*name == "array") {
        return verdandi::catalog::kind::array;
    }
    if (*name == "map") {
        return verdandi::catalog::kind::map;
    }
    return std::unexpected(verdandi::error(verdandi::code::invalid, "kind"));
}

[[nodiscard]] verdandi::result<verdandi::catalog::subscription> read_subscription(const verdandi_catalog_subscription* value) {
    if (value == nullptr) {
        return std::unexpected(verdandi::error(verdandi::code::invalid, "subscription"));
    }
    if ((value->part_count != 0 && value->parts == nullptr) || (value->path_count != 0 && value->paths == nullptr)) {
        return std::unexpected(verdandi::error(verdandi::code::invalid, "subscription"));
    }
    verdandi::catalog::subscription output;
    output.zone = value->zone != 0;
    output.parts.reserve(value->part_count);
    for (std::size_t index = 0; index < value->part_count; ++index) {
        auto part = verdandi::c_api::read_text(value->parts[index], "part");
        if (!part) {
            return std::unexpected(part.error());
        }
        output.parts.emplace_back(*part);
    }
    output.paths.reserve(value->path_count);
    for (std::size_t index = 0; index < value->path_count; ++index) {
        auto path = read_path(value->paths[index]);
        if (!path) {
            return std::unexpected(path.error());
        }
        output.paths.push_back(std::move(*path));
    }
    return output;
}

} // namespace

extern "C" {

int VERDANDI_C_CALL verdandi_catalog_client_open(verdandi_client* root, verdandi_catalog_client** output, verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (root == nullptr || output == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, root == nullptr ? "client" : "output"));
        }
        if (!root->configuration.catalog_enabled) {
            return std::unexpected(verdandi::error(verdandi::code::missing, "catalog"));
        }
        auto client = verdandi::catalog::client::open(root->value, root->configuration.catalog);
        if (!client) {
            return std::unexpected(client.error());
        }
        *output = new verdandi_catalog_client{std::move(*client)};
        return {};
    });
}

int VERDANDI_C_CALL verdandi_catalog_client_is_open(const verdandi_catalog_client* value) {
    return value != nullptr && value->value.open() ? 1 : 0;
}

int VERDANDI_C_CALL verdandi_catalog_client_close(verdandi_catalog_client* value, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "catalog"));
        }
        return value->value.close();
    });
}

void VERDANDI_C_CALL verdandi_catalog_client_release(verdandi_catalog_client* value) {
    if (value != nullptr) {
        static_cast<void>(value->value.close());
        delete value;
    }
}

int VERDANDI_C_CALL verdandi_catalog_publisher_create(verdandi_catalog_client* client, verdandi_catalog_publisher** output, verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (client == nullptr || output == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, client == nullptr ? "client" : "output"));
        }
        auto publisher = verdandi::catalog::publisher::create(client->value);
        if (!publisher) {
            return std::unexpected(publisher.error());
        }
        *output = new verdandi_catalog_publisher{std::move(*publisher)};
        return {};
    });
}

void VERDANDI_C_CALL verdandi_catalog_publisher_release(verdandi_catalog_publisher* value) {
    delete value;
}

int VERDANDI_C_CALL verdandi_catalog_replace(verdandi_catalog_publisher* publisher, const verdandi_catalog_path_view path, const verdandi_string_view kind,
                                             const verdandi_fields_view fields, std::uint64_t* revision, verdandi_error* error) {
    if (revision != nullptr) {
        *revision = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (publisher == nullptr || revision == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, publisher == nullptr ? "publisher" : "output"));
        }
        auto target = read_path(path);
        if (!target) {
            return std::unexpected(target.error());
        }
        auto shape = read_kind(kind);
        if (!shape) {
            return std::unexpected(shape.error());
        }
        auto value = verdandi::c_api::read_fields(fields, "fields");
        if (!value) {
            return std::unexpected(value.error());
        }
        auto result = publisher->value.replace(*target, *shape, *value);
        if (!result) {
            return std::unexpected(result.error());
        }
        *revision = result->revision;
        return {};
    });
}

int VERDANDI_C_CALL verdandi_catalog_patch(verdandi_catalog_publisher* publisher, const verdandi_catalog_path_view path, const std::uint64_t base_revision,
                                           const verdandi_fields_view fields, std::uint64_t* revision, verdandi_error* error) {
    if (revision != nullptr) {
        *revision = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (publisher == nullptr || revision == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, publisher == nullptr ? "publisher" : "output"));
        }
        auto target = read_path(path);
        if (!target) {
            return std::unexpected(target.error());
        }
        auto value = verdandi::c_api::read_fields(fields, "fields");
        if (!value) {
            return std::unexpected(value.error());
        }
        auto result = publisher->value.apply(*target, verdandi::catalog::patch{base_revision, std::move(*value)});
        if (!result) {
            return std::unexpected(result.error());
        }
        *revision = result->revision;
        return {};
    });
}

int VERDANDI_C_CALL verdandi_catalog_erase(verdandi_catalog_publisher* publisher, const verdandi_catalog_path_view path, std::uint64_t* revision,
                                           verdandi_error* error) {
    if (revision != nullptr) {
        *revision = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (publisher == nullptr || revision == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, publisher == nullptr ? "publisher" : "output"));
        }
        auto target = read_path(path);
        if (!target) {
            return std::unexpected(target.error());
        }
        auto result = publisher->value.erase(*target);
        if (!result) {
            return std::unexpected(result.error());
        }
        *revision = result->revision;
        return {};
    });
}

int VERDANDI_C_CALL verdandi_catalog_subscriber_create(verdandi_catalog_client* client, const verdandi_catalog_subscription* subscription,
                                                       verdandi_catalog_subscriber** output, verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (client == nullptr || output == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, client == nullptr ? "client" : "output"));
        }
        auto native = read_subscription(subscription);
        if (!native) {
            return std::unexpected(native.error());
        }
        auto subscriber = verdandi::catalog::subscriber::create(client->value, std::move(*native));
        if (!subscriber) {
            return std::unexpected(subscriber.error());
        }
        *output = new verdandi_catalog_subscriber{std::move(*subscriber)};
        return {};
    });
}

int VERDANDI_C_CALL verdandi_catalog_subscriber_find(verdandi_catalog_subscriber* subscriber, const verdandi_catalog_path_view path, int* found,
                                                     verdandi_catalog_entry** output, verdandi_error* error) {
    if (found != nullptr) {
        *found = 0;
    }
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (subscriber == nullptr || !subscriber->value || found == nullptr || output == nullptr) {
            const auto* field = subscriber == nullptr || !subscriber->value ? "subscriber" : "output";
            return std::unexpected(verdandi::error(verdandi::code::invalid, field));
        }
        auto target = read_path(path);
        if (!target) {
            return std::unexpected(target.error());
        }
        auto entry = subscriber->value->find(*target);
        if (entry) {
            *output = new verdandi_catalog_entry{std::move(entry)};
            *found = 1;
        }
        return {};
    });
}

int VERDANDI_C_CALL verdandi_catalog_subscriber_try_error(verdandi_catalog_subscriber* value, int* available, verdandi_error* error) {
    if (available != nullptr) {
        *available = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || !value->value || available == nullptr) {
            const auto* field = value == nullptr || !value->value ? "subscriber" : "output";
            return std::unexpected(verdandi::error(verdandi::code::invalid, field));
        }
        if (auto diagnostic = value->value->try_error()) {
            *available = 1;
            verdandi::c_api::write_error(error, *diagnostic);
        }
        return {};
    });
}

int VERDANDI_C_CALL verdandi_catalog_subscriber_close(verdandi_catalog_subscriber* value, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || !value->value) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "subscriber"));
        }
        return value->value->close();
    });
}

void VERDANDI_C_CALL verdandi_catalog_subscriber_release(verdandi_catalog_subscriber* value) {
    if (value != nullptr) {
        if (value->value) {
            static_cast<void>(value->value->close());
        }
        delete value;
    }
}

verdandi_string_view VERDANDI_C_CALL verdandi_catalog_entry_part(const verdandi_catalog_entry* value) {
    if (value == nullptr || !value->value) {
        return {};
    }
    const auto part = value->value->target().part();
    return {part.data(), part.size()};
}

verdandi_string_view VERDANDI_C_CALL verdandi_catalog_entry_id(const verdandi_catalog_entry* value) {
    if (value == nullptr || !value->value) {
        return {};
    }
    const auto id = value->value->target().id();
    return {id.data(), id.size()};
}

const char* VERDANDI_C_CALL verdandi_catalog_entry_status(const verdandi_catalog_entry* value) {
    return value == nullptr || !value->value ? "closed" : verdandi::c_api::catalog_status(value->value->state());
}

std::uint64_t VERDANDI_C_CALL verdandi_catalog_entry_revision(const verdandi_catalog_entry* value) {
    return value == nullptr || !value->value ? 0 : value->value->revision();
}

int VERDANDI_C_CALL verdandi_catalog_entry_is_synchronized(const verdandi_catalog_entry* value) {
    return value != nullptr && value->value && value->value->synchronized() ? 1 : 0;
}

int VERDANDI_C_CALL verdandi_catalog_entry_load(const verdandi_catalog_entry* value, std::uint64_t* revision, const char** status, int* synchronized,
                                                int* present, verdandi_field_set** output, verdandi_error* error) {
    if (revision != nullptr) {
        *revision = 0;
    }
    if (status != nullptr) {
        *status = "closed";
    }
    if (synchronized != nullptr) {
        *synchronized = 0;
    }
    if (present != nullptr) {
        *present = 0;
    }
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || !value->value || revision == nullptr || status == nullptr || synchronized == nullptr || present == nullptr ||
            output == nullptr) {
            const auto* field = value == nullptr || !value->value ? "entry" : "output";
            return std::unexpected(verdandi::error(verdandi::code::invalid, field));
        }
        auto snapshot = value->value->load<verdandi::fields>();
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        *revision = snapshot->revision;
        *status = verdandi::c_api::catalog_status(snapshot->state);
        *synchronized = snapshot->synchronized ? 1 : 0;
        if (snapshot->value) {
            *output = new verdandi_field_set(std::move(*snapshot->value));
            *present = 1;
        }
        return {};
    });
}

void VERDANDI_C_CALL verdandi_catalog_entry_release(verdandi_catalog_entry* value) {
    delete value;
}

} // extern "C"
