#include "clock_filter.hpp"
#include "clock_precision.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

namespace astra {
namespace {
// 先除后乘, 避免长时间失联时 age * ppm 溢出; 向上取整不低估一纳秒的误差.
std::uint64_t drift(std::uint64_t age, std::uint32_t ppm) noexcept {
    return age / 1'000'000 * ppm + (age % 1'000'000 * ppm + 999'999) / 1'000'000;
}
// 所有残余差都来自两个非负 int64 时间之差, 不可能为 INT64_MIN.
std::uint64_t magnitude(std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value < 0 ? -value : value);
}
constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t freshness = 5'000'000'000;
} // namespace

ElapsedTime EpochClock::Elapsed::now() {
    timespec value{};
    if (::clock_gettime(CLOCK_BOOTTIME, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000 ||
        value.tv_sec > (maximum - value.tv_nsec) / 1'000'000'000) {
        throw std::runtime_error("Cannot read elapsed clock");
    }
    return time_point(duration(value.tv_sec * 1'000'000'000 + value.tv_nsec));
}

std::int64_t elapsed_ns(ElapsedTime local) noexcept {
    return local.time_since_epoch().count();
}

std::uint64_t elapsed_precision_ns(std::stop_token stop) {
    // 内核定时器限制观察窗口, 不以 CPU 循环数限制粗时钟寻边.
    struct Timer {
        int descriptor = ::timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
        ~Timer() {
            if (descriptor >= 0) {
                ::close(descriptor);
            }
        }
    } timer;
    itimerspec budget{};
    budget.it_value.tv_nsec = 200'000'000;
    if (timer.descriptor < 0) {
        throw std::runtime_error("Cannot start clock precision budget");
    }
    timespec resolution{};
    if (::clock_getres(CLOCK_BOOTTIME, &resolution) != 0 || resolution.tv_sec != 0 || resolution.tv_nsec <= 0 || resolution.tv_nsec > 20'000'000) {
        throw std::runtime_error("Unsupported elapsed clock resolution");
    }
    if (::timerfd_settime(timer.descriptor, 0, &budget, nullptr) != 0) {
        throw std::runtime_error("Cannot arm clock precision budget");
    }
    return measure_clock_precision([] { return elapsed_ns(EpochClock::Elapsed::now()); },
                                   [&] {
                                       if (stop.stop_requested()) {
                                           throw std::runtime_error("Clock precision cancelled");
                                       }
                                       std::uint64_t expirations{};
                                       const auto result = ::read(timer.descriptor, &expirations, sizeof(expirations));
                                       if (result == static_cast<ssize_t>(sizeof(expirations))) {
                                           return false;
                                       }
                                       if (result < 0 && (errno == EAGAIN || errno == EINTR)) {
                                           return true;
                                       }
                                       throw std::runtime_error("Cannot read clock precision budget");
                                   },
                                   static_cast<std::uint64_t>(resolution.tv_nsec));
}

ClockFilter::ClockFilter(std::uint64_t local_precision) : local_precision_(local_precision) {
    if (local_precision == 0 || local_precision > 20'000'000) {
        throw std::invalid_argument("Invalid local clock precision");
    }
}

bool ClockFilter::observe(std::uint64_t t0, std::uint64_t t1, std::uint64_t t2, ElapsedTime received, std::uint64_t precision, std::uint64_t uncertainty,
                          bool synchronized) {
    const auto t3 = elapsed_ns(received);
    const auto limit = static_cast<std::uint64_t>(maximum);
    if (!synchronized || precision == 0 || precision > 20'000'000 || uncertainty > clock_uncertainty_limit_ns || t0 > limit || t1 > limit || t2 > limit ||
        t3 < 0 || t0 > static_cast<std::uint64_t>(t3) || t2 < t1) {
        return false;
    }
    const auto elapsed = static_cast<std::uint64_t>(t3) - t0;
    const auto processing = t2 - t1;
    if (elapsed > 200'000'000 || processing > 200'000'000) {
        return false;
    }
    // 两端底层各 500 ppm + Pulsar 调速 500 ppm; 上游绝对误差不能用来放宽处理时长检查.
    const auto dispersion = local_precision_ + precision + drift(elapsed, 1500);
    if (processing > elapsed && processing - elapsed > 2 * dispersion) {
        return false;
    }
    const auto offset = std::midpoint(static_cast<std::int64_t>(t1) - static_cast<std::int64_t>(t0), static_cast<std::int64_t>(t2) - t3);
    if ((offset > 0 && t3 > maximum - offset) || (offset < 0 && offset < -t3)) {
        return false;
    }
    const auto rtt = std::max(elapsed > processing ? elapsed - processing : 0, local_precision_);
    const auto error = uncertainty + dispersion + (rtt + 1) / 2 + 1;
    if (!best_ || rtt <= best_->rtt_ns) {
        best_ = EpochClock::Estimate{EpochClock::Time(std::chrono::nanoseconds(t3 + offset)), received, error, rtt};
    }
    count_ = std::min(count_ + 1, 8U);
    return true;
}

std::optional<EpochClock::Estimate> ClockFilter::result() const {
    return count_ >= 3 ? best_ : std::nullopt;
}

std::expected<EpochClock::Time, EpochClock::DeadlineError> EpochClock::Reading::deadline_after(std::chrono::nanoseconds ttl) const noexcept {
    const auto base = time.time_since_epoch().count();
    if (!ready) {
        return std::unexpected(EpochClock::DeadlineError::clock_unready);
    }
    if (base < 0 || ttl.count() < 0) {
        return std::unexpected(EpochClock::DeadlineError::invalid_time);
    }
    if (ttl.count() > maximum - base) {
        return std::unexpected(EpochClock::DeadlineError::exhausted);
    }
    return time + ttl;
}

EpochClock::EpochClock(std::uint32_t slew_ppm) : slew_ppm_(slew_ppm) {
    if (slew_ppm == 0 || slew_ppm > 1000) {
        throw std::invalid_argument("Epoch clock slew must be 1..1000 ppm");
    }
}

bool EpochClock::advance(ElapsedTime local) const {
    if (failed_ || local < local_) {
        failed_ = true;
        return false;
    }
    // 非负锚点之间的有序差不溢出. 保留小数, 使相同校正段内小步/大步推进等价.
    const auto elapsed = static_cast<std::uint64_t>((local - local_).count());
    const auto fraction = elapsed % 1'000'000 * slew_ppm_ + fraction_;
    const auto budget = elapsed / 1'000'000 * slew_ppm_ + fraction / 1'000'000;
    const auto correction = std::min(magnitude(debt_), budget);
    const auto step = debt_ < 0 ? elapsed - correction : elapsed + correction;
    const auto base = epoch_.time_since_epoch().count();
    if (step > static_cast<std::uint64_t>(maximum - base)) {
        failed_ = true;
        return false;
    }
    epoch_ += std::chrono::nanoseconds(static_cast<std::int64_t>(step));
    debt_ += debt_ < 0 ? static_cast<std::int64_t>(correction) : -static_cast<std::int64_t>(correction);
    fraction_ = debt_ == 0 ? 0 : fraction % 1'000'000;
    local_ = local;
    return true;
}

std::optional<EpochClock::Reading> EpochClock::read(ElapsedTime local) const {
    if (!estimate_ || !advance(local)) {
        return std::nullopt;
    }
    const auto age = static_cast<std::uint64_t>((local - estimate_->sampled).count());
    // 留出底层漂移及上游调速余量; 本机残余偏差单独计入, 新样本不能掩盖尚未追平的事实.
    const auto uncertainty = estimate_->uncertainty_ns + drift(age, 2000) + magnitude(debt_);
    return EpochClock::Reading{epoch_, uncertainty, estimate_->rtt_ns, estimate_->sampled,
                               trusted_ && age <= freshness && uncertainty <= clock_uncertainty_limit_ns};
}

std::optional<EpochClock::Reading> EpochClock::now() const {
    std::lock_guard lock(mutex_);
    return read(EpochClock::Elapsed::now());
}

std::optional<EpochClock::Reading> EpochClock::now(ElapsedTime local) const {
    std::lock_guard lock(mutex_);
    return read(local);
}

bool EpochClock::observe(const EpochClock::Estimate& estimate, ElapsedTime local) {
    const auto target = estimate.time.time_since_epoch().count();
    if (failed_ || target < 0 || elapsed_ns(estimate.sampled) < 0 || local < estimate.sampled || estimate.uncertainty_ns > 1'000'000'000 ||
        estimate.rtt_ns > 200'000'000 || (estimate_ && estimate.sampled <= estimate_->sampled)) {
        return false;
    }
    const auto age = (local - estimate.sampled).count();
    if (static_cast<std::uint64_t>(age) > freshness || target > maximum - age) {
        return false;
    }
    const auto predicted = estimate.time + std::chrono::nanoseconds(age);
    if (!estimate_) {
        // 首次校准必须质量达标, 防止以严重错误的值启动后又耗时数小时平滑恢复.
        if (estimate.uncertainty_ns + drift(static_cast<std::uint64_t>(age), 2000) > clock_uncertainty_limit_ns) {
            return false;
        }
        epoch_ = predicted;
        local_ = local;
    } else {
        if (!advance(local)) {
            return false;
        }
        const auto debt = (predicted - epoch_).count();
        if ((debt < 0) != (debt_ < 0) || debt == 0) {
            fraction_ = 0;
        }
        debt_ = debt;
    }
    estimate_ = estimate;
    trusted_ = true;
    return true;
}

bool EpochClock::publish(const EpochClock::Estimate& estimate) {
    std::lock_guard lock(mutex_);
    return observe(estimate, EpochClock::Elapsed::now());
}

bool EpochClock::publish(const EpochClock::Estimate& estimate, ElapsedTime local) {
    std::lock_guard lock(mutex_);
    return observe(estimate, local);
}

void EpochClock::revoke() {
    std::lock_guard lock(mutex_);
    trusted_ = false;
}
} // namespace astra
