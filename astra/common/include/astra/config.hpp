// 功能: 定义节点运行配置及 CLI 解析入口, 集中说明参数单位, 边界和内部资源预算.
// 本文件描述了节点启动和运行时所需的完整配置集合，并提供对这些配置的解析校验能力。
#pragma once

#include <astra/types.hpp>

#include <filesystem>
#include <span>

namespace astra {

// 生产入口使用 parse_options 完成校验后取得配置. 直接构造不会自动校验, 内部预算须保持下述约束.
// Config 结构体汇集了运行一个节点所需的所有动态参数和静态阈值。
struct Config {
    // role: 本进程角色, 由 Star/Planet 入口指定, CLI 不接受 --role. 
    // 值默认初始化为 star, 框架不会自动推断角色发现。
    Role role{};
    
    // cluster: 所属 Galaxy 标识, --galaxy 命令行参数必填, 无可用空默认值;
    // 限制条件为 1..64 字节安全 ASCII, 必须与准入身份一致。
    std::string cluster;
    
    // group: 连接偏好分组, 对应 --group 选项，默认值为 "default"。
    // 限制条件为 1..64 字节安全 ASCII; Planet 将优先选择同组候选, 该项不作为鉴权凭据。
    std::string group = "default";
    
    // listen: 本地监听网络端点, 对应 --listen 选项，必填且只接受数值 IP:PORT;
    // 允许通配 IP（如 0.0.0.0）和 端口 0（表示由系统分配实际端口）。
    Endpoint listen;
    
    // advertise: 对外公布的可达网络端点, 对应 --advertise 选项。
    // 如果省略，则回退到 listen 的值。若为通配监听，必须显式指定可达端点;
    // 回退的端口 0 在绑定成功后被实际值替换, 显式端点不允许包含零端口或通配 IP。
    Endpoint advertise;
    
    // supervisor: Supervisor 的远程拨号地址, 对应 --super 选项，必填。
    // 支持 DNS 主机名或数值 IP，且必须提供非零端口; 该选项解析阶段不联网也不执行 DNS 查询。
    std::string supervisor;
    
    // identity: 存储身份凭证材料的本地目录, 对应 --identity 选项。
    // 默认值为相对当前工作目录的 "identity", 路径长度须在 1..4096 字节。
    // 运行时该目录被用来读取 ca.pem, cert.pem, key.pem, admission.pub 和 login.json,
    // 在配置解析阶段本身不会主动读取这些秘密文件。
    std::filesystem::path identity = "identity";
    
    // max_members: 每种角色的成员连接容量限制, 对应 --max-members 选项。
    // 默认值为 64, 允许范围是 1..4096, 零为无效值。
    // Star 节点的容量计算中包含自身, Planet 的入站成员计数是分开计算的;
    // 值得注意的是，此值不改变 Planet 最多仅维持 8 个上游候选的逻辑限制。
    std::size_t max_members = 64;
    
    // max_frame_bytes: 声明在 Hello 协议握手中的最大接收帧字节数。
    // 内部写死默认值为 1 MiB, 暂无对外 CLI 选项; 协议要求有效声明至少为 1024 字节。
    // 当前控制帧的实际限制仍然是 min(4096, 本端声明, 远端声明), 需要指出该值既不是发送队列的容量，也不是 HTTP/2 的窗口大小。
    std::uint32_t max_frame_bytes = 1024 * 1024;
    
    // connect_timeout: 建立底层连接直到等待 TLS 通道就绪的时间预算。
    // 内部默认 3000 ms (3秒), 必须大于 0; 实际的连接截止时间会取此预算与握手总截止时间 (handshake_timeout) 间的较早者。
    Milliseconds connect_timeout{3000};
    
    // handshake_timeout: 建立连接以及完成准入/Hello 阶段的整体总时间预算。
    // 内部默认 5000 ms (5秒), 必须大于 0。
    // 准入过程中的连接等待和 Register 请求共用这一尝试截止时间; 这个值也用于服务端接纳连接时的 TLS 握手限制时间。
    Milliseconds handshake_timeout{5000};
    
    // heartbeat_interval: Hello 握手安装成功或在收到匹配的 Pong 回应后，距离下次发送 Ping 的心跳间隔。
    // 对应 --heartbeat-interval-ms 选项，默认 10000 ms (10秒), 允许范围 1..86400000 毫秒。
    // 零并不能表示禁用心跳。
    Milliseconds heartbeat_interval{10000};
    
    // pong_timeout: 发出 Ping 入队、实际网络写入以及等待收到匹配 Pong 的总体时间预算。
    // 这同时限制了待发 Pong 回应的排队和网络写入操作。
    // 对应 --pong-timeout-ms 选项，默认 5000 ms (5秒), 允许范围 1..86400000 毫秒; 逾期将直接关闭会话, 零为无效值。
    Milliseconds pong_timeout{5000};
    
    // reconnect_min: 连接断开后进行重连退避策略时的下限延迟。
    // 内部默认 100 ms, 应大于 0 且不能大于 reconnect_max; 当首次发生失败时即进入此基础退避延迟。
    Milliseconds reconnect_min{100};
    
    // reconnect_max: 连接断开重试的退避策略上限延迟。
    // 内部默认 5000 ms (5秒), 应不小于 reconnect_min 的值。
    // 发生连续失败时的指数增长和随机抖动时长均受到此上限约束; 它同时也被当作 Planet 角色执行两次候选上游刷新之间的最短时间间隔。
    Milliseconds reconnect_max{5000};
    
    // stable_connection: 一个建立的连接被系统认为是"稳定"所需要维持的持续时间阈值。
    // 内部默认 5000 ms (5秒), 应大于 0。
    // 该时间从角色层面上成功安装会话时开始计时; 当连接断开时，若连接时长达到该阈值，则重置失败次数重新从 reconnect_min 退避，
    // 否则认为连接不够稳定，进而延续累计失败次数来计算下一次更长的退避。
    Milliseconds stable_connection{5000};
    
    // shutdown_timeout: 停止服务时关闭准入、处理中的会话和监听器所共享的优雅退出总时间预算。
    // 对应 --shutdown-timeout-seconds 选项，默认 5 秒, 允许范围 1..60 秒。
    // 在配置对象中以毫秒级存储，默认为 5000 ms; 如果会话的关闭回调经过此截止时间仍未排空，进程将以失败码立即强制退出。
    Milliseconds shutdown_timeout{5000};
    
    // status_interval: 周期性向标准输出打印当前连接快照状态的时间间隔。
    // 对应 --status-interval-seconds 选项，默认 0 表示完全禁用该周期性输出; 若非零，范围限制为 1..3600 秒。
    // 从 CLI 读取到的秒数会转换成毫秒存储; 它输出的是当前连接的状态快照，仅代表连接层面，不代表上层业务数据已经就绪。
    Milliseconds status_interval{0};
    
    // max_inbound: 系统允许的尚未回收的入站 RPC 总数量容量限制，包含正在进行握手的会话。
    // 默认为 128 个。
    // 在 CLI 参数解析时，会被内部强制设为 max_members * 2，没有独立的 CLI 选项。
    // 达到该超额阈值时会立即拒绝新的入站连接，直到原会话发生 OnDone 后彻底回收才会释放相关槽位。
    std::size_t max_inbound = 128;
    
    // max_dials: 允许系统并行执行出站建连尝试的总数量预算。
    // 内部默认 4 个, 应至少为 1; 需要注意，已经完成 Hello 过程成功建立的连接并不占用此临时尝试预算。
    // 运行时控制循环每轮最多只会发起一次拨号，且两次发起动作之间至少间隔 250 ms 以防拥塞, 没有独立 CLI 选项。
    std::size_t max_dials = 4;

    // max_admission_request_bytes: 连接 Supervisor 发起准入请求时，最大允许发送的消息字节数。
    // 对应 --max-admission-request-bytes 选项，默认 4096 (4KB)。极小的封顶足以拦截异常大包发送。
    std::uint32_t max_admission_request_bytes = 4 * 1024;
    
    // max_admission_response_bytes: 接收 Supervisor 准入响应时，最大允许接收的消息字节数。
    // 对应 --max-admission-response-bytes 选项，默认 2097152 (2MB)。足以容纳系统硬上限的拓扑名单，防止控制面 OOM。
    std::uint32_t max_admission_response_bytes = 2 * 1024 * 1024;

    // 解析命令行参数为 Config 配置对象实体。
    // 参数字符串数组 arguments 仅在调用期间被借用; 当成功时返回一个独立的 Config 副本, 
    // 过程中不会读入身份密钥文件, 也不会调用系统 API 创建实际 socket。
    // arguments 不应包含可执行文件名称; role 由启动入口直接确定;
    // 任何存在参数缺失, 重复提供, 存在未知参数或超出允许范围的情况均返回 configuration 类别的错误。
    // 参数 arguments: 命令行参数视图数组。
    // 参数 role: 本进程的角色类型。
    // 返回: 成功返回装载好的 Config，失败返回 Error。
    static Result<Config> parse(std::span<const std::string_view> arguments, Role role);

    // 生成帮助文档的纯文本字符串。
    // 帮助信息来自同一字段上的编译期注解和默认值，不依赖维护多个平行的选项硬编码列表; 输出内容绝不会包含任何账号密码等敏感材料。
    // role 角色参数决定了帮助说明中的程序调用名; 返回的是独立构造的文本对象，执行中不解析参数也不访问文件系统。
    // 参数 role: 进程的角色，决定程序名。
    // 返回: 完整的帮助文档文本。
    static std::string help(Role role);

    // 解析并格式化 Supervisor 服务的网络地址。
    // Supervisor 允许 DNS 主机, 只校验名称/端口格式, 返回规范拨号文本.
    // value 在调用期间借用, 非零端口及主机格式不合法返回 configuration; DNS 主机文本保留原大小写.
    // 参数 value: 待解析的地址字符串。
    // 返回: 如果成功，返回标准化的可直接用于拨号的地址字符串，否则返回错误信息。
    static Result<std::string> format_supervisor(std::string_view value);
};

} // namespace astra
