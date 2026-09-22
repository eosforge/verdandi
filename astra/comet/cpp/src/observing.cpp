#include "observing.hpp"

namespace comet::detail {
template class Watching<Selection>; // 复用相同网络生命周期, 按 Ephemeris 完整注册安装.
}

namespace comet {
Observer::Observer(std::shared_ptr<detail::Observing> observing) : observing_(std::move(observing)) {}

Observer::Observer(Observer&&) noexcept = default;

Observer& Observer::operator=(Observer&& other) noexcept {
    if (this != &other) {
        close();
        observing_ = std::move(other.observing_);
    }
    return *this;
}

Observer::~Observer() {
    close();
}

Observer::View Observer::select() const {
    return observing_ ? observing_->load() : View{};
}

void Observer::close() noexcept {
    if (observing_) {
        observing_->close();
    }
}

bool Observer::wait(std::chrono::milliseconds timeout) const {
    return !observing_ || observing_->wait(timeout);
}
} // namespace comet
