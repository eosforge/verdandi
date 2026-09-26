#pragma once
#include "progress.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace astra {
// Downstream/Readout 共用的无分配调度索引. 所有方法由拥有者的同一 mutex 串行保护.
// Stream 的拥有索引必须活过排队/在途状态; pop 返回强引用, settle 必须先于最终移除拥有索引.
template <class Stream>
class Watch {
public:
    // 每条流内嵌的在途链, 与既有 queued/next 就绪链独立. 空链不占用堆节点.
    struct Link {
        Stream* previous{}; // 在途前驱, 空表示链头或未加入; 后者由 Watch::active_ 区分.
        Stream* next{};     // 在途后继, 空表示末尾或未加入.
    };

    // limits 借用领域的既有配额, 不另存一份默认值; what 为构造失败时的固定诊断.
    template <class Limits>
    static void validate(const Limits& limits, const char* what) {
        if (limits.streams == 0 || limits.streams > 65536 || limits.bytes == 0 || limits.pending == 0 || limits.pending > limits.bytes || limits.timeout.count() <= 0) {
            throw std::invalid_argument(what);
        }
    }

    // used/cost/hard 均为字节, 已超限时先拒绝, 避免 hard-used 回绕. 本方法不修改计费状态.
    static bool exceeds(std::size_t used, std::size_t cost, std::size_t hard) noexcept {
        return used > hard || cost > hard - used;
    }

    // stream 由拥有索引保活; 新入队返回 true, 已排队返回 false, 调用者可合并重复唤醒.
    // 不分配, 不取得流的 I/O 锁; 回调状态仍须先在所属锁内发布, 不能因合并而跳过.
    bool enqueue(Stream& stream) noexcept {

        if (stream.queued) {
            return false;
        }

        stream.queued = true;
        if (tail_) {
            tail_->next = &stream;
        } else {
            head_ = &stream;
        }
        tail_ = &stream;
        return true;
    }

    // 空队列返回空引用, 成功弹出前先保活. 调用者释放索引锁后仍可安全进入该流的 I/O 锁.
    std::shared_ptr<Stream> pop() noexcept {

        if (!head_) {
            return {};
        }

        auto held = head_->shared_from_this(); // 此时仍由拥有索引持有, 不创建新控制块.
        head_ = head_->next;
        if (!head_) {
            tail_ = nullptr;
        }
        held->queued = false;
        held->next = nullptr;
        return held;
    }

    // 在同一次索引锁内转交至多一个范围通知再弹出就绪流. 保留 FIFO 和 Scope 轮转,
    // 同流已就绪时合并通知, 空就绪队列也能当轮处理进度, 不等待下一次 pump.
    std::shared_ptr<Stream> pop(Progress<Stream>& progress) noexcept {

        if (auto* stream = progress.take()) { // 裸指针始终由范围索引保活, 不跨解锁借用.
            enqueue(*stream);
        }

        return pop();
    }

    // 只判断就绪队列; 在途流等待回调或低频超时扫描, 不引起空转.
    bool idle() const noexcept {
        return head_ == nullptr;
    }

    // 在 StartWrite 前加入在途链, 重复调用不重复入链; 只修改嵌入指针, 不分配或抛异常.
    void write(Stream& stream) noexcept {
        if (&stream != active_ && !stream.flight.previous) {
            stream.flight.next = active_;
            if (active_) {
                active_->flight.previous = &stream;
            }
            active_ = &stream;
        }
    }

    // 成功消费 WriteDone 或最终回收时摘除, 未入链也安全. 不影响尚待处理的就绪通知.
    void settle(Stream& stream) noexcept {

        if (&stream != active_ && !stream.flight.previous) {
            return;
        }

        if (stream.flight.previous) {
            stream.flight.previous->flight.next = stream.flight.next;
        } else {
            active_ = stream.flight.next;
        }
        if (stream.flight.next) {
            stream.flight.next->flight.previous = stream.flight.previous;
        }
        stream.flight = {};
    }

    // now/deadline 使用 steady_clock. 每秒只扫描在途链, busy 为回调发布的原子状态; wake 不得重入索引锁.
    template <class Wake>
    void sweep(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point& deadline, Wake&& wake) {

        if (now < deadline) {
            return;
        }

        // stream 借用在途节点, enqueue 仅改变另一条就绪链, 不破坏本次遍历.
        for (auto* stream = active_; stream; stream = stream->flight.next) {
            if (stream->busy.load(std::memory_order_acquire)) {
                enqueue(*stream);
            }
        }
        deadline = now + std::chrono::seconds(1);
        if (!idle()) {
            wake();
        }
    }

private:
    Stream* head_{};   // 就绪链头, 初始为空; 节点所有权始终由调用方索引承担.
    Stream* tail_{};   // 就绪链尾, 与 head_ 同时为空; 追加通知为 O(1).
    Stream* active_{}; // 在途链头, 初始为空; 任意节点摘除为 O(1).
};
} // namespace astra
