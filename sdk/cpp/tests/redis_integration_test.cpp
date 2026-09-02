#include "verdandi/catalog/catalog.hpp"
#include "verdandi/client.hpp"
#include "verdandi/registration/registration.hpp"
#include "verdandi/registration/selector.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct test_attr {
    std::string region;
};

struct test_data {
    std::int64_t power{};
    bool ready{};
};

} // namespace

VERDANDI_SCHEMA(test_attr, VERDANDI_FIELD(test_attr, region));
VERDANDI_SCHEMA(test_data, VERDANDI_FIELD(test_data, power), VERDANDI_FIELD(test_data, ready));

namespace {

int fail(const std::string_view operation, const verdandi::error& error) {
    std::cerr << operation << ": " << error.message() << '\n';
    return 1;
}

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

std::vector<std::string> split_addresses(const std::string_view source) {
    std::vector<std::string> output;
    std::size_t begin{};
    while (begin <= source.size()) {
        const auto end = source.find(',', begin);
        const auto value = source.substr(begin, end == std::string_view::npos ? source.size() - begin : end - begin);
        if (value.empty()) {
            return {};
        }
        output.emplace_back(value);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return output;
}

std::string unique_zone(std::string prefix) {
    auto value = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    for (std::size_t index = 0; index < 12; ++index) {
        prefix.push_back(static_cast<char>('a' + value % 26));
        value = value / 26 + 17;
    }
    return prefix;
}

struct checkpoint_files {
    std::filesystem::path path;

    ~checkpoint_files() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }
};

int test_root(const verdandi::client& client) {
    constexpr std::string_view key = "verdandi:test:cpp:root";
    constexpr std::string_view hash = "verdandi:test:cpp:hash";
    static_cast<void>(client.key().erase(key));
    static_cast<void>(client.key().erase(hash));

    if (auto status = client.key().set<std::string>(key, "value", std::chrono::seconds{30}); !status) {
        return fail("SET", status.error());
    }
    const auto loaded = client.key().get<std::string>(key);
    if (!loaded || !*loaded || **loaded != "value") {
        std::cerr << "GET did not return the stored value\n";
        return 1;
    }

    verdandi::fields source;
    source.emplace("first", verdandi::bytes{std::byte{'a'}});
    source.emplace("second", verdandi::bytes{std::byte{'b'}});
    if (auto status = client.hash().store(hash, source); !status) {
        return fail("HSET", status.error());
    }
    const auto fields = client.hash().load(hash);
    if (!fields || *fields != source) {
        std::cerr << "HGETALL did not return the stored fields\n";
        return 1;
    }

    static_cast<void>(client.key().erase(key));
    static_cast<void>(client.key().erase(hash));
    return 0;
}

int test_registration(const verdandi::client& transport, const std::string_view zone) {
    verdandi::registration_configuration configuration;
    configuration.zone = zone;
    configuration.selector.sync_timeout = std::chrono::seconds{2};
    auto owner = verdandi::registration::client::open(transport, configuration);
    if (!owner) {
        return fail("registration.open", owner.error());
    }

    verdandi::registration::options options;
    options.type = "Proxy";
    options.ttl = std::chrono::seconds{15};
    options.renew_interval = std::chrono::seconds{5};
    options.version = 1;
    auto handle = verdandi::registration::registration<test_attr, test_data>::create(*owner, options);
    if (!handle) {
        return fail("registration.create", handle.error());
    }
    if (auto status = handle->publish(test_attr{"cn-east"}, test_data{1, true}); !status) {
        return fail("registration.publish", status.error());
    }
    auto selector = verdandi::registration::selector<test_attr, test_data>::create(*owner, {"Proxy"});
    if (!selector) {
        return fail("selector.create", selector.error());
    }
    const auto initial = (*selector)->snapshot();
    if (!initial || initial->candidates.size() != 1 || initial->candidates.front().data.power != 1) {
        std::cerr << "selector initial synchronization failed\n";
        return 1;
    }
    if (auto status = handle->update(test_data{2, true}); !status) {
        return fail("registration.update", status.error());
    }
    if (handle->revision() != 2 || handle->timestamp() == 0) {
        std::cerr << "registration counters were not advanced\n";
        return 1;
    }
    bool updated{false};
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        auto selected = (*selector)->one([](auto& values) -> verdandi::result<std::optional<verdandi::registration::choice>> {
            auto value = values.get(0);
            if (!value || value->meta().revision != 2 || value->data().power != 2) {
                return std::optional<verdandi::registration::choice>{};
            }
            auto status = values.mutate(value->identity(), [](test_data& data) { ++data.power; });
            if (!status) {
                return std::unexpected(status.error());
            }
            return std::optional<verdandi::registration::choice>(value->identity());
        });
        if (!selected) {
            return fail("selector.one", selected.error());
        }
        if (*selected) {
            updated = (*selected)->data.power == 3;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!updated) {
        std::cerr << "selector live update or local overlay failed\n";
        return 1;
    }
    auto thrown =
        (*selector)->one([](auto&) -> verdandi::result<std::optional<verdandi::registration::choice>> { throw std::runtime_error("policy failure"); });
    if (thrown || thrown.error().category() != verdandi::code::contract || thrown.error().field() != "callback") {
        std::cerr << "selector callback exception escaped\n";
        return 1;
    }
    auto duplicate = (*selector)->any([](auto& values) -> verdandi::result<std::vector<verdandi::registration::choice>> {
        auto value = values.get(0);
        if (!value) {
            return std::vector<verdandi::registration::choice>{};
        }
        const auto choice = value->identity();
        return std::vector{choice, choice};
    });
    if (duplicate || duplicate.error().category() != verdandi::code::contract || duplicate.error().field() != "candidate") {
        std::cerr << "selector duplicate choice was accepted\n";
        return 1;
    }
    if (auto status = handle->renew(); !status) {
        return fail("registration.renew", status.error());
    }
    if (auto status = handle->close(); !status) {
        return fail("registration.close", status.error());
    }
    if (auto status = (*selector)->close(); !status) {
        return fail("selector.close", status.error());
    }
    if (auto status = owner->close(); !status) {
        return fail("registration client close", status.error());
    }
    return 0;
}

int test_catalog(const verdandi::client& transport, const std::string_view zone, const bool exact_subscription) {
    const auto checkpoint = std::filesystem::temp_directory_path() /
                            ("verdandi-cpp-catalog-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite3");
    const checkpoint_files cleanup{checkpoint};
    verdandi::catalog_configuration configuration;
    configuration.zone = zone;
    configuration.sync_timeout = std::chrono::seconds{2};
    configuration.local_store_path = checkpoint;
    auto owner = verdandi::catalog::client::open(transport, configuration);
    if (!owner) {
        return fail("catalog.open", owner.error());
    }
    auto publisher = verdandi::catalog::publisher::create(*owner);
    if (!publisher) {
        return fail("catalog.publisher", publisher.error());
    }
    auto target = verdandi::catalog::path::create("routing", "primary");
    if (!target) {
        return fail("catalog.path", target.error());
    }
    auto replaced = publisher->replace(*target, verdandi::catalog::kind::map, test_data{10, true});
    if (!replaced || replaced->revision == 0) {
        return replaced ? 1 : fail("catalog.replace", replaced.error());
    }

    verdandi::catalog::subscription scope;
    if (exact_subscription) {
        scope.paths.push_back(*target);
    } else {
        scope.parts.emplace_back("routing");
    }
    auto subscriber = verdandi::catalog::subscriber::create(*owner, std::move(scope));
    if (!subscriber) {
        return fail("catalog.subscriber", subscriber.error());
    }
    auto entry = (*subscriber)->find(*target);
    auto initial = entry ? entry->load<test_data>()
                         : verdandi::result<verdandi::catalog::snapshot<test_data>>(std::unexpected(verdandi::error(verdandi::code::unavailable)));
    if (!initial || initial->state != verdandi::catalog::status::present || !initial->value || initial->value->power != 10 ||
        initial->revision != replaced->revision) {
        std::cerr << "catalog initial synchronization failed\n";
        return 1;
    }

    auto encoded_power = verdandi::field_codec<std::int64_t>::encode(11);
    if (!encoded_power) {
        return fail("catalog.encode", encoded_power.error());
    }
    verdandi::catalog::patch change;
    change.base_revision = replaced->revision;
    change.set.emplace("power", std::move(*encoded_power));
    auto patched = publisher->apply(*target, std::move(change));
    if (!patched || patched->revision <= replaced->revision) {
        return patched ? 1 : fail("catalog.patch", patched.error());
    }
    bool observed_patch{false};
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        auto current = entry->load<test_data>();
        if (!current) {
            return fail("catalog.load", current.error());
        }
        if (current->revision == patched->revision && current->state == verdandi::catalog::status::present && current->value && current->value->power == 11) {
            observed_patch = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!observed_patch) {
        std::cerr << "catalog live patch was not observed\n";
        return 1;
    }

    if (auto status = (*subscriber)->close(); !status) {
        return fail("catalog subscriber close", status.error());
    }
    if (auto status = owner->close(); !status) {
        return fail("catalog client close", status.error());
    }

    auto recovered_owner = verdandi::catalog::client::open(transport, configuration);
    if (!recovered_owner) {
        return fail("catalog reopen", recovered_owner.error());
    }
    auto recovered_publisher = verdandi::catalog::publisher::create(*recovered_owner);
    if (!recovered_publisher) {
        return fail("catalog recovered publisher", recovered_publisher.error());
    }
    verdandi::catalog::subscription recovered_scope;
    if (exact_subscription) {
        recovered_scope.paths.push_back(*target);
    } else {
        recovered_scope.parts.emplace_back("routing");
    }
    auto recovered_subscriber = verdandi::catalog::subscriber::create(*recovered_owner, std::move(recovered_scope));
    if (!recovered_subscriber) {
        return fail("catalog recovered subscriber", recovered_subscriber.error());
    }
    auto recovered_entry = (*recovered_subscriber)->find(*target);
    auto recovered = recovered_entry ? recovered_entry->load<test_data>()
                                     : verdandi::result<verdandi::catalog::snapshot<test_data>>(std::unexpected(verdandi::error(verdandi::code::unavailable)));
    if (!recovered || recovered->revision != patched->revision || recovered->state != verdandi::catalog::status::present || !recovered->value ||
        recovered->value->power != 11) {
        std::cerr << "catalog checkpoint recovery failed\n";
        return 1;
    }

    auto erased = recovered_publisher->erase(*target);
    if (!erased || erased->revision <= patched->revision) {
        return erased ? 1 : fail("catalog.erase", erased.error());
    }
    bool observed_delete{false};
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        if (recovered_entry->revision() == erased->revision && recovered_entry->state() == verdandi::catalog::status::deleted) {
            observed_delete = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!observed_delete) {
        std::cerr << "catalog live delete was not observed\n";
        return 1;
    }
    if (auto status = (*recovered_subscriber)->close(); !status) {
        return fail("catalog subscriber close", status.error());
    }
    if (auto status = recovered_owner->close(); !status) {
        return fail("catalog client close", status.error());
    }
    return 0;
}

void cleanup_test_keys(const verdandi::client& client, const std::string_view registration_zone, const std::string_view catalog_zone) {
    const std::vector<std::string> keys{
        "verdandi:config:" + std::string(registration_zone),
        "verdandi:registry:" + std::string(registration_zone) + ":Proxy",
        "verdandi:catalog:" + std::string(catalog_zone) + ":@meta",
        "verdandi:catalog:" + std::string(catalog_zone) + ":@live",
        "verdandi:catalog:" + std::string(catalog_zone) + ":@deleted",
        "verdandi:catalog:" + std::string(catalog_zone) + ":@deleted_time",
        "verdandi:catalog:" + std::string(catalog_zone) + ":routing:primary",
        "verdandi:catalog:" + std::string(catalog_zone) + ":routing:primary:@field_revisions",
    };
    for (const auto& key : keys) {
        static_cast<void>(client.key().erase(key));
    }
}

} // namespace

int main() {
    const auto standalone = environment("VERDANDI_REDIS_ADDRESS");
    const auto sentinel = environment("VERDANDI_SENTINEL_ADDRS");
    if (standalone.empty() && sentinel.empty()) {
        return 77;
    }
    verdandi::redis_configuration configuration;
    if (!sentinel.empty()) {
        configuration.mode = verdandi::redis_mode::sentinel;
        configuration.addresses = split_addresses(sentinel);
        configuration.master_name = environment("VERDANDI_SENTINEL_MASTER");
        configuration.auth.username = environment("VERDANDI_REDIS_USERNAME");
        configuration.auth.password = environment("VERDANDI_REDIS_PASSWORD");
        configuration.sentinel_auth.username = environment("VERDANDI_SENTINEL_USERNAME");
        configuration.sentinel_auth.password = environment("VERDANDI_SENTINEL_PASSWORD");
        configuration.timeout = std::chrono::seconds{3};
    } else {
        configuration.addresses = {standalone};
    }
    if (const auto ca_file = environment("VERDANDI_TLS_CA_FILE"); !ca_file.empty()) {
        configuration.tls.enabled = true;
        configuration.tls.system_roots = false;
        configuration.tls.server_name = environment("VERDANDI_TLS_SERVER_NAME");
        configuration.tls.ca_file = ca_file;
    }
    auto client = verdandi::client::open(configuration);
    if (!client) {
        return fail("client.open", client.error());
    }
    const auto registration_zone = unique_zone("CppRegistration");
    const auto catalog_zone = unique_zone("CppCatalog");
    int result{};
    if (const auto status = test_root(*client); status != 0) {
        result = status;
    }
    if (result == 0) {
        result = test_registration(*client, registration_zone);
    }
    if (result == 0) {
        result = test_catalog(*client, catalog_zone, configuration.mode == verdandi::redis_mode::sentinel);
    }
    cleanup_test_keys(*client, registration_zone, catalog_zone);
    if (auto status = client->close(); !status) {
        return result == 0 ? fail("client.close", status.error()) : result;
    }
    return result;
}
