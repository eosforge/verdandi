// 功能: 提供手写的 Protobuf 链式赋值适配, 直接借用消息, 不拥有消息生命周期.
// 标量调用 set_, 子消息通过 mutable_ 赋值, repeated 字段整体替换; 实际分配遵循 Protobuf 的语义.
#pragma once

#include <utility>

#include "astra.pb.h"
#include "orbit.pb.h"

namespace astra {

template <typename T> struct Mutator;

template <> struct Mutator<::proto::astra::v1::Hello> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::Hello& msg;
    // 设置 protocol_major 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto protocol_major(V&& val) {
        msg.set_protocol_major(std::forward<V>(val));
        return *this;
    }
    // 设置 protocol_minor 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto protocol_minor(V&& val) {
        msg.set_protocol_minor(std::forward<V>(val));
        return *this;
    }
    // 设置 max_frame_bytes 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto max_frame_bytes(V&& val) {
        msg.set_max_frame_bytes(std::forward<V>(val));
        return *this;
    }
    // 设置 admission 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto admission(V&& val) {
        msg.set_admission(std::forward<V>(val));
        return *this;
    }
    // 设置 admission_signature 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto admission_signature(V&& val) {
        msg.set_admission_signature(std::forward<V>(val));
        return *this;
    }
};
template <> struct Mutator<::proto::astra::v1::Ping> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::Ping& msg;
    // 设置 request_id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto request_id(V&& val) {
        msg.set_request_id(std::forward<V>(val));
        return *this;
    }
};
template <> struct Mutator<::proto::astra::v1::Pong> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::Pong& msg;
    // 设置 request_id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto request_id(V&& val) {
        msg.set_request_id(std::forward<V>(val));
        return *this;
    }
};
template <> struct Mutator<::proto::astra::v1::ProtocolError> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::ProtocolError& msg;
    // 设置 code 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto code(V&& val) {
        msg.set_code(std::forward<V>(val));
        return *this;
    }
};
template <> struct Mutator<::proto::astra::v1::SessionPacket> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SessionPacket& msg;
    // 设置 hello 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto hello(V&& val) {
        *msg.mutable_hello() = std::forward<V>(val);
        return *this;
    }
    // 设置 ping 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto ping(V&& val) {
        *msg.mutable_ping() = std::forward<V>(val);
        return *this;
    }
    // 设置 pong 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto pong(V&& val) {
        *msg.mutable_pong() = std::forward<V>(val);
        return *this;
    }
    // 设置 rejection 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto rejection(V&& val) {
        *msg.mutable_rejection() = std::forward<V>(val);
        return *this;
    }
};
template <> struct Mutator<::proto::astra::v1::SyncPacket> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SyncPacket& msg;
    // 设置 hello 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto hello(V&& val) {
        *msg.mutable_hello() = std::forward<V>(val);
        return *this;
    }
    // 设置 ping 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto ping(V&& val) {
        *msg.mutable_ping() = std::forward<V>(val);
        return *this;
    }
    // 设置 pong 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto pong(V&& val) {
        *msg.mutable_pong() = std::forward<V>(val);
        return *this;
    }
    // 设置 request 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto request(V&& val) {
        *msg.mutable_request() = std::forward<V>(val);
        return *this;
    }
    // 设置 response 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto response(V&& val) {
        *msg.mutable_response() = std::forward<V>(val);
        return *this;
    }
};
template <> struct Mutator<::proto::astra::v1::SyncRequest> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SyncRequest& msg;
    // 设置 client_global_version 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto client_global_version(V&& val) {
        msg.set_client_global_version(std::forward<V>(val));
        return *this;
    }
    // 设置 server_instance_id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto server_instance_id(V&& val) {
        msg.set_server_instance_id(std::forward<V>(val));
        return *this;
    }
    // 设置 subscribe_prefix 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto subscribe_prefix(V&& val) {
        msg.set_subscribe_prefix(std::forward<V>(val));
        return *this;
    }
    // 设置 cursor_prefix 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto cursor_prefix(V&& val) {
        msg.set_cursor_prefix(std::forward<V>(val));
        return *this;
    }
};
template <> struct Mutator<::proto::astra::v1::SyncResponse> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SyncResponse& msg;
    // 设置 server_global_version 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto server_global_version(V&& val) {
        msg.set_server_global_version(std::forward<V>(val));
        return *this;
    }
    // 设置 server_instance_id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto server_instance_id(V&& val) {
        msg.set_server_instance_id(std::forward<V>(val));
        return *this;
    }
    // 设置 requires_full_snapshot 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto requires_full_snapshot(V&& val) {
        msg.set_requires_full_snapshot(std::forward<V>(val));
        return *this;
    }
    // 设置 deltas 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto deltas(V&& val) {
        *msg.mutable_deltas() = std::forward<V>(val);
        return *this;
    }
};
template <> struct Mutator<::proto::astra::v1::DeltaRecord> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::DeltaRecord& msg;
    // 设置 key 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto key(V&& val) {
        msg.set_key(std::forward<V>(val));
        return *this;
    }
    // 设置 value 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto value(V&& val) {
        msg.set_payload(std::forward<V>(val));
        return *this;
    }
    // 设置 deleted 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto deleted(V&& val) {
        msg.set_deleted(std::forward<V>(val));
        return *this;
    }
    // 设置 version 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto version(V&& val) {
        msg.set_field_version(std::forward<V>(val));
        return *this;
    }
};
template <> struct Mutator<::proto::astra::v1::SnapshotRequest> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SnapshotRequest& msg;
    // 设置 subscribe_prefix 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto subscribe_prefix(V&& val) {
        msg.set_subscribe_prefix(std::forward<V>(val));
        return *this;
    }
};
template <> struct Mutator<::proto::astra::v1::SnapshotChunk> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::astra::v1::SnapshotChunk& msg;
    // 设置 snapshot_version 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto snapshot_version(V&& val) {
        msg.set_snapshot_version(std::forward<V>(val));
        return *this;
    }
    // 设置 chunk_data 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto chunk_data(V&& val) {
        msg.set_chunk_data(std::forward<V>(val));
        return *this;
    }
    // 设置 is_last_chunk 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto is_last_chunk(V&& val) {
        msg.set_is_last_chunk(std::forward<V>(val));
        return *this;
    }
};
template <> struct Mutator<::proto::orbit::v1::RegistrationRequest> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::orbit::v1::RegistrationRequest& msg;
    // 设置 galaxy 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto galaxy(V&& val) {
        msg.set_galaxy(std::forward<V>(val));
        return *this;
    }
    // 设置 advertise 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto advertise(V&& val) {
        msg.set_advertise(std::forward<V>(val));
        return *this;
    }
    // 设置 role 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto role(V&& val) {
        msg.set_role(std::forward<V>(val));
        return *this;
    }
    // 设置 group 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto group(V&& val) {
        msg.set_group(std::forward<V>(val));
        return *this;
    }
    // 设置 candidate_round 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto candidate_round(V&& val) {
        msg.set_candidate_round(std::forward<V>(val));
        return *this;
    }
    // 设置 username 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto username(V&& val) {
        msg.set_username(std::forward<V>(val));
        return *this;
    }
    // 设置 password 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto password(V&& val) {
        msg.set_password(std::forward<V>(val));
        return *this;
    }
    // 设置 request_id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto request_id(V&& val) {
        msg.set_request_id(std::forward<V>(val));
        return *this;
    }
};
template <> struct Mutator<::proto::orbit::v1::RegistrationResponse> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::orbit::v1::RegistrationResponse& msg;
    // 设置 members 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto members(V&& val) {
        *msg.mutable_members() = std::forward<V>(val);
        return *this;
    }
    // 设置 admission 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto admission(V&& val) {
        msg.set_admission(std::forward<V>(val));
        return *this;
    }
    // 设置 signature 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto signature(V&& val) {
        msg.set_signature(std::forward<V>(val));
        return *this;
    }
};
template <> struct Mutator<::proto::orbit::v1::Member> {
    // msg 借用原始消息; 代理及其返回引用不能超过消息的生命周期.
    ::proto::orbit::v1::Member& msg;
    // 设置 galaxy 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto galaxy(V&& val) {
        msg.set_galaxy(std::forward<V>(val));
        return *this;
    }
    // 设置 id 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto id(V&& val) {
        msg.set_id(std::forward<V>(val));
        return *this;
    }
    // 设置 principal 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto principal(V&& val) {
        msg.set_principal(std::forward<V>(val));
        return *this;
    }
    // 设置 advertise 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto advertise(V&& val) {
        msg.set_advertise(std::forward<V>(val));
        return *this;
    }
    // 设置 epoch 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto epoch(V&& val) {
        msg.set_epoch(std::forward<V>(val));
        return *this;
    }
    // 设置 role 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto role(V&& val) {
        msg.set_role(std::forward<V>(val));
        return *this;
    }
    // 设置 group 并返回当前代理; 转发参数的复制或移动由对应 Protobuf 字段类型决定.
    template <typename V> auto group(V&& val) {
        msg.set_group(std::forward<V>(val));
        return *this;
    }
};

// 创建只借用 msg 的代理; 不延长消息寿命, 不进行序列化或分配.
template <typename T> inline Mutator<T> mutate(T& msg) {
    return {msg};
}

} // namespace astra
