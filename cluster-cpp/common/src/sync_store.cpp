// 功能: 先准备所有可能失败的分配, 再提交状态和历史, 保证失败不暴露部分修改.
#include "sync_store.hpp"

#include <limits>
#include <stdexcept>

namespace verdandi::cluster {

void SyncStore::put(const std::string& key, std::vector<std::uint8_t> payload, Clock::time_point expire) {
    auto const_payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload));
    std::lock_guard lock(mutex_);
    const auto version = next_version_locked();
    auto [it, inserted] = entries_.try_emplace(key);

    // 临时插入仍被锁遮蔽. 记录或历史分配失败时只撤销本次新增项, 不改动原有条目.
    try {
        std::vector<DeltaRecord> records;
        records.push_back({key, const_payload, false, version});
        history_.push_back({version, std::move(records)});
    } catch (...) {
        if (inserted) {
            entries_.erase(it);
        }
        throw;
    }

    // 历史已经准备完毕. 后续赋值不分配内存, 外部读者只能看到完整提交前或提交后的状态.
    it->second = {std::move(const_payload), version, false, expire};
    global_version_ = version;
    gc_tombstones_locked();
}

void SyncStore::remove(const std::string& key) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.deleted) {
        return;
    }
    const auto version = next_version_locked();
    std::vector<DeltaRecord> records;
    records.push_back({key, nullptr, true, version});
    history_.push_back({version, std::move(records)});

    // 所有可能抛出的记录分配先完成, 然后才删除当前值.
    it->second = {nullptr, version, true, Clock::time_point::max()};
    global_version_ = version;
    gc_tombstones_locked();
}

void SyncStore::evict_expired(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    std::vector<DeltaRecord> records;
    // 只准备 Key 列表, 不一边收集一边删除. 字符串或数组扩容失败也不留下部分过期结果.
    for (const auto& [key, entry] : entries_) {
        if (!entry.deleted && entry.lease_expire <= now) {
            records.push_back({key, nullptr, true, 0});
        }
    }
    if (records.empty()) {
        return;
    }
    const auto version = next_version_locked();
    for (auto& record : records) {
        record.field_version = version;
    }
    history_.push_back({version, std::move(records)});

    // 同一把锁内 Key 不会消失. 直接按准备好的批次提交, 不再扫描整个 Map.
    for (const auto& record : history_.back().records) {
        entries_.find(record.key)->second = {nullptr, version, true, Clock::time_point::max()};
    }
    global_version_ = version;
    gc_tombstones_locked();
}

std::uint64_t SyncStore::global_version() const {
    std::lock_guard lock(mutex_);
    return global_version_;
}

std::shared_ptr<const Snapshot> SyncStore::get_snapshot() {
    std::lock_guard lock(mutex_);
    if (!active_snapshot_ || active_snapshot_->global_version != global_version_) {
        active_snapshot_ = create_snapshot_locked();
    }
    return active_snapshot_;
}

std::shared_ptr<const Snapshot> SyncStore::create_snapshot_locked() const {
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->global_version = global_version_;
    snapshot->data.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        if (!entry.deleted) {
            snapshot->data.emplace(key, entry.payload);
        }
    }
    return snapshot;
}

SyncResult SyncStore::extract_since(std::uint64_t since_version) const {
    std::lock_guard lock(mutex_);
    SyncResult result{.require_snapshot = since_version > global_version_ || since_version < oldest_version_, .current_version = global_version_, .deltas = {}};
    if (result.require_snapshot || since_version == global_version_) {
        return result;
    }
    // 当前返回完整范围. 最后的 current_version 是全部记录的提交屏障, 不能在中途提前确认.
    for (const auto& batch : history_) {
        if (batch.global_version > since_version) {
            result.deltas.insert(result.deltas.end(), batch.records.begin(), batch.records.end());
        }
    }
    return result;
}

std::uint64_t SyncStore::next_version_locked() const {
    if (global_version_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Store version exhausted");
    }
    return global_version_ + 1;
}

void SyncStore::gc_tombstones_locked() {
    while (history_.size() > max_history_batches_) {
        const auto& batch = history_.front();
        for (const auto& record : batch.records) {
            if (record.deleted) {
                auto it = entries_.find(record.key);
                if (it != entries_.end() && it->second.field_version == record.field_version && it->second.deleted) {
                    entries_.erase(it);
                }
            }
        }
        oldest_version_ = batch.global_version;
        history_.pop_front();
    }
}

} // namespace verdandi::cluster
