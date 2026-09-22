#include "subscribing.hpp"

namespace comet::detail {
template class Watching<Subscription>; // 仅 Catalog 投影的网络实例, 公共头没有 gRPC 模板.
}

namespace comet {
Subscriber::Subscriber(std::shared_ptr<detail::Subscribing> subscribing) : subscribing_(std::move(subscribing)) {}

Subscriber::Subscriber(Subscriber&&) noexcept = default;

Subscriber& Subscriber::operator=(Subscriber&& other) noexcept {
    if (this != &other) {
        close();
        subscribing_ = std::move(other.subscribing_);
    }
    return *this;
}

Subscriber::~Subscriber() {
    close();
}

Subscriber::View Subscriber::watch() const {
    return subscribing_ ? subscribing_->load() : View{};
}

void Subscriber::close() noexcept {
    if (subscribing_) {
        subscribing_->close();
    }
}

bool Subscriber::wait(std::chrono::milliseconds timeout) const {
    return !subscribing_ || subscribing_->wait(timeout);
}
} // namespace comet
