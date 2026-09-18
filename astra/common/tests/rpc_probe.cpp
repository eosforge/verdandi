#include "admission.hpp"
#include "astra.grpc.pb.h"
#include "check.hpp"
#include <openssl/rand.h>

#include <algorithm>
#include <grpcpp/create_channel.h>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace astra;

struct Stream {
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReaderWriter<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket>> io;
    bool finished{};
    explicit Stream(proto::astra::v1::StarTransport::Stub& stub) {
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
    bool hello(const proto::astra::v1::Hello& hello, const Identity& identity) {
        proto::astra::v1::SessionPacket message;
        message.mutable_hello()->CopyFrom(hello);
        if (!io->Write(message) || !io->Read(&message)) {
            return false;
        }
        auto admission =
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(message.hello().admission().data()), message.hello().admission().size());
        auto sig = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(message.hello().admission_signature().data()),
                                                 message.hello().admission_signature().size());
        // 接收侧独立检查公开 v1 契约, 避免探针与服务同时改错常量后仍被判为互通.
        return message.has_hello() && message.hello().protocol_major() == 1 && identity.verify(admission, sig).has_value();
    }
    bool ping(std::uint64_t id) {
        proto::astra::v1::SessionPacket message;
        message.mutable_ping()->set_request_id(id);
        if (!io->Write(message)) {
            return false;
        }
        for (unsigned received = 0; received < 20 && io->Read(&message); ++received) {
            if (message.has_pong() && message.pong().request_id() == id) {
                return true;
            }
            if (message.has_ping()) {
                proto::astra::v1::SessionPacket reply;
                reply.mutable_pong()->set_request_id(message.ping().request_id());
                if (!io->Write(reply)) {
                    return false;
                }
            }
        }
        return false;
    }
};

proto::astra::v1::Hello admit(const Identity& identity, const std::string& supervisor, const std::string& advertise) {
    auto channel = grpc::CreateChannel(supervisor, identity.channel_credentials());
    auto stub = proto::orbit::v1::Admission::NewStub(channel);
    proto::orbit::v1::RegistrationRequest request;
    request.set_username(identity.username());
    request.set_password(identity.password());
    request.set_galaxy("alpha");
    request.set_advertise(advertise);
    std::string request_id(32, '\0');
    CHECK(RAND_bytes(reinterpret_cast<unsigned char*>(request_id.data()), request_id.size()) == 1);
    request.set_request_id(std::move(request_id));
    request.set_role(proto::orbit::v1::ROLE_STAR);
    request.set_group("default");
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    proto::orbit::v1::RegistrationResponse response;
    CHECK(stub->Register(&context, request, &response).ok());
    auto admission = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(response.admission().data()), response.admission().size());
    auto sig = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(response.signature().data()), response.signature().size());
    CHECK(identity.verify(admission, sig).has_value());
    proto::astra::v1::Hello hello;
    hello.set_protocol_major(protocol_major);
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
        auto stub = proto::astra::v1::StarTransport::NewStub(channel);
        {
            // 仅建立 TLS/RPC 而不提交 Hello 的调用方不能收到 bearer, 并且会在应用握手预算后被关闭.
            Stream silent(*stub);
            proto::astra::v1::SessionPacket message;
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
            auto isolated_stub = proto::astra::v1::StarTransport::NewStub(isolated);
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
                invalid.set_protocol_major(2);
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
            proto::astra::v1::SessionPacket message;
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
