#include "ephemeris.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <sys/random.h>
#include <system_error>

namespace astra {
bool Ephemeris::valid(const Value& value) noexcept {
    return value && value->size() <= 1024 * 1024;
}

bool Ephemeris::valid(const Record& record) noexcept {
    return valid(record.attr) && valid(record.data) && record.ttl >= 1000 && record.ttl <= 600000 && record.deadline.time_since_epoch().count() >= 0;
}

Ephemeris::Error Ephemeris::error(Clock::Error value) noexcept {

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

bool Ephemeris::valid(std::string_view uuid) noexcept {

    if (uuid.size() != 36 || uuid[14] != '4' || (uuid[19] != '8' && uuid[19] != '9' && uuid[19] != 'a' && uuid[19] != 'b')) {
        return false;
    }
    for (std::size_t index = 0; index < uuid.size(); ++index) {
        const auto value = uuid[index]; // 当前 ASCII 字节, 不做大小写或 Unicode 归一化.
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != '-') {
                return false;
            }
        } else if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string Ephemeris::uuid() {

    std::array<std::uint8_t, 16> bytes{}; // 系统随机字节, 仅生成时在栈上保存原始二进制.
    std::size_t offset{};                 // 已取得字节数, 正确处理 EINTR 和短读取.
    while (offset < bytes.size()) {
        const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "UUID random source failed");
        }
        offset += static_cast<std::size_t>(count);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U); // 固定 UUID 版本 4.
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U); // 固定 RFC variant, 保留其余随机位.

    constexpr std::string_view digits = "0123456789abcdef"; // 项目统一小写, 不在后续请求中重复格式化.
    std::string result(36, '-');                            // 只有此处创建规范文本, 字节不经过流格式化或区域设置.
    offset = 0;
    for (const auto value : bytes) {
        if (offset == 8 || offset == 13 || offset == 18 || offset == 23) {
            ++offset;
        }
        result[offset++] = digits[value >> 4];
        result[offset++] = digits[value & 15];
    }
    return result;
}

std::expected<Ephemeris::Record, Ephemeris::Error> Ephemeris::create(Value attr, Value data, std::uint32_t ttl, const Clock::Reading& reading) noexcept {

    if (!valid(attr) || !valid(data) || ttl < 1000 || ttl > 600000) {
        return std::unexpected(Error::input);
    }
    const auto deadline = reading.deadline_after(std::chrono::milliseconds(ttl)); // 只保存一次绝对截止, 不读取第二个时钟.
    if (!deadline) {
        return std::unexpected(error(deadline.error()));
    }
    return Record{std::move(attr), std::move(data), *deadline, 0, 0, ttl};
}

std::expected<void, Ephemeris::Error> Ephemeris::active(const Record& current, const Clock::Reading& reading) noexcept {

    if (!valid(current) || reading.time.time_since_epoch().count() < 0) {
        return std::unexpected(Error::input);
    }
    if (!reading.ready) {
        return std::unexpected(Error::clock);
    }
    if (reading.time >= current.deadline) {
        return std::unexpected(Error::ended);
    }
    return {};
}

std::expected<Ephemeris::Change, Ephemeris::Error> Ephemeris::update(const Record& current, Value data, std::uint64_t order, const Clock::Reading& reading) noexcept {

    if (!valid(data) || order == 0) {
        return std::unexpected(Error::input);
    }
    if (const auto state = active(current, reading); !state) {
        return std::unexpected(state.error());
    }
    if (order < current.update) {
        return std::unexpected(Error::obsolete);
    }
    const bool same = data == current.data || *data == *current.data; // 同指针无需比较正文, 不使用不可靠摘要决定相等.
    if (order == current.update) {
        return same ? std::expected<Change, Error>(Change{current, false, false}) : std::unexpected(Error::conflict);
    }

    auto record = current; // 保留固定 Attr、TTL、deadline 和独立续租顺序, 不分配载荷.
    record.update = order;
    if (!same) {
        record.data = std::move(data);
    }
    return Change{std::move(record), true, !same};
}

std::expected<Ephemeris::Change, Ephemeris::Error> Ephemeris::renew(const Record& current, std::uint64_t order, const Clock::Reading& reading) noexcept {

    if (order == 0) {
        return std::unexpected(Error::input);
    }
    if (const auto state = active(current, reading); !state) {
        return std::unexpected(state.error());
    }
    if (order < current.renewal) {
        return std::unexpected(Error::obsolete);
    }
    if (order == current.renewal) {
        return Change{current, false, false}; // 不再计算 deadline, 极值时的合法重试也不误报相加溢出.
    }
    const auto deadline = reading.deadline_after(std::chrono::milliseconds(current.ttl)); // 只使用 Create 时固定的 TTL.
    if (!deadline) {
        return std::unexpected(error(deadline.error()));
    }

    auto record = current; // 固定 Attr/Data 引用和 Data order, 无载荷复制.
    record.renewal = order;
    record.deadline = std::max(record.deadline, *deadline); // 不因异常旧读数缩短已经确认的期限.
    return Change{std::move(record), true, false};
}
} // namespace astra
