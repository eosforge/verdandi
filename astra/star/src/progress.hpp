#pragma once
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>

namespace astra {
// Scope 进度与有界通知轮转. 调用方始终持事件索引锁, 本类不加锁、不分配通知节点、不访问 Stream 的 I/O 状态.
template <typename Stream>
class Progress {
public:
    Progress() = default;                          // 队列初始为空, 不拥有外部路由分组.
    Progress(const Progress&) = delete;            // 链接只属于一个调度器, 不能复制活动队列.
    Progress& operator=(const Progress&) = delete; // 禁止通过赋值覆盖正在使用的链接.

    // 嵌入实际路由分组, 与最后一条流一起回收; 地址必须稳定, 不允许复制已入队的链接.
    struct Group {
        std::map<Stream*, std::shared_ptr<Stream>> streams; // RPC 的拥有索引, erase 必须经过 Progress 保持扫描位置有效.
        std::optional<std::uint64_t> first;                 // 分组存在期间的首次通知, 空表示尚无提交, 允许版本零.
        std::uint64_t version{};                            // 已收集的范围末版本, 不由某条流的发送完成推进.
        std::optional<std::uint64_t> reset;                 // 最新全量替换或断档边界, 较旧基线不得声称连续 apply.

        Group() = default;                       // 空分组尚未排入通知轮转.
        Group(const Group&) = delete;            // 侵入式链接与迭代器不能复制到另一个分组.
        Group& operator=(const Group&) = delete; // 路由节点只允许原位构造和销毁.

        // baseline 是已冻结或已确认位置; 首次观察断档和新 reset 都要求重建基线.
        bool covers(std::uint64_t baseline) const noexcept {
            return (!reset || *reset <= baseline) && (!first || baseline == UINT64_MAX || *first <= baseline + 1);
        }

    private:
        friend class Progress;
        std::optional<typename decltype(streams)::iterator> cursor; // 有值表示已入队; 下一条尚未调度的流, 由 erase 同步修正.
        std::uint64_t through{};                                    // 本次遍历开始时的范围版本, 遍历期间新提交另启一轮, 不丢失唤醒.
        Group* previous{};                                          // 通知队列前驱, 空表示队首或未入队.
        Group* next{};                                              // 通知队列后继, 空表示队尾或未入队.
    };

    // 收集提交位置并安排一次范围通知. replacement 表示完整替换; 连续性在范围内检查, 与目标过滤无关.
    void publish(Group& group, std::uint64_t version, bool replacement) noexcept {

        if (replacement || (group.first && (group.version == UINT64_MAX || version != group.version + 1))) {
            group.reset = version;
        }
        if (!group.first) {
            group.first = version;
        }
        group.version = version;

        // 一个 Scope 最多一个队列位置, 连续写入只更新标量, 不重复分配或扫描订阅.
        if (!group.cursor && !group.streams.empty()) {
            group.cursor = group.streams.begin();
            group.through = version;
            append(group);
        }
    }

    // 取一个仍由 streams 拥有的流; 调用方在同一索引锁内入队, 不跨解锁借用裸指针.
    Stream* take() noexcept {

        if (!head_) {
            return nullptr;
        }
        auto& group = *head_;                  // 队首必有尚未访问的活动流.
        auto* stream = (*group.cursor)->first; // 只访问索引, 不读取 writing/done 等 I/O 字段.
        ++*group.cursor;
        detach(group);

        // 每取一条流把 Scope 放到队尾, 大范围与持续更新都不能饿死其他范围.
        if (*group.cursor == group.streams.end()) {
            finish(group);
        } else {
            append(group);
        }
        return stream;
    }

    // 从拥有索引移除 RPC, 包括挂入失败回滚; 调用方另行清理目标索引并持有必要的局部强引用.
    void erase(Group& group, Stream* stream) noexcept {

        const auto found = group.streams.find(stream); // 失败回滚可能还未加入拥有索引.
        if (found == group.streams.end()) {
            return;
        }
        if (group.cursor && *group.cursor == found) {
            ++*group.cursor;
        }
        group.streams.erase(found);

        if (group.cursor && *group.cursor == group.streams.end()) {
            detach(group);
            finish(group);
        }
    }

    // 只在持索引锁时读取, 有剩余工作即唤醒已有控制循环, 不等待后续业务更新.
    bool pending() const noexcept {
        return head_ != nullptr;
    }

private:
    // 一次遍历结束后检查并发到达的新版本; 空分组不再入队, 因而可立即销毁.
    void finish(Group& group) noexcept {
        if (!group.streams.empty() && group.through != group.version) {
            group.cursor = group.streams.begin();
            group.through = group.version;
            append(group);
        } else {
            group.cursor.reset();
        }
    }

    // 已脱链的非空分组追加到队尾, 不申请内存; cursor 已由调用者设好.
    void append(Group& group) noexcept {

        assert(group.cursor && *group.cursor != group.streams.end());
        group.previous = tail_;
        group.next = nullptr;

        if (tail_) {
            tail_->next = &group;
        } else {
            head_ = &group;
        }
        tail_ = &group;
    }

    // 常数时间移除队列链接, 不改变下一条流的位置.
    void detach(Group& group) noexcept {

        if (group.previous) {
            group.previous->next = group.next;
        } else {
            head_ = group.next;
        }

        if (group.next) {
            group.next->previous = group.previous;
        } else {
            tail_ = group.previous;
        }
        group.previous = group.next = nullptr;
    }

    Group* head_{}; // 待通知 Scope 队首, 初始空, Group 由外部路由容器拥有.
    Group* tail_{}; // 队尾, 与 head_ 同时为空.
};
} // namespace astra
