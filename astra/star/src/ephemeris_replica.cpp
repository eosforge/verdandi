#include "ephemeris_state.hpp"
#include <astra/profile.hpp>

namespace astra {
Ephemeris::State::Guard Ephemeris::State::acquire(std::string_view id, std::unique_lock<std::shared_mutex>& domain) const {

    const auto found = replicas_.find(id); // 只在域锁内使用迭代器, 守卫自行保持来源寿命.
    Guard result(found == replicas_.end() ? nullptr : found->second, domain);
    const auto current = replicas_.find(id); // 等待期间可能退役并回收, 不能把旧对象当成新准入来源.
    if (current == replicas_.end() || current->second.get() != result.get())
        return Guard(nullptr, domain);
    return result;
}

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
    auto replica = std::make_shared<Replica>(gate_, limits_.source);
    replicas_.emplace(std::string(id), std::move(replica));
    return {};
}

void Ephemeris::State::retire(std::string_view id) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_replica.Ephemeris.State.retire");
    ASTRA_PROFILE_BEGIN(profile_lock_53, "star.ephemeris_replica.Ephemeris.State.retire.wait.lock");
    const std::lock_guard lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_53);
    const auto found = replicas_.find(id);
    if (found != replicas_.end()) {
        found->second->retired = true;
        due_ = Clock::Time{};
        found->second->coverage.clear(); // 不再安装旧任务, 已确定身份退出时无需等待旧前缀追赶.
    }
}

std::expected<std::uint64_t, Ephemeris::State::Error> Ephemeris::State::received(std::string_view id) const {
    std::unique_lock lock(*gate_);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    return !borrowed.get() ? std::expected<std::uint64_t, Error>(std::unexpected(Error::input)) : borrowed.get()->source.position();
}

std::expected<Ephemeris::State::Source::Draft, Ephemeris::State::Error> Ephemeris::State::prepare(std::string_view id, std::uint64_t position) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_replica.Ephemeris.State.prepare");

    ASTRA_PROFILE_BEGIN(profile_lock_70, "star.ephemeris_replica.Ephemeris.State.prepare.wait.lock");
    std::unique_lock lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_70);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get()) {
        return std::unexpected(Error::input);
    }
    if (borrowed.get()->retired) {
        return std::unexpected(Error::obsolete);
    }
    if (position < borrowed.get()->source.position() || std::ranges::any_of(borrowed.get()->coverage, [position](const Replica::Coverage& value) { return value.position > position; })) {
        return std::unexpected(Error::obsolete);
    }
    return borrowed.get()->source.prepare(position);
}

std::expected<std::optional<Ephemeris::Record>, Ephemeris::State::Error> Ephemeris::State::replica(std::string_view id, const Scope& scope, std::string_view uuid) const {

    ASTRA_PROFILE_SCOPE("star.ephemeris_replica.Ephemeris.State.replica");

    if (!scope.valid() || !Ephemeris::valid(uuid)) {
        return std::unexpected(Error::input);
    }
    ASTRA_PROFILE_BEGIN(profile_lock_89, "star.ephemeris_replica.Ephemeris.State.replica.wait.lock");
    std::unique_lock lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_89);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get()) {
        return std::unexpected(Error::input);
    }
    return borrowed.get()->source.find(scope, uuid); // 最终 apply 会先推进本地 TTL, 此读数不构成存活保证.
}

std::expected<Ephemeris::State::Point, Ephemeris::State::Error> Ephemeris::State::resolve(const Scope& scope, std::string_view key) {

    if (!scope.valid() || !Ephemeris::valid(key)) {
        return std::unexpected(Error::input);
    }
    const std::shared_lock lock(*export_); // 只读取自有来源事实及连续位置, 已过期正文由接收端验证绝对截止.
    return Point{source_.position(), source_.find(scope, key)};
}

std::expected<bool, Ephemeris::State::Error> Ephemeris::State::covered(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key) {

    if (!scope.valid() || !Ephemeris::valid(key) || position == 0) {
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
        return std::unexpected(Error::obsolete);
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

std::expected<void, Ephemeris::State::Error> Ephemeris::State::apply(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view uuid, std::optional<Record> record, Source::Form form) {
    return receive(id, position, scope, uuid, std::move(record), form, false);
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::repair(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view uuid, std::optional<Record> record) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_replica.Ephemeris.State.repair");
    const auto form = record ? Source::Form::record : Source::Form::erase;
    return receive(id, position, scope, uuid, std::move(record), form, true);
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::receive(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view uuid, std::optional<Record> record, Source::Form form, bool repair) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_replica.Ephemeris.State.receive");

    if (!scope.valid() || !Ephemeris::valid(uuid) || position == 0 || (record && (!Ephemeris::valid(*record) || record->deadline.time_since_epoch().count() == 0)) || (form == Source::Form::erase) != !record) {
        return std::unexpected(Error::input);
    }
    std::vector<Retired> expired;
    Retired retired;
    ASTRA_PROFILE_BEGIN(profile_lock_156, "star.ephemeris_replica.Ephemeris.State.receive.wait.lock");
    std::unique_lock lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_156);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get()) {
        return std::unexpected(Error::input);
    }
    auto& replica = *borrowed.get();
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
    auto covered = coverage == replica.coverage.end() ? 0 : coverage->position; // 精确补项可晚于范围基线, 取两者最大覆盖位置.
    for (const auto& value : replica.coverage) {
        if (*value.name->scope == scope && value.name->key.empty())
            covered = std::max(covered, value.position);
    }
    const bool precise = coverage != replica.coverage.end();
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
    if (repair && !precise && std::ranges::count_if(replica.coverage, [](const Replica::Coverage& value) { return !value.name->key.empty(); }) >= 128) {
        return std::unexpected(Error::capacity);
    }
    auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    advance(replica, stamp->time, expired); // 已持来源锁, 先处理旧期限, 再准备新事实.
    advance(stamp->time, expired, &replica);
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
            due_ = std::min(due_, replica.agenda->next()); // 首次创建的轮立即纳入统一下次边界.
        }
        prepared = std::make_unique<Timer>(origin->name());
        replica.timers.reserve(replica.timers.size() + 1);
    }
    std::optional<Replica::Coverage> prepared_coverage; // 覆盖证据与正文一同准备, 不能装入未来记录后再分配标记.
    if (repair && !precise) {
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

std::expected<Ephemeris::State::Recovery, Ephemeris::State::Error> Ephemeris::State::restore(std::string_view id, Source::Draft&& draft) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_replica.Ephemeris.State.restore");

    ASTRA_PROFILE_BEGIN(profile_lock_316, "star.ephemeris_replica.Ephemeris.State.restore.wait.lock");
    std::unique_lock lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_316);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get())
        return std::unexpected(Error::input);
    auto& replica = *borrowed.get();
    if (replica.retired || std::ranges::any_of(replica.coverage, [&](const Replica::Coverage& value) { return value.position > draft.position(); }))
        return std::unexpected(Error::obsolete);
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

std::expected<void, Ephemeris::State::Error> Ephemeris::State::replace(std::string_view id, Source::Draft&& draft) {

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

std::expected<void, Ephemeris::State::Error> Ephemeris::State::finish(std::string_view id, std::uint64_t position) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_replica.Ephemeris.State.finish");

    ASTRA_PROFILE_BEGIN(profile_lock_350, "star.ephemeris_replica.Ephemeris.State.finish.wait.lock");
    std::unique_lock lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_350);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get())
        return std::unexpected(Error::input);
    auto& replica = *borrowed.get();
    if (replica.retired || std::ranges::any_of(replica.coverage, [position](const Replica::Coverage& value) { return value.position > position; }) || !replica.source.confirm(position))
        return std::unexpected(Error::obsolete);
    replica.coverage.clear(); // 全部范围完成后, 连续前缀接管范围覆盖证据.
    return {};
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::install(std::string_view id, const Source::Draft& draft, const Scope& scope) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_replica.Ephemeris.State.install");

    // 所有大块旧资源在 gate 外回收, 包括来源根、投影根和被替换的整组调度器.
    std::optional<Source::Tree> old_source;
    std::vector<std::unique_ptr<Timer>> old_timers;      // 本范围被替换的钩子在锁外析构.
    std::optional<Projection::Batch::Retired> old_scene; // 本范围旧投影及通知事件, 解锁后释放.

    std::unordered_map<const Source::Name*, std::unique_ptr<Timer>> timers;
    std::vector<Retired> expired;
    ASTRA_PROFILE_BEGIN(profile_lock_370, "star.ephemeris_replica.Ephemeris.State.install.wait.lock");
    std::unique_lock lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_370);
    auto borrowed = acquire(id, lock); // 来源忙时不占住本地提交锁.
    if (!borrowed.get()) {
        return std::unexpected(Error::input);
    }
    auto& replica = *borrowed.get();
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
            if (!Ephemeris::valid(name->key) || !Ephemeris::valid(incoming) || incoming.deadline.time_since_epoch().count() == 0) {
                failure = Error::input;
                return;
            }
            const auto old = origin->find(scope, name->key);
            if (old && ((old->attr != incoming.attr && *old->attr != *incoming.attr) || old->ttl != incoming.ttl || incoming.update < old->update || incoming.renewal < old->renewal || incoming.deadline < old->deadline || (incoming.renewal == old->renewal && incoming.deadline != old->deadline) || (incoming.update == old->update && old->data != incoming.data && *old->data != *incoming.data))) {
                failure = Error::conflict;
                return;
            }
            if (old) {
                incoming.attr = old->attr; // 固定属性已经完整比对, 不在最终域锁里再扫描一遍.
                if (old->data == incoming.data || *old->data == *incoming.data)
                    incoming.data = old->data;
            }
            auto native = origin->set(scope, name->key, incoming);
            if (!native) {
                failure = error(native.error());
                return;
            }
            timers.emplace(native->get(), std::make_unique<Timer>(*native));
            rows.push_back({*native, std::move(incoming)});
        });
    });
    if (failure)
        return std::unexpected(*failure);
    if (replica.retired)
        return std::unexpected(Error::obsolete); // 准备期间的身份撤销优先于最终发布.
    stamp = reading();                           // 本地写入可能已经推进水位/投影/时间, 必须以当前状态重新合并.
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

    // 计划只列出真实内容变化, 相同 Attr/Data 和纯续期不更改公共游标.
    struct Change {
        Source::Tree::Key name;        // 共享来源或原可见名称, 也保持临时删除目标寿命.
        std::optional<Content> record; // 缺失表示本来源的公开删除.
        bool data{};                   // 已有 Attr 的纯 Data 更新提示.
    };

    std::vector<Change> changes; // 当前范围唯一修改列表, 不再分配范围到计划的第二层目录.
    std::vector<const Source::Name*> removed;
    Ownership ownership{*this, {}, false};
    for (const auto& name : previous) {
        const auto incoming = draft.find(*name->scope, name->key);
        if (incoming && incoming->deadline > stamp->time) {
            continue;
        }
        auto* scene = locate(*name->scope);
        const auto current = scene ? scene->find(name->key) : Projection::Point{};
        const auto owner = current.record ? owners_.find(current.name.get()) : owners_.end();
        if (owner != owners_.end() && owner->second == &replica) {
            changes.push_back({current.name, {}, false});
            removed.push_back(current.name.get());
        }
    }
    std::optional<Clock::Time> earliest; // 最后取时检查新公开记录仍有效, 过期候选不重新获得 TTL.
    for (const auto& row : rows) {
        const auto& name = row.name; // 私有准备已经校验的原生名称与正文.
        const auto& incoming = row.record;
        if (incoming.deadline <= stamp->time) {
            continue;
        }
        earliest = earliest ? std::min(*earliest, incoming.deadline) : incoming.deadline;
        auto* scene = locate(*name->scope);
        const auto current = scene ? scene->find(name->key) : Projection::Point{};
        if (current.record) {
            const auto owner = owners_.find(current.name.get());
            if (owner == owners_.end() || owner->second != &replica || (current.record->attr != incoming.attr && *current.record->attr != *incoming.attr)) {
                return std::unexpected(Error::conflict);
            }
            if (current.record->data == incoming.data || *current.record->data == *incoming.data) {
                continue;
            }
        } else {
            ownership.add(name, replica);
        }
        changes.push_back({current.record ? current.name : name, Content{incoming.attr, incoming.data}, current.record.has_value()});
    }

    // 每个范围只创建一个 COW 批候选. 失败先析构候选, 再撤销新增空范围; 不复制整个公共哈希表.
    Pending pending{*this, scope}; // 先有目录回滚责任, 后有批候选, 析构顺序不可倒置.
    std::optional<Projection::Batch> projection;
    auto history = history_; // 当前 Scope 使用全域剩余额度, 不改变其他范围历史.
    if (!changes.empty()) {
        const auto count = scopes_;
        auto located = obtain(scope);
        if (!located)
            return std::unexpected(located.error());
        pending.created = scopes_ != count;
        auto& scene = **located;
        const auto before = scene.history();
        projection.emplace(scene.prepare(allowance(scene)));
        for (const auto& row : changes) {
            auto prepared = projection->set(row.name, row.record, std::chrono::steady_clock::now(), row.data);
            if (!prepared)
                return std::unexpected(error(prepared.error()));
        }
        history = history - before + projection->history();
    }
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

    if (projection)
        old_scene.emplace(projection->commit());
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
        replica.agenda->set(*timer, record->deadline);
    }
    replica.timers.merge(timers);
    std::erase_if(replica.coverage, [&](const Replica::Coverage& value) { return *value.name->scope == scope; });
    replica.coverage.push_back({std::move(marker), draft.position()});
    replica_bytes_ = replica_bytes_ - bytes + replica.source.bytes();
    history_ = history;
    for (const auto* name : removed) {
        owners_.erase(name);
    }
    pending.committed = true;
    ownership.committed = true;
    if (notify_ && old_scene) {
        for (const auto& event : old_scene->events)
            notify_(context_, scope, event);
    }
    return {};
}

void Ephemeris::State::advance(Replica& replica, Clock::Time now, std::vector<Retired>& retired) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_replica.Ephemeris.State.advance");

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
