// 许可证: MIT, 详见仓库根目录 LICENSE.
#include <verdandi/peer/types.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <charconv>
#include <ranges>

namespace verdandi::peer {
std::string_view error_name(ErrorCode code) {
    switch (code) {
    case ErrorCode::configuration:
        return "configuration";
    case ErrorCode::identity:
        return "identity";
    case ErrorCode::protocol:
        return "protocol";
    case ErrorCode::conflict:
        return "conflict";
    case ErrorCode::capacity:
        return "capacity";
    case ErrorCode::timeout:
        return "timeout";
    case ErrorCode::transport:
        return "transport";
    case ErrorCode::cancelled:
        return "cancelled";
    case ErrorCode::internal:
        return "internal";
    }
    return "internal";
}
namespace {
// 端口解析必须消费所有字符; 只有 listener 允许零, 正负号和溢出均显式拒绝.
Result<std::uint16_t> port_number(std::string_view value, bool local) {
    unsigned port = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
    if (error != std::errc{} || end != value.data() + value.size() || port > 65535 || (!local && port == 0)) {
        return std::unexpected(Error{ErrorCode::configuration, "Invalid endpoint port"});
    }
    return static_cast<std::uint16_t>(port);
}
} // namespace

Result<Endpoint> Endpoint::parse(std::string_view value, bool local) {
    const auto separator = value.rfind(':');
    if (separator == std::string_view::npos) {
        return std::unexpected(Error{ErrorCode::configuration, "Endpoint requires IP:PORT"});
    }
    auto port = port_number(value.substr(separator + 1), local);
    if (!port) {
        return std::unexpected(port.error());
    }
    auto host = value.substr(0, separator);
    const bool ipv6 = host.starts_with('[') && host.ends_with(']');
    if (ipv6) {
        host = host.substr(1, host.size() - 2);
    }
    std::array<unsigned char, 16> bytes{};
    const auto family = ipv6 ? AF_INET6 : AF_INET;
    if (host.find('\0') != std::string_view::npos || inet_pton(family, std::string(host).c_str(), bytes.data()) != 1) {
        return std::unexpected(Error{ErrorCode::configuration, "Endpoint requires a numeric IP without scope"});
    }

    // 规范化后再比较身份, 拒绝 IPv4-mapped IPv6, 避免相同地址拥有不同部署指纹.
    const auto count = ipv6 ? 16U : 4U;
    const bool wildcard = std::all_of(bytes.begin(), bytes.begin() + count, [](auto b) { return b == 0; });
    const bool multicast = ipv6 ? bytes[0] == 255 : (bytes[0] >= 224 && bytes[0] <= 239);
    const bool mapped = ipv6 && std::all_of(bytes.begin(), bytes.begin() + 10, [](auto b) { return b == 0; }) && bytes[10] == 255 && bytes[11] == 255;
    if (multicast || mapped || (!local && wildcard)) {
        return std::unexpected(Error{ErrorCode::configuration, "Endpoint is not a supported unicast address"});
    }
    std::array<char, INET6_ADDRSTRLEN> normalized{};
    if (!inet_ntop(family, bytes.data(), normalized.data(), static_cast<socklen_t>(normalized.size()))) {
        return std::unexpected(Error{ErrorCode::internal, "Address normalization failed"});
    }
    return Endpoint{normalized.data(), *port, ipv6, wildcard};
}

std::string Endpoint::text() const {
    return (ipv6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

bool valid_name(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

bool valid_uuid(const PeerId& id) {
    return (id.bytes[6] & 0xf0) == 0x40 && (id.bytes[8] & 0xc0) == 0x80;
}

Result<std::string> supervisor_address(std::string_view value) {
    if (auto endpoint = Endpoint::parse(value)) {
        return endpoint->text();
    }
    const auto separator = value.rfind(':');
    if (separator == std::string_view::npos || !port_number(value.substr(separator + 1), false)) {
        return std::unexpected(Error{ErrorCode::configuration, "Supervisor requires HOST:PORT"});
    }
    const auto host = value.substr(0, separator);
    std::array<unsigned char, 4> numeric{};
    if (inet_pton(AF_INET, std::string(host).c_str(), numeric.data()) == 1) {
        return std::unexpected(Error{ErrorCode::configuration, "Invalid numeric Supervisor endpoint"});
    }
    if (host.empty() || host.size() > 253) {
        return std::unexpected(Error{ErrorCode::configuration, "Invalid supervisor hostname"});
    }
    for (auto label : host | std::views::split('.')) {
        const std::string_view part(label.begin(), label.end());
        if (part.empty() || part.size() > 63 || part.starts_with('-') || part.ends_with('-') || !std::ranges::all_of(part, [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
            })) {
            return std::unexpected(Error{ErrorCode::configuration, "Invalid supervisor hostname"});
        }
    }
    return std::string(value);
}

Result<void> validate_member(const Member& member) {
    if (!valid_name(member.cluster) || !valid_name(member.group) || !valid_uuid(member.id) || member.epoch.value == 0 ||
        (member.role != Role::star && member.role != Role::planet)) {
        return std::unexpected(Error{ErrorCode::identity, "Invalid member identity"});
    }
    auto endpoint = Endpoint::parse(member.address.text());
    if (!endpoint || *endpoint != member.address) {
        return std::unexpected(Error{ErrorCode::identity, "Invalid canonical member endpoint"});
    }
    return {};
}
} // namespace verdandi::peer
