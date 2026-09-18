// 功能: 定义 Supervisor 准入状态机及异步 RPC 所有权, 保持请求地址和启动幂等键稳定.
// 该头文件主要声明了用于处理节点（Star/Planet）向 Supervisor 进行注册并获取准入凭证的相关结构和类。
#pragma once
#include "identity.hpp"
#include "orbit.grpc.pb.h"

#include <atomic>
#include <functional>

namespace astra {
// Joined 结构体用于存储成功注册后的准入结果信息。
struct Joined {
    // 可选的独立 Pulse 端点. 旧控制面为空, 不暗示具有可用对时功能.
    std::string pulse_endpoint;
    // local: 当前节点成功注册后从 Supervisor 获得的本地身份成员信息。
    Member local;
    // members: 准入响应中返回的当前集群/组内其他成员节点的列表。
    std::vector<Member> members;
    // hello: 用于与其他节点进行握手时的共享 Hello 消息载荷，包含准入凭证和签名。
    std::shared_ptr<const proto::astra::v1::Hello> hello;
};

// Admission 类: 唯一拥有 Supervisor RPC 的上下文和输入, 控制循环轮询完成, gRPC 回调不修改角色索引.
// 该类封装了与 Supervisor 交互的异步 gRPC 调用过程，并作为一个有限状态机（Idle -> Connecting -> Registration）运行。
class Admission {
public:
    // 构造函数: 复制已校验配置, 持有 identity, 进程 ID 由 Supervisor 签发, wake 须可由 gRPC 回调安全调用且不得借用 Runtime.
    // 构造只准备通道和稳定请求缓冲; 使用者必须在 pending 变为 false 后才能销毁对象.
    // 参数 config: 包含节点的全局配置（如地址，角色，超时等），无默认值。
    // 参数 identity: 提供 TLS 凭证及签名的身份对象指针，无默认值。
    // 参数 wake: 异步回调完成时用于唤醒主控制循环的回调函数，无默认值。
    Admission(const Config& config, std::shared_ptr<Identity> identity, std::function<void()> wake);

    // gRPC 借用成员中的请求地址, Admission 从构造到排空回调都不能搬移.
    // 禁用拷贝构造函数以保证内存地址的稳定性。
    Admission(const Admission&) = delete;

    // 禁止复制赋值, 防止替换仍被 gRPC 借用的请求和上下文.
    Admission& operator=(const Admission&) = delete;

    // 禁止移动构造, 保持已提交请求的地址不变.
    Admission(Admission&&) = delete;

    // 禁止移动赋值, 防止在途 RPC 指向旧对象存储.
    Admission& operator=(Admission&&) = delete;

    // begin: 没有在途 RPC 时开始一次尝试, candidate_round 只影响 Planet 候选, 不改变启动请求.
    // 参数 candidate_round: 当前进行到第几轮候选更新，默认影响向 Supervisor 提交的请求内容。无默认值。
    void begin(std::uint32_t candidate_round);

    // poll: 返回完成的一次结果; nullopt 表示等待. 响应对象在完成回调结束前始终由共享状态持有.
    // 该方法由主控制循环定期调用以检查并推进异步注册的进度。
    // 返回值: 如果注册成功，返回包含 Joined 数据的结果；若出错则返回 Error；若仍在等待中则返回 std::nullopt。
    std::optional<Result<Joined>> poll();

    // cancel: 由控制循环永久取消此 Admission, 对在途 RPC 发出 TryCancel; 仍须 poll 到 pending 为 false 后销毁.
    // 取消所有正在进行的异步操作。
    void cancel();

    // pending: 由控制循环查询是否仍有未消费的尝试, 包括连接等待阶段; true 不表示已经提交 Register.
    // 返回值: bool 类型，true 表示状态机不处于 idle，有任务正在进行或等待中。
    bool pending() const;

private:
    // RegistrationCall 结构体: 封装了一次 gRPC Register 调用的所有相关数据和状态。
    struct RegistrationCall {
        // context: gRPC 客户端调用的上下文，用于设置超时和取消。
        grpc::ClientContext context;
        // response: gRPC 调用的响应消息缓冲。
        proto::orbit::v1::RegistrationResponse response;
        // status: gRPC 调用完成后的状态码和错误信息。
        grpc::Status status;
        // done: 原子布尔变量，标记异步 RPC 回调是否已经执行完毕。默认值为 false。
        std::atomic_bool done{false};
    };

    // Phase 枚举: 只由控制循环切换; 回调仅发布登记调用的完成状态.
    enum class Phase {
        // idle: 当前尝试未启动或结果已消费; 若未取消, begin 可以启动下一次尝试.
        idle,
        // connecting: 等待 Supervisor TLS 通道就绪, 同时受连接期限和握手总期限约束.
        connecting,
        // registration: Register RPC 在途, 申请准入或刷新 Planet 候选.
        registration
    };

    // start_registration: 仅在通道就绪且无登记 RPC 在途时调用, 使用剩余总预算提交 Register 并发布回调完成标志.
    void start_registration();

    // validate: 在 Register 完成后核对容量, 本地部署和已安装身份, 返回独立名单与共享 Hello.
    // response 的准入字节会移入 Hello; 失败返回 capacity 或 identity, 名单角色策略由 initialize 继续校验.
    // 参数 response: RPC 成功返回的响应报文，包含准入签名和对端成员列表。
    // 返回值: 校验成功返回 Joined，失败返回错误。
    Result<Joined> validate(proto::orbit::v1::RegistrationResponse& response);

    // config_: 存储传入的全局配置。
    Config config_;
    // identity_: 指向身份对象的共享指针。
    std::shared_ptr<Identity> identity_;
    // local_: 首次成功后固定身份, 候选刷新不能默默替换本进程. 初始为 std::nullopt。
    std::optional<Member> local_;
    // wake_: 唤醒主事件循环的闭包函数。
    std::function<void()> wake_;
    // channel_: 与 Supervisor 的 gRPC 连接通道。
    std::shared_ptr<grpc::Channel> channel_;
    // stub_: 与 Supervisor 通信的 gRPC 客户端存根。
    std::unique_ptr<proto::orbit::v1::Admission::Stub> stub_;
    // phase_: 当前的状态机阶段，默认为 Phase::idle (由于 {} 初始化)。
    Phase phase_{};
    // cancelled_: 标志是否已被要求取消操作，默认为 false (由于 {} 初始化)。
    bool cancelled_{};
    // deadline_: 单次 begin 启动后的整体截止时间。
    Clock::time_point deadline_;
    // connect_deadline_: 仅用于连接阶段的截止时间。
    Clock::time_point connect_deadline_;
    // request_: 复用的注册请求消息体。
    proto::orbit::v1::RegistrationRequest request_;
    // registration_: 当前在途的 RegistrationCall 实例指针，无在途时为 nullptr。
    std::shared_ptr<RegistrationCall> registration_;
};

} // namespace astra
