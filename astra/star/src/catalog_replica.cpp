#include "catalog_state.hpp"
#include <list>

namespace astra {
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
    replicas_.emplace(std::string(id), std::make_unique<Replica>(gate_, limits_.source));
    return {};
}

void Catalog::State::retire(std::string_view id) {
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found != replicas_.end()) {
        found->second->retired = true;
        found->second->coverage.clear(); // 不再处理此旧身份后缀, 但不更改任何记录的既有期限.
    }
}

std::expected<std::uint64_t, Catalog::State::Error> Catalog::State::received(std::string_view id) const {
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    return found == replicas_.end() ? std::expected<std::uint64_t, Error>(std::unexpected(Error::input)) : found->second->source.position();
}

std::expected<Catalog::State::Source::Draft, Catalog::State::Error> Catalog::State::prepare(std::string_view id, std::uint64_t position) {

    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found == replicas_.end()) {
        return std::unexpected(Error::input);
    }
    const auto& replica = *found->second;
    if (replica.retired || position < replica.source.position() || std::ranges::any_of(replica.coverage, [position](const Replica::Coverage& value) { return value.position > position; })) {
        return std::unexpected(Error::version);
    }
    return replica.source.prepare(position);
}

std::expected<std::optional<Catalog::Record>, Catalog::State::Error> Catalog::State::replica(std::string_view id, const Scope& scope, std::string_view key) const {

    if (!scope.valid() || !Scope::text(key, 1024)) {
        return std::unexpected(Error::input);
    }
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found == replicas_.end()) {
        return std::unexpected(Error::input);
    }
    return found->second->source.find(scope, key); // 仅返回来源事实, 不从 merged_ 借正文或较晚截止.
}

std::expected<Catalog::State::Point, Catalog::State::Error> Catalog::State::resolve(const Scope& scope, std::string_view key) {

    if (!scope.valid() || !Scope::text(key, 1024)) {
        return std::unexpected(Error::input);
    }
    std::vector<Retired> retired; // 到期清理产生的旧引用在 gate 外释放.
    const std::lock_guard lock(*gate_);
    const auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    advance(stamp->time, retired);
    return Point{source_.position(), source_.find(scope, key)};
}

std::expected<bool, Catalog::State::Error> Catalog::State::covered(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key) {

    if (!scope.valid() || !Scope::text(key, 1024) || position == 0) {
        return std::unexpected(Error::input);
    }
    Source::Retired retired;
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found == replicas_.end()) {
        return std::unexpected(Error::input);
    }
    auto& replica = *found->second;
    if (replica.retired) {
        return std::unexpected(Error::version);
    }
    if (position <= replica.source.position()) {
        return true;
    }
    if (replica.source.position() == UINT64_MAX || position != replica.source.position() + 1) {
        return std::unexpected(Error::history);
    }
    if (std::ranges::none_of(replica.coverage, [&](const Replica::Coverage& value) { return *value.name->scope == scope && value.name->key == key && value.position >= position; })) {
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
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found == replicas_.end()) {
        return std::unexpected(Error::input);
    }
    auto& replica = *found->second;
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
    const auto covered = coverage == replica.coverage.end() ? 0 : coverage->position;
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
    if (repair && covered == 0 && replica.coverage.size() == 128) {
        return std::unexpected(Error::capacity);
    }
    auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    advance(stamp->time, expired);
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
        }
        prepared = std::make_unique<Timer>(origin->name());
        replica.timers.reserve(replica.timers.size() + 1);
    }
    if (combined && combined->record.value && !deadlines_.contains(merged->name().get())) {
        if (!outlook_) {
            outlook_ = std::make_unique<Agenda>(stamp->time);
        }
        pending.deadline = merged->name().get();
        deadlines_.emplace(pending.deadline, std::make_unique<Timer>(merged->name()));
    }
    std::optional<Replica::Coverage> prepared_coverage;
    if (repair && covered == 0) {
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

std::expected<void, Catalog::State::Error> Catalog::State::replace(std::string_view id, Source::Draft&& draft) {

    // 所有旧根/旧轮/旧节点保持到释放提交锁后, 候选失败则保持已经安装的三个根不变.
    std::optional<Source::Replaced> old_source;
    std::optional<Source::Tree> old_merged;
    std::vector<Projection::Batch::Retired> old_scenes;
    std::vector<std::unique_ptr<Timer>> old_deadlines;
    std::unique_ptr<Agenda> agenda;
    std::unordered_map<const Source::Name*, std::unique_ptr<Timer>> timers;
    std::vector<Retired> expired;
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found == replicas_.end()) {
        return std::unexpected(Error::input);
    }
    auto& replica = *found->second;
    if (replica.retired || draft.position() < replica.source.position() || std::ranges::any_of(replica.coverage, [&](const Replica::Coverage& value) { return value.position > draft.position(); })) {
        return std::unexpected(Error::version);
    }
    auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    advance(stamp->time, expired);
    const auto bytes = replica.source.bytes();
    if (draft.bytes() > limits_.replica_bytes || replica_bytes_ - bytes > limits_.replica_bytes - draft.bytes()) {
        return std::unexpected(Error::capacity);
    }

    // 合并根只 upsert 已知水位, 完整来源缺项和同版本水位都不撤销本机仍有效的正文.
    struct Change {
        Source::Tree::Key name; // 使用合并索引稳定名称, 不绑定可被整体替换的来源页槽.
        Record record;          // 合并后的最高版本/最长已接纳截止.
        bool visible{};         // 只有内容变化才创建 Scene 提交.
    };

    auto merged = merged_.prepare();
    std::map<Scope, std::vector<Change>> changes;
    Hooks hooks{*this, {}, false};
    std::optional<Error> failure;
    std::optional<Clock::Time> earliest; // 所有候选活内容的最低截止, 末次取时必须仍有效.
    draft.each([&](const Source::Tree::Key& name, const Record& incoming) {
        if (failure) {
            return;
        }
        if (!Catalog::valid(incoming)) {
            failure = Error::input;
            return;
        }
        const auto old = replica.source.find(*name->scope, name->key);
        if (old && (incoming.version < old->version || (incoming.version == old->version && old->value && incoming.value && (*old->value != *incoming.value || *incoming.deadline < *old->deadline)))) {
            failure = Error::conflict;
            return;
        }
        if (incoming.value) {
            if (!agenda) {
                agenda = std::make_unique<Agenda>(stamp->time);
            }
            auto timer = std::make_unique<Timer>(name); // 已过期来源正文也安排下一拍清理成水位.
            agenda->set(*timer, *incoming.deadline);
            timers.emplace(name.get(), std::move(timer));
        }
        const auto known = merged.find(*name->scope, name->key);
        auto combined = Catalog::merge(known ? &*known : nullptr, incoming, stamp->time);
        if (!combined) {
            failure = error(combined.error());
            return;
        }
        auto prepared = merged.set(*name->scope, name->key, combined->record);
        if (!prepared) {
            failure = error(prepared.error());
            return;
        }
        if (combined->record.value) {
            if (!outlook_) {
                outlook_ = std::make_unique<Agenda>(stamp->time);
            }
            hooks.add(*prepared);
            earliest = earliest ? std::min(*earliest, *combined->record.deadline) : combined->record.deadline;
        }
        changes[*name->scope].push_back({*prepared, combined->record, combined->visible});
    });
    if (failure) {
        return std::unexpected(*failure);
    }

    struct Plan {
        Projection::Batch batch; // 每个真实内容发生变化的范围一个候选, 不复制未变范围.
    };

    std::list<Pending> pending; // 稳定的回滚责任必须比 plans 活得更久.
    std::vector<Plan> plans;
    plans.reserve(changes.size());
    auto history = history_;
    std::size_t removal{};
    for (const auto& [scope, rows] : changes) {
        removal += static_cast<std::size_t>(std::ranges::count_if(rows, [](const Change& row) { return !row.record.value; }));
        if (std::ranges::none_of(rows, [](const Change& row) { return row.visible; })) {
            continue;
        }
        pending.emplace_back(*this, scope);
        const auto count = scopes_;
        auto located = obtain(scope);
        if (!located) {
            return std::unexpected(located.error());
        }
        pending.back().created = scopes_ != count;
        auto& scene = **located;
        const auto before = scene.history();
        auto batch = scene.prepare(limits_.history - (history - before));
        for (const auto& row : rows) {
            if (!row.visible) {
                continue;
            }
            auto prepared = batch.set(row.name, row.record.value ? std::optional(Content{row.record.version, row.record.value}) : std::nullopt, std::chrono::steady_clock::now());
            if (!prepared) {
                return std::unexpected(error(prepared.error()));
            }
        }
        history = history - before + batch.history();
        plans.push_back({std::move(batch)});
    }
    old_scenes.reserve(plans.size());
    old_deadlines.reserve(removal);
    stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    if (earliest && *earliest <= stamp->time) {
        return std::unexpected(Error::ended);
    }
    auto installed = replica.source.reset(std::move(draft)); // 仍可拒绝来源候选, 在此之前没有公开半份安装.
    if (!installed) {
        return std::unexpected(error(installed.error()));
    }

    old_source.emplace(std::move(*installed));
    old_merged.emplace(merged.commit());
    for (auto& plan : plans) {
        old_scenes.push_back(plan.batch.commit());
    }
    for (const auto& [scope, rows] : changes) {
        (void)scope; // 此阶段按稳定名称调度, 不再查找范围.
        for (const auto& row : rows) {
            const auto timer = deadlines_.find(row.name.get());
            if (row.record.value) {
                outlook_->set(*timer->second, *row.record.deadline);
            } else if (timer != deadlines_.end()) {
                Agenda::erase(*timer->second);
                old_deadlines.push_back(std::move(timer->second));
                deadlines_.erase(timer);
            }
        }
    }
    replica.timers.swap(timers);
    replica.agenda.swap(agenda);
    replica.coverage.clear();
    replica_bytes_ = replica_bytes_ - bytes + replica.source.bytes();
    history_ = history;
    hooks.committed = true;
    for (auto& item : pending) {
        item.committed = true;
    }
    if (notify_) {
        for (const auto& batch : old_scenes) {
            for (const auto& event : batch.events) {
                notify_(context_, *event.name->scope, event);
            }
        }
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
