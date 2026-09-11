// 许可证: MIT, 详见仓库根目录 LICENSE.
#pragma once

#include <verdandi/peer/types.hpp>

#include <filesystem>
#include <span>

namespace verdandi::peer {

// 运行配置只在 CLI 校验完成后构造. 角色由二进制入口指定, 不接受 --role.
struct Config {
    Role role{};
    std::string cluster;
    std::string group = "default";
    Endpoint listen;
    Endpoint advertise;
    std::string supervisor;
    std::filesystem::path identity = "identity";
    // 容量包含自身, 默认 64, 范围 1..4096; Star 与 Planet 分别计数.
    std::size_t max_peers = 64;
    // 控制消息 4096 字节, Hello 声明保持 1 MiB, 不代表应用队列或 HTTP2 窗口.
    std::uint32_t max_frame_bytes = 1024 * 1024;
    Milliseconds connect_timeout{3000};
    Milliseconds handshake_timeout{5000};
    Milliseconds heartbeat_interval{10000};
    Milliseconds pong_timeout{5000};
    Milliseconds reconnect_min{100};
    Milliseconds reconnect_max{5000};
    Milliseconds stable_connection{5000};
    Milliseconds shutdown_timeout{5000};
    // 默认零禁用周期状态输出; 非零范围 1..3600 秒.
    Milliseconds status_interval{0};
    std::size_t max_inbound = 128;
    std::size_t max_dials = 4;
};

// 参数字符串仅在调用期间借用; 成功配置拥有副本, 不读身份文件、不创建 socket.
Result<Config> parse_options(std::span<const std::string_view> arguments, Role role);
// 帮助来自同一字段注解和默认值, 不维护平行选项列表; 输出不含账号材料.
std::string option_help(Role role);

} // namespace verdandi::peer
