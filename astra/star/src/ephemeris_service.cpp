#include "ephemeris_service.hpp"
#include "ephemeris_feed.hpp"
#include <cstring>

namespace astra {
Ephemeris::Service::Service(State& state, Gateway& gateway, std::ptrdiff_t maximum) : Service(state, gateway, [] {}, maximum) {}

Ephemeris::Service::Service(State& state, Gateway& gateway, std::function<void()> wake, std::ptrdiff_t maximum) : state_(state), gateway_(gateway), slots_(maximum >= 1 && maximum <= 65536 ? maximum : throw std::invalid_argument("Invalid Ephemeris concurrency")), feed_(std::make_unique<Feed>(state, gateway, std::move(wake))) {}

Ephemeris::Service::~Service() = default;

void Ephemeris::Service::pump(std::chrono::steady_clock::time_point now, std::size_t maximum) {
    try {
        state_.tick(); // 一个共享控制循环推进整个自有来源, 不逐 Watch 另设定时器.
    } catch (...) {
        feed_->invalidate(); // 到期准备失败不能继续宣称对外视图已经追平, 保留已完整提交的状态供恢复.
    }
    feed_->pump(now, maximum);
}

void Ephemeris::Service::stop() noexcept {
    feed_->stop();
}

bool Ephemeris::Service::empty() const {
    return feed_->empty();
}

Downstream<Ephemeris>::Delivery Ephemeris::Service::delivery() const {
    return feed_->delivery();
}

grpc::ServerWriteReactor<proto::comet::v1::EphemerisWatchReply>* Ephemeris::Service::Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request) {
    return feed_->Watch(context, request);
}

Ephemeris::Value Ephemeris::Service::copy(const std::string& value) {
    auto buffer = std::make_shared<Buffer>(value.size()); // 唯一可写对象只在这里创建, 返回后不存在可写别名.
    if (!value.empty()) {
        std::memcpy(buffer->data(), value.data(), value.size());
    }
    return buffer;
}

std::expected<std::optional<Access::Permit>, grpc::Status> Ephemeris::Service::enter(grpc::CallbackServerContext& context, std::string_view instance, const proto::comet::v1::Scope& scope, bool initial) const {

    auto permit = gateway_.enter(context); // 先做一次逻辑身份检查, 生命周期覆盖全部原生提交.
    if (!permit) {
        return std::unexpected(permit.error());
    }
    if ((!initial && instance.empty()) || (!instance.empty() && instance != gateway_.instance())) {
        return std::unexpected(gateway_.error(context, grpc::StatusCode::FAILED_PRECONDITION, proto::comet::v1::REASON_INSTANCE, "Unexpected Star instance"));
    }
    if (auto checked = address(context, scope); !checked.ok()) {
        return std::unexpected(std::move(checked));
    }
    if (context.IsCancelled()) {
        return std::unexpected(gateway_.error(context, grpc::StatusCode::CANCELLED, proto::comet::v1::REASON_BUSY, "Request cancelled before admission"));
    }
    return permit;
}

grpc::Status Ephemeris::Service::address(grpc::CallbackServerContext& context, const proto::comet::v1::Scope& scope) const {
    if (!Scope::text(scope.sector(), 128) || !Scope::text(scope.spectrum(), 128)) {
        return gateway_.error(context, grpc::StatusCode::INVALID_ARGUMENT, proto::comet::v1::REASON_INPUT, "Invalid Ephemeris scope");
    }
    if (scope.sector().starts_with("__")) {
        return gateway_.error(context, grpc::StatusCode::PERMISSION_DENIED, proto::comet::v1::REASON_DENIED, "Internal scopes are not public");
    }
    return grpc::Status::OK;
}

grpc::Status Ephemeris::Service::error(grpc::CallbackServerContext& context, State::Error value) const {

    using Code = grpc::StatusCode;
    using Reason = proto::comet::v1::Reason;
    Code code = Code::INVALID_ARGUMENT; // 默认非法原生输入, 不根据错误字符串猜测原因.
    Reason reason = proto::comet::v1::REASON_INPUT;
    switch (value) {
    case State::Error::input:
        break;
    case State::Error::clock:
        code = Code::UNAVAILABLE;
        reason = proto::comet::v1::REASON_CLOCK;
        break;
    case State::Error::ended:
        code = Code::NOT_FOUND;
        reason = proto::comet::v1::REASON_ENDED;
        break;
    case State::Error::obsolete:
        code = Code::FAILED_PRECONDITION;
        reason = proto::comet::v1::REASON_OBSOLETE;
        break;
    case State::Error::conflict:
        code = Code::INVALID_ARGUMENT;
        reason = proto::comet::v1::REASON_INPUT;
        break;
    case State::Error::exhausted:
        code = Code::OUT_OF_RANGE;
        reason = proto::comet::v1::REASON_LIMIT;
        break;
    case State::Error::capacity:
        code = Code::RESOURCE_EXHAUSTED;
        reason = proto::comet::v1::REASON_LIMIT;
        break;
    case State::Error::history:
        code = Code::FAILED_PRECONDITION;
        reason = proto::comet::v1::REASON_HISTORY;
        break;
    }
    return gateway_.error(context, code, reason, "Ephemeris request was not applied");
}

grpc::Status Ephemeris::Service::ttl(grpc::CallbackServerContext& context) const {

    proto::comet::v1::Failure failure; // 一份有界的静态拒绝细节, 不先写另一份重复 metadata.
    failure.set_reason(proto::comet::v1::REASON_INPUT);
    failure.set_effect(proto::comet::v1::EFFECT_UNAPPLIED);
    failure.set_ttl_min_ms(1000);
    failure.set_ttl_max_ms(600000);
    failure.set_instance(gateway_.instance());
    context.AddTrailingMetadata("comet-error-bin", failure.SerializeAsString());
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TTL must be between 1000 and 600000 milliseconds");
}

grpc::ServerUnaryReactor* Ephemeris::Service::Create(grpc::CallbackServerContext* context, const proto::comet::v1::CreateRequest* request, proto::comet::v1::CreateReply* reply) {
    return execute(*context, [&] {
        // 接入先检查字段上限, 大载荷复制在最后许可之前完成, 不让撤销等待正文准备.
        if (auto checked = address(*context, request->scope()); !checked.ok()) {
            return checked;
        }
        if (request->attr().size() > 1024 * 1024 || request->data().size() > 1024 * 1024) {
            return error(*context, State::Error::capacity);
        }
        if (request->ttl_ms() < 1000 || request->ttl_ms() > 600000) {
            return ttl(*context);
        }
        auto attr = copy(request->attr());
        auto data = copy(request->data());
        reply->set_instance(gateway_.instance());
        auto* uuid = reply->mutable_uuid(); // string 对象分配在提交前完成, 成功后只交换 UUID 存储.
        const Scope scope{request->scope().sector(), request->scope().spectrum()};
        auto permit = enter(*context, request->instance(), request->scope(), true);
        if (!permit) {
            return permit.error();
        }

        auto result = state_.create(scope, std::move(attr), std::move(data), request->ttl_ms());
        if (!result) {
            return error(*context, result.error());
        }
        uuid->swap(result->uuid);
        reply->set_ttl_ms(result->ttl);
        return grpc::Status::OK;
    });
}

grpc::ServerUnaryReactor* Ephemeris::Service::Update(grpc::CallbackServerContext* context, const proto::comet::v1::UpdateRequest* request, proto::comet::v1::UpdateReply* reply) {
    return execute(*context, [&] {
        if (auto checked = address(*context, request->scope()); !checked.ok()) {
            return checked;
        }
        if (request->data().size() > 1024 * 1024) {
            return error(*context, State::Error::capacity);
        }
        if (!Ephemeris::valid(request->uuid()) || request->order() == 0) {
            return error(*context, State::Error::input);
        }
        auto data = copy(request->data()); // 此次调用唯一载荷复制, 所有后续状态/历史共用不可变引用.
        const Scope scope{request->scope().sector(), request->scope().spectrum()};
        auto permit = enter(*context, request->instance(), request->scope(), false);
        if (!permit) {
            return permit.error();
        }

        auto result = state_.update(scope, request->uuid(), std::move(data), request->order());
        if (!result) {
            return error(*context, result.error());
        }
        reply->set_order(request->order());
        return grpc::Status::OK;
    });
}

grpc::ServerUnaryReactor* Ephemeris::Service::Renew(grpc::CallbackServerContext* context, const proto::comet::v1::RenewRequest* request, proto::comet::v1::RenewReply* reply) {
    return execute(*context, [&] {
        if (auto checked = address(*context, request->scope()); !checked.ok()) {
            return checked;
        }
        if (!Ephemeris::valid(request->uuid()) || request->order() == 0) {
            return error(*context, State::Error::input);
        }
        const Scope scope{request->scope().sector(), request->scope().spectrum()};
        auto permit = enter(*context, request->instance(), request->scope(), false);
        if (!permit) {
            return permit.error();
        }
        auto result = state_.renew(scope, request->uuid(), request->order());
        if (!result) {
            return error(*context, result.error());
        }
        reply->set_order(request->order());
        return grpc::Status::OK;
    });
}

grpc::ServerUnaryReactor* Ephemeris::Service::Remove(grpc::CallbackServerContext* context, const proto::comet::v1::RemoveRequest* request, proto::comet::v1::Empty*) {
    return execute(*context, [&] {
        if (auto checked = address(*context, request->scope()); !checked.ok()) {
            return checked;
        }
        if (!Ephemeris::valid(request->uuid())) {
            return error(*context, State::Error::input);
        }
        const Scope scope{request->scope().sector(), request->scope().spectrum()};
        auto permit = enter(*context, request->instance(), request->scope(), false);
        if (!permit) {
            return permit.error();
        }
        const auto result = state_.remove(scope, request->uuid());
        return result ? grpc::Status::OK : error(*context, result.error());
    });
}
} // namespace astra
