#include "almanac.hpp"
#include "check.hpp"

#include <atomic>
#include <iostream>
#include <latch>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
// 安装合法空基线, book 为本例唯一 Scope, version 为权威位置; 失败直接报告断言.
void initialize(astra::Almanac& book, std::uint64_t version = 0) {
    // draft 独占本次空全量, 只有完整 reset 后才允许增量.
    auto draft = book.prepare(version);
    CHECK(book.reset(std::move(draft)) == true);
}

// 验证未就绪与版本零空基线不同, 不把默认构造或收到半份数据当成初始化完成.
void baseline() {

    // book 初始未就绪, 以下读取均不能返回暂时空内存冒充权威结果.
    astra::Almanac book;
    CHECK(book.view() == std::unexpected(astra::Almanac::Error::unready));
    CHECK(book.find("missing") == std::unexpected(astra::Almanac::Error::unready));
    CHECK(book.replay(0) == std::unexpected(astra::Almanac::Error::unready));
    CHECK(book.apply(1, "key", astra::Almanac::Buffer{1}) == std::unexpected(astra::Almanac::Error::unready));

    // partial 尚未安装, 即使已收齐一个有效记录, 外部读取仍然未就绪.
    auto partial = book.prepare(10);
    CHECK(partial.set("key", {1}));
    CHECK(book.view() == std::unexpected(astra::Almanac::Error::unready));
    initialize(book);
    CHECK(book.view()->version() == 0);
    CHECK(book.view()->size() == 0);
    CHECK(book.find("missing")->value == nullptr);
    CHECK(book.replay(0)->changes.empty());

    // zero 只允许空全量; 禁止用版本零绕过内容版本契约.
    auto zero = book.prepare(0);
    CHECK(zero.set("key", {}) == std::unexpected(astra::Almanac::Error::version));
    CHECK(book.reset(std::move(zero)) == false);
    CHECK(book.apply(0, "key", astra::Almanac::Buffer{}) == std::unexpected(astra::Almanac::Error::version));
}

// 验证每个合法 +1 都提交, 重放不再加号, 空值和 Delete 保持不同含义.
void versions() {

    // book/other 是不同 Scope, 即使版本数字相同也不能共享提交状态.
    astra::Almanac book;
    astra::Almanac other;
    initialize(book);
    initialize(other);
    CHECK(book.apply(2, "key", astra::Almanac::Buffer{}) == std::unexpected(astra::Almanac::Error::version));
    CHECK(book.apply(1, "key", astra::Almanac::Buffer{}) == true);
    CHECK(book.find("key")->value && book.find("key")->value->empty());
    CHECK(book.apply(2, "key", astra::Almanac::Buffer{}) == true);
    CHECK(book.apply(3, "absent", std::nullopt) == true);
    CHECK(book.apply(1, "ignored", astra::Almanac::Buffer{7}) == false);
    CHECK(book.find("ignored")->value == nullptr);
    CHECK(book.view()->version() == 3 && book.view()->size() == 1);
    CHECK(other.view()->version() == 0 && other.view()->size() == 0);

    // replay 必须包括两次空 Set 和一次缺失 Delete, 不能仅按最终 Map 推断提交历史.
    const auto replay = book.replay(0);
    CHECK(replay && replay->version == 3 && replay->changes.size() == 3);
    CHECK(replay->changes[0].value && replay->changes[0].value->empty());
    CHECK(replay->changes[1].value && replay->changes[1].value->empty());
    CHECK(!replay->changes[2].value && *replay->changes[2].key == "absent");
    CHECK(book.apply(4, "key", std::nullopt) == true);
    CHECK(book.view()->version() == 4 && book.view()->size() == 0);
    CHECK(book.find("key")->value == nullptr);

    // maximum 是协议允许的最后一个版本, 之后禁止通过整数回绕提交版本零.
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    astra::Almanac ending;
    initialize(ending, maximum - 1);
    CHECK(ending.apply(maximum, "last", astra::Almanac::Buffer{9}) == true);
    CHECK(ending.apply(0, "last", std::nullopt) == std::unexpected(astra::Almanac::Error::version));
    CHECK(ending.view()->version() == maximum);
}

// 验证私有全量准备、重复 Key、原子替换、版本不回退以及旧根稳定性.
void snapshots() {

    astra::Almanac book;
    initialize(book);
    CHECK(book.apply(1, "old", astra::Almanac::Buffer{1}) == true);
    // old 持有版本 1 的冻结根, reset 与后续写入不能篡改这个版本的内容.
    const auto old = book.view();
    auto draft = book.prepare(10);
    CHECK(draft.set("new", {10}));
    CHECK(draft.set("new", {11}) == std::unexpected(astra::Almanac::Error::duplicate));
    CHECK(draft.set("empty", {}));
    CHECK(book.view()->version() == 1 && book.find("new")->value == nullptr);
    CHECK(book.reset(std::move(draft)) == true);
    CHECK(book.view()->version() == 10 && book.view()->size() == 2);
    CHECK(!book.find("old")->value && *book.find("new")->value == astra::Almanac::Buffer{10});
    CHECK(book.replay(1) == std::unexpected(astra::Almanac::Error::history));
    CHECK(book.reset(std::move(draft)) == std::unexpected(astra::Almanac::Error::input));

    // visited 计数固定旧根中的记录, reader 只借用 key/value, 不保存悬空引用.
    std::size_t visited{};
    old->each([&visited](const std::string& key, const astra::Almanac::Value& value) {
        CHECK(key == "old" && *value == astra::Almanac::Buffer{1});
        ++visited;
    });
    CHECK(visited == 1 && old->version() == 1);

    // lower 不可覆盖已有版本; equal 是权威重放, 不替换当前根或重新制造提交.
    auto lower = book.prepare(9);
    auto equal = book.prepare(10);
    CHECK(book.reset(std::move(lower)) == std::unexpected(astra::Almanac::Error::version));
    CHECK(book.reset(std::move(equal)) == false);
    CHECK(book.view()->size() == 2);
    CHECK(book.apply(11, "new", astra::Almanac::Buffer{11}) == true);
    CHECK(book.replay(10)->changes.size() == 1);
    auto empty = book.prepare(12);
    CHECK(book.reset(std::move(empty)) == true);
    CHECK(book.view()->version() == 12 && book.view()->size() == 0);
}

// 验证连续历史的条数/字节上限和返回结果的独立寿命, 不用超前游标跳过恢复.
void history() {

    // limits 只保留最近两次提交, 不限制本例一个活动 Key 的内容.
    astra::Almanac::Limits limits;
    limits.history = 2;
    astra::Almanac book(limits);
    initialize(book);
    CHECK(book.apply(1, "key", astra::Almanac::Buffer{1}) == true);
    CHECK(book.apply(2, "key", astra::Almanac::Buffer{2}) == true);
    const auto saved = book.replay(0);
    CHECK(book.apply(3, "key", astra::Almanac::Buffer{3}) == true);
    CHECK(book.replay(0) == std::unexpected(astra::Almanac::Error::history));
    CHECK(book.replay(4) == std::unexpected(astra::Almanac::Error::version));
    CHECK(book.replay(1)->changes.size() == 2);
    CHECK(book.replay(1)->changes.front().version == 2);
    CHECK(book.replay(2)->changes.front().version == 3);
    CHECK(book.replay(3)->changes.empty());
    CHECK(saved->version == 2 && *saved->changes.front().value == astra::Almanac::Buffer{1});

    // budget 只能容纳一条记录的逻辑成本, 证明字节上限独立于条数上限.
    astra::Almanac::Limits budget;
    budget.backlog = sizeof(astra::Almanac::Change) + 4;
    astra::Almanac bounded(budget);
    initialize(bounded);
    CHECK(bounded.apply(1, "key", astra::Almanac::Buffer{1}) == true);
    CHECK(bounded.apply(2, "key", astra::Almanac::Buffer{2}) == true);
    CHECK(bounded.replay(0) == std::unexpected(astra::Almanac::Error::history));
    CHECK(bounded.replay(1)->changes.size() == 1);

    // retained 独立保留淘汰前的回放. 新记录超过历史预算但未超过活动容量, 应提交并淘汰旧项及自身.
    const auto retained = bounded.replay(1);
    CHECK(bounded.apply(3, "key", astra::Almanac::Buffer(512 * 1024, 7)) == true);
    CHECK(bounded.find("key")->version == 3 && bounded.find("key")->value->size() == 512 * 1024);
    CHECK(bounded.replay(2) == std::unexpected(astra::Almanac::Error::history));
    CHECK(bounded.replay(3)->changes.empty());
    CHECK(retained->version == 2 && *retained->changes.front().value == astra::Almanac::Buffer{2});

    // 较小的新提交恢复连续历史, 但不能越过已淘汰的大记录, 向旧游标返回一份伪完整回放.
    CHECK(bounded.apply(4, "key", astra::Almanac::Buffer{4}) == true);
    CHECK(bounded.replay(2) == std::unexpected(astra::Almanac::Error::history));
    CHECK(bounded.replay(3)->version == 4 && bounded.replay(3)->changes.size() == 1);
    CHECK(bounded.replay(3)->changes.front().version == 4);
    CHECK(bounded.apply(5, "key", std::nullopt) == true);
    CHECK(bounded.view()->size() == 0);
    CHECK(bounded.replay(3) == std::unexpected(astra::Almanac::Error::history));
    CHECK(bounded.replay(4)->changes.size() == 1 && !bounded.replay(4)->changes.front().value);

    // oversized 初始历史为空, 第一条合法写入就超过历史预算; 不能以空 deque 作为例外路径.
    astra::Almanac oversized(budget);
    initialize(oversized);
    CHECK(oversized.apply(1, "key", astra::Almanac::Buffer(512 * 1024, 8)) == true);
    CHECK(oversized.find("key")->value->size() == 512 * 1024);
    CHECK(oversized.replay(0) == std::unexpected(astra::Almanac::Error::history));
    CHECK(oversized.replay(1)->changes.empty());

    // zero 明确关闭历史, 新提交仍可成功, 旧游标必须走全量而不是取得残缺增量.
    budget.history = 0;
    astra::Almanac zero(budget);
    initialize(zero);
    CHECK(zero.apply(1, "key", astra::Almanac::Buffer{}) == true);
    CHECK(zero.replay(0) == std::unexpected(astra::Almanac::Error::history));
    CHECK(zero.replay(1)->changes.empty());

    // bytes 只关闭历史字节预算, 条数仍允许一条; 零正文和缺失 Delete 也有元数据成本, 都不保留历史.
    budget.history = 1;
    budget.backlog = 0;
    astra::Almanac bytes(budget);
    initialize(bytes);
    CHECK(bytes.apply(1, "key", astra::Almanac::Buffer{}) == true);
    CHECK(bytes.find("key")->value && bytes.find("key")->value->empty());
    CHECK(bytes.replay(0) == std::unexpected(astra::Almanac::Error::history));
    CHECK(bytes.replay(1)->changes.empty());
    CHECK(bytes.apply(2, "missing", std::nullopt) == true);
    CHECK(bytes.view()->version() == 2 && bytes.view()->size() == 1);
    CHECK(bytes.replay(1) == std::unexpected(astra::Almanac::Error::history));
    CHECK(bytes.replay(2)->changes.empty());
}

// 验证容量失败不提交半个版本, 删除释放活动计费, 恢复候选不能绕过目标容量.
void capacity() {

    // limits 仅容纳一条长度为 1 的 Key 与两个载荷字节, 便于精确检验计费边界.
    astra::Almanac::Limits limits;
    limits.records = 1;
    limits.bytes = 3;
    astra::Almanac book(limits);
    initialize(book);
    CHECK(book.apply(1, "a", astra::Almanac::Buffer{1, 2}) == true);
    CHECK(book.apply(2, "b", astra::Almanac::Buffer{}) == std::unexpected(astra::Almanac::Error::capacity));
    CHECK(book.apply(2, "a", astra::Almanac::Buffer{1, 2, 3}) == std::unexpected(astra::Almanac::Error::capacity));
    CHECK(book.view()->version() == 1 && book.find("a")->value->size() == 2);
    CHECK(book.apply(2, "a", astra::Almanac::Buffer{}) == true);
    CHECK(book.apply(3, "a", std::nullopt) == true);
    CHECK(book.apply(4, "b", astra::Almanac::Buffer{3, 4}) == true);

    // large 来源对象的容量更大, reset 仍必须按 book 自己的容量判断.
    astra::Almanac large;
    auto draft = large.prepare(5);
    CHECK(draft.set("long", {}));
    CHECK(book.reset(std::move(draft)) == std::unexpected(astra::Almanac::Error::capacity));
    CHECK(book.view()->version() == 4);
    auto bounded = book.prepare(5);
    CHECK(bounded.set("a", {1, 2}));
    CHECK(bounded.set("b", {}) == std::unexpected(astra::Almanac::Error::capacity));
    CHECK(book.reset(std::move(bounded)) == true);

    // 无效单记录请求只返回输入错误, 不推进到下一个合法版本.
    CHECK(book.apply(6, "", std::nullopt) == std::unexpected(astra::Almanac::Error::input));
    CHECK(book.apply(6, std::string("a\0b", 3), std::nullopt) == std::unexpected(astra::Almanac::Error::input));
    CHECK(book.apply(6, std::string(1025, 'x'), std::nullopt) == std::unexpected(astra::Almanac::Error::input));
    CHECK(book.apply(6, "a", astra::Almanac::Buffer(1024 * 1024 + 1)) == std::unexpected(astra::Almanac::Error::input));
    CHECK(book.view()->version() == 5);
}

// 验证页面读完成同步不借用已销毁的状态对象, 用户回调异常也不会遗留业务锁.
void lifetime() {

    // saved 独立持有根和读完成互斥量, 外层 book 退出后仍能遍历旧内容.
    std::optional<astra::Almanac::View> saved;
    {
        astra::Almanac book;
        auto draft = book.prepare(99);
        CHECK(draft.set("alive", {9}));
        CHECK(book.reset(std::move(draft)) == true);
        saved = *book.view();
        // interrupted 只由本次 reader 的受控异常置位, 不把断言失败误认为预期异常.
        bool interrupted{};
        try {
            saved->each([](const std::string&, const astra::Almanac::Value&) { throw std::runtime_error("reader stopped"); });
        } catch (const std::runtime_error&) {
            // 回调失败只结束本次遍历, 随后覆盖仍须取得提交保护并成功.
            interrupted = true;
        }
        CHECK(interrupted);
        CHECK(book.apply(100, "alive", astra::Almanac::Buffer{10}) == true);
        // 读回调可以重入点查或写入, 证明 each 没有覆盖用户代码持有提交锁.
        saved->each([&book](const std::string&, const astra::Almanac::Value&) {
            CHECK(book.find("alive")->version == 100);
            CHECK(book.apply(101, "alive", astra::Almanac::Buffer{11}) == true);
        });
    }
    saved->each([](const std::string& key, const astra::Almanac::Value& value) { CHECK(key == "alive" && *value == astra::Almanac::Buffer{9}); });
    CHECK(saved->version() == 99 && saved->size() == 1);
}

// 重复捕获与页面复用并发, 检验版本/内容始终配对; 后续 TSan 授权执行时覆盖读完成边界.
void concurrency() {

    astra::Almanac book;
    initialize(book);
    CHECK(book.apply(1, "key", astra::Almanac::Buffer{1}) == true);
    // entered 保证读线程已启动, valid 汇总读者观察; 不在线程中抛断言越过线程入口.
    // reader 借用仍存活的 book/entered/valid, stop 接收 jthread 的退出请求, 每轮 view 是独立冻结根.
    std::latch entered{1};
    std::atomic<bool> valid{true};
    std::jthread reader([&](std::stop_token stop) {
        entered.count_down();
        while (!stop.stop_requested()) {
            const auto view = book.view();
            if (!view || view->size() != 1) {
                valid = false;
                return;
            }
            view->each([&](const std::string& key, const astra::Almanac::Value& value) {
                if (key != "key" || value->size() != 1 || value->front() != static_cast<std::uint8_t>(view->version() % 256)) {
                    valid = false;
                }
            });
        }
    });
    entered.wait();
    // version 从 2 推进到 2000, 值编码其低字节, 使任意撕裂的根/版本都能被读线程观察.
    for (std::uint64_t version = 2; version <= 2000; ++version) {
        CHECK(book.apply(version, "key", astra::Almanac::Buffer{static_cast<std::uint8_t>(version % 256)}) == true);
    }
    reader.request_stop();
    reader.join();
    CHECK(valid.load());
}

// 跨叶页构建、清空并复用槽位, 旧根仍保留原字节; 点查与分页索引不能各自残留不同记录.
void pages() {

    // book 一开始只有一个版本 10 的 150 项完整快照, 固定旧根用来覆盖删除期间的路径复制.
    astra::Almanac book;
    auto draft = book.prepare(10);
    for (unsigned index = 0; index < 150; ++index) {
        CHECK(draft.set(std::to_string(index), {1}));
    }
    CHECK(book.reset(std::move(draft)) == true);
    const auto old = book.view();

    // version 只由此测试顺序推进, key 的文本与整数 index 一一对应.
    std::uint64_t version = 10;
    for (unsigned index = 0; index < 150; ++index) {
        CHECK(book.apply(++version, std::to_string(index), std::nullopt) == true);
    }
    CHECK(book.view()->size() == 0);
    for (unsigned index = 0; index < 150; ++index) {
        CHECK(book.apply(++version, std::to_string(index), astra::Almanac::Buffer{2}) == true);
    }

    // visited 汇总旧页记录数, current 汇总新页记录数; 两者都必须保持完整且拥有不同正文.
    std::size_t visited{};
    std::size_t current{};
    old->each([&visited](const std::string&, const astra::Almanac::Value& value) {
        CHECK(*value == astra::Almanac::Buffer{1});
        ++visited;
    });
    book.view()->each([&book, &current](const std::string& key, const astra::Almanac::Value& value) {
        CHECK(*value == astra::Almanac::Buffer{2});
        CHECK(book.find(key)->value == value);
        ++current;
    });
    CHECK(visited == 150 && current == 150 && book.view()->version() == 310);

    // 分页只以捕获根的有效项排名继续, 叶内空槽/新根重用不改变旧位置, 每页最多七项.
    std::size_t offset{};
    while (offset < old->size()) {
        const auto consumed = old->page(offset, 7, [&](const std::string& key, const astra::Almanac::Value& value) {
            CHECK(key == std::to_string(offset++) && *value == astra::Almanac::Buffer{1});
            return true;
        });
        CHECK(consumed > 0 && consumed <= 7);
    }
    CHECK(offset == 150 && old->bytes() == 490);
    CHECK(old->page(150, 7, [](const auto&, const auto&) { throw std::runtime_error("End page visited an item"); return true; }) == 0);
    CHECK(old->page(1, 7, [](const auto&, const auto&) { return false; }) == 0);
    CHECK(old->page(1, 0, [](const auto&, const auto&) { throw std::runtime_error("Zero page visited an item"); return true; }) == 0);
    bool rejected{}; // 超范围不是已经完成的合法空页.
    try {
        static_cast<void>(old->page(151, 1, [](const auto&, const auto&) { return true; }));
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    CHECK(rejected);
}
} // namespace

// 独立单元入口, 不连接服务或下载依赖; 仅在取得本轮测试授权后运行.
int main() {
    try {
        baseline();
        versions();
        snapshots();
        history();
        capacity();
        lifetime();
        concurrency();
        pages();
        std::cout << "Almanac state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
