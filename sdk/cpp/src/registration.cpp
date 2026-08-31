#include "internal/registration.hpp"
#include "internal/selector.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace verdandi::registration {

namespace {

[[nodiscard]] std::string binary_string(const bytes& value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::size_t decimal_digits(std::uint64_t value) noexcept {
    std::size_t output{1};
    while (value >= 10) {
        value /= 10;
        ++output;
    }
    return output;
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index{};
    while (index < value.size()) {
        const auto lead = static_cast<unsigned char>(value[index++]);
        if (lead <= 0x7fU) {
            continue;
        }
        std::size_t continuation{};
        std::uint32_t codepoint{};
        if ((lead & 0xe0U) == 0xc0U) {
            continuation = 1;
            codepoint = lead & 0x1fU;
        } else if ((lead & 0xf0U) == 0xe0U) {
            continuation = 2;
            codepoint = lead & 0x0fU;
        } else if ((lead & 0xf8U) == 0xf0U) {
            continuation = 3;
            codepoint = lead & 0x07U;
        } else {
            return false;
        }
        if (continuation > value.size() - index) {
            return false;
        }
        for (std::size_t count = 0; count < continuation; ++count) {
            const auto next = static_cast<unsigned char>(value[index++]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        const auto minimum = continuation == 1 ? 0x80U : continuation == 2 ? 0x800U : 0x10000U;
        if (codepoint < minimum || codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] result<std::uint64_t> positive_integer(const verdandi::detail::response& value, const std::string_view field) {
    auto text = value.text();
    if (!text) {
        return std::unexpected(error(code::corrupt, std::string(field)));
    }
    return verdandi::detail::parse_unsigned(*text, field);
}

[[nodiscard]] std::chrono::milliseconds jittered(const std::chrono::milliseconds value, const std::uint8_t percent) {
    const auto span = value.count() * static_cast<std::int64_t>(percent) / 100;
    if (span <= 0) {
        return value;
    }
    thread_local std::mt19937_64 generator(std::random_device{}());
    std::uniform_int_distribution<std::int64_t> distribution(-span, span);
    return std::chrono::milliseconds(value.count() + distribution(generator));
}

[[nodiscard]] std::string config_key(const std::string_view zone) {
    return "verdandi:config:" + std::string(zone);
}

[[nodiscard]] std::vector<std::string> registration_arguments(const std::string_view uuid, const std::uint64_t revision, const std::uint64_t ttl,
                                                              const std::uint64_t version, const fields& attr, const fields& data) {
    std::vector<std::string> output;
    output.reserve(4 + (attr.size() + data.size()) * 2);
    output.emplace_back(uuid);
    output.push_back(std::to_string(revision));
    output.push_back(std::to_string(ttl));
    output.push_back(std::to_string(version));
    for (const auto& [name, value] : attr) {
        output.push_back('.' + name);
        output.push_back(binary_string(value));
    }
    for (const auto& [name, value] : data) {
        output.push_back(name);
        output.push_back(binary_string(value));
    }
    return output;
}

[[nodiscard]] std::vector<std::string> update_arguments(const std::string_view uuid, const std::uint64_t revision, const std::optional<std::uint64_t> version,
                                                        const fields& data) {
    std::vector<std::string> output;
    output.reserve(3 + data.size() * 2);
    output.emplace_back(uuid);
    output.push_back(std::to_string(revision));
    output.push_back(version ? std::to_string(*version) : std::string{});
    for (const auto& [name, value] : data) {
        output.push_back(name);
        output.push_back(binary_string(value));
    }
    return output;
}

[[nodiscard]] bool same_shape(const fields& left, const fields& right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return std::ranges::equal(
        left, right, {}, [](const auto& value) -> const std::string& { return value.first; },
        [](const auto& value) -> const std::string& { return value.first; });
}

[[nodiscard]] bool same_bytes(const bytes& left, const bytes& right) noexcept {
    return left == right;
}

[[nodiscard]] bool uncertain(const error& value) noexcept {
    return value.category() == code::ambiguous || value.category() == code::corrupt;
}

[[nodiscard]] bool recoverable_missing(const error& value) noexcept {
    return value.category() == code::missing || value.category() == code::transition;
}

} // namespace

namespace detail {

namespace {

constexpr std::array<std::string_view, 8> policy_names = {
    "protocol",
    "registration_attr_max_fields",
    "registration_data_max_fields",
    "registration_max_field_name_bytes",
    "registration_attr_max_field_value_bytes",
    "registration_data_max_field_value_bytes",
    "registration_max_bytes",
    "configuration_refresh_ms",
};

[[nodiscard]] policy configured_policy(const registration_configuration& configuration) {
    return {
        configuration.policy.attr_max_fields,      configuration.policy.data_max_fields,      configuration.policy.field_name_max_bytes,
        configuration.policy.attr_value_max_bytes, configuration.policy.data_value_max_bytes, configuration.policy.record_max_bytes,
        configuration.policy.refresh_interval,
    };
}

[[nodiscard]] std::array<std::string, 8> policy_values(const policy& value) {
    return {
        "v1",
        std::to_string(value.attr_max_fields),
        std::to_string(value.data_max_fields),
        std::to_string(value.field_name_max_bytes),
        std::to_string(value.attr_value_max_bytes),
        std::to_string(value.data_value_max_bytes),
        std::to_string(value.record_max_bytes),
        std::to_string(value.refresh_interval.count()),
    };
}

[[nodiscard]] result<std::uint64_t> parse_policy_integer(const verdandi::detail::response& value, const std::string_view field) {
    return positive_integer(value, field);
}

[[nodiscard]] result<registration_reply> parse_registration_reply(const verdandi::detail::response& source) {
    if (source.type != verdandi::detail::response::kind::array || source.children.size() < 2 || source.children.size() % 2 != 0) {
        return std::unexpected(error(code::corrupt, "reply"));
    }
    std::unordered_map<std::string_view, const verdandi::detail::response*> values;
    values.reserve(source.children.size() / 2);
    for (std::size_t index = 0; index < source.children.size(); index += 2) {
        auto name = source.children[index].text();
        if (!name || !values.emplace(*name, &source.children[index + 1]).second) {
            return std::unexpected(error(code::corrupt, "reply"));
        }
    }

    const auto result = values.find("&result");
    if (result == values.end()) {
        return std::unexpected(error(code::corrupt, "&result"));
    }
    auto result_text = result->second->text();
    if (!result_text) {
        return std::unexpected(error(code::corrupt, "&result"));
    }
    if (*result_text == "error") {
        const auto status = values.find("&status");
        if (status == values.end()) {
            return std::unexpected(error(code::corrupt, "&status"));
        }
        auto status_text = status->second->text();
        if (!status_text) {
            return std::unexpected(error(code::corrupt, "&status"));
        }
        auto category = parse_code(*status_text);
        if (!category || *category == code::unavailable || *category == code::deadline || *category == code::ambiguous || *category == code::closed) {
            return std::unexpected(error(code::corrupt, "&status"));
        }
        std::string field;
        if (const auto iterator = values.find("&field"); iterator != values.end()) {
            auto field_text = iterator->second->text();
            if (!field_text) {
                return std::unexpected(error(code::corrupt, "&field"));
            }
            field.assign(*field_text);
        }
        error output(*category, std::move(field));
        if (const auto iterator = values.find("@revision"); iterator != values.end()) {
            auto revision = positive_integer(*iterator->second, "@revision");
            if (!revision) {
                return std::unexpected(revision.error());
            }
            output = output.with_revision(*revision);
        }
        return std::unexpected(std::move(output));
    }
    if (*result_text != "ok") {
        return std::unexpected(error(code::corrupt, "&result"));
    }

    registration_reply output;
    if (const auto iterator = values.find("@revision"); iterator != values.end()) {
        auto revision = positive_integer(*iterator->second, "@revision");
        if (!revision) {
            return std::unexpected(revision.error());
        }
        output.revision = *revision;
    }
    if (const auto iterator = values.find("@timestamp"); iterator != values.end()) {
        auto timestamp = positive_integer(*iterator->second, "@timestamp");
        if (!timestamp) {
            return std::unexpected(timestamp.error());
        }
        output.timestamp = *timestamp;
    }
    return output;
}

[[nodiscard]] result<std::string> new_uuid() {
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        return std::unexpected(error(code::unavailable, "uuid"));
    }
    constexpr std::string_view hexadecimal = "0123456789abcdef";
    std::string output(32, '0');
    for (std::size_t index = 0; index < random.size(); ++index) {
        output[index * 2] = hexadecimal[random[index] >> 4U];
        output[index * 2 + 1] = hexadecimal[random[index] & 0x0fU];
    }
    return output;
}

} // namespace

bool valid_type(const std::string_view value) noexcept {
    const auto letter = [](const char character) { return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z'); };
    if (value.empty() || value.size() > 64 || !letter(value.front())) {
        return false;
    }
    return std::ranges::all_of(value.substr(1), [&](const char character) {
        return letter(character) || (character >= '0' && character <= '9') || character == '_' || character == '.' || character == '-';
    });
}

result<void> validate_record(const std::string_view uuid, const std::uint64_t revision, const std::uint64_t ttl, const std::uint64_t version,
                             const fields& attr, const fields& data, const policy& limits) {
    if (revision == 0 || revision > safe_integer_max) {
        return std::unexpected(error(code::invalid, "@revision"));
    }
    if (ttl == 0 || ttl > hash_field_deadline_max) {
        return std::unexpected(error(code::invalid, "@ttl"));
    }
    if (version == 0 || version > safe_integer_max) {
        return std::unexpected(error(code::invalid, "@version"));
    }
    if (attr.size() > limits.attr_max_fields) {
        return std::unexpected(error(code::capacity, "attr"));
    }
    if (data.size() > limits.data_max_fields) {
        return std::unexpected(error(code::capacity, "data"));
    }

    const auto validate = [&](const fields& values, const std::size_t value_limit) -> result<void> {
        for (const auto& [name, value] : values) {
            if (name.empty() || name.size() > limits.field_name_max_bytes || !valid_utf8(name) || name.front() == '&' || name.front() == '@' ||
                name.front() == '.') {
                return std::unexpected(error(code::invalid, name));
            }
            if (value.size() > value_limit) {
                return std::unexpected(error(code::capacity, name));
            }
        }
        return {};
    };
    if (auto status = validate(attr, limits.attr_value_max_bytes); !status) {
        return status;
    }
    if (auto status = validate(data, limits.data_value_max_bytes); !status) {
        return status;
    }

    std::size_t size = 5 + uuid.size() + 9 + decimal_digits(revision) + 10 + 16 + 4 + decimal_digits(ttl) + 8 + decimal_digits(version);
    for (const auto& [name, value] : attr) {
        size += 1 + name.size() + value.size();
    }
    for (const auto& [name, value] : data) {
        size += name.size() + value.size();
    }
    if (size > limits.record_max_bytes) {
        return std::unexpected(error(code::capacity, "registration"));
    }
    return {};
}

client_core::client_core(std::shared_ptr<verdandi::detail::driver> transport, registration_configuration configuration)
    : transport_(std::move(transport)), configuration_(std::move(configuration)),
      register_script_(verdandi::detail::registration_script(verdandi::detail::registration_operation::register_value)),
      update_script_(verdandi::detail::registration_script(verdandi::detail::registration_operation::update)),
      renew_script_(verdandi::detail::registration_script(verdandi::detail::registration_operation::renew)),
      unregister_script_(verdandi::detail::registration_script(verdandi::detail::registration_operation::unregister)) {}

client_core::~client_core() {
    static_cast<void>(close());
}

result<std::shared_ptr<client_core>> client_core::open(std::shared_ptr<verdandi::detail::driver> transport, const registration_configuration& configuration) {
    if (!transport || !transport->open()) {
        return std::unexpected(error(code::closed, "redis"));
    }
    if (const auto status = configuration.check(); !status) {
        return std::unexpected(status.error());
    }
    auto output = std::shared_ptr<client_core>(new client_core(std::move(transport), configuration));
    if (auto status = output->bootstrap(); !status) {
        return std::unexpected(status.error());
    }
    return output;
}

result<void> client_core::bootstrap() {
    verdandi::detail::command info("INFO");
    info.add("SERVER");
    auto server = transport_->execute(info);
    if (!server) {
        return std::unexpected(server.error());
    }
    auto text = server->text();
    const auto marker = text ? text->find("redis_version:") : std::string_view::npos;
    if (!text || marker == std::string_view::npos) {
        return std::unexpected(error(code::corrupt, "redis_version"));
    }
    const auto version = text->substr(marker + 14);
    std::uint32_t major{};
    const auto [end, status] = std::from_chars(version.data(), version.data() + version.size(), major);
    if (status != std::errc{} || end == version.data() || major < 8) {
        return std::unexpected(error(code::protocol, "redis_version"));
    }
    auto loaded = read_policy(true);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    policy_.store(std::move(*loaded), std::memory_order_release);
    return {};
}

result<std::shared_ptr<const policy>> client_core::read_policy(const bool install_defaults) {
    const auto key = config_key(configuration_.zone);
    verdandi::detail::command read("HMGET");
    read.add(key);
    for (const auto name : policy_names) {
        read.add(name);
    }
    auto response = transport_->execute(read);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->type != verdandi::detail::response::kind::array || response->children.size() != policy_names.size()) {
        return std::unexpected(error(code::corrupt, "verdandi:config"));
    }

    bool missing = std::ranges::any_of(response->children, [](const auto& value) { return value.type == verdandi::detail::response::kind::null; });
    if (missing && install_defaults) {
        const auto defaults = policy_values(configured_policy(configuration_));
        std::vector<verdandi::detail::command> writes;
        for (std::size_t index = 0; index < response->children.size(); ++index) {
            if (response->children[index].type != verdandi::detail::response::kind::null) {
                continue;
            }
            verdandi::detail::command write("HSETNX");
            write.add(key).add(policy_names[index]).add(defaults[index]);
            writes.push_back(std::move(write));
        }
        auto written = transport_->execute(writes);
        if (!written) {
            return std::unexpected(written.error());
        }
        return read_policy(false);
    }
    if (missing) {
        return std::unexpected(error(code::missing, "verdandi:config"));
    }

    auto protocol = response->children[0].text();
    if (!protocol || *protocol != "v1") {
        return std::unexpected(error(code::protocol, "protocol"));
    }
    std::array<std::uint64_t, 7> parsed{};
    for (std::size_t index = 0; index < parsed.size(); ++index) {
        auto value = parse_policy_integer(response->children[index + 1], policy_names[index + 1]);
        if (!value) {
            return std::unexpected(value.error());
        }
        parsed[index] = *value;
    }
    auto output = std::make_shared<policy>(policy{
        static_cast<std::size_t>(parsed[0]),
        static_cast<std::size_t>(parsed[1]),
        static_cast<std::size_t>(parsed[2]),
        static_cast<std::size_t>(parsed[3]),
        static_cast<std::size_t>(parsed[4]),
        static_cast<std::size_t>(parsed[5]),
        std::chrono::milliseconds(parsed[6]),
    });
    registration_policy_configuration checked{
        output->attr_max_fields,      output->data_max_fields,  output->field_name_max_bytes, output->attr_value_max_bytes,
        output->data_value_max_bytes, output->record_max_bytes, output->refresh_interval,
    };
    if (auto status = checked.check(); !status) {
        return std::unexpected(error(code::invalid, status.error().field().empty() ? "verdandi:config" : std::string(status.error().field())));
    }
    return std::shared_ptr<const policy>(std::move(output));
}

result<registration_reply> client_core::call(const verdandi::detail::registration_operation operation, const std::string_view type, const std::string_view uuid,
                                             const std::span<const std::string> arguments, const bool mutation) {
    std::array<std::string, 2> keys = {
        "verdandi:registration:" + configuration_.zone + ':' + std::string(type) + ':' + std::string(uuid),
        "verdandi:registry:" + configuration_.zone + ':' + std::string(type),
    };
    verdandi::detail::script* selected{};
    switch (operation) {
    case verdandi::detail::registration_operation::register_value:
        selected = &register_script_;
        break;
    case verdandi::detail::registration_operation::update:
        selected = &update_script_;
        break;
    case verdandi::detail::registration_operation::renew:
        selected = &renew_script_;
        break;
    case verdandi::detail::registration_operation::unregister:
        selected = &unregister_script_;
        break;
    }
    auto response = selected->run(*transport_, keys, arguments, mutation);
    if (!response) {
        return std::unexpected(response.error());
    }
    return parse_registration_reply(*response);
}

void client_core::add(const std::shared_ptr<registration_core>& value) {
    std::lock_guard lock(children_mutex_);
    children_.erase(std::remove_if(children_.begin(), children_.end(), [](const auto& child) { return child.expired(); }), children_.end());
    children_.push_back(value);
}

void client_core::add(const std::shared_ptr<selector_core>& value) {
    std::lock_guard lock(children_mutex_);
    selectors_.erase(std::remove_if(selectors_.begin(), selectors_.end(), [](const auto& child) { return child.expired(); }), selectors_.end());
    selectors_.push_back(value);
}

void client_core::acquire_policy_user() {
    std::lock_guard lock(refresh_mutex_);
    if (refresh_users_ == 0) {
        auto self = shared_from_this();
        refresh_worker_ = std::jthread([self](const std::stop_token& stop) { self->refresh_policy(stop); });
    }
    ++refresh_users_;
}

void client_core::release_policy_user() {
    std::jthread worker;
    {
        std::lock_guard lock(refresh_mutex_);
        if (refresh_users_ == 0) {
            return;
        }
        --refresh_users_;
        if (refresh_users_ == 0 && refresh_worker_.joinable()) {
            refresh_worker_.request_stop();
            refresh_changed_.notify_all();
            worker = std::move(refresh_worker_);
        }
    }
    if (worker.joinable()) {
        worker.join();
    }
}

void client_core::refresh_policy(const std::stop_token& stop) {
    std::unique_lock lock(refresh_mutex_);
    while (!stop.stop_requested() && !closed_.load(std::memory_order_acquire)) {
        const auto current = policy_.load(std::memory_order_acquire);
        const auto interval =
            jittered(current ? current->refresh_interval : configuration_.policy.refresh_interval, configuration_.policy_refresh_jitter_percent);
        refresh_changed_.wait_for(lock, interval, [&] { return stop.stop_requested() || closed_.load(std::memory_order_acquire); });
        if (stop.stop_requested() || closed_.load(std::memory_order_acquire)) {
            break;
        }
        lock.unlock();
        if (auto updated = read_policy(false); updated) {
            policy_.store(std::move(*updated), std::memory_order_release);
        }
        lock.lock();
    }
}

result<void> client_core::close() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return {};
    }
    std::vector<std::shared_ptr<registration_core>> children;
    std::vector<std::shared_ptr<selector_core>> selectors;
    {
        std::lock_guard lock(children_mutex_);
        for (const auto& weak : children_) {
            if (auto value = weak.lock()) {
                children.push_back(std::move(value));
            }
        }
        children_.clear();
        for (const auto& weak : selectors_) {
            if (auto value = weak.lock()) {
                selectors.push_back(std::move(value));
            }
        }
        selectors_.clear();
    }
    std::optional<error> failure;
    for (const auto& child : children) {
        if (auto status = child->close(); !status && !failure) {
            failure = status.error();
        }
    }
    for (const auto& selector : selectors) {
        if (auto status = selector->close(); !status && !failure) {
            failure = status.error();
        }
    }

    std::jthread refresh;
    {
        std::lock_guard lock(refresh_mutex_);
        refresh_users_ = 0;
        if (refresh_worker_.joinable()) {
            refresh_worker_.request_stop();
            refresh_changed_.notify_all();
            refresh = std::move(refresh_worker_);
        }
    }
    if (refresh.joinable()) {
        refresh.join();
    }
    if (failure) {
        return std::unexpected(std::move(*failure));
    }
    return {};
}

bool client_core::open() const noexcept {
    return !closed_.load(std::memory_order_acquire) && transport_ && transport_->open();
}

const registration_configuration& client_core::configuration() const noexcept {
    return configuration_;
}

std::shared_ptr<const policy> client_core::limits() const noexcept {
    return policy_.load(std::memory_order_acquire);
}

std::shared_ptr<verdandi::detail::driver> client_core::transport() const noexcept {
    return transport_;
}

registration_core::registration_core(std::shared_ptr<client_core> owner, options value, std::string uuid)
    : owner_(std::move(owner)), options_(std::move(value)), uuid_(std::move(uuid)) {}

registration_core::~registration_core() {
    static_cast<void>(close());
}

result<std::shared_ptr<registration_core>> registration_core::create(const std::shared_ptr<client_core>& owner, options value) {
    if (!owner || !owner->open()) {
        return std::unexpected(error(code::closed));
    }
    if (!valid_type(value.type)) {
        return std::unexpected(error(code::invalid, "type"));
    }
    if (value.ttl <= std::chrono::milliseconds::zero() || static_cast<std::uint64_t>(value.ttl.count()) > hash_field_deadline_max) {
        return std::unexpected(error(code::invalid, "ttl"));
    }
    if (value.version == 0 || value.version > safe_integer_max) {
        return std::unexpected(error(code::invalid, "@version"));
    }
    const auto renewal = value.renew_interval.value_or(value.ttl / 3);
    if (renewal < owner->configuration().min_renew_interval || renewal > value.ttl / 3) {
        return std::unexpected(error(code::invalid, "renew_interval"));
    }
    value.renew_interval = renewal;
    auto uuid = new_uuid();
    if (!uuid) {
        return std::unexpected(uuid.error());
    }
    auto output = std::shared_ptr<registration_core>(new registration_core(owner, std::move(value), std::move(*uuid)));
    owner->add(output);
    return output;
}

std::string_view registration_core::uuid() const noexcept {
    return uuid_;
}

bool registration_core::published() const noexcept {
    return published_.load(std::memory_order_acquire) && !terminal_.load(std::memory_order_acquire);
}

std::uint64_t registration_core::revision() const noexcept {
    return revision_.load(std::memory_order_acquire);
}

std::uint64_t registration_core::timestamp() const noexcept {
    return timestamp_.load(std::memory_order_acquire);
}

result<void> registration_core::publish(fields attr, fields data) {
    std::lock_guard lifecycle(lifecycle_mutex_);
    if (terminal_.load(std::memory_order_acquire) || !owner_->open()) {
        return std::unexpected(error(code::closed));
    }
    if (published_.load(std::memory_order_acquire)) {
        return std::unexpected(error(code::contract, "publish", revision(), {}));
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    auto limits = owner_->limits();
    if (!limits) {
        return std::unexpected(error(code::unavailable, "policy"));
    }
    const auto ttl = static_cast<std::uint64_t>(options_.ttl.count());
    if (auto status = validate_record(uuid_, 1, ttl, options_.version, attr, data, *limits); !status) {
        return status;
    }
    data_shape_.clear();
    for (const auto& [name, value] : data) {
        static_cast<void>(value);
        data_shape_.emplace(name, bytes{});
    }
    return start(std::move(attr), std::move(data));
}

result<void> registration_core::start(fields attr, fields data) {
    state initial{
        std::move(attr), std::move(data), 1, 0, static_cast<std::uint64_t>(options_.ttl.count()), options_.version, false, false,
    };
    auto ready = std::make_shared<std::promise<result<void>>>();
    auto completed = ready->get_future();
    auto self = shared_from_this();
    worker_ = std::jthread([self, current = std::move(initial), ready](const std::stop_token& stop) mutable { self->run(stop, std::move(current), ready); });
    result<void> result;
    try {
        result = completed.get();
    } catch (const std::exception& exception) {
        result = std::unexpected(error(code::unavailable, "registration.worker").with_detail(exception.what()));
    } catch (...) {
        result = std::unexpected(error(code::unavailable, "registration.worker"));
    }
    if (!result && worker_.joinable()) {
        worker_.join();
    }
    return result;
}

result<void> registration_core::update(std::optional<std::uint64_t> version, std::optional<fields> data) {
    if (!published() || !owner_->open()) {
        return std::unexpected(error(code::closed));
    }
    if (version && (*version == 0 || *version > safe_integer_max)) {
        return std::unexpected(error(code::invalid, "@version"));
    }
    if (data && !same_shape(data_shape_, *data)) {
        return std::unexpected(error(code::contract, "data", revision(), {}));
    }
    if (!version && !data) {
        return std::unexpected(error(code::contract, "update", revision(), {}));
    }

    auto request = std::make_shared<registration_core::request>();
    request->type = registration_core::request::kind::update;
    auto completed = request->completed.get_future();
    {
        std::unique_lock lock(mailbox_mutex_);
        const auto deadline = std::chrono::steady_clock::now() + owner_->transport()->timeout();
        if (!mailbox_changed_.wait_until(
                lock, deadline, [&] { return closing_ || pending_updates_.size() + pending_renews_.size() < owner_->configuration().buffer_capacity; })) {
            return std::unexpected(error(code::deadline, "registration.buffer"));
        }
        if (closing_) {
            return std::unexpected(error(code::closed));
        }
        if (version) {
            desired_version_ = version;
        }
        if (data) {
            desired_data_ = std::move(*data);
        }
        pending_updates_.push_back(request);
        mailbox_changed_.notify_all();
    }
    try {
        return completed.get();
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::unavailable, "registration.worker").with_detail(exception.what()));
    } catch (...) {
        return std::unexpected(error(code::unavailable, "registration.worker"));
    }
}

result<void> registration_core::renew() {
    if (!published() || !owner_->open()) {
        return std::unexpected(error(code::closed));
    }
    auto request = std::make_shared<registration_core::request>();
    request->type = registration_core::request::kind::renew;
    auto completed = request->completed.get_future();
    {
        std::unique_lock lock(mailbox_mutex_);
        const auto deadline = std::chrono::steady_clock::now() + owner_->transport()->timeout();
        if (!mailbox_changed_.wait_until(
                lock, deadline, [&] { return closing_ || pending_updates_.size() + pending_renews_.size() < owner_->configuration().buffer_capacity; })) {
            return std::unexpected(error(code::deadline, "registration.buffer"));
        }
        if (closing_) {
            return std::unexpected(error(code::closed));
        }
        pending_renews_.push_back(request);
        mailbox_changed_.notify_all();
    }
    try {
        return completed.get();
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::unavailable, "registration.worker").with_detail(exception.what()));
    } catch (...) {
        return std::unexpected(error(code::unavailable, "registration.worker"));
    }
}

void registration_core::run(const std::stop_token& stop, state current, const std::shared_ptr<std::promise<result<void>>>& ready) {
    bool ready_sent{false};
    const auto finish_ready = [&](result<void> value) {
        if (!ready_sent) {
            ready->set_value(std::move(value));
            ready_sent = true;
        }
    };
    bool policy_user{false};
    try {
        auto initial = register_state(current);
        if (!initial) {
            const std::array<std::string, 1> arguments{uuid_};
            static_cast<void>(owner_->call(verdandi::detail::registration_operation::unregister, options_.type, uuid_, arguments));
            finish_ready(std::unexpected(initial.error()));
            return;
        }
        current.healthy = true;
        published_.store(true, std::memory_order_release);
        revision_.store(current.revision, std::memory_order_release);
        timestamp_.store(current.timestamp, std::memory_order_release);
        owner_->acquire_policy_user();
        // 该标志只在后续操作抛异常时读取；Clang 静态分析器不建立这条异常控制流。
        policy_user = true; // NOLINT(clang-analyzer-deadcode.DeadStores)
        finish_ready({});

        auto renewal = std::chrono::steady_clock::now() + jittered(*options_.renew_interval, owner_->configuration().renew_jitter_percent);
        for (;;) {
            std::unique_lock lock(mailbox_mutex_);
            mailbox_changed_.wait_until(lock, renewal,
                                        [&] { return closing_ || stop.stop_requested() || !pending_updates_.empty() || !pending_renews_.empty(); });
            const auto should_close = closing_ || stop.stop_requested() || !owner_->open();
            const auto has_pending = !pending_updates_.empty() || !pending_renews_.empty();
            lock.unlock();

            if (has_pending) {
                if (handle_pending(current, renewal)) {
                    renewal = std::chrono::steady_clock::now() + jittered(*options_.renew_interval, owner_->configuration().renew_jitter_percent);
                }
                continue;
            }
            if (should_close) {
                break;
            }
            if (std::chrono::steady_clock::now() >= renewal) {
                auto status = renew_state(current);
                if (!status) {
                    report(status.error());
                }
                renewal = std::chrono::steady_clock::now() + jittered(*options_.renew_interval, owner_->configuration().renew_jitter_percent);
            }
        }

        while (handle_pending(current, renewal)) {}
        auto final = unregister_state(current);
        {
            std::lock_guard lock(mailbox_mutex_);
            if (!final) {
                final_error_ = final.error();
            }
        }
        published_.store(false, std::memory_order_release);
        owner_->release_policy_user();
        return;
    } catch (const std::exception& exception) {
        final_error_ = error(code::unavailable, "registration.worker").with_detail(exception.what());
    } catch (...) {
        final_error_ = error(code::unavailable, "registration.worker");
    }

    published_.store(false, std::memory_order_release);
    if (policy_user) {
        try {
            owner_->release_policy_user();
        } catch (const std::exception& exception) {
            final_error_ = error(code::unavailable, "registration.policy").with_detail(exception.what());
        } catch (...) {
            final_error_ = error(code::unavailable, "registration.policy");
        }
    }
    const auto failure = final_error_.value_or(error(code::unavailable, "registration.worker"));
    fail_pending(failure);
    finish_ready(std::unexpected(failure));
}

void registration_core::fail_pending(const error& value) {
    std::vector<std::shared_ptr<request>> updates;
    std::vector<std::shared_ptr<request>> renews;
    {
        std::lock_guard lock(mailbox_mutex_);
        closing_ = true;
        desired_version_.reset();
        desired_data_.reset();
        updates.swap(pending_updates_);
        renews.swap(pending_renews_);
        if (errors_.size() >= owner_->configuration().error_buffer_capacity) {
            errors_.pop_front();
        }
        errors_.push_back(value);
        mailbox_changed_.notify_all();
    }
    const result<void> failed = std::unexpected(value);
    for (const auto& request : updates) {
        request->completed.set_value(failed);
    }
    for (const auto& request : renews) {
        request->completed.set_value(failed);
    }
}

bool registration_core::handle_pending(state& current, std::chrono::steady_clock::time_point& renewal) {
    std::optional<std::uint64_t> version;
    std::optional<fields> data;
    std::vector<std::shared_ptr<request>> updates;
    std::vector<std::shared_ptr<request>> renews;
    {
        std::lock_guard lock(mailbox_mutex_);
        if (pending_updates_.empty() && pending_renews_.empty()) {
            return false;
        }
        version = desired_version_;
        data = std::move(desired_data_);
        desired_version_.reset();
        desired_data_.reset();
        updates.swap(pending_updates_);
        renews.swap(pending_renews_);
        mailbox_changed_.notify_all();
    }

    bool wrote{};
    result<void> update_result;
    if (!updates.empty()) {
        const auto previous = current.revision;
        update_result = update_state(current, version, data);
        wrote = update_result && current.revision != previous;
        for (const auto& request : updates) {
            request->completed.set_value(update_result);
        }
    }
    result<void> renew_result;
    if (!renews.empty()) {
        if (!wrote) {
            renew_result = renew_state(current);
        }
        for (const auto& request : renews) {
            request->completed.set_value(renew_result);
        }
        wrote = wrote || renew_result.has_value();
    }
    if (wrote) {
        renewal = std::chrono::steady_clock::now() + jittered(*options_.renew_interval, owner_->configuration().renew_jitter_percent);
    }
    return wrote;
}

result<void> registration_core::register_state(state& current) {
    auto arguments = registration_arguments(uuid_, current.revision, current.ttl, current.version, current.attr, current.data);
    auto reply = owner_->call(verdandi::detail::registration_operation::register_value, options_.type, uuid_, arguments);
    if (!reply) {
        return std::unexpected(reply.error());
    }
    if (reply->revision != current.revision || reply->timestamp == 0) {
        return std::unexpected(error(code::corrupt, "reply", reply->revision, {}));
    }
    current.timestamp = reply->timestamp;
    current.uncertain = false;
    current.healthy = true;
    timestamp_.store(current.timestamp, std::memory_order_release);
    return {};
}

result<void> registration_core::update_state(state& current, const std::optional<std::uint64_t> version, const std::optional<fields>& data) {
    const auto next_version = version.value_or(current.version);
    fields changed;
    fields next_data = current.data;
    if (data) {
        for (const auto& [name, value] : *data) {
            const auto previous = current.data.find(name);
            if (previous == current.data.end()) {
                return std::unexpected(error(code::contract, name, current.revision, {}));
            }
            if (!same_bytes(previous->second, value)) {
                changed.emplace(name, value);
                next_data[name] = value;
            }
        }
    }
    const auto version_changed = next_version != current.version;
    if (!version_changed && changed.empty()) {
        return {};
    }
    if (current.revision >= safe_integer_max) {
        return std::unexpected(error(code::capacity, "@revision", current.revision, {}));
    }
    const auto next_revision = current.revision + 1;
    auto limits = owner_->limits();
    if (!limits) {
        return std::unexpected(error(code::unavailable, "policy"));
    }
    if (auto status = validate_record(uuid_, next_revision, current.ttl, next_version, current.attr, next_data, *limits); !status) {
        return status;
    }

    result<registration_reply> reply = std::unexpected(error(code::unavailable));
    if (current.uncertain) {
        auto arguments = registration_arguments(uuid_, next_revision, current.ttl, next_version, current.attr, next_data);
        reply = owner_->call(verdandi::detail::registration_operation::register_value, options_.type, uuid_, arguments);
    } else {
        auto arguments = update_arguments(uuid_, next_revision, version_changed ? std::optional(next_version) : std::nullopt, changed);
        reply = owner_->call(verdandi::detail::registration_operation::update, options_.type, uuid_, arguments);
        if (!reply && recoverable_missing(reply.error())) {
            auto complete = registration_arguments(uuid_, next_revision, current.ttl, next_version, current.attr, next_data);
            reply = owner_->call(verdandi::detail::registration_operation::register_value, options_.type, uuid_, complete);
        }
    }
    if (!reply || reply->revision != next_revision || reply->timestamp == 0) {
        error failure = !reply ? reply.error() : error(code::corrupt, "reply", reply->revision, {});
        if (uncertain(failure)) {
            current.revision = next_revision;
            current.version = next_version;
            current.data = std::move(next_data);
            current.uncertain = true;
            current.healthy = false;
            revision_.store(current.revision, std::memory_order_release);
        }
        return std::unexpected(std::move(failure));
    }
    current.revision = next_revision;
    current.timestamp = reply->timestamp;
    current.version = next_version;
    current.data = std::move(next_data);
    current.uncertain = false;
    current.healthy = true;
    revision_.store(current.revision, std::memory_order_release);
    timestamp_.store(current.timestamp, std::memory_order_release);
    return {};
}

result<void> registration_core::renew_state(state& current) {
    result<registration_reply> reply = std::unexpected(error(code::unavailable));
    if (current.uncertain) {
        auto arguments = registration_arguments(uuid_, current.revision, current.ttl, current.version, current.attr, current.data);
        reply = owner_->call(verdandi::detail::registration_operation::register_value, options_.type, uuid_, arguments);
    } else {
        const std::array<std::string, 2> arguments{uuid_, std::to_string(current.revision)};
        reply = owner_->call(verdandi::detail::registration_operation::renew, options_.type, uuid_, arguments);
        if (!reply && recoverable_missing(reply.error())) {
            auto complete = registration_arguments(uuid_, current.revision, current.ttl, current.version, current.attr, current.data);
            reply = owner_->call(verdandi::detail::registration_operation::register_value, options_.type, uuid_, complete);
        }
    }
    if (!reply || reply->revision != current.revision || reply->timestamp == 0) {
        error failure = !reply ? reply.error() : error(code::corrupt, "reply", reply->revision, {});
        if (uncertain(failure)) {
            current.uncertain = true;
            current.healthy = false;
        }
        return std::unexpected(std::move(failure));
    }
    current.timestamp = reply->timestamp;
    current.uncertain = false;
    current.healthy = true;
    timestamp_.store(current.timestamp, std::memory_order_release);
    return {};
}

result<void> registration_core::unregister_state(const state& current) {
    if (!current.healthy || current.uncertain || !owner_->transport()->open()) {
        return {};
    }
    const std::array<std::string, 1> arguments{uuid_};
    auto reply = owner_->call(verdandi::detail::registration_operation::unregister, options_.type, uuid_, arguments);
    if (!reply && reply.error().category() != code::missing) {
        return std::unexpected(reply.error());
    }
    return {};
}

void registration_core::report(const error& value) {
    std::lock_guard lock(mailbox_mutex_);
    if (errors_.size() >= owner_->configuration().error_buffer_capacity) {
        errors_.pop_front();
    }
    errors_.push_back(value);
}

result<void> registration_core::close() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    if (terminal_.exchange(true, std::memory_order_acq_rel)) {
        std::lock_guard lock(mailbox_mutex_);
        return final_error_ ? result<void>(std::unexpected(*final_error_)) : result<void>{};
    }
    {
        std::lock_guard lock(mailbox_mutex_);
        closing_ = true;
        mailbox_changed_.notify_all();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard lock(mailbox_mutex_);
    return final_error_ ? result<void>(std::unexpected(*final_error_)) : result<void>{};
}

std::optional<error> registration_core::try_error() {
    std::lock_guard lock(mailbox_mutex_);
    if (errors_.empty()) {
        return std::nullopt;
    }
    auto output = std::move(errors_.front());
    errors_.pop_front();
    return output;
}

result<std::shared_ptr<registration_core>> create_registration(const std::shared_ptr<client_core>& owner, options value) {
    return registration_core::create(owner, std::move(value));
}

std::string_view registration_uuid(const std::shared_ptr<registration_core>& value) noexcept {
    return value ? value->uuid() : std::string_view{};
}

bool registration_published(const std::shared_ptr<registration_core>& value) noexcept {
    return value && value->published();
}

std::uint64_t registration_revision(const std::shared_ptr<registration_core>& value) noexcept {
    return value ? value->revision() : 0;
}

std::uint64_t registration_timestamp(const std::shared_ptr<registration_core>& value) noexcept {
    return value ? value->timestamp() : 0;
}

result<void> registration_publish(const std::shared_ptr<registration_core>& value, fields attr, fields data) {
    return value ? value->publish(std::move(attr), std::move(data)) : result<void>(std::unexpected(error(code::closed)));
}

result<void> registration_update(const std::shared_ptr<registration_core>& value, std::optional<std::uint64_t> version, std::optional<fields> data) {
    return value ? value->update(version, std::move(data)) : result<void>(std::unexpected(error(code::closed)));
}

result<void> registration_renew(const std::shared_ptr<registration_core>& value) {
    return value ? value->renew() : result<void>(std::unexpected(error(code::closed)));
}

result<void> registration_close(const std::shared_ptr<registration_core>& value) {
    return value ? value->close() : result<void>{};
}

std::optional<error> registration_error(const std::shared_ptr<registration_core>& value) {
    return value ? value->try_error() : std::nullopt;
}

} // namespace detail

client::client(std::shared_ptr<detail::client_core> core) noexcept : core_(std::move(core)) {}

result<client> client::open(const verdandi::client& transport, const registration_configuration& configuration) {
    auto driver = verdandi::detail::driver_access::get(transport);
    auto opened = detail::client_core::open(std::move(driver), configuration);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    return client(std::move(*opened));
}

result<void> client::close() const {
    return core_ ? core_->close() : result<void>{};
}

bool client::open() const noexcept {
    return core_ && core_->open();
}

} // namespace verdandi::registration
