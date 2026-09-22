#include "check.hpp"
#include "ephemeris_state.hpp"
#include <iostream>

namespace {
using namespace std::chrono_literals;
using astra::Clock;
using astra::Ephemeris;
using astra::Scope;
using State = Ephemeris::State;

// 原生不可变正文, 使用实际共享 Buffer 类型, 不经序列化假装跨进程.
Ephemeris::Value bytes(std::string_view value) {
    return std::make_shared<const Ephemeris::Buffer>(value.begin(), value.end());
}

// 从真实自有完整根读取测试记录, 不借用随后来源写入可能替换的页.
Ephemeris::Record native(State& state, std::string_view uuid) {

    std::optional<Ephemeris::Record> result;
    state.source().each([&](const Scope&, const std::string& key, const Ephemeris::Record& value) {
        if (key == uuid) {
            result = value;
        }
    });
    CHECK(result);
    return *result;
}

// 捕获完整来源后逐 Scope 填充私有候选, 最后才让接收者发布完整原生状态/投影.
void copy(State& sender, State& receiver, std::string_view id) {

    const auto source = sender.source();
    auto draft = receiver.prepare(id, source.position());
    CHECK(draft);
    source.each([&](const Scope& scope, const std::string& key, const Ephemeris::Record& value) { CHECK(draft->set(scope, key, value)); });
    CHECK(receiver.replace(id, std::move(*draft)));
}

// 副本本地 TTL 删除不广播, 缺失后由权威完整事实恢复原 UUID, 重放不能延长期限.
void lifetime() {

    auto left_time = 1000ms, right_time = 1000ms;
    State left([&] { return std::optional(Clock::Reading{.time = Clock::Time(left_time), .ready = true}); }, {});
    State right([&] { return std::optional(Clock::Reading{.time = Clock::Time(right_time), .ready = true}); }, {});
    const Scope scope{"service", "main"}, other{"service", "other"};
    CHECK(right.admit("star-a"));
    const auto first = left.create(scope, bytes("attr"), bytes("one"), 1000);
    const auto second = left.create(other, bytes("other-attr"), bytes("two"), 1000);
    const auto local = right.create(scope, bytes("local"), bytes("own"), 10000);
    CHECK(first && second && local);
    copy(left, right, "star-a");
    CHECK(right.received("star-a") == 2 && right.source().position() == 1);
    CHECK(right.capture(scope)->size() == 2 && right.capture(other)->size() == 1);
    CHECK(right.renew(scope, first->uuid, 1).error() == State::Error::ended); // 副本不获得权威写权限.

    right_time = 2200ms;
    right.tick();
    CHECK(right.capture(scope)->size() == 1 && right.capture(other)->size() == 0);
    CHECK(right.received("star-a") == 2 && right.source().position() == 1);
    CHECK(right.events(0)->size() == 1); // 删除只影响本地 Observer, 不写自己的源日志.
    left_time = 1500ms;
    CHECK(left.renew(scope, first->uuid, 1));
    const auto renewed = native(left, first->uuid);
    CHECK(renewed.deadline == Clock::Time(2500ms));
    CHECK(right.apply("star-a", 3, scope, first->uuid, renewed, State::Source::Form::renew).error() == State::Error::ended);
    CHECK(right.received("star-a") == 2);
    CHECK(right.apply("star-a", 3, scope, first->uuid, renewed, State::Source::Form::record)); // 回补完整原生状态后可确认此连续位置.
    CHECK(right.find(scope, first->uuid)->record && right.received("star-a") == 3);
    auto replayed = renewed;
    replayed.deadline = Clock::Time(100s);
    CHECK(right.apply("star-a", 3, scope, first->uuid, replayed, State::Source::Form::record));
    right_time = 2600ms;
    right.tick();
    CHECK(!right.find(scope, first->uuid)->record); // 同位置重放没有采用伪造更晚期限.
    CHECK(right.find(scope, local->uuid)->record && right.source().position() == 1);
}

// 重放保留的旧注册和权威删除, 接收者逐项确认但不能在两项之间重新公开已经到期的注册.
void expiry() {

    auto now = 1s; // 可控业务时间, 不修改实际时钟或等待历史保留窗口.
    State sender([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    State receiver([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope scope{"replay", "expired"}; // 每次用例只包含一条独立注册.
    CHECK(receiver.admit("source"));
    const auto registration = sender.create(scope, bytes("attr"), bytes("data"), 1000); // 原截止为 2 s.
    CHECK(registration);

    now = 2s;                                      // 公开历史读取必须先到期, 不要求调用方事先 tick.
    const auto changes = sender.changes(scope, 1); // 原注册的删除仍进入下游连续投影.
    CHECK(changes && changes->size() == 1 && !changes->front().record);
    const auto delivery = sender.deliver(0, 8, 4096); // 权威到期已经追加源端删除, 旧创建仍在连续历史中.
    CHECK(delivery && !delivery->baseline && delivery->events.size() == 2);
    CHECK(delivery->events.front().record && delivery->events.front().record->deadline == Clock::Time(2s));
    CHECK(!delivery->events.back().record && delivery->events.back().form == State::Source::Form::erase);
    for (const auto& event : delivery->events) { // 每项后检查投影, 避免只看最终空状态掩盖中途复活.
        CHECK(receiver.apply("source", event.position, *event.name->scope, event.name->key, event.record, event.form));
        const auto visible = receiver.find(scope, registration->uuid); // 借原 deadline 判断, 不从接收时重新加 ttl.
        CHECK(visible && !visible->record && receiver.received("source") == event.position);
    }
    CHECK(receiver.source().position() == 0); // 副本清理不能制造本机权威事件再次广播.
}

// 连续位置、Attr/TTL 固定、双序号及来源身份碰撞逐项验证, 拒绝不改变已安装位置.
void ordering() {

    auto now = 1s;
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope scope{"service", "main"};
    const auto uuid = Ephemeris::uuid();
    Ephemeris::Record first{bytes("attr"), bytes("first"), Clock::Time(10s), 0, 0, 1000};
    CHECK(state.admit("a") && state.admit("b"));
    CHECK(!state.apply("a", 2, scope, uuid, first, State::Source::Form::record));
    CHECK(state.received("a") == 0);
    CHECK(state.apply("a", 1, scope, uuid, first, State::Source::Form::record));
    CHECK(state.apply("b", 1, scope, uuid, first, State::Source::Form::record).error() == State::Error::conflict);
    CHECK(state.received("b") == 0);
    auto bad = first;
    bad.attr = bytes("changed");
    CHECK(!state.apply("a", 2, scope, uuid, bad, State::Source::Form::record));
    bad = first;
    bad.data = bytes("next");
    bad.update = 7;
    bad.deadline = Clock::Time(20s);
    CHECK(!state.apply("a", 2, scope, uuid, bad, State::Source::Form::data)); // Data 不得隐式延期.
    bad.deadline = first.deadline;
    CHECK(state.apply("a", 2, scope, uuid, bad, State::Source::Form::data));
    const auto content = state.capture(scope);
    bad.renewal = 10;
    bad.deadline = Clock::Time(20s);
    CHECK(state.apply("a", 3, scope, uuid, bad, State::Source::Form::renew));
    CHECK(state.capture(scope)->version() == content->version()); // 纯期限更新不重复推送正文.
    auto invalid = bad;                                           // 完整事实和精确回补同样不能绕过续租序号偷改截止.
    invalid.deadline = Clock::Time(30s);
    CHECK(!state.apply("a", 4, scope, uuid, invalid, State::Source::Form::record));
    CHECK(!state.repair("a", 8, scope, uuid, invalid));
    auto baseline = state.prepare("a", 8);
    CHECK(baseline && baseline->set(scope, uuid, invalid));
    CHECK(!state.replace("a", std::move(*baseline)));
    invalid.renewal = 11;
    invalid.deadline = Clock::Time(15s);
    CHECK(!state.apply("a", 4, scope, uuid, invalid, State::Source::Form::record));
    CHECK(state.received("a") == 3 && state.capture(scope)->version() == content->version());
    CHECK(state.apply("a", 4, scope, uuid, {}, State::Source::Form::erase));
    CHECK(state.received("a") == 4 && !state.find(scope, uuid)->record);
    CHECK(state.source().position() == 0 && state.source().size() == 0);
    CHECK(state.events(0)->empty());
}

// 整个来源基线的投影准备失败必须回滚所有 Scope, 不能只恢复原生根而漏掉公开部分修改.
void atomic() {

    auto now = 1s;
    State::Limits limits;
    limits.projection.records = 1;
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, limits);
    const Scope scope{"service", "main"};
    const auto original = Ephemeris::uuid(), other = Ephemeris::uuid();
    const Ephemeris::Record value{bytes("attr"), bytes("value"), Clock::Time(10s), 0, 0, 1000};
    CHECK(state.admit("a"));
    CHECK(state.apply("a", 1, scope, original, value, State::Source::Form::record));
    const auto frozen = state.capture(scope);
    auto draft = state.prepare("a", 5);
    CHECK(draft && draft->set(scope, original, value) && draft->set(scope, other, value));
    CHECK(!state.replace("a", std::move(*draft)));
    CHECK(state.received("a") == 1 && state.capture(scope)->version() == frozen->version());
    CHECK(state.find(scope, original)->record && !state.find(scope, other)->record);
    draft = state.prepare("a", 5); // 前次失败没有泄漏 Scope/所有权/容量, 删除旧项再增加新项可原子完成.
    CHECK(draft && draft->set(scope, other, value));
    CHECK(state.replace("a", std::move(*draft)));
    CHECK(state.received("a") == 5 && !state.find(scope, original)->record && state.find(scope, other)->record);
    frozen->each([&](const std::string& key, const auto&) { CHECK(key == original); });
    CHECK(!state.prepare("a", 4));
    auto empty = state.prepare("a", 6);
    CHECK(empty && state.replace("a", std::move(*empty)));
    CHECK(state.received("a") == 6 && state.capture(scope)->size() == 0);
}

// 精确回补只覆盖单个 UUID, 必须继续处理其他目标的中间事实, 本地 TTL 删除不能丢掉覆盖证据.
void repair() {

    auto now = 1000ms;
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope first_scope{"service", "one"}, second_scope{"service", "two"};
    const auto first = Ephemeris::uuid(), second = Ephemeris::uuid();
    Ephemeris::Record original{bytes("attr"), bytes("old"), Clock::Time(2s), 0, 0, 1000};
    CHECK(state.admit("a"));
    CHECK(state.apply("a", 1, first_scope, first, original, State::Source::Form::record));
    auto repaired = original;
    repaired.data = bytes("future");
    repaired.update = 3;
    repaired.renewal = 3;
    repaired.deadline = Clock::Time(3s);
    CHECK(state.repair("a", 5, first_scope, first, repaired));
    CHECK(state.received("a") == 1); // R=5 不是整个来源连续位置.
    CHECK(!state.prepare("a", 4));   // 低于已安装单目标覆盖位置的全量不能撤销它.
    CHECK(state.replica("a", first_scope, first)->value().data == repaired.data);
    CHECK(state.apply("a", 2, second_scope, second, original, State::Source::Form::record));
    now = 3500ms;
    state.tick(); // 两个目标均本地到期, 第一项的 R=5 覆盖依据仍必须存在.
    CHECK(!state.find(first_scope, first)->record);
    original.update = 1;
    original.data = bytes("stale");
    CHECK(state.apply("a", 3, first_scope, first, original, State::Source::Form::data));
    CHECK(state.apply("a", 4, first_scope, first, {}, State::Source::Form::erase));
    auto current = original;
    current.deadline = Clock::Time(5s);
    CHECK(state.apply("a", 5, second_scope, second, current, State::Source::Form::record));
    CHECK(state.received("a") == 5 && state.find(second_scope, second)->record);
    CHECK(!state.find(first_scope, first)->record);
    CHECK(state.prepare("a", 5)); // 连续位置已覆盖 R, 元数据可释放且不阻止合法同位置全量修复.
    CHECK(state.source().position() == 0);
}

// 可信身份替换只冻结新输入, 不突然删除仍有效的旧注册; 原截止到期后归还空来源容量.
void replacement() {

    auto now = 1000ms;
    State::Limits limits;
    limits.replicas = 1;
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, limits);
    const Scope scope{"service", "main"};
    const auto uuid = Ephemeris::uuid();
    const Ephemeris::Record value{bytes("attr"), bytes("value"), Clock::Time(2s), 0, 0, 1000};
    CHECK(state.admit("old"));
    CHECK(state.apply("old", 1, scope, uuid, value, State::Source::Form::record));
    auto delayed = state.prepare("old", 2);
    CHECK(delayed && delayed->set(scope, uuid, value));
    state.retire("old");
    CHECK(state.find(scope, uuid)->record && !state.admit("old"));
    CHECK(!state.replace("old", std::move(*delayed)));
    CHECK(!state.apply("old", 2, scope, uuid, {}, State::Source::Form::erase));
    CHECK(!state.admit("new")); // 有活动原生数据时仍占来源预算, 不因取消流提早释放.
    now = 2100ms;
    state.tick();
    CHECK(!state.find(scope, uuid)->record && !state.received("old"));
    CHECK(state.admit("new"));
    CHECK(state.source().position() == 0);
}
} // namespace

// 原生两端/多来源用例与独立的真实网络复制验收互补, 不相互替代.
int main() {

    try {
        lifetime();
        expiry();
        ordering();
        atomic();
        repair();
        replacement();
        std::cout << "Ephemeris replicas: ok\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
