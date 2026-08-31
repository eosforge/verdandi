#include "verdandi/client.hpp"

#include "internal/driver.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace verdandi {

namespace {

[[nodiscard]] result<std::shared_ptr<detail::driver>> require_driver(const std::shared_ptr<detail::driver>& value) {
    if (!value || !value->open()) {
        return std::unexpected(error(code::closed));
    }
    return value;
}

[[nodiscard]] result<bool> boolean_integer(const detail::response& value, const std::string_view field) {
    auto text = value.text();
    if (!text) {
        return std::unexpected(text.error());
    }
    auto number = detail::parse_unsigned(*text, field, true);
    if (!number || *number > 1) {
        return std::unexpected(error(code::corrupt, std::string(field)));
    }
    return *number == 1;
}

} // namespace

client::client(std::shared_ptr<detail::driver> driver) noexcept : driver_(std::move(driver)) {}

result<client> client::open(const redis_configuration& configuration) {
    auto opened = detail::driver::open(configuration);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    return client(std::move(*opened));
}

key_commands client::key() const noexcept {
    return key_commands(driver_);
}

hash_commands client::hash() const noexcept {
    return hash_commands(driver_);
}

result<void> client::ping() const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    auto response = (*driver)->execute(detail::command("PING"));
    if (!response) {
        return std::unexpected(response.error());
    }
    auto value = response->text();
    if (!value || *value != "PONG") {
        return std::unexpected(error(code::corrupt, "PING"));
    }
    return {};
}

result<void> client::close() const {
    if (!driver_) {
        return {};
    }
    return driver_->close();
}

bool client::open() const noexcept {
    return driver_ && driver_->open();
}

std::chrono::milliseconds client::timeout() const noexcept {
    return driver_ ? driver_->timeout() : std::chrono::milliseconds::zero();
}

key_commands::key_commands(std::shared_ptr<detail::driver> driver) noexcept : driver_(std::move(driver)) {}

result<std::optional<bytes>> key_commands::load(const std::string_view key) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty()) {
        return std::unexpected(error(code::invalid, "key"));
    }
    detail::command command("GET");
    command.add(key);
    auto response = (*driver)->execute(command);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->type == detail::response::kind::null) {
        return std::optional<bytes>{};
    }
    auto value = response->text();
    if (!value) {
        return std::unexpected(value.error());
    }
    const auto* first = reinterpret_cast<const std::byte*>(value->data());
    return std::optional<bytes>(bytes(first, first + value->size()));
}

result<void> key_commands::store(const std::string_view key, const std::span<const std::byte> value) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty()) {
        return std::unexpected(error(code::invalid, "key"));
    }
    detail::command command("SET");
    command.add(key).add(value);
    auto response = (*driver)->execute(command, true);
    if (!response) {
        return std::unexpected(response.error());
    }
    auto result = response->text();
    if (!result || *result != "OK") {
        return std::unexpected(error(code::corrupt, "SET"));
    }
    return {};
}

result<void> key_commands::store(const std::string_view key, const std::span<const std::byte> value, const std::chrono::milliseconds ttl) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty() || ttl <= std::chrono::milliseconds::zero()) {
        return std::unexpected(error(code::invalid, key.empty() ? "key" : "ttl"));
    }
    detail::command command("SET");
    command.add(key).add(value).add("PX").add(static_cast<std::uint64_t>(ttl.count()));
    auto response = (*driver)->execute(command, true);
    if (!response) {
        return std::unexpected(response.error());
    }
    auto result = response->text();
    if (!result || *result != "OK") {
        return std::unexpected(error(code::corrupt, "SET"));
    }
    return {};
}

result<bool> key_commands::erase(const std::string_view key) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty()) {
        return std::unexpected(error(code::invalid, "key"));
    }
    detail::command command("DEL");
    command.add(key);
    auto response = (*driver)->execute(command, true);
    if (!response) {
        return std::unexpected(response.error());
    }
    return boolean_integer(*response, "DEL");
}

result<bool> key_commands::contains(const std::string_view key) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty()) {
        return std::unexpected(error(code::invalid, "key"));
    }
    detail::command command("EXISTS");
    command.add(key);
    auto response = (*driver)->execute(command);
    if (!response) {
        return std::unexpected(response.error());
    }
    return boolean_integer(*response, "EXISTS");
}

result<bool> key_commands::expire(const std::string_view key, const std::chrono::milliseconds ttl) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty() || ttl <= std::chrono::milliseconds::zero()) {
        return std::unexpected(error(code::invalid, key.empty() ? "key" : "ttl"));
    }
    detail::command command("PEXPIRE");
    command.add(key).add(static_cast<std::uint64_t>(ttl.count()));
    auto response = (*driver)->execute(command, true);
    if (!response) {
        return std::unexpected(response.error());
    }
    return boolean_integer(*response, "PEXPIRE");
}

hash_commands::hash_commands(std::shared_ptr<detail::driver> driver) noexcept : driver_(std::move(driver)) {}

result<fields> hash_commands::load(const std::string_view key) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty()) {
        return std::unexpected(error(code::invalid, "key"));
    }
    detail::command command("HGETALL");
    command.add(key);
    auto response = (*driver)->execute(command);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->type != detail::response::kind::map && response->type != detail::response::kind::array) {
        return std::unexpected(error(code::corrupt, "HGETALL"));
    }
    if (response->children.size() % 2 != 0) {
        return std::unexpected(error(code::corrupt, "HGETALL"));
    }
    fields output;
    for (std::size_t index = 0; index < response->children.size(); index += 2) {
        auto name = response->children[index].text();
        auto value = response->children[index + 1].text();
        if (!name || !value || output.contains(*name)) {
            return std::unexpected(error(code::corrupt, "HGETALL"));
        }
        const auto* first = reinterpret_cast<const std::byte*>(value->data());
        output.emplace(std::string(*name), bytes(first, first + value->size()));
    }
    return output;
}

result<void> hash_commands::store(const std::string_view key, const fields& value) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty() || value.empty()) {
        return std::unexpected(error(code::invalid, key.empty() ? "key" : "fields"));
    }
    detail::command command("HSET");
    command.add(key);
    for (const auto& [name, bytes] : value) {
        if (name.empty()) {
            return std::unexpected(error(code::invalid, "field"));
        }
        command.add(name).add(bytes);
    }
    auto response = (*driver)->execute(command, true);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (!response->text()) {
        return std::unexpected(error(code::corrupt, "HSET"));
    }
    return {};
}

result<std::size_t> hash_commands::erase(const std::string_view key, const std::span<const std::string_view> names) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty() || names.empty() || std::ranges::any_of(names, [](const std::string_view name) { return name.empty(); })) {
        return std::unexpected(error(code::invalid, key.empty() ? "key" : "fields"));
    }
    detail::command command("HDEL");
    command.add(key);
    for (const auto name : names) {
        command.add(name);
    }
    auto response = (*driver)->execute(command, true);
    if (!response) {
        return std::unexpected(response.error());
    }
    auto text = response->text();
    if (!text) {
        return std::unexpected(text.error());
    }
    auto count = detail::parse_unsigned(*text, "HDEL", true);
    if (!count || *count > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(error(code::corrupt, "HDEL"));
    }
    return static_cast<std::size_t>(*count);
}

result<bool> hash_commands::contains(const std::string_view key, const std::string_view name) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty() || name.empty()) {
        return std::unexpected(error(code::invalid, key.empty() ? "key" : "field"));
    }
    detail::command command("HEXISTS");
    command.add(key).add(name);
    auto response = (*driver)->execute(command);
    if (!response) {
        return std::unexpected(response.error());
    }
    return boolean_integer(*response, "HEXISTS");
}

result<std::size_t> hash_commands::size(const std::string_view key) const {
    auto driver = require_driver(driver_);
    if (!driver) {
        return std::unexpected(driver.error());
    }
    if (key.empty()) {
        return std::unexpected(error(code::invalid, "key"));
    }
    detail::command command("HLEN");
    command.add(key);
    auto response = (*driver)->execute(command);
    if (!response) {
        return std::unexpected(response.error());
    }
    auto text = response->text();
    if (!text) {
        return std::unexpected(text.error());
    }
    auto count = detail::parse_unsigned(*text, "HLEN", true);
    if (!count || *count > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(error(code::corrupt, "HLEN"));
    }
    return static_cast<std::size_t>(*count);
}

std::shared_ptr<detail::driver> detail::driver_access::get(const client& value) noexcept {
    return value.driver_;
}

} // namespace verdandi
