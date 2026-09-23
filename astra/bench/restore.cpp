#include "catalog_state.hpp"
#include "measure.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <thread>

namespace {
using namespace std::chrono_literals;
using State = astra::Catalog::State; // 专测跨来源最高水位合并, 不把数据模型替换成假互斥量.

// 固定已校准业务时间, 排除 Pulsar/真实 TTL 与网络的干扰, 此探针不作为系统吞吐成绩.
auto reading() {
    return std::optional(astra::Clock::Reading{.time = astra::Clock::Time(1s), .ready = true});
}

// 六个本机写者与两个远端完整来源恢复并行, scopes 控制相同记录量的恢复原子范围大小.
void run(std::size_t records, std::size_t scopes, bool concurrent) {

    State target(reading, {});                                                     // 三个来源共用真实生产提交器和默认逻辑内存预算.
    const auto body = std::make_shared<const astra::Catalog::Buffer>(128, 1);      // 窗口外分配的不可变载荷.
    const auto alternate = std::make_shared<const astra::Catalog::Buffer>(128, 2); // 交替正文, 避免同内容捷径.
    std::vector<astra::Scope> addresses;                                           // 每个远端 Scope 共用固定地址, 名称构造不混入写延迟.
    for (std::size_t index = 0; index < scopes; ++index)
        addresses.push_back({"remote", std::to_string(index)});
    const astra::Scope local{"local", "writers"}; // SDK 写入范围与远端不同, 仍可能竞争现有域锁.
    std::array<std::string, 6> keys;              // 每个写者独占自己的 Key/业务版本, 没有业务冲突重试.
    for (std::size_t index = 0; index < keys.size(); ++index) {
        keys[index] = "writer-" + std::to_string(index);
        Measure::require(target.publish(local, keys[index], body, 1, 600000).has_value());
    }
    std::array<std::string, 2> sources{"remote-a", "remote-b"}; // 两个已准入且编号独立的远端来源.
    for (const auto& source : sources)
        Measure::require(target.admit(source).has_value());

    constexpr std::size_t operations = 10000;            // 每个写者固定工作量, 失败不缩短计数伪造成功.
    std::array<std::vector<std::int64_t>, 6> samples;    // 各线程仅写自己的数组, 汇总在全部退出后进行.
    std::array<Measure::Clock::time_point, 6> completed; // 每个写者的结束点, 吞吐不计入等待恢复线程退出的尾部.
    std::array<std::vector<std::int64_t>, 2> recovery;   // 每个来源完整恢复时长, 包含私有 Draft 构造与安装.
    std::atomic_bool failed{};                           // 捕获线程内异常后报告整场失败, 不让异常逃逸为 terminate.
    std::barrier barrier(9);                             // 六个写者、两个恢复者与启动线程共用一个起点.
    std::vector<std::jthread> workers;                   // 正常/异常退出均等待本轮自有线程, 不留后台负载.

    struct Launch {
        std::barrier<>& barrier;            // 启动栅栏, 已启动线程在此等待剩余参与者.
        std::vector<std::jthread>& workers; // 当前确实创建成功的工作线程.
        bool started{};                     // 主线程已正常加入栅栏后不再补偿人数.

        ~Launch() {
            if (!started) {
                for (auto missing = workers.size(); missing < 9; ++missing)
                    barrier.arrive_and_drop(); // 创建失败时先放行已启动线程再 join.
            }
        }
    } launch{barrier, workers}; // 先于 workers 析构, 避免资源拒绝时卡在启动栅栏.

    workers.reserve(8);
    for (auto& values : samples)
        values.resize(operations);
    for (auto& values : recovery)
        values.reserve(16);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        workers.emplace_back([&, index] {
            barrier.arrive_and_wait();
            try {
                for (std::size_t operation = 0; operation < operations; ++operation) {
                    const auto start = Measure::Clock::now(); // 包含真实提交锁等待和原生/可见投影提交.
                    Measure::require(target.publish(local, keys[index], operation % 2 ? body : alternate, operation + 2, 600000).has_value());
                    samples[index][operation] = std::chrono::duration_cast<std::chrono::nanoseconds>(Measure::Clock::now() - start).count();
                }
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
            }
            completed[index] = Measure::Clock::now();
        });
    }
    for (std::size_t index = 0; index < sources.size(); ++index) {
        workers.emplace_back([&, index] {
            barrier.arrive_and_wait();
            try {
                for (std::uint64_t round = 1; concurrent && round <= 16; ++round) {
                    const auto start = Measure::Clock::now(); // 完整恢复端到端时间, 不将 O(1) 捕获冒充恢复成本.
                    auto draft = target.prepare(sources[index], round);
                    Measure::require(draft.has_value());
                    for (std::size_t record = 0; record < records; ++record) {
                        Measure::require(draft->set(addresses[record % scopes], sources[index] + "-" + std::to_string(record), {round, round % 2 ? body : alternate, astra::Clock::Time(600s)}).has_value());
                    }
                    Measure::require(target.replace(sources[index], std::move(*draft)).has_value());
                    recovery[index].push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Measure::Clock::now() - start).count());
                }
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    const auto start = Measure::Clock::now(); // 起点先公布再解开栅栏, 不在线程运行后补记时间.
    launch.started = true;
    barrier.arrive_and_wait();
    workers.clear();
    Measure::require(!failed.load(std::memory_order_relaxed));

    std::vector<std::int64_t> writes; // 汇总移动只在计时结束后执行, 不干扰原生写入.
    writes.reserve(operations * samples.size());
    for (auto& values : samples)
        writes.insert(writes.end(), values.begin(), values.end());
    const auto seconds = std::chrono::duration<double>(*std::ranges::max_element(completed) - start).count();
    Measure::report(concurrent ? "catalog.writes_during_recovery" : "catalog.concurrent_writes", std::move(writes), seconds, operations * samples.size());
    for (std::size_t index = 0; index < recovery.size(); ++index) {
        if (concurrent)
            Measure::report(sources[index], std::move(recovery[index]), std::chrono::duration<double>(Measure::Clock::now() - start).count(), 16);
    }
    for (const auto& key : keys) {
        const auto point = target.find(local, key); // 明确验证查询成功, 避免探针自身在错误路径解引用空结果.
        Measure::require(point.has_value() && point->record && point->record->version == operations + 1);
    }
    for (const auto& source : sources)
        Measure::require(target.received(source) == (concurrent ? 16 : 0));
}
} // namespace

// 参数为每远端记录数、Scope 数和恢复开关; 仅使用已有工具/产物, 不启动网络或外部服务.
int main(int count, char** arguments) {
    try {
        Measure::require(count == 4);
        const auto records = Measure::number(arguments[1], 1, 10000), scopes = Measure::number(arguments[2], 1, 128);
        Measure::require(records >= scopes && records % scopes == 0);
        run(records, scopes, Measure::number(arguments[3], 0, 1) != 0);
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
