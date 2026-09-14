#include "sync_store.hpp"

namespace verdandi::cluster {

void SyncStore::put(const std::string& key, std::vector<std::uint8_t> payload, Clock::time_point expire) {
    auto const_payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload));
    std::lock_guard lock(mutex_);
    
    std::uint64_t new_version = global_version_ + 1;
    auto [it, inserted] = entries_.try_emplace(key, StoreEntry{});
    
    try {
        std::vector<DeltaRecord> records;
        records.reserve(1);
        records.push_back({key, const_payload, false, new_version});
        history_.push_back({new_version, std::move(records)});
    } catch (...) {
        if (inserted) {
            entries_.erase(it);
        }
        throw;
    }
    
    global_version_ = new_version;
    it->second.payload = const_payload;
    it->second.field_version = new_version;
    it->second.deleted = false;
    it->second.lease_expire = expire;
    
    gc_tombstones_locked();
}

void SyncStore::remove(const std::string& key) {
    std::lock_guard lock(mutex_);
    
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.deleted) {
        return;
    }
    
    std::uint64_t new_version = global_version_ + 1;
    
    try {
        std::vector<DeltaRecord> records;
        records.reserve(1);
        records.push_back({key, nullptr, true, new_version});
        history_.push_back({new_version, std::move(records)});
    } catch (...) {
        throw;
    }
    
    global_version_ = new_version;
    it->second.payload.reset();
    it->second.field_version = new_version;
    it->second.deleted = true;
    it->second.lease_expire = Clock::time_point::max();
    
    gc_tombstones_locked();
}

void SyncStore::evict_expired(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    
    std::size_t expired_count = 0;
    for (const auto& [key, entry] : entries_) {
        if (!entry.deleted && entry.lease_expire <= now) {
            expired_count++;
        }
    }
    if (expired_count == 0) return;
    
    std::uint64_t new_version = global_version_ + 1;
    
    try {
        std::vector<DeltaRecord> records;
        records.reserve(expired_count);
        for (auto& [key, entry] : entries_) {
            if (!entry.deleted && entry.lease_expire <= now) {
                records.push_back({key, nullptr, true, new_version});
            }
        }
        history_.push_back({new_version, std::move(records)});
    } catch (...) {
        throw;
    }
    
    global_version_ = new_version;
    for (auto& [key, entry] : entries_) {
        if (!entry.deleted && entry.lease_expire <= now) {
            entry.payload.reset();
            entry.field_version = new_version;
            entry.deleted = true;
            entry.lease_expire = Clock::time_point::max();
        }
    }
    
    gc_tombstones_locked();
}

std::uint64_t SyncStore::global_version() const {
    std::lock_guard lock(mutex_);
    return global_version_;
}

std::shared_ptr<const Snapshot> SyncStore::get_snapshot() {
    std::lock_guard lock(mutex_);
    
    if (active_snapshot_ && active_snapshot_->global_version == global_version_) {
        return active_snapshot_;
    }
    
    active_snapshot_ = create_snapshot_locked();
    return active_snapshot_;
}

std::shared_ptr<const Snapshot> SyncStore::create_snapshot_locked() const {
    auto snap = std::make_shared<Snapshot>();
    snap->global_version = global_version_;
    snap->data.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        if (!entry.deleted) {
            snap->data.emplace(key, entry.payload);
        }
    }
    return snap;
}

SyncResult SyncStore::extract_since(std::uint64_t since_version) const {
    std::lock_guard lock(mutex_);
    
    SyncResult result;
    result.current_version = global_version_;
    
    if (since_version > global_version_) {
        result.require_snapshot = true;
        return result;
    }
    
    if (since_version == global_version_) {
        result.require_snapshot = false;
        return result;
    }
    
    if (since_version < oldest_version_) {
        result.require_snapshot = true;
        return result;
    }
    
    result.require_snapshot = false;
    for (const auto& batch : history_) {
        if (batch.global_version > since_version) {
            for (const auto& record : batch.records) {
                result.deltas.push_back(record);
            }
        }
    }
    
    return result;
}

void SyncStore::gc_tombstones_locked() {
    while (history_.size() > max_history_batches_) {
        const auto& evicted_batch = history_.front();
        
        for (const auto& record : evicted_batch.records) {
            if (record.deleted) {
                auto it = entries_.find(record.key);
                if (it != entries_.end() && it->second.field_version == record.field_version && it->second.deleted) {
                    entries_.erase(it);
                }
            }
        }
        
        oldest_version_ = evicted_batch.global_version;
        history_.pop_front();
    }
}

} // namespace verdandi::cluster
