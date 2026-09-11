#pragma once

#include "verdandi/configuration.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <stop_token>

namespace verdandi::detail {

// 退避与可取消等待供两个域共享；factor=1 始终为常量时间。
[[nodiscard]] inline std::chrono::milliseconds retry_delay(const reconnect_configuration& configuration, const std::size_t failures) {
    std::int64_t value = configuration.initial_delay.count();
    for (std::size_t index = 0; configuration.multiplier > 1 && index < failures && value < configuration.max_delay.count(); ++index) {
        if (value > configuration.max_delay.count() / static_cast<std::int64_t>(configuration.multiplier)) {
            value = configuration.max_delay.count();
            break;
        }
        value *= configuration.multiplier;
    }
    value = std::min(value, configuration.max_delay.count());
    const auto span = value * configuration.jitter_percent / 100;
    if (span == 0) {
        return std::chrono::milliseconds{value};
    }
    thread_local std::mt19937_64 generator(std::random_device{}());
    std::uniform_int_distribution<std::int64_t> distribution(0, span);
    return std::chrono::milliseconds{value - span + distribution(generator)};
}

[[nodiscard]] inline bool wait_stop(const std::stop_token& stop, const std::chrono::milliseconds delay) {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unique_lock lock(mutex);
    return !changed.wait_for(lock, stop, delay, [] { return false; });
}

} // namespace verdandi::detail