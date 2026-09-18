#include "check.hpp"
#include "store.hpp"
#include <iostream>

using namespace astra;
using namespace std::chrono_literals;

static ElapsedTime local(std::chrono::nanoseconds time) {
    return ElapsedTime(time);
}
static EpochClock::Time epoch(std::chrono::nanoseconds time) {
    return EpochClock::Time(time);
}
static void calibrate(EpochClock& clock, std::chrono::nanoseconds at, std::chrono::nanoseconds time) {
    CHECK(clock.publish({epoch(time), local(at), 0, 0}, local(at)));
}

static void test_common_coordinate() {
    EpochClock a, b;
    Store first, second;
    calibrate(a, 100s, 1'800'000'000s);
    calibrate(b, 900s, 1'800'000'000s);
    first.tick(a.now(local(100s))->time);
    second.tick(b.now(local(900s))->time);
    const auto deadline = a.now(local(100s))->deadline_after(2s);
    CHECK(deadline && b.now(local(900s))->deadline_after(2s) == deadline);
    first.put("lease", {1}, *deadline);
    second.put("lease", {1}, *deadline);
    CHECK(first.extract(0).deltas.front().deadline == *deadline);
    CHECK(first.snapshot()->data.at("lease").deadline == *deadline);
    first.tick(a.now(local(101990ms))->time);
    second.tick(b.now(local(901990ms))->time);
    CHECK(first.snapshot()->data.contains("lease") && second.snapshot()->data.contains("lease"));
    first.tick(a.now(local(102s))->time);
    second.tick(b.now(local(902s))->time);
    CHECK(first.snapshot()->data.empty() && second.snapshot()->data.empty());
}

static void test_reference_change() {
    for (const auto correction : {-10s, 10s}) {
        EpochClock clock;
        Store store;
        calibrate(clock, 0s, 100s);
        store.tick(clock.now(local(0s))->time);
        const auto deadline = clock.now(local(0s))->deadline_after(5s);
        CHECK(deadline);
        store.put("existing", {1}, *deadline);
        const auto snapshot = store.snapshot();
        clock.revoke();
        store.tick(clock.now(local(2s))->time);
        calibrate(clock, 3s, 103s + correction);
        const auto reading = clock.now(local(3s));
        CHECK(reading->time == epoch(103s) && !reading->ready && !reading->deadline_after(5s));
        store.tick(reading->time);
        CHECK(store.snapshot() == snapshot && store.version() == 1);
        CHECK(store.extract(0).deltas.front().deadline == *deadline);
        CHECK(snapshot->data.at("existing").deadline == *deadline);
        // 无论校正方向, 老期限仍是 105s. 短时间调速最多造成有限毫秒偏移, 不获得新的完整 TTL.
        store.tick(clock.now(local(4990ms))->time);
        CHECK(store.snapshot()->data.contains("existing"));
        store.tick(clock.now(local(5020ms))->time);
        CHECK(store.snapshot()->data.empty() && store.version() == 2);
        CHECK(snapshot->data.at("existing").deadline == *deadline);
    }
}

static void test_holdover_and_recovery() {
    EpochClock clock;
    Store store;
    calibrate(clock, 0s, 100s);
    store.tick(clock.now(local(0s))->time);
    const auto deadline = clock.now(local(0s))->deadline_after(5s);
    CHECK(deadline);
    store.put("expires-offline", {1}, *deadline);
    clock.revoke();
    store.tick(clock.now(local(6s))->time);
    CHECK(store.snapshot()->data.empty());
    calibrate(clock, 7s, 107s);
    CHECK(clock.now(local(7s))->ready);
    store.tick(clock.now(local(7s))->time);
    CHECK(store.snapshot()->data.empty() && store.version() == 2);
    // 模拟新进程已校准后恢复同一个绝对截止, 不能按原 TTL 重新续满.
    Store restored;
    restored.tick(epoch(106s));
    restored.put("expired", {1}, epoch(105s));
    restored.put("future", {2}, epoch(108s));
    restored.tick(epoch(106010ms));
    CHECK(!restored.snapshot()->data.contains("expired") && restored.snapshot()->data.contains("future"));
    restored.tick(epoch(108s));
    CHECK(restored.snapshot()->data.empty());
}

static void test_initialization_and_metadata() {
    Store store;
    store.put("permanent", {1});
    bool rejected = false;
    try {
        store.put("unanchored", {2}, epoch(100s));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    CHECK(rejected && store.version() == 1);
    // 初次锚定接近真实年份, 不应从 1970 年逐拍追赶.
    const auto now = epoch(1'800'000'000s);
    store.tick(now);
    store.put("finite", {2}, now + 5s);
    const auto before = store.snapshot();
    CHECK(!before->data.at("permanent").deadline);
    CHECK(before->data.at("finite").deadline == now + 5s);
    store.put("finite", {3}, now + 10s);
    CHECK(store.snapshot()->data.at("finite").deadline == now + 10s);
    CHECK(before->data.at("finite").deadline == now + 5s);
    CHECK(store.extract(2).deltas.front().deadline == now + 10s);
    store.put("finite", {4});
    CHECK(!store.snapshot()->data.at("finite").deadline);
    store.tick(now + 20s);
    CHECK(store.snapshot()->data.size() == 2);
    rejected = false;
    try {
        store.put("negative", {}, epoch(-1ns));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected && !store.snapshot()->data.contains("negative"));
    rejected = false;
    try {
        store.tick(epoch(-1ns));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

int main() {
    try {
        test_common_coordinate();
        test_reference_change();
        test_holdover_and_recovery();
        test_initialization_and_metadata();
        std::cout << "PASS single Unix deadlines, continuous correction, holdover and snapshot metadata\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}