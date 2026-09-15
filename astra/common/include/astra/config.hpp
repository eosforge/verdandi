// 功能: 定义节点运行配置及 CLI 解析入口, 集中说明参数单位, 边界和内部资源预算.
#pragma once

#include <astra/types.hpp>

#include <filesystem>
#include <span>

namespace astra {

// 生产入口使用 parse_options 完成校验后取得配置. 直接构造不会自动校验, 内部预算须保持下述约束.
struct Config {
    // 本进程角色, 由 Star/Planet 入口设置, CLI 不接受 --role. 值初始化为 star, 不是自动角色发现.
    Role role{};
    // 所属 Galaxy 标识, --cluster 必填, 无可用空默认值; 1..64 字节安全 ASCII, 必须与准入身份一致.
    std::string cluster;
    // 连接偏好分组, --group 默认 default, 1..64 字节安全 ASCII; Planet 优先同组候选, 不作为鉴权凭据.
    std::string group = "default";
    // 本地监听端点, --listen 必填且只接受数值 IP:PORT; 允许通配 IP 和端口 0, 后者由系统分配实际端口.
    Endpoint listen;
    // 对外公布的可达数值 IP:PORT, --advertise 可省略并回退到 listen.
    // 通配监听必须显式指定可达端点; 回退的端口 0 在绑定成功后替换, 显式端点不允许零端口或通配 IP.
    Endpoint advertise;
    // Supervisor 拨号地址, --super 必填, 支持 DNS 或数值 IP 和非零端口; 解析阶段不联网也不执行 DNS.
    std::string supervisor;
    // 身份材料目录, --identity 默认相对当前工作目录的 identity, 路径长度 1..4096 字节.
    // 运行时读取 ca.pem, cert.pem, key.pem, admission.pub 和 login.json, 配置解析不读取这些秘密文件.
    std::filesystem::path identity = "identity";
    // 每种角色的成员容量, --max-members 默认 64, 范围 1..4096, 零无效.
    // Star 容量包含自身, Planet 入站成员另行计数; 此值不改变 Planet 最多 8 个上游候选的限制.
    std::size_t max_members = 64;
    // Hello 声明的最大帧字节数, 内部默认 1 MiB, 无 CLI 选项; 有效声明至少 1024 字节.
    // 当前控制帧仍限制为 min(4096, 本端声明, 远端声明), 此值不是发送队列容量或 HTTP/2 窗口.
    std::uint32_t max_frame_bytes = 1024 * 1024;
    // 等待 TLS 通道就绪的预算, 内部默认 3000 ms, 应大于 0; 实际截止取此预算与握手总截止的较早者.
    Milliseconds connect_timeout{3000};
    // 连接和准入/Hello 的总预算, 内部默认 5000 ms, 应大于 0.
    // 准入的连接等待和 Register 共享本次尝试截止; 此值也用于服务端 TLS 握手限制.
    Milliseconds handshake_timeout{5000};
    // Hello 安装成功或匹配 Pong 到达后, 距下次 Ping 的间隔.
    // --heartbeat-interval-ms 默认 10000 ms, 范围 1..86400000, 零不表示禁用心跳.
    Milliseconds heartbeat_interval{10000};
    // Ping 入队, 写入及收到匹配 Pong 的总预算, 同时限制待发 Pong 的排队和写入.
    // --pong-timeout-ms 默认 5000 ms, 范围 1..86400000; 逾期关闭会话, 零无效.
    Milliseconds pong_timeout{5000};
    // 重连退避的下限, 内部默认 100 ms, 应大于 0 且不大于 reconnect_max; 首次失败即进入退避.
    Milliseconds reconnect_min{100};
    // 重连退避的上限, 内部默认 5000 ms, 应不小于 reconnect_min.
    // 指数增长和抖动均受此上限约束; 同时用作 Planet 两次候选刷新之间的最短间隔.
    Milliseconds reconnect_max{5000};
    // 连接被视为稳定的持续时间, 内部默认 5000 ms, 应大于 0.
    // 从角色安装会话时计时; 断开时达到阈值则重新从首次失败退避, 否则延续失败次数.
    Milliseconds stable_connection{5000};
    // 关闭准入, 会话和监听器共享的总预算; --shutdown-timeout-seconds 默认 5 s, 范围 1..60.
    // 配置以毫秒保存, 默认 5000 ms; 会话回调超过截止仍未排空时进程以失败码立即退出.
    Milliseconds shutdown_timeout{5000};
    // 周期连接状态输出间隔, --status-interval-seconds 默认 0 表示禁用; 非零范围 1..3600 s.
    // CLI 秒数转为毫秒保存; 输出当前连接快照, 不代表业务数据已经就绪.
    Milliseconds status_interval{0};
    // 尚未回收的入站 RPC 总容量, 包含握手中会话, 默认 128 个.
    // CLI 解析时设为 max_members * 2, 无独立选项; 超额立即拒绝, 直到 OnDone 后回收才释放槽位.
    std::size_t max_inbound = 128;
    // 并行出站建连尝试预算, 内部默认 4 个, 应至少为 1; 已完成 Hello 的连接不占此预算.
    // 运行时每轮最多发起一次拨号, 发起后至少间隔 250 ms 再尝试, 无独立 CLI 选项.
    std::size_t max_dials = 4;
};

// 参数字符串仅在调用期间借用; 成功配置拥有副本, 不读身份文件, 不创建 socket.
// arguments 不含可执行文件名, role 由入口确定; 缺失, 重复, 未知或越界参数返回 configuration 错误.
Result<Config> parse_options(std::span<const std::string_view> arguments, Role role);
// 帮助来自同一字段注解和默认值, 不维护平行选项列表; 输出不含账号材料.
// role 决定帮助中的程序名; 返回独立文本, 不解析参数或访问文件系统.
std::string option_help(Role role);

} // namespace astra
