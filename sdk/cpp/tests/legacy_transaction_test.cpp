#include "verdandi/legacy/selector.hpp"

#include <stdexcept>

struct prediction {
    std::int64_t power;
    static bool reject_moves;
    prediction() : power(0) {}
    explicit prediction(std::int64_t value) : power(value) {}
    prediction(const prediction&) = default;
    prediction(prediction&& other) : power(other.power) {
        if (reject_moves) {
            throw std::runtime_error("application move after commit");
        }
    }
};
bool prediction::reject_moves = false;

namespace verdandi {
namespace legacy {
template <>
struct codec<prediction> {
    static result<fields> encode(const prediction& value) {
        fields output;
        const auto added = output.insert("power", value.power);
        if (!added) {
            return result<fields>(added.failure());
        }
        return result<fields>(std::move(output));
    }
    static result<prediction> decode(const fields& value) {
        const auto power = value.get<std::int64_t>("power");
        if (!power || *power == 2) {
            return result<prediction>(error("contract", "prediction"));
        }
        return result<prediction>(prediction(*power));
    }
};
} // namespace legacy
} // namespace verdandi

// 仅替代 C ABI 的 Redis 核心，公开 Legacy 编解码、策略与所有权实现直接来自项目头文件。
struct verdandi_client {};
struct verdandi_registration_client {};
struct verdandi_selector {
    std::int64_t power = 1;
};
struct verdandi_candidates {
    std::int64_t power;
};
struct verdandi_selection {
    bool selected = false;
};
struct verdandi_candidate_list {};

extern "C" {
void verdandi_error_reset(verdandi_error* value) {
    if (value) {
        *value = verdandi_error{};
    }
}
int verdandi_client_open_json(verdandi_bytes_view, verdandi_client** output, verdandi_error*) {
    *output = new verdandi_client;
    return 1;
}
void verdandi_client_release(verdandi_client* value) {
    delete value;
}
int verdandi_registration_client_open(verdandi_client*, verdandi_registration_client** output, verdandi_error*) {
    *output = new verdandi_registration_client;
    return 1;
}
void verdandi_registration_client_release(verdandi_registration_client* value) {
    delete value;
}
int verdandi_selector_create(verdandi_registration_client*, verdandi_string_view, verdandi_selector** output, verdandi_error*) {
    *output = new verdandi_selector;
    return 1;
}
void verdandi_selector_release(verdandi_selector* value) {
    delete value;
}
size_t verdandi_candidates_size(const verdandi_candidates*) {
    return 1;
}
int verdandi_candidates_metadata(const verdandi_candidates*, size_t index, verdandi_registration_metadata* output) {
    if (index != 0)
        return 0;
    const verdandi_registration_metadata value = {{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32}, 1, 1, 1000, 1};
    *output = value;
    return 1;
}
int verdandi_candidates_visit_attr(const verdandi_candidates*, size_t, verdandi_field_visitor, void*, verdandi_error*) {
    return 1;
}
int verdandi_candidates_visit_data(const verdandi_candidates* value, size_t, verdandi_field_visitor visitor, void* context, verdandi_error*) {
    const std::string text = std::to_string(value->power);
    const verdandi_string_view name = {"power", 5};
    const verdandi_bytes_view bytes = {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
    return visitor(context, name, bytes);
}
int verdandi_candidates_mutate(verdandi_candidates* value, size_t, verdandi_fields_view data, verdandi_error*) {
    const verdandi_bytes_view bytes = data.data[0].value;
    value->power = std::stoll(std::string(reinterpret_cast<const char*>(bytes.data), bytes.size));
    return 1;
}
int verdandi_selection_add(verdandi_selection* value, size_t index, verdandi_error*) {
    if (index != 0 || value->selected)
        return 0;
    value->selected = true;
    return 1;
}
int verdandi_selector_one(verdandi_selector* value, verdandi_selector_policy policy, void* context, verdandi_candidate_list** output, verdandi_error* error) {
    verdandi_candidates candidates = {value->power};
    verdandi_selection selection;
    *output = NULL;
    if (!policy(context, &candidates, &selection, error))
        return 0;
    if (selection.selected) {
        value->power = candidates.power;
        *output = new verdandi_candidate_list;
        prediction::reject_moves = true;
    }
    return 1;
}
int verdandi_selector_any(verdandi_selector* value, verdandi_selector_policy policy, void* context, verdandi_candidate_list** output, verdandi_error* error) {
    return verdandi_selector_one(value, policy, context, output, error);
}
void verdandi_candidate_list_release(verdandi_candidate_list* value) {
    delete value;
}
}

int main() {
    using namespace verdandi::legacy;
    static_assert(std::is_nothrow_move_constructible<candidate<fields, prediction>>::value, "Candidate return must not invoke application moves");
    auto root = client::open("{}");
    auto domain = registration_client::open(*root);
    auto selected = selector<fields, prediction>::create(*domain, "Fixture");
    auto failed_one = selected->one([](candidates<fields, prediction>& values) -> result<optional<choice>> {
        auto changed = values.mutate(choice(0), prediction(2));
        if (!changed)
            return result<optional<choice>>(changed.failure());
        return result<optional<choice>>(optional<choice>(choice(0)));
    });
    if (failed_one || failed_one.failure().field() != "prediction")
        return 1;
    auto failed_any = selected->any([](candidates<fields, prediction>& values) -> result<std::vector<choice>> {
        auto changed = values.mutate(choice(0), prediction(2));
        if (!changed)
            return result<std::vector<choice>>(changed.failure());
        return result<std::vector<choice>>(std::vector<choice>(1, choice(0)));
    });
    if (failed_any || failed_any.failure().field() != "prediction")
        return 1;
    auto unchanged =
        selected->one([](candidates<fields, prediction>&) -> result<optional<choice>> { return result<optional<choice>>(optional<choice>(choice(0))); });
    prediction::reject_moves = false;
    return unchanged && *unchanged && (**unchanged).data().power == 1 ? 0 : 1;
}
