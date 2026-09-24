#include "parcel.hpp"
#include <limits>
#include <stdexcept>

namespace astra {
bool Parcel::valid(const proto::astra::v1::Scope& message) noexcept {
    return Scope::text(message.sector(), 128) && Scope::text(message.spectrum(), 128);
}

std::optional<Scope> Parcel::scope(const proto::astra::v1::Scope& message) {
    return valid(message) ? std::optional<Scope>(Scope{message.sector(), message.spectrum()}) : std::nullopt;
}

void Parcel::scope(proto::astra::v1::Scope& message, const Scope& value) {
    message.set_sector(value.sector);
    message.set_spectrum(value.spectrum);
}

std::optional<Clock::Time> Parcel::time(std::uint64_t value) noexcept {
    return value != 0 && value <= static_cast<std::uint64_t>(INT64_MAX) ? std::optional<Clock::Time>(Clock::Time(std::chrono::nanoseconds(value))) : std::nullopt;
}

Catalog::Value Parcel::value(const std::string& bytes) {
    return std::make_shared<const Catalog::Buffer>(bytes.begin(), bytes.end());
}

bool Parcel::valid(const proto::astra::v1::CatalogRecord& message) noexcept {
    return message.version() != 0 && (message.has_watermark() || (message.has_value() && message.value().value().size() <= 1024 * 1024 && time(message.value().deadline())));
}

bool Parcel::valid(const proto::astra::v1::EphemerisRecord& message) noexcept {
    return message.attr().size() <= 1024 * 1024 && message.data().size() <= 1024 * 1024 && message.ttl_ms() >= 1000 && message.ttl_ms() <= 600000 && time(message.deadline()).has_value();
}

void Parcel::record(proto::astra::v1::CatalogRecord& message, const Catalog::Record& value) {

    if (!Catalog::valid(value)) {
        throw std::logic_error("Invalid native Catalog record");
    }
    message.Clear(); // 每次完整赋值, 不泄漏复用对象的旧 oneof 或未来可选字段.
    message.set_version(value.version);
    if (value.value) {
        auto* body = message.mutable_value(); // 生成消息持有编码字节, 不借用原生 Buffer 的生命周期.
        body->set_value(value.value->data(), value.value->size());
        body->set_deadline(static_cast<std::uint64_t>(value.deadline->time_since_epoch().count()));
    } else {
        message.mutable_watermark();
    }
}

std::optional<Catalog::Record> Parcel::record(const proto::astra::v1::CatalogRecord& message) {

    if (!valid(message)) {
        return std::nullopt;
    }
    return message.has_watermark() ? Catalog::Record{message.version(), {}, {}} : Catalog::Record{message.version(), value(message.value().value()), time(message.value().deadline())};
}

void Parcel::record(proto::astra::v1::EphemerisRecord& message, const Ephemeris::Record& value) {

    if (!value.attr || !value.data || value.attr->size() > 1024 * 1024 || value.data->size() > 1024 * 1024 || value.ttl < 1000 || value.ttl > 600000 || value.deadline.time_since_epoch().count() <= 0) {
        throw std::logic_error("Invalid native Ephemeris record");
    }
    message.Clear();
    message.set_attr(value.attr->data(), value.attr->size());
    message.set_data(value.data->data(), value.data->size());
    message.set_deadline(static_cast<std::uint64_t>(value.deadline.time_since_epoch().count()));
    message.set_update(value.update);
    message.set_renewal(value.renewal);
    message.set_ttl_ms(value.ttl);
}

std::optional<Ephemeris::Record> Parcel::record(const proto::astra::v1::EphemerisRecord& message) {

    if (!valid(message)) {
        return std::nullopt;
    }
    return Ephemeris::Record{value(message.attr()), value(message.data()), *time(message.deadline()), message.update(), message.renewal(), message.ttl_ms()};
}

void Parcel::delta(proto::astra::v1::CatalogDelta& message, const Origin<Catalog::Record>::Event& event) {

    using Source = Origin<Catalog::Record>; // 只有完整发布与续期进入 Catalog 广播.
    if (!event.name || !event.name->scope || !event.name->scope->valid() || !Scope::text(event.name->key, 1024) || event.position == 0 || !event.record || !Catalog::valid(*event.record)) {
        throw std::logic_error("Invalid Catalog source event");
    }
    message.Clear();
    message.set_position(event.position);
    scope(*message.mutable_scope(), *event.name->scope);
    message.set_key(event.name->key);
    if (event.form == Source::Form::renew && event.record->deadline) {
        message.mutable_lease()->set_version(event.record->version);
        message.mutable_lease()->set_deadline(static_cast<std::uint64_t>(event.record->deadline->time_since_epoch().count()));
    } else if (event.form == Source::Form::record) {
        record(*message.mutable_record(), *event.record);
    } else {
        throw std::logic_error("Catalog source has an unsupported operation");
    }
}

void Parcel::delta(proto::astra::v1::EphemerisDelta& message, const Origin<Ephemeris::Record, true>::Event& event) {

    using Source = Origin<Ephemeris::Record, true>; // 根据事件形式只编码实际改变的原生字段.
    if (!event.name || !event.name->scope || !event.name->scope->valid() || !Ephemeris::valid(event.name->key) || event.position == 0 || (event.form == Source::Form::erase) != !event.record) {
        throw std::logic_error("Invalid Ephemeris source event");
    }
    message.Clear();
    message.set_position(event.position);
    scope(*message.mutable_scope(), *event.name->scope);
    message.set_uuid(event.name->key);
    switch (event.form) {
    case Source::Form::record:
        record(*message.mutable_record(), *event.record);
        break;
    case Source::Form::data:
        if (!event.record->data || event.record->update == 0) {
            throw std::logic_error("Invalid Ephemeris data event");
        }
        message.mutable_data()->set_data(event.record->data->data(), event.record->data->size());
        message.mutable_data()->set_order(event.record->update);
        break;
    case Source::Form::renew:
        if (event.record->renewal == 0 || event.record->deadline.time_since_epoch().count() <= 0) {
            throw std::logic_error("Invalid Ephemeris lease event");
        }
        message.mutable_lease()->set_deadline(static_cast<std::uint64_t>(event.record->deadline.time_since_epoch().count()));
        message.mutable_lease()->set_order(event.record->renewal);
        break;
    case Source::Form::erase:
        message.mutable_erase();
        break;
    }
}

bool Parcel::valid(const proto::astra::v1::CatalogChanges& message) noexcept {

    if (message.entries_size() > 256 || message.ByteSizeLong() > 8 * 1024 * 1024) {
        return false;
    }
    std::uint64_t previous{}; // 首项可以从任意正位置开始, 与当前前缀的连续性由安装器检查.
    for (const auto& entry : message.entries()) {
        if (!valid(entry.scope()) || !Scope::text(entry.key(), 1024) || entry.position() == 0 || entry.position() > message.head() || (previous && (previous == UINT64_MAX || entry.position() != previous + 1))) {
            return false;
        }
        if (!((entry.has_record() && valid(entry.record())) || (entry.has_lease() && entry.lease().version() != 0 && time(entry.lease().deadline())))) {
            return false;
        }
        previous = entry.position();
    }
    return true;
}

bool Parcel::valid(const proto::astra::v1::EphemerisChanges& message) noexcept {

    if (message.entries_size() > 256 || message.ByteSizeLong() > 8 * 1024 * 1024) {
        return false;
    }
    std::uint64_t previous{}; // 包内必须完整连续, 与另一个域的 position 无比较关系.
    for (const auto& entry : message.entries()) {
        if (!valid(entry.scope()) || !Ephemeris::valid(entry.uuid()) || entry.position() == 0 || entry.position() > message.head() || (previous && (previous == UINT64_MAX || entry.position() != previous + 1))) {
            return false;
        }
        if (!((entry.has_record() && valid(entry.record())) || (entry.has_data() && entry.data().order() != 0 && entry.data().data().size() <= 1024 * 1024) || (entry.has_lease() && entry.lease().order() != 0 && time(entry.lease().deadline())) || entry.has_erase())) {
            return false;
        }
        previous = entry.position();
    }
    return true;
}
} // namespace astra
