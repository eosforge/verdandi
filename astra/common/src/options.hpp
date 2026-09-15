// 功能: 声明 CLI 字段及编译期元数据, 作为选项名称, 默认值, 范围和帮助的单一来源.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace astra::detail {
// 注解只描述当前 CLI 使用的字符串和无符号数值, 不构建通用配置框架.
struct Option {
    // 注解要求 structural type, 用内嵌字符数组保存文本, 不持有临时 string_view.
    // CLI 长选项名, 不含前导 --; 最多 47 字节加 NUL, 零初始化保留终止符.
    char name[48]{};
    // 帮助说明, 最多 159 字节加 NUL; 内容必须是固定公开文本, 不包含身份材料.
    char description[160]{};
    // 数值选项的包含式下界, 默认 0; 字符串选项不使用此数值约束.
    std::uint64_t minimum = 0;
    // 数值选项的包含式上界, 默认 0; 字符串选项由解析器另行检查长度和业务格式.
    std::uint64_t maximum = 0;
    // 是否必须在 CLI 显式出现, 默认 false; true 时不能仅依赖字段默认值通过必填检查.
    bool required = false;
    // 在编译期复制 option_name/help, lower/upper 为包含式数值范围, mandatory 控制必填检查.
    // 文本超出内嵌数组时使常量求值失败; 不保留传入 string_view 的借用.
    consteval Option(std::string_view option_name, std::string_view help, std::uint64_t lower = 0, std::uint64_t upper = 0, bool mandatory = false)
        : minimum(lower), maximum(upper), required(mandatory) {
        if (option_name.size() >= sizeof(name) || help.size() >= sizeof(description)) {
            throw "CLI annotation text exceeds its compile-time capacity";
        }
        // 长度检查后复制文本, 未覆盖的零初始化尾部作为 NUL, 避免反射元数据借用临时缓冲.
        std::ranges::copy(option_name, name);
        std::ranges::copy(help, description);
    }
};

struct Options {
    // 每个选项的名称, 范围, 单位和说明仅在这里声明. 默认值保持字段初始化器语义.
    // clang-format 尚未识别反射注解语法, 只保护这些声明, 不禁用整个文件格式化.
    // clang-format off
    // Galaxy 标识原文, 必填且无空默认值; 解析后要求 1..64 字节安全 ASCII.
    [[=Option{"cluster", "Galaxy identifier: 1..64 safe ASCII bytes", 0, 0, true}]] std::string cluster;
    // 监听端点原文, 必填; 允许数值通配 IP 和端口 0, 不支持 DNS.
    [[=Option{"listen", "Local numeric IP:PORT; wildcard requires advertise", 0, 0, true}]] std::string listen;
    // Supervisor 拨号地址原文, 必填; 允许 DNS 或数值 IP, 必须提供非零端口.
    [[=Option{"super", "Supervisor HOST:PORT", 0, 0, true}]] std::string supervisor;
    // 公布端点原文, 默认空表示使用监听端点; 显式值必须可达且使用非零端口.
    [[=Option{"advertise", "Reachable numeric IP:PORT; defaults to bound listener"}]] std::string advertise;
    // 身份目录原文, 默认 identity; 相对当前工作目录解释, 本层不读取文件.
    [[=Option{"identity", "Directory containing TLS, admission.pub and login.json"}]] std::string identity = "identity";
    // 连接偏好分组, 默认 default; 1..64 字节安全 ASCII, 不是权限范围.
    [[=Option{"group", "Preferred connection group; 1..64 safe ASCII bytes"}]] std::string group = "default";
    // 每类成员容量, 默认 64 个, 范围 1..4096; 转换时同时派生入站 RPC 预算.
    [[=Option{"max-members", "Capacity per role; Star capacity includes self", 1, 4096}]] std::uint64_t maximum = 64;
    // 发送下一次 Ping 的等待间隔, 默认 10000 ms, 范围 1..86400000, 零无效.
    [[=Option{"heartbeat-interval-ms", "Idle delay before Ping, milliseconds", 1, 86400000}]] std::uint64_t heartbeat = 10000;
    // Ping 排队, 写入和等待匹配 Pong 的总预算, 默认 5000 ms, 范围 1..86400000.
    [[=Option{"pong-timeout-ms", "Ping queue/write/Pong total deadline, milliseconds", 1, 86400000}]] std::uint64_t pong = 5000;
    // 共享关闭预算, 默认 5 s, 范围 1..60; 生成 Config 时转换成毫秒.
    [[=Option{"shutdown-timeout-seconds", "Owned shutdown deadline, seconds", 1, 60}]] std::uint64_t shutdown = 5;
    // 周期状态输出间隔, 默认 0 禁用, 非零范围 1..3600 s; 生成 Config 时转换成毫秒.
    [[=Option{"status-interval-seconds", "Actual-state snapshot interval; zero disables", 0, 3600}]] std::uint64_t status = 0;
    // clang-format on
};
} // namespace astra::detail
