#include "admission.hpp"
#include "peer_transport.grpc.pb.h"

#include <algorithm>
#include <grpcpp/create_channel.h>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace verdandi::peer;
#define CHECK(condition)                                                                                                                                       \
    do {                                                                                                                                                       \
        if (!(condition))                                                                                                                                      \
            throw std::runtime_error("RPC check failed at line " + std::to_string(__LINE__));                                                                  \
    } while (false)

struct Stream {
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReaderWriter<wire::SessionPacket, wire::SessionPacket>> io;
    bool finished{};
    explicit Stream(wire::PeerTransport::Stub& stub) {
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(15));
        io = stub.OpenSession(&context);
    }
    ~Stream() {
        if (!finished) {
            context.TryCancel();
            static_cast<void>(io->Finish());
        }
    }
    void finish() {
        context.TryCancel();
        static_cast<void>(io->Finish());
        finished = true;
    }
    bool hello(const wire::Hello& hello, const Identity& identity) {
        wire::SessionPacket message;
        message.mutable_hello()->CopyFrom(hello);
        if (!io->Write(message) || !io->Read(&message)) {
            return false;
        }
        return message.has_hello() && identity.verify(message.hello().admission(), message.hello().admission_signature()).has_value();
    }
    bool ping(std::uint64_t id) {
        wire::SessionPacket message;
        message.mutable_ping()->set_request_id(id);
        if (!io->Write(message)) {
            return false;
        }
        for (unsigned received = 0; received < 20 && io->Read(&message); ++received) {
            if (message.has_pong() && message.pong().request_id() == id) {
                return true;
            }
            if (message.has_ping()) {
                wire::SessionPacket reply;
                reply.mutable_pong()->set_request_id(message.ping().request_id());
                if (!io->Write(reply)) {
                    return false;
                }
            }
        }
        return false;
    }
};

wire::Hello admit(const Identity& identity, const std::string& supervisor, const std::string& advertise) {
    auto channel = grpc::CreateChannel(supervisor, identity.channel_credentials());
    auto stub = wire::Admission::NewStub(channel);
    wire::LoginRequest login;
    login.set_username(identity.username());
    login.set_password(identity.password());
    login.set_cluster_id("alpha");
    login.set_advertise(advertise);
    wire::RegistrationChallenge challenge;
    grpc::ClientContext challenge_context;
    challenge_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    CHECK(stub->Challenge(&challenge_context, login, &challenge).ok());
    wire::RegistrationRequest request;
    request.set_username(login.username());
    request.set_password(login.password());
    request.set_cluster_id("alpha");
    request.set_advertise(advertise);
    request.set_peer_id(Identity::new_id()->text());
    request.set_expected_epoch(challenge.expected_epoch());
    request.set_role(wire::NODE_ROLE_STAR);
    request.set_group("default");
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    wire::RegistrationResponse response;
    CHECK(stub->Register(&context, request, &response).ok());
    CHECK(identity.verify(response.admission(), response.signature()));
    wire::Hello hello;
    hello.set_protocol_major(4);
    hello.set_max_frame_bytes(4096);
    hello.set_admission(response.admission());
    hello.set_admission_signature(response.signature());
    return hello;
}

int main(int argc, char** argv) {
    try {
        CHECK(argc == 5);
        auto endpoint = Endpoint::parse(argv[4]);
        CHECK(endpoint);
        auto identity = Identity::load(argv[3], *endpoint);
        CHECK(identity);
        const auto hello = admit(**identity, argv[1], argv[4]);
        auto channel = grpc::CreateChannel(argv[2], (*identity)->channel_credentials());
        auto stub = wire::PeerTransport::NewStub(channel);
        {
            // 仅建立 TLS/RPC 而不提交 Hello 的调用方不能收到 bearer, 并且会在应用握手预算后被关闭.
            Stream silent(*stub);
            wire::SessionPacket message;
            const auto started = Clock::now();
            CHECK(!silent.io->Read(&message));
            CHECK(Clock::now() - started < std::chrono::seconds(8));
        }
        Stream first(*stub);
        CHECK(first.hello(hello, **identity));
        CHECK(first.ping(41));
        {
            Stream duplicate(*stub);
            CHECK(!duplicate.hello(hello, **identity));
            CHECK(!duplicate.ping(42));
        }
        CHECK(first.ping(43));
        {
            // 强制独立 subchannel, 验证同一凭证跨实际连接也不能绕过活动逻辑会话索引.
            grpc::ChannelArguments arguments;
            arguments.SetInt("grpc.use_local_subchannel_pool", 1);
            auto isolated = grpc::CreateCustomChannel(argv[2], (*identity)->channel_credentials(), arguments);
            auto isolated_stub = wire::PeerTransport::NewStub(isolated);
            Stream duplicate(*isolated_stub);
            CHECK(!duplicate.hello(hello, **identity));
        }
        CHECK(first.ping(45));
        {
            // 一次只保留一个业务探针, 正常处理期间仍回复服务端 Ping. 测量控制 RTT, 不模拟未实现的数据业务.
            std::vector<double> samples;
            samples.reserve(500);
            for (std::uint64_t i = 0; i < 520; ++i) {
                const auto start = Clock::now();
                CHECK(first.ping(1000 + i));
                if (i >= 20) {
                    samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
                }
            }
            std::ranges::sort(samples);
            std::cout << "{\"control_rtt_us\":{\"samples\":" << samples.size() << ",\"p50\":" << samples[249] << ",\"p95\":" << samples[474]
                      << ",\"p99\":" << samples[494] << ",\"max\":" << samples.back() << "}}\n";
        }
        first.finish();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        {
            Stream next(*stub);
            CHECK(next.hello(hello, **identity));
            CHECK(next.ping(44));
            next.finish();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
            auto invalid = hello;
            if (mutation == 0) {
                invalid.set_protocol_major(3);
            } else if (mutation == 1) {
                (*invalid.mutable_admission_signature())[0] ^= 1;
            } else if (mutation == 2) {
                invalid.set_max_frame_bytes(1);
            } else {
                invalid.set_admission(std::string(5000, 'x'));
            }
            Stream bad(*stub);
            CHECK(!bad.hello(invalid, **identity));
            CHECK(!bad.ping(50 + mutation));
        }
        {
            // 对端持续写 Ping 却不读取响应, 不应令服务端无界积累消息或阻止关闭.
            Stream slow(*stub);
            CHECK(slow.hello(hello, **identity));
            wire::SessionPacket message;
            const auto started = Clock::now();
            std::uint64_t sent = 0;
            while (sent < 100000) {
                message.mutable_ping()->set_request_id(++sent);
                if (!slow.io->Write(message)) {
                    break;
                }
            }
            CHECK(sent < 100000 && Clock::now() - started < std::chrono::seconds(10));
            std::cout << "{\"slow_reader\":{\"attempted_pings\":" << sent
                      << ",\"close_ms\":" << std::chrono::duration_cast<Milliseconds>(Clock::now() - started).count() << "}}\n";
        }
        std::cout << "PASS silent Hello deadline, logical session reuse, duplicate fencing, Ping/Pong, rejection without credential disclosure\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
