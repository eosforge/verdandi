#pragma once

#include "internal/catalog.hpp"

namespace verdandi::catalog::detail {

class subscriber_core final : public std::enable_shared_from_this<subscriber_core> {
public:
    subscriber_core(const subscriber_core&) = delete;
    subscriber_core& operator=(const subscriber_core&) = delete;
    ~subscriber_core();

    [[nodiscard]] static result<std::shared_ptr<subscriber_core>> create(const std::shared_ptr<client_core>& owner, subscription value);
    [[nodiscard]] std::shared_ptr<entry> find(const path& target);
    [[nodiscard]] result<void> close();
    [[nodiscard]] std::optional<error> try_error();

private:
    struct implementation;

    explicit subscriber_core(std::unique_ptr<implementation> implementation) noexcept;

    std::unique_ptr<implementation> implementation_;
};

} // namespace verdandi::catalog::detail
