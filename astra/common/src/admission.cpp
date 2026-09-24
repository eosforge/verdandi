// 该文件实现了 Admission 类的具体逻辑, 负责与 Pulsar 建立 gRPC 通道, 发送注册请求, 并验证响应.
#include "admission.hpp"
#include "rpc_status.hpp"
#include <astra/proto_proxy.hpp>

#include <algorithm>
#include <utility>

#include <grpc/support/time.h>
#include <grpcpp/create_channel.h>
#include <openssl/rand.h>

namespace astra {
namespace {
// 将本地剩余预算映射到 gRPC 单调时钟, 不经过可能被 NTP 调整的墙钟, 不重置总期限.
auto rpc_deadline(Steady::time_point deadline) {
    // remaining 为整体截止剩余纳秒数, 已过期按零处理, 不因新建 RPC 重置预算.
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(std::max(deadline - Steady::now(), Steady::duration::zero())).count();
    return gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_nanos(remaining, GPR_TIMESPAN));
}
} // namespace

// 构造函数实现: 初始化配置, 身份和唤醒回调, 并设置 gRPC 通道和初始的请求数据.
Admission::Admission(const Config& config, std::shared_ptr<Identity> identity, std::function<void()> wake) : config_(config), identity_(std::move(identity)), wake_(std::move(wake)) {

    // 禁用 gRPC 内建重试, 使应用层复用启动幂等键并拥有完整尝试截止与失败分类.
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(static_cast<int>(config_.max_admission_response_bytes));
    args.SetMaxSendMessageSize(static_cast<int>(config_.max_admission_request_bytes));
    args.SetInt("grpc.enable_retries", 0); // 关闭重试机制
    // 根据提供的 Pulsar 地址和 TLS 凭据创建 gRPC 通道
    channel_ = grpc::CreateCustomChannel(config_.pulsar, identity_->channel_credentials(), args);
    // 实例化对应的 RPC Stub
    stub_ = proto::orbit::v1::Admission::NewStub(channel_);

    // 随机值只用于幂等, 不成为 Star 身份. 整个进程的重试和候选刷新始终复用它.
    std::string request_id(32, '\0');
    // 生成 32 字节的安全随机数作为请求的唯一标识 (幂等键)
    if (RAND_bytes(reinterpret_cast<unsigned char*>(request_id.data()), request_id.size()) != 1) {
        throw std::runtime_error("Startup request entropy unavailable");
    }

    // 填充将要发送到 Pulsar 的请求消息基础字段
    mutate(request_).request_id(std::move(request_id)).username(identity_->username()).password(identity_->password()).galaxy(config_.galaxy).advertise(config_.advertise.text()).role(Identity::role(config_.role)).group(config_.group);
}

// begin: 启动一次新的准入尝试或候选刷新.
// 参数 candidate_round: 本次请求所属的轮次.
void Admission::begin(std::uint32_t candidate_round) {

    // 如果当前不在 idle 状态或者已经被取消, 则直接返回, 忽略新的开始请求.
    if (phase_ != Admission::Phase::idle || cancelled_) {
        return;
    }

    // 更新请求结构中的轮次
    request_.set_candidate_round(candidate_round);
    // 设定整体操作的超时截止时间 (握手超时)
    deadline_ = Steady::now() + config_.handshake_timeout;
    // 设定连接通道建立的截止时间, 取整体超时与连接超时的较小值
    connect_deadline_ = std::min(deadline_, Steady::now() + config_.connect_timeout);
    // 状态变更为连接中
    phase_ = Admission::Phase::connecting;
    // 触发 gRPC 通道进行连接尝试, true 表示如果空闲则尝试连接
    channel_->GetState(true);
}

// start_registration: 实际发起异步 gRPC 注册调用的私有方法.
void Admission::start_registration() {

    // 创建一个新的调用上下文块
    registration_ = std::make_shared<Admission::Call>();
    // 设定 RPC 的超时期限
    registration_->context.set_deadline(rpc_deadline(deadline_));
    // 切换到注册中状态
    phase_ = Admission::Phase::registration;
    if (config_.role == Member::Role::star && hello_) {
        registration_->listing = true;
        registration_->context.AddMetadata("astra-admission-bin", hello_->admission());
        registration_->context.AddMetadata("astra-signature-bin", hello_->admission_signature());
        stub_->async()->List(&registration_->context, &query_, &registration_->directory, [call = registration_, wake = wake_](grpc::Status status) {
            call->status = std::move(status); // 回调只拥有一次查询的缓冲, 不接触角色状态.
            call->done.store(true, std::memory_order_release);
            wake();
        });
        return;
    }
    // 异步调用 Register 接口
    stub_->async()->Register(&registration_->context, &request_, &registration_->response, [call = registration_, wake = wake_](grpc::Status status) {
        // 回调中保存返回的状态码
        call->status = std::move(status);
        // 使用 memory_order_release 标记原子完成状态, 以便 poll() 能安全地读取到数据
        call->done.store(true, std::memory_order_release);
        // 唤醒主事件循环来处理完成事件
        wake();
    });
}

// poll: 主循环轮询状态机进展.
std::optional<Result<Admission::Joined>> Admission::poll() {

    // 如果是闲置状态, 直接返回等待信号 (nullopt)
    if (phase_ == Admission::Phase::idle) {
        return std::nullopt;
    }

    // 通道就绪后只发一次登记 RPC, 连接等待和登记共享同一次尝试的总期限.
    if (phase_ == Admission::Phase::connecting) {
        // 检查是否取消或超时
        if (cancelled_ || Steady::now() >= connect_deadline_) {
            // 退回空闲状态, 并返回相应的错误
            phase_ = Admission::Phase::idle;
            return std::unexpected(Status{cancelled_ ? Status::Code::cancelled : Status::Code::timeout, "Pulsar connection did not become ready"});
        }

        // 如果通道还未达到 READY 状态, 则继续等待
        if (channel_->GetState(true) != GRPC_CHANNEL_READY) {
            return std::nullopt;
        }

        // 通道已就绪, 发起注册
        start_registration();
    }

    // 检查注册阶段是否完成 (acquire 屏障保证读取回调写出的数据)
    if (phase_ == Admission::Phase::registration && registration_->done.load(std::memory_order_acquire)) {
        phase_ = Admission::Phase::idle;
        // 成功和失败都消费当前调用. 回调仍持有自己的 shared_ptr, 发布完成后只执行独立唤醒.
        // 将 registration_ 指针交换为空, 转移所有权
        auto completed = std::exchange(registration_, {});
        if (completed->listing && (completed->status.error_code() == grpc::StatusCode::UNAUTHENTICATED || completed->status.error_code() == grpc::StatusCode::PERMISSION_DENIED)) {
            revoked_ = true;
        }
        // 检查 RPC 调用是否成功或被整体取消
        if (!completed->status.ok() || cancelled_) {
            return std::unexpected(cancelled_ ? Status{Status::Code::cancelled, "Admission cancelled"} : rpc_error(completed->status));
        }

        if (completed->listing) {
            // 复用原准入验证路径, 只读查询不提供也不改变自己的签名身份与对时端点.
            completed->response.mutable_members()->Swap(completed->directory.mutable_members());
            completed->response.set_admission(hello_->admission());
            completed->response.set_signature(hello_->admission_signature());
            completed->response.set_pulse_endpoint(pulse_endpoint_);
        }
        // 调用成功, 进行业务层面的有效性校验
        return validate(completed->response);
    }
    return std::nullopt;
}

// validate: 校验 Pulsar 发回的响应数据.
// 参数 response: 从 gRPC 接收到的 RegistrationResponse 对象引用.
Result<Admission::Joined> Admission::validate(proto::orbit::v1::RegistrationResponse& response) {

    // 对时端点来自同一受信 TLS 控制面, 单独校验格式后交给持有独立 TLS 通道的采样器.
    std::string pulse_endpoint;
    if (!response.pulse_endpoint().empty()) {
        // endpoint 为受信控制面提供的 Pulse 地址解析结果, 不接受非法主机和端口.
        auto endpoint = Config::format_pulsar(response.pulse_endpoint());
        if (!endpoint) {
            return Status::identity("Invalid Pulse endpoint");
        }
        pulse_endpoint = std::move(*endpoint);
    }

    // count 为返回名单长度, 先检查角色容量再分配本地成员数组.
    const auto count = static_cast<std::size_t>(response.members_size());
    // 检查返回的节点成员数量是否超过硬限制, 或者超过配置所允许的角色容量限制
    if (count > 8192 || (config_.role == Member::Role::planet && count > 8)) {
        return Status::capacity("Admission list exceeds role capacity");
    }

    // 对原始准入字节验签并核对部署. 首次接受 Pulsar 签发的身份, 后续刷新不得更换它.
    // 调用身份类的 verify 进行公钥验签和解析
    auto local = identity_->verify(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(response.admission().data()), response.admission().size()), std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(response.signature().data()), response.signature().size()));
    // 确保验签通过, 并且其内容与当前节点的配置预期一致; 如果是刷新请求, 还要保证与先前的身份不变
    if (!local || local->galaxy != config_.galaxy || local->group != config_.group || local->role != config_.role || local->address != config_.advertise || local->principal != identity_->principal(config_.galaxy, config_.advertise.text()) || (local_ && *local != *local_)) {
        return Status::identity("Admission credential does not match this process");
    }

    // 名单转换为拥有的成员值, 生成消息只留在适配层; 完整角色关系由后续 Policy 初始化检查.
    std::vector<Member> members;
    members.reserve(count);
    // services 仅保留控制面目标, 单点 Polaris 限制在这里明确检查, 不按可达性任选一个.
    std::vector<Member> services;
    // 遍历响应中包含的所有成员节点
    for (const auto& encoded : response.members()) {
        auto member = decode_member(encoded); // 解码字节流为 Member 结构体
        if (!member) {
            return std::unexpected(member.error());
        }
        if (member->galaxy != config_.galaxy) {
            return Status::identity("Directory galaxy mismatch");
        }
        if (member->role == Member::Role::polaris && config_.role == Member::Role::star) {
            if (!services.empty() || member->id == local->id || member->principal == local->principal || member->address == local->address) {
                return Status::identity("Conflicting Polaris deployment");
            }
            services.push_back(std::move(*member));
            continue;
        }
        if (member->role != Member::Role::star) {
            return Status::identity("Unexpected admission directory role");
        }
        members.push_back(std::move(*member));
    }
    if (members.size() > config_.max_members) {
        return Status::capacity("Star directory exceeds configured capacity");
    }
    // Star 的完整目录必须包含本次已签发身份. 缺失/被替换不更新缓存, 也不凭名单缺项自行重新登记.
    if (config_.role == Member::Role::star && std::ranges::find(members, *local) == members.end()) {
        return Status::identity("Directory does not contain this Star identity");
    }
    // 控制服务不能与任一 Star 共用身份或端点, 即使后续 Policy 只看 Star 数组也必须拒绝.
    if (!services.empty() && std::ranges::any_of(members, [&](const auto& member) { return member.id == services.front().id || member.principal == services.front().principal || member.address == services.front().address; })) {
        return Status::identity("Control service conflicts with Star directory");
    }

    // 更新或确认本地身份
    local_ = *local;
    // 构造即将用于与其他节点通讯的 Hello 消息, 并预先填充相关属性
    auto hello = std::make_shared<proto::astra::v1::Hello>();
    mutate(*hello).protocol_major(protocol_major).protocol_minor(0U).max_frame_bytes(config_.max_frame_bytes).admission(std::move(*response.mutable_admission())).admission_signature(std::move(*response.mutable_signature()));
    hello_ = hello;
    pulse_endpoint_ = pulse_endpoint;

    // 返回成功验证后的完整 Admission::Joined 对象
    return Admission::Joined{std::move(pulse_endpoint), std::move(*local), std::move(members), std::move(hello), std::move(services)};
}

// cancel: 发出取消信号.
void Admission::cancel() {

    cancelled_ = true; // 标志位设为真, 阻止新的开始和继续
    // 如果有正在进行的注册 RPC 调用, 则尝试通过 gRPC Context 取消
    if (phase_ == Admission::Phase::registration) {
        registration_->context.TryCancel();
    }
}

// pending: 返回是否有操作正在处理中.
bool Admission::pending() const {
    return phase_ != Admission::Phase::idle;
}

bool Admission::revoked() const noexcept {
    return revoked_;
}
} // namespace astra
