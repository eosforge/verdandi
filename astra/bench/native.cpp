#include "catalog_state.hpp"
#include "ephemeris_state.hpp"
#include "measure.hpp"
#include <string>

namespace {
using namespace std::chrono_literals;

// 固定业务时间排除真实过期干扰; 另设 Agenda 场景测真实追赶算法, 不伪称已经测过时钟服务.
auto reading() {
    return std::optional(astra::Clock::Reading{.time = astra::Clock::Time(1s), .ready = true});
}

// 分别测当前生产原生状态, 不以已冻结的通用 Store 代替 Catalog/Ephemeris.
void run(std::size_t count, std::size_t bytes) {

    const astra::Scope scope{"measure", "native"};                                 // 单范围, 无内部名称绕过.
    const auto value = std::make_shared<const astra::Catalog::Buffer>(bytes, 42);  // 正文不可变共享, 分配在窗口外.
    const auto other = std::make_shared<const astra::Catalog::Buffer>(bytes, 43);  // 交替正文确保 Update 真正修改内容.
    std::vector<std::string> keys(count);                                          // 名称预构造, 不测调用方格式化.
    std::vector<std::uint64_t> versions(count, 1);                                 // 每 Key 内容版本独立, 初值一.
    astra::Catalog::State catalog(reading, astra::Catalog::State::Limits{});       // 使用生产默认容量.
    astra::Ephemeris::State ephemeris(reading, astra::Ephemeris::State::Limits{}); // 独立来源及时间轮.
    for (std::size_t index = 0; index < count; ++index) {
        keys[index] = "key-" + std::to_string(index); // 顺序名称集合, 哈希表实际计算散列.
        Measure::require(catalog.publish(scope, keys[index], value, 1, 600000).has_value());
    }

    Measure::run("catalog.find", 20000, [&](std::size_t index) { const auto found = catalog.find(scope, keys[index % count]); Measure::require(found && found->record && found->record->value->size() == bytes); });
    Measure::run("catalog.capture", 20000, [&](std::size_t) { const auto view = catalog.capture(scope); Measure::require(view && view->size() == count); });
    Measure::run("catalog.publish", 20000, [&](std::size_t index) { const auto slot = index % count; const auto version = ++versions[slot]; Measure::require(catalog.publish(scope, keys[slot], version % 2 ? value : other, version, 600000).has_value()); });

    // 持有旧 View 时重复修改, 单独观察 COW 开销, 并在结束后校验旧根保持可读.
    const auto frozen = catalog.capture(scope); // 整个后续写入期间保留旧根.
    Measure::require(frozen.has_value());
    Measure::run("catalog.publish_with_view", 20000, [&](std::size_t index) { const auto slot = index % count; const auto version = ++versions[slot]; Measure::require(catalog.publish(scope, keys[slot], version % 2 ? value : other, version, 600000).has_value()); });
    std::size_t visited{}; // 只验证旧视图完整性, 不混入写入样本.
    frozen->each([&](const auto&, const auto&) { ++visited; });
    Measure::require(visited == count);

    // 注册阶段单独计时, UUID 随机生成属于实际 Create 路径; 随后 update/renew 不重新格式化 UUID.
    std::vector<std::string> identities(count); // Create 返回的真实 UUID, 全部保留至场景退出.
    Measure::run("ephemeris.create", count, [&](std::size_t index) { auto result = ephemeris.create(scope, value, value, 600000); Measure::require(result.has_value()); identities[index] = std::move(result->uuid); });
    Measure::run("ephemeris.update", 20000, [&](std::size_t index) { Measure::require(ephemeris.update(scope, identities[index % count], index / count % 2 ? value : other, index + 1).has_value()); });
    Measure::run("ephemeris.renew", 20000, [&](std::size_t index) { Measure::require(ephemeris.renew(scope, identities[index % count], index + 1).has_value()); });
    Measure::run("ephemeris.find", 20000, [&](std::size_t index) { const auto found = ephemeris.find(scope, identities[index % count]); Measure::require(found && found->record); });

    // 单来源全量重建包含私有 Draft 构造、原子安装及接收者析构, 与 O(1) capture 分开统计.
    const auto source = catalog.source(); // 冻结完整来源, 不借合并投影生成虚假的远端事实.
    Measure::run("catalog.recover", 5, [&](std::size_t) {
        astra::Catalog::State receiver(reading, {}); // 每次模拟一个空接收者, 不走同版本短路.
        Measure::require(receiver.admit("source").has_value());
        auto draft = receiver.prepare("source", source.position());
        Measure::require(draft.has_value());
        source.each([&](const auto& address, const auto& key, const auto& record) { Measure::require(draft->set(address, key, record).has_value()); });
        Measure::require(receiver.replace("source", std::move(*draft)).has_value());
        const auto view = receiver.capture(scope);
        Measure::require(view && view->size() == count);
    });
}

// 空闲发送准备也会检查所有来源的 TTL, 用实际活跃来源数观察其增长趋势.
void sources() {
    for (const auto count : {0, 7, 31}) {
        astra::Catalog::State state(reading, {}); // 每档独立来源容器, 生产默认副本上限未修改.
        const astra::Scope scope{"measure", "sources"};
        const auto value = std::make_shared<const astra::Catalog::Buffer>(16, 1);
        for (int index = 0; index < count; ++index) {
            const auto id = "source-" + std::to_string(index); // 每个副本一条记录及实际时间轮.
            Measure::require(state.admit(id).has_value());
            Measure::require(state.apply(id, 1, scope, id, {1, value, astra::Clock::Time(600s)}, astra::Catalog::State::Source::Form::record).has_value());
        }
        Measure::run("catalog.idle_sources_" + std::to_string(count), 20000, [&](std::size_t) { Measure::require(state.deliver(0, 256, 65536).has_value()); });
    }
}

// 用当前实现测空轮长暂停追赶, 不假设一亿拍只需几十毫秒.
void advance() {
    for (const auto hours : {1, 24, 168}) {
        astra::Agenda agenda(astra::Clock::Time(1s)); // 每个时长使用新轮, 排除上一场景的状态.
        Measure::run("agenda.hours_" + std::to_string(hours), 1, [&](std::size_t) { agenda.advance(astra::Clock::Time(1s + std::chrono::hours(hours)), [](auto*, auto) { throw std::runtime_error("Empty agenda fired"); }); });
    }
}
} // namespace

// 参数为生产容量内记录数和正文字节; 非法或失败返回非零, 不输出伪成功数据.
int main(int count, char** arguments) {
    try {
        Measure::require(count == 3);
        const auto records = Measure::number(arguments[1], 1, 10000), bytes = Measure::number(arguments[2], 1, 4096); // 两个显式有界场景参数, 完整消费输入.
        run(records, bytes);
        sources();
        advance();
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
