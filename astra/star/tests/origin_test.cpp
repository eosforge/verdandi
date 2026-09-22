#include "catalog.hpp"
#include "check.hpp"
#include "ephemeris.hpp"
#include "origin.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
using astra::Catalog;
using astra::Clock;
using astra::Ephemeris;
using astra::Scope;
// 测试来源不启动网络, 原生规则和索引/历史使用生产模板, 只在外部明确控制提交保护和时刻.
using Source = astra::Origin<Catalog::Record>;

// 类型名探测应在约束边界失败, 不实例化一个内部 static_assert 失败的完整容器.
template <typename Item>
concept Accepted = requires { typename astra::Origin<Item>; };

static_assert(Accepted<Catalog::Record> && Accepted<Ephemeris::Record> && !Accepted<std::string>); // 原生共享记录合法, 复制可能分配的 string 被拒绝.

// Catalog 计量共享正文的保守字节, 水位计费由 Origin 的元数据部分承担.
std::size_t measure(const Catalog::Record& record) noexcept {
    return record.value ? record.value->size() : 0;
}

// Ephemeris 计量两个完整正文, 不拼接 Attr/Data 或构造临时序列化结果.
std::size_t measure(const Ephemeris::Record& record) noexcept {
    return (record.attr ? record.attr->size() : 0) + (record.data ? record.data->size() : 0);
}

// 返回一条水位, 不包含载荷或时钟, 便于区分来源位置与 Key 业务版本.
Catalog::Record record(std::uint64_t version) {
    return Catalog::Record{version, nullptr, std::nullopt};
}

// 显式模拟外层原子提交, 旧资源声明在锁之前, 离开锁后才释放.
void commit(Source& source, const std::shared_ptr<std::mutex>& gate, Scope scope, std::string key, std::optional<Catalog::Record> value, std::optional<std::uint64_t> position, std::chrono::steady_clock::time_point now) {

    Source::Retired retired; // commit 移出的旧正文/事件, 不在 gate 内最终析构.
    const std::lock_guard lock(*gate);
    auto edit = source.prepare(scope, std::move(key), std::move(value), position, now);
    CHECK(edit);
    retired = edit->commit();
}

// 不同 Sector/Spectrum 共用唯一来源位置, 回收空范围也不会重用旧序列.
void scopes() {

    const auto gate = std::make_shared<std::mutex>();
    Source source(gate, measure, true);
    const auto now = std::chrono::steady_clock::now();
    commit(source, gate, {"one", "main"}, "key", record(20), 1, now);
    commit(source, gate, {"two", "other"}, "key", record(7), 2, now);
    const auto frozen = [&] { const std::lock_guard lock(*gate); return source.capture(); }();
    CHECK(frozen.size() == 2 && frozen.position() == 2);
    unsigned count{};
    frozen.each([&](const Scope& scope, const std::string& key, const Catalog::Record& value) {
        CHECK(key == "key" && ((scope.sector == "one" && value.version == 20) || (scope.sector == "two" && value.version == 7)));
        ++count;
    });
    CHECK(count == 2);
    {
        const std::lock_guard lock(*gate);
        auto history = source.replay(0);
        CHECK(history && history->size() == 2 && (*history)[0].position == 1 && (*history)[1].position == 2);
        CHECK((*history)[0].name->scope->sector == "one" && (*history)[1].name->scope->sector == "two");
        CHECK(source.next() == 3 && source.find({"one", "main"}, "key")->version == 20);
        CHECK(!source.find({"unknown", "main"}, "key"));
    }

    commit(source, gate, {"one", "main"}, "key", std::nullopt, 3, now);
    commit(source, gate, {"two", "other"}, "key", std::nullopt, 4, now);
    commit(source, gate, {"one", "main"}, "new", record(100), 5, now);
    CHECK(frozen.size() == 2); // 旧根仍拥有原范围和 Key, 不借用已回收目录.
    {
        const std::lock_guard lock(*gate);
        CHECK(source.position() == 5 && source.capture().size() == 1);
        const auto history = source.replay(2);
        CHECK(history && history->size() == 3 && !(*history)[0].record && !(*history)[1].record);
    }
}

// 外层第二项准备失败时丢弃 Edit, 原生根/目录/位置/历史全部维持原状态.
void rollback() {

    const auto gate = std::make_shared<std::mutex>();
    Source source(gate, measure, true, {.records = 1, .scopes = 1, .bytes = 4096});
    const auto now = std::chrono::steady_clock::now();
    {
        const std::lock_guard lock(*gate);
        {
            auto prepared = source.prepare({"first", "scope"}, "key", record(8), 1, now);
            CHECK(prepared);
            bool blocked{};
            try {
                static_cast<void>(source.capture());
            } catch (const std::logic_error&) {
                blocked = true;
            }
            CHECK(blocked); // 准备事务不允许被误读成完整状态.
        }
        CHECK(source.position() == 0 && source.capture().size() == 0 && source.replay(0)->empty());
    }
    commit(source, gate, {"second", "scope"}, "key", record(12), 1, now); // 空目录及容量已归还.
    {
        const std::lock_guard lock(*gate);
        {
            auto prepared = source.prepare({"second", "scope"}, "key", record(99), 2, now);
            CHECK(prepared); // 模拟本地投影随后准备失败, 不调用 commit.
        }
        CHECK(source.position() == 1 && source.find({"second", "scope"}, "key")->version == 12);
        CHECK(source.replay(0)->size() == 1);
        const auto full = source.prepare({"third", "scope"}, "key", record(1), 2, now);
        CHECK(!full && full.error() == Source::Error::capacity);
        const auto skipped = source.prepare({"second", "scope"}, "key", record(99), 3, now);
        CHECK(!skipped && skipped.error() == Source::Error::version);
        CHECK(source.position() == 1);
    }
}

// 超大历史项、禁用历史和实际淘汰必须明确要求全量, 不空队列 pop 或返回断裂后缀.
void history() {

    const auto gate = std::make_shared<std::mutex>();
    const auto now = std::chrono::steady_clock::now();
    Source source(gate, measure, true, {.history = 2, .backlog = 4096});
    for (std::uint64_t position = 1; position <= 3; ++position) {
        commit(source, gate, {"one", "scope"}, "key", record(position), position, now);
    }
    {
        const std::lock_guard lock(*gate);
        CHECK(source.replay(0).error() == Source::Error::history);
        const auto result = source.replay(1);
        CHECK(result && result->size() == 2 && result->front().position == 2 && result->back().position == 3);
        CHECK(source.replay(4).error() == Source::Error::version);
    }
    Catalog::Record large{4, std::make_shared<const Catalog::Buffer>(8192), Clock::Time(5s)};
    commit(source, gate, {"one", "scope"}, "key", large, 4, now);
    {
        const std::lock_guard lock(*gate);
        CHECK(source.position() == 4 && source.find({"one", "scope"}, "key")->value->size() == 8192);
        CHECK(source.replay(3).error() == Source::Error::history);
    }
    commit(source, gate, {"one", "scope"}, "key", record(4), std::nullopt, now); // 本地 TTL 只剩水位, 不占新的来源位置.
    commit(source, gate, {"one", "scope"}, "key", record(5), 5, now);
    {
        const std::lock_guard lock(*gate);
        CHECK(source.replay(4)->size() == 1 && source.position() == 5);
    }

    for (const auto limits : {Source::Limits{.history = 0}, Source::Limits{.backlog = 0}, Source::Limits{.retention = 0s}}) {
        Source disabled(gate, measure, true, limits);
        commit(disabled, gate, {"one", "scope"}, "key", record(1), 1, now);
        const std::lock_guard lock(*gate);
        CHECK(disabled.position() == 1 && disabled.capture().size() == 1);
        CHECK(disabled.replay(0).error() == Source::Error::history);
        const auto delivery = disabled.deliver(0, 8, 4096); // 零预算连新项也已淘汰, 有界发送必须返回完整基线.
        CHECK(delivery && delivery->baseline && delivery->events.empty() && delivery->baseline->position() == 1);
    }
    Source replica(gate, measure, false);
    commit(replica, gate, {"one", "scope"}, "key", record(1), 1, now);
    {
        const std::lock_guard lock(*gate);
        CHECK(replica.position() == 1 && replica.replay(0).error() == Source::Error::history);
    }
}

// 历史年龄只在写入时裁剪, 完整回放与有界复制读取必须一致; 删除不能因空闲被迫改走全量.
void retention() {

    const auto gate = std::make_shared<std::mutex>();  // 生产要求来源读写使用同一外层锁.
    Source source(gate, measure, true);                // 默认 10 分钟保留时间, 无其他容量压力.
    const auto now = std::chrono::steady_clock::now(); // 新写入触发维护的固定时刻.
    const auto stored = now - 1h;                      // 直接构造已经超龄的历史, 不休眠或修改系统时间.
    const Scope scope{"idle", "history"};              // 创建和删除位于同一来源范围.
    commit(source, gate, scope, "key", record(1), 1, stored);
    commit(source, gate, scope, "key", std::nullopt, 2, stored);
    {
        const std::lock_guard lock(*gate);
        const auto replay = source.replay(0); // 完整历史仍包含已删除条目的创建及删除.
        CHECK(replay && replay->size() == 2 && replay->front().record && !replay->back().record);
        CHECK(replay->front().stored == stored && replay->back().form == Source::Form::erase);
        const auto first = source.deliver(0, 1, 4096); // 超龄不触发快照, 条数上限仍只交付首项.
        CHECK(first && !first->baseline && first->events.size() == 1 && first->events.front().position == 1);
        const auto cost = first->events.front().bytes;   // 只够首项的字节预算, 不依赖结构体固定大小.
        const auto limited = source.deliver(0, 8, cost); // 同时验证字节上限没有被年龄策略放宽.
        CHECK(limited && !limited->baseline && limited->events.size() == 1);
        CHECK(source.deliver(0, 1, cost - 1).error() == Source::Error::capacity);
        const auto tail = source.deliver(1, 8, 4096); // 下一次发送不能跳过历史里的删除.
        CHECK(tail && !tail->baseline && tail->events.size() == 1 && tail->events.front().position == 2 && !tail->events.front().record);
        const auto caught = source.deliver(2, 8, 4096); // 已追平不退回快照, 也不创建无意义事件.
        CHECK(caught && !caught->baseline && caught->events.empty() && source.replay(2)->empty());
        CHECK(source.deliver(3, 8, 4096).error() == Source::Error::version);
        CHECK(source.deliver(0, 0, 4096).error() == Source::Error::input && source.deliver(0, 1, 0).error() == Source::Error::input);
        CHECK(source.capture().size() == 0); // 读取旧创建不会使已删除记录复活.
    }

    commit(source, gate, scope, "key", record(3), 3, now); // 真正的新提交淘汰旧前缀.
    {
        const std::lock_guard lock(*gate);
        CHECK(source.replay(0).error() == Source::Error::history && source.replay(1).error() == Source::Error::history);
        const auto missing = source.deliver(1, 8, 4096); // 丢失删除所在位置时, 明确要求完整基线.
        CHECK(missing && missing->baseline && missing->events.empty() && missing->baseline->position() == 3);
        const auto replay = source.replay(2);         // 刚好持有裁剪边界仍允许连续恢复.
        const auto tail = source.deliver(2, 8, 4096); // 两个读取入口承接同一位置.
        CHECK(replay && replay->size() == 1 && replay->front().position == 3);
        CHECK(tail && !tail->baseline && tail->events.size() == 1 && tail->events.front().position == 3);
    }
    commit(source, gate, scope, "key", std::nullopt, 4, now + 10min); // 恰好超龄时物理裁剪仍生效.
    {
        const std::lock_guard lock(*gate);
        CHECK(source.replay(2).error() == Source::Error::history);
        const auto replay = source.replay(3); // 新删除事件保留原顺序, 不返回空成功.
        CHECK(replay && replay->size() == 1 && replay->front().position == 4 && !replay->front().record);
    }
}

// 全量是整个来源/域的一次替换, 准备中断/重复项不影响旧根, 不能逐 Scope 发布半份新基线.
void snapshot() {

    const auto gate = std::make_shared<std::mutex>();
    Source replica(gate, measure, false);
    const auto now = std::chrono::steady_clock::now();
    commit(replica, gate, {"old", "scope"}, "key", record(1), 1, now);
    const auto old = [&] { const std::lock_guard lock(*gate); return replica.capture(); }();
    auto draft = replica.prepare(20); // 在目标 gate 外逐范围填充, 不取得部分目标可见状态.
    CHECK(draft.set({"new", "first"}, "one", record(9)));
    CHECK(draft.set({"new", "second"}, "two", record(7)));
    const auto duplicate = draft.set({"new", "first"}, "one", record(99));
    CHECK(!duplicate && duplicate.error() == Source::Error::duplicate);
    {
        std::optional<Source::Replaced> retired;
        const std::lock_guard lock(*gate);
        CHECK(replica.position() == 1 && replica.capture().size() == 1);
        auto result = replica.reset(std::move(draft));
        CHECK(result);
        retired.emplace(std::move(*result));
        CHECK(replica.position() == 20 && replica.capture().size() == 2);
        CHECK(!replica.find({"old", "scope"}, "key") && replica.find({"new", "first"}, "one")->version == 9);
    }
    old.each([](const Scope& scope, const auto& key, const auto& value) { CHECK(scope.sector == "old" && key == "key" && value.version == 1); });
    commit(replica, gate, {"new", "second"}, "two", record(8), 21, now);
    auto stale = replica.prepare(19);
    auto empty = replica.prepare(22);
    {
        std::optional<Source::Replaced> retired;
        const std::lock_guard lock(*gate);
        CHECK(replica.reset(std::move(stale)).error() == Source::Error::version);
        CHECK(replica.position() == 21);
        auto result = replica.reset(std::move(empty));
        CHECK(result);
        retired.emplace(std::move(*result));
        CHECK(replica.position() == 22 && replica.capture().size() == 0 && replica.next() == 23);
    }
    auto exhausted = replica.prepare(UINT64_MAX); // 来源最大位置不能通过新范围或全量空表偷偷归零.
    {
        std::optional<Source::Replaced> retired;
        const std::lock_guard lock(*gate);
        auto result = replica.reset(std::move(exhausted));
        CHECK(result);
        retired.emplace(std::move(*result));
        CHECK(replica.next().error() == Source::Error::version);
        const auto rejected = replica.prepare({"new", "scope"}, "key", record(1), 0, now);
        CHECK(!rejected && rejected.error() == Source::Error::version);
    }
}

// Ephemeris 使用相同来源位置机制但原生载荷结构不同, 不通过同一 KV schema 串接两个 Buffer.
void native() {

    const auto gate = std::make_shared<std::mutex>();
    astra::Origin<Ephemeris::Record> source(gate, measure, true);
    const auto now = std::chrono::steady_clock::now();
    const Ephemeris::Record initial{std::make_shared<const Ephemeris::Buffer>(3), std::make_shared<const Ephemeris::Buffer>(7), Clock::Time(10s), 0, 0, 1000};
    std::optional<astra::Origin<Ephemeris::Record>::View> frozen;
    {
        astra::Origin<Ephemeris::Record>::Retired retired;
        const std::lock_guard lock(*gate);
        auto edit = source.prepare({"services", "main"}, "uuid", initial, 1, now);
        CHECK(edit);
        retired = edit->commit();
        frozen = source.capture();
    }
    {
        astra::Origin<Ephemeris::Record>::Retired retired;
        const std::lock_guard lock(*gate);
        auto next = initial; // 仅续租 metadata 变化, 原正文仍共享, 旧来源快照固定原截止.
        next.deadline = Clock::Time(20s);
        next.renewal = 1;
        auto edit = source.prepare({"services", "main"}, "uuid", next, 2, now, astra::Origin<Ephemeris::Record>::Form::renew);
        CHECK(edit);
        retired = edit->commit();
        CHECK(source.replay(1)->front().form == astra::Origin<Ephemeris::Record>::Form::renew);
    }
    frozen->each([&](const auto&, const auto&, const auto& value) {
        CHECK(value.attr == initial.attr && value.data == initial.data && value.deadline == initial.deadline && value.renewal == 0);
    });
}

// 合并水位的内部 COW 批次不编号, 准备失败/放弃不能泄漏新目录或覆盖老版本.
void batch() {

    const auto gate = std::make_shared<std::mutex>();
    Source source(gate, measure, false, {.records = 3});
    const auto now = std::chrono::steady_clock::now();
    const astra::Scope scope{"one", "scope"};
    commit(source, gate, scope, "one", record(1), 1, now);
    const auto frozen = [&] { const std::lock_guard lock(*gate); return source.capture(); }();
    {
        const std::lock_guard lock(*gate);
        auto candidate = source.prepare();
        CHECK(candidate.set(scope, "one", record(9)));
        CHECK(candidate.set({"new", "scope"}, "two", record(2)));
        CHECK(candidate.find(scope, "one")->version == 9);
        CHECK(candidate.set(scope, "one", record(10)).error() == Source::Error::duplicate);
    }
    {
        std::optional<Source::Tree> retired; // 老页树在 gate 之后释放.
        const std::lock_guard lock(*gate);
        CHECK(source.position() == 1 && source.capture().size() == 1);
        CHECK(source.find(scope, "one")->version == 1 && !source.find({"new", "scope"}, "two"));
        auto candidate = source.prepare();
        CHECK(candidate.set(scope, "one", record(9)));
        CHECK(candidate.set({"new", "scope"}, "two", record(2)));
        retired.emplace(candidate.commit());
        CHECK(source.position() == 1 && source.capture().size() == 2);
        CHECK(source.find(scope, "one")->version == 9);
    }
    frozen.each([](const auto&, const auto& key, const auto& value) { CHECK(key == "one" && value.version == 1); });
}

// 旧 View 的读完成锁独立存活, 页面复制与回调异常退出仍与写者同步, 不读取已析构的来源对象.
void lifetime() {

    const auto gate = std::make_shared<std::mutex>();
    std::optional<Source::View> frozen;
    {
        Source source(gate, measure, true);
        const auto now = std::chrono::steady_clock::now();
        commit(source, gate, {"one", "scope"}, "key", record(1), 1, now);
        frozen = [&] { const std::lock_guard lock(*gate); return source.capture(); }();
        frozen->each([&](const auto&, const auto&, const auto&) {
            commit(source, gate, {"one", "scope"}, "key", record(2), 2, now); // 回调不持提交锁, 合法重入不死锁.
        });
    }
    frozen->each([](const auto&, const auto& key, const auto& value) { CHECK(key == "key" && value.version == 1); });
    bool thrown{};
    try {
        frozen->each([](const auto&, const auto&, const auto&) { throw std::runtime_error("reader"); });
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    CHECK(thrown);
    const std::lock_guard lock(*gate); // 异常路径没有遗留持锁的 Fence.
}
} // namespace

// 本组为源码级确定性用例, 不启动任何外部服务, 不代替多 Star 网络恢复验收.
int main() {

    try {
        scopes();
        rollback();
        history();
        retention();
        snapshot();
        native();
        lifetime();
        batch();
        std::cout << "source commits: ok\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
