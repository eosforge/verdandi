#include "check.hpp"
#include "store.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <thread>

using namespace astra;
using namespace std::chrono_literals;

namespace {

// 写入和删除不修改已经交给读者的快照. 相同版本复用缓存, 普通值传递隔离外部缓冲.
void test_put_and_remove() {

    // store 使用默认历史容量, 初始版本应为零.
    Store store;
    CHECK(store.version() == 0);

    // payload1 保留在调用方并随后修改, 验证按值调用的缓冲隔离.
    std::vector<std::uint8_t> payload1{1, 2, 3};
    store.put("key1", payload1);
    payload1[0] = 9;

    CHECK(store.version() == 1);

    // snapshot1 跨越后续删除继续持有旧版本, 检查缓存身份和旧值不变.
    const auto snapshot1 = store.snapshot();
    CHECK(snapshot1->version == 1);
    CHECK(snapshot1->data.size() == 1);
    CHECK(snapshot1->data.at("key1").value->at(0) == 1);
    CHECK(store.snapshot() == snapshot1);

    store.remove("key1");
    CHECK(store.version() == 2);

    // snapshot2 代表删除后的当前状态, 不允许修改 snapshot1 的 Map.
    const auto snapshot2 = store.snapshot();
    CHECK(snapshot2->version == 2);
    CHECK(snapshot2->data.size() == 0);
    CHECK(snapshot1->data.size() == 1);
    store.remove("key1");
    CHECK(store.version() == 2);
}

// 相等的业务纪元截止必须到期, 空截止数据不随有限租约清理.
void test_evict_expired() {

    // now 是本场景的确定性纪元起点, 同时安排有限租约和空截止数据.
    const auto now = Clock::Time{};
    // store 独立持有本场景状态, 构造参数显式指定所需历史预算和时间规则.
    Store store(1000, 10min, 1s, now);

    store.put("key1", {1}, now + 10s);
    store.put("key2", {2});

    CHECK(store.version() == 2);

    store.tick(now + 5s);
    CHECK(store.version() == 2);

    store.tick(now + 10s);
    CHECK(store.version() == 3);

    // snapshot 确认截止相等时只删除有限租约 Key.
    const auto snapshot = store.snapshot();
    CHECK(snapshot->version == 3);
    CHECK(snapshot->data.size() == 1);
    CHECK(snapshot->data.find("key2") != snapshot->data.end());
    CHECK(snapshot->data.find("key1") == snapshot->data.end());

    // 极值 now 仍不能删除空截止数据, 而接近最大坐标的有限截止应当到期.
    // 只推进极值附近的两拍, 不用从普通时刻补数十亿空拍来覆盖相同边界.
    Store edge(1000, 10min, Steady::duration{1}, Clock::Time::max() - Steady::duration{2});
    edge.put("permanent", {2});
    edge.put("last_finite", {3}, Clock::Time::max() - Steady::duration{1});
    edge.tick(Clock::Time::max());
    CHECK(edge.version() == 3);
    CHECK(edge.snapshot()->data.size() == 1);
    CHECK(edge.snapshot()->data.contains("permanent"));
    edge.tick(Clock::Time::max());
    CHECK(edge.version() == 3);
}

// 淘汰边界为最后淘汰的完整批次, 未来游标必须显式回退到快照.
void test_extract_since_and_history_trim() {

    // store 只保留两个批次, 第三次提交会把最旧续传边界推进到 1.
    Store store(2);

    store.put("k1", {});
    store.put("k2", {});
    store.remove("k1");

    // res1 只包含最后一次删除, 不重复发送游标本身所在的批次.
    const auto res1 = store.extract(2);
    CHECK(!res1.stale);
    CHECK(res1.version == 3);
    CHECK(res1.deltas.size() == 1);
    CHECK(res1.deltas[0].deleted);

    // res2 恰好位于淘汰边界, 仍应取得后续两个完整批次.
    const auto res2 = store.extract(1);
    CHECK(!res2.stale);
    CHECK(res2.version == 3);
    CHECK(res2.deltas.size() == 2);

    // res3 已超出历史窗口, 必须要求快照, 不能把残余历史当作完整结果.
    const auto res3 = store.extract(0);
    CHECK(res3.stale);

    // res_future 不属于当前已提交范围, 与历史不足一样显式回退.
    const auto res_future = store.extract(100);
    CHECK(res_future.stale);
}

// 同次过期删除不能因缓存容量被截成部分批次, 重新写入的 Key 不受旧删除记录淘汰影响.
void test_batches_and_recreation() {

    // store 的容量是批次数而非记录数, 一批两个删除都必须保留.
    Store store(1, 10min, 1ms, Clock::Time{});
    store.put("a", {1}, Clock::Time{});
    store.put("b", {2}, Clock::Time{});
    store.tick(Clock::Time{} + 1ms);
    // expired 持有历史批次副本, 后续淘汰不影响这个结果的生命周期.
    const auto expired = store.extract(2);
    CHECK(!expired.stale && expired.version == 3 && expired.deltas.size() == 2);
    // delta 的 Key 顺序不固定, 但同批版本和删除语义必须相同.
    for (const auto& delta : expired.deltas) {
        CHECK(delta.deleted && delta.version == 3);
    }
    store.put("a", {3});
    CHECK(store.extract(2).stale);
    CHECK(store.snapshot()->data.at("a").value->at(0) == 3);
    CHECK(!store.snapshot()->data.contains("b"));

    // 零历史模式仍保留当前状态, 只有恰好位于当前提交的客户端可以免快照.
    // no_history 验证立即淘汰历史不误删当前有效值.
    Store no_history(0);
    CHECK(!no_history.extract(0).stale);
    no_history.put("a", {1});
    CHECK(no_history.extract(0).stale);
    CHECK(!no_history.extract(1).stale);
    no_history.remove("a");
    no_history.put("a", {2});
    CHECK(no_history.snapshot()->data.at("a").value->at(0) == 2);

    // reused 覆盖同 Key 删除, 重建, 再删除后淘汰第一次删除记录, 不依赖空节点是否仍缓存.
    Store reused(2);
    reused.put("a", {1});
    reused.remove("a");
    reused.put("a", {2});
    reused.remove("a");
    CHECK(reused.snapshot()->data.empty());
    reused.remove("a");
    CHECK(reused.version() == 4);
    reused.put("a", {3});
    // replay 必须保留第二次删除和最后重建, 不能受缓存节点回收时机影响.
    const auto replay = reused.extract(3);
    CHECK(!replay.stale && replay.deltas.size() == 2);
    CHECK(replay.deltas.front().deleted && replay.deltas.front().version == 4);
    CHECK(!replay.deltas.back().deleted && replay.deltas.back().value->front() == 3);
}

// 覆盖跨 deque 分段的查找, 精确游标边界及长历史后缀, 不依赖内部迭代器布局.
void test_long_history_suffix() {

    // store 共提交 1128 次, 只保留最后 1000 个批次; Key 循环覆盖而提交号持续增长.
    Store store(1000);
    // version 同时编码 Key 和 value, 使漏批次, 重复批次和错序都能独立检出.
    for (std::uint64_t version = 1; version <= 1128; ++version) {
        store.put("key/" + std::to_string(version % 32), {static_cast<std::uint8_t>(version % 251)});
    }
    CHECK(store.extract(127).stale);
    CHECK(store.extract(1129).stale);
    // cursor 覆盖最早可续传位置, 中间位置, 只差一个批次和已经同步的位置.
    for (const std::uint64_t cursor : {128U, 129U, 499U, 1000U, 1127U, 1128U}) {
        // result 必须精确覆盖 (cursor, 1128], 不能越过历史窗口或提交屏障.
        const auto result = store.extract(cursor);
        CHECK(!result.stale && result.version == 1128);
        CHECK(result.deltas.size() == 1128 - cursor);
        // expected 是外部可观察提交序列, 不使用被测容器的定位算法生成期望值.
        auto expected = cursor + 1;
        for (const auto& delta : result.deltas) {
            CHECK(delta.version == expected && !delta.deleted);
            CHECK(delta.key == "key/" + std::to_string(expected % 32));
            CHECK(delta.value->at(0) == expected % 251);
            ++expected;
        }
    }
}

// 提取预算覆盖数组和长 Key, 拒绝保持原历史/快照, 不把半个批次伪装成完整提交.
void test_extract_budget() {

    // 单条长 Key 强制触发字符串复制; 大 Value 应继续共享, 不消耗本次复制预算.
    Store store;
    const std::string key(256, 'k');
    store.put(key, Store::Buffer(4096, 7));
    // previous 保留提取前的完整视图, 失败和后续写入均不能修改其载荷.
    const auto previous = store.snapshot();
    // exact 是一条 Delta 数组元素和完整键字节含终止符的最小复制预算.
    const auto exact = sizeof(Store::Delta) + key.size() + 1;
    // extracted 使用刚好够用的预算, 验证键复制计费与 Value 共享同时成立.
    const auto extracted = store.extract(0, exact);
    CHECK(extracted.version == 1 && extracted.deltas.size() == 1 && extracted.deltas.front().value == previous->data.at(key).value);
    for (const auto budget : {std::size_t{0}, sizeof(Store::Delta) - 1, exact - 1}) {
        // rejected 初始 false, 只有预期参数或预算异常才置 true.
        bool rejected = false;
        try {
            static_cast<void>(store.extract(0, budget));
        } catch (const std::length_error&) {
            rejected = true;
        }
        CHECK(rejected && store.version() == 1 && store.snapshot() == previous);
    }
    CHECK(store.extract(0, exact).deltas.size() == 1);
    CHECK(store.extract(1, 0).deltas.empty() && !store.extract(1, 0).stale);
    CHECK(store.extract(2, 0).stale);
    // 多批次共用一个总预算; 游标跳过已确认前缀后, 后缀可以在同一预算内继续提取.
    store.put(key, {8});
    // multiple_rejected 只记录多批次总预算不足的明确异常, 防止按单批分别计费.
    bool multiple_rejected = false;
    try {
        static_cast<void>(store.extract(0, exact));
    } catch (const std::length_error&) {
        multiple_rejected = true;
    }
    CHECK(multiple_rejected && store.version() == 2 && previous->data.at(key).value->front() == 7);
    // suffix 跳过已确认前缀后应在相同预算内成功, 只包含第二次更新.
    const auto suffix = store.extract(1, exact);
    CHECK(suffix.deltas.size() == 1 && suffix.deltas.front().version == 2 && suffix.deltas.front().value->front() == 8);

    // 一个 TTL 提交含多个删除, 不能因预算仅够第一条就确认整个提交版本.
    Store expiring(100, 10min, 1ms, Clock::Time{});
    expiring.put("a", {1}, Clock::Time{} + 1ms);
    expiring.put("b", {2}, Clock::Time{} + 1ms);
    expiring.tick(Clock::Time{} + 1ms);
    // batch_bytes 包含同批两个单字节键的完整计费, 少一字节都不能返回部分批次.
    const auto batch_bytes = 2 * (sizeof(Store::Delta) + 2);
    // rejected 初始 false, 只有预期参数或预算异常才置 true.
    bool rejected = false;
    try {
        static_cast<void>(expiring.extract(2, batch_bytes - 1));
    } catch (const std::length_error&) {
        rejected = true;
    }
    CHECK(rejected && expiring.version() == 3);
    // complete 为完整删除批次, 两条记录必须共用同一提交序号.
    const auto complete = expiring.extract(2, batch_bytes);
    CHECK(complete.deltas.size() == 2 && complete.version == 3);
    CHECK(complete.deltas[0].version == 3 && complete.deltas[1].version == 3 && complete.deltas[0].deleted && complete.deltas[1].deleted);
}

// 返回值必须独立拥有 Key 和载荷引用, 即使 Store 和其中全部历史已经被销毁.
void test_result_lifetime() {

    // snapshot 和 delta 被保留到 Store 作用域之外, 暴露任何错误的借用或悬垂引用.
    std::shared_ptr<const Store::Snapshot> snapshot;
    // delta 独立保留提取出的键和值引用, 在源 Store 销毁后仍应可读.
    Store::Extraction delta;
    {
        // store 的空 Key 和空值仍是有效写入, 不能与删除混淆.
        Store store;
        store.put("", {});
        snapshot = store.snapshot();
        delta = store.extract(0);
        store.remove("");
    }
    CHECK(snapshot->version == 1 && snapshot->data.at("").value->empty());
    CHECK(delta.version == 1 && delta.deltas.size() == 1);
    CHECK(!delta.deltas.front().deleted && delta.deltas.front().value->empty());
}

// 用唯一 Key 的提交号编码载荷, 并发快照必须同时读到同一个版本及其内容.
void test_concurrent_snapshots() {

    // store 的每个提交只有一个字节, 值本身就是期望版本.
    Store store;
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
        const auto snapshot = store.snapshot();
        if (snapshot->version == 0) {
            CHECK(snapshot->data.empty());
        } else {
            CHECK(snapshot->data.size() == 1);
            CHECK(snapshot->data.at("counter").value->at(0) == snapshot->version);
        }
    } while (!finished.load());
    writer.join();
    if (writer_error) {
        std::rethrow_exception(writer_error);
    }
    CHECK(store.snapshot()->version == 200);
}

// 验证历史按本地经过时间淘汰, 空闲 tick 也维护墓碑, 业务纪元时间与历史年龄分离.
void test_retention_and_idle_maintenance() {

    // start 为业务时间起点, 一小时拍间隔使年龄维护用例无需大量空拍.
    const auto start = Clock::Time(1h);
    // store 独立持有本场景状态, 构造参数显式指定所需历史预算和时间规则.
    Store store(100, 1h, 1h, start);
    store.put("live", {1});
    store.put("removed", {2});
    store.remove("removed");
    // written 捕获预置写入完成后的本地时间, 人工推进历史年龄而不实际休眠.
    const auto written = Steady::now();
    store.tick(start - 1ns);
    CHECK(!store.extract(0).stale && store.extract(0).deltas.size() == 3);
    store.tick(start + 1h, written + 1h);
    CHECK(store.version() == 3 && store.extract(0).stale && store.extract(2).stale);
    CHECK(!store.extract(3).stale && store.extract(3).deltas.empty());
    CHECK(store.snapshot()->data.size() == 1 && store.snapshot()->data.at("live").value->front() == 1);
    store.put("removed", {3});
    // changes 为已确认版本之后的后缀, 检查清理后的同键重建仍产生正常写入.
    const auto changes = store.extract(3);
    CHECK(!changes.stale && changes.deltas.size() == 1 && !changes.deltas.front().deleted);
    CHECK(store.snapshot()->data.at("removed").value->front() == 3);

    Store no_retention(100, Steady::duration::zero());
    no_retention.put("live", {1});
    CHECK(no_retention.extract(0).stale && !no_retention.extract(1).stale);
    no_retention.remove("live");
    CHECK(no_retention.extract(1).stale && !no_retention.extract(2).stale);
    no_retention.put("live", {2});
    CHECK(no_retention.snapshot()->data.at("live").value->front() == 2);

    // rejected 初始 false, 只有预期参数或预算异常才置 true.
    bool rejected = false;
    try {
        Store invalid(100, -1ns);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

// 使用 weak_ptr 检查缓存失效时的引用释放, 外部持有快照时旧载荷仍应存活.
void test_invalidated_snapshot_release() {

    for (unsigned operation = 0; operation < 3; ++operation) {
        // store 独立持有本场景状态, 构造参数显式指定所需历史预算和时间规则.
        Store store(0, 10min, 1ms, Clock::Time{});
        store.put("payload", Store::Buffer(4096, 7), Clock::Time{} + 1ms);
        // snapshot 为当前持有的强引用, 显式 reset 用于区分缓存与外部读者的所有权.
        auto snapshot = store.snapshot();
        // old_snapshot 只观察旧缓存是否被回收, 不延长快照寿命.
        std::weak_ptr<const Store::Snapshot> old_snapshot = snapshot;
        // old_payload 只观察旧载荷寿命, 覆盖,删除和过期都应释放失效缓存引用.
        std::weak_ptr<const Store::Buffer> old_payload = snapshot->data.at("payload").value;
        snapshot.reset();
        CHECK(!old_snapshot.expired() && !old_payload.expired());
        if (operation == 0) {
            store.put("payload", {8});
        } else if (operation == 1) {
            store.remove("payload");
        } else {
            store.tick(Clock::Time{} + 1ms);
        }
        CHECK(store.version() == 2 && old_snapshot.expired() && old_payload.expired());
    }

    // store 独立持有本场景状态, 构造参数显式指定所需历史预算和时间规则.
    Store store(0);
    store.put("held", {9});
    // snapshot 为当前持有的强引用, 显式 reset 用于区分缓存与外部读者的所有权.
    auto snapshot = store.snapshot();
    // payload 观察外部快照释放后的最终回收, 弱引用自身不拥有字节.
    std::weak_ptr<const Store::Buffer> payload = snapshot->data.at("held").value;
    store.remove("missing");
    CHECK(store.snapshot() == snapshot);
    store.remove("held");
    CHECK(!payload.expired() && snapshot->data.at("held").value->front() == 9);
    snapshot.reset();
    CHECK(payload.expired());
}

// 页边界, 根扩展, 删除剪枝和多代共享都必须保留旧视图, 不将可变时间轮节点搬入快照.
void test_snapshot_index() {

    Index index;
    // empty 捕获空根, 之后扩展和写入不能改变其零项视图.
    const auto empty = index.capture();
    // original 为旧视图共享的不可变单字节载荷, 内容为 1.
    const auto original = std::make_shared<const Store::Buffer>(Store::Buffer{1});
    // changed 为新版本载荷, 内容为 2, 用共享指针身份区分是否误改旧页.
    const auto changed = std::make_shared<const Store::Buffer>(Store::Buffer{2});
    // checkpoints 在各树高边界保留旧根, 让后续写入确实走共享页路径.
    std::vector<Index::View> checkpoints;
    // 64 个叶槽, 16 个分支, 跨越 64 和 1024 边界并在根扩展前保留读者.
    for (std::uint64_t slot = 0; slot < 1100; ++slot) {
        CHECK(index.next() == slot);
        index.prepare(slot);
        index.set(slot, std::make_shared<const std::string>(std::to_string(slot)), {original, {}});
        if (slot == 63 || slot == 1023) {
            checkpoints.push_back(index.capture());
        }
    }

    // before 保留结构变更前的稳定视图, 原键,载荷和有效项数必须保持不变.
    const auto before = index.capture();
    CHECK(empty.size() == 0 && before.size() == 1100);
    for (std::uint64_t slot = 0; slot < 1100; ++slot) {
        index.prepare(slot);
        if ((slot & 1) == 0) {
            index.erase(slot);
        } else {
            index.set(slot, {}, {changed, {}});
        }
    }

    // after 捕获删除偶数槽并替换奇数槽后的视图, 应只含 550 个新值.
    const auto after = index.capture();
    CHECK(after.size() == 550);
    // seen 从零统计遍历得到的有效项, 与 View::size 独立对照.
    std::size_t seen = 0;
    after.each([&](const std::string& key, const Index::Record& record) {
        CHECK(std::stoull(key) % 2 == 1 && record.value == changed);
        ++seen;
    });
    CHECK(seen == 550);
    checkpoints.push_back(before);
    for (const auto& view : checkpoints) {
        // slot 从零核对旧快照的递增槽顺序, 每访问一项递增一次.
        std::size_t slot = 0;
        view.each([&](const std::string& key, const Index::Record& record) { CHECK(key == std::to_string(slot++) && record.value == original); });
        CHECK(slot == view.size());
    }

    // 删除产生的空槽可复用, 清空后再次添加也不能修改仍被读者持有的旧根.
    CHECK(index.next() == 0);
    index.prepare(0);
    index.set(0, std::make_shared<const std::string>("reused"), {original, {}});
    CHECK(index.next() == 2 && index.capture().size() == 551 && after.size() == 550);
    before.each([&](const std::string& key, const Index::Record& record) { CHECK(key != "reused" && record.value == original); });
    index.prepare(0);
    index.erase(0);
    for (std::uint64_t slot = 1; slot < 1100; slot += 2) {
        index.prepare(slot);
        index.erase(slot);
    }
    CHECK(index.capture().size() == 0 && after.size() == 550);
    // slot 在索引清空后重新申请, 应复用最小空槽而不受旧 View 影响.
    const auto slot = index.next();
    index.prepare(slot);
    index.set(slot, std::make_shared<const std::string>("reborn"), {original, {}});
    CHECK(index.capture().size() == 1 && before.size() == 1100);

    // 索引销毁不释放仍被 View 拥有的页面, 读者不借用原对象中的 Key 或 Value.
    Index::View retained;
    {
        Index temporary;
        temporary.prepare(0);
        temporary.set(0, std::make_shared<const std::string>("retained"), {changed, {}});
        retained = temporary.capture();
    }
    CHECK(retained.size() == 1);
    retained.each([&](const std::string& key, const Index::Record& record) { CHECK(key == "retained" && record.value == changed); });
}

// 收缩不能删掉尚未提交的准备路径, 也不能把非零高位的唯一子树当成第 0 个子树提升.
void test_snapshot_index_prepared_paths() {

    Index index;
    // value 为准备路径场景共用的不可变载荷, 不通过业务字节区分槽位.
    const auto value = std::make_shared<const Store::Buffer>(Store::Buffer{1});
    // low_key 为低槽共享键, 用来验证缩根与重新插入不破坏旧视图.
    const auto low_key = std::make_shared<const std::string>("low");
    // high_key 为触发根扩展的高槽键, 删除后检查槽号高位仍保持语义.
    const auto high_key = std::make_shared<const std::string>("high");
    // pending_key 用于先 prepare 后 set 的槽, 其他删除不能丢掉其准备路径.
    const auto pending_key = std::make_shared<const std::string>("pending");
    index.prepare(0);
    index.set(0, low_key, {value, {}});
    index.prepare(64);
    index.set(64, high_key, {value, {}});
    // before 保留结构变更前的稳定视图, 原键,载荷和有效项数必须保持不变.
    const auto before = index.capture();

    // 1024 只有空的准备路径. 删除 64 后仍需保留它, 后续 set 不再 prepare 或分配页面.
    index.prepare(1024);
    index.prepare(64);
    index.erase(64);
    index.set(1024, pending_key, {value, {}});
    CHECK(index.capture().size() == 2 && index.next() == 1);
    before.each([&](const std::string& key, const Index::Record& record) { CHECK((key == "low" || key == "high") && record.value == value); });

    // 剩下的唯一分支在非零位置, 原槽号 1024 必须继续定位到同一条记录.
    index.prepare(0);
    index.erase(0);
    CHECK(index.next() == 0);
    index.prepare(1024);
    index.set(1024, {}, {value, Clock::Time{} + 1s});
    index.capture().each([&](const std::string& key, const Index::Record& record) { CHECK(key == "pending" && record.deadline == Clock::Time{} + 1s); });

    // Store 的批量过期先准备全部路径再删除. 中途收缩不得使剩余的提交需要重新 prepare.
    index.prepare(0);
    index.set(0, low_key, {value, {}});
    index.prepare(0);
    index.prepare(1024);
    index.erase(1024);
    index.erase(0);
    CHECK(index.capture().size() == 0 && index.next() == 0 && before.size() == 2);

    // 分页必须跳过稀疏高位树, 不把数值槽号当有效项排名, 读者暂停不消费当前行.
    index.prepare(1);
    index.set(1, low_key, {value, {}});
    index.prepare(std::uint64_t{1} << 62);
    index.set(std::uint64_t{1} << 62, high_key, {value, {}});
    const auto sparse = index.capture(); // 独立固定深度 15 的旧根.
    CHECK(sparse.page(1, 1, [&](const std::string& key, const Index::Record& record) { CHECK(key == "high" && record.value == value); return true; }) == 1);
    CHECK(sparse.page(0, 1, [&](const std::string& key, const Index::Record&) { CHECK(key == "low"); return false; }) == 0);
    index.prepare(std::uint64_t{1} << 62);
    index.erase(std::uint64_t{1} << 62);
    CHECK(sparse.page(1, 1, [&](const std::string& key, const Index::Record&) { CHECK(key == "high"); return true; }) == 1);
    CHECK(index.capture().page(1, 1, [](const auto&, const auto&) { return false; }) == 0);
}

} // namespace

// 所有断言在 Release 也生效, 异常使 CTest 明确失败.
int main() {

    try {
        test_put_and_remove();
        test_evict_expired();
        test_extract_since_and_history_trim();
        test_batches_and_recreation();
        test_long_history_suffix();
        test_extract_budget();
        test_result_lifetime();
        test_concurrent_snapshots();
        test_retention_and_idle_maintenance();
        test_invalidated_snapshot_release();
        test_snapshot_index();
        test_snapshot_index_prepared_paths();
        std::cout << "All tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        // error 包含首个失败原因, 保留非零退出码供 CTest 判定失败.
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
