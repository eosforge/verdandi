// 功能: 定义双向 gRPC 会话和回调边界, 由控制循环独占协议推进与关闭流程.
#pragma once
#include "astra.grpc.pb.h"
#include "identity.hpp"
#include <astra/policy.hpp>

#include <atomic>
#include <functional>
#include <mutex>

namespace astra {
// 每个逻辑流一个对象. gRPC 回调只发布完成标志, 控制循环独占协议状态和角色操作.
class RpcSession {
public:
    // 复制配置与 expected, 持有不可变 hello 及 wake, generation 由 Runtime 唯一分配.
    // direction 决定握手方向, 出站携带授权目标, 入站 expected 为空; wake 不能借用会话或 Runtime.
    RpcSession(const Config& config, Direction direction, SessionGeneration generation, std::shared_ptr<const proto::astra::v1::Hello> hello,
               std::optional<Member> expected, std::function<void()> wake);
    // 允许基类指针释放具体 reactor; 仅在最终完成已发布后销毁, 析构不是取消操作.
    virtual ~RpcSession() = default;
    // 每轮最多消费一个接收消息. 返回因合法成员替换而需要在其他会话上执行的取消动作.
    // 仅由控制循环调用, policy/identity 仅在调用期间借用, now 为单调时间.
    // 协议错误转换为本会话取消状态, 返回的旧代次取消由 Runtime 执行, 不在回调线程推进.
    std::vector<SessionGeneration> pump(Policy& policy, const Identity& identity, Clock::time_point now);
    // 只请求关闭. 最终完成之前仍由 Runtime 持有, 不能立即析构 context 或写入消息.
    // 仅控制循环调用, error 记录首次关闭原因, 默认 cancelled; 重复请求不重置 250 ms 取消宽限期.
    void cancel(ErrorCode error = ErrorCode::cancelled);
    // 以 acquire 读取最终完成发布, true 后可读取最终状态并由 Runtime 回收会话.
    bool done() const;
    // 仅控制循环查询 Hello 是否通过身份与角色校验, 不表示已完成业务同步.
    bool installed() const;
    // 返回本进程固定会话代次, 用于精确对应角色索引和完成事件.
    SessionGeneration generation() const;
    // 返回相对于本进程的固定连接方向, 生命周期内不变化.
    Direction direction() const;
    // 返回原出站拨号目标的只读借用, 入站为空; 借用不能超过此会话生命周期.
    const std::optional<Member>& expected() const;
    // 控制循环读取本地关闭原因; 最终非成功 RPC 可细化普通 transport 原因, 不覆盖明确本地协议或身份错误.
    std::optional<ErrorCode> error() const;

protected:
    // 控制循环查询传输是否就绪; 默认适用于已接纳的入站连接, 客户端重载检查 Channel 状态.
    virtual bool transport_ready() const {
        return true;
    }
    // 由 pump 首次启动调用, 客户端 StartCall, 服务端发送初始元数据; 同一会话最多调用一次.
    virtual void begin_call() = 0;
    // 提交唯一在途读取, message 存储由会话持有直到 read_done, 完成消费之前禁止复用.
    virtual void begin_read(proto::astra::v1::SessionPacket* message) = 0;
    // 提交唯一在途写入, message 必须保持有效且不可变直到 write_done, 调用方已设置写入截止.
    virtual void begin_write(const proto::astra::v1::SessionPacket* message) = 0;
    // 请求底层取消; force=true 表示宽限期结束必须 TryCancel, false 允许保留远端最终错误状态.
    virtual void request_cancel(bool force) = 0;
    // 服务端只能在没有在途写时 Finish; 客户端在外部操作完全停止后归还唯一 hold.
    // error 为首次本地关闭原因, 由服务端映射为固定最终状态; 此函数不表示 OnDone 已到达.
    virtual void finish_call(ErrorCode error) = 0;
    // 由读取回调发布 ok 和缓冲所有权, 锁外唤醒控制循环; 不再次发起读取或解析消息.
    void read_done(bool ok);
    // 由写完成回调释放在途标志并记录 ok, 锁外唤醒控制循环, 不消费发送队列.
    void write_done(bool ok);
    // 由初始元数据回调释放在途标志, !ok 记录发送失败并唤醒, 不安装逻辑身份.
    void metadata_done(bool ok);
    // 由服务端取消回调发布关闭事实并唤醒, 最终完成仍以 OnDone 为准.
    void server_cancelled();
    // 仅最终回调调用一次, 移入 status 并以 release 发布 done; 发布后不得再次访问任何会话成员.
    void completed(grpc::Status status);

private:
    // 控制循环解析单条 packet, 先验证容量与 Hello 再调用角色策略, 返回待取消代次或有限错误.
    // 参数仅在调用期间借用; 心跳响应进入本地有界队列, 不在此函数中直接读写传输.
    Result<std::vector<SessionGeneration>> receive(const proto::astra::v1::SessionPacket& packet, Policy& policy, const Identity& identity,
                                                   Clock::time_point now);
    // 控制循环移入 message 及绝对 deadline; 四槽队列满返回 false, 成功只入队, 不重置原有截止.
    bool enqueue(proto::astra::v1::SessionPacket message, Clock::time_point deadline);
    Config config_;
    Direction direction_;
    SessionGeneration generation_;
    std::shared_ptr<const proto::astra::v1::Hello> hello_;
    std::optional<Member> expected_;
    std::function<void()> wake_;
    mutable std::mutex mutex_;
    proto::astra::v1::SessionPacket read_;
    proto::astra::v1::SessionPacket write_;
    bool read_inflight_{};
    bool read_ready_{};
    bool read_failed_{};
    bool write_inflight_{};
    bool write_failed_{};
    bool metadata_inflight_{};
    bool remote_cancelled_{};
    grpc::Status final_status_;
    std::atomic_bool done_{false};
    // 以下字段只由控制循环访问, 不与 callback 共享可变协议状态.
    bool started_{};
    bool installed_{};
    bool finish_called_{};
    // 首次本地关闭原因同时表示进入关闭阶段, 避免另外维护可不一致的 closing 标志.
    std::optional<ErrorCode> error_;
    Clock::time_point handshake_deadline_;
    Clock::time_point connect_deadline_;
    Clock::time_point next_ping_;
    Clock::time_point cancel_deadline_;
    Clock::time_point write_deadline_;
    std::optional<std::pair<std::uint64_t, Clock::time_point>> pending_ping_;
    std::uint64_t next_id_ = 1;
    std::uint32_t maximum_ = 4096;
    struct PendingPacket {
        proto::astra::v1::SessionPacket packet;
        Clock::time_point deadline;
    };
    std::array<std::optional<PendingPacket>, 4> queue_;
    std::size_t head_{};
    std::size_t queued_{};
};

// 工厂在启动 Call 前把对象交给 Runtime, 不从 callback 中 delete this 或创建所有权环.
// 使用 config/identity 准备 target 对应的客户端, 持有 hello/wake 和目标副本; 返回共享对象供 Runtime 接管后 pump.
std::shared_ptr<RpcSession> connect_session(const Config& config, const Identity& identity, SessionGeneration generation,
                                            std::shared_ptr<const proto::astra::v1::Hello> hello, Member target, std::function<void()> wake);
// 生成的服务 handler 使用实际 ServerBidiReactor, 生命周期由同一 Runtime 集合持有.
class AcceptedSession : public RpcSession, public grpc::ServerBidiReactor<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket> {
public:
    // context 由 gRPC 借出直到 OnDone, config 被复制, hello/wake 被持有; Runtime 须在 handler 返回前登记所有权.
    AcceptedSession(grpc::CallbackServerContext* context, const Config& config, SessionGeneration generation,
                    std::shared_ptr<const proto::astra::v1::Hello> hello, std::function<void()> wake);
    // gRPC 读取完成入口, 仅将 ok 转发给 read_done, 不推进角色状态.
    void OnReadDone(bool ok) override;
    // gRPC 写入完成入口, 仅将 ok 转发给 write_done, 不复用写缓冲.
    void OnWriteDone(bool ok) override;
    // gRPC 初始元数据完成入口, 将 ok 发布到控制循环.
    void OnSendInitialMetadataDone(bool ok) override;
    // gRPC 服务端取消通知入口, 记录事实而不释放 reactor.
    void OnCancel() override;
    // gRPC 最终完成入口, 发布完成状态, 生命周期由 Runtime 回收.
    void OnDone() override;

private:
    // 发送不含准入凭证的初始元数据, 避免客户端等待响应头导致 Hello 互等.
    void begin_call() override;
    // 将会话拥有的接收缓冲借给 StartRead, 直到 OnReadDone 前不得修改.
    void begin_read(proto::astra::v1::SessionPacket* message) override;
    // 将不可变发送缓冲借给 StartWrite, 直到 OnWriteDone 前不得复用.
    void begin_write(const proto::astra::v1::SessionPacket* message) override;
    // 仅 force 时取消服务端 context, 宽限期内允许 Finish 携带明确拒绝状态.
    void request_cancel(bool force) override;
    // 将本地 error 转为固定 gRPC 状态并 Finish, 调用方须保证写入和初始元数据已完成.
    void finish_call(ErrorCode error) override;
    grpc::CallbackServerContext* context_;
};
} // namespace astra
