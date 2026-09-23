#pragma once
#include "comet.pb.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace astra {
// 动态域共用的冻结分页机制. Encoding 仅定义内容计费, 校验, 字段编码及同名事件合并.
// 不拥有可变 State, 不引入虚函数, 运行时域标志或额外的正文副本; 不负责网络确认.
template <typename Encoding>
class Pagination {
    using Projection = typename Encoding::Projection; // 本域的不可变 Scene 根与事件类型.
    using Content = typename Encoding::Content;       // 公开内容, 不包含 TTL 或来源恢复位置.
    using Event = typename Projection::Event;         // 已验证连续范围内的可见变化.

public:
    using Reply = typename Encoding::Reply; // 独立生成消息, 保留两域的静态类型检查.

    // 冻结整范围, 只共享已有索引根; 版本和计费从移动后的目的对象读取.
    explicit Pagination(typename Projection::View view) : data_(std::make_shared<const Data>(std::move(view))) {
        const auto& captured = std::get<typename Projection::View>(*data_); // 根与元数据来自同一提交边界.
        version_ = captured.version();
        bytes_ = captured.bytes();
    }

    // 精确目标的缺项也产生合法空 reset, 不构造整张范围快照.
    Pagination(std::string key, typename Projection::Point point) : data_(std::make_shared<const Data>(Point{std::move(key), std::move(point.record)})), version_(point.version) {
        const auto& value = std::get<Point>(*data_); // key 已移交, record 非空时拥有完整公开内容.
        bytes_ = sizeof(Point) + value.key.size() + (value.record ? Encoding::measure(*value.record) : 0);
    }

    // changes 已连续覆盖到 version. 过滤/压缩只改变待发送行, 不重算完成游标; 非法批次抛 invalid_argument.
    Pagination(std::uint64_t version, std::vector<Event> changes, std::string_view target = {}) : mode_(proto::comet::v1::MODE_APPLY), version_(version) {

        // 校验先于目标过滤, 防止无关目标掩盖损坏事件. data-only 不能指向删除项.
        for (const auto& change : changes) {
            if (!change.name || change.version == 0 || change.version > version || (change.data && !change.record)) {
                throw std::invalid_argument("Invalid projection change batch");
            }
        }
        if (!target.empty()) {
            std::erase_if(changes, [&](const auto& change) { return change.name->key != target; });
        }
        const auto order = [](const auto& change) { return std::tie(change.name->key, change.version); }; // 引用排序键, 不复制名称或载荷.
        if (!std::ranges::is_sorted(changes, {}, order)) {
            std::ranges::sort(changes, {}, order);
        }

        // 原地合并相同名称, 最终 action 的领域语义由 Encoding 保留, 不用新 HashMap 收集.
        std::size_t kept{}; // 压缩后的有效前缀, 不等于 vector 实际容量.
        for (std::size_t first = 0; first < changes.size();) {
            auto& latest = changes[first]; // 本组首项作为合并目标, 不额外增加共享引用计数.
            auto end = first + 1;          // 下一个尚未合并的位置, 最大为 changes.size().
            while (end < changes.size() && changes[end].name->key == latest.name->key) {
                latest = merge(latest, std::move(changes[end]));
                ++end;
            }
            bytes_ += latest.bytes;
            if (kept != first) {
                changes[kept] = std::move(latest); // 避免自移动丢失名称/正文.
            }
            ++kept;
            first = end;
        }
        bytes_ += (changes.capacity() - kept) * sizeof(Event); // resize 不释放尾部容量, 计费仍包含它.
        changes.resize(kept);
        data_ = std::make_shared<const Data>(std::move(changes));
    }

    // 移动交接共享来源, 复制保留各自的分页位置, 都不复制底层正文或历史容器.
    Pagination(Pagination&&) noexcept = default;
    Pagination(const Pagination&) noexcept = default;
    // 缓存命中采纳已经编码的位置, 不代表对应网络 Write 已完成.
    Pagination& operator=(const Pagination&) noexcept = default;

    // 非空目标的格式由各域决定, 空串的全范围语义由调用方处理.
    static bool target(std::string_view value) noexcept {
        return Encoding::target(value);
    }

    // 冻结批次内合并, Catalog 保留末值, Ephemeris 还须保留完整 Attr 的依据.
    static Event merge(const Event& previous, Event current) noexcept {
        return Encoding::merge(previous, std::move(current));
    }

    // 用于缓存匹配, 不根据版本是否为零推测模式.
    bool reset() const noexcept {
        return mode_ == proto::comet::v1::MODE_RESET;
    }

    // 完成页已构造, 不等价于网络已确认.
    bool complete() const noexcept {
        return complete_;
    }

    // 与冻结根/后缀同边界的完整游标, 只由外部在 Write 成功后确认.
    std::uint64_t version() const noexcept {
        return version_;
    }

    // 本批保守持有计费, 不随页面消费减少, 不等于新增 RSS.
    std::size_t bytes() const noexcept {
        return bytes_;
    }

    // 软目标 256 KiB/256 项, 单项可独占大页, 硬上限 8 MiB. 失败不消费当前分页位置.
    Reply next(std::string_view instance) {

        if (complete_) {
            throw std::logic_error("Projection edition already completed");
        }
        if (instance.empty() || instance.size() > 128) {
            throw std::invalid_argument("Invalid serving instance");
        }
        Reply page; // 每页独立拥有消息, 不复用上一页的可写对象.
        page.set_mode(mode_);
        std::size_t bytes = instance.size() + 64; // 预留尾页身份/游标, 不事后突破上限.
        std::size_t consumed{};                   // 本页成功消费的行数, 异常时保持 offset_.
        bool finished = true;                     // 空范围, 精确缺项, 无相关变化也必须发送 complete.

        if (const auto* view = std::get_if<typename Projection::View>(data_.get())) {
            consumed = view->page(offset_, 256, [&](const std::string& key, const Content& record) { return append(page, bytes, key, &record, false); });
            finished = offset_ + consumed == view->size();
        } else if (const auto* point = std::get_if<Point>(data_.get())) {
            if (point->record) {
                static_cast<void>(append(page, bytes, point->key, &*point->record, false));
                consumed = 1;
            }
        } else {
            const auto& changes = std::get<std::vector<Event>>(*data_); // 永远读冻结内容, 不补入当前 State 的新记录.
            while (offset_ + consumed < changes.size()) {
                const auto& change = changes[offset_ + consumed]; // 本行最终 action 及 Attr 基线提示.
                if (!append(page, bytes, change.name->key, change.record ? &*change.record : nullptr, change.data)) {
                    break;
                }
                ++consumed;
            }
            finished = offset_ + consumed == changes.size();
        }

        if (finished) {
            page.set_complete(true);
            page.set_version(version_);
            page.set_instance(instance);
        }
        offset_ += consumed;
        complete_ = finished;
        return page;
    }

private:
    // 精确点查拥有名称和可选完整内容, record 为空是缺项, 不是删除增量.
    struct Point {
        std::string key;               // Catalog Key 或 Ephemeris UUID 的原始拥有式文本.
        std::optional<Content> record; // 捕获时的完整公开内容, 不从后续状态补字段.
    };

    // 先由领域校验并计量载荷, 再统一执行预算和消息追加; soft 超限不消费该行.
    static bool append(Reply& page, std::size_t& bytes, std::string_view key, const Content* record, bool data) {

        const auto payload = Encoding::payload(key, record, data); // 只读标量长度, 不分配中间编码.
        const auto cost = key.size() + payload + 48;               // 保守包含嵌套字段/长度, 领域载荷仍受既有业务上限约束.
        if (cost > 8 * 1024 * 1024 - bytes) {
            throw std::length_error("Projection record exceeds page budget");
        }
        if (page.changes_size() != 0 && (page.changes_size() >= 256 || cost > 256 * 1024 - std::min(bytes, std::size_t{256 * 1024}))) {
            return false;
        }
        Encoding::append(page, key, record, data);
        bytes += cost;
        return true;
    }

    using Data = std::variant<typename Projection::View, Point, std::vector<Event>>; // 三种固定来源, 与业务领域选择无关.
    std::shared_ptr<const Data> data_;                                               // 多订阅共享同一冻结来源, 分页位置独立.
    proto::comet::v1::Mode mode_ = proto::comet::v1::MODE_RESET;                     // 全批固定, 不在页间切换.
    std::uint64_t version_{};                                                        // 冻结时完整范围游标, 允许合法零值.
    std::size_t bytes_{};                                                            // 保守引用字节, 初始零.
    std::size_t offset_{};                                                           // 已成功构造的行数, 初始零.
    bool complete_{};                                                                // 最后一页构造后为 true, 禁止再次 next.
};
} // namespace astra
