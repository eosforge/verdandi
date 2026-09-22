#include "catalog_edition.hpp"
#include <ranges>

namespace astra {
Catalog::Edition::Edition(State::Projection::View view) : data_(view), version_(view.version()), bytes_(view.bytes()) {}

Catalog::Edition::Edition(std::string key, State::Projection::Point point) : data_(Point{std::move(key), point.record}), version_(point.version) {
    const auto& value = std::get<Point>(data_); // 点查只计自身名称与完整载荷, 不捕获整 Scope.
    bytes_ = sizeof(Point) + value.key.size() + (value.record ? value.record->value->size() : 0);
}

Catalog::State::Projection::Event Catalog::Edition::merge(const State::Projection::Event& previous, State::Projection::Event current) noexcept {
    static_cast<void>(previous); // Catalog 每次增量都是完整正文, 不需要 正文 基线传播.
    return current;
}

Catalog::Edition::Edition(std::uint64_t version, std::vector<State::Projection::Event> changes, std::string_view target) : data_(std::in_place_type<std::vector<State::Projection::Event>>), mode_(proto::comet::v1::MODE_APPLY), version_(version) {

    // 原生状态/活动收集器已经确认连续覆盖. 此处不能用是否存在相关变化反推完整游标.
    // 先按目标过滤, 再按 Key/游标排序, 合并只保留每个 Key 的最后完整内容/删除.
    for (const auto& change : changes) {
        if (!change.name || change.version == 0 || change.version > version || (change.data && !change.record)) {
            throw std::invalid_argument("Invalid Catalog change batch");
        }
    }
    if (!target.empty()) {
        std::erase_if(changes, [&](const auto& change) { return change.name->key != target; });
    }
    const auto order = [](const auto& left, const auto& right) { return left.name->key < right.name->key || (left.name->key == right.name->key && left.version < right.version); }; // 同 Key 仍按版本选择最后一次提交.
    if (!std::ranges::is_sorted(changes, order)) {
        std::ranges::sort(changes, order); // 活动收集器的有序唯一后缀跳过排序, 历史回放仍允许交错 Key.
    }
    std::size_t kept{}; // 合并后的有效前缀, 不另建 HashMap 或复制 Buffer.
    for (std::size_t first = 0; first < changes.size();) {
        auto& latest = changes[first]; // 在本组首项原地合并, 保留最高游标, 不增加共享引用计数.
        auto end = first + 1;
        while (end < changes.size() && changes[end].name->key == latest.name->key) {
            latest = merge(latest, std::move(changes[end]));
            ++end;
        }
        bytes_ += latest.bytes;
        if (kept != first) {
            changes[kept] = std::move(latest); // 只有真正压缩前缀时移动, 禁止向自身移动而清空记录.
        }
        ++kept;
        first = end;
    }
    bytes_ += (changes.capacity() - kept) * sizeof(State::Projection::Event); // 不假装 resize 收回 vector 容量.
    changes.resize(kept);
    data_.emplace<std::vector<State::Projection::Event>>(std::move(changes));
}

bool Catalog::Edition::append(proto::comet::v1::CatalogWatchReply& page, std::size_t& bytes, std::string_view key, const State::Content* record, bool data) {

    if (!Scope::text(key, 1024) || (record && (!record->value || record->version == 0))) {
        throw std::invalid_argument("Invalid Catalog projection record");
    }
    const auto payload = record ? record->value->size() : 0; // 期限不编码到公共视图.
    const auto cost = key.size() + payload + 48;             // 单条原生载荷受硬上限保护, 保守包含嵌套/字段长度开销.
    if (cost > 8 * 1024 * 1024 - bytes) {
        throw std::length_error("Catalog record exceeds page budget");
    }
    if (page.changes_size() != 0 && (page.changes_size() >= 256 || cost > 256 * 1024 - std::min(bytes, std::size_t{256 * 1024}))) {
        return false;
    }
    auto* change = page.add_changes();
    change->set_key(key);
    if (!record) {
        change->mutable_erase();
    } else {
        change->set_version(record->version);
        change->set_value(record->value->data(), record->value->size());
    }
    static_cast<void>(data); // 共用 Scene 的提示不改变 Catalog 完整正文编码.
    bytes += cost;
    return true;
}

proto::comet::v1::CatalogWatchReply Catalog::Edition::next(std::string_view instance) {

    if (complete_) {
        throw std::logic_error("Catalog edition already completed");
    }
    if (instance.empty() || instance.size() > 128) {
        throw std::invalid_argument("Invalid serving instance");
    }
    proto::comet::v1::CatalogWatchReply page; // 整页成功构造后才推进位置, 不复用上一页的可写消息.
    page.set_mode(mode_);
    std::size_t bytes = instance.size() + 64; // 每页预留末页身份/游标字段, 不事后突破硬上限.
    std::size_t consumed{};                   // 本页成功消费数, 失败时不更新 offset_.
    bool finished = true;                     // 空范围/精确缺项/无关目标增量也需要 complete 帧.
    if (const auto* view = std::get_if<State::Projection::View>(&data_)) {
        consumed = view->page(offset_, 256, [&](const std::string& key, const State::Content& record) { return append(page, bytes, key, &record, false); });
        finished = offset_ + consumed == view->size();
    } else if (const auto* point = std::get_if<Point>(&data_)) {
        if (point->record) {
            static_cast<void>(append(page, bytes, point->key, &*point->record, false));
            consumed = 1;
        }
    } else {
        const auto& changes = std::get<std::vector<State::Projection::Event>>(data_);
        while (offset_ + consumed < changes.size()) {
            const auto& change = changes[offset_ + consumed]; // 固定的最终 action, 不查询当前表的较新 正文.
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

bool Catalog::Edition::complete() const noexcept {
    return complete_;
}

std::uint64_t Catalog::Edition::version() const noexcept {
    return version_;
}

std::size_t Catalog::Edition::bytes() const noexcept {
    return bytes_;
}
} // namespace astra
