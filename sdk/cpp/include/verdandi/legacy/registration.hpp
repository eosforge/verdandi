#pragma once

#include "verdandi/c/registration.h"
#include "verdandi/legacy/client.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace verdandi {
namespace legacy {

struct registration_options {
    /// Zone 内 Registry 类型；具体长度和字符规则由 Redis 中的共享策略验证。
    std::string type;
    /// Redis 租约时长；必须是正整数毫秒且不超过协议上限。
    std::chrono::milliseconds ttl;
    /// 自动续期间隔；空值由核心按 TTL 和共享策略选择默认值。
    optional<std::chrono::milliseconds> renew_interval;
    /// 应用版本正整数；具体上限由同一原生配置检查负责。
    std::uint64_t version;

    /// 构造尚未填写的选项；TTL 和 Version 为零并将在创建时被拒绝。
    registration_options() : ttl(0), version(0) {}
};

/// Selector 和脱离结果拥有的 Registration 元数据。
struct registration_metadata {
    /// 服务进程本次启动生成且在 Registration 生命周期内不变的 UUID。
    std::string uuid;
    /// 内容 revision；Renew 不推进该值。
    std::uint64_t revision;
    /// 最近一次 Redis 确认的毫秒时间戳。
    std::uint64_t timestamp;
    /// Registration 的固定租约毫秒数。
    std::uint64_t ttl_ms;
    /// 应用提供的当前正整数版本。
    std::uint64_t version;

    /// 构造空元数据；数值在从 C ABI 填充前均为零。
    registration_metadata() : revision(0), timestamp(0), ttl_ms(0), version(0) {}
};

namespace detail {

struct registration_state {
    /// 同时持有根状态与 Registration 域句柄，保证释放顺序正确。
    registration_state(std::shared_ptr<root_state> root_value, verdandi_registration_client* value) : root(std::move(root_value)), handle(value) {}

    /// 延长根 Client 生命周期的共享所有权。
    std::shared_ptr<root_state> root;
    /// Registration 域 C 句柄的唯一释放所有权。
    owned_handle<verdandi_registration_client, verdandi_registration_client_release> handle;
};

/// 把回调期借用的 C 元数据完整复制为 Legacy 拥有值。
inline registration_metadata metadata(const verdandi_registration_metadata& value) {
    registration_metadata output;
    output.uuid.assign(value.uuid.data == NULL ? "" : value.uuid.data, value.uuid.size);
    output.revision = value.revision;
    output.timestamp = value.timestamp;
    output.ttl_ms = value.ttl_ms;
    output.version = value.version;
    return output;
}

} // namespace detail

/// Registration/Selector 子域；副本共享一个 C ABI 域句柄并保持根 Client 存活。
class registration_client {
public:
    /// 构造未打开的空领域 Client。
    registration_client() {}

    /// 从根 JSON 中的 Registration 配置打开领域并完成 Redis 策略引导。
    static result<registration_client> open(const client& root) {
        if (!root.valid()) {
            return result<registration_client>(error("invalid", "client"));
        }
        verdandi_registration_client* output = NULL;
        verdandi_error failure = {};
        if (verdandi_registration_client_open(root.state_->handle.get(), &output, &failure) == 0) {
            return result<registration_client>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_registration_client, verdandi_registration_client_release> guard(output);
        registration_client value;
        value.state_ = std::make_shared<detail::registration_state>(root.state_, output);
        guard.release();
        return result<registration_client>(std::move(value));
    }

    /// 返回当前是否持有 Registration 域句柄。
    bool valid() const {
        return state_ && state_->handle;
    }
    /// 返回领域是否仍接纳新 Registration 和 Selector。
    bool is_open() const {
        return valid() && verdandi_registration_client_is_open(state_->handle.get()) != 0;
    }

    /// 关闭领域及其子对象但不关闭根传输；重复关闭由核心幂等处理。
    result<void> close() const {
        if (!valid()) {
            return result<void>(error("invalid", "registration"));
        }
        verdandi_error failure = {};
        const int succeeded = verdandi_registration_client_close(state_->handle.get(), &failure);
        return detail::status(succeeded, failure);
    }

private:
    std::shared_ptr<detail::registration_state> state_;

    template <class Attr, class Data>
    friend class registration;
    template <class Attr, class Data>
    friend class selector;
};

/// C++11 强类型 Registration；编解码在调用线程完成，生命周期仍由 C++23 核心执行。
template <class Attr, class Data>
class registration {
public:
    /// 构造未绑定 UUID 的空 Registration。
    registration() {}
    /// 转移 Registration、领域和 UUID 所有权。
    registration(registration&&) noexcept = default;
    /// 释放旧句柄后转移 Registration、领域和 UUID 所有权。
    registration& operator=(registration&&) noexcept = default;
    registration(const registration&) = delete;
    registration& operator=(const registration&) = delete;

    /// 本地创建固定身份与租约对象；不发布 Redis 状态，首次写入留给 `publish`。
    static result<registration> create(const registration_client& owner, const registration_options& options) {
        if (!owner.valid()) {
            return result<registration>(error("invalid", "registration"));
        }
        verdandi_registration_options native = {};
        native.type = detail::native_text(options.type);
        result<std::uint64_t> ttl_ms = detail::native_duration(options.ttl, "ttl");
        if (!ttl_ms) {
            return result<registration>(ttl_ms.failure());
        }
        native.ttl_ms = *ttl_ms;
        native.version = options.version;
        if (options.renew_interval) {
            result<std::uint64_t> renew_interval_ms = detail::native_duration(*options.renew_interval, "renew_interval");
            if (!renew_interval_ms) {
                return result<registration>(renew_interval_ms.failure());
            }
            native.has_renew_interval = 1U;
            native.renew_interval_ms = *renew_interval_ms;
        }

        verdandi_registration* output = NULL;
        verdandi_error failure = {};
        if (verdandi_registration_create(owner.state_->handle.get(), &native, &output, &failure) == 0) {
            return result<registration>(error::from_native(failure));
        }
        detail::owned_handle<verdandi_registration, verdandi_registration_release> handle(output);
        const verdandi_string_view borrowed_uuid = verdandi_registration_uuid(output);
        const std::string uuid(borrowed_uuid.data == NULL ? "" : borrowed_uuid.data, borrowed_uuid.size);
        registration value(owner.state_, std::move(handle), uuid);
        return result<registration>(std::move(value));
    }

    /// 返回当前是否同时持有领域和 Registration 句柄。
    bool valid() const {
        return owner_ && handle_;
    }
    /// 返回本次进程生命周期内不变的 32 位 UUID；无效对象返回空字符串。
    const std::string& uuid() const {
        return uuid_;
    }
    /// 返回是否已成功完成首次发布且尚未关闭。
    bool is_published() const {
        return valid() && verdandi_registration_is_published(handle_.get()) != 0;
    }
    /// 返回核心当前期望的内容 revision；首次发布前或无效时为零。
    std::uint64_t revision() const {
        return valid() ? verdandi_registration_revision(handle_.get()) : 0;
    }
    /// 返回最近一次 Redis 确认的毫秒时间戳；尚无确认或无效时为零。
    std::uint64_t timestamp() const {
        return valid() ? verdandi_registration_timestamp(handle_.get()) : 0;
    }

    /// 服务准备完毕后发布完整不可变 Attr 和完整 Data，并启动核心同步任务。
    result<void> publish(const Attr& attr, const Data& data) {
        if (!valid()) {
            return result<void>(error("invalid", "registration"));
        }
        result<fields> encoded_attr = detail::encode_value(attr);
        if (!encoded_attr) {
            return result<void>(encoded_attr.failure());
        }
        result<fields> encoded_data = detail::encode_value(data);
        if (!encoded_data) {
            return result<void>(encoded_data.failure());
        }
        detail::native_fields native_attr(*encoded_attr);
        detail::native_fields native_data(*encoded_data);
        verdandi_error failure = {};
        const int succeeded = verdandi_registration_publish(handle_.get(), native_attr.view(), native_data.view(), &failure);
        return detail::status(succeeded, failure);
    }

    /// 编码并提交完整期望 Data；核心只发送变化字段并刷新租约。
    result<void> update(const Data& data) {
        if (!valid()) {
            return result<void>(error("invalid", "registration"));
        }
        result<fields> encoded = detail::encode_value(data);
        if (!encoded) {
            return result<void>(encoded.failure());
        }
        detail::native_fields native(*encoded);
        verdandi_error failure = {};
        const int succeeded = verdandi_registration_update(handle_.get(), native.view(), &failure);
        return detail::status(succeeded, failure);
    }

    /// 只修改应用 Version；实际变化时核心推进内容 revision 并刷新租约。
    result<void> set_version(std::uint64_t version) {
        if (!valid()) {
            return result<void>(error("invalid", "registration"));
        }
        verdandi_error failure = {};
        const int succeeded = verdandi_registration_set_version(handle_.get(), version, &failure);
        return detail::status(succeeded, failure);
    }

    /// 在一个核心内容变更中同时设置 Version 与完整期望 Data。
    result<void> update_content(std::uint64_t version, const Data& data) {
        if (!valid()) {
            return result<void>(error("invalid", "registration"));
        }
        result<fields> encoded = detail::encode_value(data);
        if (!encoded) {
            return result<void>(encoded.failure());
        }
        detail::native_fields native(*encoded);
        verdandi_error failure = {};
        const int succeeded = verdandi_registration_update_content(handle_.get(), version, native.view(), &failure);
        return detail::status(succeeded, failure);
    }

    /// 只刷新 timestamp 和租约，不修改内容 revision。
    result<void> renew() {
        if (!valid()) {
            return result<void>(error("invalid", "registration"));
        }
        verdandi_error failure = {};
        const int succeeded = verdandi_registration_renew(handle_.get(), &failure);
        return detail::status(succeeded, failure);
    }

    /// 非阻塞取得一条自动续期或恢复诊断；队列为空时 `available` 为假。
    result<diagnostic> try_error() {
        if (!valid()) {
            return result<diagnostic>(error("invalid", "registration"));
        }
        int available = 0;
        verdandi_error failure = {};
        const int succeeded = verdandi_registration_try_error(handle_.get(), &available, &failure);
        return detail::diagnostic_result(succeeded, available, failure);
    }

    /// 停止准入、排空已接纳工作并尽力注销 Redis 状态；核心保证幂等。
    result<void> close() {
        if (!valid()) {
            return result<void>(error("invalid", "registration"));
        }
        verdandi_error failure = {};
        const int succeeded = verdandi_registration_close(handle_.get(), &failure);
        return detail::status(succeeded, failure);
    }

private:
    registration(std::shared_ptr<detail::registration_state> owner, detail::owned_handle<verdandi_registration, verdandi_registration_release>&& handle,
                 std::string uuid)
        : owner_(std::move(owner)), handle_(std::move(handle)), uuid_(std::move(uuid)) {}

    std::shared_ptr<detail::registration_state> owner_;
    detail::owned_handle<verdandi_registration, verdandi_registration_release> handle_;
    std::string uuid_;
};

} // namespace legacy
} // namespace verdandi
