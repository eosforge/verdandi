#pragma once

#include "internal/driver.hpp"
#include "internal/protocol.hpp"
#include "internal/script.hpp"
#include "verdandi/catalog/catalog.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace verdandi::catalog::detail {

constexpr std::uint64_t maximum_revision = (std::uint64_t{1} << 53U) - 1U;
constexpr std::size_t maximum_fields = 65'536;

class subscriber_core;
class checkpoint_store;

[[nodiscard]] result<std::size_t> validate_catalog_value(kind shape, const fields& value, std::size_t maximum_bytes);
[[nodiscard]] result<void> validate_catalog_patch(const fields& value, std::size_t maximum_bytes);
[[nodiscard]] std::optional<kind> parse_catalog_kind(std::string_view value) noexcept;
[[nodiscard]] std::string catalog_prefix(std::string_view zone);
[[nodiscard]] std::string catalog_meta_key(std::string_view zone);
[[nodiscard]] std::string catalog_live_key(std::string_view zone);
[[nodiscard]] std::string catalog_deleted_key(std::string_view zone);
[[nodiscard]] std::string catalog_deleted_time_key(std::string_view zone);
[[nodiscard]] std::string catalog_value_key(std::string_view zone, const path& target);
[[nodiscard]] std::string catalog_field_revisions_key(std::string_view zone, const path& target);
[[nodiscard]] std::vector<std::string> catalog_read_keys(std::string_view zone, const path& target);

class client_core final : public std::enable_shared_from_this<client_core> {
public:
    client_core(std::shared_ptr<verdandi::detail::driver> transport, catalog_configuration configuration);
    ~client_core();

    [[nodiscard]] static result<std::shared_ptr<client_core>> open(std::shared_ptr<verdandi::detail::driver> transport,
                                                                   const catalog_configuration& configuration);
    [[nodiscard]] result<void> close();
    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] const catalog_configuration& configuration() const noexcept;
    [[nodiscard]] std::shared_ptr<verdandi::detail::driver> transport() const noexcept;
    [[nodiscard]] verdandi::detail::script& read_script() noexcept;
    [[nodiscard]] verdandi::detail::script& replace_script() noexcept;
    [[nodiscard]] verdandi::detail::script& patch_script() noexcept;
    [[nodiscard]] verdandi::detail::script& delete_script() noexcept;
    [[nodiscard]] std::shared_mutex& operations() noexcept;
    [[nodiscard]] std::shared_ptr<checkpoint_store> store() const noexcept;
    void add(const std::shared_ptr<subscriber_core>& value);

private:
    [[nodiscard]] result<void> bootstrap();

    std::shared_ptr<verdandi::detail::driver> transport_;
    catalog_configuration configuration_;
    verdandi::detail::script read_script_;
    verdandi::detail::script replace_script_;
    verdandi::detail::script patch_script_;
    verdandi::detail::script delete_script_;
    std::shared_ptr<checkpoint_store> store_;
    std::atomic_bool closed_{false};
    std::shared_mutex operations_;
    std::mutex children_mutex_;
    std::vector<std::weak_ptr<subscriber_core>> children_;
};

} // namespace verdandi::catalog::detail
