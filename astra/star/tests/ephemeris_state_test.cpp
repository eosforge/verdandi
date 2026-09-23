#include "check.hpp"
#include "ephemeris_state.hpp"
#include "exports.hpp"
#include <array>
#include <iostream>
#include <new>
#include <string>

namespace {
using namespace std::chrono_literals;
using astra::Clock;
using astra::Ephemeris;
using astra::Scope;
using State = Ephemeris::State;

// 确定性计时, 只用于真实 State 的公共构造注入, 不读取物理墙钟或休眠.
struct Time {
    std::chrono::nanoseconds value = 1s; // 当前非负业务时间, 默认 1 s.
    std::chrono::nanoseconds jump{};     // 下一次采样后增加的偏移, 默认零; 用于准备中到期.
    bool ready = true;                   // false 模拟本地计时尚未建立, 与参考源失联不同.

    // 先返回本次值再应用一次 jump, 可以确定性地使两阶段准备之间越过 TTL.
    std::optional<Clock::Reading> read() {
        const auto reading = Clock::Reading{.time = Clock::Time(value), .ready = ready, .synchronized = false};
        value += std::exchange(jump, 0ns);
        return reading;
    }
};

// 构造不可变测试载荷, 零字节仍保持非空 Value.
Ephemeris::Value bytes(std::string_view text) {
    return std::make_shared<const Ephemeris::Buffer>(text.begin(), text.end());
}

// 保留固定小通知表, noexcept 接收器不在业务锁内分配或触发用户回调.
struct Notice {
    std::array<std::uint64_t, 32> versions{}; // 按真实通知到达顺序保存的各 Scope 游标.
    std::size_t size{};                       // 已收到的条数, 初始零.
    bool overflow{};                          // 用例超过固定容量时显式失败, 不越界.

    static void receive(void* context, const Scope&, const State::Projection::Event& event) noexcept {
        auto& self = *static_cast<Notice*>(context);
        if (self.size == self.versions.size()) {
            self.overflow = true;
            return;
        }
        self.versions[self.size++] = event.version;
    }
};

// 真实组合层验证: 来源跨 Scope 连续, 可见游标按范围独立, 续租不广播 Attr/Data 内容变化.
void lifecycle() {

    Time time;
    State state([&] { return time.read(); }, {});
    Notice notice;
    state.notify(Notice::receive, &notice);
    const Scope left{"service", "left"};
    const Scope right{"service", "right"};
    const auto attr = bytes("fixed");
    const auto initial = bytes("one");
    const auto first = state.create(left, attr, initial, 1000);
    const auto second = state.create(right, attr, bytes(""), 1000);
    CHECK(first && second && first->uuid != second->uuid && Ephemeris::valid(first->uuid));
    CHECK(state.source().position() == 2 && state.source().size() == 2);
    const auto frozen = state.capture(left);
    const auto foreign = state.capture(right);
    CHECK(frozen && foreign && frozen->version() == 1 && foreign->version() == 1);
    CHECK(notice.size == 2);

    time.value = 1200ms;
    CHECK(state.renew(left, first->uuid, 1));
    CHECK(state.source().position() == 3 && state.capture(left)->version() == 1 && notice.size == 2);
    CHECK(state.renew(left, first->uuid, 1));
    CHECK(state.source().position() == 3); // 相同 order 不再刷新截止或制造来源位置.
    CHECK(state.update(left, first->uuid, bytes("one"), 1));
    CHECK(state.source().position() == 4 && state.capture(left)->version() == 1 && notice.size == 2);
    CHECK(state.update(left, first->uuid, bytes("two"), 2));
    CHECK(state.source().position() == 5 && state.capture(left)->version() == 2 && notice.size == 3);
    const auto old = state.update(left, first->uuid, bytes("old"), 1);
    const auto conflict = state.update(left, first->uuid, bytes("wrong"), 2);
    CHECK(!old && old.error() == State::Error::obsolete);
    CHECK(!conflict && conflict.error() == State::Error::conflict);
    CHECK(state.source().position() == 5);
    const auto events = state.events(2);
    CHECK(events && events->size() == 3);
    CHECK((*events)[0].form == State::Source::Form::renew && (*events)[1].form == State::Source::Form::data);
    CHECK((*events)[0].record->attr == attr && (*events)[0].record->deadline == Clock::Time(2200ms));
    const auto changes = state.changes(left, 1);
    CHECK(changes && changes->size() == 1 && changes->front().data && changes->front().record->attr == attr);
    frozen->each([&](const std::string& key, const State::Content& content) { CHECK(key == first->uuid && content.attr == attr && content.data == initial); });

    time.value = 2000ms;
    state.tick();
    CHECK(state.source().position() == 6 && state.source().size() == 1); // right 权威到期, left 的续租仍有效.
    CHECK(state.capture(right)->size() == 0 && state.capture(right)->version() == 2);
    CHECK(state.capture(left)->size() == 1 && state.capture(left)->version() == 2);
    CHECK(state.remove(left, first->uuid));
    CHECK(state.source().position() == 7 && state.source().size() == 0 && state.capture(left)->version() == 3);
    const auto ended = state.renew(left, first->uuid, 2);
    CHECK(!ended && ended.error() == State::Error::ended);
    CHECK(!state.find(left, first->uuid)->record && state.find(left, first->uuid)->version == 3);
    const auto recreated = state.create(left, attr, initial, 1000);
    CHECK(recreated && recreated->uuid != first->uuid && state.capture(left)->version() == 4);
    CHECK(!notice.overflow && notice.size == 6);
}

// 准备末尾重读时钟: 新注册从最终时间计 TTL, 已到期注册不因准备前有效而被续活.
void boundary() {

    Time time;
    State state([&] { return time.read(); }, {});
    const Scope scope{"time", "boundary"};
    const auto data = bytes("data");
    time.jump = 2s;
    const auto first = state.create(scope, data, data, 1000);
    CHECK(first && time.value == 3s);
    state.source().each([](const Scope&, const std::string&, const Ephemeris::Record& record) { CHECK(record.deadline == Clock::Time(4s)); });
    time.value = 3500ms;
    time.jump = 600ms;
    const auto rejected = state.renew(scope, first->uuid, 1);
    CHECK(!rejected && rejected.error() == State::Error::ended);
    CHECK(state.events(0)->size() == 1); // 原生导出不执行 TTL 清理, 也没有成功续租.
    state.tick();                        // 明确推进才发布权威删除.
    const auto end = state.events(0);
    CHECK(end && end->size() == 2 && end->back().form == State::Source::Form::erase && end->back().position == 2);
    CHECK(state.capture(scope)->size() == 0 && state.capture(scope)->version() == 2);

    const auto second = state.create(scope, data, data, 1000);
    CHECK(second);
    CHECK(state.renew(scope, second->uuid, 1));
    time.jump = 2s;
    const auto duplicate = state.renew(scope, second->uuid, 1);
    CHECK(!duplicate && duplicate.error() == State::Error::ended); // 幂等确认也重新检查期限.
    state.tick();
    const auto position = state.source().position();
    time.ready = false;
    const auto unavailable = state.create(scope, data, data, 1000);
    CHECK(!unavailable && unavailable.error() == State::Error::clock);
    time.ready = true; // synchronized 一直 false, 参考源不可达不阻止本地新租约.
    const auto local = state.create(scope, data, data, 1000);
    CHECK(local && state.source().position() == position + 1);
    time.value += 1h;
    state.tick();
    CHECK(state.source().size() == 0 && state.capture(scope)->size() == 0);
}

// Data 更新的最终复核只检查活性, 仍必须拒绝准备期间到期, 同值新 order 也不能例外.
void updates() {

    for (const bool same : {true, false}) {
        Time time; // 每个分支独立从 1 s 建立 1 s 租约.
        State state([&] { return time.read(); }, {});
        const Scope scope{"data", "boundary"};
        const auto original = bytes(std::string(65536, 'a')); // 足够长且独立拥有, 可以覆盖内容比较路径.
        const auto created = state.create(scope, original, original, 1000);
        CHECK(created);
        const auto held = state.capture(scope); // 失败后这份旧完整视图仍须保持原始 Data.
        CHECK(held && held->version() == 1);

        time.value = 1500ms;
        time.jump = 500ms; // 第一次检查合法, 最终采样恰好到 2 s 截止边界.
        const auto updated = state.update(scope, created->uuid, bytes(std::string(65536, same ? 'a' : 'b')), 1);
        CHECK(!updated && updated.error() == State::Error::ended);
        CHECK(state.events(0)->size() == 1); // 失败的 Data 更新没有制造来源提交.
        state.tick();                        // 到期推进与历史导出分开, 仍只能看到 Create 和 Erase.
        const auto history = state.events(0);
        CHECK(history && history->size() == 2 && history->back().form == State::Source::Form::erase);
        CHECK(state.capture(scope)->version() == 2 && state.capture(scope)->size() == 0 && held->version() == 1);
        held->each([&](const std::string&, const State::Content& record) { CHECK(record.data == original); });
    }
}

// 统一读取入口保留原错误映射, 取时失败不偷建范围, 异常展开后仍可正常注册.
void reads() {

    Time time; // 与其他组合层用例共用确定性业务时钟.
    time.ready = false;
    bool fail{};          // 下一次取时是否抛错, 默认关闭, 触发后立即复原.
    State::Limits limits; // 只有一个范围名额, 失败路径不能占用它.
    limits.scopes = 1;
    State state([&] {
        if (std::exchange(fail, false)) {
            throw std::bad_alloc{};
        }
        return time.read();
    },
                limits);
    const Scope pending{"pending", "main"}, active{"active", "main"}; // 分离失败输入与最终正常注册的范围.
    const auto uuid = Ephemeris::uuid();                              // 合法但尚未注册的 UUID, 不让输入校验掩盖时钟错误.
    CHECK(state.capture({}) == std::unexpected(State::Error::input));
    CHECK(state.capture(pending) == std::unexpected(State::Error::clock));
    CHECK(state.find(pending, uuid) == std::unexpected(State::Error::clock));
    CHECK(state.changes(pending, 0) == std::unexpected(State::Error::clock));
    CHECK(state.events(0) && state.events(0)->empty()); // 无时钟也可导出已有空来源, 不隐式清理.
    CHECK(state.deliver(0, 8, 4096));                   // 来源恢复不依赖公共读取的时钟资格.

    time.ready = fail = true;
    bool caught{}; // 确认经过异常展开后再检查锁/目录可用性.
    try {
        static_cast<void>(state.capture(pending));
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    CHECK(caught);
    for (unsigned index = 0; index < 4; ++index) {
        const Scope unknown{"unknown", std::to_string(index)}; // 读取范围数超过写入额度, 不能挤掉真正的业务 Scope.
        const auto view = state.capture(unknown);              // 空范围完整根, 不借用临时 Scene.
        const auto point = state.find(unknown, uuid);          // 同边界精确缺项.
        const auto replay = state.changes(unknown, 0);         // 零游标表示已追平合法空基线.
        CHECK(view && view->version() == 0 && view->size() == 0 && view->bytes() == 0);
        CHECK(point && point->version == 0 && !point->record && !point->name);
        CHECK(replay && replay->empty());
        CHECK(state.changes(unknown, 1) == std::unexpected(State::Error::input));
    }
    const auto empty = state.capture(active); // 未创建时的冻结根不能因后来首次写入而发生变化.
    CHECK(empty && state.create(active, bytes("attr"), bytes("data"), 1000));
    CHECK(empty->page(0, 1, [](const auto&, const auto&) { CHECK(false); return true; }) == 0);
    CHECK(state.capture(active)->size() == 1 && state.capture(active)->version() == 1);
    CHECK(state.capture(pending)->version() == 0 && !state.find(pending, uuid)->record && state.changes(pending, 0)->empty());
    CHECK(state.create(pending, bytes("attr"), bytes("data"), 1000) == std::unexpected(State::Error::capacity));
    CHECK(state.changes(active, 2) == std::unexpected(State::Error::input));
    CHECK(state.events(2) == std::unexpected(State::Error::input));
    CHECK(state.deliver(2, 8, 4096) == std::unexpected(State::Error::input));
}

// 到期后最后一份 Attr/Data 的析构可以重新取得域锁, 统一读取不能把旧资源回收移动到持锁区.
void reclaim() {

    Time time;                   // 原截止为 2 s, 直接推进后由读取触发清理.
    bool released{}, unlocked{}; // 结果存活至 State 之后, 异常退出同样不会借用悬空状态.
    State::Limits limits;        // 两类历史均不持有载荷, 本用例只检查当前原生状态与投影的回收.
    limits.history = limits.source.history = 0;
    const auto state = std::make_shared<State>([&] { return time.read(); }, limits);
    const Scope scope{"expiry", "reclaim"}; // 单个注册的公开范围.
    Ephemeris::Value value(new Ephemeris::Buffer(16), [owner = std::weak_ptr<State>(state), &released, &unlocked](const Ephemeris::Buffer* payload) noexcept {
        released = true;
        try {
            const auto live = owner.lock(); // 不形成 State -> Value -> State 强引用环.
            unlocked = live && live->received("absent") == std::unexpected(State::Error::input);
        } catch (...) {
            unlocked = false; // 回收异常只记录失败, 不从 noexcept 析构逃逸.
        }
        delete payload;
    });
    CHECK(state->create(scope, value, value, 1000));
    value.reset(); // Attr/Data 共用测试载荷, 生产状态释放全部引用后才触发析构.
    CHECK(!released);

    time.value = 2s;
    const auto view = state->capture(scope); // 必须先解锁, 再回收本次到期的源端/投影资源.
    CHECK(view && view->size() == 0 && released && unlocked);
}

// 来源准备成功但投影容量拒绝时, 不保留半条记录、来源位置或失败请求偷建的 Scope.
void capacity() {

    Time time;
    State::Limits limits;
    limits.scopes = 1;
    limits.projection.bytes = 700;
    State state([&] { return time.read(); }, limits);
    const auto oversized = std::make_shared<const Ephemeris::Buffer>(1024, 7);
    const Scope failed{"failed", "scope"};
    const Scope accepted{"accepted", "scope"};
    const auto rejected = state.create(failed, oversized, bytes("small"), 1000);
    CHECK(!rejected && rejected.error() == State::Error::capacity);
    CHECK(state.source().position() == 0 && state.source().size() == 0);
    const auto good = state.create(accepted, bytes("a"), bytes("b"), 1000);
    CHECK(good && state.source().position() == 1); // 失败范围没有占满 scopes=1.
    const auto failed_update = state.update(accepted, good->uuid, oversized, 1);
    CHECK(!failed_update && failed_update.error() == State::Error::capacity);
    CHECK(state.source().position() == 1 && state.capture(accepted)->version() == 1);
    CHECK(*state.find(accepted, good->uuid)->record->data == *bytes("b"));
    CHECK(state.remove(accepted, good->uuid));
    const auto blocked = state.create(failed, bytes("a"), bytes("b"), 1000);
    CHECK(!blocked && blocked.error() == State::Error::capacity); // 已公开的空范围仍保留游标, 不能重用位置.
}

// 历史可以为零, 但实时收集器仍得到所有真实内容变化; 续租不伪造内容通知.
void history() {

    Time time;
    State::Limits limits;
    limits.history = 0;
    limits.source.history = 0;
    State state([&] { return time.read(); }, limits);
    Notice notice;
    state.notify(Notice::receive, &notice);
    const Scope scope{"zero", "history"};
    const auto first = state.create(scope, bytes("a"), bytes("b"), 1000);
    CHECK(first && notice.size == 1);
    CHECK(!state.changes(scope, 0) && state.changes(scope, 0).error() == State::Error::history);
    CHECK(!state.events(0) && state.events(0).error() == State::Error::history);
    CHECK(state.changes(scope, 1)->empty() && state.events(1)->empty());
    CHECK(state.renew(scope, first->uuid, 1) && notice.size == 1);
    CHECK(state.remove(scope, first->uuid) && notice.size == 2);
    const auto missing = state.capture(Scope{"empty", "scope"});
    CHECK(missing && missing->size() == 0 && missing->version() == 0);
}

// 内容 View 超过 State 寿命仍安全, 读回调可以重入活着的 State, 不在域锁内调用它.
void lifetime() {

    Time time;
    const Scope scope{"view", "lifetime"};
    std::optional<State::Projection::View> old;
    {
        State state([&] { return time.read(); }, {});
        const auto first = state.create(scope, bytes("a"), bytes("b"), 1000);
        CHECK(first);
        old = *state.capture(scope);
        old->each([&](const std::string& key, const State::Content&) { CHECK(state.remove(scope, key)); });
        CHECK(state.capture(scope)->size() == 0);
    }
    std::size_t count{};
    old->each([&](const std::string&, const State::Content& content) { ++count; CHECK(*content.attr == *bytes("a") && *content.data == *bytes("b")); });
    CHECK(count == 1 && old->version() == 1);
}
} // namespace

// 独立组合层用例, 不启动网络、Pulsar 或长期测试.
int main() {
    try {
        exports<State>([](State& state, const Scope& scope) { const auto receipt = state.create(scope, bytes("attr"), bytes("data"), 1000); CHECK(receipt); return receipt->uuid; });
        lifecycle();
        boundary();
        updates();
        reads();
        reclaim();
        capacity();
        history();
        lifetime();
        std::cout << "Ephemeris state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
