#pragma once
#include "pages.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace astra {
// 单 Scope 的公开内容投影, 不拥有业务版本规则、期限或来源位置. Item 不含隐藏 TTL/order.
// Name 复用 Origin 的共享地址/Key, 不复制键或载荷; 写入和 Edit 寿命持同一个域 gate 独占锁, const 读取持共享锁.
// 两阶段提交要求 Item 复制构造和移动赋值无异常, 在模板使用边界拒绝可能中断发布的类型.
template <typename Item, typename Name>
    requires(std::is_nothrow_copy_constructible_v<Item> && std::is_nothrow_move_assignable_v<Item>)
class Scene {
public:
    using Tree = Pages<Item, Name>;                        // 只包含公开内容的分页根, 续租不触碰此树.
    using Key = typename Tree::Key;                        // shared_ptr<const Name>, 移动 Event 只转移所有权, 不移动 Name 或其 SSO 字符串.
    using Measure = std::size_t (*)(const Item&) noexcept; // 只计载荷的有界无分配函数, 元数据另行计费.

    // 单范围的保守内容/历史上限, 外层仍负责活动范围和总恢复预算.
    struct Limits {
        std::size_t records = 65536;                                              // 可见记录数, 零只允许空范围.
        std::size_t bytes = 64 * 1024 * 1024;                                     // 当前内容和索引的逻辑字节, 不是 RSS.
        std::size_t history = 4096;                                               // 连续可见变更数, 零禁用旧游标恢复.
        std::size_t backlog = 8 * 1024 * 1024;                                    // 历史逻辑字节上限, 包含共享载荷的保守引用计费.
        std::chrono::steady_clock::duration retention = std::chrono::minutes(10); // 写入时按年龄裁剪历史, 零提交后即淘汰; 未淘汰的连续后缀仍可读取.
    };

    // 本类只报告投影准备问题, 不将业务失败伪装成游标错误.
    enum class Error {
        // 缺失名称、非法 Key 或删除未存在的投影; 上层不应为无可见变化生成事件.
        input,
        // 当前内容或计费超过局部预算.
        capacity,
        // 游标耗尽或请求的恢复位置领先当前范围.
        version,
        // 不能提供完整连续后缀, 必须重新捕获基线.
        history
    };

    // 单次已完成的可见变化, 旧基线已经具备 Attr 时编码器才可选择 data-only.
    struct Event {
        Key name;                                       // 地址/Key 的固定共享所有权.
        std::optional<Item> record;                     // 完整公开内容; 空表示删除, 空 Buffer 仍是存在记录.
        std::uint64_t version{};                        // 此 Scope 的连续视图游标, 与来源位置独立.
        bool data{};                                    // Ephemeris 仅 Data 变化的提示, 不能替代基线存在性证明.
        std::size_t bytes{};                            // 本项保守计费, 不包含临时网络编码.
        std::chrono::steady_clock::time_point stored{}; // 只用于历史窗口, 不进入公开内容.
    };

    // 精确查找同边界取得名称/内容/游标, 空 record 对应明确缺项.
    struct Point {
        Key name;                   // 有记录才非空; 调度器可以比较其地址识别来源, 无字符串拼接.
        std::optional<Item> record; // 不含任何业务期限, 共享原生载荷.
        std::uint64_t version{};    // 即使缺项仍返回当前 Scope 游标.
    };

    // O(1) 冻结根, View 不借用 Scene 或网络服务, 可以越过所属对象寿命.
    class View {
    public:
        // 捕获时的完整可见游标, 初始空范围为零.
        std::uint64_t version() const noexcept {
            return version_;
        }

        // 捕获时记录数, 不重新访问当前可复用页.
        std::size_t size() const noexcept {
            return size_;
        }

        // 捕获时保守当前内容字节, 不将逻辑预算称为硬内存上限.
        std::size_t bytes() const noexcept {
            return bytes_;
        }

        // 回调借用 Key/公开记录, 可以重入已解锁的业务对象并抛异常; 保存内容时复制所有权.
        void each(auto&& reader) const {

            const auto root = root_;  // 本次读区持有独立根, 先于 Fence 构造.
            const Fence fence{gate_}; // 正常/异常退出都先同步再释放读区根.
            root.each([&reader](const Name& name, const Item& record) { reader(name.key, record); });
        }

        // 返回实际消费行数, offset 在 0..size, reader=false 在当前项前停下; 不重复遍历分页前缀.
        std::size_t page(std::size_t offset, std::size_t maximum, auto&& reader) const {

            const auto root = root_;
            const Fence fence{gate_};
            return root.page(offset, maximum, [&reader](const Name& name, const Item& record) { return reader(name.key, record); });
        }

    private:
        friend class Scene;

        // 写者共用的读完成同步, shared_ptr::use_count 本身不能建立内存屏障.
        struct Fence {
            std::shared_ptr<std::shared_mutex> gate; // 独立延长原同步域寿命.

            ~Fence() {
                const std::shared_lock lock(*gate);
            } // 最后一次读取后经过写者锁.
        };

        // 只在域锁内捕获, 不分配或遍历; size 同根固定.
        View(typename Tree::View root, std::uint64_t version, std::size_t bytes, std::shared_ptr<std::shared_mutex> gate) : root_(std::move(root)), version_(version), size_(root_.size()), bytes_(bytes), gate_(std::move(gate)) {}

        typename Tree::View root_;                // 完整公开内容根.
        std::uint64_t version_{};                 // 与 root 同边界的视图游标.
        std::size_t size_{};                      // 捕获计数, 初始合法为零.
        std::size_t bytes_{};                     // 捕获计费, 不随当前写入改变.
        std::shared_ptr<std::shared_mutex> gate_; // 只保留同步对象, 不保持服务或 Scope 存活.
    };

    // 原子提交返回通知与旧资源; 在域锁内使用 event 通知内部收集器, 锁外释放整体.
    struct Retired {
        Event event;                // 即使历史预算为零仍有真实提交通知, 不依赖 replay 找回它.
        std::optional<Item> record; // 被覆盖的旧记录, 避免最后载荷引用在锁内释放.
        std::vector<Event> events;  // 准备阶段预留的淘汰空间, commit 不分配.
    };

    // 单目标投影准备, 与来源 Edit 同时持有后才一起发布, 失败自动撤销尚未生效的索引/历史.
    class Edit {
    public:
        // 未提交时回滚, 要求 Scene 与同一域锁覆盖整个寿命.
        ~Edit() {
            abort();
        }

        // 转交唯一提交责任, 原事务不再执行回滚.
        Edit(Edit&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), slot_(other.slot_), bytes_(other.bytes_), evicted_(other.evicted_), added_(other.added_), queued_(other.queued_), retired_(std::move(other.retired_)) {}

        // 不覆盖仍未提交的事务.
        Edit& operator=(Edit&&) = delete;
        // 不复制回滚责任.
        Edit(const Edit&) = delete;
        // 不复制发布责任.
        Edit& operator=(const Edit&) = delete;

        // 无异常更新内容、查找表、游标和历史; 外层仍在同一域锁内完成来源提交/调度/通知.
        Retired commit() noexcept {

            auto& owner = *owner_;        // 调用一次后失效, 不允许二次 commit.
            auto& event = retired_.event; // 已准备且不会分配的完整新事件.
            if (event.record) {
                // 新项先让树持有同一个不可变 Name, 再淘汰历史; 零历史预算也不影响 entries_ 的键寿命.
                owner.tree_.set(slot_, added_ ? event.name : nullptr, *event.record);
            } else {
                owner.tree_.erase(slot_);
                owner.entries_.erase(event.name->key);
            }
            owner.bytes_ = bytes_;
            owner.version_ = event.version;
            for (std::size_t index = 0; index < evicted_; ++index) {
                owner.backlog_ -= owner.history_.front().bytes;
                retired_.events.push_back(std::move(owner.history_.front()));
                owner.history_.pop_front();
            }
            owner.editing_ = false;
            owner_ = nullptr;
            return std::move(retired_);
        }

    private:
        friend class Scene;

        // 从此之后所有失败由析构统一撤销, 不抢先增长公开游标.
        explicit Edit(Scene& owner) : owner_(&owner) {
            owner.editing_ = true;
        }

        // 只移除本次新增查找项/历史尾, 页面 prepare 没有改变逻辑内容.
        void abort() noexcept {

            if (!owner_) {
                return;
            }
            if (queued_) {
                owner_->backlog_ -= owner_->history_.back().bytes;
                owner_->history_.pop_back();
            }
            if (added_) {
                // retired_.event 此时仍持有 Name; 先撤销借用索引, 成员随后析构才释放名称.
                owner_->entries_.erase(retired_.event.name->key);
            }
            owner_->editing_ = false;
            owner_ = nullptr;
        }

        Scene* owner_{};        // 未提交事务的稳定所属对象, 不允许移动 Scene.
        std::uint64_t slot_{};  // 目标页槽, prepare 后已经可无分配修改.
        std::size_t bytes_{};   // 提交后的当前内容计费.
        std::size_t evicted_{}; // 可包含新项自身的历史淘汰数.
        bool added_{};          // 是否已暂存新查找项, 析构需要撤销.
        bool queued_{};         // 是否已暂存历史尾项, 析构需要撤销.
        Retired retired_;       // 固定通知以及锁外释放的旧内容.
    };

    // 来源整组替换时的一个 Scope 投影候选. 仅克隆被修改的页, 不复制全量 Key 哈希表.
    // 它不是外部多 Key 写 API; 外层同一域锁覆盖准备和最终提交, 其他 Scope 可同时持有自己的候选.
    class Batch {
    public:
        // 旧根/旧历史及完整提交通知交给锁外回收, 不在根交换时销毁大批正文.
        struct Retired {
            Tree root;                                  // 原投影根, 老 View 可以继续独立保持它.
            std::unique_ptr<std::deque<Event>> history; // 原有界历史容器, 不复制业务载荷字节.
            std::vector<Event> events;                  // 本次真正可见变化的连续事件, 用于内部通知.
        };

        // 放弃候选只撤销新查找项, 原投影根/游标/历史未改变.
        ~Batch() {
            abort();
        }

        // 唯一候选责任可移动进有界 Scope 计划, 原候选不再回滚.
        Batch(Batch&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), tree_(std::move(other.tree_)), history_(std::move(other.history_)), events_(std::move(other.events_)), added_(std::move(other.added_)), removed_(std::move(other.removed_)), seen_(std::move(other.seen_)), bytes_(other.bytes_), backlog_(other.backlog_), version_(other.version_), retention_(other.retention_), failed_(other.failed_) {}

        // 不覆盖一个尚未结束的准备责任.
        Batch& operator=(Batch&&) = delete;
        // 不复制暂存查找项的回滚责任.
        Batch(const Batch&) = delete;
        // 不复制赋值.
        Batch& operator=(const Batch&) = delete;

        // 只读原投影目标, 同一批内每个 Key 至多修改一次, 输入计划必须先合并同目标变化.
        Point find(std::string_view key) const {
            const auto found = owner_->entries_.find(key);
            if (found == owner_->entries_.end()) {
                return {{}, {}, owner_->version_};
            }
            const auto row = owner_->tree_.find(found->second);
            return row.first && (*row.first)->key == key ? Point{*row.first, *row.second, owner_->version_} : Point{{}, {}, owner_->version_};
        }

        // 完整候选中每 Key 一次可见 Set/Erase; 失败后只能析构整批, 不能提交此前的部分准备.
        std::expected<void, Error> set(Key name, std::optional<Item> record, std::chrono::steady_clock::time_point now, bool data = false) {

            if (failed_ || !owner_ || !name || name->key.empty() || name->key.size() > 1024) {
                failed_ = true;
                return std::unexpected(Error::input);
            }
            failed_ = true; // 任意分配异常和失败返回均使整份候选不可提交.
            if (version_ == UINT64_MAX) {
                return std::unexpected(Error::version);
            }
            const auto found = owner_->entries_.find(name->key);
            const bool exists = found != owner_->entries_.end();
            if ((!record && !exists) || (data && (!record || !exists)) || seen_.contains(name->key)) {
                return std::unexpected(Error::input);
            }
            auto slot = exists ? found->second : tree_.next();
            const auto old = exists ? owner_->tree_.find(slot) : std::pair<const Key*, const Item*>{};
            if (exists) {
                name = *old.first; // 沿用原公开名称, 不在整组替换时复制所有字符串.
            }
            const auto before = exists ? owner_->weight(name->key, *old.second) : std::optional<std::size_t>(0);
            const auto after = record ? owner_->weight(name->key, *record) : std::optional<std::size_t>(0);
            if (!before || !after || *after > owner_->limits_.bytes || bytes_ - *before > owner_->limits_.bytes - *after || (!exists && owner_->entries_.size() - removed_.size() == owner_->limits_.records)) {
                return std::unexpected(Error::capacity);
            }
            const auto metadata = sizeof(Event) + sizeof(Name) + name->key.size();
            const auto payload = record ? owner_->measure_(*record) : 0;
            if (payload > std::numeric_limits<std::size_t>::max() - metadata || metadata + payload > std::numeric_limits<std::size_t>::max() - backlog_) {
                return std::unexpected(Error::capacity);
            }
            const auto cost = metadata + payload;
            events_.push_back(Event{name, record, version_ + 1, data, cost, now}); // 先固定所有权, 后续 string_view 始终有效.
            seen_.insert(name->key);
            if (!exists) {
                added_.push_back(name); // emplace 之前预留回滚所有权, 分配失败也不会泄漏已插入目录.
                owner_->entries_.emplace(name->key, slot);
            } else if (!record) {
                removed_.push_back(name); // 原查找项等整批提交时才真正删除.
            }
            tree_.prepare(slot);
            if (record) {
                tree_.set(slot, exists ? nullptr : name, *record);
            } else {
                tree_.erase(slot);
            }
            history_->push_back(events_.back());
            backlog_ += cost;
            while (!history_->empty() && (history_->size() > owner_->limits_.history || backlog_ > std::min(owner_->limits_.backlog, retention_) || (now >= history_->front().stored && now - history_->front().stored >= owner_->limits_.retention))) {
                backlog_ -= history_->front().bytes;
                history_->pop_front(); // 这里是未发布候选历史, 不能操作 owner 的已确认历史.
            }
            bytes_ = bytes_ - *before + *after;
            ++version_;
            failed_ = false;
            return {};
        }

        // 所有 Scope/来源/调度候选准备成功后才调用, 无分配交换并发布完整连续变化.
        Retired commit() noexcept {

            assert(owner_ && !failed_);
            owner_->history_.swap(*history_); // 候选容器已提前分配, commit 不默认构造可能分配的 deque.
            Retired retired{std::move(owner_->tree_), std::move(history_), std::move(events_)};
            owner_->tree_ = std::move(tree_);
            for (const auto& name : removed_) {
                owner_->entries_.erase(name->key);
            }
            owner_->bytes_ = bytes_;
            owner_->backlog_ = backlog_;
            owner_->version_ = version_;
            owner_->editing_ = false;
            owner_ = nullptr;
            return retired;
        }

        // 用于跨 Scope 最终预算核对, 只包含候选真实保留的历史.
        std::size_t history() const noexcept {
            return backlog_;
        }

    private:
        friend class Scene;

        // 只复制一个根及有界旧历史, 键目录仍借用 owner; 构造分配失败时不设置 editing_.
        Batch(Scene& owner, std::size_t retention) : owner_(&owner), tree_(owner.tree_), history_(std::make_unique<std::deque<Event>>(owner.history_)), bytes_(owner.bytes_), backlog_(owner.backlog_), version_(owner.version_), retention_(retention) {
            owner.editing_ = true;
        }

        // 根始终未发布, 只移除本次新查找项, 删除/覆盖旧项无须逆向补偿.
        void abort() noexcept {
            if (owner_) {
                for (const auto& name : added_) {
                    owner_->entries_.erase(name->key);
                }
                owner_->editing_ = false;
                owner_ = nullptr;
            }
        }

        Scene* owner_{};                             // 唯一准备责任, commit/移动后置空.
        Tree tree_;                                  // 候选 COW 根, 与 owner 共用 gate 保持读完成同步.
        std::unique_ptr<std::deque<Event>> history_; // 候选有界历史, 失败不影响 owner 旧历史.
        std::vector<Event> events_;                  // 本批完整可见事件, 名称所有权覆盖下列借用索引.
        std::vector<Key> added_;                     // 已准备的新增目录项, 失败需撤销.
        std::vector<Key> removed_;                   // 提交时才删除的旧目录项, 不借助悬空字符串.
        std::unordered_set<std::string_view> seen_;  // 每 Key 一次, 不复制键文本.
        std::size_t bytes_{};                        // 候选内容计费.
        std::size_t backlog_{};                      // 候选保留历史计费.
        std::uint64_t version_{};                    // 候选完整游标, 失败不会提前暴露.
        std::size_t retention_{};                    // 外层分配给本 Scope 的下游历史额度.
        bool failed_{};                              // 单次 set 失败/异常使整批只允许放弃.
    };

    // gate/measure 非空, limits 固定; 不读取 Clock、不启动线程或隐式建立来源身份.
    Scene(std::shared_ptr<std::shared_mutex> gate, Measure measure, Limits limits) : gate_(std::move(gate)), measure_(measure), limits_(limits) {
        if (!gate_ || !measure_ || limits_.retention < decltype(limits_.retention)::zero()) {
            throw std::invalid_argument("Invalid projection configuration");
        }
    }

    // 默认局部容量, 构造即为合法游标零的空本地投影.
    Scene(std::shared_ptr<std::shared_mutex> gate, Measure measure) : Scene(std::move(gate), measure, Limits{}) {}

    // View 可越过本对象寿命, Edit 和直接借用不可以.
    ~Scene() = default;
    // 不复制提交边界/游标/借用键目录.
    Scene(const Scene&) = delete;
    // 不覆盖仍有读者的同步域.
    Scene& operator=(const Scene&) = delete;

    // 未创建范围的零版本基线, 只保留同步域, 不分配页、历史或永久目录名额.
    static View empty(std::shared_ptr<std::shared_mutex> gate) {

        if (!gate) {
            throw std::invalid_argument("Empty projection requires a synchronization gate");
        }

        return View(Tree{}.capture(), 0, 0, std::move(gate));
    }

    // 无分配捕获公开根, 外层须先按当前业务时间推进到期并挂入后续观察.
    View capture() const {
        idle();
        return View(tree_.capture(), version_, bytes_, gate_);
    }

    // 未知目标也返回当前 Scope 游标, 不创建节点或构建整表快照.
    Point find(std::string_view key) const {

        idle();
        const auto found = entries_.find(key); // string_view 直接查找, 不分配 UUID/Key 临时文本.
        if (found == entries_.end()) {
            return {{}, {}, version_};
        }
        const auto row = tree_.find(found->second); // 一次页寻址取得原生共享名称和公开记录.
        return {*row.first, *row.second, version_};
    }

    // 本 Scope 已保留历史计费, 用于外层跨 Scope 共用预算; 准备事务内不允许观察暂存尾项.
    std::size_t history() const {
        idle();
        return backlog_;
    }

    // 仅内部来源替换使用批量候选, 普通业务写入继续使用单目标 Edit 热路径.
    Batch prepare(std::size_t retention) {
        idle();
        return Batch(*this, retention);
    }

    // 只为真实内容变化调用; record 空必须对应已有目标. data=true 只允许已有完整记录的更新.
    std::expected<Edit, Error> prepare(Key name, std::optional<Item> record, std::chrono::steady_clock::time_point now, bool data = false, std::size_t retention = std::numeric_limits<std::size_t>::max()) {

        idle();
        if (!name || name->key.empty() || name->key.size() > 1024) {
            return std::unexpected(Error::input);
        }
        if (version_ == UINT64_MAX) {
            return std::unexpected(Error::version);
        }
        const auto found = entries_.find(name->key); // 在修改目录前完成可见存在性和 Data-only 形状检查.
        const bool exists = found != entries_.end();
        if ((!record && !exists) || (data && (!record || !exists))) {
            return std::unexpected(Error::input);
        }
        Edit edit(*this); // 后续所有可失败准备由 RAII 回滚, 不发布半份投影.
        edit.retired_.event = Event{std::move(name), std::move(record), version_ + 1, data, 0, now};
        auto& event = edit.retired_.event;
        if (exists) {
            edit.slot_ = found->second;
            const auto row = tree_.find(edit.slot_);
            event.name = *row.first; // 已有项保留原名称, 不在每次覆盖重新分配字符串.
            edit.retired_.record = *row.second;
        }
        const auto before = exists ? weight(event.name->key, *edit.retired_.record) : std::optional<std::size_t>(0);
        const auto after = event.record ? weight(event.name->key, *event.record) : std::optional<std::size_t>(0);
        if (!before || !after || *after > limits_.bytes || bytes_ - *before > limits_.bytes - *after || (!exists && entries_.size() == limits_.records)) {
            return std::unexpected(Error::capacity);
        }
        edit.bytes_ = bytes_ - *before + *after;
        if (!exists) {
            edit.slot_ = tree_.next();
            // 借用堆上不可变 Name 内的字符, 不是 Event 对象内的字符. 准备期由 edit 保活, commit 后由树保活.
            entries_.emplace(event.name->key, edit.slot_);
            edit.added_ = true;
        }
        tree_.prepare(edit.slot_); // COW 只复制被共享的必要路径, 续租路径根本不调用此方法.

        const auto metadata = sizeof(Event) + sizeof(Name) + event.name->key.size(); // 固定元数据有 Key 上限约束.
        const auto payload = event.record ? measure_(*event.record) : 0;
        if (payload > std::numeric_limits<std::size_t>::max() - metadata || metadata + payload > std::numeric_limits<std::size_t>::max() - backlog_) {
            return std::unexpected(Error::capacity);
        }
        event.bytes = metadata + payload;
        history_.push_back(event);
        backlog_ += event.bytes;
        edit.queued_ = true;
        auto retained = history_.size(); // 包含准备的新项, 允许超预算单项提交后不保留.
        auto backlog = backlog_;
        while (edit.evicted_ < history_.size() && (retained > limits_.history || backlog > std::min(limits_.backlog, retention) || (now >= history_[edit.evicted_].stored && now - history_[edit.evicted_].stored >= limits_.retention))) {
            backlog -= history_[edit.evicted_].bytes;
            --retained;
            ++edit.evicted_;
        }
        edit.retired_.events.reserve(edit.evicted_);
        return edit;
    }

    // 完整连续后缀, 条数/载荷受历史预算限制; 当前游标返回空, 断档不能返回部分成功.
    // since 为调用方完整安装的位置; 年龄仅参与写入裁剪, 不因空闲超时拒绝仍保留的连续历史.
    std::expected<std::vector<Event>, Error> replay(std::uint64_t since) const {

        idle();
        if (since > version_) {
            return std::unexpected(Error::version);
        }
        if (since == version_) {
            return std::vector<Event>{};
        }
        if (history_.empty() || history_.front().version - 1 > since) {
            return std::unexpected(Error::history);
        }
        const auto first = std::upper_bound(history_.begin(), history_.end(), since, [](std::uint64_t value, const Event& event) { return value < event.version; });
        if (first == history_.end()) {
            return std::unexpected(Error::history);
        }
        return std::vector<Event>(first, history_.end());
    }

private:
    // 事务中不能读到暂存的查找项/历史, 不将此标志视为线程同步替代品.
    void idle() const {
        if (editing_) {
            throw std::logic_error("Projection has an unfinished edit");
        }
    }

    // 计费采用受 Key 长度约束的元数据及公开载荷, 溢出拒绝而非回绕.
    std::optional<std::size_t> weight(std::string_view key, const Item& record) const noexcept {
        const auto metadata = sizeof(Item) + sizeof(Name) + key.size() + 128; // 索引/控制块保守预留.
        const auto payload = measure_(record);                                // 不访问网络或再分配.
        return payload > std::numeric_limits<std::size_t>::max() - metadata ? std::nullopt : std::optional<std::size_t>(metadata + payload);
    }

    const std::shared_ptr<std::shared_mutex> gate_;               // 来源与投影统一的域提交锁.
    const Measure measure_;                                       // 固定公开载荷计费函数.
    const Limits limits_;                                         // 不按客户端请求动态改变的局部预算.
    Tree tree_;                                                   // 持有名称, 析构晚于借用名称的 entries_.
    std::unordered_map<std::string_view, std::uint64_t> entries_; // 精确 Key -> 页槽; 活动 Name 由 tree_ 持有, 暂存新增项由 Edit/Batch 持有.
    std::deque<Event> history_;                                   // 独立于来源历史的连续可见变化.
    std::uint64_t version_{};                                     // 删除到空仍保留游标, 从不回收重用.
    std::size_t bytes_{};                                         // 当前可见内容的逻辑计费.
    std::size_t backlog_{};                                       // 当前保留历史计费.
    bool editing_{};                                              // 同一时刻至多一个两阶段修改.
};
} // namespace astra
