#include "edition.hpp"
#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <tuple>

namespace astra {
Edition::Edition() = default;

Edition::Edition(Almanac::View view) : version_(view.version()), bytes_(view.bytes()) {
    data_.emplace<Almanac::View>(std::move(view));
}

Edition::Edition(std::string key, Almanac::Point point) : version_(point.version), bytes_(point.value ? key.size() + point.value->size() : 0) {
    data_.emplace<Point>(std::move(key), std::move(point.value));
}

Edition::Edition(Almanac::Replay replay, std::string_view target) : mode_(proto::comet::v1::MODE_APPLY), version_(replay.version) {

    // 精确 Watch 只保留最后一次相关变更, 空投影仍需确认整个冻结覆盖区间已经消费.
    if (!target.empty()) {
        const auto last = std::ranges::find_if(replay.changes | std::views::reverse, [target](const Almanac::Change& change) { return *change.key == target; });
        if (last != replay.changes.rend()) {
            bytes_ = last->bytes();
            data_.emplace<std::vector<Almanac::Change>>(1, *last);
        }
        return;
    }

    // 连续历史已在原生状态中确认; 按 Key/版本排序后原位保留最终 action, 不另建散列表或复制 Buffer.
    const auto order = [](const Almanac::Change& change) { return std::tie(*change.key, change.version); }; // 引用投影按 Key/版本比较, 不复制字符串, 不对相等 Key 再作一次独立等号比较.
    if (!std::ranges::is_sorted(replay.changes, {}, order)) {
        std::ranges::sort(replay.changes, {}, order); // 活动收集器的有序唯一后缀跳过排序, 历史回放仍允许交错 Key.
    }
    std::size_t kept{}; // 压缩后有效前缀长度, 原顺序只用于选择同 Key 的最高版本.
    for (std::size_t first = 0; first < replay.changes.size();) {
        auto last = first; // 本次相同 Key 的最后一项, 原生历史保证版本递增且无重复版本.
        while (last + 1 < replay.changes.size() && *replay.changes[last + 1].key == *replay.changes[first].key) {
            ++last;
        }
        bytes_ += replay.changes[last].bytes();
        if (kept != last) {
            replay.changes[kept] = std::move(replay.changes[last]);
        }
        ++kept;
        first = last + 1;
    }
    bytes_ += (replay.changes.capacity() - kept) * sizeof(Almanac::Change);
    replay.changes.resize(kept);
    data_.emplace<std::vector<Almanac::Change>>(std::move(replay.changes));
}

bool Edition::append(proto::comet::v1::AlmanacWatchReply& page, std::size_t& bytes, std::string_view key, const Almanac::Value& value) {

    // cost 保守计入嵌套字段/长度编码, 不对整页反复 ByteSizeLong, 避免二次方遍历.
    const auto cost = key.size() + (value ? value->size() : 0) + 32;
    if (cost > 8 * 1024 * 1024 - bytes) {
        throw std::length_error("Almanac record exceeds message budget");
    }
    if (page.changes_size() != 0 && (page.changes_size() >= 256 || cost > 256 * 1024 - std::min(bytes, std::size_t{256 * 1024}))) {
        return false;
    }
    auto* change = page.add_changes(); // 当前页独占可写消息, 不影响固定旧根或其他订阅.
    change->set_key(key);
    if (value) {
        change->set_value(value->data(), value->size());
    } else {
        change->mutable_erase();
    }
    bytes += cost;
    return true;
}

proto::comet::v1::AlmanacWatchReply Edition::next(std::string_view instance) {

    if (complete_) {
        throw std::logic_error("Almanac edition already completed");
    }
    if (instance.empty() || instance.size() > 128) {
        throw std::invalid_argument("Invalid Almanac serving instance");
    }
    proto::comet::v1::AlmanacWatchReply page; // 本次返回一个拥有式页, 不与下一次编码共用可写内容.
    page.set_mode(mode_);
    std::size_t bytes = instance.size() + 64; // 即使非末页也预留最终控制字段的最坏空间.
    std::size_t count{};                      // 只有整页构造成功才累计到 offset_, 异常保留原位置.
    bool finished = true;                     // 空基线和空 apply 也产生一个明确 complete 帧.
    if (const auto* view = std::get_if<Almanac::View>(&data_)) {
        count = view->page(offset_, 256, [&](const std::string& key, const Almanac::Value& value) { return append(page, bytes, key, value); });
        finished = offset_ + count == view->size();
    } else if (const auto* point = std::get_if<Point>(&data_)) {
        if (point->value) {
            static_cast<void>(append(page, bytes, point->key, point->value));
            count = 1;
        }
    } else if (const auto* changes = std::get_if<std::vector<Almanac::Change>>(&data_)) {
        while (offset_ + count < changes->size()) {
            const auto& change = (*changes)[offset_ + count]; // 借用冻结最终 action, 不向当前分组回查数据.
            if (!append(page, bytes, *change.key, change.value)) {
                break;
            }
            ++count;
        }
        finished = offset_ + count == changes->size();
    }
    if (finished) {
        page.set_complete(true);
        page.set_version(version_);
        page.set_instance(instance);
    }
    offset_ += count;
    complete_ = finished;
    return page;
}

bool Edition::complete() const noexcept {
    return complete_;
}

std::uint64_t Edition::version() const noexcept {
    return version_;
}

std::size_t Edition::bytes() const noexcept {
    return bytes_;
}
} // namespace astra
