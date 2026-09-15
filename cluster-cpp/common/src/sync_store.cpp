// 功能: 先准备所有可能失败的分配, 再提交状态和历史, 保证失败不暴露部分修改.
#include "sync_store.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace verdandi::cluster {

void SyncStore::put(const std::string& key, std::vector<std::uint8_t> payload, Clock::time_point expire) {
    // 在锁外接管缓冲并准备唯一记录, 将可提前完成的 Key 复制和容器分配移出临界区.
    auto const_payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(payload));
    // 提交序号尚未确定, 0 只存在于未发布的临时记录中.
    std::vector<DeltaRecord> records;
    records.emplace_back(key, const_payload, false, 0);
    // lock 从此处到函数退出串行化 Map, 历史和版本; 异常时自动释放.
    std::lock_guard lock(mutex_);
    // 只计算而不发布下一版本, 先拒绝耗尽以免产生回绕.
    const auto version = next_version_locked();
    records.front().field_version = version;
    // it 指向新增或已有条目; inserted 仅用于撤销尚未完成的新键插入.
    auto [it, inserted] = entries_.try_emplace(key);

    // 临时插入仍被锁遮蔽. 记录或历史分配失败时只撤销本次新增项, 不改动原有条目.
    try {
        history_.push_back({version, std::move(records)});
    } catch (...) {
        if (inserted) {
            entries_.erase(it);
        }
        throw;
    }

    // 历史已经准备完毕. 后续赋值不分配内存, 外部读者只能看到完整提交前或提交后的状态.
    it->second = {std::move(const_payload), version, expire};
    global_version_ = version;
    trim_history_locked();
}

void SyncStore::remove(const std::string& key) {
    // lock 同时保护存在性判断与删除提交, 不用先解锁再重复查找.
    std::lock_guard lock(mutex_);
    // it 指向当前节点. 不存在或 payload 已为空时均无操作, 不额外维护删除标志.
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.payload) {
        return;
    }
    // version 在历史成功追加之前不对外可见.
    const auto version = next_version_locked();
    // records 独立持有删除指令, 节点中的版本仅用于空节点缓存的回收匹配.
    std::vector<DeltaRecord> records;
    records.emplace_back(key, nullptr, true, version);
    history_.push_back({version, std::move(records)});

    // 释放当前载荷但复用节点, 避免同 Key 删除/重建反复分配. 历史淘汰时再回收空节点.
    it->second = {nullptr, version, Clock::time_point::max()};
    global_version_ = version;
    trim_history_locked();
}

void SyncStore::evict_expired(Clock::time_point now) {
    // lock 遮蔽整批删除的中间过程, 快照读者只会看到提交前或提交后的完整状态.
    std::lock_guard lock(mutex_);
    // records 在扫描中收集到期 Key; 准备失败时临时对象自行销毁, Map 保持原状.
    std::vector<DeltaRecord> records;
    // 只准备 Key 列表, 不一边收集一边删除. 字符串或数组扩容失败也不留下部分过期结果.
    for (const auto& [key, entry] : entries_) {
        // key/entry 借用当前 Map 节点. 永不过期哨兵必须独立判断, 不能视为有限截止.
        if (entry.lease_expire != Clock::time_point::max() && entry.lease_expire <= now) {
            records.emplace_back(key, nullptr, true, 0);
        }
    }
    if (records.empty()) {
        return;
    }
    // version 属于整批到期项, 不按 Key 单独推进.
    const auto version = next_version_locked();
    // record 是尚未发布的临时删除指令, 统一补上整批提交序号.
    for (auto& record : records) {
        record.field_version = version;
    }
    history_.push_back({version, std::move(records)});

    // 同一把锁内 Key 不会消失. 直接按准备好的批次提交, 不再扫描整个 Map.
    for (const auto& record : history_.back().records) {
        entries_.find(record.key)->second = {nullptr, version, Clock::time_point::max()};
    }
    global_version_ = version;
    trim_history_locked();
}

std::uint64_t SyncStore::global_version() const {
    // lock 保证读到完整提交, 无需再引入与 Map 不同边界的 atomic 版本计数器.
    std::lock_guard lock(mutex_);
    return global_version_;
}

std::shared_ptr<const Snapshot> SyncStore::get_snapshot() {
    // lock 同时保护缓存版本判断与构建, 避免多个读者重复生成同一快照.
    std::lock_guard lock(mutex_);
    if (!active_snapshot_ || active_snapshot_->global_version != global_version_) {
        // 先完成右侧临时对象, 成功后才替换缓存. 构建失败不会破坏先前已返回的快照.
        active_snapshot_ = create_snapshot_locked();
    }
    return active_snapshot_;
}

std::shared_ptr<const Snapshot> SyncStore::create_snapshot_locked() const {
    // snapshot 只在本函数内可写, 完成后转换为 shared_ptr<const Snapshot> 返回.
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->global_version = global_version_;
    // 用索引数量作容量上界, 避免持锁时先扫描一遍精确计数. 空节点多时会预留部分多余桶.
    snapshot->data.reserve(entries_.size());
    // 一次遍历复制有效值, 空节点不进入对外快照.
    for (const auto& [key, entry] : entries_) {
        // key 复制到独立节点, entry.payload 仅增加共享引用, 不复制载荷字节.
        if (entry.payload) {
            snapshot->data.emplace(key, entry.payload);
        }
    }
    return snapshot;
}

SyncResult SyncStore::extract_since(std::uint64_t since_version) const {
    // lock 保证定位历史, 统计长度和复制记录时批次不会被并发淘汰.
    std::lock_guard lock(mutex_);
    // result 在临时返回值中累积, 分配失败不会改动 Store 或调用者已经拿到的结果.
    SyncResult result{.require_snapshot = since_version > global_version_ || since_version < oldest_version_, .current_version = global_version_, .deltas = {}};
    if (result.require_snapshot || since_version == global_version_) {
        return result;
    }
    // first 指向首个待返回批次. 成员投影直接按版本二分, 不扫描已经确认的历史前缀.
    const auto first = std::ranges::upper_bound(history_, since_version, {}, &VersionBatch::global_version);
    // batches 仅借用锁内稳定的后缀范围, 不分配容器或增加额外状态.
    const auto batches = std::ranges::subrange(first, history_.end());
    // total_deltas 用于一次性预留结果数组. Key 字符串仍需复制, 不能据此宣称零分配.
    std::size_t total_deltas = 0;
    for (const auto& batch : batches) {
        // batch 是一个不可拆分的提交. 先检查剩余容量, 避免求和溢出或超出 vector 上限.
        if (batch.records.size() > result.deltas.max_size() - total_deltas) {
            throw std::length_error("Store delta result too large");
        }
        total_deltas += batch.records.size();
    }
    result.deltas.reserve(total_deltas);

    // 保留已测量的迭代器区间追加. 预留数组避免反复搬移记录, payload 继续共享不可变数据.
    for (const auto& batch : batches) {
        result.deltas.insert(result.deltas.end(), batch.records.begin(), batch.records.end());
    }
    // current_version 是全部记录的提交屏障, 调用者应用完整结果前不能提前确认.
    return result;
}

std::uint64_t SyncStore::next_version_locked() const {
    // 本地提交空间耗尽后必须显式失败. 无符号回绕会破坏历史排序和增量边界.
    if (global_version_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Store version exhausted");
    }
    return global_version_ + 1;
}

void SyncStore::trim_history_locked() {
    // 删除指令的续传责任在历史中. 回收空节点时仍匹配版本, 避免反复删除/重建时提前释放节点.
    while (history_.size() > max_history_batches_) {
        // record 借用待淘汰批次. 只处理删除指令, 避免对正常写入的每条记录再次查表.
        for (const auto& record : history_.front().records) {
            if (record.deleted) {
                // entry 可能已经重建或再次删除; 只回收仍然对应当前淘汰记录的空节点.
                const auto entry = entries_.find(record.key);
                if (entry != entries_.end() && !entry->second.payload && entry->second.field_version == record.field_version) {
                    entries_.erase(entry);
                }
            }
        }
        oldest_version_ = history_.front().global_version;
        history_.pop_front();
    }
}

} // namespace verdandi::cluster
