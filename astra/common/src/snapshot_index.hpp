#pragma once
#include "pages.hpp"
#include <astra/clock.hpp>

namespace astra {
// 既有 KV/Almanac 使用的单值行, 不用它伪装原生 Catalog 或 Ephemeris.
struct Cell {
    // 不可变字节所有权, 空指针表示内部无有效载荷; 合法零字节值仍非空.
    std::shared_ptr<const std::vector<std::uint8_t>> value;
    // 固定 Unix 截止, 空表示永久; Almanac 始终为空.
    std::optional<Clock::Time> deadline{};
};

// 保留既有 Index::Record/Buffer/Value/View API, 页算法同时供真实的原生记录使用.
using Index = Pages<Cell>;
} // namespace astra
