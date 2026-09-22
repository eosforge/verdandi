#include "catalog.hpp"
#include <algorithm>
#include <chrono>

namespace astra {
bool Catalog::valid(const Record& record) noexcept {
    return record.version != 0 && static_cast<bool>(record.value) == record.deadline.has_value() && (!record.value || (record.value->size() <= 1024 * 1024 && record.deadline->time_since_epoch().count() >= 0));
}

bool Catalog::valid(const Record* source, const Record* merged) noexcept {
    return (!source || valid(*source)) && (!merged || valid(*merged));
}

Catalog::Error Catalog::error(Clock::Error value) noexcept {

    switch (value) {
    case Clock::Error::clock_unready:
        return Error::clock;
    case Clock::Error::invalid_time:
        return Error::input;
    case Clock::Error::exhausted:
        return Error::exhausted;
    }
    return Error::input;
}

std::expected<void, Catalog::Error> Catalog::ready(const Clock::Reading& reading) noexcept {

    if (reading.time.time_since_epoch().count() < 0) {
        return std::unexpected(Error::input);
    }
    if (!reading.ready) {
        return std::unexpected(Error::clock);
    }
    return {};
}

std::expected<void, Catalog::Error> Catalog::check(const Record* current, std::uint64_t version, const Value& value) noexcept {

    if (!current) {
        return {};
    }
    if (version < current->version) {
        return std::unexpected(Error::version);
    }
    if (version == current->version && current->value && value != current->value && *value != *current->value) {
        return std::unexpected(Error::conflict);
    }
    return {};
}

std::expected<Catalog::Record, Catalog::Error> Catalog::publish(const Record* source, const Record* merged, Value value, std::uint64_t version, std::uint32_t ttl, const Clock::Reading& reading) noexcept {

    if (!valid(source, merged) || !value || value->size() > 1024 * 1024 || version == 0 || ttl < 1000 || ttl > 600000) {
        return std::unexpected(Error::input);
    }
    if (const auto checked = check(source, version, value); !checked) {
        return std::unexpected(checked.error());
    }
    if (const auto checked = check(merged, version, value); !checked) {
        return std::unexpected(checked.error());
    }
    const auto deadline = reading.deadline_after(std::chrono::milliseconds(ttl)); // 自己的候选截止, 不借 merged 的远端较晚截止.
    if (!deadline) {
        return std::unexpected(error(deadline.error()));
    }

    Record record{version, std::move(value), *deadline}; // 新版本仅使用此次请求的 TTL, 不继承旧版本期限.
    if (source && source->version == version && source->value) {
        record.value = source->value; // check 已确认完全相同, 保留既有字节所有权, 不制造重复内容历史.
        record.deadline = std::max(*source->deadline, *deadline);
    } else if (merged && merged->version == version && merged->value) {
        record.value = merged->value; // 可以复用不可变正文, 不能复用该来源的期限或身份.
    }
    return record;
}

std::expected<Catalog::Record, Catalog::Error> Catalog::renew(const Record* source, const Record* merged, std::uint64_t version, std::uint32_t ttl, const Clock::Reading& reading) noexcept {

    if (!valid(source, merged) || version == 0 || ttl < 1000 || ttl > 600000) {
        return std::unexpected(Error::input);
    }
    if (const auto clock = ready(reading); !clock) {
        return std::unexpected(clock.error());
    }
    if ((merged && merged->version > version) || (source && source->version != version)) {
        return std::unexpected(Error::version);
    }
    if (!source || !source->value || reading.time >= *source->deadline) {
        return std::unexpected(Error::ended);
    }
    const auto deadline = reading.deadline_after(std::chrono::milliseconds(ttl)); // 请求明确提供 TTL, 零不表示沿用旧值.
    if (!deadline) {
        return std::unexpected(error(deadline.error()));
    }

    auto record = *source; // 所有权共享但 metadata 私有, 冻结的旧来源快照不被续租修改.
    record.deadline = std::max(*record.deadline, *deadline);
    return record;
}

Catalog::Record Catalog::expire(const Record& current, Clock::Time now) noexcept {
    return current.deadline && now >= *current.deadline ? Record{current.version, nullptr, std::nullopt} : current;
}

bool Catalog::visible(const Record* current, const Record& candidate) noexcept {

    if (!current || !current->value) {
        return static_cast<bool>(candidate.value);
    }
    if (!candidate.value || current->version != candidate.version) {
        return true;
    }
    return current->value != candidate.value && *current->value != *candidate.value;
}

std::expected<Catalog::Change, Catalog::Error> Catalog::merge(const Record* current, const Record& incoming, Clock::Time now) noexcept {

    if (!valid(current, &incoming) || now.time_since_epoch().count() < 0) {
        return std::unexpected(Error::input);
    }
    if (current && current->version == incoming.version && current->value && incoming.value && current->value != incoming.value && *current->value != *incoming.value) {
        return std::unexpected(Error::conflict);
    }

    auto record = current ? expire(*current, now) : Record{}; // 本地删除只生成下游投影变化, 绝不广播成源端结束.
    const auto fact = expire(incoming, now);                  // 旧的源端截止原样判定, 不把迟到消息再续一个完整 TTL.
    if (!current || incoming.version > current->version) {
        record = fact; // 更高的过期事实/水位也提高下限, 不回落到仍有效的低版本.
    } else if (incoming.version == current->version && fact.value) {
        if (!record.value) {
            record = fact;
        } else {
            record.deadline = std::max(*record.deadline, *fact.deadline); // 同版本来自多个来源时保留最长已知有效期限.
        }
    }
    return Change{record, visible(current, record)};
}
} // namespace astra
