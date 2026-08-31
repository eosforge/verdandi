#pragma once

#include "verdandi/catalog/path.hpp"
#include "verdandi/client.hpp"
#include "verdandi/configuration.hpp"

#include <memory>

namespace verdandi::catalog {

namespace detail {
class client_core;
}

/// Catalog 领域 Client；复用根 Redis 传输但独立拥有 Zone、脚本和子对象生命周期。
class client final {
public:
    client() noexcept = default;

    /// 校验配置、确认 Redis 8 并准备 Catalog 脚本与可选本地检查点。
    [[nodiscard]] static result<client> open(const verdandi::client& transport, const catalog_configuration& configuration);

    /// 关闭 Subscriber 和检查点资源，不关闭根 Redis Client；幂等。
    [[nodiscard]] result<void> close() const;

    /// 返回是否仍可接纳 Publisher 操作或新 Subscriber。
    [[nodiscard]] bool open() const noexcept;

private:
    explicit client(std::shared_ptr<detail::client_core> core) noexcept : core_(std::move(core)) {}

    std::shared_ptr<detail::client_core> core_;

    friend class publisher;
    friend class subscriber;
};

} // namespace verdandi::catalog
