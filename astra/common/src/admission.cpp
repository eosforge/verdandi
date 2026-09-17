// 功能: 推进连接和单次登记阶段, 校验准入结果, 在统一截止内处理重试与取消.
// 该文件实现了 Admission 类的具体逻辑，负责与 Supervisor 建立 gRPC 通道，发送注册请求，并验证响应。
#include "admission.hpp"
#include "rpc_status.hpp"
#include <astra/proto_proxy.hpp>

#include <algorithm>
#include <utility>

#include <grpcpp/create_channel.h>
#include <openssl/rand.h>

namespace astra {
namespace {
// rpc_deadline: 计算相对当前的截止时间，gRPC 接受 system_clock 截止, 剩余预算始终从单调时钟计算, 不在每次 RPC 重置总期限.
// 参数 deadline: 基于单调时钟 (steady_clock) 的绝对截止时间。
// 返回值: 转换到 system_clock 的等效时间点。
auto rpc_deadline(Clock::time_point deadline) {
    // std::max 防止时间倒流导致负的持续时间
    return std::chrono::system_clock::now() + std::max(deadline - Clock::now(), Clock::duration::zero());
}
} // namespace

// 构造函数实现: 初始化配置、身份和唤醒回调，并设置 gRPC 通道和初始的请求数据。
Admission::Admission(const Config& config, std::shared_ptr<Identity> identity, std::function<void()> wake)
    : config_(config), identity_(std::move(identity)), wake_(std::move(wake)) {
    // 禁用 gRPC 内建重试, 使应用层复用启动幂等键并拥有完整尝试截止与失败分类.
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(static_cast<int>(config_.max_admission_response_bytes));
    args.SetMaxSendMessageSize(static_cast<int>(config_.max_admission_request_bytes));
    args.SetInt("grpc.enable_retries", 0); // 关闭重试机制
    // 根据提供的 Supervisor 地址和 TLS 凭据创建 gRPC 通道
    channel_ = grpc::CreateCustomChannel(config_.supervisor, identity_->channel_credentials(), args);
    // 实例化对应的 RPC Stub
    stub_ = proto::orbit::v1::Admission::NewStub(channel_);

    // 随机值只用于幂等, 不成为 Star 身份. 整个进程的重试和候选刷新始终复用它.
    std::string request_id(32, '\0');
    // 生成 32 字节的安全随机数作为请求的唯一标识 (幂等键)
    if (RAND_bytes(reinterpret_cast<unsigned char*>(request_id.data()), request_id.size()) != 1) {
        throw std::runtime_error("Startup request entropy unavailable");
    }
    // 填充将要发送到 Supervisor 的请求消息基础字段
    mutate(request_)
        .request_id(std::move(request_id))
        .username(identity_->username())
        .password(identity_->password())
        .galaxy(config_.galaxy)
        .advertise(config_.advertise.text())
        .role(config_.role == Role::star ? proto::orbit::v1::ROLE_STAR : proto::orbit::v1::ROLE_PLANET)
        .group(config_.group);
}

// begin: 启动一次新的准入尝试或候选刷新。
// 参数 candidate_round: 本次请求所属的轮次。
void Admission::begin(std::uint32_t candidate_round) {
    // 如果当前不在 idle 状态或者已经被取消，则直接返回，忽略新的开始请求。
    if (phase_ != Phase::idle || cancelled_) {
        return;
    }
    // 更新请求结构中的轮次
    request_.set_candidate_round(candidate_round);
    // 设定整体操作的超时截止时间 (握手超时)
    deadline_ = Clock::now() + config_.handshake_timeout;
    // 设定连接通道建立的截止时间，取整体超时与连接超时的较小值
    connect_deadline_ = std::min(deadline_, Clock::now() + config_.connect_timeout);
    // 状态变更为连接中
    phase_ = Phase::connecting;
    // 触发 gRPC 通道进行连接尝试，true 表示如果空闲则尝试连接
    channel_->GetState(true);
}

// start_registration: 实际发起异步 gRPC 注册调用的私有方法。
void Admission::start_registration() {
    // 创建一个新的调用上下文块
    registration_ = std::make_shared<RegistrationCall>();
    // 设定 RPC 的超时期限
    registration_->context.set_deadline(rpc_deadline(deadline_));
    // 切换到注册中状态
    phase_ = Phase::registration;
    // 异步调用 Register 接口
    stub_->async()->Register(&registration_->context, &request_, &registration_->response, [call = registration_, wake = wake_](grpc::Status status) {
        // 回调中保存返回的状态码
        call->status = std::move(status);
        // 使用 memory_order_release 标记原子完成状态，以便 poll() 能安全地读取到数据
        call->done.store(true, std::memory_order_release);
        // 唤醒主事件循环来处理完成事件
        wake();
    });
}

// poll: 主循环轮询状态机进展。
std::optional<Result<Joined>> Admission::poll() {
    // 如果是闲置状态，直接返回等待信号 (nullopt)
    if (phase_ == Phase::idle) {
        return std::nullopt;
    }
    // 通道就绪后只发一次登记 RPC, 连接等待和登记共享同一次尝试的总期限.
    if (phase_ == Phase::connecting) {
        // 检查是否取消或超时
        if (cancelled_ || Clock::now() >= connect_deadline_) {
            // 退回空闲状态，并返回相应的错误
            phase_ = Phase::idle;
            return std::unexpected(Error{cancelled_ ? Error::Code::cancelled : Error::Code::timeout, "Supervisor connection did not become ready"});
        }
        // 如果通道还未达到 READY 状态，则继续等待
        if (channel_->GetState(true) != GRPC_CHANNEL_READY) {
            return std::nullopt;
        }
        // 通道已就绪，发起注册
        start_registration();
    }
    // 检查注册阶段是否完成 (acquire 屏障保证读取回调写出的数据)
    if (phase_ == Phase::registration && registration_->done.load(std::memory_order_acquire)) {
        phase_ = Phase::idle;
        // 成功和失败都消费当前调用. 回调仍持有自己的 shared_ptr, 发布完成后只执行独立唤醒.
        // 将 registration_ 指针交换为空，转移所有权
        auto completed = std::exchange(registration_, {});
        // 检查 RPC 调用是否成功或被整体取消
        if (!completed->status.ok() || cancelled_) {
            return std::unexpected(cancelled_ ? Error{Error::Code::cancelled, "Admission cancelled"} : rpc_error(completed->status));
        }
        // 调用成功，进行业务层面的有效性校验
        return validate(completed->response);
    }
    return std::nullopt;
}

// validate: 校验 Supervisor 发回的响应数据。
// 参数 response: 从 gRPC 接收到的 RegistrationResponse 对象引用。
Result<Joined> Admission::validate(proto::orbit::v1::RegistrationResponse& response) {
    const auto count = static_cast<std::size_t>(response.members_size());
    // 检查返回的节点成员数量是否超过硬限制，或者超过配置所允许的角色容量限制
    if (count > 4096 || (config_.role == Role::planet && count > 8) || (config_.role == Role::star && count > config_.max_members)) {
        return Error::capacity("Admission list exceeds role capacity");
    }
    // 对原始准入字节验签并核对部署. 首次接受 Supervisor 签发的身份, 后续刷新不得更换它.
    // 调用身份类的 verify 进行公钥验签和解析
    auto local =
        identity_->verify(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(response.admission().data()), response.admission().size()),
                          std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(response.signature().data()), response.signature().size()));
    // 确保验签通过，并且其内容与当前节点的配置预期一致；如果是刷新请求，还要保证与先前的身份不变
    if (!local || local->galaxy != config_.galaxy || local->group != config_.group || local->role != config_.role || local->address != config_.advertise ||
        local->principal != identity_->principal(config_.galaxy, config_.advertise.text()) || (local_ && *local != *local_)) {
        return Error::identity("Admission credential does not match this process");
    }
    // 名单转换为拥有的成员值, 生成消息只留在适配层; 完整角色关系由后续 Policy 初始化检查.
    std::vector<Member> members;
    members.reserve(count);
    // 遍历响应中包含的所有成员节点
    for (const auto& encoded : response.members()) {
        auto member = decode_member(encoded); // 解码字节流为 Member 结构体
        if (!member) {
            return std::unexpected(member.error());
        }
        members.push_back(std::move(*member));
    }
    // 更新或确认本地身份
    local_ = *local;
    // 构造即将用于与其他节点通讯的 Hello 消息，并预先填充相关属性
    auto hello = std::make_shared<proto::astra::v1::Hello>();
    mutate(*hello)
        .protocol_major(protocol_major)
        .protocol_minor(0U)
        .max_frame_bytes(config_.max_frame_bytes)
        .admission(std::move(*response.mutable_admission()))
        .admission_signature(std::move(*response.mutable_signature()));

    // 返回成功验证后的完整 Joined 对象
    return Joined{std::move(*local), std::move(members), std::move(hello)};
}

// cancel: 发出取消信号。
void Admission::cancel() {
    cancelled_ = true; // 标志位设为真，阻止新的开始和继续
    // 如果有正在进行的注册 RPC 调用，则尝试通过 gRPC Context 取消
    if (phase_ == Phase::registration) {
        registration_->context.TryCancel();
    }
}

// pending: 返回是否有操作正在处理中。
bool Admission::pending() const {
    return phase_ != Phase::idle;
}
} // namespace astra
