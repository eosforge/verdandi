#pragma once
#include "pages.hpp"
#include <algorithm>
#include <astra/scope.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace astra {
// 一个来源 Star/动态域的连续事实, position 覆盖全部 Sector/Spectrum. 不作为 Almanac 或三域统一 Store.
// Item 是 Catalog/Ephemeris 原生记录; 本类不解释 TTL/order/业务版本, 不决定广播内容或本地投影.
// 除已返回 View 外, 所有调用及 Edit 寿命均由传入的同一 gate 串行保护. 同时至多一个准备事务.
// 提交只移交不可变字节引用和原生标量, Item 的复制/移动必须无异常, 在模板使用边界拒绝不合格类型.
template <typename Item>
    requires(std::is_nothrow_copy_constructible_v<Item> && std::is_nothrow_copy_assignable_v<Item> && std::is_nothrow_move_constructible_v<Item> && std::is_nothrow_move_assignable_v<Item>)
class Origin {
public:
    // 仅为实际来源记录持有一份 Key, Scope 文本在组内共享, 不编码拼接路径或复制进原生 Item.
    struct Name {
        // 已验证的二元地址, 不同 Key 共享本来源 Scope 的同一个对象.
        std::shared_ptr<const Scope> scope;
        // 原始 UTF-8 Key 或规范 UUID, 不包含 Scope/来源前缀.
        std::string key;
    };

    // 来源根直接保存原生记录, 不把 Attr/Data 或仅水位序列化成 KV Buffer.
    using Tree = Pages<Item, Name>;
    // 保守计算原生载荷字节的无分配函数, 元数据由本类单独计费; 必须返回有限且不会溢出的计费.
    using Measure = std::size_t (*)(const Item&) noexcept;

    // 组级容量, 不为每个 Scope 重复分配历史窗口, 不等于进程 RSS.
    struct Limits {
        // 当前原生记录上限, 默认 65536, Catalog 仅水位同样占一项.
        std::size_t records = 65536;
        // 活动 Scope 上限, 默认 4096, 空来源 Scope 可回收且不重置来源位置.
        std::size_t scopes = 4096;
        // 记录、键、Scope 及原生载荷的保守计费上限, 默认 64 MiB.
        std::size_t bytes = 64 * 1024 * 1024;
        // 本机来源发送历史条数, 默认 4096; 远端来源由构造参数禁用发送历史.
        std::size_t history = 4096;
        // 历史记录和被引用载荷的保守字节上限, 默认 8 MiB, 零禁用.
        std::size_t backlog = 8 * 1024 * 1024;
        // 写入时按本地单调时间裁剪发送历史, 默认 10 分钟, 零禁用; 未淘汰的连续后缀仍可读取.
        std::chrono::steady_clock::duration retention = std::chrono::minutes(10);
    };

    // 不包含业务版本冲突或租约结束, 这些条件由原生规则在最终提交边界判断.
    enum class Error {
        // 地址/Key/计费或版本参数非法.
        input,
        // 全量来源快照出现重复 Scope/Key, 不以最后一项覆盖前面的候选.
        duplicate,
        // 原生记录或准备元数据超过该来源的硬容量.
        capacity,
        // 来源位置不是严格 +1 或已耗尽, 不改用按 Scope 编号.
        version,
        // 历史不足以覆盖完整连续后缀, 必须按协议恢复来源基线.
        history
    };

    // 发送方按业务事实选择编码, 续租不因历史保留完整原生候选而重发 Attr/Data.
    enum class Form {
        // 创建、Catalog 完整发布或仅水位, 包含该域规定的完整事实.
        record,
        // Ephemeris 只更新动态正文及 Data order, 固定 Attr 不重复发送.
        data,
        // 仅期限/续租顺序刷新, 缺项由接收者发起精确回补.
        renew,
        // Ephemeris 来源权威结束, 不等于副本本地 TTL 删除.
        erase
    };

    // 一次完整的来源变化, 只有本机组保留发送历史, 不为远端组复制第二份转发日志.
    struct Event {
        // 不可变地址/Key 所有权, 即使当前记录被删也保持历史可解释.
        typename Tree::Key name;
        // 完整的原生候选, 空表示权威结束; 编码层按业务变化选择 data/renew/full 消息.
        std::optional<Item> record;
        // 本来源本域的位置, 初始提交从 1 开始, 从不从 Scope 计数推导.
        std::uint64_t position{};
        // 原生变化形式, 只控制下游源端编码, 不把本地维护作为广播事实.
        Form form = Form::record;
        // 本项保守历史计费, 包含被引用载荷, 不宣称按实际共享页去重后的 RSS.
        std::size_t bytes{};
        // 仅用于历史窗口, 不进入业务记录或线上的绝对 deadline.
        std::chrono::steady_clock::time_point stored{};
    };

    // 提交后必须移到 gate 外再释放, 避免一次裁剪大量历史或最后载荷引用拖住业务锁.
    struct Retired {
        // 被替换的原生记录, 避免页覆盖在锁内释放其最后大块载荷.
        std::optional<Item> record;
        // 被裁剪的完整事件, 向量在 prepare 时已分配, commit 只移动.
        std::vector<Event> events;
        // 删除时延长 Name 寿命, 查找表摘除后不存在悬空的 string_view.
        typename Tree::Key name;
    };

    // 不可变完整来源根, 版本与根同边界捕获; 读完经过独立 gate, 可以越过来源对象寿命.
    class View {
    public:
        // 完整连续来源位置, 空来源合法为零.
        std::uint64_t position() const noexcept {
            return position_;
        }

        // 捕获时的原生记录数, 包括 Catalog 水位.
        std::size_t size() const noexcept {
            return size_;
        }

        // 捕获根可能保留的原生逻辑字节, 给发送恢复预算使用, 不宣称等于实际 RSS.
        std::size_t bytes() const noexcept {
            return bytes_;
        }

        // 回调借用 Scope/Key/原生记录, 不持有 gate, 可抛异常; 保存记录时复制共享所有权.
        void each(auto&& reader) const {

            const auto root = root_;  // 保持本次遍历根, 必须先于 Fence 构造.
            const Fence fence{gate_}; // 正常/异常均在最后读取后建立完成同步, 再释放根.
            root.each([&reader](const Name& name, const Item& record) { reader(*name.scope, name.key, record); });
        }

        // 按有效行数跳过既有前缀, reader 返回 false 在该项前暂停, 不为每页重复遍历整组.
        std::size_t page(std::size_t offset, std::size_t maximum, auto&& reader) const {

            const auto root = root_;
            const Fence fence{gate_};
            return root.page(offset, maximum, [&reader](const Name& name, const Item& record) { return reader(*name.scope, name.key, record); });
        }

    private:
        friend class Origin;

        // 最后一次读取后经过与写者相同的同步域, use_count 本身不是内存屏障.
        struct Fence {
            std::shared_ptr<std::mutex> gate; // 可独立存活的原提交锁.

            ~Fence() {
                const std::lock_guard lock(*gate);
            } // 根临时引用释放前完成同步.
        };

        // 只在所属 gate 内构造, 不遍历或分配; 参数分别固定根、版本及同步域.
        View(typename Tree::View root, std::uint64_t position, std::size_t bytes, std::shared_ptr<std::mutex> gate) : root_(std::move(root)), position_(position), bytes_(bytes), size_(root_.size()), gate_(std::move(gate)) {}

        typename Tree::View root_;         // 捕获时的完整页树.
        std::uint64_t position_{};         // 与 root 同边界的来源位置.
        std::size_t bytes_{};              // 捕获时原生行/目录计费, 不随后续来源变化改变.
        std::size_t size_{};               // 捕获时计数, 查询不再读取可复用页.
        std::shared_ptr<std::mutex> gate_; // 不延长来源或网络对象寿命.
    };

    // 一次发送准备, 完整根或连续历史前缀二选一; head 与结果在同一 gate 内捕获.
    struct Delivery {
        std::uint64_t head{};         // 捕获时已提交头部, 不是对端 ACK.
        std::optional<View> baseline; // 历史断档时提供整个来源根, 不按 Scope 补齐.
        std::vector<Event> events;    // 有界连续前缀, 不为每个对端复制整份保留日志.
    };

private:
    // string_view 只借用页中 Name 的字符串, 新项在提交前由 Edit 保持名称存活.
    using Table = std::unordered_map<std::string_view, std::uint64_t>;

    // 每 Scope 只保存直接查找目录, 没有独立来源版本/历史/时间轮.
    struct Group {
        std::shared_ptr<const Scope> scope; // 组内 Key 共用的完整地址.
        Table entries;                      // Key -> 全来源页槽, 不复制正文.
    };

    // 用户确认的 Sector -> Spectrum 目录, 不通过单字符串路径处理标识.
    using Groups = std::map<std::string, std::map<std::string, Group, std::less<>>, std::less<>>;

public:
    // 活动准备事务, 期间不得再读写同一个 Origin. 外层可继续准备独立的本地投影, 全部成功后一起 commit.
    class Edit {
    public:
        // 未 commit 则撤销新查找项和准备历史, 不改原根/位置; 必须在同一 gate 内完成.
        ~Edit() {
            abort();
        }

        // 只转移准备责任, 移后对象不再回滚; 与所属 Origin 的地址绑定不变.
        Edit(Edit&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), group_(other.group_), name_(std::move(other.name_)), record_(std::move(other.record_)), position_(other.position_), slot_(other.slot_), bytes_(other.bytes_), old_(other.old_), added_(other.added_), queued_(other.queued_), evicted_(other.evicted_), retired_(std::move(other.retired_)) {}

        // 不允许覆盖仍活跃的准备事务, 外层显式结束旧事务再准备下一项.
        Edit& operator=(Edit&&) = delete;
        // 不复制准备事务或同时提交两次.
        Edit(const Edit&) = delete;
        // 禁止复制赋值复制回滚责任.
        Edit& operator=(const Edit&) = delete;

        // 已准备的稳定名称, 供同一次原子提交的投影/调度共享; 不重新格式化或复制 Key.
        const typename Tree::Key& name() const noexcept {
            return name_;
        }

        // 候选行及当前目录的保守计费, 删除最后一项时可能比提交后的精确值略高.
        std::size_t bytes() const noexcept {
            return bytes_ + owner_->directory_;
        }

        // 最终重读时钟后更新等计费的原生标量, 例如截止; 不允许改变载荷预算或把删除改成新增.
        // 同一 gate 下连同暂存历史一起修订, 返回 false 时保持全部候选不变.
        bool revise(const Item& record) noexcept {
            if (!owner_ || !record_ || owner_->measure_(record) != owner_->measure_(*record_)) {
                return false;
            }
            record_ = record;
            if (queued_) {
                owner_->history_.back().record = record;
            }
            return true;
        }

        // 无异常发布来源根、查找表、位置、历史和计费. 外层在同一 gate 内发布投影, 再释放返回的旧资源.
        Retired commit() noexcept {

            auto& owner = *owner_; // 单个仍有效的所属来源; 二次 commit 属于内部调用错误.
            if (record_) {
                owner.tree_.set(slot_, added_ ? name_ : nullptr, std::move(*record_));
            } else if (old_) {
                owner.tree_.erase(slot_);
                group_->entries.erase(name_->key);
                --owner.records_;
            }
            if (added_) {
                ++owner.records_;
            }
            owner.bytes_ = bytes_;
            owner.position_ = position_;
            for (std::size_t index = 0; index < evicted_; ++index) {
                owner.backlog_ -= owner.history_.front().bytes;
                retired_.events.push_back(std::move(owner.history_.front()));
                owner.history_.pop_front();
            }
            if (group_ && group_->entries.empty()) {
                owner.prune(*name_->scope);
            }
            owner.editing_ = false;
            owner_ = nullptr;
            return std::move(retired_);
        }

    private:
        friend class Origin;

        // 一旦构造即取得单项准备责任, 所有可失败步骤都在此后的 prepare 中完成.
        explicit Edit(Origin& owner) : owner_(&owner), position_(owner.position_) {
            owner.editing_ = true;
        }

        // 撤销只涉及本次新增目录/历史, 已有页的 prepare 路径内容与计数保持原样.
        void abort() noexcept {

            if (!owner_) {
                return;
            }
            if (queued_) {
                owner_->backlog_ -= owner_->history_.back().bytes;
                owner_->history_.pop_back();
            }
            if (added_) {
                group_->entries.erase(name_->key);
            }
            if (group_ && group_->entries.empty()) {
                owner_->prune(*group_->scope);
            }
            owner_->editing_ = false;
            owner_ = nullptr;
        }

        Origin* owner_{};            // 未提交时借用来源, 外层保证其寿命与 gate.
        Group* group_{};             // 本次目标组, 缺失删除可为空.
        typename Tree::Key name_;    // 本次地址/Key 的固定所有权.
        std::optional<Item> record_; // 待发布原生候选, 空表示删除.
        std::uint64_t position_{};   // 新位置, 维护性清理可以保持旧值.
        std::uint64_t slot_{};       // 已有或新准备的页槽, 缺失删除不使用.
        std::size_t bytes_{};        // 提交后的活动行计费, Scope 目录另受 scopes 上限约束.
        bool old_{};                 // 原记录确实存在, 决定删除/旧资源保存.
        bool added_{};               // 本次已插入尚未发布的新查找项, 析构负责撤销.
        bool queued_{};              // 已准备发送历史尾项, 析构负责撤销.
        std::size_t evicted_{};      // 提交后淘汰项数, 可以包含刚准备的新项.
        Retired retired_;            // 大块旧资源在 commit 后交由外层锁外析构.
    };

    // 内部整组恢复的保留式合并候选, 用于 Catalog 最高水位. 只允许逐唯一目标 upsert, 不编号或保留广播历史.
    // 原生字段含义仍由业务层决定, 不开放成 Comet 多 Key API, 也不能给自有来源绕过连续位置.
    class Batch {
    public:
        // 失败只撤销新增查找项/空组, 原根、旧记录及来源位置不变.
        ~Batch() {
            abort();
        }

        // 唯一回滚责任可以移动, 禁止复制多个拥有者.
        Batch(Batch&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), tree_(std::move(other.tree_)), added_(std::move(other.added_)), created_(std::move(other.created_)), seen_(std::move(other.seen_)), bytes_(other.bytes_), records_(other.records_), failed_(other.failed_) {}

        // 不覆盖未结束事务.
        Batch& operator=(Batch&&) = delete;
        // 不复制回滚责任.
        Batch(const Batch&) = delete;
        // 不复制赋值.
        Batch& operator=(const Batch&) = delete;

        // 候选点查, 调用方持有同一域锁; 复制不可变记录, 不借用后续 COW 可能替换的页.
        std::optional<Item> find(const Scope& scope, std::string_view key) const {
            const auto* group = owner_->locate(scope);
            if (!group) {
                return std::nullopt;
            }
            const auto found = group->entries.find(key);
            return found == group->entries.end() ? std::nullopt : std::optional<Item>(*tree_.at(found->second));
        }

        // 一个候选中每个 Scope/Key 至多修改一次. 任意失败后只能放弃整批, 不提交先前准备的子集.
        std::expected<typename Tree::Key, Error> set(const Scope& scope, std::string_view key, Item record) {

            if (failed_ || !owner_ || !scope.valid() || !Scope::text(key, 1024)) {
                failed_ = true;
                return std::unexpected(Error::input);
            }
            failed_ = true; // 包含分配异常的全部失败都保持候选不可提交.
            auto* group = owner_->locate(scope);
            const auto found = group ? group->entries.find(key) : typename Table::iterator{};
            const bool exists = group && found != group->entries.end();
            const auto slot = exists ? found->second : tree_.next();
            auto row = exists ? tree_.find(slot) : std::pair<const typename Tree::Key*, const Item*>{};
            typename Tree::Key name = exists ? *row.first : nullptr;
            if (exists && seen_.contains(name.get())) {
                return std::unexpected(Error::duplicate);
            }
            const auto before = exists ? owner_->weight(key, *row.second) : std::optional<std::size_t>(0);
            const auto after = owner_->weight(key, record);
            const auto extra = group ? 0 : directory(scope);
            if (!before || !after || *after > owner_->limits_.bytes || bytes_ - *before > owner_->limits_.bytes - *after || (!exists && records_ == owner_->limits_.records) || owner_->directory_ > owner_->limits_.bytes || extra > owner_->limits_.bytes - owner_->directory_ || bytes_ - *before + *after > owner_->limits_.bytes - owner_->directory_ - extra) {
                return std::unexpected(Error::capacity);
            }
            if (!group) {
                if (owner_->scopes_ == owner_->limits_.scopes) {
                    return std::unexpected(Error::capacity);
                }
                created_.reserve(created_.size() + 1); // obtain 成功后登记撤销责任不允许再抛错.
                group = &owner_->obtain(scope);
                created_.push_back(group->scope);
            }
            if (!name) {
                name = std::make_shared<const Name>(group->scope, std::string(key));
                added_.push_back(Added{group, name}); // 名称所有权先保持, emplace 失败也能正确回滚.
                group->entries.emplace(name->key, slot);
            }
            seen_.insert(name.get());
            tree_.prepare(slot);
            tree_.set(slot, exists ? nullptr : name, record);
            bytes_ = bytes_ - *before + *after;
            records_ += !exists;
            failed_ = false;
            return name;
        }

        // 无分配发布候选根/计费, 返回旧根须在域锁外释放, 不更改来源位置/历史.
        Tree commit() noexcept {
            assert(owner_ && !failed_);
            auto old = std::move(owner_->tree_);
            owner_->tree_ = std::move(tree_);
            owner_->bytes_ = bytes_;
            owner_->records_ = records_;
            owner_->editing_ = false;
            owner_ = nullptr;
            return old;
        }

        // 外层统一核对原生候选计费, 不把共享正文当成只有指针大小.
        std::size_t bytes() const noexcept {
            return bytes_ + owner_->directory_;
        }

    private:
        friend class Origin;

        // 一条新增目录项的撤销责任, Group 地址在事务结束前稳定.
        struct Added {
            Group* group{};          // owner 两级目录中的固定分组对象.
            typename Tree::Key name; // 保持借用字符串有效, 不是载荷副本.
        };

        // 本构造不复制 Key 哈希表, 只增加一个根引用, 所有新页按需 COW.
        explicit Batch(Origin& owner) : owner_(&owner), tree_(owner.tree_), bytes_(owner.bytes_), records_(owner.records_) {
            owner.editing_ = true;
        }

        // 必须先擦除所有新增条目, 再回收新空组, 不访问已经析构的 Group 指针.
        void abort() noexcept {
            if (owner_) {
                for (const auto& item : added_) {
                    item.group->entries.erase(item.name->key);
                }
                for (const auto& scope : created_) {
                    owner_->prune(*scope);
                }
                owner_->editing_ = false;
                owner_ = nullptr;
            }
        }

        Origin* owner_{};                                   // 唯一准备责任, 移动/提交后置空.
        Tree tree_;                                         // 候选 COW 根, 与真实来源共用 gate 同步域.
        std::vector<Added> added_;                          // 新查找项回滚列表.
        std::vector<std::shared_ptr<const Scope>> created_; // 新空组回滚列表.
        std::unordered_set<const Name*> seen_;              // 固定名称去重, 不复制 Key 文本.
        std::size_t bytes_{};                               // 候选记录计费, 目录在 owner 暂存但被 editing_ 隔离.
        std::size_t records_{};                             // 候选记录数.
        bool failed_{};                                     // 任意失败后不允许再提交此候选.
    };

    // 来源完整快照的私有候选, 不开放捕获接口, 安装前不存在使用另一同步域读取新页的读者.
    class Draft {
    public:
        // 放弃半份全量只释放候选, 不访问目标来源或改写它的位置.
        ~Draft() = default;
        // 恢复任务可转交候选唯一所有权, 被移动对象只能析构或接收赋值.
        Draft(Draft&&) noexcept = default;
        // 替换私有候选, 不安装到任何来源.
        Draft& operator=(Draft&&) noexcept = default;
        // 不复制整组恢复工作集.
        Draft(const Draft&) = delete;
        // 不复制恢复责任.
        Draft& operator=(const Draft&) = delete;

        // 添加唯一原生记录, 全量遍历和记录准备在目标 gate 外进行; 失败不改变既有完整状态.
        std::expected<void, Error> set(const Scope& scope, std::string key, Item record) {

            if (!candidate_) {
                return std::unexpected(Error::input);
            }
            Retired retired; // 无旧记录/历史, 保留同样的锁外资源释放边界.
            const std::lock_guard lock(*candidate_->gate_);
            if (candidate_->find(scope, key)) {
                return std::unexpected(Error::duplicate);
            }
            auto edit = candidate_->prepare(scope, std::move(key), std::move(record), std::nullopt, std::chrono::steady_clock::now());
            if (!edit) {
                return std::unexpected(edit.error());
            }
            retired = edit->commit();
            return {};
        }

        // 私有候选的完整位置, 最终安装还需对照当前连续前缀/回补覆盖位置重新验证.
        std::uint64_t position() const noexcept {
            return position_;
        }

        // 私有候选当前原生计费, 不包含网络编码缓冲.
        std::size_t bytes() const noexcept {
            return candidate_ ? candidate_->bytes_ + candidate_->directory_ : 0;
        }

        // 点查复制不可变载荷所有权, 不把私有候选暴露成第二个同步域的 View.
        std::optional<Item> find(const Scope& scope, std::string_view key) const {
            return candidate_ ? candidate_->find(scope, key) : std::nullopt;
        }

        // 构建投影/调度计划时借用候选原生条目和共享名称, 不逃逸可变根或新的 View 同步域.
        // reader 只能收集/准备其他对象, 不重入修改当前 Draft; 名称/内容可按共享所有权保存.
        void each(auto&& reader) const {
            if (candidate_) {
                candidate_->each(std::forward<decltype(reader)>(reader));
            }
        }

    private:
        friend class Origin;

        // position 为完整快照声称的来源位置, measure/limits 固定使用接收目标的预算.
        Draft(std::uint64_t position, Measure measure, Limits limits) : candidate_(std::make_unique<Origin>(std::make_shared<std::mutex>(), measure, false, limits)), position_(position) {}

        std::unique_ptr<Origin> candidate_; // 独立候选及目录, 从不暴露 View 或裸节点给异步读者.
        std::uint64_t position_{};          // complete 之前不得发布或 ACK 的来源位置.
    };

    // 全量安装后接走整份旧目录/根/历史, 对外只提供唯一寿命, 避免旧状态再次被修改.
    class Replaced {
    public:
        // 析构必须在目标 gate 外执行, 可以释放大规模旧树; 老 View 仍保持其独立引用.
        ~Replaced() = default;
        // 转交锁外回收责任, 不恢复旧来源状态.
        Replaced(Replaced&&) noexcept = default;
        // 调用方只在锁外替换非空回收责任, 避免同步销毁旧树.
        Replaced& operator=(Replaced&&) noexcept = default;
        // 不能复制整组回收责任.
        Replaced(const Replaced&) = delete;
        // 禁止复制赋值.
        Replaced& operator=(const Replaced&) = delete;

    private:
        friend class Origin;

        explicit Replaced(std::unique_ptr<Origin> value) : value_(std::move(value)) {} // 无分配接走已交换的旧状态.

        std::unique_ptr<Origin> value_; // 从此只析构, 不用候选旧 gate 再访问/修改旧页面.
    };

    // local 决定是否保留发送历史, 远端组不转播收到的数据. gate/measure 均必需, 构造不持锁或启动线程.
    Origin(std::shared_ptr<std::mutex> gate, Measure measure, bool local, Limits limits) : gate_(std::move(gate)), measure_(measure), local_(local), limits_(limits) {
        if (!gate_ || !measure_ || limits_.retention < decltype(limits_.retention)::zero()) {
            throw std::invalid_argument("Invalid source configuration");
        }
    }

    // 使用默认组级预算, 不按 Scope 创建多份历史.
    Origin(std::shared_ptr<std::mutex> gate, Measure measure, bool local) : Origin(std::move(gate), measure, local, Limits{}) {}

    // Edit 和外部裸引用不得越过来源寿命, 已返回 View/Item 则独立保持自己的存储.
    ~Origin() = default;
    // 稳定 Group/事务地址禁止复制或搬移来源.
    Origin(const Origin&) = delete;
    // 不复制来源位置和事务边界.
    Origin& operator=(const Origin&) = delete;

    // 下一完整位置由所属来源分配, 耗尽显式返回 version, 绝不预先自增.
    std::expected<std::uint64_t, Error> next() const {
        idle();
        return position_ == UINT64_MAX ? std::expected<std::uint64_t, Error>(std::unexpected(Error::version)) : position_ + 1;
    }

    // 无准备事务时取得当前完整来源位置.
    std::uint64_t position() const {
        idle();
        return position_;
    }

    // 当前行/目录的逻辑计费, 外层用于跨来源预算; 活跃准备事务内不允许调用.
    std::size_t bytes() const {
        idle();
        return bytes_ + directory_;
    }

    // 直接点查, 未知 Scope/Key 不创建空目录, 返回共享载荷的原生副本.
    std::optional<Item> find(const Scope& scope, std::string_view key) const {

        idle();
        const auto* group = locate(scope); // 只借用已有目录, 不进行正文遍历.
        if (!group) {
            return std::nullopt;
        }
        const auto found = group->entries.find(key);
        return found == group->entries.end() ? std::nullopt : std::optional<Item>(*tree_.at(found->second));
    }

    // O(1) 捕获整个来源/域, 不是逐 Scope 复制快照向量.
    View capture() const {
        idle();
        return View(tree_.capture(), position_, bytes_ + directory_, gate_);
    }

    // 域提交锁内的内部枚举, 不另建带 Fence 的 View, 避免回调结束重锁同一个 mutex.
    // reader 借用共享名称和完整记录, 不得重入修改本来源或执行用户/网络回调.
    void each(auto&& reader) const {

        idle();
        for (const auto& [sector, spectra] : groups_) {
            for (const auto& [spectrum, group] : spectra) {
                for (const auto& [key, slot] : group.entries) {
                    const auto row = tree_.find(slot);
                    reader(*row.first, *row.second);
                }
            }
        }
    }

    // 准备一次单目标事实. position 非空必须是严格 +1; nullopt 仅供 Catalog/副本本地 TTL 维护, 不写来源历史.
    // 原生校验及最终计时由外层完成, 本类只验证地址/容量/连续位置. 失败不发布半个记录或预占版本.
    std::expected<Edit, Error> prepare(const Scope& scope, std::string_view key, std::optional<Item> record, std::optional<std::uint64_t> position, std::chrono::steady_clock::time_point now, Form form = Form::record) {

        idle();
        if (!scope.valid() || !Scope::text(key, 1024)) {
            return std::unexpected(Error::input);
        }
        if ((record && form == Form::erase) || (!record && (form == Form::data || form == Form::renew))) {
            return std::unexpected(Error::input);
        }
        if (position && (position_ == UINT64_MAX || *position != position_ + 1)) {
            return std::unexpected(Error::version);
        }
        Edit edit(*this); // 从此之后失败统一由 Edit 回滚准备责任.
        edit.group_ = locate(scope);
        const auto found = edit.group_ ? edit.group_->entries.find(key) : typename Table::iterator{};
        edit.old_ = edit.group_ && found != edit.group_->entries.end();
        if (edit.old_) {
            edit.slot_ = found->second;
            auto row = tree_.find(edit.slot_); // 一次页寻址同时取得原生记录与共享名称.
            edit.retired_.record = *row.second;
            edit.name_ = *row.first;
        }
        const auto before = edit.old_ ? weight(edit.name_->key, *edit.retired_.record) : std::optional<std::size_t>(0);
        const auto after = record ? weight(key, *record) : std::optional<std::size_t>(0);
        if (!before || !after || *after > limits_.bytes || bytes_ - *before > limits_.bytes - *after || (record && !edit.old_ && records_ == limits_.records)) {
            return std::unexpected(Error::capacity);
        }
        edit.bytes_ = bytes_ - *before + *after;
        const auto extra = record && !edit.group_ ? directory(scope) : 0; // 新 Scope 的目录成本, 不按每 Key 重复计费.
        if (directory_ > limits_.bytes || extra > limits_.bytes - directory_ || edit.bytes_ > limits_.bytes - directory_ - extra) {
            return std::unexpected(Error::capacity);
        }
        if (!edit.name_) {
            if (record && !edit.group_) {
                if (scopes_ == limits_.scopes) {
                    return std::unexpected(Error::capacity);
                }
                edit.group_ = &obtain(scope);
            }
            auto address = edit.group_ ? edit.group_->scope : std::make_shared<const Scope>(scope); // 缺失结束仍需要独立历史名称.
            edit.name_ = std::make_shared<const Name>(std::move(address), std::string(key));
        }
        if (record && !edit.old_) {
            edit.slot_ = tree_.next();
            edit.group_->entries.emplace(edit.name_->key, edit.slot_);
            edit.added_ = true;
        }
        if (record || edit.old_) {
            tree_.prepare(edit.slot_); // 只私有化路径, 尚不修改原生内容.
        }
        edit.retired_.name = edit.name_;
        edit.record_ = std::move(record);
        edit.position_ = position.value_or(position_);
        if (position && local_) {
            const auto metadata = sizeof(Event) + sizeof(Name) + edit.name_->key.size(); // 历史容器之外的条目元数据.
            const auto payload = edit.record_ ? measure_(*edit.record_) : 0;
            if (payload > std::numeric_limits<std::size_t>::max() - metadata) {
                return std::unexpected(Error::capacity);
            }
            const auto cost = metadata + payload;
            if (cost > std::numeric_limits<std::size_t>::max() - backlog_) {
                return std::unexpected(Error::capacity);
            }
            history_.push_back(Event{edit.name_, edit.record_, *position, edit.record_ ? form : Form::erase, cost, now});
            backlog_ += cost;
            edit.queued_ = true;
        }
        auto retained = history_.size(); // 计数包含待提交的尾项, 超预算时允许连新项一起淘汰.
        auto backlog = backlog_;
        while (edit.evicted_ < history_.size() && (retained > limits_.history || backlog > limits_.backlog || (now >= history_[edit.evicted_].stored && now - history_[edit.evicted_].stored >= limits_.retention))) {
            backlog -= history_[edit.evicted_].bytes;
            --retained;
            ++edit.evicted_;
        }
        edit.retired_.events.reserve(edit.evicted_); // 必须在提交前准备淘汰空间, commit 不分配.
        return edit;
    }

    // 在目标锁外创建空候选, 只读取构造后固定的计费/容量; 不隐式把新来源位置设为快照版本.
    Draft prepare(std::uint64_t position) const {
        return Draft(position, measure_, limits_);
    }

    // 仅远端/合并容器可准备内部保留式合并, 本机来源必须逐项连续编号.
    Batch prepare() {
        idle();
        if (local_) {
            throw std::logic_error("Local source cannot bypass ordered commits");
        }
        return Batch(*this);
    }

    // 仅远端来源接收完整基线, 允许同位置修复但不回退. 调用者先准备关联投影/水位/调度, 确认 complete 后一起发布.
    // 新旧根交换不遍历、不分配, 返回对象须在 gate 外回收. 不给自有来源引入任意改号入口.
    std::expected<Replaced, Error> reset(Draft&& draft) {

        idle();
        if (local_ || !draft.candidate_ || draft.candidate_->measure_ != measure_) {
            return std::unexpected(Error::input);
        }
        if (draft.position_ < position_ || (draft.position_ == 0 && draft.candidate_->records_ != 0)) {
            return std::unexpected(Error::version);
        }
        auto& candidate = *draft.candidate_; // 不存在对其页面的逃逸读者, 可安全移入目标同步域.
        static_assert(std::is_nothrow_swappable_v<Tree> && noexcept(groups_.swap(candidate.groups_)) && noexcept(history_.swap(candidate.history_)));
        if (candidate.records_ > limits_.records || candidate.scopes_ > limits_.scopes || candidate.directory_ > limits_.bytes || candidate.bytes_ > limits_.bytes - candidate.directory_) {
            return std::unexpected(Error::capacity);
        }
        groups_.swap(candidate.groups_);
        std::swap(tree_, candidate.tree_);
        history_.swap(candidate.history_);
        std::swap(records_, candidate.records_);
        std::swap(scopes_, candidate.scopes_);
        std::swap(bytes_, candidate.bytes_);
        std::swap(directory_, candidate.directory_);
        std::swap(backlog_, candidate.backlog_);
        position_ = draft.position_;
        return Replaced(std::move(draft.candidate_));
    }

    // 提取完整连续后缀, 结果共用载荷但拥有事件容器; 超前/断档不返回看似完整的局部结果.
    // since 为调用方完整安装的位置; 历史年龄只影响写入裁剪, 不在读取时使完整后缀失效.
    std::expected<std::vector<Event>, Error> replay(std::uint64_t since) const {

        idle();
        if (since > position_) {
            return std::unexpected(Error::version);
        }
        if (since == position_) {
            return std::vector<Event>{};
        }
        if (history_.empty() || history_.front().position - 1 > since) {
            return std::unexpected(Error::history);
        }
        const auto begin = std::upper_bound(history_.begin(), history_.end(), since, [](std::uint64_t value, const Event& event) { return value < event.position; });
        if (begin == history_.end()) {
            return std::unexpected(Error::history);
        }
        return std::vector<Event>(begin, history_.end());
    }

    // 捕获至多 count 项/bytes 逻辑字节的完整连续前缀. 首项超限明确 capacity, 不静默跳过或拆分提交.
    // 历史不足返回 O(1) 全来源基线, 未被裁剪的超龄历史仍可发送. 不提前增长 since, 网络发送和编码须在调用者释放 gate 后进行.
    std::expected<Delivery, Error> deliver(std::uint64_t since, std::size_t count, std::size_t bytes) const {

        idle();
        if (!local_ || count == 0 || bytes == 0) {
            return std::unexpected(Error::input);
        }
        if (since > position_) {
            return std::unexpected(Error::version);
        }
        Delivery result{position_, {}, {}}; // 没有尾部时不分配事件向量.
        if (since == position_) {
            return result;
        }
        const auto begin = std::upper_bound(history_.begin(), history_.end(), since, [](std::uint64_t value, const Event& event) { return value < event.position; });
        if (begin == history_.end() || begin->position - 1 != since) {
            result.baseline.emplace(capture());
            return result;
        }

        std::size_t used{}; // 载荷引用也按完整原生事件保守计费, 不只计 vector 元素.
        auto end = begin;
        for (; end != history_.end() && static_cast<std::size_t>(end - begin) < count; ++end) {
            if (end->bytes > bytes - used) {
                if (end == begin) {
                    return std::unexpected(Error::capacity);
                }
                break;
            }
            used += end->bytes;
        }
        result.events.assign(begin, end); // 精确分配本轮完整前缀, 不预留 count 指定的任意巨大容量.
        return result;
    }

private:
    // 两级目录、共享地址和空桶的保守预留, 标识已通过 128 字节上限校验, 相加不会溢出.
    static std::size_t directory(const Scope& scope) noexcept {
        return 512 + 2 * (scope.sector.size() + scope.spectrum.size());
    }

    // 防止同一准备事务中误读临时目录或再开另一个事务, 不用它代替外部互斥锁.
    void idle() const {
        if (editing_) {
            throw std::logic_error("Source has an unfinished edit");
        }
    }

    // 保守计量一条原生行, 包含页/目录/Name 基础开销; Scope 目录单独受 scopes 数量上限约束.
    std::optional<std::size_t> weight(std::string_view key, const Item& record) const noexcept {

        const auto metadata = sizeof(Item) + sizeof(Name) + key.size() + 128; // 固定索引/控制块预留, 不是 allocator 精确承诺.
        const auto payload = measure_(record);                                // 原生类型提供的无分配载荷计费.
        return payload > std::numeric_limits<std::size_t>::max() - metadata ? std::nullopt : std::optional<std::size_t>(metadata + payload);
    }

    // 直接查找两个名称, 未知范围不分配; 调用者保持 gate 和组对象寿命.
    Group* locate(const Scope& scope) {
        const auto sector = groups_.find(scope.sector);
        if (sector == groups_.end()) {
            return nullptr;
        }
        const auto spectrum = sector->second.find(scope.spectrum);
        return spectrum == sector->second.end() ? nullptr : &spectrum->second;
    }

    // 只读查找复用相同目录规则, 不通过 const_cast 暴露可变状态.
    const Group* locate(const Scope& scope) const {
        const auto sector = groups_.find(scope.sector);
        if (sector == groups_.end()) {
            return nullptr;
        }
        const auto spectrum = sector->second.find(scope.spectrum);
        return spectrum == sector->second.end() ? nullptr : &spectrum->second;
    }

    // 在已检查 scopes 容量后准备空组, 分配失败不遗留新建的空 Sector.
    Group& obtain(const Scope& scope) {

        auto address = std::make_shared<const Scope>(scope);
        auto [sector, created] = groups_.try_emplace(scope.sector);
        try {
            auto [spectrum, inserted] = sector->second.try_emplace(scope.spectrum, Group{std::move(address), {}});
            scopes_ += inserted;
            if (inserted) {
                directory_ += directory(scope);
            }
            return spectrum->second;
        } catch (...) {
            if (created) {
                groups_.erase(sector);
            }
            throw;
        }
    }

    // 只回收空 Scope 目录, 不修改来源位置或尚存活快照/历史拥有的地址.
    void prune(const Scope& scope) noexcept {

        const auto sector = groups_.find(scope.sector);
        if (sector == groups_.end()) {
            return;
        }
        const auto spectrum = sector->second.find(scope.spectrum);
        if (spectrum != sector->second.end() && spectrum->second.entries.empty()) {
            directory_ -= directory(scope); // 在释放 Group 的最后 Scope 引用前计算.
            sector->second.erase(spectrum);
            --scopes_;
        }
        if (sector->second.empty()) {
            groups_.erase(sector);
        }
    }

    std::size_t directory_{};                // 两级目录保守计费, 与当前行计费共同受总 bytes 约束.
    const std::shared_ptr<std::mutex> gate_; // 外层统一提交锁, View 独立保持同步寿命.
    const Measure measure_;                  // 固定的原生计费函数, 构造后不改变规则.
    const bool local_;                       // 仅自有来源保留发送历史.
    const Limits limits_;                    // 来源级静态预算.
    Tree tree_;                              // 原生记录的完整来源根.
    Groups groups_;                          // 两级目录先析构, 借用的键文本由后析构的 tree_ 保持有效.
    std::deque<Event> history_;              // 有界连续本机来源历史, 远端组为空.
    std::uint64_t position_{};               // 全 Scope 共享的唯一来源位置.
    std::size_t records_{};                  // 当前原生行数, 初始零.
    std::size_t scopes_{};                   // 当前非空或本次准备的 Scope 数.
    std::size_t bytes_{};                    // 当前行的保守计费, 初始零.
    std::size_t backlog_{};                  // 保留历史的保守计费, 包含尚未提交尾项时由 editing_ 阻止观察.
    bool editing_{};                         // 一次只允许一个 prepare/commit, 不是跨线程原子标记.
};
} // namespace astra
