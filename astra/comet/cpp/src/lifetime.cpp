#include "lifetime.hpp"
#ifdef _WIN32
#include <windows.h>

// realtimeapiset 依赖 Windows 基础类型和体系结构宏, 必须在 windows.h 之后包含.
#include <realtimeapiset.h>
#else
#include <ctime>
#endif

namespace comet::detail {
// Lifetime::now 读取包含系统挂起时间的单调毫秒, 不受墙钟调整影响.
// 不抛异常, 越界或调用失败即无时钟.
std::optional<Lifetime::Time> Lifetime::now() noexcept {
#ifdef _WIN32
    // Windows 10/Server 2016 起提供精确 interrupt time, 保留休眠补偿; 单位为 100 ns.
    // 除以 10000 后必定落在非负 int64 范围内, 与 Linux 分支同样向下取整为毫秒.
    ULONGLONG value{};
    ::QueryInterruptTimePrecise(&value);
    return static_cast<Time>(value / 10'000);
#else
    timespec value{}; // 由内核填写, 秒/纳秒分量须可表示为非负 int64 毫秒.
    if (::clock_gettime(CLOCK_BOOTTIME, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000 || static_cast<std::uint64_t>(value.tv_sec) > static_cast<std::uint64_t>((std::numeric_limits<Time>::max() - 999) / 1000)) {
        return std::nullopt;
    }
    return static_cast<Time>(value.tv_sec) * 1000 + value.tv_nsec / 1'000'000;
#endif
}
} // namespace comet::detail
