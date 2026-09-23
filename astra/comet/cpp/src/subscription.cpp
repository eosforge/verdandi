#include "subscription.hpp"
#include <astra/scope.hpp>

namespace comet {
Subscriber::State Subscriber::View::state() const noexcept {
    return state_;
}

const Scope& Subscriber::View::scope() const noexcept {
    return scope_;
}

const std::string& Subscriber::View::target() const noexcept {
    return target_;
}

const std::string& Subscriber::View::instance() const noexcept {
    static const std::string empty; // 未就绪视图的稳定空引用, 不借用已关闭对象.
    return contents_ ? contents_->instance : empty;
}

std::optional<std::uint64_t> Subscriber::View::version() const noexcept {
    return contents_ ? std::optional(contents_->version) : std::nullopt;
}

std::size_t Subscriber::View::size() const noexcept {
    return contents_ ? contents_->data.size() : 0;
}

const std::optional<Error>& Subscriber::View::error() const noexcept {
    return error_;
}

std::optional<Subscriber::Record> Subscriber::View::find(std::string_view key) const {
    const auto* record = contents_ ? contents_->data.find(key) : nullptr; // 只借用本视图拥有的不可变页.
    return record ? std::optional(record->record) : std::nullopt;
}

void Subscriber::View::visit(void* context, void (*visitor)(void*, std::string_view, const Record&)) const {
    if (contents_) {
        contents_->data.each([&](std::string_view key, const detail::Publication& record) { visitor(context, key, record.record); });
    }
}
} // namespace comet

namespace comet::detail {
Subscription::Subscription(Scope scope, std::string target, std::size_t bytes, std::size_t records, Reserve reserve) : scope_(std::move(scope)), target_(std::move(target)), bytes_(bytes), records_(records), reserve_(std::move(reserve)) {
    if (bytes == 0 || bytes > 1024ULL * 1024 * 1024 || records == 0 || records > 65536) {
        throw std::invalid_argument("Invalid Subscriber view capacity");
    }
}

Subscription::~Subscription() {
    if (reserve_) {
        static_cast<void>(reserve_(0));
    }
}

void Subscription::discard() noexcept {
    draft_.reset();
    std::unordered_set<std::string>{}.swap(seen_); // 不保留曾经大快照的桶数组高水线.
    seen_bytes_ = 0;
    mode_ = proto::comet::v1::MODE_UNSPECIFIED;
    if (reserve_) {
        static_cast<void>(reserve_(current_ ? current_->data.footprint() : 0));
    }
}

void Subscription::begin(std::string expected) {
    discard();
    expected_ = std::move(expected);
    first_ = true;
    valid_ = true;
}

Result<std::optional<Subscriber::View>> Subscription::fail(Error::Code code) {
    discard();
    valid_ = false;
    return std::unexpected(Error{code, Error::Effect::unapplied, {}, {}, {}});
}

Subscriber::View Subscription::view(Subscriber::State state, std::optional<Error> error) const {
    Subscriber::View result; // 标量状态与当前完整根一起复制, 不改变旧 View 的状态或版本.
    result.contents_ = current_;
    result.state_ = state;
    result.scope_ = scope_;
    result.target_ = target_;
    result.error_ = std::move(error);
    return result;
}

Result<std::optional<Subscriber::View>> Subscription::accept(const proto::comet::v1::CatalogWatchReply& page) {

    // 所有批次只在完整尾页出现位置, 非尾空页没有业务意义, 禁止用它构造无限无进展流.
    if (!valid_ || (page.mode() != proto::comet::v1::MODE_RESET && page.mode() != proto::comet::v1::MODE_APPLY) || page.complete() != page.has_version() || page.complete() != !page.instance().empty() || (!page.complete() && page.changes().empty()) || page.ByteSizeLong() > 8 * 1024 * 1024) {
        return fail(Error::Code::protocol);
    }
    if (page.complete() && (!astra::Scope::text(page.instance(), 128) || (!expected_.empty() && expected_ != page.instance()) || (current_ && current_->instance == page.instance() && page.version() < current_->version))) {
        return fail(Error::Code::protocol);
    }
    if (!draft_) {
        if ((page.mode() == proto::comet::v1::MODE_RESET && !first_) || (page.mode() == proto::comet::v1::MODE_APPLY && !current_)) {
            return fail(Error::Code::protocol);
        }
        mode_ = page.mode();
        if (mode_ == proto::comet::v1::MODE_RESET) {
            draft_.emplace(Table<Publication>{});
        } else {
            draft_.emplace(current_->data);
        } // 直接借原根构造候选, 避免条件表达式先复制一份 Table.
    } else if (mode_ != page.mode()) {
        return fail(Error::Code::protocol);
    }
    if (page.complete() && mode_ == proto::comet::v1::MODE_APPLY && (current_->instance != page.instance() || (page.version() == current_->version && (!first_ || !seen_.empty() || !page.changes().empty())))) {
        return fail(Error::Code::protocol);
    }

    // 一页一项的完整批次不可能重复, 无须为每次小更新分配去重节点和桶.
    const bool track = !page.complete() || page.changes_size() != 1 || !seen_.empty(); // 之前已有页面时必须继续跨页去重.
    for (const auto& change : page.changes()) {
        // seen 只跟踪本批, 上限包含当前及最终两份记录集合, 不跨重连累计历史 Key.
        if (!astra::Scope::text(change.key(), 1024) || (!target_.empty() && target_ != change.key())) {
            return fail(Error::Code::protocol);
        }
        if (track) {
            if (seen_.size() == records_ * 2) {
                return fail(seen_.contains(change.key()) ? Error::Code::protocol : Error::Code::limit); // 满额仍优先报告重复.
            }
            if (!seen_.insert(change.key()).second) {
                return fail(Error::Code::protocol);
            }
            seen_bytes_ += change.key().size();
        }
        switch (change.action_case()) {
        case proto::comet::v1::CatalogChange::kValue: {
            if (change.version() == 0) {
                return fail(Error::Code::protocol);
            }
            if (change.value().size() > 1024 * 1024) {
                return fail(Error::Code::limit);
            }
            const auto& value = change.value(); // Protobuf 字节在下一 Read 会复用, 在此取得独立不可变所有权.
            draft_->set(change.key(), Publication{Subscriber::Record{change.version(), std::make_shared<const std::vector<std::uint8_t>>(value.begin(), value.end())}});
            break;
        }
        case proto::comet::v1::CatalogChange::kErase:
            if (mode_ != proto::comet::v1::MODE_APPLY || change.version() != 0) {
                return fail(Error::Code::protocol);
            }
            draft_->erase(change.key());
            break;
        default:
            return fail(Error::Code::protocol);
        }
        // apply 可能先收到 Set 再收到其他 Key 的 Delete, 暂存峰值与最终可见容量分开检查.
        if (draft_->size() > records_ * 2 || draft_->bytes() > bytes_ * 2) {
            return fail(Error::Code::limit);
        }
    }
    // 准备阶段分别容纳原内容与候选, 去重桶/Key 也计入, 共享页按保守引用寿命计量.
    const auto metadata = seen_bytes_ + seen_.size() * (sizeof(std::string) + 32) + seen_.bucket_count() * sizeof(void*);
    const auto prepared = draft_->footprint() + metadata;
    if (prepared > bytes_ * 2 || (reserve_ && !reserve_(prepared + (current_ ? current_->data.footprint() : 0)))) {
        return fail(Error::Code::limit);
    }
    if (!page.complete()) {
        return std::optional<Subscriber::View>{};
    }
    if (draft_->size() > records_ || draft_->bytes() > bytes_) {
        return fail(Error::Code::limit);
    }
    if (page.version() == 0 && draft_->size() != 0) {
        return fail(Error::Code::protocol);
    }

    // 连完整 View 包装的有界字段分配也先准备, 分配失败不能已经推进根或恢复游标.
    auto next = std::make_shared<Publications>();
    next->instance = page.instance();
    next->version = page.version();
    std::string expected = page.instance(); // 准备后再交换, 不在提交后留下可失败字符串赋值.
    auto result = view(Subscriber::State::ready);
    next->data = std::move(*draft_).finish();
    if (next->data.footprint() > bytes_) {
        return fail(Error::Code::limit);
    }
    result.contents_ = next;
    current_ = std::move(next);
    expected_.swap(expected);
    first_ = false;
    discard();
    return std::optional<Subscriber::View>(std::move(result));
}
} // namespace comet::detail
