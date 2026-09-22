#pragma once
#include "issuer.hpp"
#include "pulse.hpp"

namespace astra {
// Pulsar 进程服务组合, 独占登记, 对时监听及其共享身份和账本资源.
class Server {
public:
    // Pulsar 专属启动参数, 与 Star 的 astra::Config 分开校验.
    struct Config {
        // 两个必填的具体 IP:PORT. 允许端口 0 分配, 不允许通配 IP, 证书必须包含相应 IP SAN.
        Endpoint listen, pulse_listen;
        // 唯一 Galaxy, 必须是 1..64 字节安全 ASCII 名称.
        std::string galaxy;
        // 既有 TLS/签名/账号格式的目录, 默认 identity, 路径上限 4096 字节.
        std::filesystem::path identity = "identity";
        // 专用 SQLite 文件, 默认 state/pulsar.db; 不接受旧 journal/bbolt, 父目录须已存在.
        std::filesystem::path state = "state/pulsar.db";
        // 默认仅恢复已有库. true 显式初始化新群组, 遇已有文件拒绝覆盖.
        bool initialize = false;
        // 每角色最多 64 个当前成员, 允许 1..4096.
        std::size_t maximum = 64;
        // 最多 65536 条历史启动记录, 允许 1..1000000. 达限必须显式维护, 不静默忘掉旧请求.
        std::size_t starts = 65536;
        // 纯解析配置, 不读身份文件或创建目录. 拒绝重复与未知选项.
        static Result<Config> parse(std::span<const std::string_view> arguments);
        // 返回固定的参数和单位说明, 不加载秘密.
        static std::string_view help();
    };

    // 加载身份并独占账本, 尚不监听. 构造失败不保留半初始化服务.
    explicit Server(Config config, Source::Provider provider = Source::sample);
    // 先停止两个服务并等待所有 handler 退出, 再释放账本及签发者.
    ~Server();
    // 禁止复制服务监听和线程的关闭责任.
    Server(const Server&) = delete;
    // 禁止覆盖持有活动 RPC 和账本的服务对象.
    Server& operator=(const Server&) = delete;
    // 启动 Pulse 后将其实际端点写入登记应答. 任一监听失败都回滚已经启动的监听.
    void start();
    // 幂等关闭两个 Server, 共用五秒 RPC 截止; 故障磁盘上的阻塞系统调用仍可能延迟 handler 退出.
    void stop();
    // 返回实际监听端点, start 成功后可用于诊断或测试.
    std::string admission_endpoint() const;
    std::string pulse_endpoint() const;
    // 只读当前公共时间和质量, 冷启动未校准时为空.
    std::optional<Clock::Reading> time() const;

private:
    // 拥有配置, ephemeral 端口会在成功监听后替换.
    Config config_;
    // 声明顺序保证服务比所借用的材料和存储先析构.
    std::unique_ptr<Authority> authority_;
    // 独占持久成员账本, 先于引用它的 Issuer/Pulse 构造, 晚于它们销毁.
    std::unique_ptr<Ledger> ledger_;
    // 连续物理参考及采样线程, RPC 全部退出后才销毁.
    std::unique_ptr<Source> clock_;
    // 异步四时间戳服务, 借用 authority_,ledger_ 与 clock_.
    std::unique_ptr<Pulse> pulse_;
    // 登记处理器, 在 Pulse 确定实际监听端口后创建.
    std::unique_ptr<Issuer> issuer_;
    // 分别拥有对时和登记监听, 初始为空; stop 等待两端所有 handler 退出后清空.
    std::unique_ptr<grpc::Server> pulse_server_, admission_server_;
};
} // namespace astra
