// 功能: 定义传输无关的身份, 端点, 成员和错误类型, 提供输入校验与规范编码接口.
#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace verdandi::cluster {

// 有限错误类别用于重试和诊断, message 只能由本地固定文本构造, 不包含远端正文或秘密.
enum class ErrorCode {
    // 本地参数缺失, 格式错误或超出允许范围, 应修正配置后再启动.
    configuration,
    // 身份材料, TLS 身份或准入凭证未通过验证, 不应按普通网络故障盲目重试.
    identity,
    // 协议版本, 报文类型或字段组合不符合约定, Planet 会隔离对应候选.
    protocol,
    // 成员代次, 身份绑定或同方向会话发生冲突, 不能覆盖当前有效状态.
    conflict,
    // 成员, 消息或队列达到容量边界; 是否重试由调用阶段和角色策略决定.
    capacity,
    // 连接, 握手或心跳超过单调时钟截止, 关闭本次尝试并进入相应退避.
    timeout,
    // 传输断开或暂时不可达, 未表明身份或协议本身无效.
    transport,
    // 操作被本地或远端取消, 仍须等待异步完成才能释放 RPC 资源.
    cancelled,
    // 本地内部资源或运行条件异常, 使用固定诊断文本报告失败.
    internal
};
// 稳定诊断词只由这个白名单生成, 不输出远端错误正文.
// code 映射为静态存储的诊断词, 未识别枚举值回退 internal; 返回视图不依赖调用对象的生命周期.
std::string_view error_name(ErrorCode code);
struct Error {
    ErrorCode code;
    std::string message;
};
template <class T> using Result = std::expected<T, Error>;

// 节点运行角色, 由二进制入口指定, 与网络消息的角色枚举显式转换.
enum class Role {
    // 维护 Star 拓扑并接受 Star 和 Planet 入站连接的节点.
    star,
    // 从授权 Star 候选中维持一个已完成握手的出站上游的节点.
    planet
};
// 相对于本进程的连接方向; 元素顺序同时对应 Star 会话数组下标.
enum class Direction {
    // 本进程主动拨号建立的连接, 对应会话槽位 0.
    outbound,
    // 远端主动连接本进程监听器的连接, 对应会话槽位 1.
    inbound
};
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

// Supervisor 签发的不透明字符串, 只比较原值, 不解释编码格式或版本位.
using Id = std::string;

// 账号, Galaxy 和规范端点的 SHA-256 部署摘要, 用于识别同部署的新旧实例, 不是进程 ID.
struct Principal {
    std::array<std::uint8_t, 32> bytes{};
    // 按拥有的摘要字节比较, 不执行身份授权.
    auto operator<=>(const Principal&) const = default;
    // 借用 64 字符小写十六进制文本, 成功返回独立摘要; 长度或字符非法返回 identity.
    static Result<Principal> parse(std::string_view value);
    // 返回拥有的 64 字符编码, 不借用原始 Protobuf 或解析缓冲.
    std::string text() const;
};

// 成员代次由 Supervisor 签发, 会话代次由本进程分配, 类型分开以阻止误用.
struct MemberEpoch {
    std::uint64_t value{};
    // 只比较 Supervisor 签发的成员代次, 不与本地会话编号混用.
    auto operator<=>(const MemberEpoch&) const = default;
};
struct SessionGeneration {
    std::uint64_t value{};
    // 只比较本进程分配的会话编号, 用于精确关联关闭回调.
    auto operator<=>(const SessionGeneration&) const = default;
};

// 规范端点只包含数值 IP 和端口. host 不带 IPv6 方括号, text() 添加协议要求的括号.
struct Endpoint {
    std::string host;
    std::uint16_t port{};
    bool ipv6{};
    bool wildcard{};
    // 按规范字段比较端点, 调用方须先 parse, 本操作不消除地址别名.
    auto operator<=>(const Endpoint&) const = default;

    // local=true 允许通配地址和零端口, 不允许多播, 映射 IPv6 或 scope 别名. 不执行 DNS.
    // value 仅在调用期间借用; 成功返回拥有的规范端点, 格式或地址不支持返回 configuration, 规范化失败返回 internal.
    static Result<Endpoint> parse(std::string_view value, bool local = false);
    // 返回独立 IP:PORT 文本, IPv6 自动补方括号; 不重新校验手工构造的字段.
    std::string text() const;
};

// 角色状态持有这个独立值, 不持有生成消息的引用或密码材料.
struct Member {
    std::string cluster;
    Id id;
    Principal principal;
    Endpoint address;
    MemberEpoch epoch;
    Role role{};
    std::string group;
    // 逐字段比较完整成员值; 相等用于同代次一致性判断, 不替代凭证验签.
    auto operator<=>(const Member&) const = default;
};

// 名称采用与 Go/Rust 一致的 ASCII 范围, 不做 Unicode 或大小写归一化.
// value 长度必须为 1..64 字节且仅含字母, 数字, 点, 下划线和连字符; 空串或越界返回 false.
bool valid_name(std::string_view value);
// 仅检查 ID 非空和 128 字节传输上限; UTF-8 由 Protobuf 解码验证, 签发真实性由 Identity 验签.
bool valid_id(std::string_view id);
// Supervisor 允许 DNS 主机, 只校验名称/端口格式, 返回规范拨号文本.
// value 在调用期间借用, 非零端口及主机格式不合法返回 configuration; DNS 主机文本保留原大小写.
Result<std::string> supervisor_address(std::string_view value);
// 验证完整成员, 所有调用者必须在安装共享状态之前检查结果.
// member 须含有效名称, 不透明 ID, 非零 epoch 和规范可达端点; 失败返回 identity, 不修改传入值或执行验签.
Result<void> validate_member(const Member& member);

} // namespace verdandi::cluster
