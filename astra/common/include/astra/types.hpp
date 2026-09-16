// 功能: 定义传输无关的身份, 端点, 成员和错误类型, 提供输入校验与规范编码接口.
// 本文件包含所有跨核心组件使用的基础类型定义，如错误代码、角色、方向、端点、成员等，
// 为整个系统提供了强类型的基础设施。
#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace astra {


// 稳定诊断词只由这个白名单生成, 不输出远端错误正文.
// code 映射为静态存储的诊断词, 未识别枚举值回退 internal; 返回视图不依赖调用对象的生命周期.
// 参数 code: 需要被转换为字符串表示的 Error::Code 错误码。
// 返回: 对应错误码的静态字符串视图。

struct Error {
    // 错误代码的枚举定义，已完全收敛到 Error 作用域内
    enum class Code {
    // configuration: 本地参数缺失, 格式错误或超出允许范围, 应修正配置后再启动.
    // 代表配置项存在逻辑或语法错误。
    configuration,
    // identity: 身份材料, TLS 身份或准入凭证未通过验证, 不应按普通网络故障盲目重试.
    // 涉及到安全认证和授权失败的错误。
    identity,
    // protocol: 协议版本, 报文类型或字段组合不符合约定, Planet 会隔离对应候选.
    // 通信过程中协议不匹配或数据包结构错误。
    protocol,
    // conflict: 成员代次, 身份绑定或同方向会话发生冲突, 不能覆盖当前有效状态.
    // 状态机冲突，如并发会话试图覆写同一槽位。
    conflict,
    // capacity: 成员, 消息或队列达到容量边界; 是否重试由调用阶段和角色策略决定.
    // 系统资源或队列达到设计上限。
    capacity,
    // timeout: 连接, 握手或心跳超过单调时钟截止, 关闭本次尝试并进入相应退避.
    // 各种基于超时的失败，需要退避后重试。
    timeout,
    // transport: 传输断开或暂时不可达, 未表明身份或协议本身无效.
    // 底层网络或传输层发生错误。
    transport,
    // cancelled: 操作被本地或远端取消, 仍须等待异步完成才能释放 RPC 资源.
    // 用户或系统主动取消了某个正在进行的操作。
    cancelled,
    // internal: 本地内部资源或运行条件异常, 使用固定诊断文本报告失败.
    // 其他未分类的内部逻辑异常或系统错误。
    internal
};

    // 发生什么类别的错误
    Code code;
    // 错误的详细描述信息，必须是本地生成的安全文本
    std::string message;

    static auto configuration(std::string msg) { return std::unexpected(Error{Code::configuration, std::move(msg)}); }
    static auto identity(std::string msg) { return std::unexpected(Error{Code::identity, std::move(msg)}); }
    static auto protocol(std::string msg) { return std::unexpected(Error{Code::protocol, std::move(msg)}); }
    static auto conflict(std::string msg) { return std::unexpected(Error{Code::conflict, std::move(msg)}); }
    static auto capacity(std::string msg) { return std::unexpected(Error{Code::capacity, std::move(msg)}); }
    static auto timeout(std::string msg) { return std::unexpected(Error{Code::timeout, std::move(msg)}); }
    static auto transport(std::string msg) { return std::unexpected(Error{Code::transport, std::move(msg)}); }
    static auto cancelled(std::string msg) { return std::unexpected(Error{Code::cancelled, std::move(msg)}); }
    static auto internal(std::string msg) { return std::unexpected(Error{Code::internal, std::move(msg)}); }

    static std::string_view name(Code code);
};

// Result 模板别名，用于替代传统的异常处理或裸指针返回，表示操作可能成功返回类型 T，或者失败返回 Error。
template <class T> using Result = std::expected<T, Error>;

// 节点运行角色, 由二进制入口指定, 与网络消息的角色枚举显式转换.
// 决定了节点在系统拓扑中的行为。
enum class Role {
    // star: 维护 Star 拓扑并接受 Star 和 Planet 入站连接的节点.
    // 核心节点，构成主干网络。
    star,
    // planet: 从授权 Star 候选中维持一个已完成握手的出站上游的节点.
    // 边缘节点，依附于 Star 节点。
    planet
};

// 相对于本进程的连接方向; 元素顺序同时对应 Star 会话数组下标.
// 区分网络连接是主动发起还是被动接收。
enum class Direction {
    // outbound: 本进程主动拨号建立的连接, 对应会话槽位 0.
    // 我方作为客户端主动连接他方。
    outbound,
    // inbound: 远端主动连接本进程监听器的连接, 对应会话槽位 1.
    // 他方作为客户端主动连接我方监听端口。
    inbound
};

// 定义稳定的时间类型
// Clock: 使用单调时钟以保证时间的单向递增，不受系统时间修改影响。
using Clock = std::chrono::steady_clock;
// Milliseconds: 毫秒级别的时间间隔，广泛用于超时配置。
using Milliseconds = std::chrono::milliseconds;

// Supervisor 签发的不透明字符串, 只比较原值, 不解释编码格式或版本位.
// 用于全局唯一标识一个节点或实体的 ID。
using Id = std::string;

// 账号, Galaxy 和规范端点的 SHA-256 部署摘要, 用于识别同部署的新旧实例, 不是进程 ID.
// 包含一个 32 字节（256位）的哈希值。
struct Principal {
    // bytes: 存放 SHA-256 哈希结果的字节数组，默认初始化为全零。
    std::array<std::uint8_t, 32> bytes{};
    
    // 按拥有的摘要字节比较, 不执行身份授权.
    // 自动生成的比较运算符，用于比较两个 Principal 是否完全相同。
    auto operator<=>(const Principal&) const = default;
    
    // 解析字符串形式的凭证，转化为内部表示。
    // 借用 64 字符小写十六进制文本, 成功返回独立摘要; 长度或字符非法返回 identity 错误。
    // 参数 value: 64字符十六进制文本视图。
    static Result<Principal> parse(std::string_view value);
    
    // 格式化为字符串输出。
    // 返回拥有的 64 字符编码, 不借用原始 Protobuf 或解析缓冲.
    // 返回: 64位十六进制字符串。
    std::string text() const;
};

// 成员代次由 Supervisor 签发, 会话代次由本进程分配, 类型分开以阻止误用.
// 用于在分布式环境下识别数据的时序和新鲜度。
struct MemberEpoch {
    // value: 代表代次的 64 位无符号整数，默认值为 0。
    std::uint64_t value{};
    
    // 只比较 Supervisor 签发的成员代次, 不与本地会话编号混用.
    // 自动生成的比较运算符。
    auto operator<=>(const MemberEpoch&) const = default;
};

// 会话代次，由本进程本地分配，保证在单一进程生命周期内唯一。
struct SessionGeneration {
    // value: 代表会话唯一编号的 64 位无符号整数，默认值为 0。
    std::uint64_t value{};
    
    // 只比较本进程分配的会话编号, 用于精确关联关闭回调.
    // 自动生成的比较运算符。
    auto operator<=>(const SessionGeneration&) const = default;
};

// 规范端点只包含数值 IP 和端口. host 不带 IPv6 方括号, text() 添加协议要求的括号.
// 表示网络中的一个访问点。
struct Endpoint {
    // host: 节点的 IP 地址（数值表示，例如 "127.0.0.1" 或 "::1"）。
    std::string host;
    // port: 节点的访问端口号，默认为 0。
    std::uint16_t port{};
    // ipv6: 布尔标志位，标示 host 是否是一个 IPv6 地址，默认为 false。
    bool ipv6{};
    // wildcard: 布尔标志位，标示这是否是一个通配符地址（如 0.0.0.0 或 ::），默认为 false。
    bool wildcard{};
    
    // 按规范字段比较端点, 调用方须先 parse, 本操作不消除地址别名.
    // 自动生成的比较运算符。
    auto operator<=>(const Endpoint&) const = default;

    // 解析一个给定的字符串为端点结构。
    // local=true 允许通配地址和零端口, 不允许多播, 映射 IPv6 或 scope 别名. 不执行 DNS.
    // value 仅在调用期间借用; 成功返回拥有的规范端点, 格式或地址不支持返回 configuration, 规范化失败返回 internal.
    // 参数 value: 要解析的地址字符串（如 "127.0.0.1:8080"）。
    // 参数 local: 是否视为本地监听地址进行放宽检查，默认 false。
    static Result<Endpoint> parse(std::string_view value, bool local = false);
    
    // 序列化回规范的文本格式。
    // 返回独立 IP:PORT 文本, IPv6 自动补方括号; 不重新校验手工构造的字段.
    // 返回: 诸如 "127.0.0.1:8080" 或 "[::1]:80" 的标准地址字符串。
    std::string text() const;
};

// 角色状态持有这个独立值, 不持有生成消息的引用或密码材料.
// 描述系统中一个成员的完整拓扑信息。
struct Member {
    // galaxy: 该成员所属的逻辑集群（Galaxy）标识。
    std::string galaxy;
    // id: 成员在集群中的全局唯一 ID。
    Id id;
    // principal: 部署相关的安全凭据指纹。
    Principal principal;
    // address: 该成员对外公布的网络访问端点。
    Endpoint address;
    // epoch: 成员的当前状态代次，用于识别数据新鲜度。
    MemberEpoch epoch;
    // role: 成员在系统中所扮演的角色 (Star 或 Planet)，默认为 star。
    Role role{};
    // group: 成员所在的连接偏好分组名称，用于 Planet 选择上游。
    std::string group;
    
    // 逐字段比较完整成员值; 相等用于同代次一致性判断, 不替代凭证验签.
    // 自动生成的比较运算符，用于检测两个成员对象是否在所有字段上一致。
    auto operator<=>(const Member&) const = default;

    // 验证一个 Member 对象中所有字段是否均合法，是接入逻辑前的必经步骤。
    // 验证完整成员, 所有调用者必须在安装共享状态之前检查结果.
    // member 须含有效名称, 不透明 ID, 非零 epoch 和规范可达端点; 失败返回 identity, 不修改传入值或执行验签.
    // 返回: 成功返回 void 的 Result，失败返回 Error 附带对应错误码。
    Result<void> validate() const;

    // 检查传入字符串是否符合标识符名称的标准要求。
    // 名称采用与 Go/Rust 一致的 ASCII 范围, 不做 Unicode 或大小写归一化.
    // value 长度必须为 1..64 字节且仅含字母, 数字, 点, 下划线和连字符; 空串或越界返回 false.
    // 参数 value: 待校验的名称字符串。
    // 返回: true 表示合法，false 表示非法。
    static bool valid_name(std::string_view value);

    // 检查传入的标识符是否是一个有效的 ID。
    // 仅检查 ID 非空和 128 字节传输上限; UTF-8 由 Protobuf 解码验证, 签发真实性由 Identity 验签.
    // 参数 id: 待校验的唯一标识符。
    // 返回: true 表示格式合法，false 表示非法。
    static bool valid_id(std::string_view id);
};

} // namespace astra
