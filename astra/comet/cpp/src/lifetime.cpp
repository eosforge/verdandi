#include "lifetime.hpp"
#include <ctime>

namespace comet::detail {
std::optional<Lifetime::Time> Lifetime::now() noexcept {
    timespec value{}; // 由内核填写, 秒/纳秒分量须可表示为非负 int64 毫秒.
    if (::clock_gettime(CLOCK_BOOTTIME, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000 || static_cast<std::uint64_t>(value.tv_sec) > static_cast<std::uint64_t>((std::numeric_limits<Time>::max() - 999) / 1000)) {
        return std::nullopt;
    }
    return static_cast<Time>(value.tv_sec) * 1000 + value.tv_nsec / 1'000'000;
}
} // namespace comet::detail
