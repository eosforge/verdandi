// 功能: 在同一提交边界内维护当前状态, 有界增量历史和按版本复用的只读快照.
// 此文件定义了一个支持在并发和锁保护下进行单写、多读、记录历史与快照的 KV 同步存储机制。
#pragma once

#include <astra/types.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace astra {

// Store 类: 内部存储工具, 尚不提供业务鉴权, Catalog CAS, 持久化或网络订阅.
// 每个实例有独立版本空间. 同一把锁保护状态, 历史和快照, 调用方不在锁内收到回调.
class Store {
public:
    using Buffer = std::vector<std::uint8_t>;
    using Value = std::shared_ptr<const Buffer>;
    
    // Delta 结构体: 增量属于单个 Store 的提交序列, 不代表 Catalog 发布者的业务版本.
    // 用于表示一次更新（写入或删除）中单条 KV 的变化。
    struct Delta {
        // key: 本次变更的完整 Key. 记录独立拥有字符串, 历史淘汰后返回结果仍然有效.
        std::string key;
        
        // value: 写入时共享不可变载荷, 删除时为空. Key 和容器会复制, value 字节不会复制 (减少内存消耗).
        Value value;
        
        // deleted: true 表示该记录是删除指令, 空 value 的正常值仍以 false 区分.
        bool deleted{};
        
        // version: 所属原子批次的本地提交序号. 同批多 Key 的值相等, 不表示 Catalog 业务版本.
        std::uint64_t version{};
    };

    // Extraction 结构体: 封装向其他节点或上层同步变更时所产生的增量或全量标志。
    // stale 为 true 时 deltas 为空. 否则包含到 version 为止的全部变更.
    struct Extraction {
        // stale: 历史不足或游标超前时为 true; 此时不允许把空 deltas 当作已经同步 (要求全量快照拉取).
        bool stale{};
        
        // version: 取得锁时最后完成的提交版本. 只有应用全部 deltas 后才能确认到这个位置.
        std::uint64_t version{};
        
        // deltas: 按提交顺序排列的完整批次变更记录. 同一过期批次内 Key 顺序未定义, 不影响原子边界.
        std::vector<Delta> deltas;
    };

    // Snapshot 结构体: 内容和版本由同一临界区取得. Map 和 Key 需要复制, 仅 value 与 Store 共享.
    // 用于对外安全地发布一份在特定版本下的全部当前 KV 内容快照，避免多线程读取冲突。
    struct Snapshot {
        // version: 与 data 在同一临界区取得的本地全局提交序号.
        std::uint64_t version{};
        
        // data: 仅包含当前存在的 Key. 已返回快照持有自己的 Map, 与后续写入隔离.
        std::unordered_map<std::string, Value> data;
    };

    // 构造函数: capacity 限制队列长度, retention 限制最长保留时间. 0 长度表示不保留历史.
    // 创建独立的零版本实例, 尚未创建快照. deque 初始化可能分配内存并抛出 bad_alloc.
    explicit Store(std::size_t capacity = 1000, Clock::duration retention = std::chrono::minutes(10)) 
        : capacity_(capacity), retention_(retention) {}

    // put 函数: 向 key 写入或覆盖 value, 即使内容相同也提交一个新版本; 无返回值.
    // 接管 value 值; 移入后调用方不得继续使用指向原缓冲的可写指针或引用.
    // expire 为单调时间租约截止, max 表示不自动到期. 分配失败或版本耗尽时抛异常且状态不变.
    // 参数 key: 要写入的数据主键.
    // 参数 value: 二进制载荷的数据向量，右值移入，由容器底层直接接管不再进行深拷贝。
    // 参数 expire: 可选的超时到期时间，默认为无限制.
    void put(const std::string& key, Buffer value, Clock::time_point expire = Clock::time_point::max());
    
    // remove 函数: 删除 key, 无返回值. 不存在或已经删除时不推进版本; 当前值与单条删除历史同时提交.
    // 分配失败或版本耗尽向调用方抛异常, 当前状态和版本不变.
    // 参数 key: 指示要抹去的记录主键.
    void remove(const std::string& key);
    
    // sweep 函数: 将有限截止 <= now 的条目作为同一批次删除, 无返回值. now 通常为 Clock::now().
    // max 截止始终表示永不过期, 即使 now 为 max. 无到期项时不推进版本.
    // 分配失败或版本耗尽向调用方传播, 整批保持原状, 由调用方决定重试.
    // 参数 now: 以此时刻为准判断所有已有键值是否超时需要被批量逐出。
    void sweep(Clock::time_point now);
    
    // version 函数: 加锁返回本实例已完整提交的版本. 不分配内存, 不能据此推断其他实例的同步进度.
    // 返回值: 内部全局的递增版本号。
    std::uint64_t version() const;
    
    // snapshot 函数: 加锁返回当前只读快照, 同版本复用缓存. 首次构建复制 Key/Map, 仅共享 value.
    // 创建失败抛出分配异常并保留旧缓存; 已返回的快照不受后续提交影响.
    // 返回值: 指向当前存储快照的不变指针。
    std::shared_ptr<const Store::Snapshot> snapshot();
    
    // extract 函数: 返回 since 之后的完整增量. 历史不足或游标超前时明确要求快照.
    // 调用前必须在上层核对实例和订阅范围. 本接口只支持同实例的全范围历史.
    // 分配失败抛异常且 Store 不变. 结果独立拥有 Key 和容器, 不能在拿到全部结果前确认游标.
    // 参数 since: 客户端已持有的记录版本，以此开始提取更新量。
    // 返回值: 包含所有增量变更或标志要求重拉全量的 Store::Extraction.
    Store::Extraction extract(std::uint64_t since) const;

private:
    // Entry 结构体: 内部存储的数据节点.
    // value 为空时缓存已删除的 Map 节点, 供同 Key 重建复用, 无需独立 deleted 标志.
    // 删除指令由历史持有; 保留节点版本是为了避免旧记录淘汰时过早回收新近删除的缓存.
    struct Entry {
        // value: 非空表示当前有效值, 载荷允许零字节; 空表示可复用的已删除节点.
        Value value;
        
        // version: 本节点最后一次写入/删除的本地序号. 回收空节点时匹配对应历史, 保留同 Key 的快速重建路径.
        std::uint64_t version{};
        
        // expire: 基于单调时钟的到期时刻. max 为明确的永不过期哨兵.
        Clock::time_point expire{Clock::time_point::max()};
    };
    
    // Batch 结构体: 一次过期清理可有多个 Key, 必须整批保留和淘汰, 不允许截断批次内部记录.
    struct Batch {
        // version: 与前一批次相差 1 的本地提交序号, 失败或无操作不产生批次.
        std::uint64_t version;
        // timestamp: 记录该批次产生的系统单调时间，用于时间维度淘汰.
        Clock::time_point timestamp;
        // records: 同次提交的完整记录, 发布后不再改动; 一次过期清理可能有多条.
        std::vector<Store::Delta> records;
    };
    
    // advance 函数: 调用方必须持有 mutex_. 返回下一提交序号而不改动状态; 耗尽时抛 overflow_error.
    std::uint64_t advance() const;
    
    // trim 函数: 调用方必须持有 mutex_. 淘汰完整历史批次及可丢弃的空节点, 推进续传下界, 不分配内存.
    void trim(Clock::time_point now);
    
    // dump 函数: 调用方必须持有 mutex_. 在临时对象中构建完整快照, 返回前不替换缓存; 分配失败抛异常.
    std::shared_ptr<const Store::Snapshot> dump() const;

    // mutex_: 唯一状态锁. mutable 允许只读查询与写入共享相同的同步边界.
    mutable std::mutex mutex_;
    
    // entries_: 当前 Key 索引及短期空节点缓存. 空节点只为避免同 Key 重建反复分配, 不出现在快照中.
    std::unordered_map<std::string, Entry> entries_;
    
    // version_: 最后完整提交的本地序号, 初始为 0, 达到 uint64_t 上限后拒绝新提交.
    std::uint64_t version_{};
    
    // capacity_: 构造后不改变的历史批次容量. 不是记录数或内存字节上限.
    std::size_t capacity_;
    
    // retention_: 历史批次在队列中的最长存活时间.
    Clock::duration retention_;
    
    // history_: 按提交号递增的完整批次. deque 支持头部淘汰和随机访问二分查找.
    std::deque<Batch> history_;
    
    // oldest_: 已淘汰的最后一个完整版本; 此版本的客户端仍可从其下一条增量续传.
    std::uint64_t oldest_{};
    
    // cache_: 最近成功构建的不可变快照. 可能落后当前版本, 但只在匹配版本时复用.
    std::shared_ptr<const Store::Snapshot> cache_;
};

} // namespace astra
