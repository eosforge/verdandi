#pragma once

#include "internal/registration.hpp"
#include "verdandi/registration/selector.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace verdandi::registration::detail {

class selector_core final : public std::enable_shared_from_this<selector_core> {
public:
    selector_core(std::shared_ptr<client_core> owner, selector_options options, projector project);
    ~selector_core();

    [[nodiscard]] static result<std::shared_ptr<selector_core>> create(const std::shared_ptr<client_core>& owner, selector_options options, projector project);
    [[nodiscard]] std::shared_ptr<const selector_view> current_view() const noexcept;
    [[nodiscard]] result<void> validate_data(const fields& data) const;
    [[nodiscard]] std::chrono::milliseconds wait() const noexcept;
    [[nodiscard]] result<void> close();
    [[nodiscard]] std::optional<error> try_error();

private:
    void run(const std::stop_token& stop, const std::shared_ptr<std::promise<result<void>>>& ready);
    void report(error value);

    std::shared_ptr<client_core> owner_;
    selector_options options_;
    projector project_;
    std::atomic<std::shared_ptr<const selector_view>> view_;
    std::atomic_bool closed_{false};
    std::mutex lifecycle_mutex_;
    std::jthread listener_;
    std::mutex errors_mutex_;
    std::deque<error> errors_;
    std::optional<error> final_error_;
};

} // namespace verdandi::registration::detail
