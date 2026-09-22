#include "check.hpp"
#include <astra/proto_proxy.hpp>

#include <google/protobuf/arena.h>
#include <iostream>
#include <type_traits>

namespace {
// 构造所有代理字段, 防止未实例化的模板把不存在的生成 API 隐藏到业务接入时.
void test_fields_and_oneof() {

    // hello 校验标量与签名字段的链式赋值, 作为后续 oneof 消息源.
    proto::astra::v1::Hello hello;
    astra::mutate(hello).protocol_major(1U).protocol_minor(0U).max_frame_bytes(4096U).admission("body").admission_signature("signature");
    CHECK(hello.protocol_major() == 1 && hello.max_frame_bytes() == 4096 && hello.admission() == "body");
    // ping 为心跳请求输入, 序号在当前场景显式设置, 代理不能改变其消息类型.
    proto::astra::v1::Ping ping;
    // pong 为独立应答输入, 用不同序号核对 Ping/Pong 分支不混淆.
    proto::astra::v1::Pong pong;
    // error 为明确的拒绝消息, 用于检查 oneof 切换和错误枚举传递.
    proto::astra::v1::ProtocolError error;
    astra::mutate(ping).request_id(std::uint64_t{9});
    astra::mutate(pong).request_id(std::uint64_t{10});
    astra::mutate(error).code(proto::astra::v1::PROTOCOL_ERROR_CODE_UNAUTHORIZED);
    // packet 依次切换 Hello/Ping/Pong/拒绝分支, 前一分支必须被清除.
    proto::astra::v1::SessionPacket packet;
    astra::mutate(packet).hello(hello);
    CHECK(packet.has_hello() && packet.hello().admission_signature() == "signature");
    astra::mutate(packet).ping(ping);
    CHECK(packet.has_ping() && !packet.has_hello() && packet.ping().request_id() == 9);
    astra::mutate(packet).pong(pong);
    CHECK(packet.has_pong() && packet.pong().request_id() == 10);
    astra::mutate(packet).rejection(error);
    CHECK(packet.has_rejection() && packet.rejection().code() == error.code());
    // pulse_ping 保存发送时的本机纳秒值, 用来核对 Pong 原样回显.
    proto::pulsar::v1::Ping pulse_ping;
    // pulse_pong 同时设置四时间戳中的服务端部分,误差和同步资格.
    proto::pulsar::v1::Pong pulse_pong;
    astra::mutate(pulse_ping).t0(std::uint64_t{10});
    astra::mutate(pulse_pong).t0(std::uint64_t{10}).t1(std::uint64_t{20}).t2(std::uint64_t{21}).uncertainty_ns(std::uint64_t{2000}).synchronized(true).precision_ns(std::uint64_t{1000});
    // decoded_pulse 接收真实编码往返结果, 不只验证模板能实例化.
    proto::pulsar::v1::Pong decoded_pulse;
    CHECK(decoded_pulse.ParseFromString(pulse_pong.SerializeAsString()));
    CHECK(decoded_pulse.t0() == pulse_ping.t0() && decoded_pulse.precision_ns() == 1000 && decoded_pulse.uncertainty_ns() == 2000 && decoded_pulse.synchronized());
}

// 跨 Arena 复制后销毁源消息, 目标仍拥有独立内容; 代理不使用 set_allocated 夺取调用方指针.
void test_registration_and_arena() {

    // response 在源 Arena 销毁后继续存在, 成员列表必须拥有独立内容.
    proto::orbit::v1::RegistrationResponse response;
    {
        // arena 只拥有本作用域的源成员, 作用域退出即释放, 目标不能保留其借用.
        google::protobuf::Arena arena;
        // member 由 arena 拥有, 在源作用域内构造全部拓扑字段.
        auto* member = google::protobuf::Arena::Create<proto::orbit::v1::Member>(&arena);
        astra::mutate(*member).galaxy("alpha").id("issued/1").principal("digest").advertise("127.0.0.1:7443").epoch(std::uint64_t{1}).role(proto::orbit::v1::ROLE_STAR).group("local");
        // members 拥有源成员副本, repeated 代理再次赋值应替换而非累计.
        google::protobuf::RepeatedPtrField<proto::orbit::v1::Member> members;
        *members.Add() = *member;
        astra::mutate(response).members(members).members(members).admission("admission").signature("signature");
    }
    CHECK(response.members_size() == 1 && response.members(0).galaxy() == "alpha" && response.members(0).id() == "issued/1");
    // request 覆盖账号,角色,分组和二进制启动键, 不省略登记适配字段.
    proto::orbit::v1::RegistrationRequest request;
    astra::mutate(request).galaxy("alpha").advertise("127.0.0.1:7443").role(proto::orbit::v1::ROLE_STAR).group("local").candidate_round(2U).username("user").password("password").request_id(std::string(32, 'r'));
    // serialized 用真实序列化往返验证字段持有的字节, 不只检查链式表达式能编译.
    const auto serialized = request.SerializeAsString();
    // decoded 接收序列化后的独立消息, 对照原始字段检查转发和持有语义.
    proto::orbit::v1::RegistrationRequest decoded;
    CHECK(decoded.ParseFromString(serialized));
    CHECK(decoded.galaxy() == "alpha" && decoded.candidate_round() == 2 && decoded.request_id().size() == 32);
    CHECK(decoded.username() == "user" && decoded.password() == "password");
}

// 代理按值返回轻量借用对象, 临时代理结束不能使返回代理引用悬空, 原消息仍由调用者持有.
void test_proxy_lifetime() {

    // message 为各代理共享修改的原消息, 其寿命覆盖全部临时和命名代理.
    proto::astra::v1::Ping message;
    static_assert(std::is_trivially_copy_constructible_v<astra::Mutator<proto::astra::v1::Ping>>);
    static_assert(!std::is_reference_v<decltype(astra::mutate(message).request_id(std::uint64_t{1}))>);
    // edit 延长返回代理临时值的寿命, 不能引用已经销毁的链式调用中间对象.
    auto&& edit = astra::mutate(message).request_id(std::uint64_t{1});
    edit.request_id(std::uint64_t{2});
    CHECK(message.request_id() == 2);
    // returned 接走 lambda 中局部代理的按值结果, 原消息仍在外层有效.
    const auto returned = [&] {
        // local 为只借用 message 的栈代理, 返回字段调用结果后可以安全销毁.
        auto local = astra::mutate(message);
        return local.request_id(std::uint64_t{3});
    }();
    // next 复制轻量代理, 应继续写入同一个原消息, 不复制 Protobuf 对象.
    auto next = returned;
    next.request_id(std::uint64_t{4});
    CHECK(message.request_id() == 4);
    // named 为命名代理, 连续调用应与临时链式调用具有相同写入语义.
    auto named = astra::mutate(message);
    named.request_id(std::uint64_t{5}).request_id(std::uint64_t{6});
    CHECK(message.request_id() == 6);
}

// 目标位于另一 Arena 时保持 Protobuf 所有权规则, 源销毁后目标内容仍完整有效.
void test_target_arena() {

    // destination 拥有外层目标消息, 寿命覆盖内层 source 的创建与销毁.
    google::protobuf::Arena destination;
    // packet 为 destination 拥有的目标 oneof 消息, 后续切换分支不能借用已释放源.
    auto* packet = google::protobuf::Arena::Create<proto::astra::v1::SessionPacket>(&destination);
    // response 为 destination 拥有的目标 repeated 消息, 接受跨 Arena 移动请求.
    auto* response = google::protobuf::Arena::Create<proto::orbit::v1::RegistrationResponse>(&destination);
    {
        // source 仅拥有内层输入消息, 退出此作用域后强制检验目标的独立性.
        google::protobuf::Arena source;
        // hello 属于 source, 移入不同 Arena 时由 Protobuf 决定必要复制, 代理不夺取原指针.
        auto* hello = google::protobuf::Arena::Create<proto::astra::v1::Hello>(&source);
        astra::mutate(*hello).admission(std::string(128, 'a'));
        astra::mutate(*packet).hello(std::move(*hello));
        // members 属于 source, 将其 repeated 字段转发给更长寿命的目标 response.
        auto* members = google::protobuf::Arena::Create<proto::orbit::v1::RegistrationResponse>(&source);
        members->add_members()->set_id("source/member");
        astra::mutate(*response).members(std::move(*members->mutable_members()));
    }
    CHECK(packet->has_hello() && packet->hello().admission() == std::string(128, 'a'));
    CHECK(response->members_size() == 1 && response->members(0).id() == "source/member");
    // ping 为心跳请求输入, 序号在当前场景显式设置, 代理不能改变其消息类型.
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
