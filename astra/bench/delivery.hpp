#pragma once
#include "measure.hpp"
#include <atomic>
#include <comet/observer.hpp>
#include <comet/subscriber.hpp>
#include <condition_variable>
#include <cstring>
#include <mutex>

// 基准的推流确认器. 每个写者一个等待槽, 回调只点查热点 Key, 不让 N 个写者轮询 SDK 的同一锁.
class Delivery {
    struct Slot {
        std::mutex gate;                 // 条件检查与通知边界, 不覆盖 SDK 的 View::find.
        std::condition_variable changed; // 只有对应写者等待, 不唤醒其他 Key 的工作线程.
        std::vector<std::uint64_t> seen; // 各订阅已安装序号, 由 gate 保护, 初始零.
    };

public:
    // keys 前 writers 项是热点, 其余仅用于初始视图记录数校验; 名称在构造时独立拥有.
    Delivery(std::vector<std::string> keys, std::size_t writers, std::size_t watchers, std::size_t bytes, std::size_t attr) : keys_(std::move(keys)), writers_(writers), bytes_(bytes), attr_(attr), slots_(std::make_unique<Slot[]>(writers)) {
        for (std::size_t index = 0; index < writers_; ++index) {
            slots_[index].seen.resize(watchers); // 固定大小, 收包热路径不扩容.
        }
    }

    // 收到完整 Catalog View 后只验证热点, 不为每条变化扫描全部背景记录.
    void receive(std::size_t watcher, const comet::Subscriber::View& view) {

        // watcher 为固定订阅序号, 未完整就绪的 View 不能确认任何业务操作.
        if (view.state() != comet::Subscriber::State::ready || view.size() != keys_.size()) {
            return;
        }

        for (std::size_t index = 0; index < writers_; ++index) {
            const auto record = view.find(keys_[index]); // 借用本次拥有式 View, 不反向轮询 Subscriber.
            if (!record || record->value->size() != bytes_ || sequence(*record->value) != record->version) {
                fail();
                return;
            }
            accept(index, watcher, record->version);
        }
    }

    // Ephemeris 不暴露 Data order, 使用本次载荷序号关联更新, 同时检查 Attr 尺寸保持不变.
    void receive(std::size_t watcher, const comet::Observer::View& view) {

        // watcher 为固定订阅序号, 不通过部分注册视图提前释放等待者.
        if (view.state() != comet::Observer::State::ready || view.size() != keys_.size()) {
            return;
        }

        for (std::size_t index = 0; index < writers_; ++index) {
            const auto record = view.find(keys_[index]); // UUID 固定至本场景结束, 意外重注册不能算原请求成功.
            if (!record || record->data->size() != bytes_ || record->attr->size() != attr_) {
                fail();
                return;
            }
            accept(index, watcher, sequence(*record->data));
        }
    }

    // 等待全部订阅确认恰好本次序号, 超时/正文错误使整份样本无效. 应用线程调用, 不在 SDK 回调阻塞.
    void wait(std::size_t writer, std::uint64_t version) {
        auto& slot = slots_[writer]; // 一个写者独占一个等待槽, 多个 Watch 回调可并发提交不同 seen 项.
        std::unique_lock lock(slot.gate);
        Measure::require(slot.changed.wait_for(lock, std::chrono::seconds(5), [&] { return failed_.load() || std::ranges::all_of(slot.seen, [&](auto observed) { return observed == version; }); }));
        Measure::require(!failed_.load());
    }

private:
    // 正文尺寸已由调用方校验, 序号只是同进程探针的关联标识, 不是线上新增协议字段.
    static std::uint64_t sequence(const std::vector<std::uint8_t>& bytes) noexcept {
        std::uint64_t result{};
        std::memcpy(&result, bytes.data(), sizeof(result));
        return result;
    }

    // 相同版本的其他 Key 推送不反复唤醒本写者. 条件变量与 seen 使用同一锁, 不丢通知.
    void accept(std::size_t writer, std::size_t watcher, std::uint64_t version) {
        auto& slot = slots_[writer];
        const std::lock_guard lock(slot.gate);
        if (slot.seen[watcher] != version) {
            slot.seen[watcher] = version;
            slot.changed.notify_one();
        }
    }

    // 在所有等待槽的同步边界内通知失败, 包括刚完成条件检查尚未睡眠的写者.
    void fail() {
        failed_.store(true);
        for (std::size_t index = 0; index < writers_; ++index) {
            const std::lock_guard lock(slots_[index].gate);
            slots_[index].changed.notify_one();
        }
    }

    const std::vector<std::string> keys_; // 固定名称集合, 不借用调用方的可变容器.
    const std::size_t writers_;           // 1..32 个独立热点及等待槽.
    const std::size_t bytes_;             // 正文字节, 至少 16, 足够存放八字节序号.
    const std::size_t attr_;              // Ephemeris 固定属性尺寸, Catalog 不使用.
    const std::unique_ptr<Slot[]> slots_; // 槽位/互斥量地址保持稳定, 回调由共享 Delivery 延长寿命.
    std::atomic_bool failed_{};           // 正文校验失败后粘滞为 true, 不将失败当作丢失样本跳过.
};
