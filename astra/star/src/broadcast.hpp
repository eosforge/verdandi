#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace astra {
// 同范围/目标/游标区间的一份冻结分页. 只由已有控制线程构造/推进, 不添加锁或后台任务.
template <typename Edition>
class Broadcast {
public:
    // gRPC 只读借用 message; after 保存编码此页后的私有游标, 复制只共享不可变批次.
    struct Page {
        typename Edition::Reply message; // 成功构造后不修改, 保持到所有对应 OnWriteDone.
        Edition after;                   // 接收该页的每条流独立采纳此位置, 不采纳别人的网络完成进度.
        std::size_t bytes{};             // 完整 Protobuf 线长, 编码时只计算一次, 每流仍保守计费.
    };

    // since 空为 reset, 非空为确定的连续 apply 起点; target 为空时表示完整 Scope.
    Broadcast(Edition edition, std::optional<std::uint64_t> since, std::string target) : seed_(std::move(edition)), since_(since), target_(std::move(target)) {}

    // 只匹配同一批次, 绝不把不同游标的 Data-only 或删除事件混用.
    bool matches(std::optional<std::uint64_t> since, std::uint64_t version, std::string_view target) const noexcept {
        return since_ == since && seed_.version() == version && target_ == target;
    }

    // 缓存可作为较新后缀的完整前缀, 但必须推进当前位置, 防止重复发送空 apply 忙循环.
    bool prefix(std::uint64_t since, std::uint64_t latest, std::string_view target) const noexcept {
        return since_ == since && target_ == target && since < seed_.version() && seed_.version() <= latest;
    }

    // 冻结末游标, 与每条流的最后网络确认位置无关.
    std::uint64_t version() const noexcept {
        return seed_.version();
    }

    // 显式取得独立分页游标, 共享根/后缀容器, 不复制内容或完整事件向量.
    Edition begin() const {
        return seed_;
    }

    // 每个页号独立缓存, 快慢订阅交错不覆盖彼此; 仅保存弱引用, 无流使用时立即释放正文.
    std::shared_ptr<const Page> next(Edition& cursor, std::size_t offset, std::string_view instance) {

        if (cursor.complete())
            throw std::logic_error("Publication cursor already completed");
        if (offset > pages_.size())
            throw std::logic_error("Publication page skipped"); // 内部游标只能逐页前进, 不允许异常页号触发稀疏巨量分配.
        if (offset == pages_.size())
            pages_.emplace_back(); // 先准备弱槽, 分配失败不会提前消费当前游标; 最多一槽对应一个实际请求页.
        if (auto cached = pages_[offset].lock()) {
            cursor = cached->after;
            return cached;
        }

        auto message = cursor.next(instance); // 异常只终止当前流, 其他流的游标和已发布消息不变.
        const auto bytes = message.ByteSizeLong();
        auto page = std::make_shared<const Page>(std::move(message), cursor, bytes);
        pages_[offset] = page;
        return page;
    }

private:
    const Edition seed_;                           // 只保存初始分页位置, 全部派生游标共享这份不可变记录来源.
    const std::optional<std::uint64_t> since_;     // 区分 reset 与从零位置恢复, 不以零作哨兵.
    const std::string target_;                     // 固定目标, 一份批次内不更改过滤条件.
    std::vector<std::weak_ptr<const Page>> pages_; // 页号索引, 元数据随冻结批次页数增长并随批次释放; 不强持有已完成页正文.
};
} // namespace astra
