#pragma once
#include "identity.hpp"
#include "polaris.grpc.pb.h"
#include "receiver.hpp"
#include <atomic>
#include <functional>

namespace astra {
// Star 的唯一 Polaris 接收流. 回调只发布 I/O 完成, 解析、安装和发送由既有控制循环推进.
// 所有者必须 cancel 后持续 pump, 直到 done 再析构, 不在析构中阻塞等待 gRPC.
class Intake final : public grpc::ClientBidiReactor<proto::polaris::v1::Packet, proto::polaris::v1::Packet> {
public:
    // identity/local 为当前 Star 材料, target 必须来自受信 Polaris 目录, output/access 配套跨重连保留.
    // wake 必须持有独立唤醒对象, 不借用 Runtime; 构造不启动 RPC.
    Intake(std::shared_ptr<Identity> identity, const proto::astra::v1::Hello& local, Member target, Library& output, Access& access, std::function<void()> wake);
    // 只在最终完成后由唯一控制循环销毁, 不自动取消仍被 gRPC 借用的缓冲.
    ~Intake() override = default;
    // 禁止复制一个有固定 reactor/context 地址的流.
    Intake(const Intake&) = delete;
    // 禁止覆盖尚未归还的传输所有权.
    Intake& operator=(const Intake&) = delete;
    // 控制循环单轮最多处理一页, now 是本地单调时刻, 不在 gRPC callback 内调用.
    void pump(Steady::time_point now);
    // 永久取消本次流, cause 为固定分类; 归还 hold 后不再提出新的异步读写.
    void cancel(Status::Code cause = Status::Code::cancelled);
    // 真正完成后为 true, 是唯一允许析构的边界.
    bool done() const noexcept;
    // 当前流已验证且首轮安装完成; 断线/取消后为 false, 不清除 Library 已有数据.
    bool ready() const noexcept;
    // 已验证的当前 Polaris, 握手可接受同部署更新代次, 不改变部署主体或端点.
    const Member& target() const noexcept;
    // 返回固定失败分类, 未取消时为空; 只由控制循环读取.
    std::optional<Status::Code> error() const noexcept;
    // 以下回调不解析 Proto 或操作业务, ok 的发布将缓冲所有权交回控制循环.
    void OnReadDone(bool ok) override;
    // 唯一在途写完成, 返回前不重用帧或发下一次 Write.
    void OnWriteDone(bool ok) override;
    // 所有 transport 操作结束后发布最终完成, 之后不访问任何成员.
    void OnDone(const grpc::Status& status) override;

private:
    // 首帧验签并比较部署身份, 后续只交给 Receiver, 不逐页重复公钥验证.
    Result<void> receive();
    // 不可变身份材料, 在所有回调结束前持有.
    std::shared_ptr<Identity> identity_;
    // 本流固定部署, 合法更高代次握手后更新为实际对端.
    Member target_;
    // 私有安装状态, 半份快照只在此流内存在.
    Receiver receiver_;
    // 独立通知闭包, OnDone 移到局部再发布完成.
    std::function<void()> wake_;
    // TLS Channel 和生成 Stub, 在 context 完成之前保持存活.
    std::shared_ptr<grpc::Channel> channel_;
    // 唯一同步接口存根, 不为每个 Scope 建立 RPC.
    std::unique_ptr<proto::polaris::v1::Almanac::Stub> stub_;
    // 稳定上下文, 长流由本地阶段期限/取消控制, 不设置整个流的短截止.
    grpc::ClientContext context_;
    // 接收缓冲只在 read_ 非零并 acquire 后消费, 下次 StartRead 前清空.
    proto::polaris::v1::Packet input_;
    // 发送缓冲在 write_ 完成前不可修改, 构造时装入本地 Hello.
    proto::polaris::v1::Packet output_;
    // 0 等待, 1 成功, -1 失败; 分别由对应 gRPC callback release 发布.
    std::atomic_int read_{};
    // 写结果与读结果独立, 允许一个读和一个写同时在途.
    std::atomic_int write_{};
    // 只有 OnDone 发布 true, 外部 acquire 后可以析构整个对象.
    std::atomic_bool done_{};
    // 首次失败的固定分类, 只由控制循环写, 不记录远端状态正文.
    std::optional<Status::Code> error_;
    // 首次 StartCall 是否已发, 初始 false.
    bool started_{};
    // 已通过 Hello 的身份标志, 初始 false.
    bool authenticated_{};
    // 当前写是否在途, 初始 false, 只由控制循环操作.
    bool writing_{};
    // 对端声明的接收字节上限, Hello 验证后固定; 首帧前使用本地硬上限 8 MiB.
    std::size_t maximum_ = 8 * 1024 * 1024;
    // 首次握手的总截止, 构造时固定为五秒.
    Steady::time_point handshake_ = Steady::now() + std::chrono::seconds(5);
    // 最近收到完整帧的时刻, 初始为构造时刻, 75 秒无进度取消.
    Steady::time_point received_ = Steady::now();
    // 当前写操作的截止, 每次真实 StartWrite 前固定.
    Steady::time_point deadline_{};
};
} // namespace astra
