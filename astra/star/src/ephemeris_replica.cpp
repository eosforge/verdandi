#include "ephemeris_state.hpp"
#include <list>

namespace astra {
Ephemeris::State::Ownership::~Ownership() {
    if (!committed) {
        for (const auto* name : added) {
            owner.owners_.erase(name);
        }
    }
}

void Ephemeris::State::Ownership::add(const Source::Tree::Key& name, Replica& replica) {
    added.push_back(name.get()); // emplace 抛错仍可无异常擦除不存在的新项.
    owner.owners_.emplace(name.get(), &replica);
}

Ephemeris::State::Projection* Ephemeris::State::locate(const Scope& scope) {
    const auto sector = scenes_.find(scope.sector);
    if (sector == scenes_.end()) {
        return nullptr;
    }
    const auto spectrum = sector->second.find(scope.spectrum);
    return spectrum == sector->second.end() ? nullptr : &spectrum->second;
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::admit(std::string_view id) {

    if (!Scope::text(id, 1024)) {
        return std::unexpected(Error::input);
    }
    const std::lock_guard lock(*gate_);
    if (const auto existing = replicas_.find(id); existing != replicas_.end()) {
        return existing->second->retired ? std::unexpected(Error::obsolete) : std::expected<void, Error>{};
    }
    if (replicas_.size() == limits_.replicas) {
        return std::unexpected(Error::capacity);
    }
    auto replica = std::make_unique<Replica>(gate_, limits_.source);
    replicas_.emplace(std::string(id), std::move(replica));
    return {};
}

void Ephemeris::State::retire(std::string_view id) {
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found != replicas_.end()) {
        found->second->retired = true;
        found->second->coverage.clear(); // 不再安装旧任务, 已确定身份退出时无需等待旧前缀追赶.
    }
}

std::expected<std::uint64_t, Ephemeris::State::Error> Ephemeris::State::received(std::string_view id) const {
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    return found == replicas_.end() ? std::expected<std::uint64_t, Error>(std::unexpected(Error::input)) : found->second->source.position();
}

std::expected<Ephemeris::State::Source::Draft, Ephemeris::State::Error> Ephemeris::State::prepare(std::string_view id, std::uint64_t position) {

    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found == replicas_.end()) {
        return std::unexpected(Error::input);
    }
    if (found->second->retired) {
        return std::unexpected(Error::obsolete);
    }
    if (position < found->second->source.position() || std::ranges::any_of(found->second->coverage, [position](const Replica::Coverage& value) { return value.position > position; })) {
        return std::unexpected(Error::obsolete);
    }
    return found->second->source.prepare(position);
}

std::expected<std::optional<Ephemeris::Record>, Ephemeris::State::Error> Ephemeris::State::replica(std::string_view id, const Scope& scope, std::string_view uuid) const {

    if (!scope.valid() || !Ephemeris::valid(uuid)) {
        return std::unexpected(Error::input);
    }
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found == replicas_.end()) {
        return std::unexpected(Error::input);
    }
    return found->second->source.find(scope, uuid); // 最终 apply 会先推进本地 TTL, 此读数不构成存活保证.
}

std::expected<Ephemeris::State::Point, Ephemeris::State::Error> Ephemeris::State::resolve(const Scope& scope, std::string_view key) {

    if (!scope.valid() || !Ephemeris::valid(key)) {
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

std::expected<bool, Ephemeris::State::Error> Ephemeris::State::covered(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key) {

    if (!scope.valid() || !Ephemeris::valid(key) || position == 0) {
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
        return std::unexpected(Error::obsolete);
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

std::expected<void, Ephemeris::State::Error> Ephemeris::State::apply(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view uuid, std::optional<Record> record, Source::Form form) {
    return receive(id, position, scope, uuid, std::move(record), form, false);
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::repair(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view uuid, std::optional<Record> record) {
    const auto form = record ? Source::Form::record : Source::Form::erase;
    return receive(id, position, scope, uuid, std::move(record), form, true);
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::receive(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view uuid, std::optional<Record> record, Source::Form form, bool repair) {

    if (!scope.valid() || !Ephemeris::valid(uuid) || position == 0 || (record && (!Ephemeris::valid(*record) || record->deadline.time_since_epoch().count() == 0)) || (form == Source::Form::erase) != !record) {
        return std::unexpected(Error::input);
    }
    std::vector<Retired> expired;
    Retired retired;
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found == replicas_.end()) {
        return std::unexpected(Error::input);
    }
    auto& replica = *found->second;
    if (replica.retired) {
        return std::unexpected(Error::obsolete);
    }
    if (position <= replica.source.position()) {
        return {}; // 已安装前缀的重放不再更新期限/投影.
    }
    if (!repair && (replica.source.position() == UINT64_MAX || position != replica.source.position() + 1)) {
        return std::unexpected(Error::history);
    }
    const auto coverage = std::ranges::find_if(replica.coverage, [&](const Replica::Coverage& value) { return *value.name->scope == scope && value.name->key == uuid; });
    const auto covered = coverage == replica.coverage.end() ? 0 : coverage->position;
    const auto index = static_cast<std::size_t>(coverage - replica.coverage.begin()); // 后续 reserve 可移动 vector, 不跨它保留迭代器.
    if (covered >= position) {
        if (!repair) {
            // 回补 R 不能跳过其他目标; 对这个目标的旧事件仍逐项推进组前缀, 不改记录或租约.
            auto skipped = replica.source.prepare(scope, uuid, replica.source.find(scope, uuid), position, std::chrono::steady_clock::now());
            if (!skipped) {
                return std::unexpected(error(skipped.error()));
            }
            retired.source = skipped->commit();
            std::erase_if(replica.coverage, [&](const Replica::Coverage& value) { return value.position <= position; });
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
    const auto old = replica.source.find(scope, uuid);
    if (!old && (form == Source::Form::data || form == Source::Form::renew)) {
        return std::unexpected(Error::ended); // 部分字段不能凭空创建半条注册, 等精确完整回补.
    }
    if (old && record) {
        if (*old->attr != *record->attr || old->ttl != record->ttl || record->update < old->update || record->renewal < old->renewal || record->deadline < old->deadline || (record->renewal == old->renewal && record->deadline != old->deadline) || (record->update == old->update && *record->data != *old->data)) {
            return std::unexpected(Error::conflict);
        }
        if ((form == Source::Form::data && (record->update <= old->update || record->renewal != old->renewal || record->deadline != old->deadline)) || (form == Source::Form::renew && (record->renewal <= old->renewal || record->update != old->update || *record->data != *old->data || record->deadline < old->deadline))) {
            return std::unexpected(Error::obsolete);
        }
    }
    if (record && record->deadline <= stamp->time) {
        record.reset(); // 迟到完整事实仍确认连续位置, 但不重新发放 TTL 或公开已到期注册.
    }
    auto* scene = locate(scope);
    const auto visible = scene ? scene->find(uuid) : Projection::Point{};
    if (visible.record) {
        const auto owner = owners_.find(visible.name.get());
        if (owner == owners_.end() || owner->second != &replica) {
            return std::unexpected(Error::conflict); // 不覆盖同 UUID 的本机/其他远端注册.
        }
    }
    const bool changed = record ? !visible.record || *visible.record->data != *record->data : visible.record.has_value();
    Pending pending{*this, scope}; // 只用于新投影目录的回滚, 不持有本机时间轮节点.
    if (record && !scene) {
        const auto located = obtain(scope);
        if (!located) {
            return std::unexpected(located.error());
        }
        scene = *located;
        pending.created = true;
    }
    const auto before = scene ? scene->history() : 0;
    const auto retention = scene ? allowance(*scene) : 0;
    const auto bytes = replica.source.bytes();
    auto origin = replica.source.prepare(scope, uuid, record, repair ? std::nullopt : std::optional(position), std::chrono::steady_clock::now());
    if (!origin) {
        return std::unexpected(error(origin.error()));
    }
    if (origin->bytes() > limits_.replica_bytes || replica_bytes_ - bytes > limits_.replica_bytes - origin->bytes()) {
        return std::unexpected(Error::capacity);
    }
    std::optional<Projection::Edit> projection;
    Ownership ownership{*this, {}, false};
    if (changed) {
        auto prepared = scene->prepare(origin->name(), record ? std::optional(Content{record->attr, record->data}) : std::nullopt, std::chrono::steady_clock::now(), record && visible.record.has_value(), retention);
        if (!prepared) {
            return std::unexpected(error(prepared.error()));
        }
        projection.emplace(std::move(*prepared));
        if (record && !visible.record) {
            ownership.add(origin->name(), replica);
        }
    }
    const auto timer = replica.timers.find(origin->name().get());
    Timer* hook = timer == replica.timers.end() ? nullptr : timer->second.get(); // reserve 前取得稳定节点, 不保留会失效的迭代器.
    std::unique_ptr<Timer> prepared;                                             // 新节点在最后一次取时之前分配, 暂不放入活动表.
    if (record && !hook) {
        if (!replica.agenda) {
            replica.agenda = std::make_unique<Agenda>(stamp->time);
        }
        prepared = std::make_unique<Timer>(origin->name());
        replica.timers.reserve(replica.timers.size() + 1);
    }
    std::optional<Replica::Coverage> prepared_coverage; // 覆盖证据与正文一同准备, 不能装入未来记录后再分配标记.
    if (repair && covered == 0) {
        replica.coverage.reserve(replica.coverage.size() + 1);
        prepared_coverage.emplace(origin->name(), position);
    }
    stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    if (record && record->deadline <= stamp->time) {
        return std::unexpected(Error::ended); // 末次准备期间过期, 放弃本次候选再按当前状态恢复.
    }

    // 插入钩子是最后一个可能分配的操作, 成功后只允许无异常提交/调度/通知.
    if (prepared) {
        hook = prepared.get();
        replica.timers.emplace(origin->name().get(), std::move(prepared));
    }
    retired.source = origin->commit();
    if (projection) {
        retired.scene = projection->commit();
    }
    replica_bytes_ = replica_bytes_ - bytes + replica.source.bytes();
    if (record) {
        replica.agenda->set(*hook, record->deadline);
    } else if (hook) {
        Agenda::erase(*hook);
        const auto removed = replica.timers.find(retired.source.name.get());
        retired.timer = std::move(removed->second);
        replica.timers.erase(removed);
    }
    if (!record && visible.record) {
        owners_.erase(visible.name.get());
    }
    if (prepared_coverage) {
        replica.coverage.push_back(std::move(*prepared_coverage));
    } else if (repair) {
        replica.coverage[index].position = position;
    } else {
        std::erase_if(replica.coverage, [&](const Replica::Coverage& value) { return value.position <= position; });
    }
    ownership.committed = pending.committed = true;
    if (projection) {
        publish(*scene, before, retired.scene.event);
    }
    return {};
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::replace(std::string_view id, Source::Draft&& draft) {

    // 所有大块旧资源在 gate 外回收, 包括来源根、投影根和被替换的整组调度器.
    std::optional<Source::Replaced> old_source;
    std::vector<Projection::Batch::Retired> old_scenes;
    std::unique_ptr<Agenda> agenda;
    std::unordered_map<const Source::Name*, std::unique_ptr<Timer>> timers;
    std::vector<Retired> expired;
    const std::lock_guard lock(*gate_);
    const auto found = replicas_.find(id);
    if (found == replicas_.end()) {
        return std::unexpected(Error::input);
    }
    auto& replica = *found->second;
    if (replica.retired) {
        return std::unexpected(Error::obsolete);
    }
    if (draft.position() < replica.source.position() || std::ranges::any_of(replica.coverage, [&](const Replica::Coverage& value) { return value.position > draft.position(); })) {
        return std::unexpected(Error::obsolete);
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

    // 计划只列出真实内容变化, 相同 Attr/Data 和纯续期不更改公共游标.
    struct Change {
        Source::Tree::Key name;        // 共享来源或原可见名称, 也保持临时删除目标寿命.
        std::optional<Content> record; // 缺失表示本来源的公开删除.
        bool data{};                   // 已有 Attr 的纯 Data 更新提示.
    };

    std::map<Scope, std::vector<Change>> changes;
    std::vector<const Source::Name*> removed;
    Ownership ownership{*this, {}, false};
    std::optional<Error> failure; // 回调只准备计划, 失败不修改已经安装的来源根.
    replica.source.each([&](const Source::Tree::Key& name, const Record&) {
        const auto incoming = draft.find(*name->scope, name->key);
        if (incoming && incoming->deadline > stamp->time) {
            return;
        }
        auto* scene = locate(*name->scope);
        const auto current = scene ? scene->find(name->key) : Projection::Point{};
        const auto owner = current.record ? owners_.find(current.name.get()) : owners_.end();
        if (owner != owners_.end() && owner->second == &replica) {
            changes[*name->scope].push_back({current.name, {}, false});
            removed.push_back(current.name.get());
        }
    });
    std::optional<Clock::Time> earliest; // 最后取时检查新公开记录仍有效, 过期候选不重新获得 TTL.
    draft.each([&](const Source::Tree::Key& name, const Record& incoming) {
        if (failure) {
            return;
        }
        if (!Ephemeris::valid(name->key) || !Ephemeris::valid(incoming) || incoming.deadline.time_since_epoch().count() == 0) {
            failure = Error::input;
            return;
        }
        const auto old = replica.source.find(*name->scope, name->key);
        if (old && (*old->attr != *incoming.attr || old->ttl != incoming.ttl || incoming.update < old->update || incoming.renewal < old->renewal || incoming.deadline < old->deadline || (incoming.renewal == old->renewal && incoming.deadline != old->deadline) || (incoming.update == old->update && *old->data != *incoming.data))) {
            failure = Error::conflict;
            return;
        }
        if (!agenda) {
            agenda = std::make_unique<Agenda>(stamp->time);
        }
        auto timer = std::make_unique<Timer>(name); // 连已过期原生项也挂下一拍清理, 不泄漏不可见副本正文.
        agenda->set(*timer, incoming.deadline);
        timers.emplace(name.get(), std::move(timer));
        if (incoming.deadline <= stamp->time) {
            return;
        }
        earliest = earliest ? std::min(*earliest, incoming.deadline) : incoming.deadline;
        auto* scene = locate(*name->scope);
        const auto current = scene ? scene->find(name->key) : Projection::Point{};
        if (current.record) {
            const auto owner = owners_.find(current.name.get());
            if (owner == owners_.end() || owner->second != &replica || *current.record->attr != *incoming.attr) {
                failure = Error::conflict;
                return;
            }
            if (*current.record->data == *incoming.data) {
                return;
            }
        } else {
            ownership.add(name, replica);
        }
        changes[*name->scope].push_back({current.record ? current.name : name, Content{incoming.attr, incoming.data}, current.record.has_value()});
    });
    if (failure) {
        return std::unexpected(*failure);
    }

    // 每个范围只创建一个 COW 批候选. 失败先析构候选, 再撤销新增空范围; 不复制整个公共哈希表.
    struct Plan {
        Projection* scene;       // 新旧范围地址均在域锁及目录内保持稳定.
        Projection::Batch batch; // 一个 Scope 的完整投影修改候选.
    };

    std::list<Pending> pending; // 列表保证引用和析构次序, 不移动隐式拥有回滚责任的 Pending.
    std::vector<Plan> plans;
    plans.reserve(changes.size());
    auto history = history_; // 按顺序分配共享下游历史预算, 不能每个 Scope 都占满剩余额度.
    for (const auto& [scope, rows] : changes) {
        pending.emplace_back(*this, scope);
        const auto count = scopes_;
        auto located = obtain(scope);
        if (!located) {
            return std::unexpected(located.error());
        }
        pending.back().created = scopes_ != count;
        auto* scene = *located;
        const auto before = scene->history();
        auto batch = scene->prepare(limits_.history - (history - before));
        for (const auto& row : rows) {
            const auto prepared = batch.set(row.name, row.record, std::chrono::steady_clock::now(), row.data);
            if (!prepared) {
                return std::unexpected(error(prepared.error()));
            }
        }
        history = history - before + batch.history();
        plans.push_back({scene, std::move(batch)});
    }
    old_scenes.reserve(plans.size()); // 最终提交路径不再为回收或通知分配.
    stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    if (earliest && *earliest <= stamp->time) {
        return std::unexpected(Error::ended);
    }
    auto installed = replica.source.reset(std::move(draft)); // reset 先做位置/容量校验, 成功后是无分配根交换.
    if (!installed) {
        return std::unexpected(error(installed.error()));
    }

    old_source.emplace(std::move(*installed));
    for (auto& plan : plans) {
        old_scenes.push_back(plan.batch.commit());
    }
    replica.coverage.clear(); // 完整组基线覆盖全部 R 后才归还精确回补元数据.
    replica.timers.swap(timers);
    replica.agenda.swap(agenda);
    replica_bytes_ = replica_bytes_ - bytes + replica.source.bytes();
    history_ = history;
    for (const auto* name : removed) {
        owners_.erase(name);
    }
    for (auto& item : pending) {
        item.committed = true;
    }
    ownership.committed = true;
    if (notify_) {
        for (const auto& batch : old_scenes) {
            for (const auto& event : batch.events) {
                notify_(context_, *event.name->scope, event);
            }
        }
    }
    return {};
}

void Ephemeris::State::advance(Replica& replica, Clock::Time now, std::vector<Retired>& retired) {

    if (!replica.agenda) {
        return;
    }
    replica.agenda->advance(now, [&](Agenda::Node* hook, Clock::Time boundary) {
        auto& timer = *static_cast<Timer*>(hook);
        const auto old = replica.source.find(*timer.name->scope, timer.name->key);
        if (!old) {
            throw std::logic_error("Replica timer has no native record");
        }
        if (old->deadline > boundary) {
            replica.agenda->set(timer, old->deadline);
            return;
        }
        retired.emplace_back(); // 提交前准备锁外资源槽, 失败由 Agenda 重排当前节点.
        auto& released = retired.back();
        auto* scene = locate(*timer.name->scope);
        const auto visible = scene ? scene->find(timer.name->key) : Projection::Point{};
        const auto owner = visible.record ? owners_.find(visible.name.get()) : owners_.end();
        const bool visible_here = owner != owners_.end() && owner->second == &replica;
        const auto before = visible_here ? scene->history() : 0;
        const auto retention = visible_here ? allowance(*scene) : 0;
        const auto bytes = replica.source.bytes();
        auto origin = replica.source.prepare(*timer.name->scope, timer.name->key, {}, std::nullopt, std::chrono::steady_clock::now());
        if (!origin) {
            throw std::runtime_error("Replica expiry could not prepare native state");
        }
        std::optional<Projection::Edit> projection;
        if (visible_here) {
            auto prepared = scene->prepare(visible.name, {}, std::chrono::steady_clock::now(), false, retention);
            if (!prepared) {
                throw std::runtime_error("Replica expiry could not prepare projection");
            }
            projection.emplace(std::move(*prepared));
        }
        released.source = origin->commit(); // nullopt position 保持远端已安装连续位置, 不广播本地删除.
        if (projection) {
            released.scene = projection->commit();
            owners_.erase(visible.name.get());
        }
        replica_bytes_ = replica_bytes_ - bytes + replica.source.bytes();
        const auto node = replica.timers.find(timer.name.get());
        Agenda::erase(timer);
        released.timer = std::move(node->second);
        replica.timers.erase(node);
        if (projection) {
            publish(*scene, before, released.scene.event);
        }
    });
}
} // namespace astra
