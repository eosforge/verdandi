#include "internal/registration_pending.hpp"
#include "internal/retry.hpp"
#include "internal/selector_state.hpp"
#include "internal/subscription_queue.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
int failures{};
std::size_t checks{};

void check(const bool passed, const std::string_view message) {
    ++checks;
    if (!passed) {
        ++failures;
        std::cerr << message << '\n';
    }
}

void overflow_precedes_control_items() {
    using namespace verdandi::detail;
    using kind = subscription_item::kind;
    for (const auto control : {kind::fence, kind::reconnected, kind::failure, kind::closed}) {
        subscription_queue queue(1);
        queue.push({kind::message, {}, "before", {}, 0, {}});
        queue.push({control, {}, {}, {}, 7, {}});
        check(queue.pop().type == kind::lagged, "overflow must precede every retained control item");
        check(queue.pop().type == control && queue.empty(), "control remains available after loss notification");
        queue.push({kind::message, {}, "a", {}, 0, {}});
        queue.push({kind::message, {}, "b", {}, 0, {}});
        queue.push({control, {}, {}, {}, 8, {}});
        check(queue.pop().type == kind::lagged, "later control cannot erase pending loss");
        check(queue.pop().type == control && queue.empty(), "queue resets loss after delivery");
    }
    subscription_queue queue(2);
    queue.push({kind::message, {}, "a", {}, 0, {}});
    queue.push({kind::fence, {}, {}, {}, 1, {}});
    check(queue.pop().payload == "a" && queue.pop().type == kind::fence, "non-overflow preserves fence order");
}

void pending_preserves_gaps_and_accounts_for_merged_fields() {
    using namespace verdandi;
    using namespace verdandi::registration::detail;
    registration_event registered;
    registered.uuid = "a";
    registered.revision = 1;
    registered.data = {{"a", bytes(1)}, {"b", bytes(1)}};
    auto update = registered;
    update.kind = event_kind::update;
    update.base_revision = 2;
    update.revision = 3;
    update.data = {{"a", bytes(10)}};
    pending_events gap(10, 4096);
    check(gap.add(registered).has_value() && gap.add(update).has_value(), "gap fits budget");
    const auto sequence = gap.drain();
    check(sequence.size() == 2 && sequence[0].revision == 1 && sequence[1].base_revision == 2, "register cannot hide missing update");

    update.base_revision = 1;
    update.revision = 2;
    pending_events consecutive(10, 4096);
    check(consecutive.add(registered).has_value() && consecutive.add(update).has_value(), "consecutive updates fit budget");
    const auto merged = consecutive.drain();
    check(merged.size() == 1 && merged[0].revision == 2 && merged[0].data.at("a").size() == 10, "continuous updates still compact");

    update.data = {{"new", bytes(1)}};
    check(consecutive.add(registered).has_value() && consecutive.add(update).has_value(), "unknown-field sequence remains bounded");
    check(consecutive.drain().size() == 2, "unknown data field must reach authoritative repair");

    pending_events budget(10, 300);
    update.data = {{"a", bytes(100)}};
    check(budget.add(update).has_value(), "first update fits");
    update.base_revision = 2;
    update.revision = 3;
    update.data = {{"b", bytes(100)}};
    const auto rejected = budget.add(update);
    check(!rejected && rejected.error().category() == code::capacity, "union of individually fitting updates must respect byte limit");
    const auto preserved = budget.drain();
    check(preserved.size() == 1 && preserved[0].revision == 2 && preserved[0].data.size() == 1, "capacity rejection preserves accepted merge");
}

void renewal_keeps_one_deadline_per_registration() {
    using namespace verdandi::registration::detail;
    selector_state state;
    verdandi::selector_configuration configuration;
    configuration.max_active_bytes = configuration.max_retained_bytes = 1024;
    for (std::uint64_t deadline = 1000; deadline < 11000; ++deadline) {
        auto record = std::make_shared<selector_record>();
        record->meta.uuid = "a";
        record->meta.ttl = 100;
        record->deadline = deadline;
        record->size = 10;
        check(set_active(state, record, configuration).has_value(), "renewal accepted");
        check(state.active_deadlines.size() == 1 && state.retained_deadlines.empty(), "renewal does not accumulate old deadlines");
    }
    auto second = std::make_shared<selector_record>(*state.active.at("a"));
    second->meta.uuid = "b";
    check(set_active(state, second, configuration).has_value() && state.active_deadlines.size() == 2, "equal deadlines keep both UUIDs");
    check(!expire(state, 10998, configuration), "old deadlines cannot prematurely expire current records");
    check(expire(state, 10999, configuration) && state.active.empty() && state.retained.size() == 2, "expiry retains both records");
    check(state.active_deadlines.empty() && state.retained_deadlines.size() == 2, "retention transfers deadline ownership");
    remove_record(state, "a");
    check(state.retained.size() == 1 && state.retained_deadlines.size() == 1, "removal erases retained deadline");
    check(expire(state, 11099, configuration) && state.retained.empty() && state.retained_deadlines.empty(), "second deadline releases all state");
}

void diagnostics_and_retry_boundaries() {
    using namespace verdandi;
    for (const auto& source : {std::string(513, 'x'), std::string("x") + std::string(512, 'x')}) {
        check(error(code::unavailable).with_detail(source).detail().size() == 512, "ASCII detail limit");
    }
    std::string chinese;
    for (int index = 0; index < 200; ++index) {
        chinese += "错";
    }
    check(error(code::unavailable).with_detail(chinese).detail() == chinese.substr(0, 510), "diagnostic retains complete UTF-8 characters");
    reconnect_configuration configuration;
    configuration.initial_delay = std::chrono::milliseconds(10);
    configuration.max_delay = std::chrono::milliseconds(1000);
    configuration.multiplier = 1;
    configuration.jitter_percent = 0;
    check(detail::retry_delay(configuration, std::numeric_limits<std::size_t>::max()) == configuration.initial_delay, "factor=1 ignores failure count");
    configuration.multiplier = 2;
    check(detail::retry_delay(configuration, 3).count() == 80, "exponential delay remains correct");
    check(detail::retry_delay(configuration, 100).count() == 1000, "retry caps at configured maximum");
}
} // namespace

int main() {
    overflow_precedes_control_items();
    pending_preserves_gaps_and_accounts_for_merged_fields();
    renewal_keeps_one_deadline_per_registration();
    diagnostics_and_retry_boundaries();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
