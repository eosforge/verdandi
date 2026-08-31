#pragma once

#include "verdandi/c/catalog.h"
#include "verdandi/legacy/client.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace verdandi {
namespace legacy {

/// Catalog 完整值的稳定结构类别；Redis 仍只保存扁平二进制 Fields。
enum class catalog_kind : std::uint8_t {
    /// 一个语义上的单值；字段布局由应用 Codec 定义。
    value,
    /// 一个连续数组；字段名使用协议规定的数组索引。
    array,
    /// 一个按字段名组织的 Map。
    map
};

struct catalog_path {
    /// Catalog 的一级分区名称。
    std::string part;
    /// Part 内的完整记录身份。
    std::string id;

    /// 构造尚未填写的空 Path。
    catalog_path() {}
    /// 接管 Part 和 ID 文本；具体协议规则由核心在调用时验证。
    catalog_path(std::string part_value, std::string id_value) : part(std::move(part_value)), id(std::move(id_value)) {}
};

struct catalog_subscription {
    /// 为真时覆盖当前 Catalog Zone 的全部 Path。
    bool zone;
    /// 订阅这些完整 Part 下的所有 Path。
    std::vector<std::string> parts;
    /// 订阅这些精确 Path。
    std::vector<catalog_path> paths;

    /// 构造不覆盖全 Zone 且没有 Part/Path 的空订阅。
    catalog_subscription() : zone(false) {}
};

template <class T>
struct catalog_snapshot {
    /// Entry 当前不可变状态的完整 revision。
    std::uint64_t revision;
    /// 稳定小写状态，如 `present`、`deleted` 或 `unavailable`。
    std::string status;
    /// 当前状态是否完成权威同步。
    bool synchronized;
    /// 状态存在完整值时的拥有型解码结果。
    optional<T> value;

    /// 构造 revision 为零且尚未同步的空快照。
    catalog_snapshot() : revision(0), synchronized(false) {}
};

namespace detail {

struct catalog_state {
    /// 同时持有根状态与 Catalog 域句柄，保证释放顺序正确。
    catalog_state(std::shared_ptr<root_state> root_value, verdandi_catalog_client* value) : root(std::move(root_value)), handle(value) {}

    /// 延长根 Client 生命周期的共享所有权。
    std::shared_ptr<root_state> root;
    /// Catalog 域 C 句柄的唯一释放所有权。
    owned_handle<verdandi_catalog_client, verdandi_catalog_client_release> handle;
};

struct subscriber_state {
    /// 同时持有 Catalog 域与 Subscriber 句柄，供稳定 Entry 共享。
    subscriber_state(std::shared_ptr<catalog_state> owner_value, verdandi_catalog_subscriber* value) : owner(std::move(owner_value)), handle(value) {}

    /// 延长 Catalog 域生命周期的共享所有权。
    std::shared_ptr<catalog_state> owner;
    /// Subscriber C 句柄的唯一释放所有权。
    owned_handle<verdandi_catalog_subscriber, verdandi_catalog_subscriber_release> handle;
};

/// 构造一次 C ABI 调用期间借用 Part/ID 的 Path 视图。
inline verdandi_catalog_path_view native_path(const catalog_path& value) {
    verdandi_catalog_path_view output = {native_text(value.part), native_text(value.id)};
    return output;
}

/// 把类型安全 Kind 转为静态 C ABI 字符串视图；损坏枚举返回空值并由核心拒绝。
inline verdandi_string_view native_kind(catalog_kind value) {
    switch (value) {
    case catalog_kind::value:
        return verdandi_string_view{"value", 5U};
    case catalog_kind::array:
        return verdandi_string_view{"array", 5U};
    case catalog_kind::map:
        return verdandi_string_view{"map", 3U};
    }
    return verdandi_string_view{NULL, 0U};
}

} // namespace detail

/// Catalog 子域；副本共享域句柄并保持根传输存活。
class catalog_client {
public:
    /// 构造未打开的空 Catalog 领域 Client。
    catalog_client() {}

    /// 从根 JSON 中的 Catalog 配置打开领域；缺少配置时返回 `missing`。
    static result<catalog_client> open(const client& root) {
        if (!root.valid()) {
            return result<catalog_client>(error("invalid", "client"));
        }
        verdandi_catalog_client* output = NULL;
        verdandi_error failure = {};
        if (verdandi_catalog_client_open(root.state_->handle.get(), &output, &failure) == 0) {
            return result<catalog_client>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_catalog_client, verdandi_catalog_client_release> guard(output);
        catalog_client value;
        value.state_ = std::make_shared<detail::catalog_state>(root.state_, output);
        guard.release();
        return result<catalog_client>(std::move(value));
    }

    /// 返回当前是否持有 Catalog 域句柄。
    bool valid() const {
        return state_ && state_->handle;
    }
    /// 返回领域是否仍接纳 Publisher 和 Subscriber。
    bool is_open() const {
        return valid() && verdandi_catalog_client_is_open(state_->handle.get()) != 0;
    }

    /// 关闭 Subscriber 和检查点资源但不关闭根传输；核心保证幂等。
    result<void> close() const {
        if (!valid()) {
            return result<void>(error("invalid", "catalog"));
        }
        verdandi_error failure = {};
        const int succeeded = verdandi_catalog_client_close(state_->handle.get(), &failure);
        return detail::status(succeeded, failure);
    }

private:
    std::shared_ptr<detail::catalog_state> state_;

    friend class catalog_publisher;
    friend class catalog_subscriber;
};

/// 无后台任务的强类型 Catalog Publisher。
class catalog_publisher {
public:
    /// 构造未绑定领域的空 Publisher。
    catalog_publisher() {}
    /// 转移无任务 Publisher 的领域和 C 句柄所有权。
    catalog_publisher(catalog_publisher&&) noexcept = default;
    /// 释放旧句柄后转移 Publisher 所有权。
    catalog_publisher& operator=(catalog_publisher&&) noexcept = default;
    catalog_publisher(const catalog_publisher&) = delete;
    catalog_publisher& operator=(const catalog_publisher&) = delete;

    /// 创建一个无后台任务的 Publisher，并延长 Catalog 域生命周期。
    static result<catalog_publisher> create(const catalog_client& owner) {
        if (!owner.valid()) {
            return result<catalog_publisher>(error("invalid", "catalog"));
        }
        verdandi_catalog_publisher* output = NULL;
        verdandi_error failure = {};
        if (verdandi_catalog_publisher_create(owner.state_->handle.get(), &output, &failure) == 0) {
            return result<catalog_publisher>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_catalog_publisher, verdandi_catalog_publisher_release> handle(output);
        catalog_publisher value(owner.state_, std::move(handle));
        return result<catalog_publisher>(std::move(value));
    }

    /// 返回当前是否同时持有领域与 Publisher 句柄。
    bool valid() const {
        return owner_ && handle_;
    }

    /// 原子覆盖完整 Value/Array/Map，并返回 Redis 分配的新全局 revision。
    template <class T>
    result<std::uint64_t> replace(const catalog_path& path, catalog_kind kind, const T& value) {
        if (!valid()) {
            return result<std::uint64_t>(error("invalid", "publisher"));
        }
        result<fields> encoded = detail::encode_value(value);
        if (!encoded) {
            return result<std::uint64_t>(encoded.failure());
        }
        detail::native_fields native(*encoded);
        std::uint64_t revision = 0;
        verdandi_error failure = {};
        if (verdandi_catalog_replace(handle_.get(), detail::native_path(path), detail::native_kind(kind), native.view(), &revision, &failure) == 0) {
            return result<std::uint64_t>(error::from_native(failure));
        }
        return result<std::uint64_t>(revision);
    }

    /// 在 `base_revision` 精确匹配时原子覆盖编码字段，并返回新 revision。
    /// Schema 类型会编码全部声明成员；真正的局部 Patch 应传入只含目标字段的 `fields`。
    template <class T>
    result<std::uint64_t> patch(const catalog_path& path, std::uint64_t base_revision, const T& value) {
        if (!valid()) {
            return result<std::uint64_t>(error("invalid", "publisher"));
        }
        result<fields> encoded = detail::encode_value(value);
        if (!encoded) {
            return result<std::uint64_t>(encoded.failure());
        }
        detail::native_fields native(*encoded);
        std::uint64_t revision = 0;
        verdandi_error failure = {};
        if (verdandi_catalog_patch(handle_.get(), detail::native_path(path), base_revision, native.view(), &revision, &failure) == 0) {
            return result<std::uint64_t>(error::from_native(failure));
        }
        return result<std::uint64_t>(revision);
    }

    /// 原子删除完整 Path、创建 tombstone，并返回新全局 revision。
    result<std::uint64_t> erase(const catalog_path& path) {
        if (!valid()) {
            return result<std::uint64_t>(error("invalid", "publisher"));
        }
        std::uint64_t revision = 0;
        verdandi_error failure = {};
        if (verdandi_catalog_erase(handle_.get(), detail::native_path(path), &revision, &failure) == 0) {
            return result<std::uint64_t>(error::from_native(failure));
        }
        return result<std::uint64_t>(revision);
    }

private:
    catalog_publisher(std::shared_ptr<detail::catalog_state> owner,
                      detail::owned_handle<verdandi_catalog_publisher, verdandi_catalog_publisher_release>&& handle)
        : owner_(std::move(owner)), handle_(std::move(handle)) {}

    std::shared_ptr<detail::catalog_state> owner_;
    detail::owned_handle<verdandi_catalog_publisher, verdandi_catalog_publisher_release> handle_;
};

class catalog_entry;

/// Catalog Subscriber 的共享拥有状态；Entry 可安全延长 Subscriber 生命周期。
class catalog_subscriber {
public:
    /// 构造未绑定领域和覆盖范围的空 Subscriber。
    catalog_subscriber() {}

    /// 建立订阅并在返回前完成确认、权威同步和同连接栅栏。
    static result<catalog_subscriber> create(const catalog_client& owner, const catalog_subscription& subscription) {
        if (!owner.valid()) {
            return result<catalog_subscriber>(error("invalid", "catalog"));
        }
        std::vector<verdandi_string_view> parts;
        parts.reserve(subscription.parts.size());
        for (std::vector<std::string>::const_iterator iterator = subscription.parts.begin(); iterator != subscription.parts.end(); ++iterator) {
            parts.push_back(detail::native_text(*iterator));
        }
        std::vector<verdandi_catalog_path_view> paths;
        paths.reserve(subscription.paths.size());
        for (std::vector<catalog_path>::const_iterator iterator = subscription.paths.begin(); iterator != subscription.paths.end(); ++iterator) {
            paths.push_back(detail::native_path(*iterator));
        }
        verdandi_catalog_subscription native = {};
        native.zone = subscription.zone ? 1 : 0;
        native.parts = parts.empty() ? NULL : &parts[0];
        native.part_count = parts.size();
        native.paths = paths.empty() ? NULL : &paths[0];
        native.path_count = paths.size();

        verdandi_catalog_subscriber* output = NULL;
        verdandi_error failure = {};
        if (verdandi_catalog_subscriber_create(owner.state_->handle.get(), &native, &output, &failure) == 0) {
            return result<catalog_subscriber>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_catalog_subscriber, verdandi_catalog_subscriber_release> guard(output);
        catalog_subscriber value;
        value.state_ = std::make_shared<detail::subscriber_state>(owner.state_, output);
        guard.release();
        return result<catalog_subscriber>(std::move(value));
    }

    /// 返回当前是否持有 Subscriber 共享状态。
    bool valid() const {
        return state_ && state_->handle;
    }

    /// 查找订阅覆盖范围内的稳定 Entry；未覆盖或不存在返回空 Optional。
    result<optional<catalog_entry>> find(const catalog_path& path) const;

    /// 非阻塞取得一条同步、恢复、检查点或协议诊断。
    result<diagnostic> try_error() const {
        if (!valid()) {
            return result<diagnostic>(error("invalid", "subscriber"));
        }
        int available = 0;
        verdandi_error failure = {};
        const int succeeded = verdandi_catalog_subscriber_try_error(state_->handle.get(), &available, &failure);
        return detail::diagnostic_result(succeeded, available, failure);
    }

    /// 关闭常驻监听和当前临时同步任务；核心保证幂等。
    result<void> close() const {
        if (!valid()) {
            return result<void>(error("invalid", "subscriber"));
        }
        verdandi_error failure = {};
        const int succeeded = verdandi_catalog_subscriber_close(state_->handle.get(), &failure);
        return detail::status(succeeded, failure);
    }

private:
    std::shared_ptr<detail::subscriber_state> state_;
};

/// 在 C++11 包装层中保持 Subscriber 与底层稳定 Entry 同时存活。
class catalog_entry {
public:
    /// 构造未绑定 Subscriber 和 Path 的空 Entry。
    catalog_entry() {}
    /// 转移 Entry、Subscriber 共享状态和 Path 所有权。
    catalog_entry(catalog_entry&&) noexcept = default;
    /// 释放旧句柄后转移 Entry 所有权。
    catalog_entry& operator=(catalog_entry&&) noexcept = default;
    catalog_entry(const catalog_entry&) = delete;
    catalog_entry& operator=(const catalog_entry&) = delete;

    /// 返回当前是否同时持有 Subscriber 与稳定 Entry 句柄。
    bool valid() const {
        return owner_ && handle_;
    }
    /// 返回构造 Entry 时拥有型保存的 Path。
    const catalog_path& path() const {
        return path_;
    }
    /// 返回稳定小写状态；无效 Entry 返回 `closed`。
    std::string status() const {
        return valid() ? std::string(verdandi_catalog_entry_status(handle_.get())) : std::string("closed");
    }
    /// 返回最后已知完整 revision；无效 Entry 返回零。
    std::uint64_t revision() const {
        return valid() ? verdandi_catalog_entry_revision(handle_.get()) : 0;
    }
    /// 返回当前不可变状态是否完成权威同步。
    bool is_synchronized() const {
        return valid() && verdandi_catalog_entry_is_synchronized(handle_.get()) != 0;
    }

    /// 从同一个不可变 Entry 状态加载并解码完整值，不执行 Redis 或磁盘 I/O。
    template <class T>
    result<catalog_snapshot<T>> load() const {
        if (!valid()) {
            return result<catalog_snapshot<T>>(error("invalid", "entry"));
        }
        std::uint64_t revision_value = 0;
        const char* status_value = NULL;
        int synchronized_value = 0;
        int present = 0;
        verdandi_field_set* output = NULL;
        verdandi_error failure = {};
        if (verdandi_catalog_entry_load(handle_.get(), &revision_value, &status_value, &synchronized_value, &present, &output, &failure) == 0) {
            return result<catalog_snapshot<T>>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_field_set, verdandi_field_set_release> output_handle(output);

        catalog_snapshot<T> snapshot;
        snapshot.revision = revision_value;
        snapshot.status = status_value == NULL ? "closed" : status_value;
        snapshot.synchronized = synchronized_value != 0;
        if (present != 0) {
            result<fields> loaded = detail::take_field_set(output_handle.release());
            if (!loaded) {
                return result<catalog_snapshot<T>>(loaded.failure());
            }
            result<T> decoded = detail::decode_value<T>(*loaded);
            if (!decoded) {
                return result<catalog_snapshot<T>>(decoded.failure());
            }
            snapshot.value.emplace(std::move(*decoded));
        }
        return result<catalog_snapshot<T>>(std::move(snapshot));
    }

private:
    catalog_entry(std::shared_ptr<detail::subscriber_state> owner, detail::owned_handle<verdandi_catalog_entry, verdandi_catalog_entry_release>&& handle,
                  catalog_path path_value)
        : owner_(std::move(owner)), handle_(std::move(handle)), path_(std::move(path_value)) {}

    std::shared_ptr<detail::subscriber_state> owner_;
    detail::owned_handle<verdandi_catalog_entry, verdandi_catalog_entry_release> handle_;
    catalog_path path_;

    friend class catalog_subscriber;
};

/// 查找稳定 Entry 并让返回值共享 Subscriber 生命周期；C 输出始终由 RAII 接管。
inline result<optional<catalog_entry>> catalog_subscriber::find(const catalog_path& path) const {
    if (!valid()) {
        return result<optional<catalog_entry>>(error("invalid", "subscriber"));
    }
    int found = 0;
    verdandi_catalog_entry* output = NULL;
    verdandi_error failure = {};
    if (verdandi_catalog_subscriber_find(state_->handle.get(), detail::native_path(path), &found, &output, &failure) == 0) {
        return result<optional<catalog_entry>>(error::from_native(failure));
    }
    detail::owned_handle<verdandi_catalog_entry, verdandi_catalog_entry_release> handle(output);
    if (found == 0) {
        return result<optional<catalog_entry>>(optional<catalog_entry>());
    }
    catalog_entry value(state_, std::move(handle), path);
    return result<optional<catalog_entry>>(optional<catalog_entry>(std::move(value)));
}

} // namespace legacy
} // namespace verdandi
