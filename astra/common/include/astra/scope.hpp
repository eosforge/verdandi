#pragma once
#include <compare>
#include <cstddef>
#include <string>
#include <string_view>

namespace astra {
// 业务地址按两个独立 UTF-8 字符串精确匹配, 不把斜线解释为路径或通配符.
struct Scope {
    // 第一层分组, 1..128 字节; __ 开头表示内部数据, 只由外部入口禁止访问.
    std::string sector;
    // 第二层分组, 1..128 字节; 与 sector 独立拥有存储.
    std::string spectrum;
    // 校验地址文本, 不检查访问角色, 不分配或归一化 Unicode.
    bool valid() const noexcept;
    // 返回内部保留地址标志, 未通过 valid 的输入仍须先拒绝.
    bool internal() const noexcept;
    // 校验非空、无 NUL 的完整 UTF-8, maximum 为最大字节数, 零不接受任何文本.
    static bool text(std::string_view value, std::size_t maximum) noexcept;
    // 原字节词典序用于稳定路由/测试, 不用于发布优先级或权限匹配.
    auto operator<=>(const Scope&) const = default;
};
} // namespace astra
