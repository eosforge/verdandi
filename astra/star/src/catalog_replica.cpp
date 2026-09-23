#include "catalog_state.hpp"

namespace astra {
Catalog::State::Guard Catalog::State::acquire(std::string_view id, std::unique_lock<std::shared_mutex>& domain) const {

    const auto found = replicas_.find(id); // 只在域锁内使用迭代器, 守卫自行保持来源寿命.
    Guard result(found == replicas_.end() ? nullptr : found->second, domain);
    const auto current = replicas_.find(id); // 等待期间可能退役并回收, 不能把旧对象当成新准入来源.
    if (current == replicas_.end() || current->second.get() != result.get())
        return Guard(nullptr, domain);
    return result;
}

Catalog::State::Hooks::~Hooks() {
    if (!committed) {
        for (const auto* name : added) {
            owner.deadlines_.erase(name);
        }
    }
}

void Catalog::State::Hooks::add(const Source::Tree::Key& name) {
    if (!owner.deadlines_.contains(name.get())) {
        added.push_back(name.get()); // 先登记责任, 即使后续分配失败也可安全回滚.
        owner.deadlines_.emplace(name.get(), std::make_unique<Timer>(name));
    }
}

Catalog::State::Projection* Catalog::State::locate(const Scope& scope) {
    const auto sector = scenes_.find(scope.sector); // 内部查找不创建空范围或新的公开游标.
    if (sector == scenes_.end()) {
        return nullptr;
    }
    const auto spectrum = sector->second.find(scope.spectrum);
    return spectrum == sector->second.end() ? nullptr : &spectrum->second;
}

std::expected<void, Catalog::State::Error> Catalog::State::admit(std::string_view id) {

    if (!Scope::text(id, 1024)) {
        return std::unexpected(Error::input);
    }
    const std::lock_guard lock(*gate_);
    if (const auto found = replicas_.find(id); found != replicas_.end()) {
        return found->second->retired ? std::unexpected(Error::version) : std::expected<void, Error>{};
    }
    if (replicas_.size() == limits_.replicas) {
        return std::unexpected(Error::capacity);
    }
    replicas_.emplace(std::string(id), std::make_shared<Replica>(gate_, limits_.source));
    return {};
}

void Catalog::State::retire(std::string_view id) {
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found != replicas_.end()) {
        found->second->retired = true;
        due_ = Clock::Time{};
        found->second->coverage.clear(); // 不再处理此旧身份后缀, 但不更改任何记录的既有期限.
    }
}

std::expected<std::uint64_t, Catalog::State::Error> Catalog::State::received(std::string_view id) const {
    std::unique_lock lock(*gate_);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    return !borrowed.get() ? std::expected<std::uint64_t, Error>(std::unexpected(Error::input)) : borrowed.get()->source.position();
}

std::expected<Catalog::State::Source::Draft, Catalog::State::Error> Catalog::State::prepare(std::string_view id, std::uint64_t position) {

    std::unique_lock lock(*gate_);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get()) {
        return std::unexpected(Error::input);
    }
    const auto& replica = *borrowed.get();
    if (replica.retired || position < replica.source.position() || std::ranges::any_of(replica.coverage, [position](const Replica::Coverage& value) { return value.position > position; })) {
        return std::unexpected(Error::version);
    }
    return replica.source.prepare(position);
}

std::expected<std::optional<Catalog::Record>, Catalog::State::Error> Catalog::State::replica(std::string_view id, const Scope& scope, std::string_view key) const {

    if (!scope.valid() || !Scope::text(key, 1024)) {
        return std::unexpected(Error::input);
    }
    std::unique_lock lock(*gate_);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get()) {
        return std::unexpected(Error::input);
    }
    return borrowed.get()->source.find(scope, key); // 仅返回来源事实, 不从 merged_ 借正文或较晚截止.
}

std::expected<Catalog::State::Point, Catalog::State::Error> Catalog::State::resolve(const Scope& scope, std::string_view key) {

    if (!scope.valid() || !Scope::text(key, 1024)) {
        return std::unexpected(Error::input);
    }
    const std::shared_lock lock(*export_); // 只读取自有来源事实及连续位置, 已过期正文由接收端验证绝对截止.
    return Point{source_.position(), source_.find(scope, key)};
}

std::expected<bool, Catalog::State::Error> Catalog::State::covered(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key) {

    if (!scope.valid() || !Scope::text(key, 1024) || position == 0) {
        return std::unexpected(Error::input);
    }
    Source::Retired retired;
    std::unique_lock lock(*gate_);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get()) {
        return std::unexpected(Error::input);
    }
    auto& replica = *borrowed.get();
    if (replica.retired) {
        return std::unexpected(Error::version);
    }
    if (position <= replica.source.position()) {
        return true;
    }
    if (replica.source.position() == UINT64_MAX || position != replica.source.position() + 1) {
        return std::unexpected(Error::history);
    }
    if (std::ranges::none_of(replica.coverage, [&](const Replica::Coverage& value) { return *value.name->scope == scope && (value.name->key.empty() || value.name->key == key) && value.position >= position; })) {
        return false;
    }
    // 编码适配层先询问覆盖, 避免在原生正文已到期时又为被覆盖的 Data/Renew 发起回补.
    auto skipped = replica.source.prepare(scope, key, replica.source.find(scope, key), position, std::chrono::steady_clock::now());
    if (!skipped) {
        return std::unexpected(error(skipped.error()));
    }
    retired = skipped->commit();
    std::erase_if(replica.coverage, [position](const Replica::Coverage& value) { return value.position <= position; });
    return true;
}

std::expected<void, Catalog::State::Error> Catalog::State::apply(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key, Record record, Source::Form form) {
    return receive(id, position, scope, key, std::move(record), form, false);
}

std::expected<void, Catalog::State::Error> Catalog::State::repair(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key, std::optional<Record> record) {
    return receive(id, position, scope, key, std::move(record), Source::Form::record, true);
}

std::expected<void, Catalog::State::Error> Catalog::State::receive(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key, std::optional<Record> record, Source::Form form, bool repair) {

    if (!scope.valid() || !Scope::text(key, 1024) || position == 0 || (record && !Catalog::valid(*record)) || (!repair && !record) || (form != Source::Form::record && form != Source::Form::renew)) {
        return std::unexpected(Error::input);
    }
    std::vector<Retired> expired; // 所有大块旧引用在 gate 之后析构.
    Retired retired;
    std::unique_lock lock(*gate_);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get()) {
        return std::unexpected(Error::input);
    }
    auto& replica = *borrowed.get();
    if (replica.retired) {
        return std::unexpected(Error::version);
    }
    if (position <= replica.source.position()) {
        return {}; // 重复前缀不能刷新 TTL 或反向覆盖本地较新事实.
    }
    if (!repair && (replica.source.position() == UINT64_MAX || position != replica.source.position() + 1)) {
        return std::unexpected(Error::history);
    }
    const auto coverage = std::ranges::find_if(replica.coverage, [&](const Replica::Coverage& value) { return *value.name->scope == scope && value.name->key == key; });
    auto covered = coverage == replica.coverage.end() ? 0 : coverage->position; // 精确补项可晚于范围基线, 取两者最大覆盖位置.
    for (const auto& value : replica.coverage) {
        if (*value.name->scope == scope && value.name->key.empty())
            covered = std::max(covered, value.position);
    }
    const bool precise = coverage != replica.coverage.end();
    const auto index = static_cast<std::size_t>(coverage - replica.coverage.begin()); // reserve 前只保留索引.
    if (covered >= position) {
        if (!repair) {
            auto skipped = replica.source.prepare(scope, key, replica.source.find(scope, key), position, std::chrono::steady_clock::now());
            if (!skipped) {
                return std::unexpected(error(skipped.error()));
            }
            retired.source = skipped->commit();
            std::erase_if(replica.coverage, [position](const Replica::Coverage& value) { return value.position <= position; });
        }
        return {};
    }
    if (repair && !precise && std::ranges::count_if(replica.coverage, [](const Replica::Coverage& value) { return !value.name->key.empty(); }) >= 128) {
        return std::unexpected(Error::capacity);
    }
    auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    advance(replica, stamp->time, expired); // 已持来源锁, 先处理旧期限, 再准备新事实.
    advance(stamp->time, expired, &replica);
    const auto old = replica.source.find(scope, key);
    if (form == Source::Form::renew && (!old || !old->value || !record->value || old->version != record->version)) {
        return std::unexpected(Error::ended); // 仅期限不能创建本来源缺失或不同版本的正文.
    }
    if (old && record && (record->version < old->version || (record->version == old->version && old->value && record->value && (*old->value != *record->value || *record->deadline < *old->deadline)))) {
        return std::unexpected(Error::conflict);
    }
    if (record) {
        record = Catalog::expire(*record, stamp->time);
    }
    const auto known = merged_.find(scope, key); // 来源事实与跨来源水位分开准备, 未知不清水位.
    std::optional<Catalog::Change> combined;
    if (record) {
        auto candidate = Catalog::merge(known ? &*known : nullptr, *record, stamp->time);
        if (!candidate) {
            return std::unexpected(error(candidate.error()));
        }
        combined = std::move(*candidate);
    }
    const auto bytes = replica.source.bytes();
    auto origin = replica.source.prepare(scope, key, record, repair ? std::nullopt : std::optional(position), std::chrono::steady_clock::now());
    if (!origin) {
        return std::unexpected(error(origin.error()));
    }
    if (origin->bytes() > limits_.replica_bytes || replica_bytes_ - bytes > limits_.replica_bytes - origin->bytes()) {
        return std::unexpected(Error::capacity);
    }
    std::optional<Source::Edit> merged;
    if (combined) {
        auto candidate = merged_.prepare(scope, key, combined->record, std::nullopt, std::chrono::steady_clock::now());
        if (!candidate) {
            return std::unexpected(error(candidate.error()));
        }
        merged.emplace(std::move(*candidate));
    }
    Pending pending{*this, scope}; // 新投影目录/合并钩子在提交失败时撤销.
    auto* scene = locate(scope);
    if (combined && combined->visible && !scene) {
        auto located = obtain(scope);
        if (!located) {
            return std::unexpected(located.error());
        }
        scene = *located;
        pending.created = true;
    }
    std::optional<Projection::Edit> projection;
    const auto before = scene ? scene->history() : 0;
    if (combined && combined->visible) {
        auto candidate = scene->prepare(merged->name(), combined->record.value ? std::optional(Content{combined->record.version, combined->record.value}) : std::nullopt, std::chrono::steady_clock::now(), false, allowance(*scene));
        if (!candidate) {
            return std::unexpected(error(candidate.error()));
        }
        projection.emplace(std::move(*candidate));
    }
    const auto node = replica.timers.find(origin->name().get());
    Timer* hook = node == replica.timers.end() ? nullptr : node->second.get(); // 不跨 reserve 保存迭代器.
    std::unique_ptr<Timer> prepared;
    if (record && record->value && !hook) {
        if (!replica.agenda) {
            replica.agenda = std::make_unique<Agenda>(stamp->time);
            due_ = std::min(due_, replica.agenda->next()); // 首次创建的轮立即纳入统一下次边界.
        }
        prepared = std::make_unique<Timer>(origin->name());
        replica.timers.reserve(replica.timers.size() + 1);
    }
    if (combined && combined->record.value && !deadlines_.contains(merged->name().get())) {
        if (!outlook_) {
            outlook_ = std::make_unique<Agenda>(stamp->time);
            due_ = std::min(due_, outlook_->next()); // 首次创建的轮立即纳入统一下次边界.
        }
        pending.deadline = merged->name().get();
        deadlines_.emplace(pending.deadline, std::make_unique<Timer>(merged->name()));
    }
    std::optional<Replica::Coverage> prepared_coverage;
    if (repair && !precise) {
        replica.coverage.reserve(replica.coverage.size() + 1);
        prepared_coverage.emplace(origin->name(), position);
    }
    stamp = reading(); // 末次受理边界不能公开准备过程中已经到期的正文.
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    if ((record && record->value && *record->deadline <= stamp->time) || (combined && combined->record.value && *combined->record.deadline <= stamp->time)) {
        return std::unexpected(Error::ended);
    }

    if (prepared) {
        hook = prepared.get();
        replica.timers.emplace(origin->name().get(), std::move(prepared)); // 最后一个可能分配的动作.
    }
    retired.source = origin->commit();
    if (merged) {
        retired.merged = merged->commit();
    }
    if (projection) {
        retired.scene = projection->commit();
    }
    replica_bytes_ = replica_bytes_ - bytes + replica.source.bytes();
    if (record && record->value) {
        replica.agenda->set(*hook, *record->deadline);
    } else if (hook) {
        Agenda::erase(*hook);
        const auto removed = replica.timers.find(retired.source.name.get());
        retired.timer = std::move(removed->second);
        replica.timers.erase(removed);
    }
    if (combined) {
        const auto deadline = deadlines_.find(retired.merged.name.get());
        if (combined->record.value) {
            outlook_->set(*deadline->second, *combined->record.deadline);
        } else if (deadline != deadlines_.end()) {
            Agenda::erase(*deadline->second);
            retired.deadline = std::move(deadline->second);
            deadlines_.erase(deadline);
        }
    }
    if (prepared_coverage) {
        replica.coverage.push_back(std::move(*prepared_coverage));
    } else if (repair) {
        replica.coverage[index].position = position;
    } else {
        std::erase_if(replica.coverage, [position](const Replica::Coverage& value) { return value.position <= position; });
    }
    pending.committed = true;
    if (projection) {
        publish(*scene, before, retired.scene.event);
    }
    return {};
}

std::expected<Catalog::State::Recovery, Catalog::State::Error> Catalog::State::restore(std::string_view id, Source::Draft&& draft) {

    std::unique_lock lock(*gate_);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get())
        return std::unexpected(Error::input);
    auto& replica = *borrowed.get();
    if (replica.retired || std::ranges::any_of(replica.coverage, [&](const Replica::Coverage& value) { return value.position > draft.position(); }))
        return std::unexpected(Error::version);
    const auto valid = replica.source.validate(draft);
    if (!valid)
        return std::unexpected(error(valid.error()));
    auto scopes = replica.source.scopes(); // 旧范围缺失也要安装空基线, 防止只处理新范围而遗留旧事实.
    auto incoming = draft.scopes();
    scopes.insert(scopes.end(), std::make_move_iterator(incoming.begin()), std::make_move_iterator(incoming.end()));
    std::ranges::sort(scopes);
    scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
    return Recovery(*this, std::string(id), std::move(draft), std::move(scopes));
}

std::expected<void, Catalog::State::Error> Catalog::State::replace(std::string_view id, Source::Draft&& draft) {

    auto task = restore(id, std::move(draft));
    if (!task)
        return std::unexpected(task.error());
    for (;;) {
        const auto result = task->step(); // 每一步释放锁并回收旧页后, 再开始下一个 Scope.
        if (!result)
            return std::unexpected(result.error());
        if (*result)
            return {};
    }
}

std::expected<void, Catalog::State::Error> Catalog::State::finish(std::string_view id, std::uint64_t position) {

    std::unique_lock lock(*gate_);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get())
        return std::unexpected(Error::input);
    auto& replica = *borrowed.get();
    if (replica.retired || std::ranges::any_of(replica.coverage, [position](const Replica::Coverage& value) { return value.position > position; }) || !replica.source.confirm(position))
        return std::unexpected(Error::version);
    replica.coverage.clear(); // 全部范围完成后, 连续前缀接管范围覆盖证据.
    return {};
}

std::expected<void, Catalog::State::Error> Catalog::State::install(std::string_view id, const Source::Draft& draft, const Scope& scope) {

    // 所有旧根/旧轮/旧节点保持到释放提交锁后, 候选失败则保持已经安装的三个根不变.
    std::optional<Source::Tree> old_source;
    std::vector<std::unique_ptr<Timer>> old_timers; // 本范围被替换的钩子在锁外析构.
    std::optional<Source::Tree> old_merged;
    std::optional<Projection::Batch::Retired> old_scene; // 本范围旧投影及通知事件, 解锁后释放.
    std::vector<std::unique_ptr<Timer>> old_deadlines;

    std::unordered_map<const Source::Name*, std::unique_ptr<Timer>> timers;
    std::vector<Retired> expired;
    std::unique_lock lock(*gate_);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get()) {
        return std::unexpected(Error::input);
    }
    auto& replica = *borrowed.get();
    if (replica.retired || draft.position() < replica.source.position() || std::ranges::any_of(replica.coverage, [&](const Replica::Coverage& value) { return value.position > draft.position(); })) {
        return std::unexpected(Error::version);
    }
    auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    advance(replica, stamp->time, expired); // 已持来源锁, 先处理旧期限, 再准备新事实.
    advance(stamp->time, expired, &replica);
    const auto bytes = replica.source.bytes();

    // 来源独占准备与公共投影提交分离. rows 只拥有名称/不可变正文, 不复制业务字节.
    struct Native {
        Source::Tree::Key name; // 原生候选中的稳定名称, 不是临时请求的 string_view.
        Record record;          // 完整原生事实, 截止保持发送者原值, 不续一个新的 TTL.
    };

    std::vector<Source::Tree::Key> previous; // 本范围旧项, 在来源锁内枚举, 不占域锁.
    std::vector<Native> rows;                // 与候选一起拥有, 最终合并不重新查询/验证输入正文.
    std::optional<Source::Batch> origin;     // 先于锁外准备建立寿命, 恢复域锁后才回滚或发布.
    std::optional<Error> failure;            // 回调失败只终止私有准备, 不发布此前已经准备的行.
    Guard::outside(lock, [&] {
        replica.source.each(scope, [&](const Source::Tree::Key& name, const Record&) { previous.push_back(name); });
        origin.emplace(replica.source.prepare());
        for (const auto& name : previous) {
            if (!draft.find(scope, name->key)) {
                const auto erased = origin->erase(scope, name->key);
                if (!erased) {
                    failure = error(erased.error());
                    return;
                }
            }
        }
        draft.each(scope, [&](const Source::Tree::Key& name, Record incoming) {
            if (failure)
                return;
            if (!Catalog::valid(incoming)) {
                failure = Error::input;
                return;
            }
            const auto old = origin->find(scope, name->key);
            if (old && (incoming.version < old->version || (incoming.version == old->version && old->value && incoming.value && ((old->value != incoming.value && *old->value != *incoming.value) || *incoming.deadline < *old->deadline)))) {
                failure = Error::conflict;
                return;
            }
            if (old && old->version == incoming.version && old->value && incoming.value)
                incoming.value = old->value; // 完整字节相等已在锁外证明, 后续合并复用既有正文.
            auto native = origin->set(scope, name->key, incoming);
            if (!native) {
                failure = error(native.error());
                return;
            }
            if (incoming.value)
                timers.emplace(native->get(), std::make_unique<Timer>(*native));
            rows.push_back({*native, std::move(incoming)});
        });
    });
    if (failure)
        return std::unexpected(*failure);
    if (replica.retired)
        return std::unexpected(Error::version); // 准备期间的身份撤销优先于最终发布.
    stamp = reading();                          // 本地写入可能已经推进水位/投影/时间, 必须以当前状态重新合并.
    if (!stamp)
        return std::unexpected(stamp.error());
    advance(stamp->time, expired, &replica); // 本来源由 borrowed 排他保护, 其他来源/合并投影仍正常到期.
    if (!timers.empty() && !replica.agenda) {
        replica.agenda = std::make_unique<Agenda>(stamp->time);
        due_ = std::min(due_, replica.agenda->next());
    }
    const auto scope_coverage = std::ranges::find_if(replica.coverage, [&](const Replica::Coverage& value) { return *value.name->scope == scope && value.name->key.empty(); });
    if (scope_coverage == replica.coverage.end() && std::ranges::count_if(replica.coverage, [](const Replica::Coverage& value) { return value.name->key.empty(); }) >= static_cast<std::ptrdiff_t>(limits_.source.scopes * 2))
        return std::unexpected(Error::capacity);
    auto marker = std::make_shared<const Source::Name>(std::make_shared<const Scope>(scope), std::string{}); // 空 Key 仅为内部整个范围覆盖标记.
    replica.coverage.reserve(replica.coverage.size() + 1);                                                   // 提交后登记覆盖不能分配失败.

    // 合并根只 upsert 已知水位, 完整来源缺项和同版本水位都不撤销本机仍有效的正文.
    struct Change {
        Source::Tree::Key name; // 使用合并索引稳定名称, 不绑定可被整体替换的来源页槽.
        Record record;          // 合并后的最高版本/最长已接纳截止.
        bool visible{};         // 只有内容变化才创建 Scene 提交.
    };

    auto merged = merged_.prepare();
    std::vector<Change> changes; // 当前范围唯一修改列表, 不再分配范围到计划的第二层目录.
    Hooks hooks{*this, {}, false};
    std::optional<Clock::Time> earliest; // 所有候选活内容的最低截止, 末次取时必须仍有效.
    for (const auto& row : rows) {
        const auto& name = row.name; // 私有准备已经校验的原生名称与正文.
        const auto& incoming = row.record;
        const auto known = merged.find(*name->scope, name->key);
        auto combined = Catalog::merge(known ? &*known : nullptr, incoming, stamp->time);
        if (!combined) {
            return std::unexpected(error(combined.error()));
        }
        auto prepared = merged.set(*name->scope, name->key, combined->record);
        if (!prepared) {
            return std::unexpected(error(prepared.error()));
        }
        if (combined->record.value) {
            if (!outlook_) {
                outlook_ = std::make_unique<Agenda>(stamp->time);
                due_ = std::min(due_, outlook_->next()); // 首次创建的轮立即纳入统一下次边界.
            }
            hooks.add(*prepared);
            earliest = earliest ? std::min(*earliest, *combined->record.deadline) : combined->record.deadline;
        }
        changes.push_back({*prepared, combined->record, combined->visible});
    }

    Pending pending{*this, scope}; // 先有目录回滚责任, 后有批候选, 析构顺序不可倒置.
    std::optional<Projection::Batch> projection;
    auto history = history_; // 当前 Scope 使用全域剩余额度, 不改变其他范围历史.
    if (std::ranges::any_of(changes, [](const Change& row) { return row.visible; })) {
        const auto count = scopes_;
        auto located = obtain(scope);
        if (!located)
            return std::unexpected(located.error());
        pending.created = scopes_ != count;
        auto& scene = **located;
        const auto before = scene.history();
        projection.emplace(scene.prepare(allowance(scene)));
        for (const auto& row : changes) {
            if (!row.visible)
                continue;
            auto prepared = projection->set(row.name, row.record.value ? std::optional(Content{row.record.version, row.record.value}) : std::nullopt, std::chrono::steady_clock::now());
            if (!prepared)
                return std::unexpected(error(prepared.error()));
        }
        history = history - before + projection->history();
    }
    old_deadlines.reserve(changes.size()); // 删除节点容量在提交前准备, 不在通知/提交路径分配.
    if (origin->bytes() > limits_.replica_bytes || replica_bytes_ - bytes > limits_.replica_bytes - origin->bytes())
        return std::unexpected(Error::capacity);
    old_timers.reserve(previous.size());
    replica.timers.reserve(replica.timers.size() + timers.size()); // merge 转移节点, 提交阶段不创建新节点或扩桶.
    stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    if (earliest && *earliest <= stamp->time) {
        return std::unexpected(Error::ended);
    }
    old_source.emplace(origin->commit()); // 仅发布此范围, 来源位置保持原完整前缀.

    old_merged.emplace(merged.commit());
    if (projection)
        old_scene.emplace(projection->commit());
    for (const auto& row : changes) {
        const auto timer = deadlines_.find(row.name.get());
        if (row.record.value) {
            outlook_->set(*timer->second, *row.record.deadline);
        } else if (timer != deadlines_.end()) {
            Agenda::erase(*timer->second);
            old_deadlines.push_back(std::move(timer->second));
            deadlines_.erase(timer);
        }
    }
    for (const auto& name : previous) {
        const auto node = replica.timers.find(name.get());
        if (node != replica.timers.end()) {
            Agenda::erase(*node->second);
            old_timers.push_back(std::move(node->second));
            replica.timers.erase(node);
        }
    }
    for (auto& [name, timer] : timers) {
        const auto record = replica.source.find(scope, name->key); // 此时原生批已提交, 定时器指向其稳定名称.
        replica.agenda->set(*timer, *record->deadline);
    }
    replica.timers.merge(timers);
    std::erase_if(replica.coverage, [&](const Replica::Coverage& value) { return *value.name->scope == scope; });
    replica.coverage.push_back({std::move(marker), draft.position()});
    replica_bytes_ = replica_bytes_ - bytes + replica.source.bytes();
    history_ = history;
    hooks.committed = true;
    pending.committed = true;
    if (notify_ && old_scene) {
        for (const auto& event : old_scene->events)
            notify_(context_, scope, event);
    }
    return {};
}

void Catalog::State::advance(Replica& replica, Clock::Time now, std::vector<Retired>& retired) {

    if (!replica.agenda) {
        return;
    }
    replica.agenda->advance(now, [&](Agenda::Node* hook, Clock::Time boundary) {
        auto& timer = *static_cast<Timer*>(hook); // 摘链后仍由来源 timers 保持对象寿命.
        const auto old = replica.source.find(*timer.name->scope, timer.name->key);
        if (!old || !old->value) {
            throw std::logic_error("Catalog replica timer has no active record");
        }
        if (*old->deadline > boundary) {
            replica.agenda->set(timer, *old->deadline);
            return;
        }
        retired.emplace_back();
        auto& released = retired.back();
        const auto bytes = replica.source.bytes();
        auto origin = replica.source.prepare(*timer.name->scope, timer.name->key, Catalog::expire(*old, boundary), std::nullopt, std::chrono::steady_clock::now());
        if (!origin) {
            throw std::runtime_error("Catalog replica expiry could not prepare watermark");
        }
        released.source = origin->commit(); // 合并可见内容由自己的最长截止推进, 不广播远端到期.
        replica_bytes_ = replica_bytes_ - bytes + replica.source.bytes();
        const auto node = replica.timers.find(timer.name.get());
        Agenda::erase(timer);
        released.timer = std::move(node->second);
        replica.timers.erase(node);
    });
}
} // namespace astra
