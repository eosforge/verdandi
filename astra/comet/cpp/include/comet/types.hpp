#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace comet {
// 不透明的不可变原始载荷. 非空指针指向空 vector 是合法空值, 空指针表示没有记录.
using Value = std::shared_ptr<const std::vector<std::uint8_t>>;

// 两个独立 UTF-8 分组名, 不解释路径、通配符或 Unicode 归一化.
struct Scope {
    std::string sector;   // 1..128 字节, 公共入口不能以 __ 开头.
    std::string spectrum; // 1..128 字节, 与 sector 精确匹配.
    // 原字节比较用于绑定一致性, 不定义所有者或权限.
    bool operator==(const Scope&) const = default;
};

// 错误为有界本地值, 不泄漏 gRPC 类型、SECRET 或远端任意正文.
struct Error {
    // 原因与本次尝试的提交确定性分开, 不根据网络超时猜测未提交.
    enum class Code {
        input,     // 参数或绑定范围非法.
        session,   // 登录被拒绝或已失效, 需要用户更换凭据或重新登录.
        closed,    // 本地对象已关闭, 不再接纳业务.
        version,   // 权威版本暂时落后或写入版本冲突.
        conflict,  // 同一业务身份的内容冲突.
        ended,     // 注册或当前目标记录已结束.
        obsolete,  // 操作顺序过时或未发送请求被更新替代.
        history,   // 服务端无法连续恢复当前游标.
        limit,     // 本地或远端永久资源上限, 停止重复下载.
        busy,      // 暂时容量不足, 可有界重试.
        instance,  // 实际 Star 与请求绑定不一致.
        clock,     // 服务端业务时间尚不可用.
        timeout,   // 本次操作超过有限期限, 不表示未提交.
        transport, // 网络失败或不完整关闭, 可有界恢复.
        protocol,  // 成功回包或推流违反已确认契约.
        internal   // 非业务预期失败, 不回显异常正文.
    };
    enum class Effect {
        unknown,  // 默认没有证据证明本次未提交.
        unapplied // 确认本次未发送或服务端明确表示未提交.
    };
    Code code = Code::internal;           // 默认不伪装成成功或可安全重试的参数错误.
    Effect effect = Effect::unknown;      // 对写入尤其重要, 先前尝试不受这次证据覆盖.
    std::string instance;                 // 服务端明确报告的实际实例, 空表示未知.
    std::optional<std::uint64_t> version; // 已明确观察的权威位置, 未知与零不同.
    std::chrono::milliseconds retry{};    // 有界重试提示, 零表示没有远端提示.
};

// 所有可预期业务错误用相同值语义返回, 分配失败仍可能抛出标准异常.
template <class T>
using Result = std::expected<T, Error>;
} // namespace comet
