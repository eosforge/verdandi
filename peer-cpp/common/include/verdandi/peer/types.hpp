// 许可证: MIT, 详见仓库根目录 LICENSE.
#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace verdandi::peer {

// 有限错误类别用于重试和诊断, message 只能由本地固定文本构造, 不包含远端正文或秘密.
enum class ErrorCode { configuration, identity, protocol, conflict, capacity, timeout, transport, cancelled, internal };
// 稳定诊断词只由这个白名单生成, 不输出远端错误正文.
std::string_view error_name(ErrorCode code);
struct Error {
    ErrorCode code;
    std::string message;
};
template <class T> using Result = std::expected<T, Error>;

enum class Role { star, planet };
enum class Direction { outbound, inbound };
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

// 两种固定长度标识拥有自己的字节, 不借用 Protobuf 或临时解析缓冲.
template <std::size_t N> struct HexId {
    std::array<std::uint8_t, N> bytes{};
    auto operator<=>(const HexId&) const = default;

    // 仅接受定长小写十六进制; 先检查长度, 再进行索引, 失败不返回部分身份.
    static Result<HexId> parse(std::string_view value) {
        if (value.size() != N * 2) {
            return std::unexpected(Error{ErrorCode::identity, "Invalid identity length"});
        }
        HexId result;
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto c = value[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return std::unexpected(Error{ErrorCode::identity, "Identity must use lowercase hexadecimal"});
            }
            const auto nibble = static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
            result.bytes[i / 2] = static_cast<std::uint8_t>((static_cast<unsigned>(result.bytes[i / 2]) << 4) | nibble);
        }
        return result;
    }

    // 返回拥有的编码字符串, 可安全跨越回调边界.
    std::string text() const {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string result(N * 2, '0');
        for (std::size_t i = 0; i < N; ++i) {
            result[i * 2] = digits[bytes[i] >> 4];
            result[i * 2 + 1] = digits[bytes[i] & 15];
        }
        return result;
    }
};
using PeerId = HexId<16>;
using Principal = HexId<32>;

// 成员代次由 Supervisor 签发, 会话代次由本进程分配, 类型分开以阻止误用.
struct MemberEpoch {
    std::uint64_t value{};
    auto operator<=>(const MemberEpoch&) const = default;
};
struct SessionGeneration {
    std::uint64_t value{};
    auto operator<=>(const SessionGeneration&) const = default;
};

// 规范端点只包含数值 IP 和端口. host 不带 IPv6 方括号, text() 添加协议要求的括号.
struct Endpoint {
    std::string host;
    std::uint16_t port{};
    bool ipv6{};
    bool wildcard{};
    auto operator<=>(const Endpoint&) const = default;

    // local=true 允许通配地址和零端口, 不允许多播、映射 IPv6 或 scope 别名. 不执行 DNS.
    static Result<Endpoint> parse(std::string_view value, bool local = false);
    std::string text() const;
};

// 角色状态持有这个独立值, 不持有生成消息的引用或密码材料.
struct Member {
    std::string cluster;
    PeerId id;
    Principal principal;
    Endpoint address;
    MemberEpoch epoch;
    Role role{};
    std::string group;
    auto operator<=>(const Member&) const = default;
};

// 名称采用与 Go/Rust 一致的 ASCII 范围, 不做 Unicode 或大小写归一化.
bool valid_name(std::string_view value);
// UUID 版本和 variant 校验与长度解析分开, 对已经固定大小的字节读取不会越界.
bool valid_uuid(const PeerId& id);
// Supervisor 允许 DNS 主机, 只校验名称/端口格式, 返回规范拨号文本.
Result<std::string> supervisor_address(std::string_view value);
// 验证完整成员, 所有调用者必须在安装共享状态之前检查结果.
Result<void> validate_member(const Member& member);

} // namespace verdandi::peer
