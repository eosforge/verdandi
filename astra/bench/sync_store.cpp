// 功能: 对内部 SyncStore 的增量提取、写入和快照构建做有限次数微基准, 不启动任何服务.
// 对照实验必须使用同一编译器、参数与本文件, 分别链接待比较版本的 sync_store.cpp/.hpp.
#include "sync_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

using namespace astra;

// 对 operation 测量七轮, 每轮 iterations 次, 输出每次调用耗时的中位数与防消除校验和.
// operation 返回可观察结果的整数摘要, 准备数据不计时. 本基准单线程, 不代表争用吞吐.
void measure(std::string_view name, std::size_t iterations, auto&& operation) {
    // samples 保存每轮 ns/op, 排序后取中位数以降低偶发调度噪声的影响.
    std::array<double, 7> samples{};
    // checksum 汇总实际查询结果, 不把结果未消费的空循环当作有效基准.
    std::uint64_t checksum = 0;
    // sample 是本轮耗时的写入位置, 各轮复用同一预置 Store.
    for (auto& sample : samples) {
        // started 使用单调时钟, 避免系统校时影响计时区间.
        const auto started = Clock::now();
        // iteration 仅控制调用次数, 不改变被测接口的业务输入.
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            checksum += operation();
        }
        sample = std::chrono::duration<double, std::nano>(Clock::now() - started).count() / static_cast<double>(iterations);
    }
    std::ranges::sort(samples);
    std::cout << "{\"case\":\"" << name << "\",\"median_ns\":" << samples[samples.size() / 2] << ",\"iterations\":" << iterations
              << ",\"checksum\":" << checksum << "}\n";
}

// 运行固定有限矩阵后退出. 数据准备、结果输出与 Store 析构不计入每次调用时间.
int main() {
    // batches 分别模拟小历史和默认容量历史, key_size 区分短字符串与需要堆分配的 Key.
    for (const std::uint64_t batches : {64U, 1000U}) {
        for (const std::size_t key_size : {8U, 128U}) {
            // store 的全部历史都是同 Key 覆盖, 避免把当前 Map 大小混入提取路径对比.
            SyncStore store(static_cast<std::size_t>(batches));
            // key 在计时外准备, 所有版本使用同样的 Key 长度和一字节载荷.
            const std::string key(key_size, 'k');
            // version 在准备阶段填满保留历史, 不计入提取耗时.
            for (std::uint64_t version = 1; version <= batches; ++version) {
                store.put(key, {1});
            }
            // missing 覆盖接近追平、少量落后和整个历史窗口重放.
            for (const auto missing : {std::uint64_t{1}, std::uint64_t{16}, batches}) {
                // iterations 按结果大小缩放, 避免大窗口复制让实验变成长时负载.
                const auto iterations = std::clamp<std::size_t>(100000 / static_cast<std::size_t>(missing), 50, 10000);
                // name 标明三个维度, 便于不同实现按同名场景比较.
                const auto name = "delta/b" + std::to_string(batches) + "/k" + std::to_string(key_size) + "/d" + std::to_string(missing);
                measure(name, iterations, [&] {
                    // result 同时消费版本和记录数, 析构发生在计时范围内.
                    const auto result = store.extract_since(batches - missing);
                    return result.current_version + result.deltas.size();
                });
            }
        }
    }
    // writes 保持很小的当前 Map, 对比覆盖、删除再创建及伴随的历史淘汰成本.
    SyncStore writes;
    measure("write/overwrite", 10000, [&] {
        writes.put("key", {1});
        return writes.global_version();
    });
    measure("write/remove-recreate", 10000, [&] {
        writes.remove("key");
        writes.put("key", {1});
        return writes.global_version();
    });
    // deleted 分别覆盖全部存活与大部分已删除, 检查精确计数对两类快照的影响.
    for (const unsigned deleted : {0U, 900U}) {
        // snapshots 保留足够长的历史, 使空节点在七轮测量内始终保留.
        SyncStore snapshots(10000);
        // index 在准备阶段创建 1000 个 Key, 然后删除指定前缀.
        for (unsigned index = 0; index < 1000; ++index) {
            snapshots.put(std::to_string(index), {1});
        }
        for (unsigned index = 0; index < deleted; ++index) {
            snapshots.remove(std::to_string(index));
        }
        measure("snapshot/deleted" + std::to_string(deleted), 300, [&] {
            snapshots.put("live", {1});
            return snapshots.get_snapshot()->data.size();
        });
    }
}
