#include "catalog_edition.hpp"

namespace astra {
Catalog::State::Projection::Event Catalog::Encoding::merge(const State::Projection::Event& previous, State::Projection::Event current) noexcept {
    static_cast<void>(previous); // Catalog 每次增量都是完整正文, 不需要传播 Attr 基线提示.
    return current;
}

void Catalog::Encoding::append(Reply& page, std::string_view key, const Content* record, bool data) {
    static_cast<void>(data);           // 不将 Ephemeris 的 Attr 提示解释为 Catalog 内容变更.
    auto* change = page.add_changes(); // 本次消息唯一可写的新行.
    change->set_key(key);
    if (!record) {
        change->mutable_erase();
    } else {
        change->set_version(record->version);
        change->set_value(record->value->data(), record->value->size());
    }
}
} // namespace astra
