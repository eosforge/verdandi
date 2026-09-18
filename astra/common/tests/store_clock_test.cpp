#include "check.hpp"
#include "store.hpp"
#include <iostream>

using namespace astra;
using namespace std::chrono_literals;

namespace {

// 把 time 纳秒显式标记为本地经过时间, 供确定性校准入口使用.
static ElapsedTime local(std::chrono::nanoseconds time) {
    return ElapsedTime(time);
}

// 把 time 纳秒显式标记为 Unix 时间, 不引入进程纪元编号.
static Clock::Time epoch(std::chrono::nanoseconds time) {
    return Clock::Time(time);
}

// 给 clock 注入 at 时刻的 time 观测, 误差和往返为零, 发布失败直接断言.
static void calibrate(Clock& clock, std::chrono::nanoseconds at, std::chrono::nanoseconds time) {
    CHECK(clock.publish({epoch(time), local(at), 0, 0}, local(at)));
}

// 两个本地起点不同的时钟共享同一 Unix 截止, 两个 Store 应在同一业务时刻过期.
static void test_common_coordinate() {

    // a/b 分别从 100 s 和 900 s 本地坐标校准到同一 Unix 时刻.
    Clock a, b;
    // first/second 独立持有相同绝对租约, 用于检查截止不依赖本地时钟起点.
    Store first, second;
    calibrate(a, 100s, 1'800'000'000s);
    calibrate(b, 900s, 1'800'000'000s);
    first.tick(a.now(local(100s))->time);
    second.tick(b.now(local(900s))->time);
    // deadline 是就绪读数计算的一次性绝对截止, 后续参考变化不能改写它.
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

// 参考前跳或回拨只影响调速与资格, 不重写已接受租约的绝对截止.
static void test_reference_change() {

    for (const auto correction : {-10s, 10s}) {
        // clock 为当前用例的独立纪元时钟, 默认 1000 ppm 调速, 由 calibrate 显式锚定.
        Clock clock;
        // store 使用默认 10 ms 拍间隔, 初始未锚定, 由有效时钟读数驱动.
        Store store;
        calibrate(clock, 0s, 100s);
        store.tick(clock.now(local(0s))->time);
        // deadline 是就绪读数计算的一次性绝对截止, 后续参考变化不能改写它.
        const auto deadline = clock.now(local(0s))->deadline_after(5s);
        CHECK(deadline);
        store.put("existing", {1}, *deadline);
        // snapshot 保留参考变化前的稳定视图, 用于核对旧值和截止均未被修改.
        const auto snapshot = store.snapshot();
        clock.revoke();
        store.tick(clock.now(local(2s))->time);
        calibrate(clock, 3s, 103s + correction);
        // reading 为参考变更后的连续输出, 偏差尚未消化时应拒绝签发新有限期限.
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

// 断开参考后继续清理既有 TTL, 恢复参考或新建 Store 都不能重新续满旧租约.
static void test_holdover_and_recovery() {

    // clock 为当前用例的独立纪元时钟, 默认 1000 ppm 调速, 由 calibrate 显式锚定.
    Clock clock;
    // store 使用默认 10 ms 拍间隔, 初始未锚定, 由有效时钟读数驱动.
    Store store;
    calibrate(clock, 0s, 100s);
    store.tick(clock.now(local(0s))->time);
    // deadline 是就绪读数计算的一次性绝对截止, 后续参考变化不能改写它.
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

// 覆盖未锚定的有限写入拒绝,初次 Unix 锚定,快照截止隔离及非法负时间.
static void test_initialization_and_metadata() {

    // store 使用默认 10 ms 拍间隔, 初始未锚定, 由有效时钟读数驱动.
    Store store;
    store.put("permanent", {1});
    // rejected 只在明确抛出预期配置或状态异常时设置, 不接受悄悄降级成永久值.
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
    // before 保留续租前版本, 后续写入只能改变新视图中的截止.
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

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
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
