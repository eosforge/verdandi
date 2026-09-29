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

namespace comet {
void Observer::stop() noexcept {
    close();
}

Result<std::optional<Observer::Item>> Observer::choose(void* context, std::optional<Item> (*selector)(void*, const Pool&)) const {

    const auto owner = observing_;
    if (!owner) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    auto pool = owner->pool();
    if (!pool) {
        return std::unexpected(pool.error());
    }
    auto selected = selector(context, *pool); // 同步应用代码, 没有 SDK 锁, 抛异常直接由调用方处理.
    if (owner->load().state() == State::closed) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    if (selected && selected->owner_.lock() != owner) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    return selected;
}

Result<void> Observer::Item::update(std::vector<std::uint8_t> data) {
    if (data.size() > 1024 * 1024) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    return update(std::make_shared<const std::vector<std::uint8_t>>(std::move(data)));
}

Result<void> Observer::Item::update(Value data) {

    if (!data || data->size() > 1024 * 1024) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    const auto owner = owner_.lock();
    if (!owner) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    auto result = owner->estimate(id_, authority_, record_.data, data);
    if (result) {
        record_.data = std::move(data);
    }
    return result;
}
} // namespace comet
