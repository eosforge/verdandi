#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace verdandi {

/// 跨语言稳定、可供程序判断的 Verdandi 结果类别。
enum class code : std::uint8_t {
    invalid,
    protocol,
    contract,
    target,
    capacity,
    missing,
    stale,
    transition,
    immutable,
    corrupt,
    unavailable,
    deadline,
    ambiguous,
    closed,
};

/// 返回 `value` 对应的稳定小写协议名称。
[[nodiscard]] constexpr std::string_view to_string(code value) noexcept {
    using enum code;
    switch (value) {
    case invalid:
        return "invalid";
    case protocol:
        return "protocol";
    case contract:
        return "contract";
    case target:
        return "target";
    case capacity:
        return "capacity";
    case missing:
        return "missing";
    case stale:
        return "stale";
    case transition:
        return "transition";
    case immutable:
        return "immutable";
    case corrupt:
        return "corrupt";
    case unavailable:
        return "unavailable";
    case deadline:
        return "deadline";
    case ambiguous:
        return "ambiguous";
    case closed:
        return "closed";
    }
    return "corrupt";
}

/// 把稳定小写协议 `value` 转换为结果类别；未知字符串返回空值。
[[nodiscard]] std::optional<code> parse_code(std::string_view value) noexcept;

/// 一个带有界可选上下文的稳定 Verdandi 错误值。
class error final {
public:
    /// 只用机器可读 `value` 构造错误，其余上下文为空。
    explicit error(code value) noexcept;

    /// 用 `value` 和被拒绝或损坏的协议 `field` 构造错误。
    error(code value, std::string field);

    /// 构造包含协议字段、权威 `revision` 与有界底层 `detail` 的完整错误。
    error(code value, std::string field, std::uint64_t revision, std::string detail);

    /// 返回机器可读的稳定结果类别。
    [[nodiscard]] code category() const noexcept;

    /// 返回被拒绝或损坏的协议字段；不适用时为空字符串。
    [[nodiscard]] std::string_view field() const noexcept;

    /// 返回拒绝结果关联的权威 revision；不适用时为空值。
    [[nodiscard]] std::optional<std::uint64_t> revision() const noexcept;

    /// 返回限长后的驱动、运行时或应用编解码诊断；机器逻辑不得依赖该文本。
    [[nodiscard]] std::string_view detail() const noexcept;

    /// 生成供日志诊断使用的有界文本；机器判断应使用 `category()`。
    [[nodiscard]] std::string message() const;

    /// 返回附加了 `value` 的副本，便于在协议解析边界补充权威 revision。
    [[nodiscard]] error with_revision(std::uint64_t value) const;

    /// 返回附加了限长 `value` 的副本；最多保留 512 个字节。
    [[nodiscard]] error with_detail(std::string value) const;

private:
    code category_;
    std::string field_;
    std::optional<std::uint64_t> revision_;
    std::string detail_;
};

/// Verdandi C++23 公共失败返回类型；实现层异常必须在进入该边界前转换。
template <class T>
using result = std::expected<T, error>;

} // namespace verdandi
