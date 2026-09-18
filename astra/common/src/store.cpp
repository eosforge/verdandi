// 功能: 先准备所有可能失败的分配, 再提交状态和历史, 保证失败不暴露部分修改.
// 本文件实现了 store.hpp 里的键值内存存储，以及基于增量和全量的同步提取。
#include "store.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace astra {

// put 函数实现: 增加或覆盖一项键值数据，同时追加记录到变更历史。
void Store::put(const std::string& key, Buffer value, std::optional<EpochTime> deadline) {
    if (deadline && deadline->time_since_epoch().count() < 0) {
        throw std::invalid_argument("Store deadline must be nonnegative Unix time");
    }
    // 在锁外接管缓冲并准备唯一记录, 将可提前完成的 Key 复制和容器分配移出临界区，提高并发度.
    // value 直接由 std::vector 被接管，包裹在 const 权限的 shared_ptr 中进行后续分享.
    auto shared = std::make_shared<const Buffer>(std::move(value));

    // 提交序号尚未确定, 0 只存在于未发布的临时记录中.
    std::vector<Store::Delta> records;
    // deleted 参数设为 false, version 暂设为 0。
    records.emplace_back(key, shared, false, 0, deadline);

    std::shared_ptr<const Snapshot> retired;
    // lock 从此处到函数退出串行化 Map, 历史和版本; 异常时自动释放以保证原子性.
    std::lock_guard lock(mutex_);

    if (deadline && !clock_) {
        throw std::logic_error("Store requires an epoch anchor before finite deadlines");
    }

    // 捕获统一的单调时间，避免在锁内多次调用底层时钟获取
    const auto now = Clock::now();

    // 只计算而不发布下一版本, 先拒绝耗尽以免产生回绕. 避免状态被污染.
    const auto version = advance();
    records.front().version = version;

    // try_emplace 先尝试找到现有键或者新插入空的占位。
    // it 指向新增或已有条目; inserted 仅用于撤销尚未完成的新键插入.
    auto [it, inserted] = entries_.try_emplace(key);

    // 临时插入仍被锁遮蔽. 记录或历史分配失败时只撤销本次新增项, 不改动原有条目.
    std::uint64_t snapshot_slot{};
    SnapshotIndex::Key snapshot_key;
    try {
        // 可变 Entry 留在原 Map, 只读索引按路径准备私有页; 已有有效 Key 覆盖时复用共享字符串.
        snapshot_slot = it->second.value ? it->second.snapshot_slot : snapshot_index_.next();
        if (!it->second.value) {
            snapshot_key = std::make_shared<const std::string>(key);
        }
        snapshot_index_.prepare(snapshot_slot);
        // 尝试把此次操作追加至历史队列
        history_.push_back({version, now, std::move(records)});
    } catch (...) {
        // 如果系统内存耗尽抛出异常，为了维持原先的状态需要将上面创建的新记录退回
        if (inserted) {
            entries_.erase(it);
        }
        throw;
    }

    // 历史已经准备完毕. 后续赋值不分配内存, 外部读者只能看到完整提交前或提交后的状态.
    snapshot_index_.set(snapshot_slot, std::move(snapshot_key), {shared, deadline});
    it->second.snapshot_slot = snapshot_slot;
    it->second.key = &it->first;
    it->second.value = std::move(shared);
    it->second.version = version;
    it->second.deadline = deadline;

    // 从已推进的拍边界安排绝对截止, 不把事件循环的延迟再次加到租约上.
    if (clock_) {
        schedule(it->second, *clock_);
    }

    version_ = version;
    trim(now);
    retired = std::move(cache_);
}

// remove 函数实现: 将指定的键置为删除状态，清理载荷但不立即删除 map 节点以备复用。
void Store::remove(const std::string& key) {
    std::shared_ptr<const Snapshot> retired;
    // lock 同时保护存在性判断与删除提交, 不用先解锁再重复查找.
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();

    // it 指向当前节点. 不存在或 value 已为空时均无操作, 不额外维护删除标志.
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.value) {
        return; // 若不存在，或者本来就已经是一个被置空的（已删除）节点，则不需要生成新版本历史。
    }

    // version 在历史成功追加之前不对外可见.
    const auto version = advance();
    // records 独立持有删除指令, 节点中的版本仅用于空节点缓存的回收匹配.
    std::vector<Store::Delta> records;
    // 构建一条表示删除的记录，把 value 置空, deleted 置为 true.
    records.emplace_back(key, nullptr, true, version);
    snapshot_index_.prepare(it->second.snapshot_slot);
    history_.push_back({version, now, std::move(records)});

    snapshot_index_.erase(it->second.snapshot_slot);
    Timer::cancel(it->second);
    it->second.value = nullptr;
    it->second.version = version;
    it->second.deadline.reset();
    version_ = version;
    trim(now);
    retired = std::move(cache_);
}

// 在同一临界区补拍并准备整批历史. 准备阶段只摘调度钩子, 不修改可观察的状态和版本.
void Store::tick(EpochTime now, Clock::time_point local) {
    if (now.time_since_epoch().count() < 0) {
        throw std::invalid_argument("Store time must be nonnegative Unix time");
    }
    std::shared_ptr<const Snapshot> retired;
    // 仅接走彻底清空的大桶表. 先于 lock 声明, 析构在解锁后执行, 不把大块回收放进状态临界区.
    decltype(entries_) retired_entries;
    static_assert(noexcept(entries_.swap(retired_entries)), "Empty Store reclamation must not throw after commit");
    std::lock_guard lock(mutex_);
    // 首次有效 Unix 时间直接建立拍锚点, 尚未允许有限租约, 无需扫描或从纪元零点追赶.
    if (!clock_) {
        clock_ = now;
    }
    const auto pending = now > *clock_ ? static_cast<std::uint64_t>((now - *clock_).count() / interval_.count()) : 0;

    // records 同时保存提交内容和失败时需重排的 Key. 空拍不预分配数组.
    std::vector<Store::Delta> records;
    try {
        for (std::uint64_t step = 0; step < pending; ++step) {
            // before 区分前次异常遗留回调与本次推进. 两者的真实拍边界可能不同.
            const auto before = wheel_.now();
            try {
                wheel_.tick([&](Timer::Node* node) {
                    // entry 在状态锁内地址稳定; 到期值仍以真实截止核对, 不把分段唤醒当成过期.
                    auto& entry = *static_cast<Entry*>(node);
                    const auto boundary = *clock_ + (wheel_.now() == before ? std::chrono::nanoseconds::zero() : interval_);
                    if (entry.deadline && *entry.deadline > boundary) {
                        schedule(entry, boundary);
                        return;
                    }
                    try {
                        records.emplace_back(*entry.key, nullptr, true, 0);
                    } catch (...) {
                        // 当前节点尚未进入 records, 单独重排. 旧数据仍有效, 不能失去后续过期机会.
                        schedule(entry, boundary);
                        throw;
                    }
                    // 页面分配属于准备阶段; 失败时该节点已在 records 中, 由外层统一重新调度.
                    snapshot_index_.prepare(entry.snapshot_slot);
                });
            } catch (...) {
                // Wheel 可能已推进但回调失败; 与其逻辑时钟保持一致, 不重复计算这一拍.
                if (wheel_.now() != before) {
                    *clock_ += interval_;
                }
                throw;
            }
            *clock_ += interval_;
        }
        if (!records.empty()) {
            // version 只计算不发布. 先分配空历史批次, 成功后用无异常 swap 移交记录.
            const auto version = advance();
            for (auto& record : records) {
                record.version = version;
            }
            history_.emplace_back(version, local, std::vector<Store::Delta>{});
        }
    } catch (...) {
        // 所有已收集节点仍在 Map 中; 重排不分配内存, 也不扫描未到期条目.
        for (const auto& record : records) {
            schedule(entries_.find(record.key)->second, *clock_);
        }
        throw;
    }

    if (!records.empty()) {
        // 从此处开始均为不分配的提交操作. 整次补拍产生一个版本, 不逐 Key 或逐拍发布.
        history_.back().records.swap(records);
        const auto version = history_.back().version;
        for (const auto& record : history_.back().records) {
            auto& entry = entries_.find(record.key)->second;
            snapshot_index_.erase(entry.snapshot_slot);
            Timer::cancel(entry);
            entry.value.reset();
            entry.version = version;
            entry.deadline.reset();
        }
        version_ = version;
        retired = std::move(cache_);
    }
    // 空拍也淘汰历史墓碑. 只有 Map 真正无节点时才交还桶数组, 不依据有效值计数误删待复用节点.
    trim(local);
    // 小表保留容量避免频繁清空/重建时抖动. 4096 桶是内部回收阈值, 不是 Key 容量或协议限制.
    // 对非空表不执行 rehash, 不扫描/迁移仍挂在时间轮上的节点, 也不在提交后引入分配失败.
    if (entries_.empty() && entries_.bucket_count() > 4096) {
        entries_.swap(retired_entries);
    }
}

// 仅在已经确认顺序后进行无符号差值, 覆盖最小至最大有限 time_point 的完整距离.
std::uint64_t Store::distance(Clock::time_point later, Clock::time_point earlier) noexcept {
    static_assert(std::numeric_limits<Clock::duration::rep>::digits <= 63);
    return static_cast<std::uint64_t>(later.time_since_epoch().count()) - static_cast<std::uint64_t>(earlier.time_since_epoch().count());
}

// 保留有限绝对截止, 超出单轮范围时先安排一次分段唤醒; 回调再次检查截止并续排.
void Store::schedule(Entry& entry, EpochTime boundary) noexcept {
    if (!entry.deadline) {
        Timer::cancel(entry);
        return;
    }
    // delay 向上取整且不使用可能溢出的 distance + interval - 1 表达式.
    const auto remaining = *entry.deadline > boundary ? static_cast<std::uint64_t>((*entry.deadline - boundary).count()) : std::uint64_t{0};
    const auto interval = static_cast<std::uint64_t>(interval_.count());
    const auto delay = remaining / interval + static_cast<std::uint64_t>(remaining % interval != 0);
    // 裁剪后必定可调度. delay=0 由 Wheel 安排到下一拍, 永远不在 put 内执行回调.
    static_cast<void>(wheel_.schedule(entry, std::min(delay, Timer::limit)));
}

// version 函数实现: 获取当前安全的总递增操作序号。
std::uint64_t Store::version() const {
    // lock 保证读到完整提交, 无需再引入与 Map 不同边界的 atomic 版本计数器.
    std::lock_guard lock(mutex_);
    return version_;
}

// snapshot 函数实现: 当需要向远端拉取一份全量拷贝时，取得当前的干净快照指针。
std::shared_ptr<const Store::Snapshot> Store::snapshot() {
    {
        std::lock_guard lock(mutex_);
        if (cache_) {
            return cache_;
        }
    }
    // 仅快照构建者互相等待. 写入与 TTL 从不持有此锁, 不会等待全表复制.
    std::lock_guard builder(snapshot_mutex_);
    SnapshotIndex::View view;
    std::uint64_t version{};
    {
        std::lock_guard lock(mutex_);
        if (cache_) {
            return cache_;
        }
        view = snapshot_index_.capture();
        version = version_;
    }
    // O(N) 的容器分配/Key 复制和视图回收均在状态锁外. 捕获后写入只复制被共享的修改路径.
    std::shared_ptr<const Snapshot> result;
    try {
        result = dump(view, version);
    } catch (...) {
        // 异常路径也先结束读区再经过状态锁, 最后在锁外释放 View. 仅观察 shared_ptr 独占不足以同步前次读取.
        std::lock_guard lock(mutex_);
        throw;
    }
    {
        // 与写者建立读完成的同步边界, 然后才允许 View 析构; 后续独占页面的原地修改不会与旧读取重叠.
        std::lock_guard lock(mutex_);
        if (version_ == version) {
            cache_ = result;
        }
    }
    return result;
}

// dump 函数实现: 根据现有的有效数据集合真正创建并拷贝键的索引结构。
std::shared_ptr<const Store::Snapshot> Store::dump(const SnapshotIndex::View& view, std::uint64_t version) {
    // snapshot 只在本函数内可写, 完成后转换为 shared_ptr<const Store::Snapshot> 返回.
    auto snapshot = std::make_shared<Store::Snapshot>();
    snapshot->version = version;
    // 只按有效条目预留, 历史墓碑和已经删除的槽号不扩大桶数组.
    snapshot->data.reserve(view.size());
    view.each([&](const std::string& key, const SnapshotIndex::Record& record) { snapshot->data.emplace(key, record); });
    return snapshot;
}

// extract 函数实现: 服务于上级客户端按已有的游标版本尝试追平差异的需求。
Store::Extraction Store::extract(std::uint64_t since, std::size_t max_bytes) const {
    // lock 保证定位历史, 统计长度和复制记录时批次不会被并发淘汰.
    std::lock_guard lock(mutex_);
    // result 在临时返回值中累积, 分配失败不会改动 Store 或调用者已经拿到的结果.
    // 检查调用方游标，如果游标过于超前（未提交过），或是本地的历史队列太短已经丢弃过了最老部分，则触发 stale 请求它做全量拉。
    Store::Extraction result{.stale = since > version_ || since < oldest_, .version = version_, .deltas = {}};

    // 如果游标不匹配无法发送增量，或两者完全同步，则不再构建内容即返回。
    if (result.stale || since == version_) {
        return result;
    }

    // first 指向首个待返回批次. 成员投影直接按版本二分, 不扫描已经确认的历史前缀.
    // 寻找大于 since 版本的开始处
    const auto first = std::ranges::upper_bound(history_, since, {}, &Batch::version);
    // batches 仅借用锁内稳定的后缀范围, 不分配容器或增加额外状态.
    const auto batches = std::ranges::subrange(first, history_.end());

    // total_deltas 用于一次性预留结果数组. Key 字符串仍需复制, 不能据此宣称零分配.
    std::size_t total_deltas = 0;
    // remaining 先扣数组再扣字符串; 全程使用减法/除法检查, 避免计费本身乘加溢出.
    auto remaining = max_bytes;
    for (const auto& batch : batches) {
        // batch 是一个不可拆分的提交. 先检查剩余容量, 避免求和溢出或超出 vector 上限.
        if (batch.records.size() > result.deltas.max_size() - total_deltas || batch.records.size() > remaining / sizeof(Delta)) {
            throw std::length_error("Store delta copy budget exceeded");
        }
        remaining -= batch.records.size() * sizeof(Delta);
        // Key 不同于共享 Value, 复制时可能单独分配. 即使短字符串内联也保守计费, 不绑定标准库布局.
        for (const auto& record : batch.records) {
            if (record.key.size() >= remaining) {
                throw std::length_error("Store delta copy budget exceeded");
            }
            remaining -= record.key.size() + 1;
        }
        total_deltas += batch.records.size();
    }
    // 事先设定容量避免迭代时内存拷贝挪移
    result.deltas.reserve(total_deltas);

    // 保留已测量的迭代器区间追加. 预留数组避免反复搬移记录, value 继续共享不可变数据.
    for (const auto& batch : batches) {
        result.deltas.insert(result.deltas.end(), batch.records.begin(), batch.records.end());
    }
    // version 是全部记录的提交屏障, 调用者应用完整结果前不能提前确认.
    return result;
}

// advance 函数实现: 返回下个递增的单调版本号。
std::uint64_t Store::advance() const {
    if (version_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Store version exhausted");
    }
    return version_ + 1;
}

// trim 函数实现: 维持设定的队列长度控制缓存使用，同时执行真正的废弃节点清理。
void Store::trim(Clock::time_point now) {
    // 删除指令的续传责任在历史中. 回收空节点时仍匹配版本, 避免反复删除/重建时提前释放节点.
    while (!history_.empty() &&
           (history_.size() > capacity_ || retention_ == Clock::duration::zero() ||
            (now >= history_.front().timestamp && distance(now, history_.front().timestamp) >= static_cast<std::uint64_t>(retention_.count())))) {
        // record 借用待淘汰批次. 只处理删除指令, 避免对正常写入的每条记录再次查表.
        for (const auto& record : history_.front().records) {
            if (record.deleted) {
                // entry 可能已经重建或再次删除; 只回收仍然对应当前淘汰记录的空节点.
                const auto entry = entries_.find(record.key);
                if (entry != entries_.end() && !entry->second.value && entry->second.version == record.version) {
                    entries_.erase(entry); // 当从历史记录中踢出且确定对应的也是删空节点，才彻底移除。
                }
            }
        }
        // 更新丢弃线的版本号到该即将丢弃的头部
        oldest_ = history_.front().version;
        // 弹出已过时的最早批次
        history_.pop_front();
    }
}

} // namespace astra
