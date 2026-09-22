#include "catalog_service.hpp"
#include "catalog_feed.hpp"
#include <cstring>

namespace astra {
Catalog::Service::Service(State& state, Gateway& gateway, std::ptrdiff_t maximum) : Service(state, gateway, [] {}, maximum) {}

Catalog::Service::Service(State& state, Gateway& gateway, std::function<void()> wake, std::ptrdiff_t maximum) : state_(state), gateway_(gateway), slots_(maximum >= 1 && maximum <= 65536 ? maximum : throw std::invalid_argument("Invalid Catalog concurrency")), feed_(std::make_unique<Feed>(state, gateway, std::move(wake))) {}

Catalog::Service::~Service() = default;

void Catalog::Service::pump(std::chrono::steady_clock::time_point now, std::size_t maximum) {
    try {
        state_.tick(); // 一个共享控制循环推进整个自有来源, 不逐 Watch 另设定时器.
    } catch (...) {
        feed_->invalidate(); // 到期准备失败不能继续宣称对外视图已经追平, 保留已完整提交的状态供恢复.
    }
    feed_->pump(now, maximum);
}

void Catalog::Service::stop() noexcept {
    feed_->stop();
}

bool Catalog::Service::empty() const {
    return feed_->empty();
}

grpc::ServerWriteReactor<proto::comet::v1::CatalogWatchReply>* Catalog::Service::Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request) {
    return feed_->Watch(context, request);
}

Catalog::Value Catalog::Service::copy(const std::string& value) {
    auto buffer = std::make_shared<Buffer>(value.size()); // 唯一可写对象只在这里创建, 返回后不存在可写别名.
    if (!value.empty()) {
        std::memcpy(buffer->data(), value.data(), value.size());
    }
    return buffer;
}

std::expected<std::optional<Access::Permit>, grpc::Status> Catalog::Service::enter(grpc::CallbackServerContext& context, std::string_view instance, const proto::comet::v1::Scope& scope, bool initial) const {

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

grpc::Status Catalog::Service::address(grpc::CallbackServerContext& context, const proto::comet::v1::Scope& scope) const {
    if (!Scope::text(scope.sector(), 128) || !Scope::text(scope.spectrum(), 128)) {
        return gateway_.error(context, grpc::StatusCode::INVALID_ARGUMENT, proto::comet::v1::REASON_INPUT, "Invalid Catalog scope");
    }
    if (scope.sector().starts_with("__")) {
        return gateway_.error(context, grpc::StatusCode::PERMISSION_DENIED, proto::comet::v1::REASON_DENIED, "Internal scopes are not public");
    }
    return grpc::Status::OK;
}

grpc::Status Catalog::Service::error(grpc::CallbackServerContext& context, State::Error value) const {

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
    case State::Error::version:
        code = Code::FAILED_PRECONDITION;
        reason = proto::comet::v1::REASON_VERSION;
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
    return gateway_.error(context, code, reason, "Catalog request was not applied");
}

grpc::Status Catalog::Service::ttl(grpc::CallbackServerContext& context) const {

    proto::comet::v1::Failure failure; // 一份有界的静态拒绝细节, 不先写另一份重复 metadata.
    failure.set_reason(proto::comet::v1::REASON_INPUT);
    failure.set_effect(proto::comet::v1::EFFECT_UNAPPLIED);
    failure.set_ttl_min_ms(1000);
    failure.set_ttl_max_ms(600000);
    failure.set_instance(gateway_.instance());
    context.AddTrailingMetadata("comet-error-bin", failure.SerializeAsString());
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TTL must be between 1000 and 600000 milliseconds");
}

grpc::ServerUnaryReactor* Catalog::Service::Publish(grpc::CallbackServerContext* context, const proto::comet::v1::PublishRequest* request, proto::comet::v1::PublishReply* reply) {
    return execute(*context, [&] {
        // 不复制超限字段, 通过静态契约后才准备唯一不可变载荷和小响应.
        if (auto checked = address(*context, request->scope()); !checked.ok()) {
            return checked;
        }
        if (!Scope::text(request->key(), 1024) || request->version() == 0) {
            return error(*context, State::Error::input);
        }
        if (request->value().size() > 1024 * 1024) {
            return error(*context, State::Error::capacity);
        }
        if (request->ttl_ms() < 1000 || request->ttl_ms() > 600000) {
            return ttl(*context);
        }
        auto value = copy(request->value());
        reply->set_instance(gateway_.instance());
        const Scope scope{request->scope().sector(), request->scope().spectrum()};
        auto permit = enter(*context, request->instance(), request->scope(), true);
        if (!permit) {
            return permit.error();
        }

        const auto result = state_.publish(scope, request->key(), std::move(value), request->version(), request->ttl_ms());
        if (!result) {
            return error(*context, result.error());
        }
        reply->set_version(request->version());
        return grpc::Status::OK;
    });
}

grpc::ServerUnaryReactor* Catalog::Service::Renew(grpc::CallbackServerContext* context, const proto::comet::v1::CatalogRenewRequest* request, proto::comet::v1::Empty*) {
    return execute(*context, [&] {
        if (auto checked = address(*context, request->scope()); !checked.ok()) {
            return checked;
        }
        if (!Scope::text(request->key(), 1024) || request->version() == 0) {
            return error(*context, State::Error::input);
        }
        if (request->ttl_ms() < 1000 || request->ttl_ms() > 600000) {
            return ttl(*context);
        }
        const Scope scope{request->scope().sector(), request->scope().spectrum()};
        auto permit = enter(*context, request->instance(), request->scope(), false);
        if (!permit) {
            return permit.error();
        }
        const auto result = state_.renew(scope, request->key(), request->version(), request->ttl_ms());
        return result ? grpc::Status::OK : error(*context, result.error());
    });
}
} // namespace astra
