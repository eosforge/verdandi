#include "ephemeris_edition.hpp"
#include <ranges>

namespace astra {
Ephemeris::Edition::Edition(State::Projection::View view) : data_(view), version_(view.version()), bytes_(view.bytes()) {}

Ephemeris::Edition::Edition(std::string uuid, State::Projection::Point point) : data_(Point{std::move(uuid), point.record}), version_(point.version) {
    const auto& value = std::get<Point>(data_); // 点查只计自身名称与完整载荷, 不捕获整 Scope.
    bytes_ = sizeof(Point) + value.uuid.size() + (value.record ? value.record->attr->size() + value.record->data->size() : 0);
}

Ephemeris::State::Projection::Event Ephemeris::Edition::merge(const State::Projection::Event& previous, State::Projection::Event current) noexcept {
    if (current.record && (!previous.record || !previous.data)) {
        current.data = false;
    }
    return current;
}

Ephemeris::Edition::Edition(std::uint64_t version, std::vector<State::Projection::Event> changes, std::string_view target) : data_(std::in_place_type<std::vector<State::Projection::Event>>), mode_(proto::comet::v1::MODE_APPLY), version_(version) {

    // 原生状态/活动收集器已经确认连续覆盖. 此处不能用是否存在相关变化反推完整游标.
    // 先按目标过滤, 再按 UUID/游标排序, 合并保留 Create/恢复 Record 的 Attr 依据.
    for (const auto& change : changes) {
        if (!change.name || change.version == 0 || change.version > version || (change.data && !change.record)) {
            throw std::invalid_argument("Invalid Ephemeris change batch");
        }
    }
    if (!target.empty()) {
        std::erase_if(changes, [&](const auto& change) { return change.name->key != target; });
    }
    const auto order = [](const auto& left, const auto& right) { return left.name->key < right.name->key || (left.name->key == right.name->key && left.version < right.version); }; // 同 UUID 的 Attr 基线必须先于后续 Data 合并.
    if (!std::ranges::is_sorted(changes, order)) {
        std::ranges::sort(changes, order); // 活动收集器的有序唯一后缀跳过排序, 历史回放仍允许交错 UUID.
    }
    std::size_t kept{}; // 合并后的有效前缀, 不另建 HashMap 或复制 Buffer.
    for (std::size_t first = 0; first < changes.size();) {
        auto& latest = changes[first]; // 在本组首项原地合并, 保留完整性依据, 不增加共享引用计数.
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

bool Ephemeris::Edition::append(proto::comet::v1::EphemerisWatchReply& page, std::size_t& bytes, std::string_view uuid, const State::Content* record, bool data) {

    if (!Ephemeris::valid(uuid) || (record && (!record->attr || !record->data))) {
        throw std::invalid_argument("Invalid Ephemeris projection record");
    }
    const auto payload = record ? record->data->size() + (data ? 0 : record->attr->size()) : 0; // Data-only 不读取/序列化 Attr.
    const auto cost = uuid.size() + payload + 48;                                               // 单条原生载荷受硬上限保护, 保守包含嵌套/字段长度开销.
    if (cost > 8 * 1024 * 1024 - bytes) {
        throw std::length_error("Ephemeris record exceeds page budget");
    }
    if (page.changes_size() != 0 && (page.changes_size() >= 256 || cost > 256 * 1024 - std::min(bytes, std::size_t{256 * 1024}))) {
        return false;
    }
    auto* change = page.add_changes();
    change->set_uuid(uuid);
    if (!record) {
        change->mutable_erase();
    } else if (data) {
        change->set_data(record->data->data(), record->data->size());
    } else {
        auto* complete = change->mutable_record();
        complete->set_attr(record->attr->data(), record->attr->size());
        complete->set_data(record->data->data(), record->data->size());
    }
    bytes += cost;
    return true;
}

proto::comet::v1::EphemerisWatchReply Ephemeris::Edition::next(std::string_view instance) {

    if (complete_) {
        throw std::logic_error("Ephemeris edition already completed");
    }
    if (instance.empty() || instance.size() > 128) {
        throw std::invalid_argument("Invalid serving instance");
    }
    proto::comet::v1::EphemerisWatchReply page; // 整页成功构造后才推进位置, 不复用上一页的可写消息.
    page.set_mode(mode_);
    std::size_t bytes = instance.size() + 64; // 每页预留末页身份/游标字段, 不事后突破硬上限.
    std::size_t consumed{};                   // 本页成功消费数, 失败时不更新 offset_.
    bool finished = true;                     // 空范围/精确缺项/无关目标增量也需要 complete 帧.
    if (const auto* view = std::get_if<State::Projection::View>(&data_)) {
        consumed = view->page(offset_, 256, [&](const std::string& uuid, const State::Content& record) { return append(page, bytes, uuid, &record, false); });
        finished = offset_ + consumed == view->size();
    } else if (const auto* point = std::get_if<Point>(&data_)) {
        if (point->record) {
            static_cast<void>(append(page, bytes, point->uuid, &*point->record, false));
            consumed = 1;
        }
    } else {
        const auto& changes = std::get<std::vector<State::Projection::Event>>(data_);
        while (offset_ + consumed < changes.size()) {
            const auto& change = changes[offset_ + consumed]; // 固定的最终 action, 不查询当前表的较新 Attr/Data.
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

bool Ephemeris::Edition::complete() const noexcept {
    return complete_;
}

std::uint64_t Ephemeris::Edition::version() const noexcept {
    return version_;
}

std::size_t Ephemeris::Edition::bytes() const noexcept {
    return bytes_;
}
} // namespace astra
