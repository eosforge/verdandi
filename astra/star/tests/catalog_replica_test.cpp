#include "catalog_state.hpp"
#include "check.hpp"
#include <iostream>

namespace {
using namespace std::chrono_literals;
using astra::Catalog;
using astra::Clock;
using astra::Scope;
using State = Catalog::State;

// 完整正文, 空字符串仍是有所有者的有效记录, 不能冒充无载荷水位.
Catalog::Value bytes(std::string_view text) {
    return std::make_shared<const Catalog::Buffer>(text.begin(), text.end());
}

// 人工事实使用固定绝对截止, 接收者必须尊重它而不是从接收时重新加 TTL.
Catalog::Record record(std::uint64_t version, std::string_view value = "body", Clock::Time deadline = Clock::Time(10s)) {
    return {version, bytes(value), deadline};
}

// 真正的自有来源根经过 Draft 装入接收者, 不复制合并公开视图.
void copy(State& sender, State& receiver, std::string_view id) {

    const auto source = sender.source();
    auto draft = receiver.prepare(id, source.position());
    CHECK(draft);
    source.each([&](const Scope& scope, const std::string& key, const Catalog::Record& value) { CHECK(draft->set(scope, key, value)); });
    CHECK(receiver.replace(id, std::move(*draft)));
}

// 自有来源、水位与公开投影各有明确职责, 不借远端期限延长本机 Renew 资格.
void sources() {

    auto now = 1s;
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope scope{"route", "main"};
    CHECK(state.admit("a") && state.admit("b"));
    CHECK(state.publish(scope, "key", bytes("body"), 2, 1000));
    CHECK(state.apply("a", 1, scope, "key", record(2), State::Source::Form::record));
    CHECK(state.source().position() == 1 && state.capture(scope)->version() == 1);
    now = 3s;
    state.tick();
    CHECK(state.find(scope, "key")->record->version == 2);
    CHECK(state.renew(scope, "key", 2, 1000).error() == State::Error::ended);
    state.source().each([](const Scope&, const std::string&, const Catalog::Record& value) { CHECK(value.version == 2 && !value.value); });
    CHECK(state.apply("b", 1, scope, "key", record(8, "new"), State::Source::Form::record));
    CHECK(state.publish(scope, "key", bytes("body"), 2, 1000).error() == State::Error::version);
    CHECK(state.apply("a", 2, scope, "key", record(3, "old"), State::Source::Form::record));
    CHECK(state.find(scope, "key")->record->version == 8 && state.capture(scope)->version() == 2);
    CHECK(state.replica("a", scope, "key")->value().version == 3);
    CHECK(state.source().position() == 1); // 学到高版本也不能改写本机来源水位/广播.

    now = 11s;
    state.tick();
    CHECK(!state.find(scope, "key")->record && state.capture(scope)->version() == 3);
    CHECK(state.publish(scope, "key", bytes("old"), 7, 1000).error() == State::Error::version);
    CHECK(state.apply("a", 3, scope, "key", record(6, "older", Clock::Time(20s)), State::Source::Form::record));
    CHECK(!state.find(scope, "key")->record);
    CHECK(state.apply("b", 2, scope, "key", record(8, "new", Clock::Time(20s)), State::Source::Form::renew).error() == State::Error::ended);
    CHECK(state.repair("b", 2, scope, "key", record(8, "new", Clock::Time(20s))));
    CHECK(state.received("b") == 1 && state.find(scope, "key")->record->version == 8);
    CHECK(state.apply("b", 2, scope, "key", record(8, "new", Clock::Time(20s)), State::Source::Form::renew));
    CHECK(state.received("b") == 2 && state.source().position() == 1);
}

// 仍保留的源日志可包含已过期正文, 重放必须使用原截止, 只恢复水位而不重新发放 TTL.
void expiry() {

    auto now = 1s; // 两端共享可控业务时间, 历史保留时间独立使用单调时钟.
    State sender([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    State receiver([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope scope{"replay", "expired"}; // 独立范围, 初始没有内容或水位.
    CHECK(receiver.admit("source") && sender.publish(scope, "key", bytes("body"), 1, 1000));

    now = 2s;                                      // 恰好达到原 TTL, 直接读取增量也应先执行本地到期, 不依赖显式 tick.
    const auto changes = sender.changes(scope, 1); // 下游必须得到删除, 不能因旧创建仍在源日志中而保留正文.
    CHECK(changes && changes->size() == 1 && !changes->front().record);
    const auto delivery = sender.deliver(0, 8, 4096); // Catalog 到期不广播新源事件, 原创建历史仍可读取.
    CHECK(delivery && !delivery->baseline && delivery->events.size() == 1);
    const auto& event = delivery->events.front(); // 借用本次交付保持的完整旧事实.
    CHECK(event.record && event.record->value && event.record->deadline == Clock::Time(2s));
    CHECK(receiver.apply("source", event.position, *event.name->scope, event.name->key, *event.record, event.form));
    const auto known = receiver.replica("source", scope, "key"); // 迟到事实确认位置并保留版本下限, 不留下活动正文.
    CHECK(known && *known && (**known).version == 1 && !(**known).value && !(**known).deadline);
    const auto visible = receiver.find(scope, "key"); // 公共投影不能短暂或永久复活已过期内容.
    CHECK(visible && !visible->record && receiver.received("source") == 1 && receiver.source().position() == 0);
}

// 水位/完整来源缺项均不广播 Catalog 到期; 更高水位则必须排除仍有效的低版本.
void watermarks() {

    auto now = 1s;
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope scope{"route", "main"};
    CHECK(state.admit("a"));
    CHECK(state.apply("a", 1, scope, "key", record(5), State::Source::Form::record));
    const auto frozen = state.capture(scope);
    auto mark = state.prepare("a", 1);
    CHECK(mark && mark->set(scope, "key", {5, {}, {}}));
    CHECK(state.replace("a", std::move(*mark)));
    CHECK(state.find(scope, "key")->record && state.capture(scope)->version() == 1);
    auto empty = state.prepare("a", 1);
    CHECK(empty && state.replace("a", std::move(*empty)));
    CHECK(state.find(scope, "key")->record && !state.replica("a", scope, "key")->has_value());
    CHECK(state.apply("a", 2, scope, "key", {9, {}, {}}, State::Source::Form::record));
    CHECK(!state.find(scope, "key")->record && state.capture(scope)->version() == 2);
    frozen->each([](const std::string& key, const State::Content& value) { CHECK(key == "key" && value.version == 5 && *value.value == *bytes("body")); });
    CHECK(state.publish(scope, "key", bytes("lower"), 8, 1000).error() == State::Error::version);
    CHECK(state.repair("a", 3, scope, "key", {}));
    CHECK(state.publish(scope, "key", bytes("lower"), 8, 1000).error() == State::Error::version); // 未知也不归零合并下限.
    CHECK(state.publish(scope, "key", bytes(""), 9, 1000));
    CHECK(state.find(scope, "key")->record->value->empty());
}

// 回补 R 只覆盖指定目标, 本地过期后也必须继续挡住 R 以内的旧正文/旧截止.
void repair() {

    auto now = 1s;
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope one{"one", "main"}, two{"two", "main"};
    CHECK(state.admit("a"));
    CHECK(state.apply("a", 1, one, "key", record(1), State::Source::Form::record));
    CHECK(state.repair("a", 5, one, "key", record(3, "new", Clock::Time(2s))));
    CHECK(state.received("a") == 1);
    now = 3s;
    state.tick();
    CHECK(!state.find(one, "key")->record);
    CHECK(state.apply("a", 2, two, "other", record(1), State::Source::Form::record));
    CHECK(state.apply("a", 3, one, "key", record(2, "old", Clock::Time(100s)), State::Source::Form::record));
    CHECK(state.apply("a", 4, one, "key", record(2, "old", Clock::Time(101s)), State::Source::Form::renew));
    CHECK(!state.find(one, "key")->record && state.received("a") == 4);
    CHECK(state.prepare("a", 4).error() == State::Error::version); // 完整基线不能撤销 R=5 的局部证据.
    CHECK(state.apply("a", 5, two, "other", record(2, "other"), State::Source::Form::record));
    CHECK(state.received("a") == 5 && state.find(two, "other")->record->version == 2);
    CHECK(state.apply("a", 6, one, "key", record(3, "new", Clock::Time(20s)), State::Source::Form::record));
    CHECK(state.find(one, "key")->record->version == 3);
    CHECK(state.apply("a", 8, one, "key", record(4), State::Source::Form::record).error() == State::Error::history);
}

// 完整来源合并跨 Scope 原子失败, 新目录/水位/调度器都不能提前进入可见状态.
void rollback() {

    auto now = 1s;
    State::Limits limits;
    limits.projection.records = 1;
    State source([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    State target([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, limits);
    const Scope one{"one", "main"}, two{"two", "main"};
    CHECK(source.publish(one, "key", bytes("a"), 1, 1000));
    CHECK(source.publish(two, "key", bytes("b"), 1, 1000));
    CHECK(target.admit("source"));
    copy(source, target, "source");
    auto draft = target.prepare("source", 3);
    CHECK(draft && draft->set(one, "key", record(9)) && draft->set(two, "key", record(9)) && draft->set(two, "extra", record(1)));
    CHECK(target.replace("source", std::move(*draft)).error() == State::Error::capacity);
    CHECK(target.received("source") == 2 && target.find(one, "key")->record->version == 1 && target.find(two, "key")->record->version == 1);
    CHECK(!target.find(two, "extra")->record);
    CHECK(target.publish(one, "key", bytes("local"), 2, 1000)); // 失败候选的水位 9 没有泄漏.
    now = 3s;
    target.tick();
    CHECK(!target.find(one, "key")->record && !target.find(two, "key")->record); // 失败候选的 10 s 期限也没有泄漏.
}

// 同版本冲突拒绝整次安装, 可信退役禁止旧任务, 组回收仍保留本进程防回退水位.
void retired() {

    auto now = 1s;
    State::Limits limits;
    limits.replicas = 1;
    State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, limits);
    const Scope scope{"route", "main"};
    CHECK(state.admit("old"));
    CHECK(state.apply("old", 1, scope, "key", record(10, "v", Clock::Time(2s)), State::Source::Form::record));
    CHECK(state.apply("old", 2, scope, "key", record(10, "bad"), State::Source::Form::record).error() == State::Error::conflict);
    CHECK(state.received("old") == 1);
    auto delayed = state.prepare("old", 2);
    CHECK(delayed && delayed->set(scope, "key", record(20)));
    state.retire("old");
    CHECK(state.replace("old", std::move(*delayed)).error() == State::Error::version);
    CHECK(state.admit("next").error() == State::Error::capacity);
    CHECK(state.find(scope, "key")->record->version == 10);
    now = 3s;
    state.tick();
    CHECK(state.admit("next"));
    CHECK(state.apply("next", 1, scope, "key", record(9), State::Source::Form::record));
    CHECK(!state.find(scope, "key")->record);
    CHECK(state.publish(scope, "key", bytes("old"), 9, 1000).error() == State::Error::version);
}
} // namespace

// 组件恢复用例不代替实际双向流和进程故障回归, 本轮须另获运行授权.
int main() {

    try {
        sources();
        expiry();
        watermarks();
        repair();
        rollback();
        retired();
        std::cout << "catalog replicas: ok\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
