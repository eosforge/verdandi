#include "ephemeris_state.hpp"
#include <astra/profile.hpp>
#include <cassert>
#include <tuple>
#include <utility>

namespace astra {
Ephemeris::State::State(Clock& clock) : State([&clock] { return clock.now(); }, Limits{}) {}

Ephemeris::State::State(Time time, Limits limits) : time_(std::move(time)), limits_(limits), source_(export_, static_cast<Source::Measure>(&State::measure), true, limits.source) {
    if (!time_) {
        throw std::invalid_argument("Ephemeris requires a clock");
    }
}

Ephemeris::State::~State() = default;

Ephemeris::State::Pending::~Pending() {

    if (committed) {
        return;
    }
    if (timer) {
        owner.timers_.erase(timer);
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

std::size_t Ephemeris::State::measure(const Record& record) noexcept {
    return record.attr->size() + record.data->size() + 2 * sizeof(Contribution) + 128;
}

std::size_t Ephemeris::State::measure(const Content& record) noexcept {
    return record.attr->size() + record.data->size();
}

Ephemeris::State::Error Ephemeris::State::error(Ephemeris::Error value) noexcept {
    switch (value) {
    case Ephemeris::Error::input:
        return Error::input;
    case Ephemeris::Error::clock:
        return Error::clock;
    case Ephemeris::Error::ended:
        return Error::ended;
    case Ephemeris::Error::obsolete:
        return Error::obsolete;
    case Ephemeris::Error::conflict:
        return Error::conflict;
    case Ephemeris::Error::exhausted:
        return Error::exhausted;
    }
    return Error::input;
}

Ephemeris::State::Error Ephemeris::State::error(Source::Error value) noexcept {
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

Ephemeris::State::Error Ephemeris::State::error(Projection::Error value) noexcept {
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

void Ephemeris::State::notify(Notify notify, void* context) {
    const std::lock_guard lock(*gate_); // 只更新内部收集器, 不在此触发回放.
    notify_ = notify;
    context_ = context;
}

std::expected<Clock::Reading, Ephemeris::State::Error> Ephemeris::State::reading() {
    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.reading");
    return Context<State>::reading(*this);
}

std::expected<Ephemeris::State::Projection*, Ephemeris::State::Error> Ephemeris::State::obtain(const Scope& scope) {
    return Context<State>::obtain(*this, scope);
}

std::size_t Ephemeris::State::allowance(const Projection& scene) const {
    return Context<State>::allowance(*this, scene);
}

void Ephemeris::State::publish(Projection& scene, std::size_t before, const Projection::Event& event) noexcept {
    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.publish");
    Context<State>::publish(*this, scene, before, event);
}

std::expected<Ephemeris::State::Receipt, Ephemeris::State::Error> Ephemeris::State::create(const Scope& scope, Value attr, Value data, std::uint32_t ttl, std::string uuid, std::uint64_t generation, std::uint64_t order, std::string_view capability) {

    if (!scope.valid() || !Ephemeris::valid(attr) || !Ephemeris::valid(data) || ttl < 1000 || ttl > 600000 || (generation == 0 && (!uuid.empty() || !capability.empty() || order != 0))) {
        return std::unexpected(Error::input);
    }
    if (generation) {
        if (!Ephemeris::valid(uuid) || capability.size() != 32 || uuid != Ephemeris::identity(scope, attr, ttl, capability)) {
            return std::unexpected(Error::input);
        }
    } else {
        uuid = Ephemeris::uuid();
    }
    Receipt receipt{uuid, {}, 0, generation, ttl}; // 分配回执文本先于提交.
    std::vector<Retired> expired;
    Retired retired;
    const std::lock_guard lock(*gate_);
    auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    advance(stamp->time, expired);
    const std::unique_lock origin_lock(*export_);
    const auto old = source_.find(scope, uuid);
    if (old && (!generation || old->generation > generation)) {
        return std::unexpected(Error::obsolete);
    }
    if (old && old->generation == generation) {
        if (*old->attr != *attr || old->ttl != ttl) {
            return std::unexpected(Error::conflict);
        }
        receipt.data = old->data;
        receipt.order = old->update;
        return receipt; // 相同注册代次只确认原结果, 不重新发放 TTL.
    }
    auto lookup = std::make_shared<const Source::Name>(std::make_shared<const Scope>(scope), uuid);
    if (const auto known = choices_.find(lookup); known != choices_.end()) {
        for (const auto& row : known->second) {
            if (row.record.deadline <= stamp->time) {
                continue;
            }
            if (!generation || !row.record.generation || row.record.generation > generation || row.record.ttl != ttl || *row.record.attr != *attr) {
                return std::unexpected(Error::conflict);
            }
            if (row.record.update > order) {
                data = row.record.data;
                order = row.record.update;
            } else if (row.record.update == order && *row.record.data != *data) {
                return std::unexpected(Error::conflict);
            }
        }
    }
    auto candidate = Ephemeris::create(attr, data, ttl, *stamp);
    if (!candidate) {
        return std::unexpected(error(candidate.error()));
    }
    candidate->update = order;
    candidate->generation = generation;
    const auto count = scopes_;
    const auto located = obtain(scope);
    if (!located) {
        return std::unexpected(located.error());
    }
    auto& scene = **located;
    Pending pending{*this, scope, nullptr, scopes_ != count, false};
    const auto before = scene.history();
    const auto position = source_.next();
    if (!position) {
        return std::unexpected(error(position.error()));
    }
    auto origin = source_.prepare(scope, uuid, *candidate, *position, std::chrono::steady_clock::now());
    if (!origin) {
        return std::unexpected(error(origin.error()));
    }
    auto choice = choose(origin->name(), *candidate, nullptr, stamp->time);
    if (!choice) {
        return std::unexpected(choice.error());
    }
    std::optional<Projection::Edit> projection;
    if (choice->changed) {
        auto prepared = scene.prepare(choice->name, choice->content(), std::chrono::steady_clock::now(), scene.find(uuid).record.has_value(), allowance(scene));
        if (!prepared) {
            return std::unexpected(error(prepared.error()));
        }
        projection.emplace(std::move(*prepared));
    }
    if (!agenda_) {
        agenda_ = std::make_unique<Agenda>(stamp->time);
        due_ = std::min(due_, agenda_->next());
    }
    auto found = timers_.find(origin->name().get());
    if (found == timers_.end()) {
        auto timer = std::make_unique<Timer>(origin->name());
        found = timers_.emplace(origin->name().get(), std::move(timer)).first;
        pending.timer = origin->name().get();
    }
    stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    auto final = Ephemeris::create(attr, data, ttl, *stamp);
    if (!final) {
        return std::unexpected(error(final.error()));
    }
    candidate->deadline = final->deadline;
    if (choice->record && choice->name != origin->name() && choice->record->deadline <= stamp->time) {
        return std::unexpected(Error::ended);
    }
    for (auto& row : choice->rows) {
        if (!row.owner) {
            row.record = *candidate;
        }
    }
    if (!origin->revise(*candidate)) {
        throw std::logic_error("Ephemeris creation cost changed");
    }
    receipt.data = candidate->data;
    receipt.order = candidate->update;
    retired.source = origin->commit();
    if (projection) {
        retired.scene = projection->commit();
    }
    choice->commit();
    agenda_->set(*found->second, candidate->deadline);
    pending.committed = true;
    if (projection) {
        publish(scene, before, retired.scene.event);
    }
    return receipt;
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::update(const Scope& scope, std::string_view uuid, Value data, std::uint64_t order, std::uint64_t generation, std::string_view capability) {
    return change(scope, uuid, std::move(data), order, false, generation, capability);
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::renew(const Scope& scope, std::string_view uuid, std::uint64_t order, std::uint64_t generation, std::string_view capability) {
    return change(scope, uuid, {}, order, true, generation, capability);
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::change(const Scope& scope, std::string_view uuid, Value data, std::uint64_t order, bool renewal, std::uint64_t generation, std::string_view capability) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.change");

    if (!scope.valid() || !Ephemeris::valid(uuid)) {
        return std::unexpected(Error::input);
    }
    std::vector<Retired> expired; // 追赶产生的旧载荷在锁外批量析构.
    Retired retired;
    ASTRA_PROFILE_BEGIN(profile_lock_236, "star.ephemeris_state.Ephemeris.State.change.wait.lock");
    const std::lock_guard lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_236);
    auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    advance(stamp->time, expired);
    ASTRA_PROFILE_BEGIN(profile_lock_242, "star.ephemeris_state.Ephemeris.State.change.wait.origin_lock");
    const std::unique_lock origin_lock(*export_); // 本机根/日志发布与导出同域, 远端范围安装无需持有此锁.
    ASTRA_PROFILE_END(profile_lock_242);
    const auto old = source_.find(scope, uuid); // 只查询自有来源, 副本不能成为可续租本机注册.
    if (!old) {
        return std::unexpected(Error::ended);
    }
    if (old->generation != generation || (generation && uuid != Ephemeris::identity(scope, old->attr, old->ttl, capability))) {
        return std::unexpected(Error::obsolete);
    }
    auto candidate = renewal ? Ephemeris::renew(*old, order, *stamp) : Ephemeris::update(*old, data, order, *stamp);
    if (!candidate) {
        return std::unexpected(error(candidate.error()));
    }
    if (!candidate->changed) {
        const auto final = reading(); // 即使幂等确认也不借用追赶前的旧时间绕过过期判断.
        if (!final) {
            return std::unexpected(final.error());
        }
        const auto checked = Ephemeris::active(*old, *final);
        if (!checked) {
            return std::unexpected(error(checked.error()));
        }
        return {};
    } // 同 order 有效确认不推进任何位置.
    const auto located = obtain(scope);
    if (!located) {
        return std::unexpected(located.error());
    }
    auto& scene = **located;
    const auto before = scene.history();
    const auto retention = allowance(scene);
    const auto position = source_.next();
    if (!position) {
        return std::unexpected(error(position.error()));
    }
    auto origin = source_.prepare(scope, uuid, candidate->record, *position, std::chrono::steady_clock::now(), renewal ? Source::Form::renew : Source::Form::data);
    if (!origin) {
        return std::unexpected(error(origin.error()));
    }
    auto choice = choose(origin->name(), candidate->record, nullptr, stamp->time);
    if (!choice) {
        return std::unexpected(choice.error());
    }
    std::optional<Projection::Edit> projection;
    if (choice->changed) {
        auto prepared = scene.prepare(choice->name, choice->content(), std::chrono::steady_clock::now(), scene.find(uuid).record.has_value(), retention);
        if (!prepared) {
            return std::unexpected(error(prepared.error()));
        }
        projection.emplace(std::move(*prepared));
    }
    const auto timer = timers_.find(origin->name().get()); // 名称指针查钩子, 无 UUID 格式化/Key 分配.
    if (timer == timers_.end()) {
        throw std::logic_error("Active Ephemeris lacks its timer");
    }

    stamp = reading(); // 准备期间过期的原始租约不得通过旧读数续活.
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    candidate = renewal ? Ephemeris::renew(*old, order, *stamp) : Ephemeris::update(*old, data, order, *stamp);
    if (!candidate) {
        return std::unexpected(error(candidate.error()));
    }
    if (choice->record && choice->name != origin->name() && choice->record->deadline <= stamp->time) {
        return std::unexpected(Error::ended);
    }
    for (auto& row : choice->rows) {
        if (!row.owner) {
            row.record = candidate->record;
        }
    }
    if (!origin->revise(candidate->record)) {
        throw std::logic_error("Ephemeris final check changed prepared payload cost");
    }
    retired.source = origin->commit();
    if (projection) {
        retired.scene = projection->commit();
    }
    choice->commit();
    agenda_->set(*timer->second, candidate->record.deadline);
    if (projection) {
        publish(scene, before, retired.scene.event);
    }
    return {};
}

std::expected<Ephemeris::State::Retired, Ephemeris::State::Error> Ephemeris::State::erase(const Scope& scope, std::string_view uuid, std::chrono::steady_clock::time_point now, bool active) {

    const auto old = source_.find(scope, uuid);
    if (!old) {
        return std::unexpected(Error::ended);
    }
    const auto located = obtain(scope);
    if (!located) {
        return std::unexpected(located.error());
    }
    auto& scene = **located;
    const auto before = scene.history();
    const auto retention = allowance(scene);
    const auto position = source_.next();
    if (!position) {
        return std::unexpected(error(position.error()));
    }
    auto origin = source_.prepare(scope, uuid, {}, *position, now, Source::Form::erase);
    if (!origin) {
        return std::unexpected(error(origin.error()));
    }
    const auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    auto choice = choose(origin->name(), {}, nullptr, stamp->time);
    if (!choice) {
        return std::unexpected(choice.error());
    }
    std::optional<Projection::Edit> projection;
    if (choice->changed) {
        auto prepared = scene.prepare(choice->name, choice->content(), now, scene.find(uuid).record.has_value() && choice->record.has_value(), retention);
        if (!prepared) {
            return std::unexpected(error(prepared.error()));
        }
        projection.emplace(std::move(*prepared));
    }
    const auto timer = timers_.find(origin->name().get());
    if (timer == timers_.end()) {
        throw std::logic_error("Active Ephemeris lacks its timer");
    }
    if (active) {
        const auto stamp = reading(); // 显式注销在准备后也确认 UUID 尚未过期.
        if (!stamp) {
            return std::unexpected(stamp.error());
        }
        const auto checked = Ephemeris::active(*old, *stamp);
        if (!checked) {
            return std::unexpected(error(checked.error()));
        }
    }

    Retired retired; // 移到外层域锁之外释放, 不在这里析构非空载荷.
    retired.source = origin->commit();
    if (projection) {
        retired.scene = projection->commit();
    }
    choice->commit();
    Agenda::erase(*timer->second); // 移到锁外前必须摘链, 析构不再访问活动轮.
    retired.timer = std::move(timer->second);
    timers_.erase(timer);
    if (projection) {
        publish(scene, before, retired.scene.event);
    }
    return retired;
}

std::expected<void, Ephemeris::State::Error> Ephemeris::State::remove(const Scope& scope, std::string_view uuid, std::uint64_t generation, std::string_view capability) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.remove");

    if (!scope.valid() || !Ephemeris::valid(uuid)) {
        return std::unexpected(Error::input);
    }
    std::vector<Retired> expired;
    std::optional<Retired> retired; // 声明早于锁, 删除资源在解锁后回收.
    ASTRA_PROFILE_BEGIN(profile_lock_373, "star.ephemeris_state.Ephemeris.State.remove.wait.lock");
    const std::lock_guard lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_373);
    const auto stamp = reading();
    if (!stamp) {
        return std::unexpected(stamp.error());
    }
    advance(stamp->time, expired);
    ASTRA_PROFILE_BEGIN(profile_lock_379, "star.ephemeris_state.Ephemeris.State.remove.wait.origin_lock");
    const std::unique_lock origin_lock(*export_); // 本机根/日志发布与导出同域, 远端范围安装无需持有此锁.
    ASTRA_PROFILE_END(profile_lock_379);
    const auto old = source_.find(scope, uuid);
    if (!old) {
        return std::unexpected(Error::ended);
    }
    if (old->generation != generation || (generation && uuid != Ephemeris::identity(scope, old->attr, old->ttl, capability))) {
        return std::unexpected(Error::obsolete);
    }
    auto removed = erase(scope, uuid, std::chrono::steady_clock::now(), true);
    if (!removed) {
        return std::unexpected(removed.error());
    }
    retired.emplace(std::move(*removed));
    return {};
}

std::shared_ptr<Ephemeris::State::Replica> Ephemeris::State::advance(Clock::Time now, std::vector<Retired>& retired, Replica* held) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.advance");

    std::shared_ptr<Replica> blocked; // 返回仍有到期/退役责任的忙来源, 公开读取稍后在域锁外等待.
    if (now < due_)
        return {}; // 本轮无任何时间轮能前进, 不再逐来源遍历; 不是 max_ticks 或时间截断.
    for (auto item = replicas_.begin(); item != replicas_.end();) {
        const auto owner = item->second; // 保活到 preparing 解锁后, 回收目录不能先销毁仍被锁住的 mutex.
        auto& replica = *owner;          // 保持当前组地址直到本轮到期处理完成.
        if (&replica == held) {
            ++item;
            continue; // 已持非递归来源锁, 不能再次 try_lock, 更不能访问正在编辑的原生目录.
        }
        ASTRA_PROFILE_BEGIN(profile_lock_400, "star.ephemeris_state.Ephemeris.State.advance.wait.preparing");
        const std::unique_lock preparing(replica.mutex, std::try_to_lock); // 持域锁时只尝试, 禁止等待形成反向锁序.
        ASTRA_PROFILE_END(profile_lock_400);
        if (!preparing.owns_lock()) {
            if (!blocked && (replica.retired || (replica.agenda && replica.agenda->next() <= now)))
                blocked = item->second;
            ++item;
            continue;
        }
        advance(replica, now, retired);
        if (replica.retired && replica.timers.empty()) {
            item = replicas_.erase(item); // 无原生行/活动调度/所有权索引, 不永久保留每个旧进程的空轮.
        } else {
            ++item;
        }
    }
    ASTRA_PROFILE_BEGIN(profile_lock_414, "star.ephemeris_state.Ephemeris.State.advance.wait.origin_lock");
    const std::unique_lock origin_lock(*export_); // 远端清理已经结束, 只为本机来源维护获取导出锁.
    ASTRA_PROFILE_END(profile_lock_414);
    if (agenda_)
        agenda_->advance(now, [&](Agenda::Node* hook, Clock::Time boundary) {
            auto& timer = *static_cast<Timer*>(hook); // 钩子仅由本 State 分配, 不接受外部任意节点.
            const auto record = source_.find(*timer.name->scope, timer.name->key);
            if (!record) {
                throw std::logic_error("Scheduled Ephemeris lacks its native record");
            }
            if (record->deadline > boundary) {
                agenda_->set(timer, record->deadline); // 长期限分段唤醒不能提前删除.
                return;
            }
            retired.emplace_back(); // 先准备锁外回收空间, 分配失败由 Agenda 重排当前钩子.
            auto removed = erase(*timer.name->scope, timer.name->key, std::chrono::steady_clock::now());
            if (!removed) {
                throw std::runtime_error("Ephemeris expiry could not commit");
            }
            retired.back() = std::move(*removed); // 本机到期为权威结束, 与下游删除一起发布.
        });
    // 忙碌来源仍以其旧边界参加最小值, 不能把未完成维护伪装成时间已追平; 异常保持旧 due_.
    due_ = Clock::Time::max();
    const auto include = [&](const Agenda* agenda) {
        if (agenda)
            due_ = std::min(due_, agenda->next());
    };
    include(agenda_.get());
    for (const auto& [id, replica] : replicas_)
        include(replica->agenda.get());
    if (blocked)
        due_ = std::min(due_, now); // 空退役来源也有待回收责任, 不能因没有轮而丢失下次检查.
    return blocked;
}

void Ephemeris::State::tick() {

    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.tick");

    std::vector<Retired> retired; // 等待/回收不持域锁, 失败仍保留之前已经完成的到期删除.
    ASTRA_PROFILE_BEGIN(profile_lock_450, "star.ephemeris_state.Ephemeris.State.tick.wait.lock");
    std::unique_lock lock(*gate_);
    ASTRA_PROFILE_END(profile_lock_450);
    if (!agenda_ && replicas_.empty())
        return; // 尚无任何清理责任时允许 Clock 尚未初始化.
    for (;;) {
        const auto stamp = reading(); // 每次等待后重新采样, 不拿先前读数作为完成证据.
        if (!stamp)
            throw std::runtime_error("Ephemeris clock is unavailable");
        auto blocked = advance(stamp->time, retired);
        if (!blocked)
            return;
        Guard waiting(std::move(blocked), lock); // 等待期间其他来源/本地写入仍可取得域锁.
    }
}

auto Ephemeris::State::execute(auto&& action) {
    return Reading<State>::execute(*this, std::forward<decltype(action)>(action));
}

std::expected<Ephemeris::State::Projection::View, Ephemeris::State::Error> Ephemeris::State::capture(const Scope& scope) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.capture");

    if (!scope.valid()) {
        return std::unexpected(Error::input);
    }

    // scene 仅在域锁内借用, 返回的 View 自持页面和同步域, 不把指针泄漏到锁外.
    return execute([&]() -> std::expected<Projection::View, Error> {
        const auto* scene = locate(scope); // 未创建的范围不占永久目录额度, 观察注册仍由 Feed 保持.
        return scene ? scene->capture() : Projection::empty(gate_);
    });
}

std::expected<Ephemeris::State::Projection::Point, Ephemeris::State::Error> Ephemeris::State::find(const Scope& scope, std::string_view uuid) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.find");

    if (!scope.valid() || !Ephemeris::valid(uuid)) {
        return std::unexpected(Error::input);
    }

    // uuid 同步借用调用参数, Point 拥有名称/正文引用; 未创建范围返回版本零的空结果.
    return execute([&]() -> std::expected<Projection::Point, Error> {
        const auto* scene = locate(scope); // 缺失范围与缺失目标都返回明确缺项, 不隐式创建 Scope.
        return scene ? scene->find(uuid) : Projection::Point{};
    });
}

std::expected<std::vector<Ephemeris::State::Projection::Event>, Ephemeris::State::Error> Ephemeris::State::changes(const Scope& scope, std::uint64_t since) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.changes");

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

Ephemeris::State::Source::View Ephemeris::State::source() {

    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.source");
    ASTRA_PROFILE_BEGIN(profile_lock_513, "star.ephemeris_state.Ephemeris.State.source.wait.lock");
    const std::shared_lock lock(*export_); // 来源事实自带 deadline, 接收端自行过期; 导出不触发任何域 GC.
    ASTRA_PROFILE_END(profile_lock_513);
    return source_.capture();
}

std::expected<std::vector<Ephemeris::State::Source::Event>, Ephemeris::State::Error> Ephemeris::State::events(std::uint64_t since) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.events");
    ASTRA_PROFILE_BEGIN(profile_lock_518, "star.ephemeris_state.Ephemeris.State.events.wait.lock");
    const std::shared_lock lock(*export_);
    ASTRA_PROFILE_END(profile_lock_518);
    return source_.replay(since).transform_error([](Source::Error failure) { return failure == Source::Error::version ? Error::input : error(failure); });
}

std::expected<Ephemeris::State::Source::Delivery, Ephemeris::State::Error> Ephemeris::State::deliver(std::uint64_t since, std::size_t count, std::size_t bytes) {

    ASTRA_PROFILE_SCOPE("star.ephemeris_state.Ephemeris.State.deliver");
    ASTRA_PROFILE_BEGIN(profile_lock_523, "star.ephemeris_state.Ephemeris.State.deliver.wait.lock");
    const std::shared_lock lock(*export_); // 快照/后缀捕获不等待远端安装、公开投影准备或其他来源 GC.
    ASTRA_PROFILE_END(profile_lock_523);
    return source_.deliver(since, count, bytes).transform_error([](Source::Error failure) { return failure == Source::Error::version ? Error::input : error(failure); });
}

} // namespace astra

namespace astra {
Ephemeris::State::Choice::Choice(State& owner, Choices::iterator position, std::vector<Contribution> rows, Source::Tree::Key name, std::optional<Record> record, bool changed, bool created) : owner(&owner), position(position), rows(std::move(rows)), name(std::move(name)), record(std::move(record)), changed(changed), created(created) {}

Ephemeris::State::Choice::Choice(Choice&& other) noexcept : owner(std::exchange(other.owner, nullptr)), position(other.position), rows(std::move(other.rows)), name(std::move(other.name)), record(std::move(other.record)), changed(other.changed), created(other.created) {}

Ephemeris::State::Choice::~Choice() {
    if (owner && created) {
        owner->choices_.erase(position);
    }
}

void Ephemeris::State::Choice::commit() noexcept {
    position->second.swap(rows);
    if (position->second.empty()) {
        owner->choices_.erase(position);
    }
    owner = nullptr;
}

std::expected<Ephemeris::State::Choice, Ephemeris::State::Error> Ephemeris::State::choose(const Source::Tree::Key& name, std::optional<Record> record, const Replica* owner, Clock::Time now) {

    const auto found = choices_.find(name);
    std::vector<Contribution> rows = found == choices_.end() ? std::vector<Contribution>{} : found->second;
    std::erase_if(rows, [&](const Contribution& row) { return row.owner == owner; });
    if (record) {
        for (const auto& row : rows) {
            if (row.record.deadline <= now) {
                continue;
            }
            if (!record->generation || !row.record.generation || record->ttl != row.record.ttl || *record->attr != *row.record.attr || (record->update == row.record.update && *record->data != *row.record.data)) {
                return std::unexpected(Error::conflict);
            }
        }
        rows.push_back({name, *record, owner});
    }
    const Contribution* selected{};
    for (const auto& row : rows) {
        if (row.record.deadline <= now) {
            continue;
        }
        if (!selected || std::tie(row.record.update, row.record.generation, row.record.deadline) > std::tie(selected->record.update, selected->record.generation, selected->record.deadline)) {
            selected = &row;
        }
    }
    auto key = selected ? selected->name : name;
    auto winner = selected ? std::optional(selected->record) : std::nullopt;
    auto* scene = locate(*name->scope);
    const auto visible = scene ? scene->find(name->key) : Projection::Point{};
    const bool changed = winner ? !visible.record || *visible.record->attr != *winner->attr || *visible.record->data != *winner->data : visible.record.has_value();
    const auto position = found == choices_.end() ? choices_.emplace(name, std::vector<Contribution>{}).first : found;
    return Choice(*this, position, std::move(rows), std::move(key), std::move(winner), changed, found == choices_.end());
}
} // namespace astra
