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
    // store 使用默认历史容量, 初始版本应为零.
    SyncStore store;
    CHECK(store.global_version() == 0);

    // payload1 保留在调用方并随后修改, 验证按值调用的缓冲隔离.
    std::vector<std::uint8_t> payload1{1, 2, 3};
    store.put("key1", payload1);
    payload1[0] = 9;

    CHECK(store.global_version() == 1);

    // snapshot1 跨越后续删除继续持有旧版本, 检查缓存身份和旧值不变.
    const auto snapshot1 = store.get_snapshot();
    CHECK(snapshot1->global_version == 1);
    CHECK(snapshot1->data.size() == 1);
    CHECK(snapshot1->data.at("key1")->at(0) == 1);
    CHECK(store.get_snapshot() == snapshot1);

    store.remove("key1");
    CHECK(store.global_version() == 2);

    // snapshot2 代表删除后的当前状态, 不允许修改 snapshot1 的 Map.
    const auto snapshot2 = store.get_snapshot();
    CHECK(snapshot2->global_version == 2);
    CHECK(snapshot2->data.size() == 0);
    CHECK(snapshot1->data.size() == 1);
    store.remove("key1");
    CHECK(store.global_version() == 2);
}

// 相等的单调截止必须到期, Catalog 的 max 截止不随普通租约清理.
void test_evict_expired() {
    // store 同时持有有限租约和永不过期数据, 验证两者使用同一个清理入口.
    SyncStore store;

    // now 为固定测试基准, 不依赖实际等待或调度速度.
    const auto now = Clock::now();

    store.put("key1", {1}, now + 10s);
    store.put("key2", {2}, Clock::time_point::max());

    CHECK(store.global_version() == 2);

    store.evict_expired(now + 5s);
    CHECK(store.global_version() == 2);

    store.evict_expired(now + 10s);
    CHECK(store.global_version() == 3);

    // snapshot 确认截止相等时只删除有限租约 Key.
    const auto snapshot = store.get_snapshot();
    CHECK(snapshot->global_version == 3);
    CHECK(snapshot->data.size() == 1);
    CHECK(snapshot->data.find("key2") != snapshot->data.end());
    CHECK(snapshot->data.find("key1") == snapshot->data.end());

    // 极值 now 仍不能删除 max 哨兵, 而真实的最大有限截止应当到期.
    store.put("last_finite", {3}, Clock::time_point::max() - Clock::duration{1});
    store.evict_expired(Clock::time_point::max());
    CHECK(store.global_version() == 5);
    CHECK(store.get_snapshot()->data.size() == 1);
    CHECK(store.get_snapshot()->data.contains("key2"));
    store.evict_expired(Clock::time_point::max());
    CHECK(store.global_version() == 5);
}

// 淘汰边界为最后淘汰的完整批次, 未来游标必须显式回退到快照.
void test_extract_since_and_history_trim() {
    // store 只保留两个批次, 第三次提交会把最旧续传边界推进到 1.
    SyncStore store(2);

    store.put("k1", {});
    store.put("k2", {});
    store.remove("k1");

    // res1 只包含最后一次删除, 不重复发送游标本身所在的批次.
    const auto res1 = store.extract_since(2);
    CHECK(!res1.require_snapshot);
    CHECK(res1.current_version == 3);
    CHECK(res1.deltas.size() == 1);
    CHECK(res1.deltas[0].deleted);

    // res2 恰好位于淘汰边界, 仍应取得后续两个完整批次.
    const auto res2 = store.extract_since(1);
    CHECK(!res2.require_snapshot);
    CHECK(res2.current_version == 3);
    CHECK(res2.deltas.size() == 2);

    // res3 已超出历史窗口, 必须要求快照, 不能把残余历史当作完整结果.
    const auto res3 = store.extract_since(0);
    CHECK(res3.require_snapshot);

    // res_future 不属于当前已提交范围, 与历史不足一样显式回退.
    const auto res_future = store.extract_since(100);
    CHECK(res_future.require_snapshot);
}

// 同次过期删除不能因缓存容量被截成部分批次, 重新写入的 Key 不受旧删除记录淘汰影响.
void test_batches_and_recreation() {
    // store 的容量是批次数而非记录数, 一批两个删除都必须保留.
    SyncStore store(1);
    store.put("a", {1}, Clock::time_point{});
    store.put("b", {2}, Clock::time_point{});
    store.evict_expired(Clock::now());
    // expired 持有历史批次副本, 后续淘汰不影响这个结果的生命周期.
    const auto expired = store.extract_since(2);
    CHECK(!expired.require_snapshot && expired.current_version == 3 && expired.deltas.size() == 2);
    // delta 的 Key 顺序不固定, 但同批版本和删除语义必须相同.
    for (const auto& delta : expired.deltas) {
        CHECK(delta.deleted && delta.field_version == 3);
    }
    store.put("a", {3});
    CHECK(store.extract_since(2).require_snapshot);
    CHECK(store.get_snapshot()->data.at("a")->at(0) == 3);
    CHECK(!store.get_snapshot()->data.contains("b"));

    // 零历史模式仍保留当前状态, 只有恰好位于当前提交的客户端可以免快照.
    // no_history 验证立即淘汰历史不误删当前有效值.
    SyncStore no_history(0);
    CHECK(!no_history.extract_since(0).require_snapshot);
    no_history.put("a", {1});
    CHECK(no_history.extract_since(0).require_snapshot);
    CHECK(!no_history.extract_since(1).require_snapshot);
    no_history.remove("a");
    no_history.put("a", {2});
    CHECK(no_history.get_snapshot()->data.at("a")->at(0) == 2);

    // reused 覆盖同 Key 删除、重建、再删除后淘汰第一次删除记录, 不依赖空节点是否仍缓存.
    SyncStore reused(2);
    reused.put("a", {1});
    reused.remove("a");
    reused.put("a", {2});
    reused.remove("a");
    CHECK(reused.get_snapshot()->data.empty());
    reused.remove("a");
    CHECK(reused.global_version() == 4);
    reused.put("a", {3});
    // replay 必须保留第二次删除和最后重建, 不能受缓存节点回收时机影响.
    const auto replay = reused.extract_since(3);
    CHECK(!replay.require_snapshot && replay.deltas.size() == 2);
    CHECK(replay.deltas.front().deleted && replay.deltas.front().field_version == 4);
    CHECK(!replay.deltas.back().deleted && replay.deltas.back().payload->front() == 3);
}

// 覆盖跨 deque 分段的查找、精确游标边界及长历史后缀, 不依赖内部迭代器布局.
void test_long_history_suffix() {
    // store 共提交 1128 次, 只保留最后 1000 个批次; Key 循环覆盖而提交号持续增长.
    SyncStore store(1000);
    // version 同时编码 Key 和 payload, 使漏批次、重复批次和错序都能独立检出.
    for (std::uint64_t version = 1; version <= 1128; ++version) {
        store.put("key/" + std::to_string(version % 32), {static_cast<std::uint8_t>(version % 251)});
    }
    CHECK(store.extract_since(127).require_snapshot);
    CHECK(store.extract_since(1129).require_snapshot);
    // cursor 覆盖最早可续传位置、中间位置、只差一个批次和已经同步的位置.
    for (const std::uint64_t cursor : {128U, 129U, 499U, 1000U, 1127U, 1128U}) {
        // result 必须精确覆盖 (cursor, 1128], 不能越过历史窗口或提交屏障.
        const auto result = store.extract_since(cursor);
        CHECK(!result.require_snapshot && result.current_version == 1128);
        CHECK(result.deltas.size() == 1128 - cursor);
        // expected 是外部可观察提交序列, 不使用被测容器的定位算法生成期望值.
        auto expected = cursor + 1;
        for (const auto& delta : result.deltas) {
            CHECK(delta.field_version == expected && !delta.deleted);
            CHECK(delta.key == "key/" + std::to_string(expected % 32));
            CHECK(delta.payload->at(0) == expected % 251);
            ++expected;
        }
    }
}

// 返回值必须独立拥有 Key 和载荷引用, 即使 Store 和其中全部历史已经被销毁.
void test_result_lifetime() {
    // snapshot 和 delta 被保留到 Store 作用域之外, 暴露任何错误的借用或悬垂引用.
    std::shared_ptr<const Snapshot> snapshot;
    SyncResult delta;
    {
        // store 的空 Key 和空值仍是有效写入, 不能与删除混淆.
        SyncStore store;
        store.put("", {});
        snapshot = store.get_snapshot();
        delta = store.extract_since(0);
        store.remove("");
    }
    CHECK(snapshot->global_version == 1 && snapshot->data.at("")->empty());
    CHECK(delta.current_version == 1 && delta.deltas.size() == 1);
    CHECK(!delta.deltas.front().deleted && delta.deltas.front().payload->empty());
}

// 用唯一 Key 的提交号编码载荷, 并发快照必须同时读到同一个版本及其内容.
void test_concurrent_snapshots() {
    // store 的每个提交只有一个字节, 值本身就是期望版本.
    SyncStore store;
    // finished 只协调测试退出; Store 的同步必须由其自身的锁完成.
    std::atomic_bool finished{false};
    // writer_error 由写线程设置, 主线程 join 后读取, 不跨线程逃逸异常.
    std::exception_ptr writer_error;
    // writer 由 jthread 保证异常展开时也会 join, 防止后台线程继续访问已析构的 Store.
    std::jthread writer([&] {
        try {
            // value 限制在 uint8_t 可表示范围内, 不让回绕遮蔽版本不一致.
            for (std::uint16_t value = 1; value <= 200; ++value) {
                store.put("counter", {static_cast<std::uint8_t>(value)});
            }
        } catch (...) {
            writer_error = std::current_exception();
        }
        finished.store(true);
    });
    do {
        // snapshot 在写入同时读取, 内容必须与同一次加锁取得的版本严格一致.
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
        test_extract_since_and_history_trim();
        test_batches_and_recreation();
        test_long_history_suffix();
        test_result_lifetime();
        test_concurrent_snapshots();
        std::cout << "All tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        // error 包含首个失败原因, 保留非零退出码供 CTest 判定失败.
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
