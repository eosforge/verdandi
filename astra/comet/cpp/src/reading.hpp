#pragma once
#include "projection.hpp"
#include "watching.hpp"

namespace comet::detail {
extern template class Watching<Projection>; // 私有网络核心只在 reading.cpp 实例化.
}
