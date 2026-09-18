// 本文件包含所有跨核心组件使用的基础类型定义, 如错误代码, 角色, 方向, 端点, 成员等,
// 为整个系统提供了强类型的基础设施.
#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace astra {

// 失败状态及其固定分类, Result 的成功状态由 std::expected 表示.
struct Status {
    enum class Code {
        // configuration: 本地参数缺失, 格式错误或超出允许范围, 应修正配置后再启动.
        configuration,
        // identity: 身份材料, TLS 身份或准入凭证未通过验证, 不应按普通网络故障盲目重试.
        identity,
        // protocol: 协议版本, 报文类型或字段组合不符合约定, Planet 会隔离对应候选.
        // 通信过程中协议不匹配或数据包结构错误.
        protocol,
        // conflict: 成员代次, 身份绑定或同方向会话发生冲突, 不能覆盖当前有效状态.
        conflict,
        // capacity: 成员, 消息或队列达到容量边界; 是否重试由调用阶段和角色策略决定.
        capacity,
        // timeout: 连接, 握手或心跳超过单调时钟截止, 关闭本次尝试并进入相应退避.
        timeout,
        // transport: 传输断开或暂时不可达, 未表明身份或协议本身无效.
        transport,
        // cancelled: 操作被本地或远端取消, 仍须等待异步完成才能释放 RPC 资源.
        cancelled,
        // internal: 本地内部资源或运行条件异常, 使用固定诊断文本报告失败.
        internal
    };

    // 必须由失败创建者显式选择分类, 不将默认零值当成成功.
    Code code;
    // 拥有的内部诊断文本, 可为空; 对外日志仍使用 name() 的固定分类.
    std::string message;

    // 创建配置错误结果; message 为拥有的诊断文本, 移入结果, 允许空串且不包含秘密材料.
    static auto configuration(std::string message) {
        return std::unexpected(Status{Code::configuration, std::move(message)});
    }

    // 创建身份错误结果; message 为拥有的诊断文本, 移入结果, 允许空串且不包含秘密材料.
    static auto identity(std::string message) {
        return std::unexpected(Status{Code::identity, std::move(message)});
    }

    // 创建协议错误结果; message 为拥有的诊断文本, 移入结果, 允许空串且不包含秘密材料.
    static auto protocol(std::string message) {
        return std::unexpected(Status{Code::protocol, std::move(message)});
    }

    // 创建状态冲突结果; message 为拥有的诊断文本, 移入结果, 允许空串且不包含秘密材料.
    static auto conflict(std::string message) {
        return std::unexpected(Status{Code::conflict, std::move(message)});
    }

    // 创建容量不足结果; message 为拥有的诊断文本, 移入结果, 允许空串且不包含秘密材料.
    static auto capacity(std::string message) {
        return std::unexpected(Status{Code::capacity, std::move(message)});
    }

    // 创建超时结果; message 为拥有的诊断文本, 移入结果, 允许空串且不包含秘密材料.
    static auto timeout(std::string message) {
        return std::unexpected(Status{Code::timeout, std::move(message)});
    }

    // 创建传输失败结果; message 为拥有的诊断文本, 移入结果, 允许空串且不包含秘密材料.
    static auto transport(std::string message) {
        return std::unexpected(Status{Code::transport, std::move(message)});
    }

    // 创建取消结果; message 为拥有的诊断文本, 移入结果, 允许空串且不包含秘密材料.
    static auto cancelled(std::string message) {
        return std::unexpected(Status{Code::cancelled, std::move(message)});
    }

    // 创建内部错误结果; message 为拥有的诊断文本, 移入结果, 允许空串且不包含秘密材料.
    static auto internal(std::string message) {
        return std::unexpected(Status{Code::internal, std::move(message)});
    }

    // 稳定诊断词只由白名单生成, 不输出远端错误正文.
    // 未识别枚举值回退 internal; 返回视图不依赖调用对象的生命周期.
    static std::string_view name(Code code);
};

// T 为成功值类型, void 表示无结果载荷; 失败独立持有 Status, 不借用远端消息.
template <class T>
using Result = std::expected<T, Status>;

// 会话, 退避与历史保留使用本地单调时间. 业务绝对期限见 clock.hpp 的 Clock.
using Steady = std::chrono::steady_clock;
// Milliseconds: 毫秒级别的时间间隔, 广泛用于超时配置.
using Milliseconds = std::chrono::milliseconds;

// Supervisor 签发的不透明字符串, 只比较原值, 不解释编码格式或版本位.
// 用于全局唯一标识一个节点或实体的 ID.
using Id = std::string;

// 账号, Galaxy 和规范端点的 SHA-256 部署摘要, 用于识别同部署的新旧实例, 不是进程 ID.
// 包含一个 32 字节(256位)的哈希值.
struct Principal {
    // bytes: 存放 SHA-256 哈希结果的字节数组, 默认初始化为全零.
    std::array<std::uint8_t, 32> bytes{};

    // 按拥有的摘要字节比较, 不执行身份授权.
    auto operator<=>(const Principal&) const = default;

    // 参数 value: 64字符十六进制文本视图.
    static Result<Principal> parse(std::string_view value);

    // 返回拥有的 64 字符编码, 不借用原始 Protobuf 或解析缓冲.
    std::string text() const;
};

// 会话代次, 由本进程本地分配, 保证在单一进程生命周期内唯一.
struct Generation {
    // value: 代表会话唯一编号的 64 位无符号整数, 默认值为 0.
    std::uint64_t value{};

    // 仅比较当前进程的会话编号, 不提供跨进程身份顺序.
    auto operator<=>(const Generation&) const = default;
};

// 规范端点只包含数值 IP 和端口. host 不带 IPv6 方括号, text() 添加协议要求的括号.
// 表示网络中的一个访问点.
struct Endpoint {
    // host: 节点的 IP 地址(数值表示, 例如 "127.0.0.1" 或 "::1").
    std::string host;
    // port: 节点的访问端口号, 默认为 0.
    std::uint16_t port{};
    // ipv6: 布尔标志位, 标示 host 是否是一个 IPv6 地址, 默认为 false.
    bool ipv6{};
    // wildcard: 布尔标志位, 标示这是否是一个通配符地址(如 0.0.0.0 或 ::), 默认为 false.
    bool wildcard{};

    // 按规范字段逐项比较, 不进行 DNS 查询或重新解析.
    auto operator<=>(const Endpoint&) const = default;

    // 解析一个给定的字符串为端点结构.
    // local=true 允许通配地址和零端口, 不允许多播, 映射 IPv6 或 scope 别名. 不执行 DNS.
    // value 仅在调用期间借用; 成功返回拥有的规范端点, 格式或地址不支持返回 configuration, 规范化失败返回 internal.
    // 参数 value: 要解析的地址字符串(如 "127.0.0.1:8080").
    // 参数 local: 是否视为本地监听地址进行放宽检查, 默认 false.
    static Result<Endpoint> parse(std::string_view value, bool local = false);

    // 返回独立 IP:PORT 文本, IPv6 自动补方括号; 不重新校验手工构造的字段.
    std::string text() const;
};

// 角色状态持有这个独立值, 不持有生成消息的引用或密码材料.
// 描述系统中一个成员的完整拓扑信息.
struct Member {
    // 成员代次由 Supervisor 签发, 会话代次由本进程分配, 类型分开以阻止误用.
    // 用于在分布式环境下识别数据的时序和新鲜度.
    struct Epoch {
        // value: 代表代次的 64 位无符号整数, 默认值为 0.
        std::uint64_t value{};

        // 仅比较成员代次数值, 不与本地会话代次混用.
        auto operator<=>(const Epoch&) const = default;
    };

    // 决定节点在拓扑中的行为, 角色编码由协议适配层显式转换.
    enum class Role {
        // 星节点, 承担对等网络的核心职责; 值初始化的默认角色.
        star,
        // 行星节点, 通过选定的 Star 上游接入; 不参与 Star 全互联.
        planet
    };

    // galaxy: 该成员所属的逻辑集群(Galaxy)标识.
    std::string galaxy;
    // id: 成员在集群中的全局唯一 ID.
    Id id;
    // principal: 部署相关的安全凭据指纹.
    Principal principal;
    // address: 该成员对外公布的网络访问端点.
    Endpoint address;
    // epoch: 成员的当前状态代次, 用于识别数据新鲜度.
    Member::Epoch epoch;
    // role: 成员在系统中所扮演的角色 (Star 或 Planet), 默认为 star.
    Member::Role role{};
    // group: 成员所在的连接偏好分组名称, 用于 Planet 选择上游.
    std::string group;

    // 按全部拓扑字段比较成员值, 不代替验签和角色准入.
    auto operator<=>(const Member&) const = default;

    // 验证一个 Member 对象中所有字段是否均合法, 是接入逻辑前的必经步骤.
    // 验证完整成员, 所有调用者必须在安装共享状态之前检查结果.
    // member 须含有效名称, 不透明 ID, 非零 epoch 和规范可达端点; 失败返回 identity, 不修改传入值或执行验签.
    Result<void> validate() const;

    // 检查传入字符串是否符合标识符名称的标准要求.
    // 名称采用与 Go/Rust 一致的 ASCII 范围, 不做 Unicode 或大小写归一化.
    // value 长度必须为 1..64 字节且仅含字母, 数字, 点, 下划线和连字符; 空串或越界返回 false.
    // 参数 value: 待校验的名称字符串.
    static bool valid_name(std::string_view value);

    // 检查传入的标识符是否是一个有效的 ID.
    // 仅检查 ID 非空和 128 字节传输上限; UTF-8 由 Protobuf 解码验证, 签发真实性由 Identity 验签.
    // 参数 id: 待校验的唯一标识符.
    static bool valid_id(std::string_view id);
};

} // namespace astra
