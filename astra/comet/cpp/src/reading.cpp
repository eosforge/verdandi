#include "reading.hpp"

namespace comet::detail {
template class Watching<Projection>; // 仅 Almanac 投影的网络实例, 公共头没有 gRPC 模板.
}

namespace comet {
// Reader 构造接管读取意图, 空指针表示已关闭.
// reading 为读取实现, 移动后原对象失效.
Reader::Reader(std::shared_ptr<detail::Reading> reading) : reading_(std::move(reading)) {}

// Reader 移动构造转移意图, 原对象不再拥有.
Reader::Reader(Reader&&) noexcept = default;

// Reader 移动赋值先关闭原意图再接管, 自赋值安全.
// other 为源对象.
Reader& Reader::operator=(Reader&& other) noexcept {
    if (this != &other) {
        close();
        reading_ = std::move(other.reading_);
    }
    return *this;
}

// Reader 析构关闭读取意图, 不等待在途页.
Reader::~Reader() {
    close();
}

// Reader::load 返回当前视图快照, 未读取返回空视图.
Reader::View Reader::load() const {
    return reading_ ? reading_->load() : View{};
}

// Reader::close 关闭读取, 幂等, 不等待控制轮.
void Reader::close() noexcept {
    if (reading_) {
        reading_->close();
    }
}

// Reader::wait 等待清理完成, 超时返回 false, 回调内禁止等待.
bool Reader::wait(std::chrono::milliseconds timeout) const {
    return !reading_ || reading_->wait(timeout);
}
} // namespace comet
