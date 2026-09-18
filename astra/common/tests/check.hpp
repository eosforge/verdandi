#pragma once

#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

namespace astra::test {
// Release 与 Debug 使用同一断言, 不受 NDEBUG 影响. 位置取自调用者, 失败保留表达式及文件行号.
inline void check(bool condition, std::string_view expression, std::source_location location = std::source_location::current()) {

    if (!condition) {
        throw std::runtime_error(std::string(location.file_name()) + ':' + std::to_string(location.line()) + ": " + std::string(expression));
    }
}
} // namespace astra::test

// 宏只保留表达式文本, 其值恰好计算一次; static_cast 支持 expected 和智能指针的显式 bool 转换.
#define CHECK(condition) ::astra::test::check(static_cast<bool>(condition), #condition)
