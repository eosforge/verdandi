// 功能: 用固定分层槽位管理到期节点, 调度与取消不分配内存, 节点数据由调用方拥有.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace astra {

// Levels 为层数, Bits 为每层索引位数. 单线程使用, 或由外部同一把锁串行保护全部操作及节点销毁.
// 每次 tick 推进一个逻辑时间单位; 同拍回调无顺序承诺. 不读取时钟, 不自动跳过空槽或限制单拍回调数.
template <std::size_t Levels = 3, std::size_t Bits = 8>
    requires(Levels > 0 && Bits > 0 && Bits <= 8 && Levels <= 64 / Bits)
class Wheel {
    // 每层至多 256 槽, 总索引位数至多 64. 这些约束同时排除无效移位和意外的巨型槽数组.
    static constexpr std::size_t slots = std::size_t{1} << Bits;
    // 槽索引掩码, 代替取模; slots 始终为二的幂.
    static constexpr std::uint64_t mask = slots - 1;

public:
    // 最大相对延迟, 不包含额外溢出层. 总索引位数为 1..64, 右移量为 0..63, 不会发生非法移位.
    static constexpr std::uint64_t max_delay = std::numeric_limits<std::uint64_t>::max() >> (64 - Levels * Bits);

    // 可嵌入业务对象的定时器钩子, 每次最多属于一个 Wheel. 挂链期间地址稳定, 不拥有业务数据.
    struct Node {
        // 创建未调度节点. 不分配资源, 到期值只在调度成功后有意义.
        Node() noexcept = default;
        // 无论节点还是 Wheel 先销毁, 都先解除连接; 回调也可以释放当前节点或尚未触发的节点.
        ~Node() {
            Wheel::cancel(*this);
        }
        // 链接包含其他对象的地址, 不能复制或搬移, 业务对象可通过稳定地址容器持有节点.
        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;
        Node(Node&&) = delete;
        Node& operator=(Node&&) = delete;

        // 是否仍挂在槽位或待回调队列中. 仅借用状态, 与其他操作一样要求串行访问.
        [[nodiscard]] bool scheduled() const noexcept {
            return pprev_ != nullptr;
        }

    private:
        friend class Wheel;
        // 同桶后继, 节点自身及调用方均不拥有后继的生命周期.
        Node* next_{};
        // 指向引用本节点的指针: 槽头, ready_ 或前驱的 next_. 用于 O(1) 摘链.
        Node** pprev_{};
        // 相对于当前 Wheel 逻辑时钟的绝对到期拍, 无符号加减允许跨 uint64_t 回绕.
        std::uint64_t expire_ticks_{};
    };

    // 创建空轮, initial_tick 指定逻辑时钟起点, 不要求与系统时钟或其他 Wheel 一致.
    explicit Wheel(std::uint64_t initial_tick = 0) noexcept : current_tick_(initial_tick) {}
    // 清除所有钩子但不调用回调或销毁节点. 禁止在本轮 tick 的回调中销毁本 Wheel.
    ~Wheel() {
        unlink_all(ready_);
        for (auto& level : wheels_) {
            for (auto& head : level) {
                unlink_all(head);
            }
        }
    }
    // 槽头及 ready_ 的地址被节点引用, 因而即使空轮也统一禁止复制与移动.
    Wheel(const Wheel&) = delete;
    Wheel& operator=(const Wheel&) = delete;
    Wheel(Wheel&&) = delete;
    Wheel& operator=(Wheel&&) = delete;

    // 借用 node 并调度相对延迟; 0 等同于 1, 只在未来逻辑拍回调, 从不在此处执行回调.
    // 超过 max_delay 返回 false 且原调度不变. 成功会取消原调度, 允许在串行保护下转移到另一个 Wheel.
    [[nodiscard]] bool schedule(Node& node, std::uint64_t delay_ticks) noexcept {
        if (delay_ticks > max_delay) {
            return false;
        }
        cancel(node);
        node.expire_ticks_ = current_tick_ + std::max(delay_ticks, std::uint64_t{1});
        insert(node);
        return true;
    }

    // 取消任何 Wheel 上的 node, 未调度时为空操作. 无分配, 不调用用户代码, 不改变到期值.
    static void cancel(Node& node) noexcept {
        if (node.pprev_ == nullptr) {
            return;
        }
        *node.pprev_ = node.next_;
        if (node.next_ != nullptr) {
            node.next_->pprev_ = node.pprev_;
        }
        node.pprev_ = nullptr;
        node.next_ = nullptr;
    }

    // 返回已推进的逻辑拍数, uint64_t 自然回绕. 回调看到的是本次到期拍, 不是下一拍.
    [[nodiscard]] std::uint64_t now() const noexcept {
        return current_tick_;
    }

    // 调用可接收 Node* 的回调, 不复制回调或通过 std::function 擦除类型.
    // 调用前摘下当前节点, 回调可取消/改期/销毁任意节点, 但不能销毁本 Wheel.
    // 异常原样传播: 当前回调已消费, 剩余节点仍可取消, 下次 tick 先处理它们再推进一拍.
    // 对同一 Wheel 递归 tick 抛 logic_error 且不推进时钟; 其他 Wheel 的 tick 不受影响.
    template <typename F>
        requires std::invocable<F&, Node*>
    void tick(F&& on_expire) {
        // guard 在正常返回及回调异常时都恢复重入标记; 此标记不是线程同步原语.
        TickGuard guard(ticking_);
        drain(on_expire);
        ++current_tick_;

        // 只有低位全部归零时才级联. 从最高到最低处理, 新落入低层的节点可在本拍继续下沉.
        // level 是本拍必须推进的最高层; countr_zero(0) 返回 64, 正确覆盖时钟回绕.
        for (auto level = std::min(Levels - 1, static_cast<std::size_t>(std::countr_zero(current_tick_)) / Bits); level > 0; --level) {
            // cascade_list 接管整个高层桶. 期间无回调且不抛异常, 无需逐节点维护旧链表的反向链接.
            // 摘桶为 O(1), 后续重新分层仍需遍历桶内全部节点.
            auto* cascade_list = std::exchange(wheels_[level][(current_tick_ >> (level * Bits)) & mask], nullptr);
            while (cascade_list != nullptr) {
                // node 在保存后继后重新归类; insert 会覆盖其链接, 不能在插入后再读取旧 next_.
                auto* node = cascade_list;
                cascade_list = node->next_;
                insert(*node);
            }
        }

        // 将到期桶的头指针归属改为成员 ready_, 即使抛异常也不会留下指向栈变量的钩子.
        ready_ = std::exchange(wheels_[0][current_tick_ & mask], nullptr);
        if (ready_ != nullptr) {
            ready_->pprev_ = &ready_;
        }
        drain(on_expire);
    }

private:
    // 栈上恢复标记, 不分配资源. 只负责阻止同一 Wheel 的递归推进.
    struct TickGuard {
        // flag 借用 Wheel 的重入标记, guard 的生命周期严格包含于 tick.
        bool& flag;
        // 已进入时抛出且保留原 true 状态, 由外层 guard 负责恢复.
        explicit TickGuard(bool& value) : flag(value) {
            if (std::exchange(flag, true)) {
                throw std::logic_error("Wheel::tick cannot reenter the same wheel");
            }
        }
        // 异常展开也恢复标记, 不接触节点或回调.
        ~TickGuard() {
            flag = false;
        }
        // 防止两个 guard 重复恢复同一个借用标记.
        TickGuard(const TickGuard&) = delete;
        TickGuard& operator=(const TickGuard&) = delete;
    };

    // 将已摘链节点插入匹配槽, 仅用于通过 schedule 范围检查的节点和级联节点.
    void insert(Node& node) noexcept {
        // remaining 是模 2^64 的剩余拍数, 在 max_delay 范围内; 0 在本拍最低层到期.
        const auto remaining = node.expire_ticks_ - current_tick_;
        // level 由最高有效位确定. 置位最低位使零值归入最低层, 不改变非零值的最高有效位.
        const auto level = static_cast<std::size_t>(std::bit_width(remaining | 1) - 1) / Bits;
        // head 是最终拥有节点的稳定槽头, 节点地址及槽头地址在挂链期间均不变化.
        auto& head = wheels_[level][(node.expire_ticks_ >> (level * Bits)) & mask];
        node.next_ = head;
        node.pprev_ = &head;
        if (head != nullptr) {
            head->pprev_ = &node.next_;
        }
        head = &node;
    }

    // 从成员队列每次只摘一个节点. 绝不跨回调保存 next, 因为回调可能取消或释放它.
    template <typename F> void drain(F& on_expire) {
        while (ready_ != nullptr) {
            // node 的借用到调用回调为止, 调用后不再访问, 支持当前节点自销毁及重新调度.
            // 已知节点为队首, 直接弹出并修正后继链接, 在用户回调前恢复其余队列的完整链接.
            auto* node = ready_;
            ready_ = node->next_;
            if (ready_ != nullptr) {
                ready_->pprev_ = &ready_;
            }
            node->pprev_ = nullptr;
            node->next_ = nullptr;
            std::invoke(on_expire, node);
        }
    }

    // 摘下 head 的全部节点, 保留它们的对象寿命. 用于 Wheel 析构, 不分配也不抛异常.
    static void unlink_all(Node*& head) noexcept {
        // node 接管整条链表. 清理期间无回调, 不必维护中间状态的反向链接, 只需清空各节点钩子.
        auto* node = std::exchange(head, nullptr);
        while (node != nullptr) {
            // next 在清空钩子前保存, 只用于本次无回调的析构遍历.
            auto* next = node->next_;
            node->pprev_ = nullptr;
            node->next_ = nullptr;
            node = next;
        }
    }

    // 本 Wheel 的逻辑时钟, 仅 tick 在回调队列处理完成后推进.
    std::uint64_t current_tick_{};
    // 到期但尚未调用的节点, 所有回调和异常路径共享一个稳定的链表头.
    Node* ready_{};
    // 防止回调递归 tick; 外部仍须保证串行访问, 不为每个节点额外保存 owner 指针.
    bool ticking_{};
    // 固定槽头数组, 零初始化表示空轮. Wheel 不拥有节点, 析构仅摘链.
    std::array<std::array<Node*, slots>, Levels> wheels_{};
};

} // namespace astra
