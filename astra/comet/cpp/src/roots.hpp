#pragma once
#include <string_view>

namespace comet::detail {
// 只编入公开 CA PEM, CMake 检查 PRIVATE KEY 标记; 外部文件优先, 不修改系统信任库.
inline std::string_view roots() noexcept {
#ifdef COMET_CA_FILE
    static constexpr char certificate[] = {
#embed COMET_CA_FILE
    };
    return {certificate, sizeof(certificate)};
#else
    return {};
#endif
}
} // namespace comet::detail
