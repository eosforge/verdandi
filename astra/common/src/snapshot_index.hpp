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
class Index {
public:
    // 拥有原始载荷字节, 不在索引内解释业务编码; 空 Buffer 仍是合法值.
    using Buffer = std::vector<std::uint8_t>;
    // 不可变载荷的共享所有权, 空指针只用于尚未占用的内部行.
    using Value = std::shared_ptr<const Buffer>;
    // 不可变键的共享所有权, 使路径复制不重复分配长字符串.
    using Key = std::shared_ptr<const std::string>;

    // 值与截止属于同一不可变视图, 全量同步不能丢失或重新续满 TTL.
    struct Record {
        // 共享不可变载荷, 与历史/Entry 使用相同所有权.
        Value value;
        // 固定 Unix 截止, 空表示永久; 不保留第二份本地期限.
        std::optional<Clock::Time> deadline{};
    };

private:
    // used 是子树中的有效项数, 同时用于 O(1) 空子树识别和删除回收.
    struct Node {
        // 当前子树的有效记录数, 初始为零; prepare 的空页不增加计数.
        std::size_t used{};
    };

    // 一页 64 项. Key 独立共享, 覆盖已有值不重新复制长 Key, 旧视图拥有自己的 Value 引用.
    struct Row {
        // 有效行独占一个槽号并共享键文本, 空指针表示该行未占用.
        Key key;
        // 与 key 同版本的载荷和截止, 旧 View 继续拥有原记录.
        Record record;
    };

    struct Leaf : Node {
        // 位图只用于 O(1) 定位页内空槽, 与 used 在无异常提交中同时更新.
        std::uint64_t occupied{};
        // 页内固定 64 行, 由槽号低六位寻址; 空行不对外返回.
        std::array<Row, 64> rows;
    };

    // 上层每页 16 个子页. shared_ptr 保留实际派生类型的删除器, Node 不做多态访问.
    struct Branch : Node {
        // 固定 16 路共享子页, 空指针表示未分配; 最高层仅使用前四路.
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
        friend class Index;
        // 捕获的只读根, 可为空; 保持旧页面寿命直到读者释放此 View.
        std::shared_ptr<const Node> root_;
        // 捕获时的树高, 0 表示叶页, 最大 15; 不随当前索引后续变化.
        unsigned height_{};
    };

    // 取得当前索引版本的稳定视图, 调用方同时捕获 Store version, 不在此处分配.
    View capture() const noexcept {

        // result 仅复制根的共享引用和树高, 不复制键值或遍历节点.
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

        // slot 为最低可用槽号, UINT64_MAX 保留为耗尽哨兵, 不交给业务项.
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
            // parent 在提交前独立分配, 子树放在第零路以保持所有既有槽号不变.
            auto parent = std::make_shared<Branch>();
            parent->children[0] = root_;
            parent->used = root_ ? root_->used : 0;
            root_ = std::move(parent);
            ++height_;
        }

        // link 借用逐层共享指针槽位, 路径私有化完成前不改动任何业务记录.
        auto* link = &root_;
        // level 从当前树高向叶页递减, 范围 1..15, 零层由循环后的叶操作处理.
        for (auto level = height_; level != 0; --level) {
            private_page<Branch>(*link);
            link = &static_cast<Branch&>(**link).children[branch(slot, level)];
        }
        private_page<Leaf>(*link);
    }

    // prepare 后提交新值. key 非空表示新有效项; 覆盖已有项传空 Key 并复用原始字符串.
    void set(std::uint64_t slot, Key key, Record record) noexcept {

        // added 表示新占用槽位, 只在这种情况下累加沿途 used 并设置位图.
        const bool added = static_cast<bool>(key);
        // node 借用 prepare 后的私有页, 本次无分配提交期间地址稳定.
        auto* node = root_.get();
        // level 从当前树高向叶页递减, 范围 1..15, 零层由循环后的叶操作处理.
        for (auto level = height_; level != 0; --level) {
            node->used += added;
            node = static_cast<Branch*>(node)->children[branch(slot, level)].get();
        }

        // leaf 为零层叶页, 页类型与 height_ 的路径结构保持一致.
        auto& leaf = static_cast<Leaf&>(*node);
        // row 借用槽号低六位选中的记录, 有效范围为 0..63.
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
            // children 只读借用当前根的子页, 缩根前必须先取得被提升子页的独立引用.
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
    template <typename Page>
    static void private_page(std::shared_ptr<Node>& link) {

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
            // free 位图中的 1 表示空槽, 零表示整页已满, 不对零调用位号选择逻辑.
            const auto free = ~static_cast<const Leaf*>(node)->occupied;
            return free ? prefix | static_cast<std::uint64_t>(std::countr_zero(free)) : UINT64_MAX;
        }

        // shift 为当前子页代表的低位宽, 由层数得到 6..62, 所有移位均小于 64.
        const auto shift = 6 + (level - 1) * 4;
        // children 为本层子页数组, 结合 used 跳过满子树而不扫描叶行.
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
            // leaf 是已私有化的叶页, 删除只修改目标行和对应占用位.
            auto& leaf = static_cast<Leaf&>(*link);
            // row 必须有效, 清空共享引用与占用位后再回收沿途空页.
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
            // row 顺序借用叶页各项, 只把占用行传给 reader; 引用寿命受 View 约束.
            for (const auto& row : static_cast<const Leaf*>(node)->rows) {
                if (row.key) {
                    reader(*row.key, row.record);
                }
            }
        } else {
            // child 顺序借用子页, 递归跳过空树, 遍历期间不改变当前页.
            for (const auto& child : static_cast<const Branch*>(node)->children) {
                visit(child.get(), level - 1, reader);
            }
        }
    }

    // root_ 只由 Store 写线程在状态锁内访问. View 的引用使被读页面保持不可变.
    std::shared_ptr<Node> root_;
    // 当前树高为 0..15, 空树为零; 删除只在不改变槽号和已准备路径时缩根.
    unsigned height_{};
};
} // namespace astra
