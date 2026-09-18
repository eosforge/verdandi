#include "pulsar_test.hpp"
#include "pulse_client.hpp"
#include "server.hpp"
#include "store.hpp"
#include <atomic>
#include <grpc/support/time.h>
#include <grpcpp/create_channel.h>
#include <iostream>
#include <thread>

using namespace astra;
using namespace std::chrono_literals;

// 本例拥有一条已鉴权但随后静默的流; 独立子通道避免 HTTP/2 单连接流上限把测试串行化.
struct IdlePulse {
    grpc::ClientContext context;
    std::unique_ptr<proto::pulsar::v1::Pulse::Stub> stub;
    std::unique_ptr<grpc::ClientReaderWriter<proto::pulsar::v1::Ping, proto::pulsar::v1::Pong>> stream;
    bool finished{};
    IdlePulse(const std::string& endpoint, const Identity& identity, const proto::astra::v1::Hello& hello, bool long_deadline = false) {
        if (long_deadline) {
            context.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(3600, GPR_TIMESPAN)));
        }
        context.AddMetadata("astra-admission-bin", hello.admission());
        context.AddMetadata("astra-signature-bin", hello.admission_signature());
        grpc::ChannelArguments arguments;
        arguments.SetInt("grpc.use_local_subchannel_pool", 1);
        stub = proto::pulsar::v1::Pulse::NewStub(grpc::CreateCustomChannel(endpoint, identity.channel_credentials(), arguments));
        stream = stub->Bounce(&context);
    }
    ~IdlePulse() {
        if (!finished) {
            context.TryCancel();
            static_cast<void>(stream->Finish());
        }
    }
    // 先取得真实 Pong 确认服务已接纳, 再停止发送, 不把尚未到达服务的请求当作慢流.
    void ping() {
        proto::pulsar::v1::Ping ping;
        proto::pulsar::v1::Pong pong;
        ping.set_t0(static_cast<std::uint64_t>(elapsed_ns(EpochClock::Elapsed::now())));
        CHECK(stream->Write(ping) && stream->Read(&pong));
        CHECK(pong.t0() == ping.t0() && pong.precision_ns() >= 1 && pong.precision_ns() <= 20'000'000);
        CHECK(pong.synchronized() && pong.t2() >= pong.t1() && pong.t1() >= 1'800'000'000'000'000'000ULL);
    }
    // 故意不 half-close. 只能由服务端自己的单调截止结束; CTest 另有整例超时.
    grpc::Status wait_expiry() {
        proto::pulsar::v1::Pong pong;
        CHECK(!stream->Read(&pong));
        auto status = stream->Finish();
        finished = true;
        return status;
    }
};

struct Client {
    std::shared_ptr<Identity> identity;
    std::unique_ptr<proto::orbit::v1::Admission::Stub> stub;
    explicit Client(const std::string& endpoint) {
        identity = *Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", *Endpoint::parse("127.0.0.1:7443"));
        stub = proto::orbit::v1::Admission::NewStub(grpc::CreateChannel(endpoint, identity->channel_credentials()));
    }
    proto::orbit::v1::RegistrationRequest request(char startup, std::uint16_t port = 7443) const {
        proto::orbit::v1::RegistrationRequest request;
        request.set_username(identity->username());
        request.set_password(identity->password());
        request.set_galaxy("alpha");
        request.set_group("default");
        request.set_role(proto::orbit::v1::ROLE_STAR);
        request.set_advertise("127.0.0.1:" + std::to_string(port));
        request.set_request_id(std::string(32, startup));
        return request;
    }
    grpc::Status call(const proto::orbit::v1::RegistrationRequest& request, proto::orbit::v1::RegistrationResponse& response) {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 10s);
        response.Clear();
        return stub->Register(&context, request, &response);
    }
};

static EpochClock::Reading wait_clock(const EpochClock& clock, ElapsedTime after = {}) {
    const auto deadline = Clock::now() + 15s;
    while (Clock::now() < deadline) {
        if (auto value = clock.now(); value && value->ready && value->sampled > after) {
            return *value;
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Star did not calibrate within deadline");
}

int main() {
    try {
        test::Directory directory;
        PulsarServer::Config config;
        config.listen = *Endpoint::parse("127.0.0.1:0", true);
        config.pulse_listen = *Endpoint::parse("127.0.0.1:0", true);
        config.galaxy = "alpha";
        config.identity = std::filesystem::path(ASTRA_FIXTURES) / "supervisor";
        config.state = directory.path / "membership.journal";
        // RPC 测试注入独立参考源, 不要求测试主机联网对时, 也不更改系统墙钟或生产配置.
        std::atomic_bool source_available{false};
        const auto provider = [&]() -> std::optional<EpochClock::Estimate> {
            if (!source_available.load()) {
                return std::nullopt;
            }
            const auto local = EpochClock::Elapsed::now();
            return EpochClock::Estimate{EpochClock::Time(1'800'000'000s) + local.time_since_epoch(), local, 1000, 0};
        };
        auto server = std::make_unique<PulsarServer>(config, provider);
        server->start();
        config.listen = *Endpoint::parse(server->admission_endpoint());
        config.pulse_listen = *Endpoint::parse(server->pulse_endpoint());
        CHECK(config.listen != config.pulse_listen);
        {
            // 配置错误直接拒绝; 同端口第二个服务即使用另一日志也不能启动.
            const auto parse = [](std::initializer_list<std::string_view> args) { return PulsarServer::Config::parse({args.begin(), args.size()}); };
            CHECK(parse({"--listen=127.0.0.1:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha"}));
            CHECK(!parse({"--listen=127.0.0.1:1", "--pulse-listen=127.0.0.1:1", "--galaxy=alpha"}));
            CHECK(!parse({"--listen=0.0.0.0:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha"}));
            CHECK(!parse({"--listen=127.0.0.1:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha", "--galaxy=alpha"}));
            CHECK(!parse({"--listen=127.0.0.1:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha", "--max-starts=0"}));
            CHECK(!parse({"--listen=127.0.0.1:0", "--galaxy=alpha"}));
            auto duplicate_config = config;
            duplicate_config.state = directory.path / "duplicate.journal";
            bool refused = false;
            try {
                PulsarServer duplicate(duplicate_config);
                duplicate.start();
            } catch (const std::exception&) {
                refused = true;
            }
            CHECK(refused);
        }
        Client client(server->admission_endpoint());
        proto::orbit::v1::RegistrationResponse response;
        auto first = client.request('a');
        CHECK(client.call(first, response).ok());
        CHECK(response.members_size() == 1 && response.pulse_endpoint() == server->pulse_endpoint());
        const auto original = response.admission();
        const auto original_signature = response.signature();
        CHECK(client.call(client.request('b', 7444), response).ok());
        CHECK(client.call(first, response).ok());
        CHECK(response.admission() == original && response.members_size() == 2);
        CHECK(response.members(0).id() < response.members(1).id());
        auto bad = first;
        bad.set_password("not-the-fixture-password");
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::UNAUTHENTICATED);
        bad = first;
        bad.set_username("missing-account");
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::UNAUTHENTICATED);
        bad = first;
        bad.set_role(proto::orbit::v1::ROLE_PLANET);
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::PERMISSION_DENIED);
        bad = first;
        bad.set_galaxy("other");
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
        bad = first;
        bad.set_advertise("127.0.0.1:7445");
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::ABORTED);
        bad = first;
        bad.set_request_id("short");
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::INVALID_ARGUMENT);

        // 同部署新启动替换旧实例, 旧启动请求不能再取得成功凭证.
        auto replacement = client.request('c');
        CHECK(client.call(replacement, response).ok());
        proto::orbit::v1::Member member;
        CHECK(member.ParseFromString(response.admission()) && member.epoch() == 2);
        auto hello = std::make_shared<proto::astra::v1::Hello>();
        hello->set_admission(response.admission());
        hello->set_admission_signature(response.signature());
        const auto committed = response.admission();
        CHECK(client.call(first, response).error_code() == grpc::StatusCode::ABORTED);

        auto pulse_stub = proto::pulsar::v1::Pulse::NewStub(grpc::CreateChannel(server->pulse_endpoint(), client.identity->channel_credentials()));
        // 签名伪造和已被替换的真实凭证均拒绝, 不能只验证 TLS 能建立连接.
        const auto reject_credential = [&](const std::string& admission, const std::string& signature) {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 2s);
            context.AddMetadata("astra-admission-bin", admission);
            context.AddMetadata("astra-signature-bin", signature);
            auto stream = pulse_stub->Bounce(&context);
            proto::pulsar::v1::Pong pong;
            static_cast<void>(stream->WritesDone());
            CHECK(!stream->Read(&pong));
            CHECK(stream->Finish().error_code() == grpc::StatusCode::PERMISSION_DENIED);
        };
        reject_credential(original, original_signature);
        reject_credential(hello->admission(), std::string(64, '\0'));
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 2s);
            auto stream = pulse_stub->Bounce(&context);
            proto::pulsar::v1::Pong pong;
            static_cast<void>(stream->WritesDone());
            CHECK(!stream->Read(&pong));
            CHECK(stream->Finish().error_code() == grpc::StatusCode::UNAUTHENTICATED);
        }
        // 系统时间未就绪不阻塞以上登记, 但带有效凭证的采样仍必须明确失败.
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 2s);
            context.AddMetadata("astra-admission-bin", hello->admission());
            context.AddMetadata("astra-signature-bin", hello->admission_signature());
            auto stream = pulse_stub->Bounce(&context);
            proto::pulsar::v1::Ping ping;
            ping.set_t0(static_cast<std::uint64_t>(elapsed_ns(EpochClock::Elapsed::now())));
            proto::pulsar::v1::Pong pong;
            CHECK(stream->Write(ping));
            CHECK(!stream->Read(&pong));
            static_cast<void>(stream->WritesDone());
            CHECK(stream->Finish().error_code() == grpc::StatusCode::UNAVAILABLE);
        }
        source_available.store(true);
        const auto reference_deadline = Clock::now() + 10s;
        while (Clock::now() < reference_deadline) {
            if (const auto time = server->time(); time && time->ready) {
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
        CHECK(server->time() && server->time()->ready);
        {
            // 无符号协议字段不能把有符号本地计数的越界值带入偏移运算.
            IdlePulse invalid(server->pulse_endpoint(), *client.identity, *hello);
            proto::pulsar::v1::Ping ping;
            proto::pulsar::v1::Pong pong;
            ping.set_t0(UINT64_MAX);
            CHECK(invalid.stream->Write(ping));
            CHECK(!invalid.stream->Read(&pong));
            const auto status = invalid.stream->Finish();
            invalid.finished = true;
            CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
        }
        {
            // 一条完整采样流恰好八个应答, 服务主动正常结束, 不等待第九个请求或客户端截止.
            IdlePulse complete(server->pulse_endpoint(), *client.identity, *hello);
            for (unsigned sample = 0; sample < 8; ++sample) {
                complete.ping();
            }
            proto::pulsar::v1::Pong extra;
            CHECK(!complete.stream->Read(&extra));
            const auto status = complete.stream->Finish();
            complete.finished = true;
            CHECK(status.ok());
        }
        // 没有客户端截止和超长截止均不能无限占用流, 由服务端主动回收.
        for (const auto long_deadline : {false, true}) {
            IdlePulse idle(server->pulse_endpoint(), *client.identity, *hello, long_deadline);
            idle.ping();
            const auto start = Clock::now();
            CHECK(idle.wait_expiry().error_code() == grpc::StatusCode::CANCELLED);
            CHECK(Clock::now() - start < 10s);
        }
        {
            // 多个静默流存在时, 新 Pulse 和独立 Register 仍可完成. 不将此用例当作吞吐或尾延迟基准.
            std::vector<std::unique_ptr<IdlePulse>> idle;
            for (unsigned i = 0; i < 24; ++i) {
                auto stream = std::make_unique<IdlePulse>(server->pulse_endpoint(), *client.identity, *hello);
                stream->ping();
                idle.push_back(std::move(stream));
            }
            CHECK(client.call(replacement, response).ok());
            IdlePulse active(server->pulse_endpoint(), *client.identity, *hello);
            active.ping();
            // 析构同时取消多个流, 覆盖取消与 Alarm/OnDone 交接, 后续正常采样证明配额可复用.
        }

        EpochClock clock;
        {
            PulseClient synchronizer(server->pulse_endpoint(), client.identity, hello, clock);
            const auto before = wait_clock(clock);
            CHECK(before.rtt_ns <= 200'000'000 && before.time >= EpochClock::Time(1'800'000'000s));
            // 真实网络对时产生一次期限, 后续 Pulsar 停机不应冻结清理或为数据重新续满 TTL.
            Store store;
            store.tick(before.time);
            const auto deadline = before.deadline_after(2s);
            CHECK(deadline);
            store.put("expires-during-outage", {1}, *deadline);
            const auto snapshot = store.snapshot();
            const auto previous_sample = before.sampled;
            server->stop();
            server.reset();
            // 暂停参考服务直到采样过期, 业务时间仍前进. 重启只提供新观测, 不重建 Star 的锚点.
            const auto outage_deadline = Clock::now() + 10s;
            while (Clock::now() < outage_deadline && clock.now()->ready) {
                std::this_thread::sleep_for(10ms);
            }
            CHECK(clock.now() && !clock.now()->ready && clock.now()->time > before.time);
            store.tick(clock.now()->time);
            CHECK(store.snapshot()->data.empty() && store.version() == 2);
            CHECK(snapshot->data.at("expires-during-outage").deadline == *deadline);
            const auto before_restart = clock.now()->time;
            server = std::make_unique<PulsarServer>(config, provider);
            server->start();
            const auto recovered = wait_clock(clock, previous_sample);
            CHECK(recovered.time >= before_restart);
            store.tick(recovered.time);
            CHECK(store.snapshot()->data.empty() && store.version() == 2);
            Client restored(server->admission_endpoint());
            CHECK(restored.call(replacement, response).ok());
            CHECK(response.admission() == committed && response.members_size() == 2);
            CHECK(restored.call(first, response).error_code() == grpc::StatusCode::ABORTED);
            synchronizer.stop();
        }
        CHECK(clock.now() && !clock.now()->ready);
        server->stop();
        server->stop();
        std::cout << "PASS Pulsar TLS/password admission, current topology, signed Pulse, restart recovery and Star clock reconnect\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
