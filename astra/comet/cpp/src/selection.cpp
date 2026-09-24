#include "selection.hpp"
#include <algorithm>
#include <astra/scope.hpp>

namespace comet {
Observer::State Observer::View::state() const noexcept {
    return state_;
}

const Scope& Observer::View::scope() const noexcept {
    return scope_;
}

const std::string& Observer::View::target() const noexcept {
    return target_;
}

const std::string& Observer::View::instance() const noexcept {
    static const std::string empty; // 未就绪视图的稳定空引用, 不借用已关闭对象.
    return contents_ ? contents_->instance : empty;
}

std::optional<std::uint64_t> Observer::View::version() const noexcept {
    return contents_ ? std::optional(contents_->version) : std::nullopt;
}

std::size_t Observer::View::size() const noexcept {
    return contents_ ? contents_->data.size() : 0;
}

const std::optional<Error>& Observer::View::error() const noexcept {
    return error_;
}

std::optional<Observer::Record> Observer::View::find(std::string_view uuid) const {
    const auto* record = contents_ ? contents_->data.find(uuid) : nullptr; // 本 View 保持两份不可变载荷的寿命.
    return record ? std::optional(Record{record->attr, record->data}) : std::nullopt;
}

void Observer::View::visit(void* context, void (*visitor)(void*, std::string_view, const Record&)) const {
    if (contents_) {
        contents_->data.each([&](std::string_view uuid, const detail::Registration& record) {
            const Record complete{record.attr, record.data}; // 只共享所有权, 不复制任意 Attr/Data 字节.
            visitor(context, uuid, complete);
        });
    }
}

} // namespace comet

namespace comet::detail {
Selection::Selection(Scope scope, std::string target, std::size_t bytes, std::size_t records, Reserve reserve) : scope_(std::move(scope)), target_(std::move(target)), bytes_(bytes), records_(records), reserve_(std::move(reserve)) {
    if (bytes == 0 || bytes > 1024ULL * 1024 * 1024 || records == 0 || records > 65536) {
        throw std::invalid_argument("Invalid Observer view capacity");
    }
}

Selection::~Selection() {
    if (reserve_) {
        static_cast<void>(reserve_(0));
    }
}

// Selection::discard 丢弃候选与去重状态, 保留已确认视图与游标.
// 失败路径调用, 成功提交后同样调用以释放准备资源.
void Selection::discard() noexcept {
    draft_.reset();
    std::unordered_set<std::string>{}.swap(seen_); // 不保留曾经大快照的桶数组高水线.
    seen_bytes_ = 0;
    mode_ = proto::comet::v1::MODE_UNSPECIFIED;
    if (reserve_) {
        static_cast<void>(reserve_(current_ ? current_->data.footprint() : 0));
    }
}

// Selection::begin 开始新实例的候选, 记录期望实例, 清空旧候选.
// expected 为期望实例, 跨实例即全量替换.
void Selection::begin(std::string expected) {
    discard();
    expected_ = std::move(expected);
    first_ = true;
    valid_ = true;
}

Result<std::optional<Observer::View>> Selection::fail(Error::Code code) {
    discard();
    valid_ = false;
    return std::unexpected(Error{code, Error::Effect::unapplied, {}, {}, {}});
}

Observer::View Selection::view(Observer::State state, std::optional<Error> error) const {
    Observer::View result; // 标量状态与当前完整根一起复制, 不改变旧 View 的状态或版本.
    result.contents_ = current_;
    result.state_ = state;
    result.scope_ = scope_;
    result.target_ = target_;
    result.error_ = std::move(error);
    return result;
}

Result<std::optional<Observer::View>> Selection::accept(const proto::comet::v1::EphemerisWatchReply& page) {

    // 所有批次只在完整尾页出现位置, 非尾空页没有业务意义, 禁止用它构造无限无进展流.
    if (!valid_ || (page.mode() != proto::comet::v1::MODE_RESET && page.mode() != proto::comet::v1::MODE_APPLY) || page.complete() != page.has_version() || page.complete() != !page.instance().empty() || (!page.complete() && page.changes().empty()) || page.ByteSizeLong() > 8 * 1024 * 1024) {
        return fail(Error::Code::protocol);
    }
    if (page.complete() && (!astra::Scope::text(page.instance(), 128) || (!expected_.empty() && expected_ != page.instance()) || (current_ && current_->instance == page.instance() && page.version() < current_->version))) {
        return fail(Error::Code::protocol);
    }
    if (!draft_) {
        if ((page.mode() == proto::comet::v1::MODE_RESET && !first_) || (page.mode() == proto::comet::v1::MODE_APPLY && (!current_ || fresh_))) {
            return fail(Error::Code::protocol);
        }
        mode_ = page.mode();
        if (mode_ == proto::comet::v1::MODE_RESET) {
            draft_.emplace(Table<Registration>{});
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
        if (!valid(change.uuid()) || (!target_.empty() && target_ != change.uuid())) {
            return fail(Error::Code::protocol);
        }
        if (track) {
            if (seen_.size() == records_ * 2) {
                return fail(seen_.contains(change.uuid()) ? Error::Code::protocol : Error::Code::limit); // 满额仍优先报告重复.
            }
            if (!seen_.insert(change.uuid()).second) {
                return fail(Error::Code::protocol);
            }
            seen_bytes_ += change.uuid().size();
        }
        switch (change.action_case()) {
        case proto::comet::v1::EphemerisChange::kRecord: {
            const auto& record = change.record(); // 一条完整注册, 空 Attr/Data 同样是合法的完整值.
            if (record.attr().size() > 1024 * 1024 || record.data().size() > 1024 * 1024) {
                return fail(Error::Code::limit);
            }
            const auto* old = current_ ? current_->data.find(change.uuid()) : nullptr;
            const bool same = old && std::ranges::equal(*old->attr, record.attr(), [](std::uint8_t left, char right) { return left == static_cast<std::uint8_t>(right); }); // 完整 Attr 最多比较一次, Data-only 无须进入这条路径.
            if (mode_ == proto::comet::v1::MODE_APPLY && old && !same) {
                return fail(Error::Code::protocol); // 已知 UUID 的 Attr 永远不可借完整 record 回退改写.
            }
            auto attr = same ? old->attr : std::make_shared<const std::vector<std::uint8_t>>(record.attr().begin(), record.attr().end());
            auto data = std::make_shared<const std::vector<std::uint8_t>>(record.data().begin(), record.data().end());
            draft_->set(change.uuid(), Registration{std::move(attr), std::move(data)});
            break;
        }
        case proto::comet::v1::EphemerisChange::kData: {
            if (mode_ != proto::comet::v1::MODE_APPLY) {
                return fail(Error::Code::protocol);
            }
            if (change.data().size() > 1024 * 1024) {
                return fail(Error::Code::limit);
            }
            const auto* old = current_->data.find(change.uuid()); // 同批 UUID 不重复, 当前完整基线就是唯一 Attr 依据.
            if (!old) {
                return fail(Error::Code::history); // 内部专用回退信号, 不安装半条注册或推进游标.
            }
            draft_->set(change.uuid(), Registration{old->attr, std::make_shared<const std::vector<std::uint8_t>>(change.data().begin(), change.data().end())});
            break;
        }
        case proto::comet::v1::EphemerisChange::kErase:
            if (mode_ != proto::comet::v1::MODE_APPLY) {
                return fail(Error::Code::protocol);
            }
            draft_->erase(change.uuid());
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
        return std::optional<Observer::View>{};
    }
    if (draft_->size() > records_ || draft_->bytes() > bytes_) {
        return fail(Error::Code::limit);
    }
    if (page.version() == 0 && draft_->size() != 0) {
        return fail(Error::Code::protocol);
    }

    // 连完整 View 包装的有界字段分配也先准备, 分配失败不能已经推进根或恢复游标.
    auto next = std::make_shared<Registrations>();
    next->instance = page.instance();
    next->version = page.version();
    std::string expected = page.instance(); // 准备后再交换, 不在提交后留下可失败字符串赋值.
    auto result = view(Observer::State::ready);
    next->data = std::move(*draft_).finish();
    if (next->data.footprint() > bytes_) {
        return fail(Error::Code::limit);
    }
    result.contents_ = next;
    current_ = std::move(next);
    expected_.swap(expected);
    first_ = false;
    fresh_ = false;
    discard();
    return std::optional<Observer::View>(std::move(result));
}
} // namespace comet::detail

namespace comet::detail {
// Selection::valid 校验 UUID 二进制形状, 不分配临时字符串.
// uuid 为待校验的 16 字节原始 UUIDv4; 版本/变体位必须固定.
bool Selection::valid(std::string_view uuid) noexcept {
    if (uuid.size() != 16) {
        return false;
    }
    const auto bytes = reinterpret_cast<const std::uint8_t*>(uuid.data());
    return (bytes[6] & 0xf0U) == 0x40U && (bytes[8] & 0xc0U) == 0x80U;
}

// Selection::repair 历史不足时单对象恢复, 只允许一次, 不触发共享切换.
// code 为失败分类; 返回是否可恢复.
bool Selection::repair(Error::Code code) noexcept {
    if (code != Error::Code::history || repaired_) {
        return false;
    }
    repaired_ = true;
    fresh_ = true;
    return true;
}

// Selection::resume 返回是否可恢复游标, 新实例或修复后不可恢复.
// 未经历全量替换即可带游标恢复.
bool Selection::resume() const noexcept {
    return !fresh_;
}
} // namespace comet::detail
