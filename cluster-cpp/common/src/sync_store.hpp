#pragma once

#include "verdandi/cluster/types.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace verdandi::cluster {

struct StoreEntry {
    std::shared_ptr<const std::vector<std::uint8_t>> payload;
    std::uint64_t field_version{};
    bool deleted{false};
    Clock::time_point lease_expire{Clock::time_point::max()};
};

struct DeltaRecord {
    std::string key;
    std::shared_ptr<const std::vector<std::uint8_t>> payload;
    bool deleted;
    std::uint64_t field_version;
};

struct VersionBatch {
    std::uint64_t global_version;
    std::vector<DeltaRecord> records;
};

struct SyncResult {
    bool require_snapshot;
    std::uint64_t current_version;
    std::vector<DeltaRecord> deltas; // Flattened deltas
};

struct Snapshot {
    std::uint64_t global_version;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> data;
};

class SyncStore {
public:
    explicit SyncStore(std::size_t max_history_batches = 1000) : max_history_batches_(max_history_batches) {}

    // 值传递 payload，内部接管所有权并固化为 const，杜绝外部修改
    void put(const std::string& key, std::vector<std::uint8_t> payload, Clock::time_point expire = Clock::time_point::max());

    void remove(const std::string& key);

    void evict_expired(Clock::time_point now);

    std::uint64_t global_version() const;

    // 获取零拷贝快照缓存。内置 SnapshotCache 逻辑。
    std::shared_ptr<const Snapshot> get_snapshot();

    SyncResult extract_since(std::uint64_t since_version) const;

private:
    void gc_tombstones_locked();
    std::shared_ptr<const Snapshot> create_snapshot_locked() const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StoreEntry> entries_;
    std::uint64_t global_version_{0};

    std::size_t max_history_batches_;
    std::deque<VersionBatch> history_;
    std::uint64_t oldest_version_{0}; 

    std::shared_ptr<const Snapshot> active_snapshot_;
};

} // namespace verdandi::cluster
