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

// Unix/BOOTTIME 纳秒坐标的有符号上界, 所有相加操作在提交前检查.
constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
// 样本接纳与同步质量的新鲜度上限为 5 s, 包含系统挂起时间; 不限制已初始化时钟的租约能力.
constexpr std::uint64_t freshness = 5'000'000'000;
} // namespace

ElapsedTime Clock::Elapsed::now() {

    // value 接收内核的秒和纳秒分量, 零初始化只用于输出缓冲, 不表示有效样本.
    timespec value{};
    if (::clock_gettime(CLOCK_BOOTTIME, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000 || value.tv_sec > (maximum - value.tv_nsec) / 1'000'000'000) {
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
        // descriptor 独占本次寻边的非阻塞定时器, -1 表示创建失败; 析构关闭有效描述符.
        int descriptor = ::timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);

        // 寻边成功,取消和异常路径均关闭描述符, 不向调用者转移所有权.
        ~Timer() {

            if (descriptor >= 0) {
                ::close(descriptor);
            }
        }
    } timer;

    // budget 使用一次性 200 ms 到期, 周期字段保持零, 避免长期保留内核事件.
    itimerspec budget{};
    budget.it_value.tv_nsec = 200'000'000;
    if (timer.descriptor < 0) {
        throw std::runtime_error("Cannot start clock precision budget");
    }

    // resolution 接收内核公布的分辨率, 后续还需实测寻边, 不能仅凭该值推断精度.
    timespec resolution{};
    if (::clock_getres(CLOCK_BOOTTIME, &resolution) != 0 || resolution.tv_sec != 0 || resolution.tv_nsec <= 0 || resolution.tv_nsec > 20'000'000) {
        throw std::runtime_error("Unsupported elapsed clock resolution");
    }
    if (::timerfd_settime(timer.descriptor, 0, &budget, nullptr) != 0) {
        throw std::runtime_error("Cannot arm clock precision budget");
    }
    return measure_clock_precision([] { return elapsed_ns(Clock::Elapsed::now()); },
                                   [&] {
                                       if (stop.stop_requested()) {
                                           throw std::runtime_error("Clock precision cancelled");
                                       }

                                       // expirations 接收定时器到期次数, 只关心是否到期, 不用于推进业务时钟.
                                       std::uint64_t expirations{};
                                       // result 为读取字节数; EAGAIN 表示预算尚未到期, EINTR 允许下一轮重试.
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

Filter::Filter(std::uint64_t local_precision) : local_precision_(local_precision) {

    if (local_precision == 0 || local_precision > 20'000'000) {
        throw std::invalid_argument("Invalid local clock precision");
    }
}

bool Filter::observe(std::uint64_t t0, std::uint64_t t1, std::uint64_t t2, ElapsedTime received, std::uint64_t precision, std::uint64_t uncertainty, bool synchronized) {

    // t3 为接收时的本机经过时间, 与 t0 同域; 负值拒绝后才能转换为无符号数.
    const auto t3 = elapsed_ns(received);
    // limit 约束远端无符号字段, 使后续有符号差值和中点运算保持可表示.
    const auto limit = static_cast<std::uint64_t>(maximum);
    if (!synchronized || precision == 0 || precision > 20'000'000 || uncertainty > clock_uncertainty_limit_ns || t0 > limit || t1 > limit || t2 > limit || t3 < 0 || t0 > static_cast<std::uint64_t>(t3) || t2 < t1) {
        return false;
    }

    // elapsed 是本机往返时长, 单位 ns; 初步校验已保证 t3 >= t0.
    const auto elapsed = static_cast<std::uint64_t>(t3) - t0;
    // processing 是服务端两次采样之差, 单位 ns; 不与本机时钟原点直接比较.
    const auto processing = t2 - t1;
    if (elapsed > 200'000'000 || processing > 200'000'000) {
        return false;
    }

    // 两端底层各 500 ppm + Pulsar 调速 500 ppm; 上游绝对误差不能用来放宽处理时长检查.
    const auto dispersion = local_precision_ + precision + drift(elapsed, 1500);
    if (processing > elapsed && processing - elapsed > 2 * dispersion) {
        return false;
    }

    // offset 用四时间戳中点估计 Unix 与本机时间的偏移, midpoint 避免两差相加溢出.
    const auto offset = std::midpoint(static_cast<std::int64_t>(t1) - static_cast<std::int64_t>(t0), static_cast<std::int64_t>(t2) - t3);
    if ((offset > 0 && t3 > maximum - offset) || (offset < 0 && offset < -t3)) {
        return false;
    }

    // rtt 为扣除处理时间后的网络时长, 下界为本机分辨率, 量化造成的负值按零处理.
    const auto rtt = std::max(elapsed > processing ? elapsed - processing : 0, local_precision_);
    // error 合并上游,量化,漂移与半往返误差, 向上取整并保留中点舍入的 1 ns.
    const auto error = uncertainty + dispersion + (rtt + 1) / 2 + 1;
    if (!best_ || rtt <= best_->rtt_ns) {
        best_ = Clock::Estimate{Clock::Time(std::chrono::nanoseconds(t3 + offset)), received, error, rtt};
    }
    count_ = std::min(count_ + 1, 8U);
    return true;
}

std::optional<Clock::Estimate> Filter::result() const {
    return count_ >= 3 ? best_ : std::nullopt;
}

std::expected<Clock::Time, Clock::Error> Clock::Reading::deadline_after(std::chrono::nanoseconds ttl) const noexcept {

    // base 为本次读数的 Unix 纳秒坐标, 必须非负; ttl 为调用方给定的非负时长.
    // ready 只表示本地计时可用. synchronized 过期不阻止已校准 Star 创建或延长有限期限.
    const auto base = time.time_since_epoch().count();
    if (!ready) {
        return std::unexpected(Clock::Error::clock_unready);
    }
    if (base < 0 || ttl.count() < 0) {
        return std::unexpected(Clock::Error::invalid_time);
    }
    if (ttl.count() > maximum - base) {
        return std::unexpected(Clock::Error::exhausted);
    }
    return time + ttl;
}

Clock::Clock(std::uint32_t slew_ppm) : slew_ppm_(slew_ppm) {

    if (slew_ppm == 0 || slew_ppm > 1000) {
        throw std::invalid_argument("Epoch clock slew must be 1..1000 ppm");
    }
}

bool Clock::advance(ElapsedTime local) const {

    if (failed_ || local < local_) {
        failed_ = true;
        return false;
    }

    // 非负锚点之间的有序差不溢出. 保留小数, 使相同校正段内小步/大步推进等价.
    const auto elapsed = static_cast<std::uint64_t>((local - local_).count());
    // fraction 累积本段 ppm 分数预算, 保留频繁小步推进时尚不足 1 ns 的校正量.
    const auto fraction = elapsed % 1'000'000 * slew_ppm_ + fraction_;
    // budget 是本次允许吸收的最大偏差, 单位 ns; 先除后乘避免大跨度乘法溢出.
    const auto budget = elapsed / 1'000'000 * slew_ppm_ + fraction / 1'000'000;
    // correction 不能超过残余偏差, 避免越过目标后产生人为振荡.
    const auto correction = std::min(magnitude(debt_), budget);
    // step 叠加有限调速, slew_ppm_ <= 1000 保证负向修正不会使走时倒退.
    const auto step = debt_ < 0 ? elapsed - correction : elapsed + correction;
    // base 为上次已公开的 Unix 纳秒值, 检查可表示性后才更新锚点和残余偏差.
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

std::optional<Clock::Reading> Clock::read(ElapsedTime local) const {

    if (!estimate_ || !advance(local)) {
        return std::nullopt;
    }

    // age 为最近有效观测的非负年龄, 单位 ns; 已推进的本地锚点不会早于观测.
    const auto age = static_cast<std::uint64_t>((local - estimate_->sampled).count());
    // 留出底层漂移及上游调速余量; 本机残余偏差单独计入, 新样本不能掩盖尚未追平的事实.
    const auto uncertainty = estimate_->uncertainty_ns + drift(age, 2000) + magnitude(debt_);
    return Clock::Reading{epoch_, uncertainty, estimate_->rtt_ns, estimate_->sampled, true, trusted_ && age <= freshness && uncertainty <= clock_uncertainty_limit_ns};
}

std::optional<Clock::Reading> Clock::now() const {
    // lock 串行保护锚点,残余偏差和资格状态; 默认入口在持锁后采样, 避免伪反序.
    std::lock_guard lock(mutex_);
    return read(Clock::Elapsed::now());
}

std::optional<Clock::Reading> Clock::now(ElapsedTime local) const {
    // lock 串行保护锚点,残余偏差和资格状态; 默认入口在持锁后采样, 避免伪反序.
    std::lock_guard lock(mutex_);
    return read(local);
}

bool Clock::observe(const Clock::Estimate& estimate, ElapsedTime local) {

    // target 是待发布样本的非负 Unix 纳秒值, 仅首次校准直接建立公开锚点.
    const auto target = estimate.time.time_since_epoch().count();
    if (failed_ || target < 0 || elapsed_ns(estimate.sampled) < 0 || local < estimate.sampled || estimate.uncertainty_ns > 1'000'000'000 || estimate.rtt_ns > 200'000'000 || (estimate_ && estimate.sampled <= estimate_->sampled)) {
        return false;
    }

    // age 是从采样到消费的经过时长, 先限制为 freshness 再做 Unix 外推.
    const auto age = (local - estimate.sampled).count();
    if (static_cast<std::uint64_t>(age) > freshness || target > maximum - age) {
        return false;
    }

    // predicted 将观测外推到本次消费时刻; 已运行时钟通过 debt_ 渐进追踪该目标.
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

        // debt 为目标与已公开时钟的有符号纳秒差; 校正方向切换时不能继承旧分数预算.
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

bool Clock::publish(const Clock::Estimate& estimate) {
    // lock 串行保护锚点,残余偏差和资格状态; 默认入口在持锁后采样, 避免伪反序.
    std::lock_guard lock(mutex_);
    return observe(estimate, Clock::Elapsed::now());
}

bool Clock::publish(const Clock::Estimate& estimate, ElapsedTime local) {
    // lock 串行保护锚点,残余偏差和资格状态; 默认入口在持锁后采样, 避免伪反序.
    std::lock_guard lock(mutex_);
    return observe(estimate, local);
}

void Clock::revoke() {
    // lock 串行保护锚点,残余偏差和资格状态; 默认入口在持锁后采样, 避免伪反序.
    std::lock_guard lock(mutex_);
    trusted_ = false;
}
} // namespace astra
