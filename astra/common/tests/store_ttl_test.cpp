#include "check.hpp"
#include "store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <thread>

using namespace astra;
using namespace std::chrono_literals;

namespace {
// origin 是本测试的 Unix 业务时间起点, 所有时间均显式构造, 不读取墙钟或休眠.
constexpr auto origin = Clock::Time(1s);

// 零与负间隔必须在进入调度前拒绝, 防止除零和永不前进的补拍循环.
void test_interval() {

    for (const auto interval : {Steady::duration::zero(), Steady::duration{-1}}) {
        // rejected 从 false 开始, 仅预期的非法周期异常才能令检查通过.
        bool rejected = false;
        try {
            // store 为本场景独立实例, 显式配置历史和拍间隔, 不借用宿主墙钟作为业务时间.
            Store store(1000, 10min, interval, origin);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

// 不足一拍的余量保留到下次调用; 截止向上取整, 相同时间和倒退时间不会额外推进.
void test_precision_and_clock() {

    // store 为本场景独立实例, 显式配置历史和拍间隔, 不借用宿主墙钟作为业务时间.
    Store store(1000, 10min, 10ms, origin);
    store.put("boundary", {1}, origin + 10ms);
    store.put("fraction", {2}, origin + 10ms + 1ns);
    store.put("permanent", {3});
    store.tick(origin + 9ms);
    CHECK(store.version() == 3);
    store.tick(origin + 10ms);
    CHECK(store.version() == 4 && !store.snapshot()->data.contains("boundary"));
    CHECK(store.snapshot()->data.contains("fraction"));
    store.tick(origin + 10ms);
    store.tick(origin - 1ms);
    store.tick(origin + 19ms);
    CHECK(store.version() == 4);
    store.tick(origin + 20ms);
    CHECK(store.version() == 5 && store.snapshot()->data.size() == 1);
    CHECK(store.snapshot()->data.contains("permanent"));
}

// 调度使用 Store 已推进的时刻, 不能把系统读取时间或驱动迟到额外加到绝对租约上.
void test_renewal_and_catchup() {

    // store 为本场景独立实例, 显式配置历史和拍间隔, 不借用宿主墙钟作为业务时间.
    Store store(1000, 10min, 1ms, origin);
    store.put("renew", {1}, origin + 5ms);
    store.put("early", {2}, origin + 20ms);
    store.put("cancel", {3}, origin + 4ms);
    store.tick(origin + 2ms);
    store.put("renew", {4}, origin + 10ms);
    store.put("early", {5}, origin + 3ms);
    store.put("cancel", {6});
    store.tick(origin + 3ms);
    CHECK(!store.snapshot()->data.contains("early"));
    store.tick(origin + 5ms);
    CHECK(store.snapshot()->data.at("renew").value->front() == 4);
    CHECK(store.snapshot()->data.at("cancel").value->front() == 6);
    // delta 保留绝对 Unix 截止, 空值表示永久; 最大整数也属于合法有限期限.
    // changes 为指定游标之后的完整批次, 检查删除数和版本边界而不依赖同批键顺序.
    const auto changes = store.extract(3);
    CHECK(changes.deltas[0].deadline == origin + 10ms);
    CHECK(!changes.deltas[2].deadline);
    store.put("overdue", {7}, origin + 1ms);
    store.put("later", {8}, origin + 40ms);
    // before 保存补拍前提交序号, 整批过期只应增加一个版本.
    const auto before = store.version();
    store.tick(origin + 50ms);
    CHECK(store.version() == before + 1);
    // expired 提取刚完成的整批删除, 核对所有记录共享相同版本且没有有限截止.
    const auto expired = store.extract(before);
    CHECK(expired.deltas.size() == 3);
    for (const auto& delta : expired.deltas) {
        CHECK(delta.deleted && delta.version == before + 1 && !delta.deadline);
    }
    CHECK(store.snapshot()->data.size() == 1 && store.snapshot()->data.contains("cancel"));
}

// unordered_map 扩容后钩子和 Key 地址必须保持有效; 删除重建不能触发旧租约.
void test_rehash_and_recreation() {

    // store 为本场景独立实例, 显式配置历史和拍间隔, 不借用宿主墙钟作为业务时间.
    Store store(0, 10min, 1ms, origin);
    for (unsigned index = 0; index < 2048; ++index) {
        store.put("ttl/" + std::to_string(index), {1}, origin + 5ms);
    }
    store.remove("ttl/0");
    store.put("ttl/0", {2}, origin + 10ms);
    store.tick(origin + 5ms);
    CHECK(store.snapshot()->data.size() == 1 && store.snapshot()->data.at("ttl/0").value->front() == 2);
    store.tick(origin + 10ms);
    CHECK(store.snapshot()->data.empty());
    store.put("ttl/0", {3}, origin + 11ms);
    store.tick(origin + 11ms);
    CHECK(store.snapshot()->data.empty());
}

// 跨有符号极值的距离不允许溢出; 大间隔把完整边界测试压缩为两拍.
void test_extreme_clock() {

    // interval 使用纳秒时长上界, 用少量补拍覆盖时间算术边界.
    const auto interval = std::chrono::nanoseconds::max();
    // store 为本场景独立实例, 显式配置历史和拍间隔, 不借用宿主墙钟作为业务时间.
    Store store(1000, 10min, interval, Clock::Time{});
    store.put("last", {1}, Clock::Time::max());
    store.put("permanent", {2});
    store.tick(Clock::Time::max() - 1ns);
    CHECK(store.version() == 2);
    store.tick(Clock::Time::max());
    CHECK(store.version() == 3 && store.snapshot()->data.size() == 1);
    CHECK(store.snapshot()->data.contains("permanent"));
    // 最大整数是合法有限截止, 不再充当无限期哨兵.
    Store distant(1000, 10min, 1ns, origin);
    distant.put("distant", {1}, origin + std::chrono::nanoseconds(static_cast<std::int64_t>(Store::Timer::limit + 1)));
    distant.tick(origin + 5ns);
    CHECK(distant.version() == 1 && distant.snapshot()->data.contains("distant"));
}

// 写入, 补拍和快照在不同线程竞争同一 Store 锁, 验证侵入式钩子的全部访问也在同步边界内.
void test_concurrent_ttl() {

    // store 为本场景独立实例, 显式配置历史和拍间隔, 不借用宿主墙钟作为业务时间.
    Store store(1000, 10min, 1ms, origin);
    // errors 各槽由独立线程写, join 后读取; finished 只用于控制读者循环.
    std::array<std::exception_ptr, 2> errors{};
    // finished 标记写入线程已结束, 读者结束后仍 join 两个工作线程再检查异常.
    std::atomic_bool finished{false};
    // writer 持续更新短租约和永久项, 异常保存到 errors[0] 而不逃出线程入口.
    std::jthread writer([&] {
        try {
            for (unsigned index = 0; index < 512; ++index) {
                store.put("ttl/" + std::to_string(index), {1}, origin + 1ms);
                store.put("live", {2});
            }
        } catch (...) {
            errors[0] = std::current_exception();
        }
        finished.store(true);
    });
    // ticker 并发推进 1..200 ms, 异常保存到 errors[1], 不由真实休眠决定 TTL.
    std::jthread ticker([&] {
        try {
            for (unsigned step = 1; step <= 200; ++step) {
                store.tick(origin + std::chrono::milliseconds(step));
            }
        } catch (...) {
            errors[1] = std::current_exception();
        }
    });
    do {
        // snapshot 拥有当前调用捕获的完整版本, 检查期间并发写入不能改变其内容.
        const auto snapshot = store.snapshot();
        for (const auto& [key, value] : snapshot->data) {
            CHECK(value.value && value.value->size() == 1 && value.value->front() == (key == "live" ? 2 : 1));
        }
    } while (!finished.load());
    writer.join();
    ticker.join();
    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    store.tick(origin + 2s);
    CHECK(store.snapshot()->data.size() == 1 && store.snapshot()->data.contains("live"));
}

// 超过原预算的补拍必须一次完成, 不足一拍的截止继续保留, 重复或倒退时间不产生新提交.
void test_complete_catchup() {

    // store 为本场景独立实例, 显式配置历史和拍间隔, 不借用宿主墙钟作为业务时间.
    Store store(1000, 10min, 1ms, origin);
    store.put("boundary", {1}, origin + 1024ms);
    store.put("next", {2}, origin + 1025ms);
    store.put("later", {3}, origin + 2500ms);
    store.put("fraction", {4}, origin + 3000ms + 1ns);
    store.put("permanent", {5});
    // target 含 3000 个完整毫秒拍及 500 ns 余量, 验证补拍完整且保留未到拍的期限.
    const auto target = origin + 3000ms + 500ns;
    store.tick(target);
    // snapshot 拥有当前调用捕获的完整版本, 检查期间并发写入不能改变其内容.
    const auto snapshot = store.snapshot();
    CHECK(snapshot->version == 6 && snapshot->data.size() == 2);
    CHECK(snapshot->data.contains("fraction") && snapshot->data.contains("permanent"));
    // changes 为指定游标之后的完整批次, 检查删除数和版本边界而不依赖同批键顺序.
    const auto changes = store.extract(5);
    CHECK(!changes.stale && changes.version == 6 && changes.deltas.size() == 3);
    // keys 从空集合记录已返回删除键, 插入结果用于检测重复删除.
    std::set<std::string> keys;
    for (const auto& delta : changes.deltas) {
        CHECK(delta.deleted && !delta.value && delta.version == 6);
        CHECK(keys.insert(delta.key).second);
    }
    CHECK(keys.contains("boundary") && keys.contains("next") && keys.contains("later"));
    store.tick(target);
    store.tick(origin);
    CHECK(store.snapshot() == snapshot);
    store.tick(origin + 3001ms);
    CHECK(store.version() == 7 && store.snapshot()->data.size() == 1);
    CHECK(store.snapshot()->data.contains("permanent"));
}

// 模拟一小时未驱动, 补拍前后写入的五秒绝对租约必须在同一真实截止失效.
void test_pause_and_new_lease() {

    // store 为本场景独立实例, 显式配置历史和拍间隔, 不借用宿主墙钟作为业务时间.
    Store store(100, Steady::duration::max(), 10ms, origin);
    // resumed 模拟暂停一小时后的 Unix 时刻, 不实际等待一小时.
    const auto resumed = origin + 1h;
    // deadline 是恢复时刻后的五秒绝对截止, 补拍前后写入必须共享这一时限.
    const auto deadline = resumed + 5s;
    store.put("old", {1}, origin + 1s);
    store.put("before-catchup", {2}, deadline);
    store.tick(resumed);
    CHECK(store.version() == 3 && store.snapshot()->data.size() == 1);
    CHECK(store.snapshot()->data.contains("before-catchup"));
    store.put("after-catchup", {3}, deadline);
    store.tick(deadline - 1ns);
    CHECK(store.version() == 4 && store.snapshot()->data.size() == 2);
    store.tick(deadline);
    CHECK(store.version() == 5 && store.snapshot()->data.empty());
    // changes 为指定游标之后的完整批次, 检查删除数和版本边界而不依赖同批键顺序.
    const auto changes = store.extract(4);
    CHECK(!changes.stale && changes.deltas.size() == 2);
    for (const auto& delta : changes.deltas) {
        CHECK(delta.deleted && delta.version == 5);
    }
}

// 用独立 Map 和提交日志对照随机写入,删除,补拍及游标续传, 固定种子保证可复现.
void test_store_model() {

    // capacity 为模型与 Store 共用的八批历史上限, 不是键数量上限.
    constexpr std::uint64_t capacity = 8;
    for (const auto seed : {11U, 29U, 71U}) {
        // store 为本场景独立实例, 显式配置历史和拍间隔, 不借用宿主墙钟作为业务时间.
        Store store(capacity, Steady::duration::max(), 1ms, origin);
        // state 为独立模型, 每键保存字节和到期毫秒拍, UINT64_MAX 仅在模型内表示永久.
        std::map<std::string, std::pair<Store::Buffer, std::uint64_t>> state;
        // log 保留模型的全部提交, 与有界 Store 历史分开, 用于核对任意有效游标.
        std::vector<Store::Delta> log;
        // version 从零记录模型已提交批次, 多键同拍过期只递增一次.
        std::uint64_t version = 0;
        // observed 为模型已推进的毫秒拍数, 初始零, 只在随机推进分支递增.
        std::uint64_t observed = 0;
        std::mt19937_64 random(seed);
        // at 把模型 tick 换算为相同 origin 下的 Unix 毫秒时间, 不读取系统时钟.
        const auto at = [](std::uint64_t tick) { return origin + std::chrono::milliseconds(static_cast<std::int64_t>(tick)); };
        // check_snapshot 借用 state/version, 按全部有效键对照快照内容和版本.
        const auto check_snapshot = [&](const auto& snapshot) {
            CHECK(snapshot->version == version && snapshot->data.size() == state.size());
            for (const auto& [key, item] : state) {
                CHECK(snapshot->data.contains(key) && *snapshot->data.at(key).value == item.first);
            }
        };
        for (unsigned step = 0; step < 2000; ++step) {
            // key 从固定 32 键空间选取, 强制频繁覆盖,删除和复用同一节点.
            const auto key = "model/" + std::to_string(random() % 32);
            switch (random() % 4) {
            case 0: {
                const Store::Buffer value{static_cast<std::uint8_t>(random() % 256)};
                // expiry 四分之一为模型永久哨兵, 其余为 observed 后 0..23 拍.
                const auto expiry = random() % 4 == 0 ? UINT64_MAX : observed + random() % 24;
                // deadline 将模型哨兵转换成空值, 有限期限转换成绝对 Unix 时间.
                const std::optional<Clock::Time> deadline = expiry == UINT64_MAX ? std::nullopt : std::optional{at(expiry)};
                store.put(key, value, deadline);
                state.insert_or_assign(key, std::pair{value, expiry});
                log.emplace_back(key, std::make_shared<const Store::Buffer>(value), false, ++version, deadline);
                break;
            }
            case 1:
                store.remove(key);
                if (state.erase(key) != 0) {
                    log.emplace_back(key, nullptr, true, ++version);
                }
                break;
            case 2: {
                // previous 保存推进前拍数, 相同拍调用不能误触发刚写入的零延迟项.
                const auto previous = observed;
                observed += random() % 7;
                store.tick(at(observed));
                // changed 初始 false, 第一条真实过期时建立新批次, 后续删除复用该版本.
                bool changed = false;
                if (observed != previous) {
                    for (auto entry = state.begin(); entry != state.end();) {
                        if (entry->second.second <= observed) {
                            if (!changed) {
                                ++version;
                                changed = true;
                            }
                            log.emplace_back(entry->first, nullptr, true, version);
                            entry = state.erase(entry);
                        } else {
                            ++entry;
                        }
                    }
                }
                break;
            }
            default:
                store.tick(at(observed));
                store.tick(at(observed) - 1ms);
                break;
            }
            CHECK(store.version() == version);
            // snapshot 拥有当前调用捕获的完整版本, 检查期间并发写入不能改变其内容.
            const auto snapshot = store.snapshot();
            check_snapshot(snapshot);
            CHECK(store.snapshot() == snapshot);

            // cursor 随机覆盖有效后缀,历史不足及超前一版, 不仅测试顺序追赶.
            const auto cursor = random() % 4 == 0 ? version + 1 : version - random() % std::min(version + 1, capacity + 2);
            // result 为实际增量提取结果, 先检查 stale 再比较记录集合.
            const auto result = store.extract(cursor);
            // oldest 按模型批次数计算可续传下界, 下界本身仍是合法游标.
            const auto oldest = version > capacity ? version - capacity : 0;
            CHECK(result.version == version && result.stale == (cursor > version || cursor < oldest));
            if (result.stale) {
                CHECK(result.deltas.empty());
                continue;
            }

            // remaining 保存应返回的版本/键组合, 忽略同批顺序但拒绝重复或遗漏.
            std::set<std::pair<std::uint64_t, std::string>> remaining;
            for (const auto& delta : log) {
                if (delta.version > cursor) {
                    CHECK(remaining.emplace(delta.version, delta.key).second);
                }
            }

            // previous 跟踪返回版本的非递减顺序, 初始为调用方已确认游标.
            auto previous = cursor;
            for (const auto& delta : result.deltas) {
                CHECK(delta.version >= previous && remaining.erase({delta.version, delta.key}) == 1);
                previous = delta.version;
                // expected 在独立模型完整日志中匹配当前版本/键, 不以被测实现结果推导期望.
                const auto expected = std::ranges::find_if(log, [&](const auto& item) { return item.version == delta.version && item.key == delta.key; });
                CHECK(expected != log.end() && delta.deleted == expected->deleted && delta.deadline == expected->deadline);
                CHECK(static_cast<bool>(delta.value) == static_cast<bool>(expected->value));
                if (delta.value) {
                    CHECK(*delta.value == *expected->value);
                }
            }
            CHECK(remaining.empty());
        }
    }
}
} // namespace

// 独立进程执行全部边界用例, 任一断言失败都以非零退出码结束.
int main() {

    try {
        test_interval();
        test_precision_and_clock();
        test_renewal_and_catchup();
        test_rehash_and_recreation();
        test_extreme_clock();
        test_concurrent_ttl();
        test_complete_catchup();
        test_pause_and_new_lease();
        test_store_model();
        std::cout << "Store TTL clock, renewal, batch, rehash and extreme boundaries passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
