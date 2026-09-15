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
    // 本次变更的完整 Key. 记录独立拥有字符串, 历史淘汰后返回结果仍然有效.
    std::string key;
    // 写入时共享不可变载荷, 删除时为空. Key 和容器会复制, payload 字节不会复制.
    std::shared_ptr<const std::vector<std::uint8_t>> payload;
    // true 表示删除指令, 空 payload 的正常值仍以 false 区分.
    bool deleted{};
    // 所属原子批次的本地提交序号. 同批多 Key 的值相等, 不表示 Catalog 业务版本.
    std::uint64_t field_version{};
};

// require_snapshot 为 true 时 deltas 为空. 否则包含到 current_version 为止的全部变更.
struct SyncResult {
    // 历史不足或游标超前时为 true; 此时不允许把空 deltas 当作已经同步.
    bool require_snapshot{};
    // 取得锁时最后完成的提交. 只有应用全部 deltas 后才能确认到这个位置.
    std::uint64_t current_version{};
    // 按提交顺序排列的完整批次. 同一过期批次内 Key 顺序未定义, 不影响原子边界.
    std::vector<DeltaRecord> deltas;
};

// 内容和版本由同一临界区取得. Map 和 Key 需要复制, 仅 payload 与 Store 共享.
struct Snapshot {
    // 与 data 在同一临界区取得的本地提交序号.
    std::uint64_t global_version{};
    // 仅包含当前存在的 Key. 已返回快照持有自己的 Map, 与后续写入隔离.
    std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> data;
};

// 内部存储工具, 尚不提供业务鉴权, Catalog CAS, 持久化或网络订阅.
// 每个实例有独立版本空间. 同一把锁保护状态, 历史和快照, 调用方不在锁内收到回调.
class SyncStore {
public:
    // max_history_batches 限制保留的完整提交批次数, 不代表字节预算. 0 表示不保留历史.
    // 创建独立的零版本实例, 尚未创建快照. deque 初始化可能分配内存并抛出 bad_alloc.
    explicit SyncStore(std::size_t max_history_batches = 1000) : max_history_batches_(max_history_batches) {}

    // 向 key 写入或覆盖 payload, 即使内容相同也提交一个新版本; 无返回值.
    // 接管 payload 值; 移入后调用方不得继续使用指向原缓冲的可写指针或引用.
    // expire 为单调时间租约截止, max 表示不自动到期. 分配失败或版本耗尽时抛异常且状态不变.
    void put(const std::string& key, std::vector<std::uint8_t> payload, Clock::time_point expire = Clock::time_point::max());
    // 删除 key, 无返回值. 不存在或已经删除时不推进版本; 当前值与单条删除历史同时提交.
    // 分配失败或版本耗尽向调用方抛异常, 当前状态和版本不变.
    void remove(const std::string& key);
    // 将有限截止 <= now 的条目作为同一批次删除, 无返回值. now 通常为 Clock::now().
    // max 截止始终表示永不过期, 即使 now 为 max. 无到期项时不推进版本.
    // 分配失败或版本耗尽向调用方传播, 整批保持原状, 由调用方决定重试.
    void evict_expired(Clock::time_point now);
    // 加锁返回本实例已完整提交的版本. 不分配内存, 不能据此推断其他实例的同步进度.
    std::uint64_t global_version() const;
    // 加锁返回当前只读快照, 同版本复用缓存. 首次构建复制 Key/Map, 仅共享 payload.
    // 创建失败抛出分配异常并保留旧缓存; 已返回的快照不受后续提交影响.
    std::shared_ptr<const Snapshot> get_snapshot();
    // 返回 since_version 之后的完整增量. 历史不足或游标超前时明确要求快照.
    // 调用前必须在上层核对实例和订阅范围. 本接口只支持同实例的全范围历史.
    // 分配失败抛异常且 Store 不变. 结果独立拥有 Key 和容器, 不能在拿到全部结果前确认游标.
    SyncResult extract_since(std::uint64_t since_version) const;

private:
    // payload 为空时缓存已删除的 Map 节点, 供同 Key 重建复用, 无需独立 deleted 标志.
    // 删除指令由历史持有; 保留节点版本是为了避免旧记录淘汰时过早回收新近删除的缓存.
    struct StoreEntry {
        // 非空表示当前有效值, 载荷允许零字节; 空表示可复用的已删除节点.
        std::shared_ptr<const std::vector<std::uint8_t>> payload;
        // 本节点最后一次写入/删除的本地序号. 回收空节点时匹配对应历史, 保留同 Key 的快速重建路径.
        std::uint64_t field_version{};
        // 基于单调时钟的到期时刻. max 为明确的永不过期哨兵.
        Clock::time_point lease_expire{Clock::time_point::max()};
    };
    // 一次过期清理可有多个 Key, 必须整批保留和淘汰, 不允许截断批次内部记录.
    struct VersionBatch {
        // 与前一批次相差 1 的本地提交序号, 失败或无操作不产生批次.
        std::uint64_t global_version;
        // 同次提交的完整记录, 发布后不再改动; 一次过期清理可能有多条.
        std::vector<DeltaRecord> records;
    };
    // 调用方必须持有 mutex_. 返回下一提交序号而不改动状态; 耗尽时抛 overflow_error.
    std::uint64_t next_version_locked() const;
    // 调用方必须持有 mutex_. 淘汰完整历史批次及可丢弃的空节点, 推进续传下界, 不分配内存.
    void trim_history_locked();
    // 调用方必须持有 mutex_. 在临时对象中构建完整快照, 返回前不替换缓存; 分配失败抛异常.
    std::shared_ptr<const Snapshot> create_snapshot_locked() const;

    // 唯一状态锁. mutable 允许只读查询与写入共享相同的同步边界.
    mutable std::mutex mutex_;
    // 当前 Key 索引及短期空节点缓存. 空节点只为避免同 Key 重建反复分配, 不出现在快照中.
    std::unordered_map<std::string, StoreEntry> entries_;
    // 最后完整提交的本地序号, 初始为 0, 达到 uint64_t 上限后拒绝新提交.
    std::uint64_t global_version_{};
    // 构造后不改变的历史批次容量. 不是记录数或内存字节上限.
    std::size_t max_history_batches_;
    // 按提交号递增的完整批次. deque 支持头部淘汰和随机访问二分查找.
    std::deque<VersionBatch> history_;
    // 已淘汰的最后一个完整版本; 此版本的客户端仍可从其下一条增量续传.
    std::uint64_t oldest_version_{};
    // 最近成功构建的不可变快照. 可能落后当前版本, 但只在匹配版本时复用.
    std::shared_ptr<const Snapshot> active_snapshot_;
};

} // namespace verdandi::cluster
