#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace comet::detail {
// Client 私有采样资源, 两个工作线程和 64 个候选槽, 不占 gRPC reaction 或逐 Beacon 建线程.
class Sampling {
public:
    using Work = std::move_only_function<void()>; // 内部候选拥有 Beacon, 不公开执行器接口.

    static std::shared_ptr<Sampling> open() {

        auto result = std::make_shared<Sampling>(); // 工作者只拥有队列, 不拥有线程管理器或空闲 Client.
        for (auto& thread : result->threads_) {
            ++result->queue_->workers;
            try {
                thread = std::thread([queue = result->queue_] { run(*queue); });
            } catch (...) {
                --result->queue_->workers;
                throw; // result 析构关闭并等待此前已创建的工作者.
            }
        }
        return result;
    }

    ~Sampling() {
        close();
        wait();
    } // Core 只有在全部工作者已释放任务后才完成, 不在采样线程自等.

    void wait() {
        const std::lock_guard lock(join_); // 多个应用等待者不能同时 join 同一线程.
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    } // Core 只有在全部工作者已释放任务后才完成, 不在采样线程自等.

    bool submit(Work work) {
        const std::lock_guard lock(queue_->mutex);
        if (queue_->closed || queue_->count == queue_->items.size()) {
            return false;
        }
        queue_->items[(queue_->head + queue_->count) % queue_->items.size()] = std::move(work);
        ++queue_->count;
        queue_->ready.notify_one();
        return true;
    } // 满额不排入另一队列, Beacon 按有限期限重试采样.

    void close() noexcept {
        const std::lock_guard lock(queue_->mutex);
        queue_->closed = true;
        queue_->ready.notify_all();
    } // 已接纳候选仍执行清理路径, 不丢失 Beacon 的 sampling 标志.

    bool finished() const noexcept {
        return queue_->workers.load(std::memory_order_acquire) == 0;
    } // 最后任务和其拥有的回调已经释放.
private:
    struct Queue {
        std::mutex mutex;              // 不跨用户采样或同步 RPC.
        std::condition_variable ready; // 空队列等待, 不轮询.
        std::array<Work, 64> items;    // 固定槽位, 不扩容.
        std::size_t head{};            // 当前队首.
        std::size_t count{};           // 已接纳候选数量.
        bool closed{};                 // 单向关闭门.
        std::atomic_uint workers{};    // 实际尚未结束的工作循环, 非取消计数.
    };

    static void run(Queue& queue) {

        for (;;) {
            Work work; // 每轮末尾先释放任务及其 Beacon/Core 所有权.
            {
                std::unique_lock lock(queue.mutex);
                queue.ready.wait(lock, [&] { return queue.closed || queue.count; });
                if (!queue.count) {
                    break;
                }
                work = std::move(queue.items[queue.head]);
                queue.head = (queue.head + 1) % queue.items.size();
                --queue.count;
            }
            work(); // Beacon 捕获业务异常, 始终归还自己的采样责任.
        }
        queue.workers.fetch_sub(1, std::memory_order_release);
    }

    std::shared_ptr<Queue> queue_ = std::make_shared<Queue>(); // 工作者没有指回管理器的引用环.
    std::mutex join_;                                          // 只在退出边界保护线程回收.
    std::array<std::thread, 2> threads_;                       // 完成后仍 join, 卸载库前不遗留分离线程.
};
} // namespace comet::detail
