// 许可证: MIT, 详见仓库根目录 LICENSE.
#pragma once
#include "identity.hpp"
#include "peer_transport.grpc.pb.h"
#include <verdandi/peer/policy.hpp>

#include <atomic>
#include <functional>
#include <mutex>

namespace verdandi::peer {
// 每个逻辑流一个对象. gRPC 回调只发布完成标志, 控制循环独占协议状态和角色操作.
class RpcSession {
public:
    RpcSession(const Config& config, Direction direction, SessionGeneration generation, std::shared_ptr<const wire::Hello> hello,
               std::optional<DialTarget> expected, std::function<void()> wake);
    virtual ~RpcSession() = default;
    // 每轮最多消费一个接收消息. 返回因合法成员替换而需要在其他会话上执行的取消动作.
    std::vector<SessionGeneration> pump(Policy& policy, const Identity& identity, Clock::time_point now);
    // 只请求关闭. 最终完成之前仍由 Runtime 持有, 不能立即析构 context 或写入消息.
    void cancel(ErrorCode error = ErrorCode::cancelled);
    bool done() const;
    bool installed() const;
    SessionGeneration generation() const;
    Direction direction() const;
    const std::optional<DialTarget>& expected() const;
    std::optional<ErrorCode> error() const;

protected:
    virtual bool transport_ready() const {
        return true;
    }
    virtual void begin_call() = 0;
    virtual void begin_read(wire::SessionPacket* message) = 0;
    virtual void begin_write(const wire::SessionPacket* message) = 0;
    virtual void request_cancel(bool force) = 0;
    // 服务端只能在没有在途写时 Finish; 客户端在外部操作完全停止后归还唯一 hold.
    virtual void finish_call(ErrorCode error) = 0;
    void read_done(bool ok);
    void write_done(bool ok);
    void metadata_done(bool ok);
    void server_cancelled();
    void completed(grpc::Status status);

private:
    Result<std::vector<SessionGeneration>> receive(const wire::SessionPacket& packet, Policy& policy, const Identity& identity, Clock::time_point now);
    bool enqueue(wire::SessionPacket message, Clock::time_point deadline);
    Config config_;
    Direction direction_;
    SessionGeneration generation_;
    std::shared_ptr<const wire::Hello> hello_;
    std::optional<DialTarget> expected_;
    std::function<void()> wake_;
    mutable std::mutex mutex_;
    wire::SessionPacket read_;
    wire::SessionPacket write_;
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
    bool closing_{};
    bool finish_called_{};
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
        wire::SessionPacket packet;
        Clock::time_point deadline;
    };
    std::array<std::optional<PendingPacket>, 4> queue_;
    std::size_t head_{};
    std::size_t queued_{};
};

// 工厂在启动 Call 前把对象交给 Runtime, 不从 callback 中 delete this 或创建所有权环.
std::shared_ptr<RpcSession> connect_session(const Config& config, const Identity& identity, SessionGeneration generation,
                                            std::shared_ptr<const wire::Hello> hello, DialTarget target, std::function<void()> wake);
// 生成的服务 handler 使用实际 ServerBidiReactor, 生命周期由同一 Runtime 集合持有.
class AcceptedSession : public RpcSession, public grpc::ServerBidiReactor<wire::SessionPacket, wire::SessionPacket> {
public:
    AcceptedSession(grpc::CallbackServerContext* context, const Config& config, SessionGeneration generation, std::shared_ptr<const wire::Hello> hello,
                    std::function<void()> wake);
    void OnReadDone(bool ok) override;
    void OnWriteDone(bool ok) override;
    void OnSendInitialMetadataDone(bool ok) override;
    void OnCancel() override;
    void OnDone() override;

private:
    void begin_call() override;
    void begin_read(wire::SessionPacket* message) override;
    void begin_write(const wire::SessionPacket* message) override;
    void request_cancel(bool force) override;
    void finish_call(ErrorCode error) override;
    grpc::CallbackServerContext* context_;
};
} // namespace verdandi::peer
