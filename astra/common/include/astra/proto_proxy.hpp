// 标量调用 set_, 子消息通过 mutable_ 赋值, repeated 字段整体替换; 实际分配遵循 Protobuf 的语义.
#pragma once

#include <utility>

#include "astra.pb.h"
#include "orbit.pb.h"
#include "pulsar.pb.h"

namespace astra {

// Message 为已显式适配的生成消息类型; 未适配类型不提供通用反射或运行期兜底.
// Input 为字段输入类型, value 保留原值类别转发; 字段合法性仍由协议消费方检查.
template <typename Message>
struct Mutator;

// Hello 问候代理, 链式设置协议版本、帧上限与准入材料.
template <>
struct Mutator<::proto::astra::v1::Hello> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::Hello& message;

    // 设置 protocol_major 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto protocol_major(Input&& value) {
        message.set_protocol_major(std::forward<Input>(value));
        return *this;
    }

    // 设置 protocol_minor 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto protocol_minor(Input&& value) {
        message.set_protocol_minor(std::forward<Input>(value));
        return *this;
    }

    // 设置 max_frame_bytes 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto max_frame_bytes(Input&& value) {
        message.set_max_frame_bytes(std::forward<Input>(value));
        return *this;
    }

    // 设置 admission 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto admission(Input&& value) {
        message.set_admission(std::forward<Input>(value));
        return *this;
    }

    // 设置 admission_signature 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto admission_signature(Input&& value) {
        message.set_admission_signature(std::forward<Input>(value));
        return *this;
    }
};

// Astra Ping 代理, 设置会话保活序号, 不参与 Pulsar 四时间戳对时.
template <>
struct Mutator<::proto::astra::v1::Ping> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::Ping& message;

    // 设置 request_id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto request_id(Input&& value) {
        message.set_request_id(std::forward<Input>(value));
        return *this;
    }
};

// Astra Pong 代理, 回显会话保活序号, 不携带对时时间戳.
template <>
struct Mutator<::proto::astra::v1::Pong> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::Pong& message;

    // 设置 request_id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto request_id(Input&& value) {
        message.set_request_id(std::forward<Input>(value));
        return *this;
    }
};

// Pulsar Ping 代理, 链式设置对时探测字段.
template <>
struct Mutator<::proto::pulsar::v1::Ping> {
    // message 借用发送时间采样消息, 代理不拥有也不延长其寿命.
    ::proto::pulsar::v1::Ping& message;

    // 设置 Star 发送时的单调纳秒数, 不在代理内读取或转换时钟.
    template <typename Input>
    auto t0(Input&& value) {
        message.set_t0(std::forward<Input>(value));
        return *this;
    }
};

// Pulsar Pong 代理, 链式设置对时应答字段.
template <>
struct Mutator<::proto::pulsar::v1::Pong> {
    // message 借用四时间戳响应消息, 修改必须避开在途的 gRPC Write.
    ::proto::pulsar::v1::Pong& message;

    // 原样回显对应的 T0, 由 Star 匹配唯一在途请求.
    template <typename Input>
    auto t0(Input&& value) {
        message.set_t0(std::forward<Input>(value));
        return *this;
    }

    // 设置 Pulsar 接收后的 Unix 纳秒估计.
    template <typename Input>
    auto t1(Input&& value) {
        message.set_t1(std::forward<Input>(value));
        return *this;
    }

    // 设置 Pulsar 提交发送前的 Unix 纳秒估计.
    template <typename Input>
    auto t2(Input&& value) {
        message.set_t2(std::forward<Input>(value));
        return *this;
    }

    // 设置包含上游误差与残余校正的纳秒误差估计.
    template <typename Input>
    auto uncertainty_ns(Input&& value) {
        message.set_uncertainty_ns(std::forward<Input>(value));
        return *this;
    }

    // 来源已校准且质量达标, 未设置时保持 false.
    template <typename Input>
    auto synchronized(Input&& value) {
        message.set_synchronized(std::forward<Input>(value));
        return *this;
    }

    // 报告 Pulsar 单调时钟 rho, 用于 Star 的精度容差和误差估计.
    template <typename Input>
    auto precision_ns(Input&& value) {
        message.set_precision_ns(std::forward<Input>(value));
        return *this;
    }
};

// 协议错误代理, 链式设置错误分类与原因.
template <>
struct Mutator<::proto::astra::v1::ProtocolError> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::ProtocolError& message;

    // 设置 code 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto code(Input&& value) {
        message.set_code(std::forward<Input>(value));
        return *this;
    }
};

// 会话帧代理, 提供 Hello/Ping/Pong/错误的便捷写入; 其他业务帧直接使用生成接口.
template <>
struct Mutator<::proto::astra::v1::SessionPacket> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SessionPacket& message;

    // 设置 hello 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto hello(Input&& value) {
        *message.mutable_hello() = std::forward<Input>(value);
        return *this;
    }

    // 设置 ping 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto ping(Input&& value) {
        *message.mutable_ping() = std::forward<Input>(value);
        return *this;
    }

    // 设置 pong 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto pong(Input&& value) {
        *message.mutable_pong() = std::forward<Input>(value);
        return *this;
    }

    // 设置 rejection 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto rejection(Input&& value) {
        *message.mutable_rejection() = std::forward<Input>(value);
        return *this;
    }
};

// 登记请求代理, 链式设置成员描述与请求标识.
template <>
struct Mutator<::proto::orbit::v1::RegistrationRequest> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::orbit::v1::RegistrationRequest& message;

    // 设置 galaxy 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto galaxy(Input&& value) {
        message.set_galaxy(std::forward<Input>(value));
        return *this;
    }

    // 设置 advertise 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto advertise(Input&& value) {
        message.set_advertise(std::forward<Input>(value));
        return *this;
    }

    // 设置 role 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto role(Input&& value) {
        message.set_role(std::forward<Input>(value));
        return *this;
    }

    // 设置 group 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto group(Input&& value) {
        message.set_group(std::forward<Input>(value));
        return *this;
    }

    // 设置 candidate_round 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto candidate_round(Input&& value) {
        message.set_candidate_round(std::forward<Input>(value));
        return *this;
    }

    // 设置 username 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto username(Input&& value) {
        message.set_username(std::forward<Input>(value));
        return *this;
    }

    // 设置 password 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto password(Input&& value) {
        message.set_password(std::forward<Input>(value));
        return *this;
    }

    // 设置 request_id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto request_id(Input&& value) {
        message.set_request_id(std::forward<Input>(value));
        return *this;
    }
};

// 登记应答代理, 链式设置成员、目录与签名.
template <>
struct Mutator<::proto::orbit::v1::RegistrationResponse> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::orbit::v1::RegistrationResponse& message;

    // 设置 members 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto members(Input&& value) {
        *message.mutable_members() = std::forward<Input>(value);
        return *this;
    }

    // 设置 admission 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto admission(Input&& value) {
        message.set_admission(std::forward<Input>(value));
        return *this;
    }

    // 设置 signature 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto signature(Input&& value) {
        message.set_signature(std::forward<Input>(value));
        return *this;
    }
};

// 成员描述代理, 链式设置身份、端点、角色与代次.
template <>
struct Mutator<::proto::orbit::v1::Member> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::orbit::v1::Member& message;

    // 设置 galaxy 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto galaxy(Input&& value) {
        message.set_galaxy(std::forward<Input>(value));
        return *this;
    }

    // 设置 id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto id(Input&& value) {
        message.set_id(std::forward<Input>(value));
        return *this;
    }

    // 设置 principal 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto principal(Input&& value) {
        message.set_principal(std::forward<Input>(value));
        return *this;
    }

    // 设置 advertise 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto advertise(Input&& value) {
        message.set_advertise(std::forward<Input>(value));
        return *this;
    }

    // 设置 epoch 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto epoch(Input&& value) {
        message.set_epoch(std::forward<Input>(value));
        return *this;
    }

    // 设置 role 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto role(Input&& value) {
        message.set_role(std::forward<Input>(value));
        return *this;
    }

    // 设置 group 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto group(Input&& value) {
        message.set_group(std::forward<Input>(value));
        return *this;
    }
};

// mutate 创建只借用 message 的代理; 不延长消息寿命, 不进行序列化或分配.
// message 为目标消息; 返回链式代理, 寿命不得超过消息.
template <typename Message>
inline Mutator<Message> mutate(Message& message) {
    return {message};
}

} // namespace astra
