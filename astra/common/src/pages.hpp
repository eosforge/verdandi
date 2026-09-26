#pragma once
#include <algorithm>
#include <array>
#include <astra/profile.hpp>
#include <bit>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace astra {
// 捕获与写入由所属状态锁保护. 读者完成遍历后也经过该锁, 再在锁外释放 View, 包括异常路径.
// 这为共享页转回独占时建立读完成同步; use_count 本身不能充当读写屏障.
// prepare 可以分配但不改逻辑内容, set/erase 是无分配提交阶段.
// 空槽按页占用量复用, 空页随删除回收; 深度最多 15, 不为历史删除次数保留索引空间.
template <typename Item, typename Name = std::string>
class Pages {
    // 无分配提交要求移动记录不抛异常, 复制页时仍允许复制构造分配失败.
    static_assert(std::is_nothrow_move_assignable_v<Item>);

public:
    // 拥有原始载荷字节, 不在索引内解释业务编码; 空 Buffer 仍是合法值.
    using Buffer = std::vector<std::uint8_t>;
    // 不可变载荷的共享所有权, 空指针只用于尚未占用的内部行.
    using Value = std::shared_ptr<const Buffer>;
    // 不可变键的共享所有权, 使路径复制不重复分配长字符串.
    using Key = std::shared_ptr<const Name>;

    // 原生记录类型, 页索引不解释载荷、版本或 TTL.
    using Record = Item;

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
    // View 只拥有根和深度, 所属状态锁内复制为 O(1). 活着的 View 使后续写入复制其修改路径.
    class View {
    public:
        // 返回捕获时的有效项数, 无需遍历; 只在当前 View 寿命内读取.
        std::size_t size() const noexcept {
            return root_ ? root_->used : 0;
        }

        // reader 接收稳定的 Key/Value 引用, 遍历和最终回收均在 所属状态锁外进行.
        // reader 抛错时同样由调用者完成上述读区同步, 不将异常转成部分成功.
        void each(auto&& reader) const {
            visit(root_.get(), height_, reader);
        }

        // 从第 offset 个有效项起最多访问 maximum 项, reader 返回 true 表示已消费, false 在当前项前暂停.
        // 返回实际消费数, 不改变 View; 子树计数跳过已发送前缀, 分页不重复遍历先前全部记录.
        // offset 必须在 0..size, maximum 为零允许只询问空页; 异常由上层丢弃本页并维持旧位置.
        std::size_t page(std::size_t offset, std::size_t maximum, auto&& reader) const {

            ASTRA_PROFILE_SCOPE("common.pages.page");

            if (offset > size()) {
                throw std::out_of_range("Snapshot page position exceeds view");
            }
            std::size_t consumed{}; // 本次页已被 reader 接受的有效项数, 不包含拒绝的当前项.
            if (maximum != 0) {
                static_cast<void>(slice(root_.get(), height_, offset, maximum, consumed, reader));
            }
            return consumed;
        }

    private:
        friend class Pages;
        // 捕获的只读根, 可为空; 保持旧页面寿命直到读者释放此 View.
        std::shared_ptr<const Node> root_;
        // 捕获时的树高, 0 表示叶页, 最大 15; 不随当前索引后续变化.
        unsigned height_{};
    };

    // 取得当前索引版本的稳定视图, 调用方同时捕获 所属业务版本, 不在此处分配.
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

    // 一次槽号寻址借用键所有权及原生记录, 无记录返回两个 nullptr; 指针不跨后续 prepare/提交使用.
    std::pair<const Key*, const Record*> find(std::uint64_t slot) const noexcept {

        if (height_ < 15 && (slot >> (6 + height_ * 4)) != 0) {
            return {};
        }
        const auto* node = root_.get(); // 每层只读寻址, 不捕获根或增加共享引用.
        for (auto level = height_; node && level != 0; --level) {
            node = static_cast<const Branch*>(node)->children[branch(slot, level)].get();
        }
        if (!node) {
            return {};
        }
        const auto& row = static_cast<const Leaf*>(node)->rows[slot & 63]; // 稳定槽所属叶行.
        return row.key ? std::pair<const Key*, const Record*>{&row.key, &row.record} : std::pair<const Key*, const Record*>{};
    }

    // 仅需原生记录时不暴露页布局, 调用方仍须持有所属状态保护.
    const Record* at(std::uint64_t slot) const noexcept {
        return find(slot).second;
    }

    // 创建/复制到指定槽的全部页面. 失败只留下内容相同的私有页, 当前与已捕获视图均不变.
    void prepare(std::uint64_t slot) {

        ASTRA_PROFILE_SCOPE("common.pages.prepare");

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

        ASTRA_PROFILE_SCOPE("common.pages.set");

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
        assert(added == !row.key);
        node->used += added;
        if (added) {
            row.key = std::move(key);
            leaf.occupied |= std::uint64_t{1} << (slot & 63);
        }
        row.record = std::move(record);
    }

    // prepare 后删除有效槽. 空子树沿路径回收, 不扫描其他页或分配空闲槽列表.
    void erase(std::uint64_t slot) noexcept {

        ASTRA_PROFILE_SCOPE("common.pages.erase");

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
            ASTRA_PROFILE_COUNT("common.pages.allocate_bytes", sizeof(Page));
            link = std::make_shared<Page>();
        } else if (link.use_count() != 1) {
            ASTRA_PROFILE_COUNT("common.pages.clone_bytes", sizeof(Page));
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

    // 按槽号递增访问有效项, reader 可抛错; 页面由 View 保持存活, 不访问 所属状态的可变节点.
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

    // 有界顺序读取, skip 是待跳过有效项数, remaining 是本页余额, consumed 只在 reader 接受后增加.
    // 返回 false 表示页满或 reader 暂停, 不影响本次捕获根; 所有移位都局限于叶页 0..63.
    static bool slice(const Node* node, unsigned level, std::size_t& skip, std::size_t& remaining, std::size_t& consumed, auto& reader) {

        if (!node || node->used == 0) {
            return true;
        }
        if (skip >= node->used) {
            skip -= node->used;
            return true;
        }
        if (level != 0) {
            for (const auto& child : static_cast<const Branch*>(node)->children) {
                if (!slice(child.get(), level - 1, skip, remaining, consumed, reader)) {
                    return false;
                }
            }
            return true;
        }

        const auto& leaf = *static_cast<const Leaf*>(node); // 捕获根固定的叶页, 不访问当前可变索引.
        auto occupied = leaf.occupied;                      // 只扫描有效位, 不按历史最大槽号走空槽.
        while (occupied) {
            const auto slot = static_cast<unsigned>(std::countr_zero(occupied));
            occupied &= occupied - 1;
            if (skip != 0) {
                --skip;
                continue;
            }
            const auto& row = leaf.rows[slot]; // 借用引用只在 reader 调用期间使用.
            if (!reader(*row.key, row.record)) {
                return false;
            }
            ++consumed;
            if (--remaining == 0) {
                return false;
            }
        }
        return true;
    }

    // root_ 只由 所属状态写线程在状态锁内访问. View 的引用使被读页面保持不可变.
    std::shared_ptr<Node> root_;
    // 当前树高为 0..15, 空树为零; 删除只在不改变槽号和已准备路径时缩根.
    unsigned height_{};
};
} // namespace astra
