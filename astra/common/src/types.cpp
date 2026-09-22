// 详细说明: 这个实现文件处理 `Principal` 对象的解析和文本化, 错误码转换以及 `Endpoint` 终端地址(IPv4 / IPv6
// 和端口号)的解析和验证, 提供统一且规范的基础类型功能.
#include <astra/config.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <charconv>
#include <ranges>

namespace astra {

// Principal::parse 方法实现
// - value (std::string_view): 预期的 16 进制全小写字符串形式的 principal 摘要文本.
// 返回值: 成功返回 Principal 结构(内部存储原始字节), 失败返回对应错误.
Result<Principal> Principal::parse(std::string_view value) {

    // result 初始全零, 每两个有效十六进制字符形成一个字节, 失败不返回部分摘要.
    Principal result;
    // 字符串的长度必须正好是字节数的两倍.
    if (value.size() != result.bytes.size() * 2) {
        return Status::identity("Invalid principal length");
    }

    // 先验证长度, 再逐字节解析; 不接受大写或其他编码别名, 失败不会暴露部分摘要.
    // 详细说明: 要求输入严格是由 0-9 或是 a-f 构成的全小写 16 进制字符串, 避免出现编码不一致引发的安全问题或逻辑错误.
    for (std::size_t i = 0; i < value.size(); ++i) {
        // c 是当前十六进制字符, i 从零覆盖全部输入, 仅接受明确白名单.
        const auto c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return Status::identity("Principal must use lowercase hexadecimal");
        }

        // 算出单个 nibble (半字节), 将前一个算出的 nibble 左移 4 位后再按位或.
        const auto nibble = static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
        result.bytes[i / 2] = static_cast<std::uint8_t>((static_cast<unsigned>(result.bytes[i / 2]) << 4) | nibble);
    }
    return result;
}

// Principal::text 方法实现
// 返回值: Principal 字节内容对应的全小写 16 进制字符串.
std::string Principal::text() const {

    // digits 为小写十六进制查表, 索引只取字节的高或低四位.
    constexpr std::string_view digits = "0123456789abcdef";
    // result 一次分配完整 64 字符编码, 每个位置由摘要字节覆盖.
    std::string result(bytes.size() * 2, '0');
    // 把每一个字节转回两个小写十六进制字符.
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        result[i * 2] = digits[bytes[i] >> 4];     // 高四位
        result[i * 2 + 1] = digits[bytes[i] & 15]; // 低四位
    }
    return result;
}

// error_name 方法实现
// - code (Status::Code): 错误代码枚举值.
// 返回值: 返回对应错误的描述字面量, 用于结构化日志输出等, 保证不含有非法或不合规的字符串.
std::string_view Status::name(Code code) {

    switch (code) {
    case Status::Code::configuration:
        return "configuration";
    case Status::Code::identity:
        return "identity";
    case Status::Code::protocol:
        return "protocol";
    case Status::Code::conflict:
        return "conflict";
    case Status::Code::capacity:
        return "capacity";
    case Status::Code::timeout:
        return "timeout";
    case Status::Code::transport:
        return "transport";
    case Status::Code::cancelled:
        return "cancelled";
    case Status::Code::internal:
        return "internal";
    }
    return "internal"; // 兜底返回 internal.
}

namespace {
// 端口解析必须消费所有字符; 只有 listener 允许零, 正负号和溢出均显式拒绝.
// - value (std::string_view): 纯端口字符串, 如 "8080".
// - local (bool): true 表示这是一个本地监听端口, 允许绑定 "0" 端口(系统随机分配); false 表示远程连接端点, 不允许端口为 0.
// 返回值: 成功返回数字端口 (std::uint16_t), 失败返回错误.
Result<std::uint16_t> port_number(std::string_view value, bool local) {

    // port 用较宽无符号类型接收数字, 通过 16 位范围和零端口检查后才收窄.
    unsigned port = 0;
    // 使用 std::from_chars 对传入字符串严格执行数值转换, 要求读完所有字符不留多余(如空格, 字母等).
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
    // 判断是否出错, 是否没有读到字符串结尾, 是否超出16位整型的极限, 是否不合法地使用了端口 0.
    if (error != std::errc{} || end != value.data() + value.size() || port > 65535 || (!local && port == 0)) {
        return Status::configuration("Invalid endpoint port");
    }
    return static_cast<std::uint16_t>(port);
}
} // namespace

// - value (std::string_view): 需要解析的网络地址字符串 (例如 "127.0.0.1:80", "[::1]:8080").
// - local (bool): (默认值: false) 决定端口验证时是否允许 "0", 以及决定能否接受通配符.
// 返回值: 成功返回 Endpoint 结构, 失败返回错误.
Result<Endpoint> Endpoint::parse(std::string_view value, bool local) {

    // 逆向查找 ':' 定位端口和主机的分隔点.
    const auto separator = value.rfind(':');
    if (separator == std::string_view::npos) {
        return Status::configuration("Endpoint requires IP:PORT");
    }

    // 取冒号后边的字符串进行端口解析.
    auto port = port_number(value.substr(separator + 1), local);
    if (!port) {
        return std::unexpected(port.error());
    }

    // 取冒号前面的字符串作为主机部分.
    auto host = value.substr(0, separator);
    // 检查是否是被方括号包住的 IPv6 地址格式.
    const bool ipv6 = host.starts_with('[') && host.ends_with(']');
    if (ipv6) {
        host = host.substr(1, host.size() - 2); // 掐头去尾
    }
    std::array<unsigned char, 16> bytes{}; // 容纳 IPv4(4字节) 或 IPv6(16字节) 的空间
    // family 与剥除方括号后的地址类型一致, 决定 inet_pton/inet_ntop 的读写宽度.
    const auto family = ipv6 ? AF_INET6 : AF_INET;
    // 禁止内部出现 '\0' 并利用系统函数 inet_pton 执行实际验证和二进制提取
    if (host.find('\0') != std::string_view::npos || inet_pton(family, std::string(host).c_str(), bytes.data()) != 1) {
        return Status::configuration("Endpoint requires a numeric IP without scope");
    }

    // 规范化后再比较身份, 拒绝 IPv4-mapped IPv6, 避免相同地址拥有不同部署指纹.
    // 详细说明: 防止因为同一种网络地址由于字符串字面量表达不同而产生安全隐患, 或者绕过指纹比对.
    const auto count = ipv6 ? 16U : 4U;
    // 判断是否全部比特都是 0, 也就是相当于 0.0.0.0 (IPv4) 或 :: (IPv6).通配符.
    const bool wildcard = std::all_of(bytes.begin(), bytes.begin() + count, [](auto b) { return b == 0; });
    // 判断是否为组播地址.
    const bool multicast = ipv6 ? bytes[0] == 255 : (bytes[0] >= 224 && bytes[0] <= 239);
    // 判断是否为 IPv4 映射为 IPv6 的地址格式.
    const bool mapped = ipv6 && std::all_of(bytes.begin(), bytes.begin() + 10, [](auto b) { return b == 0; }) && bytes[10] == 255 && bytes[11] == 255;

    // 我们不支持组播, 不支持映射地址.如果不是本地监听用途, 也不支持直接向通配符发起连接.
    if (multicast || mapped || (!local && wildcard)) {
        return Status::configuration("Endpoint is not a supported unicast address");
    }

    // 将其重新序列化回标准的字符串格式, 消除原来任何多余的前导零或其他小问题.
    std::array<char, INET6_ADDRSTRLEN> normalized{};
    if (!inet_ntop(family, bytes.data(), normalized.data(), static_cast<socklen_t>(normalized.size()))) {
        return Status::internal("Address normalization failed");
    }
    return Endpoint{normalized.data(), *port, ipv6, wildcard};
}

// 返回值: 规范化的带有端口拼接的文本形式(IPv6 自动加方括号).
std::string Endpoint::text() const {
    return (ipv6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

// valid_name 方法实现
// - value (std::string_view): 需要验证的名称字符串.
// 返回值: 布尔值, 合法与否.
// 详细说明: 校验传入的名称不能空, 不能超过 64 个字符, 并且只能包含大小写英文字母, 数字, 点号('.'), 下划线('_') 以及横杠('-').
bool Member::valid_name(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; });
}

// valid_id 方法实现
// 详细说明: 校验传入 ID 非空, 且字符数不得超过 128.
bool Member::valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 128;
}

// supervisor_address 方法实现
// - value (std::string_view): 可能是 IP:PORT 或 HOSTNAME:PORT 的字符串.

// validate_member 方法实现
// - member (const Member&): 准备接受验证的成员结构.
// 返回值: 成功返回空, 失败则包含错误原因.
// 详细说明: 在一个成员从非确定的配置态或远端字节态进入到正式活跃角色状态之前, 必须通过此函数对其内各项关键字段做全局一致性与合法性边界扫描.
Result<void> Member::validate() const {

    // 检查集群名, 组名, ID 的通用规范.
    // epoch 的值必须大于 0.
    // 角色必须是明确支持的基础设施角色, 未知枚举值不默认降为 Star.
    if (!Member::valid_name(galaxy) || !Member::valid_name(group) || !Member::valid_id(id) || epoch.value == 0 || (role != Member::Role::star && role != Member::Role::planet && role != Member::Role::polaris && role != Member::Role::astrolabe)) {
        return Status::identity("Invalid member identity");
    }

    // 进一步保证里面挂载的网络地址确实是个强合法的 Endpoint 对象并且能够被无损重放.
    auto endpoint = Endpoint::parse(address.text());
    if (!endpoint || *endpoint != address) {
        return Status::identity("Invalid canonical member endpoint");
    }
    return {};
}

// 返回值: 解析通过并进行标准化处理后的字符串, 或者错误信息.
Result<std::string> Config::format_supervisor(std::string_view value) {

    // 尝试先按数字端点(IP:PORT)去解析
    if (auto endpoint = Endpoint::parse(value)) {
        return endpoint->text();
    }

    // 若不是 IP 格式, 按 HOSTNAME:PORT 解析.
    const auto separator = value.rfind(':');
    if (separator == std::string_view::npos || !port_number(value.substr(separator + 1), false)) {
        return Status::configuration("Supervisor requires HOST:PORT");
    }

    // host 借用最后一个冒号之前的主机部分, 长度与 DNS 标签随后分别校验.
    const auto host = value.substr(0, separator);
    // numeric 仅用于识别被数字端点解析拒绝的 IPv4, 避免将非法数值地址误当 DNS.
    std::array<unsigned char, 4> numeric{};
    // 为了防止部分 inet_pton 或域名解析 API 遇到伪装为非规范 IP 的边缘情况, 再拦一道.
    if (inet_pton(AF_INET, std::string(host).c_str(), numeric.data()) == 1) {
        return Status::configuration("Invalid numeric Supervisor endpoint");
    }

    // DNS 名字整体长度限制.
    if (host.empty() || host.size() > 253) {
        return Status::configuration("Invalid supervisor hostname");
    }

    // 检查每一段 label 的合法性: 不能超长, 头尾不能是横杠, 字符需符合规范.
    for (auto label : host | std::views::split('.')) {
        const std::string_view part(label.begin(), label.end());
        if (part.empty() || part.size() > 63 || part.starts_with('-') || part.ends_with('-') || !std::ranges::all_of(part, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; })) {
            return Status::configuration("Invalid supervisor hostname");
        }
    }
    return std::string(value);
}

} // namespace astra
