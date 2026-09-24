#include "observing.hpp"

namespace comet::detail {
template class Watching<Selection>; // 复用相同网络生命周期, 按 Ephemeris 完整注册安装.
}

namespace comet {
// Observer 构造接管观察意图, 空指针表示已关闭.
// observing 为观察实现, 移动后原对象失效.
Observer::Observer(std::shared_ptr<detail::Observing> observing) : observing_(std::move(observing)) {}

// Observer 移动构造转移意图, 原对象不再拥有.
Observer::Observer(Observer&&) noexcept = default;

// Observer 移动赋值先关闭原意图再接管, 自赋值安全.
// other 为源对象.
Observer& Observer::operator=(Observer&& other) noexcept {
    if (this != &other) {
        close();
        observing_ = std::move(other.observing_);
    }
    return *this;
}

// Observer 析构关闭观察意图, 不等待在途页.
Observer::~Observer() {
    close();
}

// Observer::select 返回当前视图快照, 未观察返回空视图.
Observer::View Observer::select() const {
    return observing_ ? observing_->load() : View{};
}

// Observer::close 关闭观察, 幂等, 不等待控制轮.
void Observer::close() noexcept {
    if (observing_) {
        observing_->close();
    }
}

// Observer::wait 等待清理完成, 超时返回 false, 回调内禁止等待.
bool Observer::wait(std::chrono::milliseconds timeout) const {
    return !observing_ || observing_->wait(timeout);
}
} // namespace comet
