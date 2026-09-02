#pragma once

#include "verdandi/c/types.h"

#include <string>

namespace verdandi {
namespace legacy {

/// 查询当前链接运行库是否实现一项稳定字符串能力。
///
/// 未知或空名称返回 false；true 只表示代码能力存在，不检查 Redis、证书、ACL 或网络部署。
inline bool has_capability(const std::string& capability) {
    const verdandi_string_view value = {capability.empty() ? NULL : capability.data(), capability.size()};
    return verdandi_c_has_capability(value) != 0;
}

} // namespace legacy
} // namespace verdandi
