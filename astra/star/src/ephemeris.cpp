#include "ephemeris.hpp"
#include <algorithm>
#include <array>
#include <astra/profile.hpp>
#include <cerrno>
#include <chrono>
#include <openssl/rand.h>
#include <openssl/sha.h>
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

    if (uuid.size() != 16) {
        return false;
    }
    const auto bytes = reinterpret_cast<const std::uint8_t*>(uuid.data());
    return (bytes[6] & 0xf0U) == 0x40U && (bytes[8] & 0xc0U) == 0x80U; // 仅接受版本 4 与 RFC 变体, 不解释文本别名.
}

bool Ephemeris::follows(const Record& previous, const Record& incoming) noexcept {
    return previous.ttl == incoming.ttl && *previous.attr == *incoming.attr && incoming.generation >= previous.generation && incoming.update >= previous.update && incoming.deadline >= previous.deadline && (incoming.generation > previous.generation || (incoming.renewal >= previous.renewal && (incoming.update != previous.update || incoming.renewal != previous.renewal || incoming.deadline == previous.deadline))) && (incoming.update != previous.update || *incoming.data == *previous.data);
}

std::string Ephemeris::capability() {
    std::string result(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(result.data()), result.size()) != 1) {
        throw std::runtime_error("Ephemeris random source failed");
    }
    return result;
}

std::string Ephemeris::identity(const Scope& scope, const Value& attr, std::uint32_t ttl, std::string_view capability) {

    if (capability.size() != 32 || !attr || !scope.valid()) {
        return {};
    }
    SHA256_CTX context;
    SHA256_Init(&context);
    const auto field = [&](std::string_view value) {
        const auto size = static_cast<std::uint32_t>(value.size());
        const std::array<std::uint8_t, 4> length{static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16), static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
        SHA256_Update(&context, length.data(), length.size());
        SHA256_Update(&context, value.data(), value.size());
    }; // 有长度前缀的域分离, 不允许通过拼接碰撞改变固定注册参数.
    field("astra.ephemeris.capability.v1");
    field(capability);
    field(scope.sector);
    field(scope.spectrum);
    field(std::string_view(reinterpret_cast<const char*>(attr->data()), attr->size()));
    const std::array<std::uint8_t, 4> duration{static_cast<std::uint8_t>(ttl >> 24), static_cast<std::uint8_t>(ttl >> 16), static_cast<std::uint8_t>(ttl >> 8), static_cast<std::uint8_t>(ttl)};
    SHA256_Update(&context, duration.data(), duration.size());
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256_Final(digest.data(), &context);
    digest[6] = static_cast<unsigned char>((digest[6] & 0x0fU) | 0x40U);
    digest[8] = static_cast<unsigned char>((digest[8] & 0x3fU) | 0x80U);
    return std::string(reinterpret_cast<const char*>(digest.data()), 16);
}

std::string Ephemeris::uuid() {

    std::string result(16, '\0'); // 直接保存原始二进制, 不再格式化连字符文本.
    std::size_t offset{};         // 已取得字节数, 正确处理 EINTR 和短读取.
    while (offset < result.size()) {
        const auto count = ::getrandom(result.data() + offset, result.size() - offset, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "UUID random source failed");
        }
        offset += static_cast<std::size_t>(count);
    }
    auto* bytes = reinterpret_cast<std::uint8_t*>(result.data());
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U); // 固定 UUID 版本 4.
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U); // 固定 RFC variant, 保留其余随机位.
    return result;
}

std::expected<Ephemeris::Record, Ephemeris::Error> Ephemeris::create(Value attr, Value data, std::uint32_t ttl, const Clock::Reading& reading) noexcept {

    ASTRA_PROFILE_SCOPE("star.ephemeris.Ephemeris.create");

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

    ASTRA_PROFILE_SCOPE("star.ephemeris.Ephemeris.update");

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
    const auto deadline = reading.deadline_after(std::chrono::milliseconds(current.ttl));
    if (!deadline) {
        return std::unexpected(error(deadline.error()));
    }
    record.deadline = std::max(current.deadline, *deadline);
    record.update = order;
    if (!same) {
        record.data = std::move(data);
    }
    return Change{std::move(record), true, !same};
}

std::expected<Ephemeris::Change, Ephemeris::Error> Ephemeris::renew(const Record& current, std::uint64_t order, const Clock::Reading& reading) noexcept {

    ASTRA_PROFILE_SCOPE("star.ephemeris.Ephemeris.renew");

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
