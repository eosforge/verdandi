#pragma once

#include "internal/driver.hpp"
#include "internal/protocol.hpp"
#include "internal/script.hpp"
#include "verdandi/configuration.hpp"
#include "verdandi/registration/registration.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace verdandi::registration::detail {

constexpr std::uint64_t safe_integer_max = (std::uint64_t{1} << 53U) - 1U;
constexpr std::uint64_t hash_field_deadline_max = (std::uint64_t{1} << 46U) - 1U;

struct policy {
    std::size_t attr_max_fields;
    std::size_t data_max_fields;
    std::size_t field_name_max_bytes;
    std::size_t attr_value_max_bytes;
    std::size_t data_value_max_bytes;
    std::size_t record_max_bytes;
    std::chrono::milliseconds refresh_interval;
};

struct registration_reply {
    std::uint64_t revision{};
    std::uint64_t timestamp{};
};

class registration_core;
class selector_core;

class client_core final : public std::enable_shared_from_this<client_core> {
public:
    client_core(std::shared_ptr<verdandi::detail::driver> transport, registration_configuration configuration);
    ~client_core();

    [[nodiscard]] static result<std::shared_ptr<client_core>> open(std::shared_ptr<verdandi::detail::driver> transport,
                                                                   const registration_configuration& configuration);
    [[nodiscard]] result<void> close();
    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] const registration_configuration& configuration() const noexcept;
    [[nodiscard]] std::shared_ptr<const policy> limits() const noexcept;
    [[nodiscard]] std::shared_ptr<verdandi::detail::driver> transport() const noexcept;

    [[nodiscard]] result<registration_reply> call(verdandi::detail::registration_operation operation, std::string_view type, std::string_view uuid,
                                                  std::span<const std::string> arguments, bool mutation = true);
    void add(const std::shared_ptr<registration_core>& value);
    void add(const std::shared_ptr<selector_core>& value);
    void acquire_policy_user();
    void release_policy_user();

private:
    [[nodiscard]] result<void> bootstrap();
    [[nodiscard]] result<std::shared_ptr<const policy>> read_policy(bool install_defaults);
    void refresh_policy(const std::stop_token& stop);

    std::shared_ptr<verdandi::detail::driver> transport_;
    registration_configuration configuration_;
    verdandi::detail::script register_script_;
    verdandi::detail::script update_script_;
    verdandi::detail::script renew_script_;
    verdandi::detail::script unregister_script_;
    std::atomic<std::shared_ptr<const policy>> policy_;
    std::atomic_bool closed_{false};
    std::mutex children_mutex_;
    std::vector<std::weak_ptr<registration_core>> children_;
    std::vector<std::weak_ptr<selector_core>> selectors_;
    std::mutex refresh_mutex_;
    std::condition_variable_any refresh_changed_;
    std::size_t refresh_users_{};
    std::jthread refresh_worker_;
};

class registration_core final : public std::enable_shared_from_this<registration_core> {
public:
    registration_core(std::shared_ptr<client_core> owner, options value, std::string uuid);
    ~registration_core();

    [[nodiscard]] static result<std::shared_ptr<registration_core>> create(const std::shared_ptr<client_core>& owner, options value);
    [[nodiscard]] std::string_view uuid() const noexcept;
    [[nodiscard]] bool published() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint64_t timestamp() const noexcept;
    [[nodiscard]] result<void> publish(fields attr, fields data);
    [[nodiscard]] result<void> update(std::optional<std::uint64_t> version, std::optional<fields> data);
    [[nodiscard]] result<void> renew();
    [[nodiscard]] result<void> close();
    [[nodiscard]] std::optional<error> try_error();

private:
    struct request {
        enum class kind : std::uint8_t {
            update,
            renew,
        };
        kind type;
        std::promise<result<void>> completed;
    };

    struct state {
        fields attr;
        fields data;
        std::uint64_t revision{1};
        std::uint64_t timestamp{};
        std::uint64_t ttl{};
        std::uint64_t version{};
        bool uncertain{false};
        bool healthy{false};
    };

    [[nodiscard]] result<void> start(fields attr, fields data);
    void run(const std::stop_token& stop, state current, const std::shared_ptr<std::promise<result<void>>>& ready);
    [[nodiscard]] bool handle_pending(state& current, std::chrono::steady_clock::time_point& renewal);
    [[nodiscard]] result<void> update_state(state& current, std::optional<std::uint64_t> version, const std::optional<fields>& data);
    [[nodiscard]] result<void> renew_state(state& current);
    [[nodiscard]] result<void> register_state(state& current);
    [[nodiscard]] result<void> unregister_state(const state& current);
    void fail_pending(const error& value);
    void report(const error& value);

    std::shared_ptr<client_core> owner_;
    options options_;
    std::string uuid_;
    std::atomic_bool published_{false};
    std::atomic_bool terminal_{false};
    std::atomic_uint64_t revision_{};
    std::atomic_uint64_t timestamp_{};
    std::mutex lifecycle_mutex_;
    std::mutex mailbox_mutex_;
    std::condition_variable_any mailbox_changed_;
    std::optional<std::uint64_t> desired_version_;
    std::optional<fields> desired_data_;
    std::vector<std::shared_ptr<request>> pending_updates_;
    std::vector<std::shared_ptr<request>> pending_renews_;
    std::deque<error> errors_;
    std::optional<error> final_error_;
    fields data_shape_;
    bool closing_{false};
    std::jthread worker_;
};

[[nodiscard]] bool valid_type(std::string_view value) noexcept;
[[nodiscard]] result<void> validate_record(std::string_view uuid, std::uint64_t revision, std::uint64_t ttl, std::uint64_t version, const fields& attr,
                                           const fields& data, const policy& limits);

} // namespace verdandi::registration::detail
