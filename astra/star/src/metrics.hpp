// 独立只读指标监听, 不注册业务 RPC, 不访问数据正文或认证材料.
#pragma once

#include <astra/types.hpp>
#include <memory>

namespace astra {

class Metrics {
public:
    // 固定基数的运行状态. 字节为逻辑恢复预算, 不是进程 RSS; 所有计数初始零.
    struct Sample {
        std::string instance;        // 已准入进程 ID, 空值表示尚未准入, 只进入响应头.
        bool ready{};                // 当前进程是否已开放业务.
        bool almanac{};              // 是否已安装首份完整权威基线.
        bool clock{};                // 本地连续纪元时间是否可用.
        bool synchronized{};         // 当前参考同步质量是否达标.
        std::uint64_t uncertainty{}; // 时钟估计不确定度, 纳秒; clock=false 时不作精度声明.
        std::size_t members{};       // 可信 Star 成员数, 包含自身.
        std::size_t sessions{};      // 已安装的双向对等会话数.
        std::size_t recovery{};      // 当前冻结基线与恢复候选占用的逻辑字节.
    };

    // 绑定数值端点并开始专用有界 HTTP 循环; 失败抛出, 已获取描述符由 RAII 关闭.
    explicit Metrics(const Endpoint& endpoint);
    // 请求停止并唤醒 poll, 等线程结束后释放全部客户端及监听器, 不等待网络回复.
    ~Metrics();
    // 单一监听所有者不能复制.
    Metrics(const Metrics&) = delete;
    // 单一监听所有者不能赋值.
    Metrics& operator=(const Metrics&) = delete;
    // 控制线程最多每秒发布一次完整快照; 分配失败保留旧快照, 返回 false, 不影响业务.
    bool publish(const Sample& sample) noexcept;
    // 返回实际绑定端点, 支持测试端口零; 不暴露任意部署身份.
    Endpoint endpoint() const;

private:
    struct Server;                   // Linux 非阻塞 socket 实现, 不进入公共 SDK.
    std::unique_ptr<Server> server_; // 必须在其线程退出后释放.
};
} // namespace astra
