// 功能: 独立包含代理头, 实例化全部消息适配, 验证标量, oneof, repeated 与 Arena 所有权.
#include "check.hpp"
#include <astra/proto_proxy.hpp>

#include <google/protobuf/arena.h>
#include <iostream>
#include <type_traits>

namespace {
// 构造所有代理字段, 防止未实例化的模板把不存在的生成 API 隐藏到业务接入时.
void test_fields_and_oneof() {
    proto::astra::v1::Hello hello;
    astra::mutate(hello).protocol_major(1U).protocol_minor(0U).max_frame_bytes(4096U).admission("body").admission_signature("signature");
    CHECK(hello.protocol_major() == 1 && hello.max_frame_bytes() == 4096 && hello.admission() == "body");
    proto::astra::v1::Ping ping;
    proto::astra::v1::Pong pong;
    proto::astra::v1::ProtocolError error;
    astra::mutate(ping).request_id(std::uint64_t{9});
    astra::mutate(pong).request_id(std::uint64_t{10});
    astra::mutate(error).code(proto::astra::v1::PROTOCOL_ERROR_CODE_UNAUTHORIZED);
    proto::astra::v1::SessionPacket packet;
    astra::mutate(packet).hello(hello);
    CHECK(packet.has_hello() && packet.hello().admission_signature() == "signature");
    astra::mutate(packet).ping(ping);
    CHECK(packet.has_ping() && !packet.has_hello() && packet.ping().request_id() == 9);
    astra::mutate(packet).pong(pong);
    CHECK(packet.has_pong() && packet.pong().request_id() == 10);
    astra::mutate(packet).rejection(error);
    CHECK(packet.has_rejection() && packet.rejection().code() == error.code());
    proto::astra::v1::SyncRequest request;
    astra::mutate(request).client_global_version(std::uint64_t{11}).server_instance_id("instance").subscribe_prefix("catalog/").cursor_prefix("catalog/");
    proto::astra::v1::DeltaRecord delta;
    astra::mutate(delta).key("catalog/key").value(std::string("a\0b", 3)).deleted(false).version(std::uint64_t{12});
    // deltas 整体替换一次; 第二次设置同一容器不应追加重复项.
    google::protobuf::RepeatedPtrField<proto::astra::v1::DeltaRecord> deltas;
    *deltas.Add() = delta;
    proto::astra::v1::SyncResponse response;
    astra::mutate(response).server_global_version(std::uint64_t{12}).server_instance_id("instance").requires_full_snapshot(false).deltas(deltas).deltas(deltas);
    CHECK(response.deltas_size() == 1 && response.deltas(0).payload() == std::string("a\0b", 3));
    proto::astra::v1::SyncPacket sync;
    astra::mutate(sync).hello(hello).ping(ping).pong(pong).request(request);
    CHECK(sync.has_request() && sync.request().client_global_version() == 11);
    astra::mutate(sync).response(std::move(response));
    CHECK(sync.has_response() && sync.response().deltas(0).field_version() == 12);
    proto::astra::v1::SnapshotRequest snapshot;
    proto::astra::v1::SnapshotChunk chunk;
    astra::mutate(snapshot).subscribe_prefix("catalog/");
    astra::mutate(chunk).snapshot_version(std::uint64_t{12}).chunk_data("data").is_last_chunk(true);
    CHECK(snapshot.subscribe_prefix() == "catalog/" && chunk.is_last_chunk() && chunk.snapshot_version() == 12);
}

// 跨 Arena 复制后销毁源消息, 目标仍拥有独立内容; 代理不使用 set_allocated 夺取调用方指针.
void test_registration_and_arena() {
    proto::orbit::v1::RegistrationResponse response;
    {
        google::protobuf::Arena arena;
        auto* member = google::protobuf::Arena::Create<proto::orbit::v1::Member>(&arena);
        astra::mutate(*member)
            .galaxy("alpha")
            .id("issued/1")
            .principal("digest")
            .advertise("127.0.0.1:7443")
            .epoch(std::uint64_t{1})
            .role(proto::orbit::v1::ROLE_STAR)
            .group("local");
        google::protobuf::RepeatedPtrField<proto::orbit::v1::Member> members;
        *members.Add() = *member;
        astra::mutate(response).members(members).members(members).admission("admission").signature("signature");
    }
    CHECK(response.members_size() == 1 && response.members(0).galaxy() == "alpha" && response.members(0).id() == "issued/1");
    proto::orbit::v1::RegistrationRequest request;
    astra::mutate(request)
        .galaxy("alpha")
        .advertise("127.0.0.1:7443")
        .role(proto::orbit::v1::ROLE_STAR)
        .group("local")
        .candidate_round(2U)
        .username("user")
        .password("password")
        .request_id(std::string(32, 'r'));
    // serialized 用真实序列化往返验证字段持有的字节, 不只检查链式表达式能编译.
    const auto serialized = request.SerializeAsString();
    proto::orbit::v1::RegistrationRequest decoded;
    CHECK(decoded.ParseFromString(serialized));
    CHECK(decoded.galaxy() == "alpha" && decoded.candidate_round() == 2 && decoded.request_id().size() == 32);
    CHECK(decoded.username() == "user" && decoded.password() == "password");
}
void test_proxy_lifetime() {
    proto::astra::v1::Ping message;
    static_assert(std::is_trivially_copy_constructible_v<astra::Mutator<proto::astra::v1::Ping>>);
    static_assert(!std::is_reference_v<decltype(astra::mutate(message).request_id(std::uint64_t{1}))>);
    auto&& edit = astra::mutate(message).request_id(std::uint64_t{1});
    edit.request_id(std::uint64_t{2});
    CHECK(message.request_id() == 2);
    const auto returned = [&] {
        auto local = astra::mutate(message);
        return local.request_id(std::uint64_t{3});
    }();
    auto next = returned;
    next.request_id(std::uint64_t{4});
    CHECK(message.request_id() == 4);
    auto named = astra::mutate(message);
    named.request_id(std::uint64_t{5}).request_id(std::uint64_t{6});
    CHECK(message.request_id() == 6);
}

void test_target_arena() {
    google::protobuf::Arena destination;
    auto* packet = google::protobuf::Arena::Create<proto::astra::v1::SessionPacket>(&destination);
    auto* response = google::protobuf::Arena::Create<proto::orbit::v1::RegistrationResponse>(&destination);
    {
        google::protobuf::Arena source;
        auto* hello = google::protobuf::Arena::Create<proto::astra::v1::Hello>(&source);
        astra::mutate(*hello).admission(std::string(128, 'a'));
        astra::mutate(*packet).hello(std::move(*hello));
        auto* members = google::protobuf::Arena::Create<proto::orbit::v1::RegistrationResponse>(&source);
        members->add_members()->set_id("source/member");
        astra::mutate(*response).members(std::move(*members->mutable_members()));
    }
    CHECK(packet->has_hello() && packet->hello().admission() == std::string(128, 'a'));
    CHECK(response->members_size() == 1 && response->members(0).id() == "source/member");
    proto::astra::v1::Ping ping;
    ping.set_request_id(7);
    astra::mutate(*packet).ping(std::move(ping));
    CHECK(packet->has_ping() && !packet->has_hello() && packet->ping().request_id() == 7);
}
} // namespace

// 全部检查在 Release 生效, 不需要监听端口或外部服务.
int main() {
    try {
        test_fields_and_oneof();
        test_registration_and_arena();
        test_proxy_lifetime();
        test_target_arena();
        std::cout << "Proto proxy scalar, oneof, repeated and Arena tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
