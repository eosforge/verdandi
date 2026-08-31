#pragma once

#include "verdandi/c/types.h"

#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace verdandi {
namespace legacy {

/// C++11 包装层拥有的稳定错误；字符串内容来自 C ABI，不暴露原生 C++23 类型。
class error {
public:
    /// 构造一条空诊断；只用于表示异步诊断当前没有值。
    error() : revision_(0), has_revision_(false) {}

    /// 拥有型保存稳定错误码、字段和说明；不附带权威 revision。
    explicit error(std::string code, std::string field = std::string(), std::string detail = std::string())
        : code_(std::move(code)), field_(std::move(field)), detail_(std::move(detail)), revision_(0), has_revision_(false) {}

    /// 拥有型保存稳定错误内容和 Redis 返回的权威 revision。
    error(std::string code, std::string field, std::string detail, std::uint64_t revision)
        : code_(std::move(code)), field_(std::move(field)), detail_(std::move(detail)), revision_(revision), has_revision_(true) {}

    /// 从一次 C ABI 调用拥有型复制错误，调用结束后不再借用源结构。
    static error from_native(const verdandi_error& value) {
        error output(value.code, value.field, value.detail);
        if (value.has_revision != 0U) {
            output.revision_ = value.revision;
            output.has_revision_ = true;
        }
        return output;
    }

    /// 返回用于程序分支的稳定小写错误类别。
    const std::string& code() const {
        return code_;
    }
    /// 返回导致失败的配置、字段或参数名称；没有上下文时为空。
    const std::string& field() const {
        return field_;
    }
    /// 返回仅供诊断的拥有型说明；调用方不得依赖其文本做逻辑判断。
    const std::string& detail() const {
        return detail_;
    }
    /// 返回当前错误是否携带 Redis 的权威 revision。
    bool has_revision() const {
        return has_revision_;
    }
    /// 返回权威 revision；调用前必须先确认 `has_revision()`。
    std::uint64_t revision() const {
        return revision_;
    }
    /// 返回错误码是否为空；仅用于内部表达“没有异步诊断”。
    bool empty() const {
        return code_.empty();
    }

private:
    std::string code_;
    std::string field_;
    std::string detail_;
    std::uint64_t revision_;
    bool has_revision_;
};

/// C++11 版本的单值可选容器；避免为了表达空选择进行堆分配。
template <class T>
class optional {
public:
    /// 构造不含值的 Optional，不执行堆分配。
    optional() : present_(false) {}

    /// 在内部存储中复制构造一个值。
    optional(const T& value) : present_(true) {
        new (&storage_.value) T(value);
    }

    /// 在内部存储中移动构造一个值。
    optional(T&& value) : present_(true) {
        new (&storage_.value) T(std::move(value));
    }

    /// 复制另一个 Optional 的存在状态和值。
    optional(const optional& other) : present_(other.present_) {
        if (present_) {
            new (&storage_.value) T(other.storage_.value);
        }
    }

    /// 移动另一个 Optional 的值；源对象仍保持一个合法的已移动状态。
    optional(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) : present_(false) {
        if (other.present_) {
            new (&storage_.value) T(std::move(other.storage_.value));
            present_ = true;
        }
    }

    /// 只在值存在时销毁内部对象。
    ~optional() {
        reset();
    }

    /// 用另一个 Optional 的副本替换当前状态；构造失败时当前对象变为空。
    optional& operator=(const optional& other) {
        if (this != &other) {
            reset();
            if (other.present_) {
                new (&storage_.value) T(other.storage_.value);
                present_ = true;
            }
        }
        return *this;
    }

    /// 用另一个 Optional 的移动值替换当前状态；构造失败时当前对象变为空。
    optional& operator=(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (this != &other) {
            reset();
            if (other.present_) {
                new (&storage_.value) T(std::move(other.storage_.value));
                present_ = true;
            }
        }
        return *this;
    }

    /// 返回当前是否拥有一个可访问值。
    explicit operator bool() const {
        return present_;
    }
    /// 返回当前是否拥有一个可访问值。
    bool has_value() const {
        return present_;
    }
    /// 返回可修改值；值不存在时调用违反调用方契约。
    T& value() {
        return storage_.value;
    }
    /// 返回只读值；值不存在时调用违反调用方契约。
    const T& value() const {
        return storage_.value;
    }
    /// 返回可修改值；值不存在时调用违反调用方契约。
    T& operator*() {
        return storage_.value;
    }
    /// 返回只读值；值不存在时调用违反调用方契约。
    const T& operator*() const {
        return storage_.value;
    }
    /// 返回可修改值地址；值不存在时调用违反调用方契约。
    T* operator->() {
        return &storage_.value;
    }
    /// 返回只读值地址；值不存在时调用违反调用方契约。
    const T* operator->() const {
        return &storage_.value;
    }

    /// 销毁当前值并切换为空状态；重复调用安全。
    void reset() {
        if (present_) {
            storage_.value.~T();
            present_ = false;
        }
    }

    /// 销毁旧值后用 `value` 构造新值；构造失败时保持为空。
    template <class U>
    void emplace(U&& value) {
        reset();
        new (&storage_.value) T(std::forward<U>(value));
        present_ = true;
    }

private:
    union storage {
        storage() {}
        ~storage() {}
        T value;
    } storage_;
    bool present_;
};

/// C++11 版本的 expected 风格结果；成功值和错误原地存储，不引入结果堆分配。
template <class T>
class result {
public:
    /// 复制构造一个成功值。
    result(const T& value) : succeeded_(true) {
        new (&storage_.value) T(value);
    }

    /// 移动构造一个成功值。
    result(T&& value) : succeeded_(true) {
        new (&storage_.value) T(std::move(value));
    }

    /// 复制构造一个失败值。
    result(const legacy::error& failure) : succeeded_(false) {
        new (&storage_.failure) legacy::error(failure);
    }

    /// 移动构造一个失败值。
    result(legacy::error&& failure) : succeeded_(false) {
        new (&storage_.failure) legacy::error(std::move(failure));
    }

    /// 复制另一个 Result 当前有效的分支。
    result(const result& other) : succeeded_(other.succeeded_) {
        if (succeeded_) {
            new (&storage_.value) T(other.storage_.value);
        } else {
            new (&storage_.failure) legacy::error(other.storage_.failure);
        }
    }

    /// 移动另一个 Result 当前有效的分支。
    result(result&& other) noexcept(std::is_nothrow_move_constructible<T>::value && std::is_nothrow_move_constructible<legacy::error>::value)
        : succeeded_(other.succeeded_) {
        if (succeeded_) {
            new (&storage_.value) T(std::move(other.storage_.value));
        } else {
            new (&storage_.failure) legacy::error(std::move(other.storage_.failure));
        }
    }

    /// 销毁当前有效的成功或失败分支。
    ~result() {
        destroy();
    }

    /// C++11 无法为任意可抛移动类型安全切换 Union 分支，因此 Result 只构造、不赋值。
    result& operator=(const result& other) = delete;
    /// C++11 无法为任意可抛移动类型安全切换 Union 分支，因此 Result 只构造、不赋值。
    result& operator=(result&& other) = delete;

    /// 返回当前是否保存成功值。
    explicit operator bool() const {
        return succeeded_;
    }
    /// 返回当前是否保存成功值。
    bool has_value() const {
        return succeeded_;
    }
    /// 返回可修改成功值；失败状态调用违反调用方契约。
    T& value() {
        return storage_.value;
    }
    /// 返回只读成功值；失败状态调用违反调用方契约。
    const T& value() const {
        return storage_.value;
    }
    /// 返回可修改成功值；失败状态调用违反调用方契约。
    T& operator*() {
        return storage_.value;
    }
    /// 返回只读成功值；失败状态调用违反调用方契约。
    const T& operator*() const {
        return storage_.value;
    }
    /// 返回可修改成功值地址；失败状态调用违反调用方契约。
    T* operator->() {
        return &storage_.value;
    }
    /// 返回只读成功值地址；失败状态调用违反调用方契约。
    const T* operator->() const {
        return &storage_.value;
    }
    /// 返回可修改错误；成功状态调用违反调用方契约。
    legacy::error& failure() {
        return storage_.failure;
    }
    /// 返回只读错误；成功状态调用违反调用方契约。
    const legacy::error& failure() const {
        return storage_.failure;
    }

private:
    void destroy() {
        if (succeeded_) {
            storage_.value.~T();
        } else {
            storage_.failure.~error();
        }
    }

    union storage {
        storage() {}
        ~storage() {}
        T value;
        legacy::error failure;
    } storage_;
    bool succeeded_;
};

/// 没有成功载荷的 C++11 expected 风格结果。
template <>
class result<void> {
public:
    /// 构造成功结果；成功路径只保留一个空指针。
    result() {}
    /// 在失败路径堆上复制错误，使成功对象保持最小尺寸。
    result(const legacy::error& failure) : failure_(new legacy::error(failure)) {}
    /// 在失败路径堆上移动错误，使成功对象保持最小尺寸。
    result(legacy::error&& failure) : failure_(new legacy::error(std::move(failure))) {}

    /// 深复制可选失败值。
    result(const result& other) : failure_(other.failure_ ? new legacy::error(*other.failure_) : NULL) {}
    /// 移动可选失败所有权，不执行分配。
    result(result&&) noexcept = default;

    /// 先完整复制新状态再交换，复制失败时保留旧状态。
    result& operator=(const result& other) {
        if (this != &other) {
            result copy(other);
            failure_.swap(copy.failure_);
        }
        return *this;
    }

    /// 移动可选失败所有权，不执行分配。
    result& operator=(result&&) noexcept = default;

    /// 返回本次无载荷操作是否成功。
    explicit operator bool() const {
        return !failure_;
    }
    /// 返回本次无载荷操作是否成功。
    bool has_value() const {
        return !failure_;
    }
    /// 返回可修改错误；成功状态调用违反调用方契约。
    legacy::error& failure() {
        return *failure_;
    }
    /// 返回只读错误；成功状态调用违反调用方契约。
    const legacy::error& failure() const {
        return *failure_;
    }

private:
    std::unique_ptr<legacy::error> failure_;
};

/// 非阻塞异步诊断读取结果；available 为假时 value 为空错误。
struct diagnostic {
    /// 为真时 `value` 保存一条已取出的异步诊断。
    bool available;
    /// 拥有型异步诊断；`available` 为假时内容为空。
    legacy::error value;

    /// 构造没有可用诊断的结果。
    diagnostic() : available(false) {}
};

namespace detail {

/// 把 C ABI 的非零成功约定转换为无载荷 Result。
inline result<void> status(const int succeeded, const verdandi_error& failure) {
    if (succeeded != 0) {
        return result<void>();
    }
    return result<void>(legacy::error::from_native(failure));
}

/// 区分调用失败与“调用成功但诊断队列为空”两种状态。
inline result<diagnostic> diagnostic_result(const int succeeded, const int available, const verdandi_error& value) {
    if (succeeded == 0) {
        return result<diagnostic>(legacy::error::from_native(value));
    }
    diagnostic output;
    output.available = available != 0;
    if (output.available) {
        output.value = legacy::error::from_native(value);
    }
    return result<diagnostic>(std::move(output));
}

} // namespace detail
} // namespace legacy
} // namespace verdandi
