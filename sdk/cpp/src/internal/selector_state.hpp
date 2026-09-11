#pragma once

#include "internal/registration.hpp"
#include "verdandi/registration/selector.hpp"

#include <compare>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

namespace verdandi::registration::detail {

struct deadline_entry {
    std::uint64_t value{};
    std::string uuid;

    [[nodiscard]] auto operator<=>(const deadline_entry&) const = default;
};

struct selector_state {
    std::unordered_map<std::string, std::shared_ptr<const selector_record>> active;
    std::unordered_map<std::string, retained_record> retained;
    std::set<deadline_entry> active_deadlines;
    std::set<deadline_entry> retained_deadlines;
    std::size_t active_bytes{};
    std::size_t retained_bytes{};
};

[[nodiscard]] inline std::shared_ptr<const selector_record> find_record(const selector_state& state, const std::string_view uuid) {
    if (const auto active = state.active.find(std::string(uuid)); active != state.active.end()) {
        return active->second;
    }
    if (const auto retained = state.retained.find(std::string(uuid)); retained != state.retained.end()) {
        return retained->second.record;
    }
    return {};
}

inline void remove_retained(selector_state& state, const std::string_view uuid) {
    const auto iterator = state.retained.find(std::string(uuid));
    if (iterator == state.retained.end()) {
        return;
    }
    state.retained_bytes -= iterator->second.record->size;
    state.retained_deadlines.erase({iterator->second.until, iterator->first});
    state.retained.erase(iterator);
}

inline void remove_record(selector_state& state, const std::string_view uuid) {
    const auto iterator = state.active.find(std::string(uuid));
    if (iterator != state.active.end()) {
        state.active_bytes -= iterator->second->size;
        state.active_deadlines.erase({iterator->second->deadline, iterator->first});
        state.active.erase(iterator);
    }
    remove_retained(state, uuid);
}

/// 安装活动记录并替换原截止项，每个 UUID 始终最多占一个截止节点。
[[nodiscard]] inline result<void> set_active(selector_state& state, std::shared_ptr<const selector_record> record,
                                             const selector_configuration& configuration) {
    std::size_t previous{};
    if (const auto iterator = state.active.find(record->meta.uuid); iterator != state.active.end()) {
        previous = iterator->second->size;
    }
    const auto next = state.active_bytes - previous + record->size;
    if (next > configuration.max_active_bytes) {
        return std::unexpected(error(code::capacity, "selector_view"));
    }
    if (const auto iterator = state.active.find(record->meta.uuid); iterator != state.active.end()) {
        state.active_deadlines.erase({iterator->second->deadline, iterator->first});
    }
    remove_retained(state, record->meta.uuid);
    state.active_bytes = next;
    state.active.insert_or_assign(record->meta.uuid, record);
    state.active_deadlines.insert({record->deadline, record->meta.uuid});
    return {};
}

/// 把记录移动到不可选择 retained 视图，并在超限时优先驱逐最早截止项。
inline void set_retained(selector_state& state, const std::shared_ptr<const selector_record>& record, std::uint64_t until, const std::uint64_t now,
                         const selector_configuration& configuration) {
    if (const auto active = state.active.find(record->meta.uuid); active != state.active.end()) {
        state.active_bytes -= active->second->size;
        state.active_deadlines.erase({active->second->deadline, active->first});
        state.active.erase(active);
    }
    remove_retained(state, record->meta.uuid);
    if (configuration.max_retained_bytes == 0 || until <= now) {
        return;
    }
    state.retained.insert_or_assign(record->meta.uuid, retained_record{record, until});
    state.retained_bytes += record->size;
    state.retained_deadlines.insert({until, record->meta.uuid});
    while (state.retained_bytes > configuration.max_retained_bytes && !state.retained_deadlines.empty()) {
        const auto earliest = *state.retained_deadlines.begin();
        state.retained_deadlines.erase(state.retained_deadlines.begin());
        const auto iterator = state.retained.find(earliest.uuid);
        if (iterator != state.retained.end() && iterator->second.until == earliest.value) {
            state.retained_bytes -= iterator->second.record->size;
            state.retained.erase(iterator);
        }
    }
}

inline void retain(selector_state& state, const std::shared_ptr<const selector_record>& record, const std::uint64_t now,
                   const selector_configuration& configuration) {
    const auto until = record->deadline > safe_integer_max - record->meta.ttl ? safe_integer_max : record->deadline + record->meta.ttl;
    set_retained(state, record, until, now, configuration);
}

/// 驱动活动租约和 retained 第二截止；相同截止时间的 UUID 仍分别保留。
[[nodiscard]] inline bool expire(selector_state& state, const std::uint64_t now, const selector_configuration& configuration) {
    bool changed{false};
    while (!state.active_deadlines.empty() && state.active_deadlines.begin()->value <= now) {
        const auto earliest = *state.active_deadlines.begin();
        state.active_deadlines.erase(state.active_deadlines.begin());
        const auto iterator = state.active.find(earliest.uuid);
        if (iterator != state.active.end() && iterator->second->deadline == earliest.value) {
            auto record = iterator->second;
            retain(state, record, now, configuration);
            changed = true;
        }
    }
    while (!state.retained_deadlines.empty() && state.retained_deadlines.begin()->value <= now) {
        const auto earliest = *state.retained_deadlines.begin();
        state.retained_deadlines.erase(state.retained_deadlines.begin());
        const auto iterator = state.retained.find(earliest.uuid);
        if (iterator != state.retained.end() && iterator->second.until == earliest.value) {
            state.retained_bytes -= iterator->second.record->size;
            state.retained.erase(iterator);
            changed = true;
        }
    }
    return changed;
}

} // namespace verdandi::registration::detail
