#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace comet::detail {
// SDK 私有页化字典. 每完整批次最多复制命中的 256 个页, 不按每条变更复制整表.
// 发布页永不再写, 不根据 use_count 复用页面; 旧 View 释放无需进入已经销毁的网络状态.
// Record 提供 bytes()/overhead 计量正文/额外对象, 三域可复用容器但不统一业务版本/记录语义.
template <class Record>
class Table {
    struct Hash {
        using is_transparent = void; // 查找 string_view 时不构造临时 string.

        std::size_t operator()(std::string_view key) const noexcept {
            return std::hash<std::string_view>{}(key);
        }
    };

    using Page = std::unordered_map<std::string, Record, Hash, std::equal_to<>>; // 一个 immutable 页的点查表.

    // 页头、共享控制块与桶数组的保守空间, 不包含由 bytes_/size_ 计量的键和记录.
    static std::size_t measure(const Page& page) noexcept {
        return sizeof(Page) + 32 + page.bucket_count() * sizeof(void*);
    }

    std::array<std::shared_ptr<const Page>, 256> pages_{}; // 根复制固定为 256 个共享引用, 空页无需分配.
    std::size_t size_{};                                   // 当前完整记录数, 不通过全表遍历计算.
    std::size_t bytes_{};                                  // Key/Record 的逻辑字节, 不声称等于分配器或 RSS.
    std::size_t overhead_{};                               // 当前非空页头和桶数组之和, 发布后不变, 空表为零.

public:
    // 未安装数据的空容器, 没有版本和业务角色语义.
    Table() = default;
    // 按标准共享根复制/移动, 只读页面不被后续候选修改.
    Table(const Table&) = default;
    Table(Table&&) noexcept = default;
    Table& operator=(const Table&) = default;
    Table& operator=(Table&&) noexcept = default;

    std::size_t size() const noexcept {
        return size_;
    } // O(1) 返回完整项数.

    std::size_t bytes() const noexcept {
        return bytes_;
    } // O(1) 返回完整逻辑字节.

    // 对当前固定布局保守计入页、桶、节点和独立载荷控制块, 不把它解释为分配器/RSS 硬上限.
    std::size_t footprint() const noexcept {
        return bytes_ + sizeof(Table) + size_ * (sizeof(typename Page::value_type) + Record::overhead + 32) + overhead_;
    }

    // 返回借用记录, 有效期由这份 Table 所有权保证, 未找到返回空.
    const Record* find(std::string_view key) const {
        const auto& page = pages_[Hash{}(key) & 255];
        if (!page) {
            return nullptr;
        }
        const auto entry = page->find(key);
        return entry == page->end() ? nullptr : &entry->second;
    }

    // 按稳定但不承诺字典序的页顺序遍历, reader 只借用不可变 Key/Record.
    void each(auto&& reader) const {
        for (const auto& page : pages_) {
            if (page) {
                for (const auto& [key, record] : *page) {
                    reader(key, record);
                }
            }
        }
    }

    // 一次完整批次的独占准备. 任意失败后上层丢弃整个 Draft, 不发布部分页面或计数.
    class Draft {
    public:
        // 捕获已有只读根, reset 使用默认空 Table, apply 共享原根.
        explicit Draft(const Table& base) : pages_(base.pages_), size_(base.size_), bytes_(base.bytes_), overhead_(base.overhead_) {}

        Draft(Draft&&) noexcept = default; // 单个恢复任务移交候选.
        Draft(const Draft&) = delete;      // 防止两个写者共享可写页.

        std::size_t size() const noexcept {
            return size_;
        } // 包括本批尚未发布修改.

        std::size_t bytes() const noexcept {
            return bytes_;
        } // 上层据此执行本地视图预算.

        // 私有候选包含根和修改页引用, 计费也覆盖旧页共享引用, 不仅统计 Buffer 字节.
        std::size_t footprint() const noexcept {
            return bytes_ + sizeof(Draft) + size_ * (sizeof(typename Page::value_type) + Record::overhead + 32) + overhead_;
        }

        // key 仅在本次调用中借用, 新增时才复制为页内拥有式 string; 覆盖已有 Key 不再分配名称.
        // 载荷由 Record 自己共享/拥有, 本页第一次变化才复制其他条目.
        void set(std::string_view key, Record record) {

            // 在复制页面之前检查单记录加法, 非法长度不引起整页分配.
            const auto payload = record.bytes(); // Record 只报告自身载荷, Key 单独计量.
            if (payload > SIZE_MAX - key.size()) {
                throw std::length_error("View record size overflow");
            }
            const auto cost = key.size() + payload;

            // slot 定位唯一哈希页; old/previous 固定替换前的记录及逻辑空间.
            const auto slot = Hash{}(key) & 255;
            auto& page = edit(slot);
            const auto old = page.find(key);
            const auto previous = old == page.end() ? 0 : old->first.size() + old->second.bytes();
            if (cost > SIZE_MAX - (bytes_ - previous)) {
                throw std::length_error("View byte count overflow");
            }

            // 更新复用已有迭代器, 不再由 insert_or_assign 重做字符串哈希和查找.
            // 只有新增才可能扩桶; 任意异常仍由调用方放弃整个候选.
            if (old != page.end()) {
                old->second = std::move(record);
            } else {
                const auto before = measure(page); // 只度量一个页, 不扫描其他 255 页.
                page.emplace(std::string(key), std::move(record));
                overhead_ = overhead_ - before + measure(page);
                ++size_;
            }
            bytes_ = bytes_ - previous + cost;
        }

        // 缺失 Delete 无变化, 不复制空页; 删除完成后空页回收, 不保留历史桶水线.
        void erase(std::string_view key) {

            // 原页缺失键不创建修改页; 已有独占页直接进入一次 find, 不先 contains 再 find.
            const auto slot = Hash{}(key) & 255;
            if (!dirty_[slot] && (!pages_[slot] || !pages_[slot]->contains(key))) {
                return;
            }

            // erase 不改变 unordered_map 的桶数; 空页仍计费到 finish 真正解除拥有为止.
            auto& page = edit(slot);
            const auto found = page.find(key);
            if (found == page.end()) {
                return;
            }
            bytes_ -= found->first.size() + found->second.bytes();
            --size_;
            page.erase(found);
        }

        // 发布只读页面, 成功后消费候选, 不再允许调用 set/erase/finish.
        Table finish() && {

            // 先移交旧根, 然后仅处理四个占用字和其中实际修改的页.
            Table result;
            result.pages_ = std::move(pages_);
            for (std::size_t group = 0; group < changed_.size(); ++group) {
                auto bits = changed_[group]; // 每一位对应本批已独占准备的页, 逐位清除不修改原位图.
                while (bits != 0) {
                    const auto slot = group * 64 + static_cast<std::size_t>(std::countr_zero(bits)); // 取值 0..255.
                    auto& page = dirty_[slot];                                                       // 位图置位后页一定存在, 即使其最后一个 Key 已被删除.
                    if (page->empty()) {
                        overhead_ -= measure(*page);
                        result.pages_[slot].reset();
                        page.reset();
                    } else {
                        result.pages_[slot] = std::move(page);
                    }
                    bits &= bits - 1;
                }
            }

            // 发布后的计量不再包含已释放空页, 点查和旧快照均不借用 Draft.
            result.size_ = size_;
            result.bytes_ = bytes_;
            result.overhead_ = overhead_;
            return result;
        }

    private:
        // 只复制第一次命中的页, 不将可写指针交给旧 Table 或公开 View.
        Page& edit(std::size_t slot) {
            if (!dirty_[slot]) {
                // 哈希表复制不承诺保留桶数, 必须以新页的实际空间替换原页计量.
                auto page = pages_[slot] ? std::make_shared<Page>(*pages_[slot]) : std::make_shared<Page>();
                overhead_ = overhead_ - (pages_[slot] ? measure(*pages_[slot]) : 0) + measure(*page);
                dirty_[slot] = std::move(page);
                changed_[slot / 64] |= std::uint64_t{1} << (slot % 64);
            }
            return *dirty_[slot];
        }

        std::array<std::shared_ptr<const Page>, 256> pages_; // 原根只读共享, 候选销毁可独立释放.
        std::array<std::shared_ptr<Page>, 256> dirty_{};     // 本批独占的修改页, 无外部可写别名.
        std::array<std::uint64_t, 4> changed_{};             // 256 页的修改位图, 不另建动态索引或逐页检查空指针.
        std::size_t size_{};                                 // 准备后的记录数.
        std::size_t bytes_{};                                // 准备后的 Key/载荷逻辑字节.
        std::size_t overhead_{};                             // 候选有效页头/桶空间之和, 包含尚未发布的空修改页.
    };
};
} // namespace comet::detail
