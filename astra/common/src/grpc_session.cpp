// 功能: 实现 Hello, 心跳和有界发送队列, 将 gRPC 完成回调转换为控制循环可消费的状态.
// 本文件实现了 grpc_session.hpp 中的 RpcSession 基类逻辑，以及内部隐藏的 ClientSession(客户端出站逻辑)，
// 同时提供了 factory connect_session 及 AcceptedSession (服务端入站逻辑) 的实现。
#include "grpc_session.hpp"

#include "rpc_status.hpp"
#include <algorithm>
#include <grpcpp/create_channel.h>
#include <limits>

namespace astra {
namespace {
// ClientSession 类: Client reactor 的外部读写由一个 hold 覆盖. 关闭后只取消并归还 hold, 不再 StartRead/StartWrite.
// 这是一个内部实现的私有类，用于代表主动向外建立连接的客户端 gRPC 侧双向流反应器。
class ClientSession final : public RpcSession, public grpc::ClientBidiReactor<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket> {
public:
    // 构造函数: 依据已授权 target 创建 TLS 通道并绑定稳定 context, 持有唯一外部操作 hold; StartCall 由 Runtime 接管后触发.
    // 参数 config: 会话超时及策略配置参数.
    // 参数 identity: 本地节点身份，用于设置客户端信道的证书.
    // 参数 generation: 分配的会话代次编号.
    // 参数 hello: 自身的协议能力握手包.
    // 参数 target: 想要连接的目标节点信息(含 IP 和端口等).
    // 参数 wake: 用于在完成底层动作后唤醒外部控制循环。
    ClientSession(const Config& config, const Identity& identity, SessionGeneration generation, std::shared_ptr<const proto::astra::v1::Hello> hello,
                  Member target, std::function<void()> wake)
        : RpcSession(config, Direction::outbound, generation, std::move(hello), target, std::move(wake)) {
        // 创建指定属性的 grpc 通道，关闭了内部重试，并设置包大小限制以对应会话机制
        grpc::ChannelArguments arguments;
        arguments.SetMaxReceiveMessageSize(4096);
        arguments.SetMaxSendMessageSize(4096);
        arguments.SetInt("grpc.enable_retries", 0);
        
        // 创建具备目标地址和安全认证策略（如 mTLS）的通信 Channel.
        channel_ = grpc::CreateCustomChannel(target.address.text(), identity.channel_credentials(), arguments);
        // 初始化请求一次获取连接状态，以促使 grpc 尽早开始解析并进行建立 TCP 建连动作.
        channel_->GetState(true);
        // 初始化对应的 protobuf 服务存根(Stub).
        stub_ = proto::astra::v1::StarTransport::NewStub(channel_);
        // 向 stub 发起非阻塞的双向流请求，并关联此对象作为它的流处理器 (reactor).
        stub_->async()->OpenSession(&context_, this);
        // 增加一个额外的挂起占用(hold)，表示我们主动维持住这个 client reactor 不被提前释放回收.
        AddHold();
    }
    
    // OnReadDone 回调函数: gRPC 读取回调只发布 ok, 接收缓冲由后续 pump 消费后才能再次使用.
    // 参数 ok: 标识 gRPC 是否成功在提供给 StartRead 的缓冲中填入完整的 Packet。
    void OnReadDone(bool ok) override {
        read_done(ok);
    }
    
    // OnWriteDone 回调函数: gRPC 写回调只释放在途状态并唤醒, 不在回调中取下一条队列消息.
    // 参数 ok: 标识 StartWrite 的信息是否已经成功发送或遇到失败断开。
    void OnWriteDone(bool ok) override {
        write_done(ok);
    }
    
    // OnDone 回调函数: 最终回调发布 status 和完成状态, 由 Runtime 回收, 不 delete this.
    // 参数 status: 报告此次调用的总结果和 grpc 层面的最后消息。
    void OnDone(const grpc::Status& status) override {
        completed(status);
    }

private:
    // transport_ready 重载: 只查询 Channel 当前状态, 不阻塞等待, 建连总期限由 pump 检查.
    // 返回值: 如果通道底层已经是就绪状态则返回 true，否则返回 false。
    bool transport_ready() const override {
        return channel_->GetState(false) == GRPC_CHANNEL_READY;
    }
    
    // begin_call 重载: 在持有 Runtime 所有权且已准备必要读写后调用 StartCall, 同一流只启动一次.
    void begin_call() override {
        StartCall();
    }
    
    // begin_read 重载: 借用会话接收缓冲提交读取, OnReadDone 前保持地址稳定.
    // 参数 message: 将收到的下行数据放到会话所给的容器。
    void begin_read(proto::astra::v1::SessionPacket* message) override {
        StartRead(message);
    }
    
    // begin_write 重载: 借用会话发送缓冲提交写入, OnWriteDone 前保持内容不变.
    // 参数 message: 准备发送的数据指针。
    void begin_write(const proto::astra::v1::SessionPacket* message) override {
        StartWrite(message);
    }
    
    // request_cancel 重载: force 允许宽限期后强制 TryCancel; 普通 EOF 优先等待远端最终状态, 避免丢失鉴权错误.
    // 参数 force: 若为 true 强行打断请求，否则根据当前会话是否存在正常的异常码选择是否需要中止客户端上下文。
    void request_cancel(bool force) override {
        // EOF 后保留服务器最终状态, 避免本地 TryCancel 把 PERMISSION_DENIED 等覆盖成 CANCELLED.
        if (force || error() != Error::Code::transport) {
            context_.TryCancel();
        }
    }
    
    // finish_call 重载: 控制循环停止提交新操作后归还唯一 hold, OnDone 才允许完成.
    // 参数 Error::Code (匿名): 关闭对应的错误码。此处客户端不通过 grpc 主动返回响应，故仅仅是解除占用(RemoveHold)。
    void finish_call(Error::Code) override {
        RemoveHold();
    }
    
    // context_: 客户端流对应的环境信息上下文。
    grpc::ClientContext context_;
    // channel_: 所绑定底层的 gRPC 网络通道智能指针。
    std::shared_ptr<grpc::Channel> channel_;
    // stub_: 与生成服务方法交互的异步存根实例指针。
    std::unique_ptr<proto::astra::v1::StarTransport::Stub> stub_;
};
} // namespace

// 构造函数实现: 初始化各项基类记录属性，并从此刻开始计算握手及建连截止时间。
RpcSession::RpcSession(const Config& config, Direction direction, SessionGeneration generation, std::shared_ptr<const proto::astra::v1::Hello> hello,
                       std::optional<Member> expected, std::function<void()> wake)
    : config_(config), direction_(direction), generation_(generation), hello_(std::move(hello)), expected_(std::move(expected)), wake_(std::move(wake)),
      handshake_deadline_(Clock::now() + config.handshake_timeout), connect_deadline_(std::min(handshake_deadline_, Clock::now() + config.connect_timeout)) {}

// enqueue 函数实现: 将需要传出的消息包裹及对应的超时压入环形队列.
// 如果超出容量 (队列容量固定为4)，返回 false，保证背压和流量控制。
bool RpcSession::enqueue(proto::astra::v1::SessionPacket message, Clock::time_point deadline) {
    // 检查积压消息是否达到了队列最大容量
    if (queued_ == queue_.size()) {
        return false;
    }
    // 已由容量检查保证的内部不变量, 不用契约直接检查网络输入.
    contract_assert(queued_ < queue_.size());
    // 利用模运算计算写位置，保存数据和超时期限。
    queue_[(head_ + queued_) % queue_.size()] = PendingPacket{std::move(message), deadline};
    ++queued_;
    return true;
}

// receive 函数实现: 分析刚刚读到的通信协议帧，处理身份检查与心跳 ping-pong，拦截非预期消息
Result<std::vector<SessionGeneration>> RpcSession::receive(const proto::astra::v1::SessionPacket& packet, Policy& policy, const Identity& identity,
                                                           Clock::time_point now) {
    // 基础包大小限制检查，如果对方在协商后的限额之上继续发大包则视为容量超额协议违规
    if (packet.ByteSizeLong() > maximum_) {
        return Error::capacity("Control message exceeds negotiated limit");
    }
    // 检查对方是否明确下发了拒绝我们通信请求的 rejection 包
    if (packet.has_rejection()) {
        return std::unexpected(
            Error{packet.rejection().code() == proto::astra::v1::PROTOCOL_ERROR_CODE_UNAUTHORIZED ? Error::Code::identity : Error::Code::protocol,
                  "Remote star rejected session"});
    }
    
    // 如果该会话尚未完成“安装” (即身份及握手验证未通过阶段)：
    if (!installed_) {
        // 首帧必须为兼容 Hello, 验签与角色接纳成功后才能安装逻辑身份和协商控制帧上限.
        if (!packet.has_hello() || packet.hello().protocol_major() != protocol_major || packet.hello().max_frame_bytes() < 1024) {
            return Error::protocol("Expected compatible Hello");
        }
        
        // 使用本地安全体系来查验所给 Hello 中包含凭证的合法性签名，解码还原成员信息
        auto member = identity.verify(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(packet.hello().admission().data()), packet.hello().admission().size()),
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(packet.hello().admission_signature().data()),
                                          packet.hello().admission_signature().size()));
        if (!member) {
            return std::unexpected(member.error());
        }
        
        // 传递给当前运行策略判定远端成员的准入条件和资格冲突
        auto install = policy.accept(*member, direction_, generation_, expected_);
        if (!install) {
            return std::unexpected(install.error());
        }
        
        // 所有校验均通过后将状态设置已完成初次就绪安装阶段
        installed_ = true;
        if (direction_ == Direction::inbound) {
            // 凭证是 bearer, 对于入站连接，被动方只向已经通过准入和角色验证的调用方返回本端 Hello, 用作双向握手的响应.
            proto::astra::v1::SessionPacket reply;
            reply.mutable_hello()->CopyFrom(*hello_);
            if (!enqueue(std::move(reply), handshake_deadline_)) {
                return Error::capacity("Hello send queue is full");
            }
        }
        
        // 敲定双方接受的每包最大载荷：取双方宣称数值及本段系统最大支持量 4096 间的极小值
        maximum_ = std::min({4096U, hello_->max_frame_bytes(), packet.hello().max_frame_bytes()});
        // 重置下一次主动发往对端心跳的时间点为当前 + 心跳间隙参数
        next_ping_ = now + config_.heartbeat_interval;
        return install;
    }
    
    // 握手完成之后：仅处理带非零请求号的 Ping/Pong, 只有匹配本端待确认 Ping 的 Pong 才推进心跳期限.
    if (packet.has_ping() && packet.ping().request_id() != 0) {
        // 如果对端发起的是一个合规非 0 序号的心跳检测, 就响应相对应的 pong 数据包
        proto::astra::v1::SessionPacket reply;
        reply.mutable_pong()->set_request_id(packet.ping().request_id());
        if (!enqueue(std::move(reply), now + config_.pong_timeout)) {
            return Error::capacity("Control send queue is full");
        }
    } else if (packet.has_pong() && packet.pong().request_id() != 0) {
        // 如果收到心跳应答 pong，且当前确实存在处于等待中的 ping 请求记录：
        if (pending_ping_ && packet.pong().request_id() == pending_ping_->first) {
            // 清空本地挂起记录并重新安排下一个检测周期
            pending_ping_.reset();
            next_ping_ = now + config_.heartbeat_interval;
        }
    } else {
        // 握手后接收到了无法识别、或是异常类型的非正常生命期载荷
        return Error::protocol("Unexpected control message");
    }
    
    // 正常控制帧处理完了，无需剥夺其他任何会话操作
    return std::vector<SessionGeneration>{};
}

// pump 函数实现: 上层唯一入口，定期执行，检测状态，排队发数据或者获取接收队列的新数据。
std::vector<SessionGeneration> RpcSession::pump(Policy& policy, const Identity& identity, Clock::time_point now) {
    if (done()) {
        return {}; // 已经结束，直接跳过处理。
    }
    
    if (!started_) {
        // 连接和 Hello 共用最初的总预算, 拨号阶段另有更短上限. 关闭时仍需完成已绑定 client call.
        if (!error_ && !transport_ready()) {
            if (now < connect_deadline_) {
                return {};
            }
            // 超出网络连接等待期限
            cancel(Error::Code::timeout);
        }
        
        // gRPC 允许 StartCall 前准备读写; Runtime 在任何完成回调前已持有此对象.
        std::lock_guard lock(mutex_);
        started_ = true;
        if (!error_) {
            // 开始安排投递最初的读任务与相应的头/信息
            read_inflight_ = true;
            begin_read(&read_);
            if (direction_ == Direction::outbound) {
                // 如果是主动出站方，则要主动发送出自己的 hello 数据包
                write_.mutable_hello()->CopyFrom(*hello_);
                write_deadline_ = handshake_deadline_;
                write_inflight_ = true;
                begin_write(&write_);
            } else {
                // 如果是服务端入站，则首先要送出包含元数据(metadata)的包头部.
                metadata_inflight_ = true;
            }
        }
        if (direction_ == Direction::outbound || !error_) {
            begin_call();
        }
    }
    
    std::optional<proto::astra::v1::SessionPacket> message;
    bool transport_closed = false;
    bool write_expired = false;
    
    {
        // 加锁，将属于 I/O 线程交互的回调状态信息拷贝至本地控制流判定用
        std::lock_guard lock(mutex_);
        transport_closed = read_failed_ || write_failed_ || remote_cancelled_;
        // 检查写入动作是否超时，无论是元数据流阶段还是消息队列帧发送阶段
        write_expired = (write_inflight_ && now >= write_deadline_) || (metadata_inflight_ && now >= handshake_deadline_);
        // 如果当前表明有已经完整从底层收到的封包：
        if (read_ready_) {
            message.emplace();
            message->Swap(&read_);
            read_ready_ = false; // 清空本地已就绪标志位，准备下次接收
        }
    }
    
    // 根据从底层传回的判断判定是否传输链路出现了任何故障并中止：
    if (transport_closed) {
        cancel(Error::Code::transport);
    }
    
    std::vector<SessionGeneration> cancelled;
    // 截止优先于刚到达的 Pong/Hello, 排队和写入等待均包含在最初建立的绝对期限内.
    // Hello 和 Pong 的期限可能不同, 不能假定队首总是最早到期. 固定四槽检查不随节点规模增长.
    const bool queued_expired = std::ranges::any_of(queue_, [now](const auto& item) { return item && now >= item->deadline; });
    // 对发送超时、缓冲积压超时、以及未通过握手、丢失未回 pong 等情况分别计算时间条件，超时均终止进程：
    if (write_expired || queued_expired || (!installed_ && now >= handshake_deadline_) || (pending_ping_ && now >= pending_ping_->second)) {
        cancel(Error::Code::timeout);
    }
    
    // 如果没有抛出故障且已经提取了新收到的协议包，调用 receive() 层解析。
    if (!error_ && message) {
        auto result = receive(*message, policy, identity, now);
        if (!result) {
            cancel(result.error().code);
        } else {
            cancelled = std::move(*result);
        }
    }
    
    // 心跳判定周期：如果正常运转并且到了下一次应当抛出 ping 请求的时刻：
    if (!error_ && installed_ && !pending_ping_ && now >= next_ping_) {
        // 如果序列号跑满了（极其极端的情况），认为协议失效。
        if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
            cancel(Error::Code::protocol);
        } else {
            const auto id = next_id_++;
            // 设置当前正在等待的心跳标记
            pending_ping_ = std::pair{id, now + config_.pong_timeout};
            proto::astra::v1::SessionPacket ping;
            ping.mutable_ping()->set_request_id(id);
            // 将拼装好的心跳封入发出队伍.
            if (!enqueue(std::move(ping), now + config_.pong_timeout)) {
                cancel(Error::Code::capacity);
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
            // 只要目前无发送过程在路上，就调用下层真正的流终结操作。
            if (!write_inflight_ && !metadata_inflight_ && !finish_called_) {
                finish_called_ = true;
                finish_call(*error_);
            }
        } else if (!read_failed_ && !write_failed_ && !remote_cancelled_) {
            // 失败回调也可能在两次检查之间到达. 本轮停止提交新操作, 下一轮在锁外推进取消.
            if (!write_inflight_ && !metadata_inflight_ && queued_ != 0) {
                // 若底层已闲置且缓存列队有堆积，则取队首信息提交到下一轮 I/O
                write_.Swap(&queue_[head_]->packet);
                write_deadline_ = queue_[head_]->deadline;
                queue_[head_].reset(); // 释放原缓冲位
                head_ = (head_ + 1) % queue_.size();
                --queued_;
                write_inflight_ = true;
                begin_write(&write_);
            }
            
            // 当前流只有控制消息, 必须继续接收 Pong. 队列超限按 capacity 关闭, 不暂停心跳读取.
            // 未消费的 read_ 仍归控制循环, 不能重新交给 gRPC 写入.
            if (!read_inflight_ && !read_ready_) {
                read_inflight_ = true;
                begin_read(&read_); // 让 gRPC 继续向底层拉取下一个协议帧
            }
        }
    }
    // 返回因为本逻辑会话正常建立或取代过程导致的其余待取消的旧连结代次列表.
    return cancelled;
}

// cancel 函数实现: 设置中止状态和最后期限限额. 不立刻关停以备平稳退出或汇报正确异常码。
void RpcSession::cancel(Error::Code error) {
    if (!error_ && !done()) {
        error_ = error;
        cancel_deadline_ = Clock::now() + Milliseconds(250);
        request_cancel(false);
    }
}

// 接收读数据异步完成操作通报给系统事件层。
void RpcSession::read_done(bool ok) {
    {
        std::lock_guard lock(mutex_);
        read_inflight_ = false; // 清除 I/O 在途状态标志
        read_ready_ = ok;       // 是否得到了完好无损的新包裹
        read_failed_ = !ok;     // 失败标志为 ok 的反面
    }
    wake_(); // 回调外围总循环继续处理 pump
}

// 接收发数据异步完成通报。
void RpcSession::write_done(bool ok) {
    {
        std::lock_guard lock(mutex_);
        write_inflight_ = false;
        write_failed_ = !ok;
    }
    wake_();
}

// 服务端感知流主动远端中止，被动取消事件登记。
void RpcSession::server_cancelled() {
    {
        std::lock_guard lock(mutex_);
        remote_cancelled_ = true;
    }
    wake_();
}

// 仅用于服务端将 Metadata 报头提交发送的通报回环。
void RpcSession::metadata_done(bool ok) {
    {
        std::lock_guard lock(mutex_);
        metadata_inflight_ = false;
        write_failed_ = write_failed_ || !ok;
    }
    wake_();
}

// completed 函数实现: 处理 gRPC 生命末期的最终回执. 标记流程完成。
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

// error 函数实现: 取得本地记录或者从下层异常映射而来的明确报错。
std::optional<Error::Code> RpcSession::error() const {
    // 只有在最终完全关闭 (done() 为 true)，底层有错误，并且之前没设置过自定义上层错误（或者是含混的通信 error 时），才映射具体的底层 status。
    if (done() && !final_status_.ok() && (!error_ || *error_ == Error::Code::transport)) {
        return rpc_error_code(final_status_.error_code());
    }
    return error_;
}

// 工厂函数 connect_session 实例化真正的 client 端反应堆流。
std::shared_ptr<RpcSession> connect_session(const Config& config, const Identity& identity, SessionGeneration generation,
                                            std::shared_ptr<const proto::astra::v1::Hello> hello, Member target, std::function<void()> wake) {
    return std::make_shared<ClientSession>(config, identity, generation, std::move(hello), std::move(target), std::move(wake));
}

// AcceptedSession 成员函数定义
// 构造函数，将 direction 写死为 inbound，因为是服务端连接，不存在出站的 expected
AcceptedSession::AcceptedSession(grpc::CallbackServerContext* context, const Config& config, SessionGeneration generation,
                                 std::shared_ptr<const proto::astra::v1::Hello> hello, std::function<void()> wake)
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
void AcceptedSession::begin_read(proto::astra::v1::SessionPacket* message) {
    StartRead(message);
}
void AcceptedSession::begin_write(const proto::astra::v1::SessionPacket* message) {
    StartWrite(message);
}
void AcceptedSession::request_cancel(bool force) {
    if (force) {
        context_->TryCancel();
    }
}
void AcceptedSession::finish_call(Error::Code error) {
    // 将上层会话模块计算出的内部错误转换为 gRPC 对外公开的标准状态码系列以发回对端。
    const auto status = error == Error::Code::identity                                   ? grpc::StatusCode::PERMISSION_DENIED
                        : error == Error::Code::protocol || error == Error::Code::conflict ? grpc::StatusCode::INVALID_ARGUMENT
                                                                                       : grpc::StatusCode::CANCELLED;
    Finish(grpc::Status(status, "Session closed"));
}
} // namespace astra
