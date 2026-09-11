#pragma once

#include "verdandi/fields.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace verdandi::registration::detail {

enum class event_kind : std::uint8_t {
    register_value,
    update,
    renew,
    unregister,
};

struct registration_event {
    event_kind kind{event_kind::register_value};
    std::string uuid;
    std::uint64_t revision{};
    std::uint64_t base_revision{};
    std::uint64_t timestamp{};
    std::uint64_t ttl{};
    std::uint64_t version{};
    bool has_version{false};
    fields attr;
    fields data;
    std::size_t size{};
};

class pending_events final {
public:
    pending_events(const std::size_t entry_limit, const std::size_t byte_limit) : entry_limit_(entry_limit), byte_limit_(byte_limit) {}

    /// 按 UUID 合并连续 Update/Renew，保证高频单 Registration 不会线性占用同步缓存。
    [[nodiscard]] result<void> add(registration_event value) {
        value.size = event_size(value);
        const auto size = value.size;
        auto [iterator, inserted] = values_.try_emplace(value.uuid);
        if (inserted) {
            if (values_.size() > entry_limit_) {
                values_.erase(iterator);
                return std::unexpected(error(code::capacity, "selector_pending"));
            }
        }
        auto& sequence = iterator->second;
        if (value.kind == event_kind::register_value || value.kind == event_kind::unregister) {
            for (const auto& previous : sequence) {
                bytes_ -= previous.size;
            }
            sequence.clear();
            sequence.push_back(std::move(value));
            bytes_ += size;
        } else if (!sequence.empty() && can_merge(sequence.back(), value)) {
            auto& current = sequence.back();
            const auto previous_size = current.size;
            // 临近容量上限时先准备副本；拒绝合并不破坏此前已接纳的状态。
            if (size > byte_limit_ || bytes_ > byte_limit_ - size) {
                auto next = current;
                merge(next, value);
                next.size = event_size(next);
                if (next.size > byte_limit_ || bytes_ - previous_size > byte_limit_ - next.size) {
                    return std::unexpected(error(code::capacity, "selector_pending"));
                }
                current = std::move(next);
            } else {
                merge(current, value);
                current.size = event_size(current);
            }
            bytes_ = bytes_ - previous_size + current.size;
        } else {
            sequence.push_back(std::move(value));
            bytes_ += size;
        }
        if (bytes_ > byte_limit_) {
            return std::unexpected(error(code::capacity, "selector_pending"));
        }
        return {};
    }

    [[nodiscard]] std::vector<registration_event> drain() {
        std::vector<registration_event> output;
        for (auto& [uuid, sequence] : values_) {
            static_cast<void>(uuid);
            for (auto& value : sequence) {
                output.push_back(std::move(value));
            }
        }
        values_.clear();
        bytes_ = 0;
        return output;
    }

private:
    [[nodiscard]] static std::size_t event_size(const registration_event& value) noexcept {
        std::size_t size = 128 + value.uuid.size();
        for (const auto* group : {&value.attr, &value.data}) {
            for (const auto& [name, field] : *group) {
                size += 16 + name.size() + field.size();
            }
        }
        return size;
    }

    [[nodiscard]] static bool can_merge(const registration_event& current, const registration_event& next) {
        if (next.kind == event_kind::renew && current.revision == next.revision &&
            (current.kind == event_kind::register_value || current.kind == event_kind::update || current.kind == event_kind::renew)) {
            return true;
        }
        // 保留断档事件，让回放触发权威读取；只合并紧邻的增量。
        if (next.kind != event_kind::update || next.base_revision != current.revision || next.revision != current.revision + 1) {
            return false;
        }
        if (current.kind == event_kind::register_value) {
            return std::ranges::all_of(next.data, [&current](const auto& field) { return current.data.contains(field.first); });
        }
        return current.kind == event_kind::update;
    }

    static void merge(registration_event& current, const registration_event& next) {
        current.timestamp = std::max(current.timestamp, next.timestamp);
        if (next.kind == event_kind::renew) {
            return;
        }
        for (const auto& [name, value] : next.data) {
            current.data.insert_or_assign(name, value);
        }
        current.revision = next.revision;
        if (next.has_version) {
            current.version = next.version;
            current.has_version = true;
        }
    }

    std::map<std::string, std::vector<registration_event>, std::less<>> values_;
    std::size_t entry_limit_;
    std::size_t byte_limit_;
    std::size_t bytes_{};
};

} // namespace verdandi::registration::detail
