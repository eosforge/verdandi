#include "ephemeris_edition.hpp"

namespace astra {
Ephemeris::State::Projection::Event Ephemeris::Encoding::merge(const State::Projection::Event& previous, State::Projection::Event current) noexcept {
    if (current.record && (!previous.record || !previous.data)) {
        current.data = false;
    }
    return current;
}

void Ephemeris::Encoding::append(Reply& page, std::string_view key, const Content* record, bool data) {
    auto* change = page.add_changes(); // 本次消息唯一可写的新行.
    change->set_uuid(key);
    if (!record) {
        change->mutable_erase();
    } else if (data) {
        change->set_data(record->data->data(), record->data->size());
    } else {
        auto* complete = change->mutable_record(); // reset/恢复/新注册必须携带 Attr 与 Data.
        complete->set_attr(record->attr->data(), record->attr->size());
        complete->set_data(record->data->data(), record->data->size());
    }
}
} // namespace astra
