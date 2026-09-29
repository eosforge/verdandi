#include "catalog_state.hpp"
#include <algorithm>
#include <array>
#include <astra/profile.hpp>
#include <cassert>
#include <utility>

namespace astra {
Catalog::State::State(Clock& clock) : State([&clock] { return clock.now(); }, Limits{}) {}

Catalog::State::State(Time time, Limits limits) : time_(std::move(time)), limits_(limits), source_(export_, static_cast<Source::Measure>(&State::measure), true, limits.source), merged_(gate_, static_cast<Source::Measure>(&State::measure), false, limits.source) {
    if (!time_) {
        throw std::invalid_argument("Catalog requires a clock");
    }
}

Catalog::State::~State() = default;

Catalog::State::Pending::~Pending() {

    if (committed) {
        return;
    }
    if (timer) {
        owner.timers_.erase(timer);
    }
    if (deadline) {
        owner.deadlines_.erase(deadline);
    }
    if (created) {
        const auto sector = owner.scenes_.find(scope.sector); // 只回收本次新增的游标零范围.
        sector->second.erase(scope.spectrum);
        if (sector->second.empty()) {
            owner.scenes_.erase(sector);
        }
        --owner.scopes_;
    }
}

std::size_t Catalog::State::measure(const Record& record) noexcept {
    return record.value ? record.value->size() : 0;
}

std::size_t Catalog::State::measure(const Content& record) noexcept {
    return record.value ? record.value->size() : 0;
}

Catalog::State::Error Catalog::State::error(Catalog::Error value) noexcept {
    switch (value) {
    case Catalog::Error::input:
        return Error::input;
    case Catalog::Error::clock:
        return Error::clock;
    case Catalog::Error::ended:
        return Error::ended;
    case Catalog::Error::version:
        return Error::version;
    case Catalog::Error::conflict:
        return Error::conflict;
    case Catalog::Error::exhausted:
        return Error::exhausted;
    }
    return Error::input;
}

Catalog::State::Error Catalog::State::error(Source::Error value) noexcept {
    switch (value) {
    case Source::Error::input:
        return Error::input;
    case Source::Error::duplicate:
        return Error::conflict;
    case Source::Error::capacity:
        return Error::capacity;
    case Source::Error::version:
        return Error::exhausted;
    case Source::Error::history:
        return Error::history;
    }
    return Error::input;
}

Catalog::State::Error Catalog::State::error(Projection::Error value) noexcept {
    switch (value) {
    case Projection::Error::input:
        return Error::input;
    case Projection::Error::capacity:
        return Error::capacity;
    case Projection::Error::version:
        return Error::exhausted;
    case Projection::Error::history:
        return Error::history;
    }
    return Error::input;
}

void Catalog::State::notify(Notify notify, void* context) {
    const std::lock_guard lock(*gate_); // 只更新内部收集器, 不在此触发回放.
    notify_ = notify;
    context_ = context;
}

std::expected<std::vector<std::uint64_t>, Catalog::State::Error> Catalog::State::versions(const Scope& scope, std::span<const std::string_view> keys) const {

    // 在锁外验证有界请求和准备输出, 失败不创建任何业务状态.
    if (!scope.valid() || keys.empty() || keys.size() > 128)
        return std::unexpected(Error::input);
    std::array<std::string_view, 128> names;           // 栈内借用本次参数, 不为逐 Key 校验分配树节点.
    auto unique = std::span(names).first(keys.size()); // 不改变调用方顺序, 回复仍与输入逐项对应.
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (!Scope::text(keys[index], 1024))
            return std::unexpected(Error::input);
        unique[index] = keys[index];
    }
    std::ranges::sort(unique);
    if (std::ranges::adjacent_find(unique) != unique.end())
        return std::unexpected(Error::input);
    std::vector<std::uint64_t> result(keys.size()); // 未知 Key 保留零, 不伪造墓碑或业务版本.

    // merged_ 保留过期后的最高水位; 不从可见投影中推测已丢失正文的版本.
    const std::shared_lock lock(*gate_);
    for (std::size_t index = 0; index < keys.size(); ++index)
        if (const auto record = merged_.find(scope, keys[index]))
            result[index] = record->version;
    return result;
}

std::expected<Clock::Reading, Catalog::State::Error> Catalog::State::reading() {
    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.reading");
    return Context<State>::reading(*this);
}

std::expected<Catalog::State::Projection*, Catalog::State::Error> Catalog::State::obtain(const Scope& scope) {
    return Context<State>::obtain(*this, scope);
}

std::size_t Catalog::State::allowance(const Projection& scene) const {
    return Context<State>::allowance(*this, scene);
}

void Catalog::State::publish(Projection& scene, std::size_t before, const Projection::Event& event) noexcept {
    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.publish");
    Context<State>::publish(*this, scene, before, event);
}

std::expected<void, Catalog::State::Error> Catalog::State::publish(const Scope& scope, std::string_view key, Value value, std::uint64_t version, std::uint32_t ttl) {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.publish");
    return change(scope, key, std::move(value), version, ttl, false);
}

std::expected<void, Catalog::State::Error> Catalog::State::publish(const Scope& scope, std::span<const Entry> entries, std::uint64_t version, std::uint32_t ttl, bool renewal) {

    // 批次共同使用一个版本/TTL, 不允许同 Key 重复或空批次, 全部输入先校验.
    std::array<std::string_view, 128> names; // 上限与协议一致, 在持状态锁之前一次性校验重复.
    std::size_t bytes{};                     // 单个正文先限长, 再累加, 不允许 size_t 溢出绕过整批预算.
    if (!scope.valid() || entries.empty() || entries.size() > 128 || !version || ttl < 1000 || ttl > 600000)
        return std::unexpected(Error::input);
    auto keys = std::span(names).first(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index]; // 输入及所有借用文本持续到同步提交返回.
        if (!Scope::text(entry.key, 1024) || (renewal ? static_cast<bool>(entry.value) : !entry.value))
            return std::unexpected(Error::input);
        if (entry.value && entry.value->size() > 1024 * 1024)
            return std::unexpected(Error::capacity);
        bytes += entry.key.size() + (entry.value ? entry.value->size() : 0);
        if (bytes > 1024 * 1024)
            return std::unexpected(Error::capacity);
        keys[index] = entry.key;
    }
    std::ranges::sort(keys);
    if (std::ranges::adjacent_find(keys) != keys.end())
        return std::unexpected(Error::input);

    // 旧来源/投影根和到期回收列表先于锁创建, 快照在完整提交边界捕获.
    std::vector<Retired> expired;
    std::optional<Source::Tree> old_source, old_merged;
    std::unique_ptr<std::deque<Source::Event>> old_history; // 可能持有已离开当前根的最后一份旧正文, 必须锁外回收.
    std::optional<Projection::Batch::Retired> old_scene;
    const std::lock_guard lock(*gate_);
    auto stamp = reading();
    if (!stamp)
        return std::unexpected(stamp.error());
    advance(stamp->time, expired);
    const std::unique_lock origin_lock(*export_);

    struct Row {
        std::optional<Record> old, known; // 原来源和最高水位, 最终复核仍使用这份旧事实.
        Record candidate, combined;       // 私有准备, 最终采样统一修订期限.
        Source::Tree::Key source, merged; // 稳定地址, 在调度中共享.
    };

    std::vector<Row> rows; // 与输入顺序一一对应, 不保存第二份正文.
    rows.reserve(entries.size());
    for (const auto& entry : entries) {
        auto old = source_.find(scope, entry.key), known = merged_.find(scope, entry.key);
        auto candidate = renewal ? Catalog::renew(old ? &*old : nullptr, known ? &*known : nullptr, version, ttl, *stamp) : Catalog::publish(old ? &*old : nullptr, known ? &*known : nullptr, entry.value, version, ttl, *stamp);
        if (!candidate)
            return std::unexpected(error(candidate.error()));
        auto combined = Catalog::merge(known ? &*known : nullptr, *candidate, stamp->time);
        if (!combined)
            return std::unexpected(error(combined.error()));
        rows.push_back({std::move(old), std::move(known), *candidate, combined->record, {}, {}});
    }

    // 每个新钩子独立登记回滚, 首个 Pending 还负责新 Scope. 责任对象早于各批候选析构.
    std::vector<std::unique_ptr<Pending>> pending;
    pending.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index)
        pending.push_back(std::make_unique<Pending>(*this, scope));
    const auto count = scopes_;
    const auto located = obtain(scope);
    if (!located)
        return std::unexpected(located.error());
    pending.front()->created = scopes_ != count;
    auto& scene = **located;
    const auto before = scene.history();
    auto projection = scene.prepare(allowance(scene));
    auto origin = source_.batch(); // 每项连续编号, 一次交换完整来源和发送历史.
    auto merged = merged_.prepare();
    if (!agenda_) {
        agenda_ = std::make_unique<Agenda>(stamp->time);
        due_ = std::min(due_, agenda_->next());
    }
    if (!outlook_) {
        outlook_ = std::make_unique<Agenda>(stamp->time);
        due_ = std::min(due_, outlook_->next());
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
        auto& row = rows[index]; // 本项候选与固定输入, 所有分配均在发布之前.
        const auto& entry = entries[index];
        auto source = origin.set(scope, entry.key, row.candidate, renewal ? Source::Form::renew : Source::Form::record);
        if (!source)
            return std::unexpected(error(source.error()));
        row.source = *source;
        auto target = merged.set(scope, entry.key, row.combined);
        if (!target)
            return std::unexpected(error(target.error()));
        row.merged = *target;
        if (Catalog::visible(row.known ? &*row.known : nullptr, row.combined)) {
            if (auto prepared = projection.set(row.merged, Content{row.combined.version, row.combined.value}, std::chrono::steady_clock::now()); !prepared)
                return std::unexpected(error(prepared.error()));
        }
        if (!timers_.contains(row.source.get())) {
            timers_.emplace(row.source.get(), std::make_unique<Timer>(row.source));
            pending[index]->timer = row.source.get();
        }
        if (!deadlines_.contains(row.merged.get())) {
            deadlines_.emplace(row.merged.get(), std::make_unique<Timer>(row.merged));
            pending[index]->deadline = row.merged.get();
        }
    }
    const auto events = projection.seal(); // 先准备完整通知, 提交后不能再分配或发布部分键.
    stamp = reading();
    if (!stamp)
        return std::unexpected(stamp.error());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        auto& row = rows[index]; // 最终时钟统一用于所有键, 任一续租已经结束则整批拒绝.
        auto candidate = renewal ? Catalog::renew(row.old ? &*row.old : nullptr, row.known ? &*row.known : nullptr, version, ttl, *stamp) : Catalog::publish(row.old ? &*row.old : nullptr, row.known ? &*row.known : nullptr, row.candidate.value, version, ttl, *stamp);
        if (!candidate)
            return std::unexpected(error(candidate.error()));
        auto combined = Catalog::merge(row.known ? &*row.known : nullptr, *candidate, stamp->time);
        if (!combined || !origin.revise(scope, entries[index].key, *candidate) || !merged.revise(scope, entries[index].key, combined->record))
            throw std::logic_error("Catalog batch changed prepared payload");
        row.candidate = *candidate;
        row.combined = combined->record;
    }

    old_source.emplace(origin.commit(&old_history));
    old_merged.emplace(merged.commit());
    old_scene.emplace(projection.commit());
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index]; // 调度节点已全部分配, 以下仅更新标量/链表.
        agenda_->set(*timers_.find(row.source.get())->second, *row.candidate.deadline);
        outlook_->set(*deadlines_.find(row.merged.get())->second, *row.combined.deadline);
        pending[index]->committed = true;
    }
    history_ = history_ - before + scene.history();
    if (notify_ && !events->empty()) {
        Projection::Event event; // 信封只借用准备好的完整事件, 下游持一个锁收集.
        event.version = events->back().version;
        event.batch = events;
        notify_(context_, scope, event);
    }
    return {};
}

std::expected<void, Catalog::State::Error> Catalog::State::renew(const Scope& scope, std::string_view key, std::uint64_t version, std::uint32_t ttl) {
    return change(scope, key, {}, version, ttl, true);
}

std::expected<void, Catalog::State::Error> Catalog::State::change(const Scope& scope, std::string_view key, Value value, std::uint64_t version, std::uint32_t ttl, bool renewal) {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.change");

    if (!scope.valid() || !Scope::text(key, 1024)) {
        return std::unexpected(Error::input);
    }
    std::vector<Retired> expired; // 全部旧引用在提交锁外释放, 不持锁销毁最终大正文.
    Retired retired;
    ASTRA_PROFILE_BEGIN(profile_lock_163, "star.catalog_state.Catalog.State.change.wait.lock");
    const std::lock_guard lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_163);
    auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    advance(stamp->time, expired);
    ASTRA_PROFILE_BEGIN(profile_lock_169, "star.catalog_state.Catalog.State.change.wait.origin_lock");
    const std::unique_lock origin_lock(*export_); // 本机根/日志发布与导出同域, 远端范围安装无需持有此锁.
    ASTRA_PROFILE_END(profile_lock_169);
    const auto old = source_.find(scope, key), known = merged_.find(scope, key);
    const auto* current = old ? &*old : nullptr;
    const auto* highest = known ? &*known : nullptr;
    auto candidate = renewal ? Catalog::renew(current, highest, version, ttl, *stamp) : Catalog::publish(current, highest, value, version, ttl, *stamp);
    if (!candidate) {
        return std::unexpected(error(candidate.error()));
    }
    auto combined = Catalog::merge(highest, *candidate, stamp->time);
    if (!combined) {
        return std::unexpected(error(combined.error()));
    }
    Pending pending{*this, scope}; // 三份候选均提交前, 新 Scope/两类钩子自动回滚.
    const auto scopes = scopes_;
    const auto located = obtain(scope);
    if (!located) {
        return std::unexpected(located.error());
    }
    pending.created = scopes_ != scopes;
    auto& scene = **located;
    const auto before = scene.history();
    const auto retention = allowance(scene);
    const auto position = source_.next();
    if (!position) {
        return std::unexpected(error(position.error()));
    }
    auto origin = source_.prepare(scope, key, *candidate, *position, std::chrono::steady_clock::now(), renewal ? Source::Form::renew : Source::Form::record);
    if (!origin) {
        return std::unexpected(error(origin.error()));
    }
    auto merged = merged_.prepare(scope, key, combined->record, std::nullopt, std::chrono::steady_clock::now());
    if (!merged) {
        return std::unexpected(error(merged.error()));
    }
    std::optional<Projection::Edit> projection;
    if (combined->visible) {
        auto prepared = scene.prepare(merged->name(), Content{combined->record.version, combined->record.value}, std::chrono::steady_clock::now(), false, retention);
        if (!prepared) {
            return std::unexpected(error(prepared.error()));
        }
        projection.emplace(std::move(*prepared));
    }
    if (!agenda_) {
        agenda_ = std::make_unique<Agenda>(stamp->time);
        due_ = std::min(due_, agenda_->next()); // 首次创建的轮立即纳入统一下次边界.
    }
    if (!outlook_) {
        outlook_ = std::make_unique<Agenda>(stamp->time);
        due_ = std::min(due_, outlook_->next()); // 首次创建的轮立即纳入统一下次边界.
    }
    auto found = timers_.find(origin->name().get());
    if (found == timers_.end()) {
        found = timers_.emplace(origin->name().get(), std::make_unique<Timer>(origin->name())).first;
        pending.timer = origin->name().get();
    }
    auto visible = deadlines_.find(merged->name().get());
    if (visible == deadlines_.end()) {
        visible = deadlines_.emplace(merged->name().get(), std::make_unique<Timer>(merged->name())).first;
        pending.deadline = merged->name().get();
    }

    stamp = reading(); // 最终时间同时决定本机候选及合并期限, 不借远端较晚截止延长自己的来源事实.
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    candidate = renewal ? Catalog::renew(current, highest, version, ttl, *stamp) : Catalog::publish(current, highest, candidate->value, version, ttl, *stamp); // 首次校验已复用相同正文, 最终复核沿用该引用以命中指针快速路径.
    if (!candidate) {
        return std::unexpected(error(candidate.error()));
    }
    combined = Catalog::merge(highest, *candidate, stamp->time);
    if (!combined || !origin->revise(*candidate) || !merged->revise(combined->record)) {
        throw std::logic_error("Catalog final check changed prepared payload");
    }
    retired.source = origin->commit();
    retired.merged = merged->commit();
    if (projection) {
        retired.scene = projection->commit();
    }
    agenda_->set(*found->second, *candidate->deadline);
    outlook_->set(*visible->second, *combined->record.deadline);
    pending.committed = true;
    if (projection) {
        publish(scene, before, retired.scene.event);
    }
    return {};
}

std::expected<Catalog::State::Retired, Catalog::State::Error> Catalog::State::expire(const Scope& scope, std::string_view key, Clock::Time time, bool projected) {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.expire");

    auto& source = projected ? merged_ : source_; // 两份期限不可混用, 本机来源到期并不撤销远端同版本保活.
    auto& timers = projected ? deadlines_ : timers_;
    const auto old = source.find(scope, key);
    if (!old || !old->value || *old->deadline > time) {
        return std::unexpected(Error::ended);
    }
    Projection* scene{};
    std::size_t before{}, retention{};
    if (projected) {
        const auto located = obtain(scope);
        if (!located) {
            return std::unexpected(located.error());
        }
        scene = *located;
        before = scene->history();
        retention = allowance(*scene);
    }
    auto origin = source.prepare(scope, key, Catalog::expire(*old, time), std::nullopt, std::chrono::steady_clock::now());
    if (!origin) {
        return std::unexpected(error(origin.error()));
    }
    std::optional<Projection::Edit> projection;
    if (projected) {
        auto prepared = scene->prepare(origin->name(), {}, std::chrono::steady_clock::now(), false, retention);
        if (!prepared) {
            return std::unexpected(error(prepared.error()));
        }
        projection.emplace(std::move(*prepared));
    }
    const auto timer = timers.find(origin->name().get());
    if (timer == timers.end()) {
        throw std::logic_error("Active Catalog lacks its timer");
    }

    Retired retired;
    retired.source = origin->commit(); // 无论来源或合并水位维护都不增加来源位置/广播日志.
    if (projection) {
        retired.scene = projection->commit();
    }
    Agenda::erase(*timer->second);
    retired.timer = std::move(timer->second);
    timers.erase(timer);
    if (projection) {
        publish(*scene, before, retired.scene.event);
    }
    return retired;
}

std::shared_ptr<Catalog::State::Replica> Catalog::State::advance(Clock::Time now, std::vector<Retired>& retired, Replica* held) {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.advance");

    std::shared_ptr<Replica> blocked; // 返回仍有到期/退役责任的忙来源, 公开读取稍后在域锁外等待.
    if (now < due_)
        return {}; // 本轮无任何时间轮能前进, 不再逐来源遍历; 不是 max_ticks 或时间截断.
    const auto advance = [&](Source& source, Agenda* agenda, bool projected) {
        if (!agenda) {
            return;
        }
        agenda->advance(now, [&](Agenda::Node* hook, Clock::Time boundary) {
            auto& timer = *static_cast<Timer*>(hook);
            const auto record = source.find(*timer.name->scope, timer.name->key);
            if (!record || !record->value) {
                throw std::logic_error("Scheduled Catalog lacks an active native record");
            }
            if (*record->deadline > boundary) {
                agenda->set(timer, *record->deadline);
                return;
            }
            retired.emplace_back();
            auto removed = expire(*timer.name->scope, timer.name->key, boundary, projected);
            if (!removed) {
                throw std::runtime_error("Catalog expiry could not commit");
            }
            retired.back() = std::move(*removed);
        });
    };
    {
        ASTRA_PROFILE_BEGIN(profile_lock_334, "star.catalog_state.Catalog.State.advance.wait.origin_lock");
        const std::unique_lock origin_lock(*export_);
        ASTRA_PROFILE_END(profile_lock_334);
        advance(source_, agenda_.get(), false);
    } // 本机维护结束立即释放导出锁, 不覆盖下方远端维护.

    advance(merged_, outlook_.get(), true);
    for (auto current = replicas_.begin(); current != replicas_.end();) {
        const auto owner = current->second; // 保活到 preparing 解锁后, 回收目录不能先销毁仍被锁住的 mutex.
        auto& replica = *owner;             // 身份可在准备期间退役, 但最后一个守卫释放前对象仍存在.
        if (&replica == held) {
            ++current;
            continue; // 已持非递归来源锁, 不能再次 try_lock, 更不能访问正在编辑的原生目录.
        }
        ASTRA_PROFILE_BEGIN(profile_lock_346, "star.catalog_state.Catalog.State.advance.wait.preparing");
        const std::unique_lock preparing(replica.mutex, std::try_to_lock); // 持域锁时只尝试, 禁止等待形成反向锁序.
        ASTRA_PROFILE_END(profile_lock_346);
        if (!preparing.owns_lock()) {
            if (!blocked && (replica.retired || (replica.agenda && replica.agenda->next() <= now)))
                blocked = current->second;
            ++current;
            continue;
        }
        this->advance(replica, now, retired);
        if (replica.retired && replica.timers.empty()) {
            replica_bytes_ -= replica.source.bytes();
            current = replicas_.erase(current); // 已学到的水位保留在 merged_, 拒绝旧身份的依据由控制器保持.
        } else {
            ++current;
        }
    }
    // 忙碌来源仍以其旧边界参加最小值, 不能把未完成维护伪装成时间已追平; 异常保持旧 due_.
    due_ = Clock::Time::max();
    const auto include = [&](const Agenda* agenda) {
        if (agenda)
            due_ = std::min(due_, agenda->next());
    };
    include(agenda_.get());
    include(outlook_.get());
    for (const auto& [id, replica] : replicas_)
        include(replica->agenda.get());
    if (blocked)
        due_ = std::min(due_, now); // 空退役来源也有待回收责任, 不能因没有轮而丢失下次检查.
    return blocked;
}

void Catalog::State::tick() {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.tick");

    std::vector<Retired> retired; // 等待/回收不持域锁, 失败仍保留之前已经完成的到期删除.
    ASTRA_PROFILE_BEGIN(profile_lock_379, "star.catalog_state.Catalog.State.tick.wait.lock");
    std::unique_lock lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_379);
    if (!agenda_ && !outlook_ && replicas_.empty())
        return; // 尚无任何清理责任时允许 Clock 尚未初始化.
    for (;;) {
        const auto stamp = reading(); // 每次等待后重新采样, 不拿先前读数作为完成证据.
        if (!stamp)
            throw std::runtime_error("Catalog clock is unavailable");
        auto blocked = advance(stamp->time, retired);
        if (!blocked)
            return;
        Guard waiting(std::move(blocked), lock); // 等待期间其他来源/本地写入仍可取得域锁.
    }
}

auto Catalog::State::execute(auto&& action) {
    return Reading<State>::execute(*this, std::forward<decltype(action)>(action));
}

std::expected<Catalog::State::Projection::View, Catalog::State::Error> Catalog::State::capture(const Scope& scope) {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.capture");

    if (!scope.valid()) {
        return std::unexpected(Error::input);
    }

    // scene 仅在域锁内借用, 返回的 View 自持页面和同步域, 不把指针泄漏到锁外.
    return execute([&]() -> std::expected<Projection::View, Error> {
        const auto* scene = locate(scope); // 未创建的范围不占永久目录额度, 观察注册仍由 Feed 保持.
        return scene ? scene->capture() : Projection::empty(gate_);
    });
}

std::expected<Catalog::State::Projection::Point, Catalog::State::Error> Catalog::State::find(const Scope& scope, std::string_view key) {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.find");

    if (!scope.valid() || !Scope::text(key, 1024)) {
        return std::unexpected(Error::input);
    }

    // key 同步借用调用参数, Point 拥有名称/正文引用; 未创建范围返回版本零的空结果.
    return execute([&]() -> std::expected<Projection::Point, Error> {
        const auto* scene = locate(scope); // 缺失范围与缺失目标都返回明确缺项, 不隐式创建 Scope.
        return scene ? scene->find(key) : Projection::Point{};
    });
}

std::expected<std::vector<Catalog::State::Projection::Event>, Catalog::State::Error> Catalog::State::changes(const Scope& scope, std::uint64_t since) {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.changes");

    if (!scope.valid()) {
        return std::unexpected(Error::input);
    }

    // scene 借用限于本次读取, since 超前仍是输入错误; 临时 expected 的成功向量按移动交付.
    return execute([&]() -> std::expected<std::vector<Projection::Event>, Error> {
        if (const auto* scene = locate(scope)) {
            return scene->replay(since).transform_error([](Projection::Error failure) { return failure == Projection::Error::version ? Error::input : error(failure); });
        }
        if (since != 0) {
            return std::unexpected(Error::input); // 未创建范围只有合法位置零, 不能确认超前游标.
        }
        return std::vector<Projection::Event>{};
    });
}

Catalog::State::Source::View Catalog::State::source() {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.source");
    ASTRA_PROFILE_BEGIN(profile_lock_442, "star.catalog_state.Catalog.State.source.wait.lock");
    const std::shared_lock lock(*export_); // 来源事实自带 deadline, 接收端自行过期; 导出不触发任何域 GC.
    ASTRA_PROFILE_END(profile_lock_442);
    return source_.capture();
}

std::expected<std::vector<Catalog::State::Source::Event>, Catalog::State::Error> Catalog::State::events(std::uint64_t since) {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.events");
    ASTRA_PROFILE_BEGIN(profile_lock_447, "star.catalog_state.Catalog.State.events.wait.lock");
    const std::shared_lock lock(*export_);
    ASTRA_PROFILE_END(profile_lock_447);
    return source_.replay(since).transform_error([](Source::Error failure) { return failure == Source::Error::version ? Error::input : error(failure); });
}

std::expected<Catalog::State::Source::Delivery, Catalog::State::Error> Catalog::State::deliver(std::uint64_t since, std::size_t count, std::size_t bytes) {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.deliver");
    ASTRA_PROFILE_BEGIN(profile_lock_452, "star.catalog_state.Catalog.State.deliver.wait.lock");
    const std::shared_lock lock(*export_); // 快照/后缀捕获不等待远端安装、公开投影准备或其他来源 GC.
    ASTRA_PROFILE_END(profile_lock_452);
    return source_.deliver(since, count, bytes).transform_error([](Source::Error failure) { return failure == Source::Error::version ? Error::input : error(failure); });
}

} // namespace astra
