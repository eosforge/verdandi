// Exercise real public templates against deterministic Redis-facing core fixtures.
#include "verdandi/catalog/subscriber.hpp"
#include "verdandi/registration/selector.hpp"
#include <iostream>
#include <stdexcept>

struct throwing_data {
    std::int64_t power{};
    static inline bool armed = false;
    static inline int moves_before_failure = -1;
    static void moved() {
        if (moves_before_failure == 0) {
            throw std::runtime_error("injected application move failure");
        }
        if (moves_before_failure > 0) {
            --moves_before_failure;
        }
    }
    throwing_data() = default;
    throwing_data(const throwing_data&) = default;
    throwing_data& operator=(const throwing_data&) = default;
    throwing_data(throwing_data&& other) : power(other.power) {
        moved();
        if (armed && power == 22)
            throw std::runtime_error("move of second staged value");
    }
    throwing_data& operator=(throwing_data&& other) {
        moved();
        power = other.power;
        if (armed && power == 22)
            throw std::runtime_error("move of second staged value");
        return *this;
    }
};
VERDANDI_SCHEMA(throwing_data, VERDANDI_FIELD(throwing_data, power));

namespace verdandi::registration::detail {
class client_core {};
class selector_core {
public:
    std::shared_ptr<selector_view> view = std::make_shared<selector_view>();
};
result<std::shared_ptr<selector_core>> create_selector(const std::shared_ptr<client_core>&, selector_options options, projector projections) {
    auto core = std::make_shared<selector_core>();
    core->view->synchronized = true;
    for (int index = 1; index <= 2; ++index) {
        auto record = std::make_shared<selector_record>();
        record->meta = {options.type + std::to_string(index), 1, 1, 1000, 1};
        record->data = {{"power", *field_codec<std::int64_t>::encode(index)}};
        record->projected_attr = *projections.attr(record->attr);
        record->projected_data = *projections.data(record->data);
        core->view->records.emplace(record->meta.uuid, record);
        core->view->ordered.push_back(record);
    }
    return core;
}
std::shared_ptr<const selector_view> selector_current_view(const std::shared_ptr<selector_core>& core) noexcept {
    return core->view;
}
result<void> selector_validate_data(const std::shared_ptr<selector_core>&, const fields&) {
    return {};
}
std::chrono::milliseconds selector_wait(const std::shared_ptr<selector_core>&) noexcept {
    return std::chrono::seconds(3);
}
result<void> selector_close(const std::shared_ptr<selector_core>&) {
    return {};
}
std::optional<error> selector_error(const std::shared_ptr<selector_core>&) {
    return {};
}
class registration_core {
public:
    bool closed = false;
};
std::vector<std::shared_ptr<registration_core>> workers;
result<std::shared_ptr<registration_core>> create_registration(const std::shared_ptr<client_core>&, options) {
    auto core = std::make_shared<registration_core>();
    workers.push_back(core);
    return core;
}
result<void> registration_close(const std::shared_ptr<registration_core>& core) {
    if (core)
        core->closed = true;
    return {};
}
} // namespace verdandi::registration::detail
namespace verdandi::registration {
client::client(std::shared_ptr<detail::client_core> core) noexcept : core_(std::move(core)) {}
result<client> client::open(const verdandi::client&, const registration_configuration&) {
    return client(std::make_shared<detail::client_core>());
}
} // namespace verdandi::registration
namespace verdandi::catalog {
entry::entry(path target, status initial) : target_(std::move(target)) {
    auto value = std::make_shared<detail::entry_state>();
    value->revision = value->replace_revision = 1;
    value->shape = kind::map;
    value->state = initial;
    state_.store(std::move(value));
}
bool entry::synchronized_state(status value) noexcept {
    return value == status::present || value == status::absent || value == status::deleted;
}
namespace detail {
class subscriber_core {
public:
    static std::unique_ptr<entry> empty_value(kind shape, status state, bool exists = true) {
        auto output = std::unique_ptr<entry>(new entry(path{}, state));
        auto value = std::make_shared<entry_state>(*output->state_.load());
        value->shape = shape;
        value->replace_revision = exists ? 1 : 0;
        output->state_.store(std::move(value));
        return output;
    }
};
} // namespace detail
} // namespace verdandi::catalog
int main() {
    using namespace verdandi;
    using selector_type = registration::selector<fields, fields>;
    auto owner = registration::client::open(verdandi::client{}, registration_configuration{});
    auto a = selector_type::create(*owner, {"a"});
    auto b = selector_type::create(*owner, {"b"});
    registration::choice foreign;
    auto first = (*a)->one([&](auto candidates) -> result<std::optional<registration::choice>> {
        foreign = candidates.get(0)->identity();
        return std::optional(foreign);
    });
    auto second = (*b)->one([&](auto) -> result<std::optional<registration::choice>> { return std::optional(foreign); });
    const bool foreign_rejected = !second && second.error().category() == code::contract;
    std::cout << "foreign_choice_rejected=" << foreign_rejected << '\n';

    auto c = registration::selector<fields, throwing_data>::create(*owner, {"c"});
    auto partial = (*c)->one([&](auto candidates) -> result<std::optional<registration::choice>> {
        const auto one = candidates.get(0)->identity();
        const auto two = candidates.get(1)->identity();
        if (auto changed = candidates.mutate(one, [](auto& value) { value.power = 11; }); !changed)
            return std::unexpected(changed.error());
        if (auto changed = candidates.mutate(two, [](auto& value) { value.power = 22; }); !changed)
            return std::unexpected(changed.error());
        throwing_data::armed = true;
        return std::optional(one);
    });
    throwing_data::armed = false;
    auto after = (*c)->snapshot();
    const bool rolled_back = !partial && after && after->candidates[0].data.power == 1 && after->candidates[1].data.power == 2;
    std::cout << "error_rolls_back_all_staged_values=" << rolled_back << '\n';

    bool atomic = true;
    for (const bool any : {false, true}) {
        for (int budget = 0; budget != 10; ++budget) {
            auto target = registration::selector<fields, throwing_data>::create(*owner, {"fault"});
            auto stage = [&](auto& values) {
                static_cast<void>(values.mutate(values.get(0)->identity(), [](auto& data) { data.power = 11; }));
                static_cast<void>(values.mutate(values.get(1)->identity(), [](auto& data) { data.power = 22; }));
                throwing_data::moves_before_failure = budget;
                return values.get(0)->identity();
            };
            const bool success =
                any ? (*target)->any([&](auto& values) -> result<std::vector<registration::choice>> { return std::vector{stage(values)}; }).has_value()
                    : (*target)->one([&](auto& values) -> result<std::optional<registration::choice>> { return std::optional(stage(values)); }).has_value();
            throwing_data::moves_before_failure = -1;
            const auto state = (*target)->snapshot();
            atomic = atomic && state && state->candidates[0].data.power == (success ? 11 : 1) && state->candidates[1].data.power == (success ? 22 : 2);
        }
    }
    std::cout << "all_application_move_failures_are_atomic=" << atomic << '\n';

    bool retained = true;
    for (const auto shape : {catalog::kind::map, catalog::kind::array}) {
        for (const auto state : {catalog::status::present, catalog::status::unavailable, catalog::status::synchronizing, catalog::status::closed}) {
            const auto value = catalog::detail::subscriber_core::empty_value(shape, state)->load<fields>();
            retained = retained && value && value->value && value->value->empty();
        }
        for (const auto state : {catalog::status::absent, catalog::status::deleted, catalog::status::closed}) {
            const auto value = catalog::detail::subscriber_core::empty_value(shape, state, false)->load<fields>();
            retained = retained && value && !value->value;
        }
    }
    std::cout << "retained_empty_containers_preserve_presence=" << retained << '\n';

    using registration_type = registration::registration<fields, fields>;
    auto r1 = registration_type::create(*owner, {});
    auto r2 = registration_type::create(*owner, {});
    *r1 = std::move(*r2);
    const bool released = registration::detail::workers[0]->closed && !registration::detail::workers[1]->closed;
    std::cout << "move_assignment_releases_old_owner=" << released << '\n';
    return foreign_rejected && rolled_back && atomic && retained && released ? 0 : 1;
}
