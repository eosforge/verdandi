// 自动生成的 Proto Proxy (Zero-Cost Builder)
// 允许在 C++ 中直接使用零损耗的链式语法初始化 Protobuf 消息。
#pragma once

#include <utility>

// 提前声明需要的命名空间和类，避免需要 include 臃肿的 .pb.h (这里通过依赖方自行 include 解决)
namespace proto::astra::v1 { class Hello; }
namespace proto::orbit::v1 { class RegistrationRequest; }

namespace astra {

template <typename T>
struct Mutator;

template <>
struct Mutator<::proto::astra::v1::Hello> {
    ::proto::astra::v1::Hello& msg;
    template <typename V> auto& protocol_major(V&& val) { msg.set_protocol_major(std::forward<V>(val)); return *this; }
    template <typename V> auto& protocol_minor(V&& val) { msg.set_protocol_minor(std::forward<V>(val)); return *this; }
    template <typename V> auto& max_frame_bytes(V&& val) { msg.set_max_frame_bytes(std::forward<V>(val)); return *this; }
    template <typename V> auto& admission(V&& val) { msg.set_admission(std::forward<V>(val)); return *this; }
    template <typename V> auto& admission_signature(V&& val) { msg.set_admission_signature(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::astra::v1::Ping> {
    ::proto::astra::v1::Ping& msg;
    template <typename V> auto& request_id(V&& val) { msg.set_request_id(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::astra::v1::Pong> {
    ::proto::astra::v1::Pong& msg;
    template <typename V> auto& request_id(V&& val) { msg.set_request_id(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::astra::v1::ProtocolError> {
    ::proto::astra::v1::ProtocolError& msg;
    template <typename V> auto& code(V&& val) { msg.set_code(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::astra::v1::SessionPacket> {
    ::proto::astra::v1::SessionPacket& msg;
    template <typename V> auto& hello(V&& val) { msg.set_hello(std::forward<V>(val)); return *this; }
    template <typename V> auto& ping(V&& val) { msg.set_ping(std::forward<V>(val)); return *this; }
    template <typename V> auto& pong(V&& val) { msg.set_pong(std::forward<V>(val)); return *this; }
    template <typename V> auto& rejection(V&& val) { msg.set_rejection(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::astra::v1::SyncPacket> {
    ::proto::astra::v1::SyncPacket& msg;
    template <typename V> auto& hello(V&& val) { msg.set_hello(std::forward<V>(val)); return *this; }
    template <typename V> auto& ping(V&& val) { msg.set_ping(std::forward<V>(val)); return *this; }
    template <typename V> auto& pong(V&& val) { msg.set_pong(std::forward<V>(val)); return *this; }
    template <typename V> auto& request(V&& val) { msg.set_request(std::forward<V>(val)); return *this; }
    template <typename V> auto& response(V&& val) { msg.set_response(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::astra::v1::SyncRequest> {
    ::proto::astra::v1::SyncRequest& msg;
    template <typename V> auto& client_global_version(V&& val) { msg.set_client_global_version(std::forward<V>(val)); return *this; }
    template <typename V> auto& server_instance_id(V&& val) { msg.set_server_instance_id(std::forward<V>(val)); return *this; }
    template <typename V> auto& subscribe_prefix(V&& val) { msg.set_subscribe_prefix(std::forward<V>(val)); return *this; }
    template <typename V> auto& cursor_prefix(V&& val) { msg.set_cursor_prefix(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::astra::v1::SyncResponse> {
    ::proto::astra::v1::SyncResponse& msg;
    template <typename V> auto& server_global_version(V&& val) { msg.set_server_global_version(std::forward<V>(val)); return *this; }
    template <typename V> auto& server_instance_id(V&& val) { msg.set_server_instance_id(std::forward<V>(val)); return *this; }
    template <typename V> auto& requires_full_snapshot(V&& val) { msg.set_requires_full_snapshot(std::forward<V>(val)); return *this; }
    template <typename V> auto& deltas(V&& val) { msg.set_deltas(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::astra::v1::Store::Delta> {
    ::proto::astra::v1::Store::Delta& msg;
    template <typename V> auto& key(V&& val) { msg.set_key(std::forward<V>(val)); return *this; }
    template <typename V> auto& value(V&& val) { msg.set_payload(std::forward<V>(val)); return *this; }
    template <typename V> auto& deleted(V&& val) { msg.set_deleted(std::forward<V>(val)); return *this; }
    template <typename V> auto& version(V&& val) { msg.set_field_version(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::astra::v1::SnapshotRequest> {
    ::proto::astra::v1::SnapshotRequest& msg;
    template <typename V> auto& subscribe_prefix(V&& val) { msg.set_subscribe_prefix(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::astra::v1::SnapshotChunk> {
    ::proto::astra::v1::SnapshotChunk& msg;
    template <typename V> auto& snapshot_version(V&& val) { msg.set_snapshot_version(std::forward<V>(val)); return *this; }
    template <typename V> auto& chunk_data(V&& val) { msg.set_chunk_data(std::forward<V>(val)); return *this; }
    template <typename V> auto& is_last_chunk(V&& val) { msg.set_is_last_chunk(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::orbit::v1::RegistrationRequest> {
    ::proto::orbit::v1::RegistrationRequest& msg;
    template <typename V> auto& galaxy(V&& val) { msg.set_galaxy(std::forward<V>(val)); return *this; }
    template <typename V> auto& advertise(V&& val) { msg.set_advertise(std::forward<V>(val)); return *this; }
    template <typename V> auto& role(V&& val) { msg.set_role(std::forward<V>(val)); return *this; }
    template <typename V> auto& group(V&& val) { msg.set_group(std::forward<V>(val)); return *this; }
    template <typename V> auto& candidate_round(V&& val) { msg.set_candidate_round(std::forward<V>(val)); return *this; }
    template <typename V> auto& username(V&& val) { msg.set_username(std::forward<V>(val)); return *this; }
    template <typename V> auto& password(V&& val) { msg.set_password(std::forward<V>(val)); return *this; }
    template <typename V> auto& request_id(V&& val) { msg.set_request_id(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::orbit::v1::RegistrationResponse> {
    ::proto::orbit::v1::RegistrationResponse& msg;
    template <typename V> auto& members(V&& val) { msg.set_members(std::forward<V>(val)); return *this; }
    template <typename V> auto& admission(V&& val) { msg.set_admission(std::forward<V>(val)); return *this; }
    template <typename V> auto& signature(V&& val) { msg.set_signature(std::forward<V>(val)); return *this; }
};
template <>
struct Mutator<::proto::orbit::v1::Member> {
    ::proto::orbit::v1::Member& msg;
    template <typename V> auto& galaxy(V&& val) { msg.set_galaxy(std::forward<V>(val)); return *this; }
    template <typename V> auto& id(V&& val) { msg.set_id(std::forward<V>(val)); return *this; }
    template <typename V> auto& principal(V&& val) { msg.set_principal(std::forward<V>(val)); return *this; }
    template <typename V> auto& advertise(V&& val) { msg.set_advertise(std::forward<V>(val)); return *this; }
    template <typename V> auto& epoch(V&& val) { msg.set_epoch(std::forward<V>(val)); return *this; }
    template <typename V> auto& role(V&& val) { msg.set_role(std::forward<V>(val)); return *this; }
    template <typename V> auto& group(V&& val) { msg.set_group(std::forward<V>(val)); return *this; }
};

template <typename T>
inline Mutator<T> mutate(T& msg) {
    return {msg};
}

} // namespace astra
