#include "workload.hpp"
#include <array>

namespace {
// 内存夹具只验证负载执行器的路由、线程交接及失败传播, 不代替实际服务性能样本.
class Adapter {
public:
    // 固定配置及八条初始数据, 每个记录有独立互斥量.
    explicit Adapter(const Workload::Options& options) : options_(options) {
        for (std::size_t record = 0; record < options.records; ++record) {
            values_[record] = Workload::content(options.bytes, record, 1);
        }
    }

    // 模拟完整提交, 多写者只更新自己的记录, 锁保护采样线程并发读取.
    void write(std::size_t record, std::uint64_t, std::vector<std::uint8_t> value) {
        const std::lock_guard lock(gates_[record]);
        values_[record] = std::move(value);
    }

    // watcher 按配置映射到组, 同一组的两个订阅都返回完整数据.
    void scan(std::size_t watcher, auto&& receive) {
        for (std::size_t record = watcher / options_.fanout; record < options_.records; record += options_.groups) {
            const std::lock_guard lock(gates_[record]);
            receive(std::span<const std::uint8_t>(values_[record]));
        }
    }

    // 无后台 SDK 任务, 成功条件由共用负载的最终完整视图检查负责.
    void verify() {}

private:
    const Workload::Options& options_;                // 调用方拥有, 夹具不改变配置.
    std::array<std::mutex, 8> gates_;                 // 各记录独立同步, 不用全表锁掩盖并发问题.
    std::array<std::vector<std::uint8_t>, 8> values_; // 初始化后由所属写者更新.
};

// 注入错误正文, 检查后台采样异常能传到调用方且不会留下未 join 线程.
class Broken : public Adapter {
public:
    using Adapter::Adapter; // 复用合法初始化, 只改变观察输入.

    void scan(std::size_t, auto&& receive) {
        receive(std::span<const std::uint8_t>{});
    }
};

// 已经观察到的记录随后消失时, 最终重新读取必须失败, 不能沿用采样水线宣告收敛.
class Missing : public Adapter {
public:
    using Adapter::Adapter; // 常规初始化和工作线程写入仍然有效.

    void scan(std::size_t watcher, auto&& receive) {
        if (std::this_thread::get_id() != owner_) {
            Adapter::scan(watcher, std::forward<decltype(receive)>(receive));
        }
    }

private:
    const std::thread::id owner_ = std::this_thread::get_id(); // 主线程最终核验时模拟空视图, 采样线程仍正常看到数据.
};

// 要求 callable 失败, 避免测试把未触发的边界误报为通过.
void rejected(auto&& action) {
    bool failed{}; // 只有受测操作抛出异常才置位.
    try {
        action();
    } catch (const std::invalid_argument&) {
        failed = true;
    } catch (const std::runtime_error&) {
        failed = true;
    }
    Workload::check(failed, "Expected rejection was not observed");
}
} // namespace

// 两种测量模式、跨组/多订阅/多写者以及无效数字和正文失败路径.
int main() {
    try {
        for (const auto text : {"", "-1", "+1", "4294967296", "1x", " 2"}) {
            rejected([&] { static_cast<void>(Workload::number(text)); });
        }
        Workload::check(Workload::number("0") == 0 && Workload::number("4294967295") == 4294967295U, "Unsigned boundary rejected");
        rejected([] { static_cast<void>(Measure::number("4294967301", 2, 30)); });                   // 防止先截断为 5 再通过秒数校验.
        rejected([] { static_cast<void>(Measure::number("18446744073709551616", 0, UINT64_MAX)); }); // 底层 uint64 本身溢出.
        Workload::Options options{"memory", true, true, 8, 2, 2, 2, 4, 32, 32, 2, 30000, 100, 1000}; // 两组各两个订阅, 四写者均匀操作八条记录.
        Workload::run<Adapter>(options);
        options.visible = false;
        Workload::run<Adapter>(options);
        rejected([&] { Workload::run<Broken>(options); });
        rejected([&] { Workload::run<Missing>(options); });
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
