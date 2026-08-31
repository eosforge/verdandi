#pragma once

#include "verdandi/client.hpp"
#include "verdandi/configuration.hpp"
#include "verdandi/schema.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace verdandi::registration {

struct options;
template <structured_value Attr, structured_value Data>
class selector;

namespace detail {
class client_core;
class registration_core;

[[nodiscard]] result<std::shared_ptr<registration_core>> create_registration(const std::shared_ptr<client_core>& owner, options value);
[[nodiscard]] std::string_view registration_uuid(const std::shared_ptr<registration_core>& value) noexcept;
[[nodiscard]] bool registration_published(const std::shared_ptr<registration_core>& value) noexcept;
[[nodiscard]] std::uint64_t registration_revision(const std::shared_ptr<registration_core>& value) noexcept;
[[nodiscard]] std::uint64_t registration_timestamp(const std::shared_ptr<registration_core>& value) noexcept;
[[nodiscard]] result<void> registration_publish(const std::shared_ptr<registration_core>& value, fields attr, fields data);
[[nodiscard]] result<void> registration_update(const std::shared_ptr<registration_core>& value, std::optional<std::uint64_t> version,
                                               std::optional<fields> data);
[[nodiscard]] result<void> registration_renew(const std::shared_ptr<registration_core>& value);
[[nodiscard]] result<void> registration_close(const std::shared_ptr<registration_core>& value);
[[nodiscard]] std::optional<error> registration_error(const std::shared_ptr<registration_core>& value);
} // namespace detail

/// 一条尚未发布的本地 Registration 的固定身份和租约选项。
struct options {
    /// Zone 内 Registry 类型；首字符是 ASCII 字母，总长 1..64 字节。
    std::string type;
    /// Redis 租约时长；必须为正整数毫秒且不超过 Hash-field 截止值。
    std::chrono::milliseconds ttl{};
    /// 自动续期间隔；空值使用 ttl/3，显式值必须在配置下限与 ttl/3 之间。
    std::optional<std::chrono::milliseconds> renew_interval;
    /// 应用版本正整数；范围 1..2^53-1。
    std::uint64_t version{};
};

/// Registration/Selector 领域 Client；借用根传输但独立拥有 Zone 和工作生命周期。
class client final {
public:
    client() noexcept = default;

    /// 校验配置、确认 Redis 8、加载脚本并补齐缺失的 Zone 默认策略。
    [[nodiscard]] static result<client> open(const verdandi::client& transport, const registration_configuration& configuration);

    /// 关闭全部 Registration/Selector 子对象并等待其任务结束，不关闭根 Redis Client。
    [[nodiscard]] result<void> close() const;

    /// 返回领域 Client 是否仍接纳新对象。
    [[nodiscard]] bool open() const noexcept;

private:
    explicit client(std::shared_ptr<detail::client_core> core) noexcept;

    std::shared_ptr<detail::client_core> core_;

    template <structured_value Attr, structured_value Data>
    friend class registration;
    template <structured_value Attr, structured_value Data>
    friend class selector;
};

/// 一个应用拥有、延迟发布且内部只有一个串行同步任务的强类型 Registration。
template <structured_value Attr, structured_value Data>
class registration final {
public:
    registration() noexcept = default;
    registration(const registration&) = delete;
    registration& operator=(const registration&) = delete;
    registration(registration&&) noexcept = default;
    registration& operator=(registration&&) noexcept = default;
    ~registration() {
        if (core_) {
            static_cast<void>(close());
        }
    }

    /// 只在本地校验选项并生成本次进程生命周期 UUID；不执行 Redis I/O，也不启动任务。
    [[nodiscard]] static result<registration> create(const client& owner, options value) {
        auto created = create_core(owner, std::move(value));
        if (!created) {
            return std::unexpected(created.error());
        }
        return registration(std::move(*created));
    }

    /// 返回构造时生成且在句柄终止前不变的 32 位小写十六进制 UUID。
    [[nodiscard]] std::string_view uuid() const noexcept {
        return detail::registration_uuid(core_);
    }

    /// 返回是否已成功完成首次发布且尚未终止。
    [[nodiscard]] bool published() const noexcept {
        return detail::registration_published(core_);
    }

    /// 返回当前期望内容 revision；首次发布前为零。
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return detail::registration_revision(core_);
    }

    /// 返回最近一次 Redis 确认的毫秒时间戳；尚无确认时为零。
    [[nodiscard]] std::uint64_t timestamp() const noexcept {
        return detail::registration_timestamp(core_);
    }

    /// 应用准备完毕后发布完整不可变 Attr 和完整 Data，并启动唯一同步任务。
    /// C++ 的 `register` 是保留关键字，因此公开名称使用 `publish`。
    [[nodiscard]] result<void> publish(const Attr& attr, const Data& data) {
        auto encoded_attr = encode_value(attr);
        if (!encoded_attr) {
            return std::unexpected(encoded_attr.error());
        }
        auto encoded_data = encode_value(data);
        if (!encoded_data) {
            return std::unexpected(encoded_data.error());
        }
        return publish_fields(std::move(*encoded_attr), std::move(*encoded_data));
    }

    /// 提交完整期望 Data；固定字段集合不变，只把变化字段合并进该对象的单格 Fields 邮箱。
    [[nodiscard]] result<void> update(const Data& data) {
        auto encoded = encode_value(data);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return update_fields(std::nullopt, std::move(*encoded));
    }

    /// 只修改应用 Version；实际变化时推进一个内容 revision 并刷新 TTL。
    [[nodiscard]] result<void> set_version(const std::uint64_t version) {
        return update_fields(version, std::nullopt);
    }

    /// 用一个原子内容 revision 同时修改 Version 和完整期望 Data。
    [[nodiscard]] result<void> update_content(const std::uint64_t version, const Data& data) {
        auto encoded = encode_value(data);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return update_fields(version, std::move(*encoded));
    }

    /// 显式刷新 timestamp 和租约，不修改内容 revision；与 Update 共用同一邮箱和任务。
    [[nodiscard]] result<void> renew() {
        return detail::registration_renew(core_);
    }

    /// 终止准入、排空已接纳工作、尽力删除 Redis 状态并汇合唯一任务；幂等。
    [[nodiscard]] result<void> close() {
        return detail::registration_close(core_);
    }

    /// 非阻塞取得一条自动续期/恢复诊断；队列为空时返回空 optional。
    [[nodiscard]] std::optional<error> try_error() {
        return detail::registration_error(core_);
    }

private:
    explicit registration(std::shared_ptr<detail::registration_core> core) noexcept : core_(std::move(core)) {}

    [[nodiscard]] static result<std::shared_ptr<detail::registration_core>> create_core(const client& owner, options value) {
        if (!owner.core_) {
            return std::unexpected(error(code::closed));
        }
        return detail::create_registration(owner.core_, std::move(value));
    }
    [[nodiscard]] result<void> publish_fields(fields attr, fields data) {
        return detail::registration_publish(core_, std::move(attr), std::move(data));
    }
    [[nodiscard]] result<void> update_fields(std::optional<std::uint64_t> version, std::optional<fields> data) {
        return detail::registration_update(core_, version, std::move(data));
    }

    std::shared_ptr<detail::registration_core> core_;
};

} // namespace verdandi::registration
