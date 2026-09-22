#include "library.hpp"
#include <stdexcept>

namespace astra {
Library::Library() : Library(Limits{}) {}

Library::Library(Limits limits, Notify changed) : limits_(limits), changed_(std::move(changed)) {
    if (limits.scopes == 0 || limits.scopes > 16384 || limits.scope.bytes > limits.bytes || limits.bytes > 4ULL * 1024 * 1024 * 1024) {
        throw std::invalid_argument("Invalid Almanac library capacity");
    }
}

Library::Draft::Draft(const Library* owner, Scope scope, std::uint64_t version, std::shared_ptr<Almanac> book) : owner_(owner), scope_(std::move(scope)), version_(version), book_(std::move(book)), draft_(book_->prepare(version)) {}

std::expected<void, Almanac::Error> Library::Draft::set(std::string key, Almanac::Buffer value) {

    if (!Scope::text(key, 1024)) {
        return std::unexpected(Almanac::Error::input);
    }
    // cost 来自有界单记录, 原生准备成功后才更新计费, 重复 Key 不重复累计.
    const auto cost = key.size() + value.size();
    if (auto result = draft_.set(std::move(key), std::move(value)); !result) {
        return result;
    }
    bytes_ += cost;
    return {};
}

const Scope& Library::Draft::scope() const noexcept {
    return scope_;
}

std::uint64_t Library::Draft::version() const noexcept {
    return version_;
}

std::shared_ptr<Almanac> Library::locate(const Scope& scope) const {

    // lock 仅保护索引定位, 返回独立共享所有权后不需要保持外层锁.
    const std::shared_lock lock(mutex_);
    const auto sector = books_.find(scope.sector);
    if (sector == books_.end()) {
        return {};
    }
    const auto spectrum = sector->second.find(scope.spectrum);
    return spectrum == sector->second.end() ? nullptr : spectrum->second;
}

std::shared_ptr<const Almanac> Library::find(const Scope& scope) const {
    return scope.valid() ? locate(scope) : nullptr;
}

std::expected<Library::Draft, Almanac::Error> Library::prepare(Scope scope, std::uint64_t version) {

    if (!scope.valid()) {
        return std::unexpected(Almanac::Error::input);
    }
    // book 的共享所有权使接收期间不借用 Map 节点; 全量内容只在 Draft 中创建.
    auto book = locate(scope);
    if (book) {
        const auto current = book->usage();
        if (current.version && version < *current.version) {
            return std::unexpected(Almanac::Error::version);
        }
    } else {
        const std::lock_guard lock(writer_); // 新候选占用前检查完整分组容量, 提交时再检查.
        if (scopes_ >= limits_.scopes) {
            return std::unexpected(Almanac::Error::capacity);
        }
        book = std::make_shared<Almanac>(limits_.scope);
    }
    return Draft(this, std::move(scope), version, std::move(book));
}

std::expected<bool, Almanac::Error> Library::reset(Draft&& draft) {

    if (draft.owner_ != this || !draft.book_) {
        return std::unexpected(Almanac::Error::input);
    }
    // writer 串行全局计费, 不阻塞现有 Scope 的点查和不可变遍历.
    const std::lock_guard writer(writer_);
    const auto existing = locate(draft.scope_);
    if (existing && existing != draft.book_) {
        return std::unexpected(Almanac::Error::version);
    }
    const auto before = draft.book_->usage();
    if (draft.bytes_ > limits_.bytes - (bytes_ - before.bytes) || (!existing && scopes_ == limits_.scopes)) {
        return std::unexpected(Almanac::Error::capacity);
    }

    if (!existing) {
        // 新分组先准备两级路由分配, 持锁期间读者看不到未完成条目; 原生安装失败则回滚路由.
        const std::unique_lock lock(mutex_); // 新对象没有旧内容, 提交只交换已准备的根.
        const auto [sector, created] = books_.try_emplace(draft.scope_.sector);
        try {
            sector->second.emplace(draft.scope_.spectrum, draft.book_);
            if (auto installed = draft.book_->reset(std::move(draft.draft_)); !installed) {
                sector->second.erase(draft.scope_.spectrum);
                if (created) {
                    books_.erase(sector);
                }
                return installed;
            }
        } catch (...) {
            sector->second.erase(draft.scope_.spectrum);
            if (created) {
                books_.erase(sector);
            }
            throw;
        }
        ++scopes_;
    } else {
        if (auto installed = existing->reset(std::move(draft.draft_)); !installed || !*installed) {
            return installed;
        }
    }

    // 数据已完整发布, 标量更新不分配且不失败. 消费候选后不能重复安装同一可写容器.
    bytes_ = bytes_ - before.bytes + draft.bytes_;
    history_ -= before.history;
    draft.book_.reset();
    if (changed_) {
        changed_(draft.scope_, {{}, {}, draft.version_});
    }
    return true;
}

std::expected<bool, Almanac::Error> Library::apply(const Scope& scope, std::uint64_t version, std::string key, std::optional<Almanac::Buffer> value) {

    if (!scope.valid() || !Scope::text(key, 1024)) {
        return std::unexpected(Almanac::Error::input);
    }
    const std::lock_guard writer(writer_); // 唯一发布流在此维护所有分组计费, 不持路由锁.
    const auto book = locate(scope);
    if (!book) {
        return std::unexpected(Almanac::Error::unready);
    }
    const auto before = book->usage();
    if (before.version && version != 0 && version <= *before.version) {
        return false;
    }
    const auto old = book->find(key);
    if (!old) {
        return std::unexpected(old.error());
    }
    // removed/added 分别按完整旧值和新值计费, 缺失 Delete 不创建条目.
    const auto removed = old->value ? key.size() + old->value->size() : 0;
    const auto added = value ? key.size() + value->size() : 0;
    if (added > limits_.bytes - (bytes_ - removed)) {
        return std::unexpected(Almanac::Error::capacity);
    }
    // retention 只影响连续历史保留, 总历史满时仍接受权威数据, 新范围改走快照恢复.
    const auto retention = limits_.history - (history_ - before.history);
    Almanac::Change committed; // 从真正提交边界取得共享正文, 不从随后可变状态重新点查.
    const auto result = book->apply(version, std::move(key), std::move(value), retention, &committed);
    if (result && *result) {
        const auto after = book->usage();
        bytes_ = bytes_ - before.bytes + after.bytes;
        history_ = history_ - before.history + after.history;
        if (changed_) {
            changed_(scope, committed);
        }
    }
    return result;
}

std::vector<Library::Position> Library::positions() const {

    // writer 固定版本交界, lock 固定两层索引; 不在此复制载荷或调用外部函数.
    const std::lock_guard writer(writer_);
    const std::shared_lock lock(mutex_);
    std::vector<Position> positions;
    positions.reserve(scopes_);
    for (const auto& [sector, spectra] : books_) {
        for (const auto& [spectrum, book] : spectra) {
            if (const auto version = book->usage().version) {
                positions.push_back({Scope{sector, spectrum}, *version});
            }
        }
    }
    return positions;
}
} // namespace astra
