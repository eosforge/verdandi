#include "catalog_state.hpp"
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

std::expected<Clock::Reading, Catalog::State::Error> Catalog::State::reading() {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.reading");

    ASTRA_PROFILE_BEGIN(profile_lock_100, "star.catalog_state.Catalog.State.reading.wait.timing");
    const std::lock_guard timing(timing_); // 共享读者仍按调用顺序验证注入时钟, 不并发修改 observed_.
    ASTRA_PROFILE_END(profile_lock_100);
    auto value = time_(); // 域锁内采样, 注入时钟也不能绕过非负/单调检查.
    if (!value || !value->ready || value->time.time_since_epoch().count() < 0 || (observed_ && value->time < *observed_)) {
        return std::unexpected(Error::clock);
    }
    observed_ = value->time;
    return *value;
}

std::expected<Catalog::State::Projection*, Catalog::State::Error> Catalog::State::obtain(const Scope& scope) {

    if (!scope.valid()) {
        return std::unexpected(Error::input);
    }
    const auto found = scenes_.find(scope.sector); // 普通路径只查两级目录, 不复制文本.
    if (found != scenes_.end()) {
        const auto spectrum = found->second.find(scope.spectrum);
        if (spectrum != found->second.end()) {
            return &spectrum->second;
        }
    }
    if (scopes_ == limits_.scopes) {
        return std::unexpected(Error::capacity);
    }
    auto [sector, created] = scenes_.try_emplace(scope.sector); // 新范围失败时也移除空 Sector.
    try {
        auto [spectrum, inserted] = sector->second.try_emplace(scope.spectrum, gate_, static_cast<Projection::Measure>(&State::measure), limits_.projection);
        scopes_ += inserted;
        return &spectrum->second;
    } catch (...) {
        if (created) {
            scenes_.erase(sector);
        }
        throw;
    }
}

std::size_t Catalog::State::allowance(const Projection& scene) const {
    return limits_.history - (history_ - scene.history()); // 本范围的旧历史允许在自己新历史的额度内替换.
}

void Catalog::State::publish(Projection& scene, std::size_t before, const Projection::Event& event) noexcept {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.publish");
    history_ = history_ - before + scene.history(); // Edit 已结束, 计费与来源状态同边界可见.
    if (notify_) {
        notify_(context_, *event.name->scope, event);
    } // 只能有界合并/标记溢出, 不执行网络或应用回调.
}

std::expected<void, Catalog::State::Error> Catalog::State::publish(const Scope& scope, std::string_view key, Value value, std::uint64_t version, std::uint32_t ttl) {

    ASTRA_PROFILE_SCOPE("star.catalog_state.Catalog.State.publish");
    return change(scope, key, std::move(value), version, ttl, false);
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
