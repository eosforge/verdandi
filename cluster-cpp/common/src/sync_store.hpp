// 功能: 在同一提交边界内维护当前状态, 有界增量历史和按版本复用的只读快照.
#pragma once

#include <verdandi/cluster/types.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace verdandi::cluster {

// 增量属于单个 Store 的提交序列, 不代表 Catalog 发布者的业务版本.
struct DeltaRecord {
    std::string key;
    std::shared_ptr<const std::vector<std::uint8_t>> payload;
    bool deleted{};
    std::uint64_t field_version{};
};

// require_snapshot 为 true 时 deltas 为空. 否则包含到 current_version 为止的全部变更.
struct SyncResult {
    bool require_snapshot{};
    std::uint64_t current_version{};
    std::vector<DeltaRecord> deltas;
};

// 内容和版本由同一临界区取得. Map 和 Key 需要复制, 仅 payload 与 Store 共享.
struct Snapshot {
    std::uint64_t global_version{};
    std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> data;
};

// 内部存储工具, 尚不提供业务鉴权, Catalog CAS, 持久化或网络订阅.
// 每个实例有独立版本空间. 同一把锁保护状态, 历史和快照, 调用方不在锁内收到回调.
class SyncStore {
public:
    // 限制保留的完整提交批次数, 不代表字节预算. 0 表示不保留历史, 落后客户端必须取快照.
    explicit SyncStore(std::size_t max_history_batches = 1000) : max_history_batches_(max_history_batches) {}

    // 接管 payload 值; 移入后调用方不得继续使用指向原缓冲的可写指针或引用.
    // expire 为单调时间租约截止, max 表示不自动到期. 分配失败或版本耗尽时抛异常且状态不变.
    void put(const std::string& key, std::vector<std::uint8_t> payload, Clock::time_point expire = Clock::time_point::max());
    // 不存在或已经删除时无操作. 成功时产生单条删除增量; 失败时状态和版本不变.
    void remove(const std::string& key);
    // 将截止 <= now 的有效项作为同一个批次删除. 分配失败向调用方传播, 由调用方决定重试.
    void evict_expired(Clock::time_point now);
    // 取得本实例已完整提交的版本, 不能据此推断另一个实例或订阅范围的同步进度.
    std::uint64_t global_version() const;
    // 同版本复用同一个只读快照. 创建失败保留旧缓存; 已返回的快照不受后续提交影响.
    std::shared_ptr<const Snapshot> get_snapshot();
    // 返回 since_version 之后的完整增量. 历史不足或游标超前时明确要求快照.
    // 调用前必须在上层核对实例和订阅范围. 本接口只支持同实例的全范围历史.
    SyncResult extract_since(std::uint64_t since_version) const;

private:
    // 当前值的版本与历史中的删除版本共同决定墓碑能否安全清理.
    struct StoreEntry {
        std::shared_ptr<const std::vector<std::uint8_t>> payload;
        std::uint64_t field_version{};
        bool deleted{};
        Clock::time_point lease_expire{Clock::time_point::max()};
    };
    // 一次过期清理可有多个 Key, 必须整批保留和淘汰, 不允许截断批次内部记录.
    struct VersionBatch {
        std::uint64_t global_version;
        std::vector<DeltaRecord> records;
    };
    // 以下辅助函数均要求调用方持有 mutex_. 版本耗尽时不允许回绕.
    std::uint64_t next_version_locked() const;
    // 清理与已淘汰删除记录仍然匹配的墓碑, 绝不删除其后重新写入的条目.
    void gc_tombstones_locked();
    // 先在临时对象上完成全部分配, 成功后由调用方替换缓存.
    std::shared_ptr<const Snapshot> create_snapshot_locked() const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StoreEntry> entries_;
    std::uint64_t global_version_{};
    std::size_t max_history_batches_;
    std::deque<VersionBatch> history_;
    // 已淘汰的最后一个完整版本; 此版本的客户端仍可从其下一条增量续传.
    std::uint64_t oldest_version_{};
    std::shared_ptr<const Snapshot> active_snapshot_;
};

} // namespace verdandi::cluster
