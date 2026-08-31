#pragma once

#include "verdandi/c/client.h"
#include "verdandi/legacy/fields.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace verdandi {
namespace legacy {

namespace detail {

/// 只调用 C ABI 配对释放函数的移动句柄；不承载任何协议或重试状态。
template <class T, void(VERDANDI_C_CALL* Release)(T*)>
class owned_handle {
public:
    /// 构造空句柄。
    owned_handle() : value_(NULL) {}
    /// 接管 `value`；允许为空且不复制底层对象。
    explicit owned_handle(T* value) : value_(value) {}
    /// 通过模板参数指定的 C 释放函数回收当前句柄。
    ~owned_handle() {
        reset();
    }

    /// 转移句柄所有权并清空源对象。
    owned_handle(owned_handle&& other) noexcept : value_(other.release()) {}

    /// 释放旧句柄后转移新所有权；自移动不改变状态。
    owned_handle& operator=(owned_handle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    owned_handle(const owned_handle&) = delete;
    owned_handle& operator=(const owned_handle&) = delete;

    /// 返回借用的底层地址；所有权仍属于本对象。
    T* get() const {
        return value_;
    }
    /// 返回当前是否拥有非空句柄。
    explicit operator bool() const {
        return value_ != NULL;
    }

    /// 交出底层所有权并把本对象置空。
    T* release() {
        T* output = value_;
        value_ = NULL;
        return output;
    }

    /// 释放旧句柄并接管 `value`；允许用空值实现幂等清理。
    void reset(T* value = NULL) {
        if (value_ != NULL) {
            Release(value_);
        }
        value_ = value;
    }

private:
    T* value_;
};

struct root_state {
    /// 接管根 C Client，并由最后一个共享状态持有者释放。
    explicit root_state(verdandi_client* value) : handle(value) {}
    /// 根 C Client 的唯一释放所有权。
    owned_handle<verdandi_client, verdandi_client_release> handle;
};

/// 把正毫秒时长转为 C ABI 无符号值；零或负数返回 `invalid`。
inline result<std::uint64_t> native_duration(const std::chrono::milliseconds value, const char* field) {
    if (value.count() <= 0) {
        return result<std::uint64_t>(error("invalid", field));
    }
    return result<std::uint64_t>(static_cast<std::uint64_t>(value.count()));
}

/// 接管并遍历一个 C Field Set；成功时返回拥有型 Fields，所有路径均释放句柄。
inline result<fields> take_field_set(verdandi_field_set* value) {
    owned_handle<verdandi_field_set, verdandi_field_set_release> handle(value);
    if (value == NULL) {
        return result<fields>(error("invalid", "fields"));
    }
    field_collector collector;
    verdandi_error failure = {};
    const int succeeded = verdandi_field_set_visit(value, collect_field, &collector, &failure);
    return collected_fields(succeeded, failure, collector);
}

} // namespace detail

/// C++11 根 Client；副本共享一个 C ABI 根句柄，子域可安全延长其生命周期。
class client {
public:
    /// 构造未连接的空 Client；只适合稍后移动赋值或表达无效状态。
    client() {}

    /// 从规范严格 v1 JSON 构造共享 Redis 传输。
    static result<client> open(const std::string& json) {
        verdandi_client* output = NULL;
        verdandi_error failure = {};
        verdandi_bytes_view source = {json.empty() ? NULL : reinterpret_cast<const std::uint8_t*>(json.data()), json.size()};
        if (verdandi_client_open_json(source, &output, &failure) == 0) {
            return result<client>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_client, verdandi_client_release> guard(output);
        client value;
        value.state_ = std::make_shared<detail::root_state>(output);
        guard.release();
        return result<client>(std::move(value));
    }

    /// 返回当前是否持有根 C Client。
    bool valid() const {
        return state_ && state_->handle;
    }
    /// 返回根传输是否仍接纳工作；无效 Client 返回假。
    bool is_open() const {
        return valid() && verdandi_client_is_open(state_->handle.get()) != 0;
    }

    /// 在普通命令预算内执行 Redis PING；无效或关闭的 Client 返回稳定错误。
    result<void> ping() const {
        if (!valid()) {
            return result<void>(error("invalid", "client"));
        }
        verdandi_error failure = {};
        const int succeeded = verdandi_client_ping(state_->handle.get(), &failure);
        return detail::status(succeeded, failure);
    }

    /// 终止根传输；由子域持有的根状态仍保持内存有效，但不再接纳工作。
    result<void> close() const {
        if (!valid()) {
            return result<void>(error("invalid", "client"));
        }
        verdandi_error failure = {};
        const int succeeded = verdandi_client_close(state_->handle.get(), &failure);
        return detail::status(succeeded, failure);
    }

    /// 读取完整 String Key；不存在返回空 Optional，结果字节由调用方拥有。
    result<optional<bytes>> key_load(const std::string& key) const {
        if (!valid()) {
            return result<optional<bytes>>(error("invalid", "client"));
        }
        int found = 0;
        verdandi_blob* output = NULL;
        verdandi_error failure = {};
        if (verdandi_key_load(state_->handle.get(), detail::native_text(key), &found, &output, &failure) == 0) {
            return result<optional<bytes>>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_blob, verdandi_blob_release> handle(output);
        if (found == 0) {
            return result<optional<bytes>>(optional<bytes>());
        }
        const verdandi_bytes_view view = verdandi_blob_view(output);
        bytes value = view.size == 0U ? bytes() : bytes(view.data, view.data + view.size);
        return result<optional<bytes>>(optional<bytes>(std::move(value)));
    }

    /// 无 TTL 覆盖写入完整 String Key；Redis ACL 和根命令预算仍由核心处理。
    result<void> key_store(const std::string& key, const bytes& value) const {
        if (!valid()) {
            return result<void>(error("invalid", "client"));
        }
        verdandi_error failure = {};
        return detail::status(verdandi_key_store(state_->handle.get(), detail::native_text(key), detail::native_bytes(value), &failure), failure);
    }

    /// 带正毫秒 TTL 覆盖写入完整 String Key；非法时长在 Redis I/O 前失败。
    result<void> key_store(const std::string& key, const bytes& value, std::chrono::milliseconds ttl) const {
        if (!valid()) {
            return result<void>(error("invalid", "client"));
        }
        result<std::uint64_t> ttl_ms = detail::native_duration(ttl, "ttl");
        if (!ttl_ms) {
            return result<void>(ttl_ms.failure());
        }
        verdandi_error failure = {};
        return detail::status(verdandi_key_store_ttl(state_->handle.get(), detail::native_text(key), detail::native_bytes(value), *ttl_ms, &failure), failure);
    }

    /// 删除完整 Key，并返回它在本次调用前是否存在。
    result<bool> key_erase(const std::string& key) const {
        if (!valid()) {
            return result<bool>(error("invalid", "client"));
        }
        int removed = 0;
        verdandi_error failure = {};
        if (verdandi_key_erase(state_->handle.get(), detail::native_text(key), &removed, &failure) == 0) {
            return result<bool>(error::from_native(failure));
        }
        return result<bool>(removed != 0);
    }

    /// 查询 Key 是否存在，不读取其内容。
    result<bool> key_contains(const std::string& key) const {
        if (!valid()) {
            return result<bool>(error("invalid", "client"));
        }
        int present = 0;
        verdandi_error failure = {};
        if (verdandi_key_contains(state_->handle.get(), detail::native_text(key), &present, &failure) == 0) {
            return result<bool>(error::from_native(failure));
        }
        return result<bool>(present != 0);
    }

    /// 为已有 Key 设置正毫秒 TTL；返回是否实际找到并修改该 Key。
    result<bool> key_expire(const std::string& key, std::chrono::milliseconds ttl) const {
        if (!valid()) {
            return result<bool>(error("invalid", "client"));
        }
        result<std::uint64_t> ttl_ms = detail::native_duration(ttl, "ttl");
        if (!ttl_ms) {
            return result<bool>(ttl_ms.failure());
        }
        int changed = 0;
        verdandi_error failure = {};
        if (verdandi_key_expire(state_->handle.get(), detail::native_text(key), *ttl_ms, &changed, &failure) == 0) {
            return result<bool>(error::from_native(failure));
        }
        return result<bool>(changed != 0);
    }

    /// 读取完整 Redis Hash；字段和值均复制进拥有型 Fields。
    result<fields> hash_load(const std::string& key) const {
        if (!valid()) {
            return result<fields>(error("invalid", "client"));
        }
        verdandi_field_set* output = NULL;
        verdandi_error failure = {};
        if (verdandi_hash_load(state_->handle.get(), detail::native_text(key), &output, &failure) == 0) {
            return result<fields>(error::from_native(failure));
        }
        return detail::take_field_set(output);
    }

    /// 用一次 HSET 语义写入给定字段；不删除 Hash 中未出现的其他字段。
    result<void> hash_store(const std::string& key, const fields& value) const {
        if (!valid()) {
            return result<void>(error("invalid", "client"));
        }
        detail::native_fields encoded(value);
        verdandi_error failure = {};
        return detail::status(verdandi_hash_store(state_->handle.get(), detail::native_text(key), encoded.view(), &failure), failure);
    }

    /// 删除 `names` 中列出的 Hash 字段并返回实际删除数量；空集合是合法空操作。
    result<std::size_t> hash_erase(const std::string& key, const std::vector<std::string>& names) const {
        if (!valid()) {
            return result<std::size_t>(error("invalid", "client"));
        }
        std::vector<verdandi_string_view> views;
        views.reserve(names.size());
        for (std::vector<std::string>::const_iterator iterator = names.begin(); iterator != names.end(); ++iterator) {
            views.push_back(detail::native_text(*iterator));
        }
        std::size_t removed = 0;
        verdandi_error failure = {};
        if (verdandi_hash_erase(state_->handle.get(), detail::native_text(key), views.empty() ? NULL : &views[0], views.size(), &removed, &failure) == 0) {
            return result<std::size_t>(error::from_native(failure));
        }
        return result<std::size_t>(removed);
    }

    /// 查询一个 Hash 字段是否存在，不读取其内容。
    result<bool> hash_contains(const std::string& key, const std::string& name) const {
        if (!valid()) {
            return result<bool>(error("invalid", "client"));
        }
        int present = 0;
        verdandi_error failure = {};
        if (verdandi_hash_contains(state_->handle.get(), detail::native_text(key), detail::native_text(name), &present, &failure) == 0) {
            return result<bool>(error::from_native(failure));
        }
        return result<bool>(present != 0);
    }

    /// 返回 Hash 当前字段数量；Key 不存在时由 Redis 语义返回零。
    result<std::size_t> hash_size(const std::string& key) const {
        if (!valid()) {
            return result<std::size_t>(error("invalid", "client"));
        }
        std::size_t size = 0;
        verdandi_error failure = {};
        if (verdandi_hash_size(state_->handle.get(), detail::native_text(key), &size, &failure) == 0) {
            return result<std::size_t>(error::from_native(failure));
        }
        return result<std::size_t>(size);
    }

private:
    explicit client(std::shared_ptr<detail::root_state> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::root_state> state_;

    friend class registration_client;
    friend class catalog_client;
};

} // namespace legacy
} // namespace verdandi
