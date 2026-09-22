#include "check.hpp"
#include "lifetime.hpp"
#include <iostream>

namespace {
using namespace std::chrono_literals;
using comet::detail::Lifetime;

// 不休眠模拟发送、重试、迟到、系统 suspend 与新 UUID, 所有边界均使用整数毫秒.
void budget() {

    Lifetime lifetime(1s);
    CHECK(!lifetime.ready(1000) && lifetime.due(1000));
    CHECK(lifetime.confirm(1000));
    CHECK(lifetime.ready(1999) && !lifetime.ready(2000));
    CHECK(lifetime.confirm(1000) && !lifetime.ready(2001)); // 重复成功不从收到时刻重新起算.
    CHECK(lifetime.confirm(900) && !lifetime.ready(2001));  // 迟到旧尝试不能覆盖较新的确认.
    CHECK(!lifetime.ready(std::nullopt) && !lifetime.ready(999));
    CHECK(lifetime.due(8 * 24 * 60 * 60 * 1000LL)); // 一周休眠只产生一个 due 状态.
    CHECK(lifetime.confirm(3000) && lifetime.ready(3500));
    lifetime.reset();
    CHECK(!lifetime.ready(3500) && lifetime.due(3500));
    CHECK(!lifetime.confirm(-1) && !lifetime.confirm(INT64_MAX));
    CHECK(lifetime.confirm(INT64_MAX - 1000));
    CHECK(lifetime.ready(INT64_MAX - 1) && !lifetime.ready(INT64_MAX));
}

// 最小 TTL 的抖动仍以数百毫秒调度, 边界配置不产生零周期或大整数溢出.
void cadence() {

    Lifetime lifetime(1s);
    CHECK(lifetime.confirm(1000));
    for (std::size_t seed = 0; seed != 1000; ++seed) {
        lifetime.spread(seed);
        const auto interval = lifetime.delay(1000).count();
        CHECK(interval >= 300 && interval <= 366);
        CHECK(!lifetime.due(1000 + interval - 1) && lifetime.due(1000 + interval));
        CHECK(lifetime.delay(1'000'000) == 0ms);
    }
    for (const auto ttl : {0ms, -1ms, 999ms, 600001ms}) {
        bool rejected{}; // 非法私有配置也必须在初始化阶段明确拒绝.
        try {
            Lifetime invalid(ttl);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }
}
} // namespace

int main() {
    try {
        budget();
        cadence();
        std::cout << "local lease budget cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
