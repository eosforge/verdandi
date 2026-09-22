#pragma once
#include "selection.hpp"
#include "watching.hpp"

namespace comet::detail {
extern template class Watching<Selection>; // 私有网络核心只在 observing.cpp 实例化.
}
