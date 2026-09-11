#pragma once

#include "verdandi/error.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace verdandi::detail {

/// 专用 Pub/Sub 连接传给域监听任务的有界队列项。
struct subscription_item {
    enum class kind : std::uint8_t {
        message,
        reconnected,
        lagged,
        fence,
        idle,
        failure,
        closed,
    };

    kind type{kind::message};
    std::string channel;
    std::string payload;
    std::optional<std::string> pattern;
    std::uint64_t fence_id{};
    std::optional<error> failure;
};

/// 由调用者持锁；丢失状态独立于队列，任何控制项都不能覆盖它。
class subscription_queue final {
public:
    explicit subscription_queue(const std::size_t capacity) : capacity_(capacity) {}

    void push(subscription_item item) {
        if (items_.size() >= capacity_) {
            items_.clear();
            lagged_ = true;
        }
        items_.push_back(std::move(item));
    }

    [[nodiscard]] bool empty() const noexcept {
        return !lagged_ && items_.empty();
    }

    [[nodiscard]] subscription_item pop() {
        if (std::exchange(lagged_, false)) {
            return {subscription_item::kind::lagged, {}, {}, std::nullopt, 0, std::nullopt};
        }
        auto item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    void clear() noexcept {
        items_.clear();
        lagged_ = false;
    }

private:
    std::deque<subscription_item> items_;
    std::size_t capacity_;
    bool lagged_{};
};

} // namespace verdandi::detail