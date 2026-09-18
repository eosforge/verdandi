#pragma once
#include <algorithm>
#include <array>
#include <astra/clock.hpp>
#include <bit>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace astra {
// 捕获与写入由 Store 状态锁保护. 读者完成遍历后也经过该锁, 再在锁外释放 View, 包括异常路径.
// 这为共享页转回独占时建立读完成同步; use_count 本身不能充当读写屏障.
// prepare 可以分配但不改逻辑内容, set/erase 是无分配提交阶段.
// 空槽按页占用量复用, 空页随删除回收; 深度最多 15, 不为历史删除次数保留索引空间.
class SnapshotIndex {
public:
    using Buffer = std::vector<std::uint8_t>;
    using Value = std::shared_ptr<const Buffer>;
    using Key = std::shared_ptr<const std::string>;
    // 值与截止属于同一不可变视图, 全量同步不能丢失或重新续满 TTL.
    struct Record {
        // 共享不可变载荷, 与历史/Entry 使用相同所有权.
        Value value;
        // 固定 Unix 截止, 空表示永久; 不保留第二份本地期限.
        std::optional<EpochClock::Time> deadline{};
    };

private:
    // used 是子树中的有效项数, 同时用于 O(1) 空子树识别和删除回收.
    struct Node {
        std::size_t used{};
    };
    // 一页 64 项. Key 独立共享, 覆盖已有值不重新复制长 Key, 旧视图拥有自己的 Value 引用.
    struct Row {
        Key key;
        Record record;
    };
    struct Leaf : Node {
        // 位图只用于 O(1) 定位页内空槽, 与 used 在无异常提交中同时更新.
        std::uint64_t occupied{};
        std::array<Row, 64> rows;
    };
    // 上层每页 16 个子页. shared_ptr 保留实际派生类型的删除器, Node 不做多态访问.
    struct Branch : Node {
        std::array<std::shared_ptr<Node>, 16> children;
    };

public:
    // View 只拥有根和深度, Store 锁内复制为 O(1). 活着的 View 使后续写入复制其修改路径.
    class View {
    public:
        // 返回捕获时的有效项数, 无需遍历; 只在当前 View 寿命内读取.
        std::size_t size() const noexcept {
            return root_ ? root_->used : 0;
        }
        // reader 接收稳定的 Key/Value 引用, 遍历和最终回收均在 Store 状态锁外进行.
        // reader 抛错时同样由调用者完成上述读区同步, 不将异常转成部分成功.
        void each(auto&& reader) const {
            visit(root_.get(), height_, reader);
        }

    private:
        friend class SnapshotIndex;
        std::shared_ptr<const Node> root_;
        unsigned height_{};
    };

    // 取得当前索引版本的稳定视图, 调用方同时捕获 Store version, 不在此处分配.
    View capture() const noexcept {
        View result;
        result.root_ = root_;
        result.height_ = height_;
        return result;
    }
    // 新有效 Key 优先使用现有页的空槽. 仅当前树满时扩展, 不因删除/重建造成稀疏页无限累积.
    std::uint64_t next() const {
        if (height_ < 15 && root_ && root_->used == (std::uint64_t{1} << (6 + height_ * 4))) {
            return std::uint64_t{1} << (6 + height_ * 4);
        }
        const auto slot = vacant(root_.get(), height_, 0);
        if (slot == UINT64_MAX) {
            throw std::overflow_error("Store snapshot slot exhausted");
        }
        return slot;
    }
    // 创建/复制到指定槽的全部页面. 失败只留下内容相同的私有页, 当前与已捕获视图均不变.
    void prepare(std::uint64_t slot) {
        // 第 15 层已覆盖全部 uint64 槽号, 不执行超过位宽的右移.
        while (height_ < 15 && (slot >> (6 + height_ * 4)) != 0) {
            auto parent = std::make_shared<Branch>();
            parent->children[0] = root_;
            parent->used = root_ ? root_->used : 0;
            root_ = std::move(parent);
            ++height_;
        }
        auto* link = &root_;
        for (auto level = height_; level != 0; --level) {
            private_page<Branch>(*link);
            link = &static_cast<Branch&>(**link).children[branch(slot, level)];
        }
        private_page<Leaf>(*link);
    }
    // prepare 后提交新值. key 非空表示新有效项; 覆盖已有项传空 Key 并复用原始字符串.
    void set(std::uint64_t slot, Key key, Record record) noexcept {
        const bool added = static_cast<bool>(key);
        auto* node = root_.get();
        for (auto level = height_; level != 0; --level) {
            node->used += added;
            node = static_cast<Branch*>(node)->children[branch(slot, level)].get();
        }
        auto& leaf = static_cast<Leaf&>(*node);
        auto& row = leaf.rows[slot & 63];
        assert(added == !row.key && record.value);
        node->used += added;
        if (added) {
            row.key = std::move(key);
            leaf.occupied |= std::uint64_t{1} << (slot & 63);
        }
        row.record = std::move(record);
    }
    // prepare 后删除有效槽. 空子树沿路径回收, 不扫描其他页或分配空闲槽列表.
    void erase(std::uint64_t slot) noexcept {
        remove(root_, height_, slot);
        if (!root_) {
            height_ = 0;
        }
        while (root_ && height_ != 0) {
            const auto& children = static_cast<const Branch&>(*root_).children;
            // 只有第 0 个子树能原样提升, 其他位置隐含槽号高位. 计数先排除普通的多分支根.
            if (!children[0] || children[0]->used != root_->used) {
                break;
            }
            // prepare 可能留下尚未提交的空页, 不能只按 used 丢弃这些已准备的写入路径.
            if (std::ranges::any_of(children.begin() + 1, children.end(), [](const auto& child) { return child != nullptr; })) {
                break;
            }
            // 先取得独立引用再释放旧根, 不搬空可能仍被旧 View 共享的父页链接. 提升本身不分配.
            auto child = children[0];
            root_ = std::move(child);
            --height_;
        }
    }

private:
    // 只克隆仍被旧视图/其他父页共享的页; 未共享页继续原地提交, 避免每次写入分配整条路径.
    template <typename Page> static void private_page(std::shared_ptr<Node>& link) {
        if (!link) {
            link = std::make_shared<Page>();
        } else if (link.use_count() != 1) {
            link = std::make_shared<Page>(static_cast<const Page&>(*link));
        }
    }
    // level 为 1..15, 从槽号取本层四位; 调用方保证树深度与节点类型一致.
    static unsigned branch(std::uint64_t slot, unsigned level) noexcept {
        return static_cast<unsigned>((slot >> (6 + (level - 1) * 4)) & 15);
    }
    // 按有效计数跳过满子树, 每层最多查看 16 个链接, 不扫描 Key 或历史墓碑.
    static std::uint64_t vacant(const Node* node, unsigned level, std::uint64_t prefix) noexcept {
        if (!node) {
            return prefix;
        }
        if (level == 0) {
            const auto free = ~static_cast<const Leaf*>(node)->occupied;
            return free ? prefix | static_cast<std::uint64_t>(std::countr_zero(free)) : UINT64_MAX;
        }
        const auto shift = 6 + (level - 1) * 4;
        const auto& children = static_cast<const Branch*>(node)->children;
        // 顶层只有四个链接落在 64 位槽号域内, 不允许左移后截断回绕.
        for (unsigned child = 0; child < (level == 15 ? 4U : 16U); ++child) {
            if (!children[child] || children[child]->used < (std::uint64_t{1} << shift)) {
                return vacant(children[child].get(), level - 1, prefix | (static_cast<std::uint64_t>(child) << shift));
            }
        }
        return UINT64_MAX;
    }
    // 路径已由 prepare 私有化且槽有效, 清零叶项并沿路减计数, 不触碰其他子树.
    static void remove(std::shared_ptr<Node>& link, unsigned level, std::uint64_t slot) noexcept {
        assert(link && link->used != 0);
        if (level == 0) {
            auto& leaf = static_cast<Leaf&>(*link);
            auto& row = leaf.rows[slot & 63];
            assert(row.key);
            row = {};
            leaf.occupied &= ~(std::uint64_t{1} << (slot & 63));
        } else {
            remove(static_cast<Branch&>(*link).children[branch(slot, level)], level - 1, slot);
        }
        if (--link->used == 0) {
            link.reset();
        }
    }
    // 按槽号递增访问有效项, reader 可抛错; 页面由 View 保持存活, 不访问 Store 的可变节点.
    static void visit(const Node* node, unsigned level, auto& reader) {
        if (!node || node->used == 0) {
            return;
        }
        if (level == 0) {
            for (const auto& row : static_cast<const Leaf*>(node)->rows) {
                if (row.key) {
                    reader(*row.key, row.record);
                }
            }
        } else {
            for (const auto& child : static_cast<const Branch*>(node)->children) {
                visit(child.get(), level - 1, reader);
            }
        }
    }
    // root_ 只由 Store 写线程在状态锁内访问. View 的引用使被读页面保持不可变.
    std::shared_ptr<Node> root_;
    unsigned height_{};
};
} // namespace astra
