#include <astra/profile.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {
// 相同工作负载编译成开/关两个独立二进制, value 是纯本地校验和, 不依赖诊断记录.
[[gnu::noinline]] std::uint64_t work(std::uint64_t value) {

    ASTRA_PROFILE_SCOPE("profile.test.work");
    {
        ASTRA_PROFILE_SCOPE("profile.test.child");
        // 固定少量整数运算, 原子信号栅栏只阻止编译器将整段循环当作未观察结果消除.
        for (unsigned step = 0; step != 16; ++step) {
            value = value * 6364136223846793005ULL + 1;
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }

    ASTRA_PROFILE_BEGIN(phase, "profile.test.phase");
    ASTRA_PROFILE_COUNT("profile.test.bytes", 17);
    ASTRA_PROFILE_END(phase);
    ASTRA_PROFILE_END(phase); // 提前结束幂等, 析构不再产生记录.
    return value;
}

// 异常展开必须恢复父栈, 后续 work 仍是同一个父跨度的直接子调用.
void unwind() {

    ASTRA_PROFILE_SCOPE("profile.test.unwind");
    try {
        ASTRA_PROFILE_SCOPE("profile.test.throw");
        throw 7;
    } catch (int) {
        static_cast<void>(work(7));
    }
}
} // namespace

// normal 用于功能检查, overflow 验证固定文件容量, bench 供单独授权的探针开销对照.
int main(int argc, char** argv) {

    if (argc != 2)
        return 2;
    const std::string_view mode(argv[1]); // 借用命令行, 不在热路径复制.
    if (mode != "normal" && mode != "overflow" && mode != "bench" && mode != "phase")
        return 2;

    if (mode == "phase") {
        // 模拟 Runtime 工作/等待的固定交替. 周期抽样会永久漏掉 first, 随机抽样须覆盖两类.
        for (unsigned round = 0; round != 4096; ++round) {
            {
                ASTRA_PROFILE_SCOPE("profile.test.first");
            }
            {
                ASTRA_PROFILE_SCOPE("profile.test.second");
            }
        }
        return 0;
    }

    if (mode == "normal") {
        // 关闭构建连参数也不得求值, 更不允许引用不存在的诊断变量.
        int evaluated{};
        {
            ASTRA_PROFILE_SCOPE("profile.test.arguments");
            ASTRA_PROFILE_COUNT("profile.test.argument", ++evaluated);
        }
#if !defined(ASTRA_PROFILE) || !ASTRA_PROFILE
        ASTRA_PROFILE_SCOPE(undefined_name);
        ASTRA_PROFILE_BEGIN(undefined_token, undefined_name);
        ASTRA_PROFILE_END(undefined_token);
        if (evaluated != 0)
            return 3;
#else
        if (evaluated != 1)
            return 3;
#endif
        // 线程 join 是时间戳从 mark 到 consume 的同步边界; 不把父跨度移到另一个线程.
        ASTRA_PROFILE_STAMP(stamp);
        {
            ASTRA_PROFILE_SCOPE("profile.test.mark");
            ASTRA_PROFILE_MARK(stamp);
        }
        std::jthread consumer([&] { ASTRA_PROFILE_CONSUME(stamp, "profile.test.delay"); });
        consumer.join();
        ASTRA_PROFILE_CONSUME(stamp, "profile.test.delay"); // 已消费的时间戳不能重复记账.

        // 每线程独立采样和分块. 同一映射并发写入的槽位不得重叠.
        std::vector<std::jthread> workers;
        for (unsigned thread = 0; thread != 4; ++thread)
            workers.emplace_back([] { for (unsigned round = 0; round != 20; ++round) unwind(); });
        return 0; // workers 析构先 join, 正常进程退出后才允许解析 mmap.
    }

    // 初始化不计入循环, 输出仅是微基准证据, 不能替代真实 3 Star 吞吐和尾延迟对照.
    std::uint64_t value = work(1);
    const auto count = mode == "bench" ? 1000000U : 30000U;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned round = 0; round != count; ++round)
        value = work(value);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    std::cout << "{\"calls\":" << count << ",\"elapsed_ns\":" << elapsed << ",\"checksum\":" << value << "}\n";
    return 0;
}
