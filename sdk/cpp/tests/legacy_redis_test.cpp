#include "verdandi/legacy.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct proxy_attr {
    std::string region;
};

struct proxy_data {
    std::int64_t power;
    bool ready;

    proxy_data() : power(0), ready(false) {}
};

VERDANDI_LEGACY_SCHEMA(proxy_attr, VERDANDI_LEGACY_FIELD(proxy_attr, region));
VERDANDI_LEGACY_SCHEMA(proxy_data, VERDANDI_LEGACY_FIELD(proxy_data, power), VERDANDI_LEGACY_FIELD(proxy_data, ready));

namespace {

using verdandi::legacy::bytes;
using verdandi::legacy::candidate;
using verdandi::legacy::candidates;
using verdandi::legacy::choice;
using verdandi::legacy::error;
using verdandi::legacy::optional;
using verdandi::legacy::result;

std::string make_zone(const char* prefix) {
    const std::uint64_t seed = static_cast<std::uint64_t>(std::time(NULL)) ^ static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(prefix));
    std::uint64_t value = seed;
    std::string output(prefix);
    for (std::size_t index = 0; index < 10U; ++index) {
        output.push_back(static_cast<char>('A' + value % 26U));
        value = value / 26U + 17U;
    }
    return output;
}

int fail(const char* step, const error& failure) {
    std::cerr << step << " failed: " << failure.code() << ' ' << failure.field() << ' ' << failure.detail() << '\n';
    return 1;
}

template <class T>
int require(const char* step, const result<T>& value) {
    return value ? 0 : fail(step, value.failure());
}

struct select_first {
    result<optional<choice>> operator()(candidates<proxy_attr, proxy_data>& values) const {
        if (values.size() == 0U) {
            return optional<choice>();
        }
        result<proxy_data> loaded = values.data(0U);
        if (!loaded) {
            return result<optional<choice>>(loaded.failure());
        }
        if (loaded->power != 1 || !loaded->ready) {
            return result<optional<choice>>(error("corrupt", "data"));
        }
        loaded->power = 2;
        result<void> changed = values.mutate(choice(0U), *loaded);
        if (!changed) {
            return result<optional<choice>>(changed.failure());
        }
        return optional<choice>(choice(0U));
    }
};

struct select_all {
    result<std::vector<choice>> operator()(candidates<proxy_attr, proxy_data>& values) const {
        std::vector<choice> output;
        output.reserve(values.size());
        for (std::size_t index = 0; index < values.size(); ++index) {
            output.push_back(choice(index));
        }
        return output;
    }
};

void erase_key(const verdandi::legacy::client& root, const std::string& key) {
    (void)root.key_erase(key);
}

} // namespace

int main() {
    using namespace verdandi::legacy;

    const char* address = std::getenv("VERDANDI_REDIS_ADDRESS");
    if (address == NULL || address[0] == '\0') {
        return 77;
    }

    const std::string registration_zone = make_zone("LegacyRegistration");
    const std::string catalog_zone = make_zone("LegacyCatalog");
    std::ostringstream json;
    json << "{\"version\":\"v1\",\"redis\":{\"mode\":\"standalone\",\"addresses\":[\"" << address << "\"]},\"registration\":{\"zone\":\"" << registration_zone
         << "\",\"selector\":{\"sync_timeout_ms\":5000}},\"catalog\":{\"zone\":\"" << catalog_zone << "\",\"sync_timeout_ms\":5000}}";

    result<client> opened = client::open(json.str());
    if (require("client open", opened) != 0) {
        return 1;
    }
    client root = std::move(*opened);
    result<void> pinged = root.ping();
    if (require("ping", pinged) != 0) {
        return 1;
    }

    const std::string key = "verdandi:legacy:" + registration_zone + ":key";
    const std::string hash = "verdandi:legacy:" + registration_zone + ":hash";
    bytes payload;
    payload.push_back('v');
    payload.push_back('1');
    result<void> stored = root.key_store(key, payload, std::chrono::milliseconds(5000));
    if (require("key store", stored) != 0) {
        return 1;
    }
    result<optional<bytes>> loaded_key = root.key_load(key);
    if (require("key load", loaded_key) != 0 || !*loaded_key || **loaded_key != payload) {
        return 1;
    }
    result<bool> key_present = root.key_contains(key);
    result<bool> key_expired = root.key_expire(key, std::chrono::milliseconds(5000));
    if (require("key contains", key_present) != 0 || !*key_present || require("key expire", key_expired) != 0 || !*key_expired) {
        return 1;
    }

    fields hash_value;
    if (!hash_value.insert("power", static_cast<std::int64_t>(1))) {
        return 1;
    }
    result<void> hash_stored = root.hash_store(hash, hash_value);
    result<fields> hash_loaded = root.hash_load(hash);
    result<bool> hash_present = root.hash_contains(hash, "power");
    result<std::size_t> hash_size = root.hash_size(hash);
    if (require("hash store", hash_stored) != 0 || require("hash load", hash_loaded) != 0 || hash_loaded->get<std::int64_t>("power").value() != 1 ||
        require("hash contains", hash_present) != 0 || !*hash_present || require("hash size", hash_size) != 0 || *hash_size != 1U) {
        return 1;
    }

    result<registration_client> registration_domain_result = registration_client::open(root);
    if (require("registration client", registration_domain_result) != 0) {
        return 1;
    }
    registration_client registration_domain = std::move(*registration_domain_result);
    registration_options options;
    options.type = "Proxy";
    options.ttl = std::chrono::milliseconds(5000);
    options.version = 1U;
    result<registration<proxy_attr, proxy_data>> created = registration<proxy_attr, proxy_data>::create(registration_domain, options);
    if (require("registration create", created) != 0) {
        return 1;
    }
    registration<proxy_attr, proxy_data> registration_value = std::move(*created);
    proxy_attr attr;
    attr.region = "east";
    proxy_data data;
    data.power = 1;
    data.ready = true;
    result<void> published = registration_value.publish(attr, data);
    if (require("registration publish", published) != 0 || !registration_value.is_published() || registration_value.uuid().size() != 32U) {
        return 1;
    }
    result<void> renewed = registration_value.renew();
    data.power = 3;
    result<void> updated = registration_value.update(data);
    result<void> versioned = registration_value.set_version(2U);
    data.power = 1;
    result<void> content_updated = registration_value.update_content(3U, data);
    result<diagnostic> registration_diagnostic = registration_value.try_error();
    if (require("registration renew", renewed) != 0 || require("registration update", updated) != 0 || require("registration version", versioned) != 0 ||
        require("registration content", content_updated) != 0 || require("registration diagnostic", registration_diagnostic) != 0 ||
        registration_value.revision() == 0U || registration_value.timestamp() == 0U) {
        return 1;
    }

    result<selector<proxy_attr, proxy_data>> selector_result = selector<proxy_attr, proxy_data>::create(registration_domain, "Proxy");
    if (require("selector create", selector_result) != 0) {
        return 1;
    }
    selector<proxy_attr, proxy_data> selector_value = std::move(*selector_result);
    result<optional<candidate<proxy_attr, proxy_data>>> selected = selector_value.one(select_first());
    if (require("selector one", selected) != 0 || !*selected || (*selected)->data().power != 2 || (*selected)->attr().region != "east") {
        return 1;
    }
    result<std::vector<candidate<proxy_attr, proxy_data>>> all = selector_value.any(select_all());
    if (require("selector any", all) != 0 || all->size() != 1U || (*all)[0].data().power != 2) {
        return 1;
    }
    result<selection_snapshot<proxy_attr, proxy_data>> selector_snapshot = selector_value.snapshot();
    result<diagnostic> selector_diagnostic = selector_value.try_error();
    if (require("selector snapshot", selector_snapshot) != 0 || !selector_snapshot->synchronized || selector_snapshot->candidates.size() != 1U ||
        require("selector diagnostic", selector_diagnostic) != 0) {
        return 1;
    }

    result<catalog_client> catalog_domain_result = catalog_client::open(root);
    if (require("catalog client", catalog_domain_result) != 0) {
        return 1;
    }
    catalog_client catalog_domain = std::move(*catalog_domain_result);
    result<catalog_publisher> publisher_result = catalog_publisher::create(catalog_domain);
    if (require("catalog publisher", publisher_result) != 0) {
        return 1;
    }
    catalog_publisher publisher = std::move(*publisher_result);
    const catalog_path path("routing", "primary");
    proxy_data catalog_data;
    catalog_data.power = 11;
    catalog_data.ready = true;
    result<std::uint64_t> invalid_kind = publisher.replace(path, static_cast<catalog_kind>(255), catalog_data);
    if (invalid_kind || invalid_kind.failure().code() != "invalid") {
        return 1;
    }
    result<std::uint64_t> catalog_revision = publisher.replace(path, catalog_kind::map, catalog_data);
    if (require("catalog replace", catalog_revision) != 0 || *catalog_revision == 0U) {
        return 1;
    }
    catalog_data.power = 12;
    result<std::uint64_t> patched_revision = publisher.patch(path, *catalog_revision, catalog_data);
    if (require("catalog patch", patched_revision) != 0 || *patched_revision <= *catalog_revision) {
        return 1;
    }
    catalog_subscription subscription;
    subscription.parts.push_back("routing");
    result<catalog_subscriber> subscriber_result = catalog_subscriber::create(catalog_domain, subscription);
    if (require("catalog subscriber", subscriber_result) != 0) {
        return 1;
    }
    catalog_subscriber subscriber = std::move(*subscriber_result);
    result<optional<catalog_entry>> found = subscriber.find(path);
    if (require("catalog find", found) != 0 || !*found) {
        return 1;
    }
    catalog_entry entry = std::move(**found);
    result<catalog_snapshot<proxy_data>> catalog_loaded = entry.load<proxy_data>();
    result<diagnostic> catalog_diagnostic = subscriber.try_error();
    if (require("catalog load", catalog_loaded) != 0 || !catalog_loaded->synchronized || !catalog_loaded->value || catalog_loaded->value->power != 12 ||
        require("catalog diagnostic", catalog_diagnostic) != 0) {
        return 1;
    }
    result<std::uint64_t> catalog_erased = publisher.erase(path);
    if (require("catalog erase", catalog_erased) != 0) {
        return 1;
    }

    if (!subscriber.close() || !selector_value.close() || !registration_value.close() || !catalog_domain.close() || !registration_domain.close()) {
        return 1;
    }
    std::vector<std::string> hash_fields;
    hash_fields.push_back("power");
    result<std::size_t> hash_erased = root.hash_erase(hash, hash_fields);
    if (require("hash erase", hash_erased) != 0 || *hash_erased != 1U) {
        return 1;
    }
    erase_key(root, key);
    erase_key(root, hash);
    erase_key(root, "verdandi:config:" + registration_zone);
    erase_key(root, "verdandi:registry:" + registration_zone + ":Proxy");
    erase_key(root, "verdandi:catalog:" + catalog_zone + ":@meta");
    erase_key(root, "verdandi:catalog:" + catalog_zone + ":@live");
    erase_key(root, "verdandi:catalog:" + catalog_zone + ":@deleted");
    erase_key(root, "verdandi:catalog:" + catalog_zone + ":@deleted_time");
    erase_key(root, "verdandi:catalog:" + catalog_zone + ":routing:primary");
    erase_key(root, "verdandi:catalog:" + catalog_zone + ":routing:primary:@field_revisions");
    return root.close() ? 0 : 1;
}
