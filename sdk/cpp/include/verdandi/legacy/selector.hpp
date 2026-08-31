#pragma once

#include "verdandi/c/selector.h"
#include "verdandi/legacy/registration.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace verdandi {
namespace legacy {

/// Selector 策略使用的事务内身份；只能交还给产生它的同一次回调。
struct choice {
    /// 构造指向当前回调候选序号的选择身份；不得跨回调保存。
    explicit choice(std::size_t value = 0U) : index(value) {}
    /// 当前 Selector 事务内的候选序号。
    std::size_t index;
};

/// 已脱离 Selector 锁和回调生命周期的强类型候选。
template <class Attr, class Data>
class candidate {
public:
    /// 接管一份已脱离 Selector 锁和 C 句柄生命周期的完整候选。
    candidate(registration_metadata metadata, Attr attr, Data data) : metadata_(std::move(metadata)), attr_(std::move(attr)), data_(std::move(data)) {}

    /// 返回拥有型 Registration 元数据。
    const registration_metadata& metadata() const {
        return metadata_;
    }
    /// 返回拥有型不可变 Attr。
    const Attr& attr() const {
        return attr_;
    }
    /// 返回本次选择事务提交后的拥有型 Data。
    const Data& data() const {
        return data_;
    }

private:
    registration_metadata metadata_;
    Attr attr_;
    Data data_;
};

template <class Attr, class Data>
struct retained_candidate {
    /// 构造一条不可选择的保留候选及其 Redis 毫秒截止时间。
    retained_candidate(candidate<Attr, Data> candidate_value, std::uint64_t until_value) : value(std::move(candidate_value)), retained_until(until_value) {}

    /// 已脱离 Selector 生命周期的完整候选。
    candidate<Attr, Data> value;
    /// 该候选最晚保留到的 Redis 毫秒时间。
    std::uint64_t retained_until;
};

template <class Attr, class Data>
struct selection_snapshot {
    /// 创建快照时的本地视图 generation。
    std::uint64_t generation;
    /// 快照是否来自一个已完成权威对齐的可用视图。
    bool synchronized;
    /// 当前可选择候选的完整拥有型副本。
    std::vector<candidate<Attr, Data>> candidates;
    /// 已过期但仍在有限恢复窗口中的不可选择副本。
    std::vector<retained_candidate<Attr, Data>> retained;

    /// 构造 generation 为零且尚未同步的空快照。
    selection_snapshot() : generation(0), synchronized(false) {}
};

/// 只在 One/Any 回调期间有效的借用候选集合。
template <class Attr, class Data>
class candidates {
public:
    /// 返回本次同步策略回调可见的活动候选数量。
    std::size_t size() const {
        return value_ == NULL ? 0U : verdandi_candidates_size(value_);
    }

    /// 复制指定候选的 Registration 元数据；越界返回 `invalid`。
    result<registration_metadata> metadata(std::size_t index) const {
        if (value_ == NULL) {
            return result<registration_metadata>(error("invalid", "candidates"));
        }
        verdandi_registration_metadata native = {};
        if (verdandi_candidates_metadata(value_, index, &native) == 0) {
            return result<registration_metadata>(error("invalid", "candidate"));
        }
        return result<registration_metadata>(detail::metadata(native));
    }

    /// 读取并解码指定候选的完整不可变 Attr。
    result<Attr> attr(std::size_t index) const {
        result<fields> loaded = collect(index, true);
        if (!loaded) {
            return result<Attr>(loaded.failure());
        }
        return detail::decode_value<Attr>(*loaded);
    }

    /// 读取并解码指定候选当前预测优先的完整 Data。
    result<Data> data(std::size_t index) const {
        result<fields> loaded = collect(index, false);
        if (!loaded) {
            return result<Data>(loaded.failure());
        }
        return detail::decode_value<Data>(*loaded);
    }

    /// 一次取得指定候选的元数据、Attr 和 Data 拥有型副本。
    result<candidate<Attr, Data>> get(std::size_t index) const {
        result<registration_metadata> loaded_metadata = metadata(index);
        if (!loaded_metadata) {
            return result<candidate<Attr, Data>>(loaded_metadata.failure());
        }
        result<Attr> loaded_attr = attr(index);
        if (!loaded_attr) {
            return result<candidate<Attr, Data>>(loaded_attr.failure());
        }
        result<Data> loaded_data = data(index);
        if (!loaded_data) {
            return result<candidate<Attr, Data>>(loaded_data.failure());
        }
        return result<candidate<Attr, Data>>(candidate<Attr, Data>(std::move(*loaded_metadata), std::move(*loaded_attr), std::move(*loaded_data)));
    }

    /// 在当前事务中暂存完整 Data 预测；仅在策略成功返回并选中后提交。
    result<void> mutate(choice selected, const Data& data_value) {
        if (value_ == NULL) {
            return result<void>(error("invalid", "candidates"));
        }
        result<fields> encoded = detail::encode_value(data_value);
        if (!encoded) {
            return result<void>(encoded.failure());
        }
        detail::native_fields native(*encoded);
        verdandi_error failure = {};
        const int succeeded = verdandi_candidates_mutate(value_, selected.index, native.view(), &failure);
        return detail::status(succeeded, failure);
    }

private:
    explicit candidates(verdandi_candidates* value) : value_(value) {}

    result<fields> collect(std::size_t index, bool read_attr) const {
        detail::field_collector collector;
        verdandi_error failure = {};
        const int succeeded = read_attr ? verdandi_candidates_visit_attr(value_, index, detail::collect_field, &collector, &failure)
                                        : verdandi_candidates_visit_data(value_, index, detail::collect_field, &collector, &failure);
        return detail::collected_fields(succeeded, failure, collector);
    }

    verdandi_candidates* value_;

    template <class A, class D>
    friend class selector;
};

/// C++11 强类型 Selector；策略运行、锁和本地预测提交仍由同一个 C++23 核心完成。
template <class Attr, class Data>
class selector {
public:
    /// 构造未绑定领域和 Type 的空 Selector。
    selector() {}
    /// 转移 Selector 句柄和领域生命周期。
    selector(selector&&) noexcept = default;
    /// 释放旧句柄后转移 Selector 句柄和领域生命周期。
    selector& operator=(selector&&) noexcept = default;
    selector(const selector&) = delete;
    selector& operator=(const selector&) = delete;

    /// 为一个 Registry Type 创建 Selector，并在返回前完成初始权威同步。
    static result<selector> create(const registration_client& owner, const std::string& type) {
        if (!owner.valid()) {
            return result<selector>(error("invalid", "registration"));
        }
        verdandi_selector* output = NULL;
        verdandi_error failure = {};
        if (verdandi_selector_create(owner.state_->handle.get(), detail::native_text(type), &output, &failure) == 0) {
            return result<selector>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_selector, verdandi_selector_release> handle(output);
        selector value(owner.state_, std::move(handle));
        return result<selector>(std::move(value));
    }

    /// 返回当前是否同时持有领域和 Selector 句柄。
    bool valid() const {
        return owner_ && handle_;
    }

    /// 策略返回空 optional 表示没有匹配，返回 Choice 表示提交零或一个选择。
    /// `policy` 在调用线程同步执行，借用候选只在回调返回前有效。
    template <class Policy>
    result<optional<candidate<Attr, Data>>> one(Policy&& policy) {
        if (!valid()) {
            return result<optional<candidate<Attr, Data>>>(error("invalid", "selector"));
        }
        typedef typename std::remove_reference<Policy>::type policy_type;
        one_policy_context<policy_type> context = {&policy};
        verdandi_candidate_list* output = NULL;
        verdandi_error failure = {};
        if (verdandi_selector_one(handle_.get(), &one_policy<policy_type>, &context, &output, &failure) == 0) {
            return result<optional<candidate<Attr, Data>>>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_candidate_list, verdandi_candidate_list_release> list(output);
        if (output == NULL) {
            return result<optional<candidate<Attr, Data>>>(optional<candidate<Attr, Data>>());
        }
        if (verdandi_candidate_list_size(output) != 1U) {
            return result<optional<candidate<Attr, Data>>>(error("corrupt", "selection"));
        }
        result<candidate<Attr, Data>> decoded = list_candidate(output, 0U);
        if (!decoded) {
            return result<optional<candidate<Attr, Data>>>(decoded.failure());
        }
        return result<optional<candidate<Attr, Data>>>(optional<candidate<Attr, Data>>(std::move(*decoded)));
    }

    /// 策略返回互不重复的 Choice；空集合不提交本地预测。
    /// 返回值是完全脱离回调锁和 C 候选列表的拥有型副本。
    template <class Policy>
    result<std::vector<candidate<Attr, Data>>> any(Policy&& policy) {
        if (!valid()) {
            return result<std::vector<candidate<Attr, Data>>>(error("invalid", "selector"));
        }
        typedef typename std::remove_reference<Policy>::type policy_type;
        any_policy_context<policy_type> context = {&policy};
        verdandi_candidate_list* output = NULL;
        verdandi_error failure = {};
        if (verdandi_selector_any(handle_.get(), &any_policy<policy_type>, &context, &output, &failure) == 0) {
            return result<std::vector<candidate<Attr, Data>>>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_candidate_list, verdandi_candidate_list_release> list(output);
        std::vector<candidate<Attr, Data>> values;
        const std::size_t size = verdandi_candidate_list_size(output);
        values.reserve(size);
        for (std::size_t index = 0; index < size; ++index) {
            result<candidate<Attr, Data>> decoded = list_candidate(output, index);
            if (!decoded) {
                return result<std::vector<candidate<Attr, Data>>>(decoded.failure());
            }
            values.push_back(std::move(*decoded));
        }
        return result<std::vector<candidate<Attr, Data>>>(std::move(values));
    }

    /// 创建活动与 retained 完整脱离视图；这是显式 O(N) 重型操作。
    result<selection_snapshot<Attr, Data>> snapshot() {
        if (!valid()) {
            return result<selection_snapshot<Attr, Data>>(error("invalid", "selector"));
        }
        verdandi_selector_snapshot* output = NULL;
        verdandi_error failure = {};
        if (verdandi_selector_snapshot_create(handle_.get(), &output, &failure) == 0) {
            return result<selection_snapshot<Attr, Data>>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_selector_snapshot, verdandi_selector_snapshot_release> handle(output);
        selection_snapshot<Attr, Data> value;
        value.generation = verdandi_selector_snapshot_generation(output);
        value.synchronized = verdandi_selector_snapshot_is_synchronized(output) != 0;

        const std::size_t active_size = verdandi_selector_snapshot_size(output, 0);
        value.candidates.reserve(active_size);
        for (std::size_t index = 0; index < active_size; ++index) {
            result<candidate<Attr, Data>> decoded = snapshot_candidate(output, false, index);
            if (!decoded) {
                return result<selection_snapshot<Attr, Data>>(decoded.failure());
            }
            value.candidates.push_back(std::move(*decoded));
        }

        const std::size_t retained_size = verdandi_selector_snapshot_size(output, 1);
        value.retained.reserve(retained_size);
        for (std::size_t index = 0; index < retained_size; ++index) {
            result<candidate<Attr, Data>> decoded = snapshot_candidate(output, true, index);
            if (!decoded) {
                return result<selection_snapshot<Attr, Data>>(decoded.failure());
            }
            value.retained.push_back(retained_candidate<Attr, Data>(std::move(*decoded), verdandi_selector_snapshot_retained_until(output, index)));
        }
        return result<selection_snapshot<Attr, Data>>(std::move(value));
    }

    /// 非阻塞取得一条同步、恢复或协议诊断；队列为空时 `available` 为假。
    result<diagnostic> try_error() {
        if (!valid()) {
            return result<diagnostic>(error("invalid", "selector"));
        }
        int available = 0;
        verdandi_error failure = {};
        const int succeeded = verdandi_selector_try_error(handle_.get(), &available, &failure);
        return detail::diagnostic_result(succeeded, available, failure);
    }

    /// 关闭常驻监听和当前临时同步任务；核心保证幂等。
    result<void> close() {
        if (!valid()) {
            return result<void>(error("invalid", "selector"));
        }
        verdandi_error failure = {};
        const int succeeded = verdandi_selector_close(handle_.get(), &failure);
        return detail::status(succeeded, failure);
    }

private:
    template <class Policy>
    struct one_policy_context {
        Policy* value;
    };

    template <class Policy>
    struct any_policy_context {
        Policy* value;
    };

    template <class Policy>
    static int VERDANDI_C_CALL one_policy(void* raw_context, verdandi_candidates* native_candidates, verdandi_selection* selection, verdandi_error* failure) {
        one_policy_context<Policy>* context = static_cast<one_policy_context<Policy>*>(raw_context);
        try {
            candidates<Attr, Data> values(native_candidates);
            result<optional<choice>> selected = (*context->value)(values);
            if (!selected) {
                detail::write_native_error(failure, selected.failure());
                return 0;
            }
            if (*selected) {
                if (verdandi_selection_add(selection, (*selected)->index, failure) == 0) {
                    return 0;
                }
            }
            return 1;
        } catch (const std::bad_alloc& exception) {
            detail::write_native_error(failure, "capacity", "callback", exception.what());
        } catch (const std::exception& exception) {
            detail::write_native_error(failure, "unavailable", "callback", exception.what());
        } catch (...) {
            detail::write_native_error(failure, "corrupt", "callback", "");
        }
        return 0;
    }

    template <class Policy>
    static int VERDANDI_C_CALL any_policy(void* raw_context, verdandi_candidates* native_candidates, verdandi_selection* selection, verdandi_error* failure) {
        any_policy_context<Policy>* context = static_cast<any_policy_context<Policy>*>(raw_context);
        try {
            candidates<Attr, Data> values(native_candidates);
            result<std::vector<choice>> selected = (*context->value)(values);
            if (!selected) {
                detail::write_native_error(failure, selected.failure());
                return 0;
            }
            for (typename std::vector<choice>::const_iterator iterator = selected->begin(); iterator != selected->end(); ++iterator) {
                if (verdandi_selection_add(selection, iterator->index, failure) == 0) {
                    return 0;
                }
            }
            return 1;
        } catch (const std::bad_alloc& exception) {
            detail::write_native_error(failure, "capacity", "callback", exception.what());
        } catch (const std::exception& exception) {
            detail::write_native_error(failure, "unavailable", "callback", exception.what());
        } catch (...) {
            detail::write_native_error(failure, "corrupt", "callback", "");
        }
        return 0;
    }

    static result<fields> list_fields(const verdandi_candidate_list* value, std::size_t index, bool read_attr) {
        detail::field_collector collector;
        verdandi_error failure = {};
        const int succeeded = read_attr ? verdandi_candidate_list_visit_attr(value, index, detail::collect_field, &collector, &failure)
                                        : verdandi_candidate_list_visit_data(value, index, detail::collect_field, &collector, &failure);
        return detail::collected_fields(succeeded, failure, collector);
    }

    static result<candidate<Attr, Data>> list_candidate(const verdandi_candidate_list* value, std::size_t index) {
        verdandi_registration_metadata native_metadata = {};
        if (value == NULL || verdandi_candidate_list_metadata(value, index, &native_metadata) == 0) {
            return result<candidate<Attr, Data>>(error("invalid", "candidate"));
        }
        result<fields> attr_fields = list_fields(value, index, true);
        if (!attr_fields) {
            return result<candidate<Attr, Data>>(attr_fields.failure());
        }
        result<fields> data_fields = list_fields(value, index, false);
        if (!data_fields) {
            return result<candidate<Attr, Data>>(data_fields.failure());
        }
        return decoded_candidate(native_metadata, *attr_fields, *data_fields);
    }

    static result<fields> snapshot_fields(const verdandi_selector_snapshot* value, bool retained, std::size_t index, bool read_attr) {
        detail::field_collector collector;
        verdandi_error failure = {};
        const int retained_value = retained ? 1 : 0;
        const int succeeded = read_attr ? verdandi_selector_snapshot_visit_attr(value, retained_value, index, detail::collect_field, &collector, &failure)
                                        : verdandi_selector_snapshot_visit_data(value, retained_value, index, detail::collect_field, &collector, &failure);
        return detail::collected_fields(succeeded, failure, collector);
    }

    static result<candidate<Attr, Data>> snapshot_candidate(const verdandi_selector_snapshot* value, bool retained, std::size_t index) {
        verdandi_registration_metadata native_metadata = {};
        if (value == NULL || verdandi_selector_snapshot_metadata(value, retained ? 1 : 0, index, &native_metadata) == 0) {
            return result<candidate<Attr, Data>>(error("invalid", "candidate"));
        }
        result<fields> attr_fields = snapshot_fields(value, retained, index, true);
        if (!attr_fields) {
            return result<candidate<Attr, Data>>(attr_fields.failure());
        }
        result<fields> data_fields = snapshot_fields(value, retained, index, false);
        if (!data_fields) {
            return result<candidate<Attr, Data>>(data_fields.failure());
        }
        return decoded_candidate(native_metadata, *attr_fields, *data_fields);
    }

    static result<candidate<Attr, Data>> decoded_candidate(const verdandi_registration_metadata& native_metadata, const fields& attr_fields,
                                                           const fields& data_fields) {
        result<Attr> attr_value = detail::decode_value<Attr>(attr_fields);
        if (!attr_value) {
            return result<candidate<Attr, Data>>(attr_value.failure());
        }
        result<Data> data_value = detail::decode_value<Data>(data_fields);
        if (!data_value) {
            return result<candidate<Attr, Data>>(data_value.failure());
        }
        return result<candidate<Attr, Data>>(candidate<Attr, Data>(detail::metadata(native_metadata), std::move(*attr_value), std::move(*data_value)));
    }

    selector(std::shared_ptr<detail::registration_state> owner, detail::owned_handle<verdandi_selector, verdandi_selector_release>&& handle)
        : owner_(std::move(owner)), handle_(std::move(handle)) {}

    std::shared_ptr<detail::registration_state> owner_;
    detail::owned_handle<verdandi_selector, verdandi_selector_release> handle_;
};

} // namespace legacy
} // namespace verdandi
