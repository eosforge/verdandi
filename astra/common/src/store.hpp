// 此文件定义了一个支持在并发和锁保护下进行单写, 多读, 记录历史与快照的 KV 同步存储机制.
#pragma once

#include "snapshot_index.hpp"
#include "wheel.hpp"
#include <astra/clock.hpp>
#include <astra/types.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace astra {

// Store 类: 内部存储工具, 尚不提供业务鉴权, Catalog CAS, 持久化或网络订阅.
// 每个实例有独立版本空间. 同一把锁保护状态, 历史和快照, 调用方不在锁内收到回调.
class Store {
public:
    // Timer: 使用内部的时间轮工具进行超时管理.
    // 4 层 10 位表示每层 1024 个槽, 总跨度 40 位 (约 348 年).
    // 在默认 10ms 精度下, 第一层直接包容 10.24 秒跨度, 落在该层的期限不需要高层级联.
    using Timer = Wheel<4, 10>;
    // Buffer 拥有调用方提交的字节; Value 在状态, 历史和快照间共享不可变载荷.
    using Buffer = Index::Buffer;
    using Value = Index::Value;

    // Delta 结构体: 增量属于单个 Store 的提交序列, 不代表 Catalog 发布者的业务版本.
    // 用于表示一次更新(写入或删除)中单条 KV 的变化.
    struct Delta {
        // key: 本次变更的完整 Key. 记录独立拥有字符串, 历史淘汰后返回结果仍然有效.
        std::string key;

        // value: 写入时共享不可变载荷, 删除时为空. Key 和容器会复制, value 字节不会复制 (减少内存消耗).
        Value value;

        // deleted: true 表示该记录是删除指令, 合法空载荷仍持有非空 Value 指针, deleted 为 false.
        bool deleted{};

        // version: 所属原子批次的本地提交序号. 同批多 Key 的值相等, 不表示 Catalog 业务版本.
        std::uint64_t version{};

        // deadline: 唯一绝对 Unix 截止, 空表示无限期; 删除记录也为空.
        std::optional<Clock::Time> deadline{};
    };

    // Extraction 结构体: 封装向其他节点或上层同步变更时所产生的增量或全量标志.
    // stale 为 true 时 deltas 为空. 否则包含到 version 为止的全部变更.
    struct Extraction {
        // stale: 历史不足或游标超前时为 true; 此时不允许把空 deltas 当作已经同步 (要求全量快照拉取).
        bool stale{};

        // version: 取得锁时最后完成的提交版本. 只有应用全部 deltas 后才能确认到这个位置.
        std::uint64_t version{};

        // deltas: 按提交顺序排列的完整批次变更记录. 同一过期批次内 Key 顺序未定义, 不影响原子边界.
        std::vector<Delta> deltas;
    };

    // Snapshot 结构体: 同一临界区捕获版本与分页视图, 随后在状态锁外复制 Map/Key, 共享 value.
    // 用于对外安全地发布一份在特定版本下的全部当前 KV 内容快照, 避免多线程读取冲突.
    struct Snapshot {
        // version: 与 data 在同一临界区取得的本地全局提交序号.
        std::uint64_t version{};

        // data: 仅包含当前存在的 Key. 已返回快照持有自己的 Map, 与后续写入隔离.
        std::unordered_map<std::string, Index::Record> data;
    };

    // capacity 为历史批次容量, retention 使用本地单调时间, interval 为正纳秒精度.
    // capacity 默认 1000 批, 零表示不保留历史; retention 默认 10 min, 零表示不按年龄保留旧批次.
    // interval 默认 10 ms, 必须严格为正, 零值及负值抛 invalid_argument.
    // initial 可供恢复/测试指定非负 Unix 起点. 默认等待首次 tick, 不从 1970 年补拍.
    explicit Store(std::size_t capacity = 1000, Steady::duration retention = std::chrono::minutes(10), std::chrono::nanoseconds interval = std::chrono::milliseconds(10), std::optional<Clock::Time> initial = {}) : capacity_(capacity), retention_(retention), interval_(interval), clock_(initial) {

        if (interval_ <= std::chrono::nanoseconds::zero() || retention_ < Steady::duration::zero() || (initial && initial->time_since_epoch().count() < 0)) {
            throw std::invalid_argument("Invalid Store timing configuration");
        }
    }

    // 接管 value 并原子提交值, 单一绝对 deadline 与历史. 空 deadline 表示无限期.
    // 有限截止必须非负且 Store 已由 tick 初始化; 时钟质量由调用方 Clock::Reading::deadline_after 校验.
    // 分配失败/版本耗尽不提交部分数据; 不在 put 中隐式删除到期项或更新时钟.
    void put(const std::string& key, Buffer value, std::optional<Clock::Time> deadline = {});

    // remove 函数: 删除 key, 无返回值. 不存在或已经删除时不推进版本; 当前值与单条删除历史同时提交.
    // 分配失败或版本耗尽向调用方抛异常, 当前状态和版本不变.
    // 参数 key: 指示要抹去的记录主键.
    void remove(const std::string& key);

    // 补齐到 now 的完整逻辑拍并整批删除到期项. 截止向上取整到拍边界, 延后不足一拍而不提前删除.
    // now 是非负 Unix 时间, 重复或倒退的合法时间不推进; 空截止永不过期.
    // local 只用于历史保留, 与业务时间分离; 测试可注入, 不写入 Entry/Delta.
    // 分配失败或版本耗尽向调用方传播; 数据, 历史和版本保持原状, 已摘节点在下一拍重试.
    // 时间轮逐拍推进, 大跨度补拍的工作量包含空拍; 同批处理不承诺 O(1) 或固定延迟上限.
    // 历史清理后若 Map 已空且超过 4096 桶, 换回空表并在状态锁外释放旧桶; 非空表不自动收缩.
    void tick(Clock::Time now, Steady::time_point local = Steady::now());

    // version 函数: 加锁返回本实例已完整提交的版本. 不分配内存, 不能据此推断其他实例的同步进度.
    // 返回值: 内部全局的递增版本号.
    std::uint64_t version() const;

    // snapshot 函数: 短锁捕获一致视图, 锁外复制 Key/Map. 同版本复用缓存, 同时只允许一个构建者.
    // 构建期间允许写入, 返回调用期间捕获的版本; 不为追赶新写入无限重试, 也不把旧结果缓存成新版.
    // 创建失败抛出分配异常, 不发布部分结果; 已返回的快照不受后续提交影响.
    // 返回值: 指向当前存储快照的不变指针.
    std::shared_ptr<const Store::Snapshot> snapshot();

    // extract 函数: 返回 since 之后的完整增量. 历史不足或游标超前时明确要求快照.
    // 调用前必须在上层核对实例和订阅范围. 本接口只支持同实例的全范围历史.
    // max_bytes 是本次 Delta 数组与 Key 字节(含终止符)的复制预算, 默认 32 MiB, 0 只允许空结果.
    // 不计共享 Value 载荷/分配器额外开销, 不是进程 RSS 或网络编码上限; 由本地调用方指定, 不直接信任远端输入.
    // 超预算在复制前抛 length_error; 分配失败仍抛 bad_alloc, Store 不变. 不截断批次或降级成可能更大的快照.
    // 网络适配层须捕获资源异常并映射状态码, Store 不依赖 gRPC. 拿到全部结果前不能确认游标.
    // 参数 since: 客户端已持有的记录版本, 以此开始提取更新量.
    // 返回值: 包含所有增量变更或标志要求重拉全量的 Store::Extraction.
    Store::Extraction extract(std::uint64_t since, std::size_t max_bytes = 32U * 1024 * 1024) const;

private:
    // Entry 结构体: 内部存储的数据节点.
    // value 为空时缓存已删除的 Map 节点, 供同 Key 重建复用, 无需独立 deleted 标志.
    // 删除指令由历史持有; 保留节点版本是为了避免旧记录淘汰时过早回收新近删除的缓存.
    struct Entry : public Timer::Node {
        // key 借用所在 unordered_map 节点的键; rehash 保持地址, erase 前会取消定时器.
        const std::string* key{nullptr};
        // 有效值在只读分页索引中的槽号. 删除后失效, 重建时重新申请空槽, 时间轮节点地址不变.
        std::uint64_t snapshot_slot{};
        // value 为空表示已删除节点, 可在历史淘汰前被同 Key 的新值复用.
        Value value;
        // version 标识最后写入/删除的提交, 防止旧历史回收重新使用的节点.
        std::uint64_t version{};
        // 唯一有限截止, 超长租约分段唤醒不改写此值.
        std::optional<Clock::Time> deadline{};
    };

    // Batch 结构体: 一次过期清理可有多个 Key, 必须整批保留和淘汰, 不允许截断批次内部记录.
    struct Batch {
        // version: 与前一批次相差 1 的本地提交序号, 失败或无操作不产生批次.
        std::uint64_t version;
        // timestamp: 记录该批次产生的系统单调时间, 用于时间维度淘汰.
        Steady::time_point timestamp;
        // records: 同次提交的完整记录, 发布后不再改动; 一次过期清理可能有多条.
        std::vector<Store::Delta> records;
    };

    // advance 函数: 调用方必须持有 mutex_. 返回下一提交序号而不改动状态; 耗尽时抛 overflow_error.
    std::uint64_t advance() const;
    // 调用方持有状态锁, 从已推进边界安排有限截止, 无分配且不抛异常.
    void schedule(Entry& entry, Clock::Time boundary) noexcept;

    // later >= earlier 时返回无符号时钟计数差, 避免跨有符号极值相减溢出; 调用方先比较顺序.
    static std::uint64_t distance(Steady::time_point later, Steady::time_point earlier) noexcept;

    // trim 函数: 调用方必须持有 mutex_. 淘汰完整历史批次及可丢弃的空节点, 推进续传下界, 不分配内存.
    void trim(Steady::time_point now);

    // dump 只读取捕获视图, 不访问 Store 的可变 Map 或状态锁. 分配失败不影响任何已发布状态.
    static std::shared_ptr<const Store::Snapshot> dump(const Index::View& view, std::uint64_t version);

    // mutex_: 唯一状态锁. mutable 允许只读查询与写入共享相同的同步边界.
    mutable std::mutex mutex_;
    // 只串行化快照构建者. 写入/TTL 从不获取此锁; 同时持锁时固定先 snapshot_mutex_ 后 mutex_.
    std::mutex snapshot_mutex_;
    // 可变调度容器与不可变业务视图分离. 所有索引变更和 version 在同一次无异常提交中发布.
    Index snapshot_index_;

    // entries_: 当前 Key 索引及短期空节点缓存. 空节点只为避免同 Key 重建反复分配, 不出现在快照中.
    std::unordered_map<std::string, Entry> entries_;

    // version_: 最后完整提交的本地序号, 初始为 0, 达到 uint64_t 上限后拒绝新提交.
    std::uint64_t version_{};

    // capacity_: 构造后不改变的历史批次容量. 不是记录数或内存字节上限.
    std::size_t capacity_;

    // retention_: 历史批次在队列中的最长存活时间.
    Steady::duration retention_;
    // 纳秒拍间隔, 唯一构造入口验证为正, 构造后不再改变.
    const std::chrono::nanoseconds interval_;
    // clock_ 与 wheel_.now() 同步, 异常后也不重复推进已完成的拍.
    std::optional<Clock::Time> clock_;
    // wheel_ 先于 entries_ 析构并解除全部钩子; 成员节点析构仍可安全重复取消.
    Timer wheel_{};

    // history_: 按提交号递增的完整批次. deque 支持头部淘汰和随机访问二分查找.
    std::deque<Batch> history_;

    // oldest_: 已淘汰的最后一个完整版本; 此版本的客户端仍可从其下一条增量续传.
    std::uint64_t oldest_{};

    // cache_: 仅缓存仍匹配当前版本的完整快照. 提交时移走旧缓存, 在状态锁外回收.
    std::shared_ptr<const Store::Snapshot> cache_;
};

} // namespace astra
