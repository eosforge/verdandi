#include "reading.hpp"

namespace comet::detail {
template class Watching<Projection>; // 仅 Almanac 投影的网络实例, 公共头没有 gRPC 模板.
}

namespace comet {
Reader::Reader(std::shared_ptr<detail::Reading> reading) : reading_(std::move(reading)) {}

Reader::Reader(Reader&&) noexcept = default;

Reader& Reader::operator=(Reader&& other) noexcept {
    if (this != &other) {
        close();
        reading_ = std::move(other.reading_);
    }
    return *this;
}

Reader::~Reader() {
    close();
}

Reader::View Reader::load() const {
    return reading_ ? reading_->load() : View{};
}

void Reader::close() noexcept {
    if (reading_) {
        reading_->close();
    }
}

bool Reader::wait(std::chrono::milliseconds timeout) const {
    return !reading_ || reading_->wait(timeout);
}
} // namespace comet
