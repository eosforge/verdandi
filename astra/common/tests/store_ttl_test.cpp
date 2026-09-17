// 功能: 用可控单调时间验证 TTL 精度, 补拍, 续租, 取消, 节点稳定性与极值距离.
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
// origin 是本测试的单调时钟起点, 所有时间均显式构造, 不读取墙钟或休眠.
constexpr auto origin = Clock::time_point{};

// 零与负间隔必须在进入调度前拒绝, 防止除零和永不前进的补拍循环.
void test_interval() {
    for (const auto interval : {Clock::duration::zero(), Clock::duration{-1}}) {
        bool rejected = false;
        try {
            Store store(1000, 10min, interval, origin);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

// 不足一拍的余量保留到下次调用; 截止向上取整, 相同时间和倒退时间不会额外推进.
void test_precision_and_clock() {
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
    CHECK(store.snapshot()->data.at("renew")->front() == 4);
    CHECK(store.snapshot()->data.at("cancel")->front() == 6);
    // delta 截止只属于本时钟域, 保留有限值和 max 哨兵以支持本地检查.
    const auto changes = store.extract(3);
    CHECK(changes.deltas[0].expire == origin + 10ms);
    CHECK(changes.deltas[2].expire == Clock::time_point::max());
    store.put("overdue", {7}, origin + 1ms);
    store.put("later", {8}, origin + 40ms);
    const auto before = store.version();
    store.tick(origin + 50ms);
    CHECK(store.version() == before + 1);
    const auto expired = store.extract(before);
    CHECK(expired.deltas.size() == 3);
    for (const auto& delta : expired.deltas) {
        CHECK(delta.deleted && delta.version == before + 1 && delta.expire == Clock::time_point::max());
    }
    CHECK(store.snapshot()->data.size() == 1 && store.snapshot()->data.contains("cancel"));
}

// unordered_map 扩容后钩子和 Key 地址必须保持有效; 删除重建不能触发旧租约.
void test_rehash_and_recreation() {
    Store store(0, 10min, 1ms, origin);
    for (unsigned index = 0; index < 2048; ++index) {
        store.put("ttl/" + std::to_string(index), {1}, origin + 5ms);
    }
    store.remove("ttl/0");
    store.put("ttl/0", {2}, origin + 10ms);
    store.tick(origin + 5ms);
    CHECK(store.snapshot()->data.size() == 1 && store.snapshot()->data.at("ttl/0")->front() == 2);
    store.tick(origin + 10ms);
    CHECK(store.snapshot()->data.empty());
    store.put("ttl/0", {3}, origin + 11ms);
    store.tick(origin + 11ms);
    CHECK(store.snapshot()->data.empty());
}

// 跨有符号极值的距离不允许溢出; 大间隔把完整边界测试压缩为两拍.
void test_extreme_clock() {
    const auto interval = Clock::duration::max();
    Store store(1000, 10min, interval, Clock::time_point::min());
    store.put("last", {1}, Clock::time_point::max() - Clock::duration{1});
    store.put("permanent", {2});
    store.tick(Clock::time_point{});
    CHECK(store.version() == 2);
    store.tick(Clock::time_point::max());
    CHECK(store.version() == 3 && store.snapshot()->data.size() == 1);
    CHECK(store.snapshot()->data.contains("permanent"));
    // direct 在一次调用中跨越整个有符号时钟域, 仍仅补两拍, 避免遍历 UINT64_MAX 拍.
    Store direct(1000, 10min, interval, Clock::time_point::min());
    direct.put("last", {1}, Clock::time_point::max() - Clock::duration{1});
    direct.put("permanent", {2});
    direct.tick(Clock::time_point::max());
    CHECK(direct.version() == 3 && direct.snapshot()->data.size() == 1);
    CHECK(direct.snapshot()->data.contains("permanent"));
    // 远超单轮上限的有限截止只可分段唤醒, 不应被裁剪成近期过期或转换为无限租约.
    Store distant(1000, 10min, 1ns, origin);
    distant.put("distant", {1}, origin + Clock::duration{static_cast<Clock::duration::rep>(Store::Timer::limit + 1)});
    distant.tick(origin + 5ns);
    CHECK(distant.version() == 1 && distant.snapshot()->data.contains("distant"));
}

// 写入, 补拍和快照在不同线程竞争同一 Store 锁, 验证侵入式钩子的全部访问也在同步边界内.
void test_concurrent_ttl() {
    Store store(1000, 10min, 1ms, origin);
    // errors 各槽由独立线程写, join 后读取; finished 只用于控制读者循环.
    std::array<std::exception_ptr, 2> errors{};
    std::atomic_bool finished{false};
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
        const auto snapshot = store.snapshot();
        for (const auto& [key, value] : snapshot->data) {
            CHECK(value && value->size() == 1 && value->front() == (key == "live" ? 2 : 1));
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
    Store store(1000, 10min, 1ms, origin);
    store.put("boundary", {1}, origin + 1024ms);
    store.put("next", {2}, origin + 1025ms);
    store.put("later", {3}, origin + 2500ms);
    store.put("fraction", {4}, origin + 3000ms + 1ns);
    store.put("permanent", {5});
    const auto target = origin + 3000ms + 500ns;
    store.tick(target);
    const auto snapshot = store.snapshot();
    CHECK(snapshot->version == 6 && snapshot->data.size() == 2);
    CHECK(snapshot->data.contains("fraction") && snapshot->data.contains("permanent"));
    const auto changes = store.extract(5);
    CHECK(!changes.stale && changes.version == 6 && changes.deltas.size() == 3);
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
    Store store(100, Clock::duration::max(), 10ms, origin);
    const auto resumed = origin + 1h;
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
    const auto changes = store.extract(4);
    CHECK(!changes.stale && changes.deltas.size() == 2);
    for (const auto& delta : changes.deltas) {
        CHECK(delta.deleted && delta.version == 5);
    }
}
void test_store_model() {
    constexpr std::uint64_t capacity = 8;
    for (const auto seed : {11U, 29U, 71U}) {
        Store store(capacity, Clock::duration::max(), 1ms, origin);
        std::map<std::string, std::pair<Store::Buffer, std::uint64_t>> state;
        std::vector<Store::Delta> log;
        std::uint64_t version = 0;
        std::uint64_t observed = 0;
        std::mt19937_64 random(seed);
        const auto at = [](std::uint64_t tick) { return origin + std::chrono::milliseconds(static_cast<std::int64_t>(tick)); };
        const auto check_snapshot = [&](const auto& snapshot) {
            CHECK(snapshot->version == version && snapshot->data.size() == state.size());
            for (const auto& [key, item] : state) {
                CHECK(snapshot->data.contains(key) && *snapshot->data.at(key) == item.first);
            }
        };
        for (unsigned step = 0; step < 2000; ++step) {
            const auto key = "model/" + std::to_string(random() % 32);
            switch (random() % 4) {
            case 0: {
                const Store::Buffer value{static_cast<std::uint8_t>(random() % 256)};
                const auto expiry = random() % 4 == 0 ? UINT64_MAX : observed + random() % 24;
                const auto deadline = expiry == UINT64_MAX ? Clock::time_point::max() : at(expiry);
                store.put(key, value, deadline);
                state.insert_or_assign(key, std::pair{value, expiry});
                log.emplace_back(key, std::make_shared<const Store::Buffer>(value), false, ++version, deadline);
                break;
            }
            case 1:
                store.remove(key);
                if (state.erase(key) != 0) {
                    log.emplace_back(key, nullptr, true, ++version, Clock::time_point::max());
                }
                break;
            case 2: {
                const auto previous = observed;
                observed += random() % 7;
                store.tick(at(observed));
                bool changed = false;
                if (observed != previous) {
                    for (auto entry = state.begin(); entry != state.end();) {
                        if (entry->second.second <= observed) {
                            if (!changed) {
                                ++version;
                                changed = true;
                            }
                            log.emplace_back(entry->first, nullptr, true, version, Clock::time_point::max());
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
            const auto snapshot = store.snapshot();
            check_snapshot(snapshot);
            CHECK(store.snapshot() == snapshot);

            const auto cursor = random() % 4 == 0 ? version + 1 : version - random() % std::min(version + 1, capacity + 2);
            const auto result = store.extract(cursor);
            const auto oldest = version > capacity ? version - capacity : 0;
            CHECK(result.version == version && result.stale == (cursor > version || cursor < oldest));
            if (result.stale) {
                CHECK(result.deltas.empty());
                continue;
            }
            std::set<std::pair<std::uint64_t, std::string>> remaining;
            for (const auto& delta : log) {
                if (delta.version > cursor) {
                    CHECK(remaining.emplace(delta.version, delta.key).second);
                }
            }
            auto previous = cursor;
            for (const auto& delta : result.deltas) {
                CHECK(delta.version >= previous && remaining.erase({delta.version, delta.key}) == 1);
                previous = delta.version;
                const auto expected = std::ranges::find_if(log, [&](const auto& item) { return item.version == delta.version && item.key == delta.key; });
                CHECK(expected != log.end() && delta.deleted == expected->deleted && delta.expire == expected->expire);
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
