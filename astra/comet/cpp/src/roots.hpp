#pragma once
#include <string_view>

namespace comet::detail {
// 只编入公开 CA PEM, CMake 检查 PRIVATE KEY 标记; 外部文件优先, 不修改系统信任库.
inline std::string_view roots() noexcept {
#ifdef COMET_CA_GENERATED
    // CMake 生成的公开 PEM 字节, 长度不包含额外的 NUL, 随 SDK 静态存储.
    static constexpr char certificate[] = {
#include "comet_ca.inc"
    };
    return {certificate, sizeof(certificate)};
#else
    return {};
#endif
}
} // namespace comet::detail
