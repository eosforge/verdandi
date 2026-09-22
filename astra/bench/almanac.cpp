#include "almanac.hpp"
#include "measure.hpp"
#include <string>

namespace {
// 单 Scope 权威副本, 单 Key 严格 +1, 保留生产历史预算及正文拥有式 API 的分配成本.
void run(std::size_t count, std::size_t bytes) {

    astra::Almanac book;                     // 生产默认容量, 首份合法基线后才接纳增量.
    astra::Almanac::Buffer value(bytes, 42); // 调用方拥有的完整正文, apply 按值受理, 复制计入成本.
    std::vector<std::string> keys(count);    // 名称在计时外构造, 不包含调用方整数格式化.
    auto draft = book.prepare(1);            // 非空基线采用合法的正权威版本.
    for (std::size_t index = 0; index < count; ++index) {
        keys[index] = "key-" + std::to_string(index);
        Measure::require(draft.set(keys[index], value).has_value());
    }
    Measure::require(book.reset(std::move(draft)) == true);

    Measure::run("almanac.find", 20000, [&](std::size_t index) { const auto point = book.find(keys[index % count]); Measure::require(point && point->value && point->value->size() == bytes); });
    Measure::run("almanac.capture", 20000, [&](std::size_t) { const auto view = book.view(); Measure::require(view && view->size() == count); });
    std::uint64_t version = 1; // 唯一 Scope 版本, 每次合法 Apply 恰好推进一, 同值也提交.
    Measure::run("almanac.apply", 20000, [&](std::size_t index) { Measure::require(book.apply(++version, keys[index % count], value) == true); });

    const auto frozen = book.view(); // 保持旧根, 单独统计持续写入持有旧视图的成本.
    Measure::require(frozen.has_value());
    Measure::run("almanac.apply_with_view", 20000, [&](std::size_t index) { Measure::require(book.apply(++version, keys[index % count], value) == true); });
    std::size_t visited{}; // 锁外遍历校验, 不计作更新耗时.
    frozen->each([&](const auto&, const auto&) { ++visited; });
    Measure::require(visited == count);
    Measure::run("almanac.replay_one", 20000, [&](std::size_t) { const auto replay = book.replay(version - 1); Measure::require(replay && replay->changes.size() == 1); });

    // 全量重建包括载荷复制、索引构造、原子替换及旧状态释放, 不把 reset 的根交换冒充全部恢复成本.
    Measure::run("almanac.recover", 5, [&](std::size_t) {
        auto candidate = book.prepare(++version); // 每份完整基线使用新的权威版本.
        for (const auto& key : keys) {
            Measure::require(candidate.set(key, value).has_value());
        }
        Measure::require(book.reset(std::move(candidate)) == true);
    });
}
} // namespace

// 两个显式参数与原生动态域基准相同, 异常使样本无效, 不吞掉容量或版本错误.
int main(int count, char** arguments) {
    try {
        Measure::require(count == 3);
        const auto records = Measure::number(arguments[1], 1, 10000), bytes = Measure::number(arguments[2], 1, 4096); // 生产默认预算内的有限工作集, 不接受尾部垃圾.
        run(records, bytes);
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
