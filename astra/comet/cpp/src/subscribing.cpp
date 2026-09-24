#include "subscribing.hpp"

namespace comet::detail {
template class Watching<Subscription>; // 仅 Catalog 投影的网络实例, 公共头没有 gRPC 模板.
}

namespace comet {
// Subscriber 构造接管订阅意图, 空指针表示已关闭.
// subscribing 为订阅实现, 移动后原对象失效.
Subscriber::Subscriber(std::shared_ptr<detail::Subscribing> subscribing) : subscribing_(std::move(subscribing)) {}

// Subscriber 移动构造转移意图, 原对象不再拥有.
Subscriber::Subscriber(Subscriber&&) noexcept = default;

// Subscriber 移动赋值先关闭原意图再接管, 自赋值安全.
// other 为源对象, 移动后失效.
Subscriber& Subscriber::operator=(Subscriber&& other) noexcept {
    if (this != &other) {
        close();
        subscribing_ = std::move(other.subscribing_);
    }
    return *this;
}

// Subscriber 析构关闭订阅意图, 不等待在途页.
Subscriber::~Subscriber() {
    close();
}

// Subscriber::watch 返回当前视图快照, 未订阅返回空视图.
Subscriber::View Subscriber::watch() const {
    return subscribing_ ? subscribing_->load() : View{};
}

// Subscriber::close 关闭订阅, 幂等, 不等待控制轮.
void Subscriber::close() noexcept {
    if (subscribing_) {
        subscribing_->close();
    }
}

// Subscriber::wait 等待清理完成, 超时返回 false, 回调内禁止等待.
bool Subscriber::wait(std::chrono::milliseconds timeout) const {
    return !subscribing_ || subscribing_->wait(timeout);
}
} // namespace comet
