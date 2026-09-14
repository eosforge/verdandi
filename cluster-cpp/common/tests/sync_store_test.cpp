// 功能: 验证快照隔离, 完整批次历史, 过期边界和并发读取的版本一致性.
#include "check.hpp"
#include "sync_store.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <thread>

using namespace verdandi::cluster;
using namespace std::chrono_literals;

// 写入和删除不修改已经交给读者的快照. 相同版本复用缓存, 普通值传递隔离外部缓冲.
void test_put_and_remove() {
    SyncStore store;
    CHECK(store.global_version() == 0);

    std::vector<uint8_t> payload1{1, 2, 3};
    store.put("key1", payload1);
    payload1[0] = 9;

    CHECK(store.global_version() == 1);

    auto snapshot1 = store.get_snapshot();
    CHECK(snapshot1->global_version == 1);
    CHECK(snapshot1->data.size() == 1);
    CHECK(snapshot1->data.at("key1")->at(0) == 1);
    CHECK(store.get_snapshot() == snapshot1);

    store.remove("key1");
    CHECK(store.global_version() == 2);

    auto snapshot2 = store.get_snapshot();
    CHECK(snapshot2->global_version == 2);
    CHECK(snapshot2->data.size() == 0);
    CHECK(snapshot1->data.size() == 1);
    store.remove("key1");
    CHECK(store.global_version() == 2);
}

// 相等的单调截止必须到期, Catalog 的 max 截止不随普通租约清理.
void test_evict_expired() {
    SyncStore store;

    auto now = std::chrono::steady_clock::now();
    std::vector<uint8_t> payload1{1};
    std::vector<uint8_t> payload2{2};

    store.put("key1", std::move(payload1), now + 10s);
    store.put("key2", std::move(payload2), Clock::time_point::max());

    CHECK(store.global_version() == 2);

    store.evict_expired(now + 5s);
    CHECK(store.global_version() == 2);

    store.evict_expired(now + 10s);
    CHECK(store.global_version() == 3);

    auto snapshot = store.get_snapshot();
    CHECK(snapshot->global_version == 3);
    CHECK(snapshot->data.size() == 1);
    CHECK(snapshot->data.find("key2") != snapshot->data.end());
    CHECK(snapshot->data.find("key1") == snapshot->data.end());
}

// 淘汰边界为最后淘汰的完整批次, 未来游标必须显式回退到快照.
void test_extract_since_and_tombstone_gc() {
    SyncStore store(2);

    std::vector<uint8_t> payload;

    store.put("k1", payload);
    store.put("k2", payload);
    store.remove("k1");

    auto res1 = store.extract_since(2);
    CHECK(!res1.require_snapshot);
    CHECK(res1.current_version == 3);
    CHECK(res1.deltas.size() == 1);
    CHECK(res1.deltas[0].deleted);

    auto res2 = store.extract_since(1);
    CHECK(!res2.require_snapshot);
    CHECK(res2.current_version == 3);
    CHECK(res2.deltas.size() == 2);

    auto res3 = store.extract_since(0);
    CHECK(res3.require_snapshot);

    auto res_future = store.extract_since(100);
    CHECK(res_future.require_snapshot);
}

// 同次过期删除不能因缓存容量被截成部分批次, 重新写入的 Key 不受旧墓碑清理影响.
void test_batches_and_recreation() {
    SyncStore store(1);
    store.put("a", {1}, Clock::time_point{});
    store.put("b", {2}, Clock::time_point{});
    store.evict_expired(Clock::now());
    const auto expired = store.extract_since(2);
    CHECK(!expired.require_snapshot && expired.current_version == 3 && expired.deltas.size() == 2);
    for (const auto& delta : expired.deltas) {
        CHECK(delta.deleted && delta.field_version == 3);
    }
    store.put("a", {3});
    CHECK(store.extract_since(2).require_snapshot);
    CHECK(store.get_snapshot()->data.at("a")->at(0) == 3);
    CHECK(!store.get_snapshot()->data.contains("b"));

    // 零历史模式仍保留当前状态, 只有恰好位于当前提交的客户端可以免快照.
    SyncStore no_history(0);
    CHECK(!no_history.extract_since(0).require_snapshot);
    no_history.put("a", {1});
    CHECK(no_history.extract_since(0).require_snapshot);
    CHECK(!no_history.extract_since(1).require_snapshot);
    no_history.remove("a");
    no_history.put("a", {2});
    CHECK(no_history.get_snapshot()->data.at("a")->at(0) == 2);
}

// 用唯一 Key 的提交号编码载荷, 并发快照必须同时读到同一个版本及其内容.
void test_concurrent_snapshots() {
    SyncStore store;
    std::atomic_bool finished{false};
    std::exception_ptr writer_error;
    std::jthread writer([&] {
        try {
            for (std::uint16_t value = 1; value <= 200; ++value) {
                store.put("counter", {static_cast<std::uint8_t>(value)});
            }
        } catch (...) {
            writer_error = std::current_exception();
        }
        finished.store(true);
    });
    do {
        const auto snapshot = store.get_snapshot();
        if (snapshot->global_version == 0) {
            CHECK(snapshot->data.empty());
        } else {
            CHECK(snapshot->data.size() == 1);
            CHECK(snapshot->data.at("counter")->at(0) == snapshot->global_version);
        }
    } while (!finished.load());
    writer.join();
    if (writer_error) {
        std::rethrow_exception(writer_error);
    }
    CHECK(store.get_snapshot()->global_version == 200);
}

// 所有断言在 Release 也生效, 异常使 CTest 明确失败.
int main() {
    try {
        test_put_and_remove();
        test_evict_expired();
        test_extract_since_and_tombstone_gc();
        test_batches_and_recreation();
        test_concurrent_snapshots();
        std::cout << "All tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << '\n';
        return 1;
    }
}
