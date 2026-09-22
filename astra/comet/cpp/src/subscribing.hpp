#pragma once
#include "subscription.hpp"
#include "watching.hpp"

namespace comet::detail {
extern template class Watching<Subscription>; // 私有网络核心只在 subscribing.cpp 实例化.
}
