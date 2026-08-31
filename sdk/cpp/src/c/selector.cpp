#include "c/internal.hpp"

namespace {

using native_candidate = verdandi::registration::candidate<verdandi::fields, verdandi::fields>;
using native_candidates = verdandi::registration::candidates<verdandi::fields, verdandi::fields>;

[[nodiscard]] verdandi::result<void> invoke_policy(const verdandi_selector_policy policy, void* context, verdandi_candidates& candidates,
                                                   verdandi_selection& selection) {
    if (policy == nullptr) {
        return std::unexpected(verdandi::error(verdandi::code::invalid, "policy"));
    }
    verdandi_error callback_error{};
    if (policy(context, &candidates, &selection, &callback_error) == 0) {
        if (callback_error.code[0] == '\0') {
            return std::unexpected(verdandi::error(verdandi::code::contract, "callback"));
        }
        return std::unexpected(verdandi::c_api::read_callback_error(callback_error));
    }
    return {};
}

[[nodiscard]] const native_candidate* list_candidate(const verdandi_candidate_list* value, const std::size_t index) noexcept {
    if (value == nullptr || index >= value->values.size()) {
        return nullptr;
    }
    return &value->values[index];
}

[[nodiscard]] const native_candidate* snapshot_candidate(const verdandi_selector_snapshot* value, const bool retained, const std::size_t index) noexcept {
    if (value == nullptr) {
        return nullptr;
    }
    if (retained) {
        if (index >= value->value.retained.size()) {
            return nullptr;
        }
        return &value->value.retained[index].value;
    }
    if (index >= value->value.candidates.size()) {
        return nullptr;
    }
    return &value->value.candidates[index];
}

[[nodiscard]] verdandi::result<void> visit_candidate(const native_candidate* value, const bool attr, const verdandi_field_visitor visitor, void* context) {
    if (value == nullptr) {
        return std::unexpected(verdandi::error(verdandi::code::invalid, "candidate"));
    }
    return verdandi::c_api::visit_fields(attr ? value->attr : value->data, visitor, context);
}

} // namespace

extern "C" {

int VERDANDI_C_CALL verdandi_selector_create(verdandi_registration_client* client, const verdandi_string_view type, verdandi_selector** output,
                                             verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (client == nullptr || output == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, client == nullptr ? "client" : "output"));
        }
        auto native_type = verdandi::c_api::read_text(type, "type");
        if (!native_type) {
            return std::unexpected(native_type.error());
        }
        auto selector = verdandi::registration::selector<verdandi::fields, verdandi::fields>::create(
            client->value, verdandi::registration::selector_options{std::string(*native_type)});
        if (!selector) {
            return std::unexpected(selector.error());
        }
        *output = new verdandi_selector{std::move(*selector)};
        return {};
    });
}

std::size_t VERDANDI_C_CALL verdandi_candidates_size(const verdandi_candidates* value) {
    return value == nullptr || value->value == nullptr ? 0 : value->value->size();
}

int VERDANDI_C_CALL verdandi_candidates_metadata(const verdandi_candidates* value, const std::size_t index, verdandi_registration_metadata* output) {
    if (output != nullptr) {
        *output = {};
    }
    if (value == nullptr || value->value == nullptr || output == nullptr) {
        return 0;
    }
    const auto candidate = value->value->get(index);
    if (!candidate) {
        return 0;
    }
    verdandi::c_api::write_metadata(candidate->meta(), output);
    return 1;
}

int VERDANDI_C_CALL verdandi_candidates_visit_attr(const verdandi_candidates* value, const std::size_t index, const verdandi_field_visitor visitor,
                                                   void* context, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || value->value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "candidates"));
        }
        const auto candidate = value->value->get(index);
        if (!candidate) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "candidate"));
        }
        return verdandi::c_api::visit_fields(candidate->attr(), visitor, context);
    });
}

int VERDANDI_C_CALL verdandi_candidates_visit_data(const verdandi_candidates* value, const std::size_t index, const verdandi_field_visitor visitor,
                                                   void* context, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || value->value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "candidates"));
        }
        const auto candidate = value->value->get(index);
        if (!candidate) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "candidate"));
        }
        return verdandi::c_api::visit_fields(candidate->data(), visitor, context);
    });
}

int VERDANDI_C_CALL verdandi_candidates_mutate(verdandi_candidates* value, const std::size_t index, const verdandi_fields_view data, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || value->value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "candidates"));
        }
        const auto candidate = value->value->get(index);
        if (!candidate) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "candidate"));
        }
        auto replacement = verdandi::c_api::read_fields(data, "data");
        if (!replacement) {
            return std::unexpected(replacement.error());
        }
        return value->value->mutate(candidate->identity(), [&](verdandi::fields& current) { current = std::move(*replacement); });
    });
}

int VERDANDI_C_CALL verdandi_selection_add(verdandi_selection* value, const std::size_t index, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || value->candidates == nullptr || value->candidates->value == nullptr) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "selection"));
        }
        const auto candidate = value->candidates->value->get(index);
        if (!candidate) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "candidate"));
        }
        if (value->one) {
            if (value->selected) {
                return std::unexpected(verdandi::error(verdandi::code::contract, "candidate"));
            }
            value->selected = candidate->identity();
        } else {
            value->many.push_back(candidate->identity());
        }
        return {};
    });
}

int VERDANDI_C_CALL verdandi_selector_one(verdandi_selector* value, const verdandi_selector_policy policy, void* context, verdandi_candidate_list** output,
                                          verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || !value->value || output == nullptr) {
            const auto* field = value == nullptr || !value->value ? "selector" : "output";
            return std::unexpected(verdandi::error(verdandi::code::invalid, field));
        }
        auto selected = value->value->one([&](native_candidates& native) -> verdandi::result<std::optional<verdandi::registration::choice>> {
            verdandi_candidates candidates{&native};
            verdandi_selection selection{&candidates, true, std::nullopt, {}};
            if (auto status = invoke_policy(policy, context, candidates, selection); !status) {
                return std::unexpected(status.error());
            }
            return selection.selected;
        });
        if (!selected) {
            return std::unexpected(selected.error());
        }
        if (*selected) {
            std::vector<native_candidate> values;
            values.push_back(std::move(**selected));
            *output = new verdandi_candidate_list{std::move(values)};
        }
        return {};
    });
}

int VERDANDI_C_CALL verdandi_selector_any(verdandi_selector* value, const verdandi_selector_policy policy, void* context, verdandi_candidate_list** output,
                                          verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || !value->value || output == nullptr) {
            const auto* field = value == nullptr || !value->value ? "selector" : "output";
            return std::unexpected(verdandi::error(verdandi::code::invalid, field));
        }
        auto selected = value->value->any([&](native_candidates& native) -> verdandi::result<std::vector<verdandi::registration::choice>> {
            verdandi_candidates candidates{&native};
            verdandi_selection selection{&candidates, false, std::nullopt, {}};
            if (auto status = invoke_policy(policy, context, candidates, selection); !status) {
                return std::unexpected(status.error());
            }
            return std::move(selection.many);
        });
        if (!selected) {
            return std::unexpected(selected.error());
        }
        if (!selected->empty()) {
            *output = new verdandi_candidate_list{std::move(*selected)};
        }
        return {};
    });
}

std::size_t VERDANDI_C_CALL verdandi_candidate_list_size(const verdandi_candidate_list* value) {
    return value == nullptr ? 0 : value->values.size();
}

int VERDANDI_C_CALL verdandi_candidate_list_metadata(const verdandi_candidate_list* value, const std::size_t index, verdandi_registration_metadata* output) {
    if (output != nullptr) {
        *output = {};
    }
    const auto* candidate = list_candidate(value, index);
    if (candidate == nullptr || output == nullptr) {
        return 0;
    }
    verdandi::c_api::write_metadata(candidate->meta, output);
    return 1;
}

int VERDANDI_C_CALL verdandi_candidate_list_visit_attr(const verdandi_candidate_list* value, const std::size_t index, const verdandi_field_visitor visitor,
                                                       void* context, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&] { return visit_candidate(list_candidate(value, index), true, visitor, context); });
}

int VERDANDI_C_CALL verdandi_candidate_list_visit_data(const verdandi_candidate_list* value, const std::size_t index, const verdandi_field_visitor visitor,
                                                       void* context, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&] { return visit_candidate(list_candidate(value, index), false, visitor, context); });
}

void VERDANDI_C_CALL verdandi_candidate_list_release(verdandi_candidate_list* value) {
    delete value;
}

int VERDANDI_C_CALL verdandi_selector_snapshot_create(verdandi_selector* value, verdandi_selector_snapshot** output, verdandi_error* error) {
    if (output != nullptr) {
        *output = nullptr;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || !value->value || output == nullptr) {
            const auto* field = value == nullptr || !value->value ? "selector" : "output";
            return std::unexpected(verdandi::error(verdandi::code::invalid, field));
        }
        auto snapshot = value->value->snapshot();
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        *output = new verdandi_selector_snapshot{std::move(*snapshot)};
        return {};
    });
}

std::uint64_t VERDANDI_C_CALL verdandi_selector_snapshot_generation(const verdandi_selector_snapshot* value) {
    return value == nullptr ? 0 : value->value.generation;
}

int VERDANDI_C_CALL verdandi_selector_snapshot_is_synchronized(const verdandi_selector_snapshot* value) {
    return value != nullptr && value->value.synchronized ? 1 : 0;
}

std::size_t VERDANDI_C_CALL verdandi_selector_snapshot_size(const verdandi_selector_snapshot* value, const int retained) {
    if (value == nullptr) {
        return 0;
    }
    return retained != 0 ? value->value.retained.size() : value->value.candidates.size();
}

int VERDANDI_C_CALL verdandi_selector_snapshot_metadata(const verdandi_selector_snapshot* value, const int retained, const std::size_t index,
                                                        verdandi_registration_metadata* output) {
    if (output != nullptr) {
        *output = {};
    }
    const auto* candidate = snapshot_candidate(value, retained != 0, index);
    if (candidate == nullptr || output == nullptr) {
        return 0;
    }
    verdandi::c_api::write_metadata(candidate->meta, output);
    return 1;
}

std::uint64_t VERDANDI_C_CALL verdandi_selector_snapshot_retained_until(const verdandi_selector_snapshot* value, const std::size_t index) {
    if (value == nullptr || index >= value->value.retained.size()) {
        return 0;
    }
    return value->value.retained[index].retained_until;
}

int VERDANDI_C_CALL verdandi_selector_snapshot_visit_attr(const verdandi_selector_snapshot* value, const int retained, const std::size_t index,
                                                          const verdandi_field_visitor visitor, void* context, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&] { return visit_candidate(snapshot_candidate(value, retained != 0, index), true, visitor, context); });
}

int VERDANDI_C_CALL verdandi_selector_snapshot_visit_data(const verdandi_selector_snapshot* value, const int retained, const std::size_t index,
                                                          const verdandi_field_visitor visitor, void* context, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&] { return visit_candidate(snapshot_candidate(value, retained != 0, index), false, visitor, context); });
}

void VERDANDI_C_CALL verdandi_selector_snapshot_release(verdandi_selector_snapshot* value) {
    delete value;
}

int VERDANDI_C_CALL verdandi_selector_try_error(verdandi_selector* value, int* available, verdandi_error* error) {
    if (available != nullptr) {
        *available = 0;
    }
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || !value->value || available == nullptr) {
            const auto* field = value == nullptr || !value->value ? "selector" : "output";
            return std::unexpected(verdandi::error(verdandi::code::invalid, field));
        }
        if (auto diagnostic = value->value->try_error()) {
            *available = 1;
            verdandi::c_api::write_error(error, *diagnostic);
        }
        return {};
    });
}

int VERDANDI_C_CALL verdandi_selector_close(verdandi_selector* value, verdandi_error* error) {
    return verdandi::c_api::boundary(error, [&]() -> verdandi::result<void> {
        if (value == nullptr || !value->value) {
            return std::unexpected(verdandi::error(verdandi::code::invalid, "selector"));
        }
        return value->value->close();
    });
}

void VERDANDI_C_CALL verdandi_selector_release(verdandi_selector* value) {
    if (value != nullptr) {
        if (value->value) {
            static_cast<void>(value->value->close());
        }
        delete value;
    }
}

} // extern "C"
