#pragma once

#include "verdandi/registration/registration.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace verdandi::registration {

/// Selector 可见且由 Redis 协议维护的 Registration 元数据。
struct metadata {
    /// 本次服务进程启动生成的固定身份。
    std::string uuid;
    /// Registration 独立维护的内容版本；Renew 不推进此值。
    std::uint64_t revision{};
    /// 最近一次 Redis 接受 Register、Update 或 Renew 的 Unix 毫秒时间。
    std::uint64_t timestamp{};
    /// Registration 发布时固定的租约毫秒数。
    std::uint64_t ttl{};
    /// 应用定义的正整数版本。
    std::uint64_t version{};
};

/// 标识一个 Zone 内需要同步到本地的 Registry Type。
struct selector_options {
    /// Registry Type；首字符是 ASCII 字母，总长 1..64 字节。
    std::string type;
};

namespace detail {
class selector_core;

struct selector_record {
    metadata meta;
    fields attr;
    fields data;
    std::shared_ptr<const void> projected_attr;
    std::shared_ptr<const void> projected_data;
    std::uint64_t deadline{};
    std::size_t size{};
};

struct retained_record {
    std::shared_ptr<const selector_record> record;
    std::uint64_t until{};
};

struct selector_view {
    std::uint64_t generation{};
    bool synchronized{false};
    std::map<std::string, std::shared_ptr<const selector_record>, std::less<>> records;
    std::vector<std::shared_ptr<const selector_record>> ordered;
    std::map<std::string, retained_record, std::less<>> retained;
    std::vector<retained_record> ordered_retained;
};

using field_projector = result<std::shared_ptr<const void>> (*)(const fields&);

struct projector {
    field_projector attr;
    field_projector data;
};

[[nodiscard]] result<std::shared_ptr<selector_core>> create_selector(const std::shared_ptr<client_core>& owner, selector_options options, projector project);
[[nodiscard]] std::shared_ptr<const selector_view> selector_current_view(const std::shared_ptr<selector_core>& value) noexcept;
[[nodiscard]] result<void> selector_validate_data(const std::shared_ptr<selector_core>& value, const fields& data);
[[nodiscard]] std::chrono::milliseconds selector_wait(const std::shared_ptr<selector_core>& value) noexcept;
[[nodiscard]] result<void> selector_close(const std::shared_ptr<selector_core>& value);
[[nodiscard]] std::optional<error> selector_error(const std::shared_ptr<selector_core>& value);
} // namespace detail

/// 只在一次 One/Any 回调内有效的不透明候选身份。
class choice final {
public:
    choice() noexcept = default;

private:
    choice(std::uint64_t token, std::size_t index) noexcept : token_(token), index_(index) {}

    std::uint64_t token_{};
    std::size_t index_{};

    template <structured_value Attr, structured_value Data>
    friend class candidates;
    template <structured_value Attr, structured_value Data>
    friend class selector;
};

/// 从 Selector 脱离、由调用方独占的强类型 Registration。
template <structured_value Attr, structured_value Data>
struct candidate {
    metadata meta;
    Attr attr;
    Data data;
};

/// 只在策略回调期间有效的只读借用候选。
template <structured_value Attr, structured_value Data>
class candidate_ref final {
public:
    /// 返回 Redis 管理的元数据借用。
    [[nodiscard]] const metadata& meta() const noexcept {
        return *meta_;
    }

    /// 返回生命周期内不可变的 Attr 借用。
    [[nodiscard]] const Attr& attr() const noexcept {
        return *attr_;
    }

    /// 返回当前事务已暂存预测优先的 Data 借用。
    [[nodiscard]] const Data& data() const noexcept {
        return *data_;
    }

    /// 返回只能交回当前 Candidates 的不透明身份。
    [[nodiscard]] choice identity() const noexcept {
        return identity_;
    }

private:
    candidate_ref(const metadata* meta, const Attr* attr, const Data* data, choice identity) noexcept
        : meta_(meta), attr_(attr), data_(data), identity_(identity) {}

    const metadata* meta_{};
    const Attr* attr_{};
    const Data* data_{};
    choice identity_;

    template <structured_value OtherAttr, structured_value OtherData>
    friend class candidates;
};

/// 一条额外保留一个 TTL、不可参与选择的恢复记录。
template <structured_value Attr, structured_value Data>
struct retained_candidate {
    candidate<Attr, Data> value;
    std::uint64_t retained_until{};
};

/// 一份脱离 Selector 且按 UUID 排序的完整强类型视图。
template <structured_value Attr, structured_value Data>
struct selection_snapshot {
    std::uint64_t generation{};
    bool synchronized{false};
    std::vector<candidate<Attr, Data>> candidates;
    std::vector<retained_candidate<Attr, Data>> retained;
};

template <structured_value Attr, structured_value Data>
class selector;

/// 传给 One/Any 的有序借用视图和事务性本地 Data 预测入口。
template <structured_value Attr, structured_value Data>
class candidates final {
public:
    /// 返回当前活动候选数量。
    [[nodiscard]] std::size_t size() const noexcept {
        return owner_->entries_.size();
    }

    /// 返回视图是否为空。
    [[nodiscard]] bool empty() const noexcept {
        return owner_->entries_.empty();
    }

    /// 借用按 UUID 排序的第 `index` 个候选；越界返回空 optional。
    [[nodiscard]] std::optional<candidate_ref<Attr, Data>> get(const std::size_t index) const noexcept {
        if (index >= owner_->entries_.size()) {
            return std::nullopt;
        }
        const auto& entry = owner_->entries_[index];
        const auto* data = entry.staged_data ? &*entry.staged_data : entry.data;
        return candidate_ref<Attr, Data>(&entry.record->meta, entry.attr, data, choice(owner_->token_, index));
    }

    /// 在当前事务中修改独立 Data 副本；回调失败或 One/Any 失败时不提交预测。
    template <class Mutate>
    [[nodiscard]] result<void> mutate(const choice selected, Mutate&& mutate) {
        if (selected.token_ != owner_->token_ || selected.index_ >= owner_->entries_.size()) {
            return std::unexpected(error(code::contract, "candidate"));
        }
        auto& entry = owner_->entries_[selected.index_];
        const fields& current = entry.staged_data ? entry.staged_fields : *entry.data_fields;
        auto decoded = [&]() -> result<Data> {
            if constexpr (std::copy_constructible<Data>) {
                return verdandi::detail::invoke_application(
                    "data", [&]() -> result<Data> { return result<Data>(std::in_place, entry.staged_data ? *entry.staged_data : *entry.data); });
            } else {
                return decode_value<Data>(current);
            }
        }();
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        auto changed = verdandi::detail::invoke_application("callback", [&]() -> result<void> {
            if constexpr (std::same_as<std::invoke_result_t<Mutate, Data&>, result<void>>) {
                return std::invoke(std::forward<Mutate>(mutate), *decoded);
            } else {
                std::invoke(std::forward<Mutate>(mutate), *decoded);
                return {};
            }
        });
        if (!changed) {
            return changed;
        }
        auto encoded = encode_value(*decoded);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        if (!std::ranges::equal(*encoded, current, [](const auto& left, const auto& right) { return left.first == right.first; })) {
            return std::unexpected(error(code::contract, "data"));
        }
        if (auto status = detail::selector_validate_data(owner_->core_, *encoded); !status) {
            return status;
        }
        entry.staged_data = std::move(*decoded);
        entry.staged_fields = std::move(*encoded);
        return {};
    }

private:
    explicit candidates(selector<Attr, Data>* owner) noexcept : owner_(owner) {}

    selector<Attr, Data>* owner_{};

    friend class selector<Attr, Data>;
};

/// 一个强类型本地 Registry 视图；长期只有一个监听任务，同步时至多增加一个临时任务。
template <structured_value Attr, structured_value Data>
class selector final {
public:
    selector() noexcept = default;
    selector(const selector&) = delete;
    selector& operator=(const selector&) = delete;
    selector(selector&&) = delete;
    selector& operator=(selector&&) = delete;
    ~selector() {
        if (core_) {
            static_cast<void>(close());
        }
    }

    /// 先订阅 Registry，再完成权威分页读取和订阅连接栅栏，成功后才返回可选择对象。
    [[nodiscard]] static result<std::unique_ptr<selector>> create(const client& owner, selector_options options) {
        if (!owner.core_) {
            return std::unexpected(error(code::closed));
        }
        detail::projector projections{&project_value<Attr>, &project_value<Data>};
        auto core = detail::create_selector(owner.core_, std::move(options), projections);
        if (!core) {
            return std::unexpected(core.error());
        }
        return std::unique_ptr<selector>(new selector(std::move(*core)));
    }

    /// 在调用方线程执行策略并最多选择一个候选；回调返回空 choice 表示无匹配。
    template <class Choose>
    [[nodiscard]] result<std::optional<candidate<Attr, Data>>> one(Choose&& choose) {
        const auto wait = detail::selector_wait(core_);
        auto transaction = begin_transaction(wait);
        if (!transaction) {
            return std::unexpected(transaction.error());
        }
        candidates<Attr, Data> values(this);
        const auto started = std::chrono::steady_clock::now();
        return verdandi::detail::invoke_application("callback", [&]() -> result<std::optional<candidate<Attr, Data>>> {
            auto selected = std::invoke(std::forward<Choose>(choose), values);
            if (std::chrono::steady_clock::now() - started > wait) {
                return std::unexpected(error(code::deadline, "callback"));
            }
            if (!selected) {
                return std::unexpected(selected.error());
            }
            if (!*selected) {
                return std::optional<candidate<Attr, Data>>{};
            }
            auto output = detach(**selected);
            if (!output) {
                return std::unexpected(output.error());
            }
            commit();
            return std::optional<candidate<Attr, Data>>(std::move(*output));
        });
    }

    /// 在调用方线程执行策略并选择多个互不重复候选；空集合不提交本地预测。
    template <class Choose>
    [[nodiscard]] result<std::vector<candidate<Attr, Data>>> any(Choose&& choose) {
        const auto wait = detail::selector_wait(core_);
        auto transaction = begin_transaction(wait);
        if (!transaction) {
            return std::unexpected(transaction.error());
        }
        candidates<Attr, Data> values(this);
        const auto started = std::chrono::steady_clock::now();
        return verdandi::detail::invoke_application("callback", [&]() -> result<std::vector<candidate<Attr, Data>>> {
            auto selected = std::invoke(std::forward<Choose>(choose), values);
            if (std::chrono::steady_clock::now() - started > wait) {
                return std::unexpected(error(code::deadline, "callback"));
            }
            if (!selected) {
                return std::unexpected(selected.error());
            }
            std::vector<candidate<Attr, Data>> output;
            output.reserve(selected->size());
            for (const auto value : *selected) {
                if (value.token_ != token_ || value.index_ >= entries_.size() || selection_marks_[value.index_] == token_) {
                    return std::unexpected(error(code::contract, "candidate"));
                }
                selection_marks_[value.index_] = token_;
                auto detached = detach(value);
                if (!detached) {
                    return std::unexpected(detached.error());
                }
                output.push_back(std::move(*detached));
            }
            if (!output.empty()) {
                commit();
            }
            return output;
        });
    }

    /// 返回完整脱离视图；这是显式重型操作，不执行 Redis I/O。
    [[nodiscard]] result<selection_snapshot<Attr, Data>> snapshot() {
        auto transaction = begin_transaction(detail::selector_wait(core_));
        if (!transaction) {
            return std::unexpected(transaction.error());
        }
        selection_snapshot<Attr, Data> output;
        output.generation = view_->generation;
        output.synchronized = view_->synchronized;
        output.candidates.reserve(entries_.size());
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            auto detached = detach(choice(token_, index));
            if (!detached) {
                return std::unexpected(detached.error());
            }
            output.candidates.push_back(std::move(*detached));
        }
        output.retained.reserve(view_->ordered_retained.size());
        for (const auto& retained : view_->ordered_retained) {
            auto detached = detach_record(retained.record);
            if (!detached) {
                return std::unexpected(detached.error());
            }
            output.retained.push_back({std::move(*detached), retained.until});
        }
        return output;
    }

    /// 关闭监听和可能存在的临时同步任务；幂等且不关闭 Registration Client。
    [[nodiscard]] result<void> close() {
        closed_.store(true, std::memory_order_release);
        return detail::selector_close(core_);
    }

    /// 非阻塞取得一条同步、恢复或协议诊断。
    [[nodiscard]] std::optional<error> try_error() {
        return detail::selector_error(core_);
    }

private:
    friend class candidates<Attr, Data>;

    struct overlay {
        std::uint64_t revision{};
        Data data;
        fields base;
        fields value;
    };

    struct entry {
        std::shared_ptr<const detail::selector_record> record;
        const Attr* attr{};
        const Data* data{};
        const fields* data_fields{};
        std::optional<Data> staged_data;
        fields staged_fields;
    };

    explicit selector(std::shared_ptr<detail::selector_core> core) : core_(std::move(core)) {}

    template <structured_value Value>
    [[nodiscard]] static result<std::shared_ptr<const void>> project_value(const fields& value) {
        return verdandi::detail::invoke_application("selector", [&]() -> result<std::shared_ptr<const void>> {
            auto projected = decode_value<Value>(value);
            if (!projected) {
                return std::unexpected(projected.error());
            }
            return std::shared_ptr<const void>(std::make_shared<const Value>(std::move(*projected)));
        });
    }

    using transaction_lock = std::unique_lock<std::timed_mutex>;

    /// 在超时内取得唯一策略事务并刷新可用视图；返回值持锁至调用方事务结束。
    [[nodiscard]] result<transaction_lock> begin_transaction(const std::chrono::milliseconds wait) {
        transaction_lock lock(operation_, std::defer_lock);
        if (!lock.try_lock_for(wait)) {
            return std::unexpected(error(code::deadline, "selector"));
        }
        auto prepared = verdandi::detail::invoke_application("selector", [&] { return begin(); });
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        return result<transaction_lock>(std::in_place, std::move(lock));
    }

    [[nodiscard]] result<void> begin() {
        if (closed_.load(std::memory_order_acquire) || !core_) {
            return std::unexpected(error(code::closed));
        }
        auto current = detail::selector_current_view(core_);
        if (!current || !current->synchronized) {
            return std::unexpected(error(code::unavailable, "selector"));
        }
        reconcile(current);
        view_ = std::move(current);
        ++token_;
        if (token_ == 0) {
            std::ranges::fill(selection_marks_, std::uint64_t{});
            token_ = 1;
        }
        entries_.clear();
        entries_.reserve(view_->ordered.size());
        for (const auto& record : view_->ordered) {
            auto attr = std::static_pointer_cast<const Attr>(record->projected_attr);
            auto data = std::static_pointer_cast<const Data>(record->projected_data);
            const auto overlay = overlays_.find(record->meta.uuid);
            if (overlay == overlays_.end()) {
                entries_.push_back({record, attr.get(), data.get(), &record->data, std::nullopt, {}});
            } else {
                entries_.push_back({record, attr.get(), &overlay->second.data, &overlay->second.value, std::nullopt, {}});
            }
        }
        if (selection_marks_.size() < entries_.size()) {
            selection_marks_.resize(entries_.size());
        }
        return {};
    }

    void reconcile(const std::shared_ptr<const detail::selector_view>& current) {
        if (view_.get() == current.get()) {
            return;
        }
        for (auto iterator = overlays_.begin(); iterator != overlays_.end();) {
            const auto found = current->records.find(iterator->first);
            if (found == current->records.end()) {
                iterator = overlays_.erase(iterator);
                continue;
            }
            auto& local = iterator->second;
            const auto& record = *found->second;
            if (local.revision != record.meta.revision) {
                for (const auto& [name, value] : record.data) {
                    const auto previous = local.base.find(name);
                    if (previous == local.base.end() || previous->second != value) {
                        local.value.insert_or_assign(name, value);
                    }
                }
                auto decoded = decode_value<Data>(local.value);
                if (decoded) {
                    local.data = std::move(*decoded);
                    local.base = record.data;
                    local.revision = record.meta.revision;
                } else {
                    iterator = overlays_.erase(iterator);
                    continue;
                }
            }
            ++iterator;
        }
    }

    [[nodiscard]] result<candidate<Attr, Data>> detach(const choice selected) const {
        if (selected.token_ != token_ || selected.index_ >= entries_.size()) {
            return std::unexpected(error(code::contract, "candidate"));
        }
        const auto& entry = entries_[selected.index_];
        if constexpr (std::copy_constructible<Attr> && std::copy_constructible<Data>) {
            return verdandi::detail::invoke_application("candidate", [&]() -> result<candidate<Attr, Data>> {
                return candidate<Attr, Data>{entry.record->meta, *entry.attr, entry.staged_data ? *entry.staged_data : *entry.data};
            });
        }
        return detach_record(entry.record, entry.staged_data ? &entry.staged_fields : entry.data_fields);
    }

    [[nodiscard]] static result<candidate<Attr, Data>> detach_record(const std::shared_ptr<const detail::selector_record>& record,
                                                                     const fields* selected_data = nullptr) {
        if constexpr (std::copy_constructible<Attr> && std::copy_constructible<Data>) {
            if (selected_data == nullptr) {
                const auto attr = std::static_pointer_cast<const Attr>(record->projected_attr);
                const auto data = std::static_pointer_cast<const Data>(record->projected_data);
                return verdandi::detail::invoke_application(
                    "candidate", [&]() -> result<candidate<Attr, Data>> { return candidate<Attr, Data>{record->meta, *attr, *data}; });
            }
        }
        auto attr = decode_value<Attr>(record->attr);
        if (!attr) {
            return std::unexpected(attr.error());
        }
        auto data = decode_value<Data>(selected_data == nullptr ? record->data : *selected_data);
        if (!data) {
            return std::unexpected(data.error());
        }
        return candidate<Attr, Data>{record->meta, std::move(*attr), std::move(*data)};
    }

    void commit() {
        for (auto& entry : entries_) {
            if (!entry.staged_data) {
                continue;
            }
            overlays_.insert_or_assign(entry.record->meta.uuid,
                                       overlay{entry.record->meta.revision, std::move(*entry.staged_data), entry.record->data, std::move(entry.staged_fields)});
        }
    }

    std::shared_ptr<detail::selector_core> core_;
    std::timed_mutex operation_;
    std::map<std::string, overlay, std::less<>> overlays_;
    std::shared_ptr<const detail::selector_view> view_;
    std::vector<entry> entries_;
    std::vector<std::uint64_t> selection_marks_;
    std::uint64_t token_{};
    std::atomic_bool closed_{false};
};

} // namespace verdandi::registration
