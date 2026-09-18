// 功能: Star 独占一个可取消的对时线程, 复用 TLS Channel 执行有界采样批次.
#pragma once
#include "identity.hpp"
#include "pulsar.grpc.pb.h"
#include <astra/clock.hpp>
#include <condition_variable>
#include <thread>

namespace astra {
class PulseClient {
public:
    // endpoint 来自已验证的登记应答, identity/hello 保持只读共享, output 必须存活到析构完成.
    // 构造后开始对时, 不阻塞 Runtime 的会话推进; 不在任何 I/O worker 上执行本线程的等待.
    PulseClient(std::string endpoint, std::shared_ptr<Identity> identity, std::shared_ptr<const proto::astra::v1::Hello> hello, EpochClock& output);
    // 请求取消当前 RPC 并 join 唯一工作线程, 返回后不会再访问 output.
    ~PulseClient();
    PulseClient(const PulseClient&) = delete;
    PulseClient& operator=(const PulseClient&) = delete;
    // 非阻塞提出停止, Runtime 可以同时取消其他会话, 析构负责最终 join.
    void stop() noexcept;

private:
    // 单次采样最多两秒/八个请求, 只有完整结束且至少三个有效样本才发布.
    grpc::Status sample(std::stop_token stop);
    // 有界退避和错峰采样循环, 捕获异常并撤销新租约资格, 不清除连续走时锚点.
    void run(std::stop_token stop) noexcept;
    // 批内间隔与批间退避共用可取消等待; stop 返回 false, 不承诺操作系统调度的硬实时界限.
    bool wait(std::stop_token stop, Milliseconds delay);
    // 只读 TLS 材料与进程准入凭证, 不包含传播给 Pulse 的明文密码.
    std::shared_ptr<Identity> identity_;
    std::shared_ptr<const proto::astra::v1::Hello> hello_;
    // 对外发布估计的唯一引用, 寿命由 Runtime 管理.
    EpochClock& output_;
    // 整个线程复用 Channel 和 Stub, 每轮仅创建有截止的逻辑流.
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<proto::pulsar::v1::Pulse::Stub> stub_;
    // 按本进程凭证派生采样错峰, 只用于调度而非密码学.
    std::size_t jitter_{};
    // 工作线程首次成功标定后的本机 rho. 零表示待标定, 失败退避重试, 不阻塞构造者.
    std::uint64_t precision_ns_{};
    // 只有工作线程等待, stop_token 的通知可唤醒批内间隔与批间退避.
    std::mutex wait_mutex_;
    std::condition_variable_any condition_;
    // 最后声明, 确保构造时其他字段就绪, 析构时线程先停止再释放请求依赖.
    std::jthread worker_;
};
} // namespace astra
