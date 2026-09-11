// 许可证: MIT, 详见仓库根目录 LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace verdandi::peer::detail {
// 注解只描述当前 CLI 使用的字符串和无符号数值, 不构建通用配置框架.
struct Option {
    // 注解要求 structural type, 用内嵌字符数组保存文本, 不持有临时 string_view.
    char name[48]{};
    char description[160]{};
    std::uint64_t minimum = 0;
    std::uint64_t maximum = 0;
    bool required = false;
    consteval Option(std::string_view option_name, std::string_view help, std::uint64_t lower = 0, std::uint64_t upper = 0, bool mandatory = false)
        : minimum(lower), maximum(upper), required(mandatory) {
        if (option_name.size() >= sizeof(name) || help.size() >= sizeof(description)) {
            throw "CLI annotation text exceeds its compile-time capacity";
        }
        for (std::size_t i = 0; i < option_name.size(); ++i) {
            name[i] = option_name[i];
        }
        for (std::size_t i = 0; i < help.size(); ++i) {
            description[i] = help[i];
        }
    }
};

struct Options {
    // 每个选项的名称、范围、单位和说明仅在这里声明. 默认值保持字段初始化器语义.
    // clang-format 尚未识别反射注解语法, 只保护这些声明, 不禁用整个文件格式化.
    // clang-format off
    [[=Option{"cluster", "Galaxy identifier: 1..64 safe ASCII bytes", 0, 0, true}]] std::string cluster;
    [[=Option{"listen", "Local numeric IP:PORT; wildcard requires advertise", 0, 0, true}]] std::string listen;
    [[=Option{"super", "Supervisor HOST:PORT", 0, 0, true}]] std::string supervisor;
    [[=Option{"advertise", "Reachable numeric IP:PORT; defaults to bound listener"}]] std::string advertise;
    [[=Option{"identity", "Directory containing TLS, admission.pub and login.json"}]] std::string identity = "identity";
    [[=Option{"group", "Preferred connection group; 1..64 safe ASCII bytes"}]] std::string group = "default";
    [[=Option{"max-peers", "Capacity per role; Star capacity includes self", 1, 4096}]] std::uint64_t maximum = 64;
    [[=Option{"heartbeat-interval-ms", "Idle delay before Ping, milliseconds", 1, 86400000}]] std::uint64_t heartbeat = 10000;
    [[=Option{"pong-timeout-ms", "Ping queue/write/Pong total deadline, milliseconds", 1, 86400000}]] std::uint64_t pong = 5000;
    [[=Option{"shutdown-timeout-seconds", "Owned shutdown deadline, seconds", 1, 60}]] std::uint64_t shutdown = 5;
    [[=Option{"status-interval-seconds", "Actual-state snapshot interval; zero disables", 0, 3600}]] std::uint64_t status = 0;
    // clang-format on
};
} // namespace verdandi::peer::detail
