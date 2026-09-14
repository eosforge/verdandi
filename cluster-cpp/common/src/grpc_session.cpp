// 功能: 实现 Hello, 心跳和有界发送队列, 将 gRPC 完成回调转换为控制循环可消费的状态.
#include "grpc_session.hpp"

#include "rpc_status.hpp"
#include <algorithm>
#include <grpcpp/create_channel.h>
#include <limits>

namespace verdandi::cluster {
namespace {
// Client reactor 的外部读写由一个 hold 覆盖. 关闭后只取消并归还 hold, 不再 StartRead/StartWrite.
class ClientSession final : public RpcSession, public grpc::ClientBidiReactor<wire::SessionPacket, wire::SessionPacket> {
public:
    // 依据已授权 target 创建 TLS 通道并绑定稳定 context, 持有唯一外部操作 hold; StartCall 由 Runtime 接管后触发.
    ClientSession(const Config& config, const Identity& identity, SessionGeneration generation, std::shared_ptr<const wire::Hello> hello, Member target,
                  std::function<void()> wake)
        : RpcSession(config, Direction::outbound, generation, std::move(hello), target, std::move(wake)) {
        grpc::ChannelArguments arguments;
        arguments.SetMaxReceiveMessageSize(4096);
        arguments.SetMaxSendMessageSize(4096);
        arguments.SetInt("grpc.enable_retries", 0);
        channel_ = grpc::CreateCustomChannel(target.address.text(), identity.channel_credentials(), arguments);
        channel_->GetState(true);
        stub_ = wire::StarTransport::NewStub(channel_);
        stub_->async()->OpenSession(&context_, this);
        AddHold();
    }
    // gRPC 读取回调只发布 ok, 接收缓冲由后续 pump 消费后才能再次使用.
    void OnReadDone(bool ok) override {
        read_done(ok);
    }
    // gRPC 写回调只释放在途状态并唤醒, 不在回调中取下一条队列消息.
    void OnWriteDone(bool ok) override {
        write_done(ok);
    }
    // 最终回调发布 status 和完成状态, 由 Runtime 回收, 不 delete this.
    void OnDone(const grpc::Status& status) override {
        completed(status);
    }

private:
    // 只查询 Channel 当前状态, 不阻塞等待, 建连总期限由 pump 检查.
    bool transport_ready() const override {
        return channel_->GetState(false) == GRPC_CHANNEL_READY;
    }
    // 在持有 Runtime 所有权且已准备必要读写后调用 StartCall, 同一流只启动一次.
    void begin_call() override {
        StartCall();
    }
    // 借用会话接收缓冲提交读取, OnReadDone 前保持地址稳定.
    void begin_read(wire::SessionPacket* message) override {
        StartRead(message);
    }
    // 借用会话发送缓冲提交写入, OnWriteDone 前保持内容不变.
    void begin_write(const wire::SessionPacket* message) override {
        StartWrite(message);
    }
    // force 允许宽限期后强制 TryCancel; 普通 EOF 优先等待远端最终状态, 避免丢失鉴权错误.
    void request_cancel(bool force) override {
        // EOF 后保留服务器最终状态, 避免本地 TryCancel 把 PERMISSION_DENIED 等覆盖成 CANCELLED.
        if (force || error() != ErrorCode::transport) {
            context_.TryCancel();
        }
    }
    // 外部操作停止后归还唯一 hold, 允许最终 OnDone 发生; 客户端不将 ErrorCode 写成服务端状态.
    void finish_call(ErrorCode) override {
        RemoveHold();
    }
    grpc::ClientContext context_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<wire::StarTransport::Stub> stub_;
};
} // namespace

RpcSession::RpcSession(const Config& config, Direction direction, SessionGeneration generation, std::shared_ptr<const wire::Hello> hello,
                       std::optional<Member> expected, std::function<void()> wake)
    : config_(config), direction_(direction), generation_(generation), hello_(std::move(hello)), expected_(std::move(expected)), wake_(std::move(wake)),
      handshake_deadline_(Clock::now() + config.handshake_timeout), connect_deadline_(std::min(handshake_deadline_, Clock::now() + config.connect_timeout)) {}

bool RpcSession::enqueue(wire::SessionPacket message, Clock::time_point deadline) {
    if (queued_ == queue_.size()) {
        return false;
    }
    // 已由容量检查保证的内部不变量, 不用契约直接检查网络输入.
    contract_assert(queued_ < queue_.size());
    queue_[(head_ + queued_) % queue_.size()] = PendingPacket{std::move(message), deadline};
    ++queued_;
    return true;
}

Result<std::vector<SessionGeneration>> RpcSession::receive(const wire::SessionPacket& packet, Policy& policy, const Identity& identity, Clock::time_point now) {
    if (packet.ByteSizeLong() > maximum_) {
        return std::unexpected(Error{ErrorCode::capacity, "Control message exceeds negotiated limit"});
    }
    if (packet.has_rejection()) {
        return std::unexpected(Error{packet.rejection().code() == wire::PROTOCOL_ERROR_CODE_UNAUTHORIZED ? ErrorCode::identity : ErrorCode::protocol,
                                     "Remote star rejected session"});
    }
    // 首帧必须为兼容 Hello, 验签与角色接纳成功后才能安装逻辑身份和协商控制帧上限.
    if (!installed_) {
        if (!packet.has_hello() || packet.hello().protocol_major() != 6 || packet.hello().max_frame_bytes() < 1024) {
            return std::unexpected(Error{ErrorCode::protocol, "Expected compatible Hello"});
        }
        auto member = identity.verify(packet.hello().admission(), packet.hello().admission_signature());
        if (!member) {
            return std::unexpected(member.error());
        }
        auto install = policy.accept(*member, direction_, generation_, expected_);
        if (!install) {
            return std::unexpected(install.error());
        }
        installed_ = true;
        if (direction_ == Direction::inbound) {
            // 凭证是 bearer, 只向已经通过准入和角色验证的调用方返回本端 Hello.
            wire::SessionPacket reply;
            reply.mutable_hello()->CopyFrom(*hello_);
            if (!enqueue(std::move(reply), handshake_deadline_)) {
                return std::unexpected(Error{ErrorCode::capacity, "Hello send queue is full"});
            }
        }
        maximum_ = std::min({4096U, hello_->max_frame_bytes(), packet.hello().max_frame_bytes()});
        next_ping_ = now + config_.heartbeat_interval;
        return install;
    }
    // 握手后仅处理带非零请求号的 Ping/Pong, 只有匹配本端待确认 Ping 的 Pong 才推进心跳期限.
    if (packet.has_ping() && packet.ping().request_id() != 0) {
        wire::SessionPacket reply;
        reply.mutable_pong()->set_request_id(packet.ping().request_id());
        if (!enqueue(std::move(reply), now + config_.pong_timeout)) {
            return std::unexpected(Error{ErrorCode::capacity, "Control send queue is full"});
        }
    } else if (packet.has_pong() && packet.pong().request_id() != 0) {
        if (pending_ping_ && packet.pong().request_id() == pending_ping_->first) {
            pending_ping_.reset();
            next_ping_ = now + config_.heartbeat_interval;
        }
    } else {
        return std::unexpected(Error{ErrorCode::protocol, "Unexpected control message"});
    }
    return std::vector<SessionGeneration>{};
}

std::vector<SessionGeneration> RpcSession::pump(Policy& policy, const Identity& identity, Clock::time_point now) {
    if (done()) {
        return {};
    }
    if (!started_) {
        // 连接和 Hello 共用最初的总预算, 拨号阶段另有更短上限. 关闭时仍需完成已绑定 client call.
        if (!error_ && !transport_ready()) {
            if (now < connect_deadline_) {
                return {};
            }
            cancel(ErrorCode::timeout);
        }
        // gRPC 允许 StartCall 前准备读写; Runtime 在任何完成回调前已持有此对象.
        std::lock_guard lock(mutex_);
        started_ = true;
        if (!error_) {
            read_inflight_ = true;
            begin_read(&read_);
            if (direction_ == Direction::outbound) {
                write_.mutable_hello()->CopyFrom(*hello_);
                write_deadline_ = handshake_deadline_;
                write_inflight_ = true;
                begin_write(&write_);
            } else {
                metadata_inflight_ = true;
            }
        }
        if (direction_ == Direction::outbound || !error_) {
            begin_call();
        }
    }
    std::optional<wire::SessionPacket> message;
    bool transport_closed = false;
    bool write_expired = false;
    {
        std::lock_guard lock(mutex_);
        transport_closed = read_failed_ || write_failed_ || remote_cancelled_;
        write_expired = (write_inflight_ && now >= write_deadline_) || (metadata_inflight_ && now >= handshake_deadline_);
        if (read_ready_) {
            message.emplace();
            message->Swap(&read_);
            read_ready_ = false;
        }
    }
    if (transport_closed) {
        cancel(ErrorCode::transport);
    }
    std::vector<SessionGeneration> cancelled;
    // 截止优先于刚到达的 Pong/Hello, 排队和写入等待均包含在最初建立的绝对期限内.
    // Hello 和 Pong 的期限可能不同, 不能假定队首总是最早到期. 固定四槽检查不随节点规模增长.
    const bool queued_expired = std::ranges::any_of(queue_, [now](const auto& item) { return item && now >= item->deadline; });
    if (write_expired || queued_expired || (!installed_ && now >= handshake_deadline_) || (pending_ping_ && now >= pending_ping_->second)) {
        cancel(ErrorCode::timeout);
    }
    if (!error_ && message) {
        auto result = receive(*message, policy, identity, now);
        if (!result) {
            cancel(result.error().code);
        } else {
            cancelled = std::move(*result);
        }
    }
    if (!error_ && installed_ && !pending_ping_ && now >= next_ping_) {
        if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
            cancel(ErrorCode::protocol);
        } else {
            const auto id = next_id_++;
            pending_ping_ = std::pair{id, now + config_.pong_timeout};
            wire::SessionPacket ping;
            ping.mutable_ping()->set_request_id(id);
            if (!enqueue(std::move(ping), now + config_.pong_timeout)) {
                cancel(ErrorCode::capacity);
            }
        }
    }
    {
        // 这里只锁会话的在途标志, 身份验签和 Policy 操作已经在锁外完成.
        std::lock_guard lock(mutex_);
        if (error_) {
            // 先让正常 Finish 携带明确错误码. 读, 写或最终状态尚未排空时, 最多宽限 250 ms 再强制取消.
            if (now >= cancel_deadline_) {
                request_cancel(true);
            }
            if (!write_inflight_ && !metadata_inflight_ && !finish_called_) {
                finish_called_ = true;
                finish_call(*error_);
            }
        } else if (!read_failed_ && !write_failed_ && !remote_cancelled_) {
            // 失败回调也可能在两次检查之间到达. 本轮停止提交新操作, 下一轮在锁外推进取消.
            if (!write_inflight_ && !metadata_inflight_ && queued_ != 0) {
                write_.Swap(&queue_[head_]->packet);
                write_deadline_ = queue_[head_]->deadline;
                queue_[head_].reset();
                head_ = (head_ + 1) % queue_.size();
                --queued_;
                write_inflight_ = true;
                begin_write(&write_);
            }
            // 完成回调可能在本轮取消息之后到达. 尚未消费的 read_ 仍归控制循环, 不能再次交给 gRPC 写入.
            if (!read_inflight_ && !read_ready_) {
                read_inflight_ = true;
                begin_read(&read_);
            }
        }
    }
    return cancelled;
}

void RpcSession::cancel(ErrorCode error) {
    if (!error_ && !done()) {
        error_ = error;
        cancel_deadline_ = Clock::now() + Milliseconds(250);
        request_cancel(false);
    }
}

void RpcSession::read_done(bool ok) {
    {
        std::lock_guard lock(mutex_);
        read_inflight_ = false;
        read_ready_ = ok;
        read_failed_ = !ok;
    }
    wake_();
}

void RpcSession::write_done(bool ok) {
    {
        std::lock_guard lock(mutex_);
        write_inflight_ = false;
        write_failed_ = !ok;
    }
    wake_();
}

void RpcSession::server_cancelled() {
    {
        std::lock_guard lock(mutex_);
        remote_cancelled_ = true;
    }
    wake_();
}

void RpcSession::metadata_done(bool ok) {
    {
        std::lock_guard lock(mutex_);
        metadata_inflight_ = false;
        write_failed_ = write_failed_ || !ok;
    }
    wake_();
}

void RpcSession::completed(grpc::Status status) {
    final_status_ = std::move(status);
    auto wake = std::move(wake_);
    // 发布后只使用独立局部唤醒对象, 不再访问成员. Runtime 可立即回收 reactor 和 context.
    done_.store(true, std::memory_order_release);
    wake();
}

bool RpcSession::done() const {
    return done_.load(std::memory_order_acquire);
}
bool RpcSession::installed() const {
    return installed_;
}
SessionGeneration RpcSession::generation() const {
    return generation_;
}
Direction RpcSession::direction() const {
    return direction_;
}
const std::optional<Member>& RpcSession::expected() const {
    return expected_;
}

std::optional<ErrorCode> RpcSession::error() const {
    if (done() && !final_status_.ok() && (!error_ || *error_ == ErrorCode::transport)) {
        return rpc_error_code(final_status_.error_code());
    }
    return error_;
}

std::shared_ptr<RpcSession> connect_session(const Config& config, const Identity& identity, SessionGeneration generation,
                                            std::shared_ptr<const wire::Hello> hello, Member target, std::function<void()> wake) {
    return std::make_shared<ClientSession>(config, identity, generation, std::move(hello), std::move(target), std::move(wake));
}

AcceptedSession::AcceptedSession(grpc::CallbackServerContext* context, const Config& config, SessionGeneration generation,
                                 std::shared_ptr<const wire::Hello> hello, std::function<void()> wake)
    : RpcSession(config, Direction::inbound, generation, std::move(hello), std::nullopt, std::move(wake)), context_(context) {}
void AcceptedSession::OnReadDone(bool ok) {
    read_done(ok);
}
void AcceptedSession::OnWriteDone(bool ok) {
    write_done(ok);
}
void AcceptedSession::OnSendInitialMetadataDone(bool ok) {
    metadata_done(ok);
}
void AcceptedSession::OnCancel() {
    server_cancelled();
}
void AcceptedSession::OnDone() {
    completed(grpc::Status::OK);
}
void AcceptedSession::begin_call() {
    // 先发送不含凭证的响应元数据, 让等待 RPC 响应头的客户端能够继续发送自己的 Hello.
    StartSendInitialMetadata();
}
void AcceptedSession::begin_read(wire::SessionPacket* message) {
    StartRead(message);
}
void AcceptedSession::begin_write(const wire::SessionPacket* message) {
    StartWrite(message);
}
void AcceptedSession::request_cancel(bool force) {
    if (force) {
        context_->TryCancel();
    }
}
void AcceptedSession::finish_call(ErrorCode error) {
    const auto status = error == ErrorCode::identity                                   ? grpc::StatusCode::PERMISSION_DENIED
                        : error == ErrorCode::protocol || error == ErrorCode::conflict ? grpc::StatusCode::INVALID_ARGUMENT
                                                                                       : grpc::StatusCode::CANCELLED;
    Finish(grpc::Status(status, "Session closed"));
}
} // namespace verdandi::cluster
