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

template <>
struct Mutator<::proto::astra::v1::SyncPacket> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SyncPacket& message;

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

    // 设置 request 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto request(Input&& value) {
        *message.mutable_request() = std::forward<Input>(value);
        return *this;
    }

    // 设置 response 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto response(Input&& value) {
        *message.mutable_response() = std::forward<Input>(value);
        return *this;
    }
};

template <>
struct Mutator<::proto::astra::v1::SyncRequest> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SyncRequest& message;

    // 设置 client_global_version 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto client_global_version(Input&& value) {
        message.set_client_global_version(std::forward<Input>(value));
        return *this;
    }

    // 设置 server_instance_id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto server_instance_id(Input&& value) {
        message.set_server_instance_id(std::forward<Input>(value));
        return *this;
    }

    // 设置 subscribe_prefix 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto subscribe_prefix(Input&& value) {
        message.set_subscribe_prefix(std::forward<Input>(value));
        return *this;
    }

    // 设置 cursor_prefix 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto cursor_prefix(Input&& value) {
        message.set_cursor_prefix(std::forward<Input>(value));
        return *this;
    }
};

template <>
struct Mutator<::proto::astra::v1::SyncResponse> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SyncResponse& message;

    // 设置 server_global_version 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto server_global_version(Input&& value) {
        message.set_server_global_version(std::forward<Input>(value));
        return *this;
    }

    // 设置 server_instance_id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto server_instance_id(Input&& value) {
        message.set_server_instance_id(std::forward<Input>(value));
        return *this;
    }

    // 设置 requires_full_snapshot 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto requires_full_snapshot(Input&& value) {
        message.set_requires_full_snapshot(std::forward<Input>(value));
        return *this;
    }

    // 设置 deltas 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto deltas(Input&& value) {
        *message.mutable_deltas() = std::forward<Input>(value);
        return *this;
    }
};

template <>
struct Mutator<::proto::astra::v1::DeltaRecord> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::DeltaRecord& message;

    // 设置 key 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto key(Input&& value) {
        message.set_key(std::forward<Input>(value));
        return *this;
    }

    // 设置 value 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto value(Input&& value) {
        message.set_payload(std::forward<Input>(value));
        return *this;
    }

    // 设置 deleted 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto deleted(Input&& value) {
        message.set_deleted(std::forward<Input>(value));
        return *this;
    }

    // 设置 version 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto version(Input&& value) {
        message.set_field_version(std::forward<Input>(value));
        return *this;
    }
};

template <>
struct Mutator<::proto::astra::v1::SnapshotRequest> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SnapshotRequest& message;

    // 设置 subscribe_prefix 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto subscribe_prefix(Input&& value) {
        message.set_subscribe_prefix(std::forward<Input>(value));
        return *this;
    }
};

template <>
struct Mutator<::proto::astra::v1::SnapshotChunk> {
    // message 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SnapshotChunk& message;

    // 设置 snapshot_version 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto snapshot_version(Input&& value) {
        message.set_snapshot_version(std::forward<Input>(value));
        return *this;
    }

    // 设置 chunk_data 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto chunk_data(Input&& value) {
        message.set_chunk_data(std::forward<Input>(value));
        return *this;
    }

    // 设置 is_last_chunk 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename Input>
    auto is_last_chunk(Input&& value) {
        message.set_is_last_chunk(std::forward<Input>(value));
        return *this;
    }
};

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

// 创建只借用 message 的代理; 不延长消息寿命, 不进行序列化或分配.
template <typename Message>
inline Mutator<Message> mutate(Message& message) {
    return {message};
}

} // namespace astra
