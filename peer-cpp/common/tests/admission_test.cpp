#include "admission.hpp"
#include "fixture.hpp"

#include <grpcpp/server_builder.h>
#include <iostream>
#include <thread>

using namespace verdandi::peer;
#define CHECK(condition)                                                                                                                                       \
    do {                                                                                                                                                       \
        if (!(condition))                                                                                                                                      \
            throw std::runtime_error("Admission check failed at line " + std::to_string(__LINE__));                                                            \
    } while (false)

enum class Scenario {
    lost_reply,
    excessive_members,
    wrong_identity,
    cancellation,
    oversized_reply,
    empty_member,
    challenge_deadline,
    total_deadline,
    wrong_san,
    untrusted_root,
    expired_certificate
};

// 真实 TLS/gRPC 服务只操纵响应和截止, 不复制 C++ Admission 的重试状态机.
class Authority final : public wire::Admission::Service {
public:
    Authority(std::shared_ptr<Identity> identity, Scenario scenario) : identity_(std::move(identity)), scenario_(scenario) {}
    std::atomic_uint challenges{}, registrations{};
    std::atomic_bool baseline_changed{};
    grpc::Status Challenge(grpc::ServerContext* context, const wire::LoginRequest* request, wire::RegistrationChallenge* response) override {
        ++challenges;
        if (scenario_ == Scenario::challenge_deadline) {
            while (!context->IsCancelled()) {
                std::this_thread::sleep_for(Milliseconds(1));
            }
            return grpc::Status(grpc::StatusCode::CANCELLED, "Test deadline");
        }
        if (scenario_ == Scenario::total_deadline) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        response->set_cluster_id(request->cluster_id());
        response->set_expected_epoch(0);
        return grpc::Status::OK;
    }
    grpc::Status Register(grpc::ServerContext* context, const wire::RegistrationRequest* request, wire::RegistrationResponse* response) override {
        const auto attempt = ++registrations;
        if (request->expected_epoch() != 0) {
            baseline_changed = true;
        }
        if (scenario_ == Scenario::cancellation || scenario_ == Scenario::total_deadline) {
            while (!context->IsCancelled()) {
                std::this_thread::sleep_for(Milliseconds(1));
            }
            return grpc::Status(grpc::StatusCode::CANCELLED, "Test cancelled");
        }
        if (scenario_ == Scenario::lost_reply && attempt == 1) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Simulated reply loss after registration");
        }
        wire::RegistrationResponse::Member member;
        member.set_cluster_id(request->cluster_id());
        member.set_advertise(request->advertise());
        member.set_peer_id(request->peer_id());
        member.set_role(request->role());
        member.set_group(scenario_ == Scenario::wrong_identity ? "wrong" : request->group());
        member.set_epoch(1);
        member.set_principal(identity_->principal(member.cluster_id(), member.advertise()).text());
        response->set_admission(member.SerializeAsString());
        response->set_signature(test::sign(response->admission()));
        for (unsigned n = 0; n < (scenario_ == Scenario::excessive_members ? 5U : 1U); ++n) {
            response->add_members()->CopyFrom(member);
        }
        if (scenario_ == Scenario::oversized_reply) {
            response->mutable_members(0)->set_group(std::string(2 * 1024 * 1024, 'x'));
        } else if (scenario_ == Scenario::empty_member) {
            response->mutable_members(0)->Clear();
        }
        return grpc::Status::OK;
    }

private:
    std::shared_ptr<Identity> identity_;
    Scenario scenario_;
};

struct Fixture {
    Authority authority;
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<Admission> client;
    Fixture(std::shared_ptr<Identity> identity, Scenario scenario) : authority(identity, scenario) {
        grpc::ServerBuilder builder;
        builder.RegisterService(&authority);
        int port = 0;
        const std::string address = scenario == Scenario::wrong_san ? "127.0.0.2" : "127.0.0.1";
        auto credentials = identity->server_credentials();
        if (scenario == Scenario::untrusted_root || scenario == Scenario::expired_certificate) {
            // 测试服务显式绕过自己的启动检查, 用无效证书验证被测客户端仍拒绝 TLS.
            const auto fixtures = std::filesystem::path(VERDANDI_FIXTURES);
            const auto directory =
                scenario == Scenario::expired_certificate ? fixtures / "expired" : fixtures.parent_path().parent_path().parent_path() / "testkit/tls";
            const auto certificate = scenario == Scenario::expired_certificate ? "cert.pem" : "certificate.pem";
            const auto key = scenario == Scenario::expired_certificate ? "key.pem" : "private-key.pem";
            grpc::SslServerCredentialsOptions options;
            options.pem_key_cert_pairs.push_back({test::read(directory / key), test::read(directory / certificate)});
            credentials = grpc::SslServerCredentials(options);
        }
        builder.AddListeningPort(address + ":0", credentials, &port);
        server = builder.BuildAndStart();
        CHECK(server && port > 0);
        Config config;
        config.cluster = "alpha";
        config.advertise = *Endpoint::parse("127.0.0.1:7443");
        config.supervisor = address + ":" + std::to_string(port);
        config.max_peers = 4;
        if (scenario == Scenario::wrong_san || scenario == Scenario::untrusted_root || scenario == Scenario::expired_certificate) {
            config.connect_timeout = Milliseconds(300);
        }
        const auto id = Identity::new_id();
        CHECK(id);
        client = std::make_unique<Admission>(config, identity, *id, [] {});
    }
    ~Fixture() {
        // 测试断言失败也先取消并排空本例拥有的 RPC, 避免析构正在被借用的请求.
        if (client) {
            client->cancel();
            const auto deadline = Clock::now() + std::chrono::seconds(7);
            while (client->pending() && Clock::now() < deadline) {
                static_cast<void>(client->poll());
                std::this_thread::sleep_for(Milliseconds(1));
            }
            if (client->pending()) {
                std::_Exit(3);
            }
        }
        if (server) {
            server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
            server->Wait();
        }
    }
    Result<Joined> result() {
        const auto deadline = Clock::now() + std::chrono::seconds(7);
        while (Clock::now() < deadline) {
            if (auto value = client->poll()) {
                return std::move(*value);
            }
            std::this_thread::sleep_for(Milliseconds(1));
        }
        throw std::runtime_error("Admission test exceeded deadline");
    }
};

int main() {
    try {
        auto identity = Identity::load(std::filesystem::path(VERDANDI_FIXTURES) / "peer-a", *Endpoint::parse("127.0.0.1:7443"));
        CHECK(identity);
        for (auto scenario : {Scenario::wrong_san, Scenario::untrusted_root, Scenario::expired_certificate}) {
            Fixture fixture(*identity, scenario);
            fixture.client->begin(0);
            CHECK(!fixture.result());
            CHECK(fixture.authority.challenges == 0 && fixture.authority.registrations == 0);
        }
        {
            Fixture fixture(*identity, Scenario::lost_reply);
            fixture.client->begin(0);
            auto first = fixture.result();
            CHECK(!first && first.error().code == ErrorCode::transport);
            fixture.client->begin(0);
            CHECK(fixture.result());
            CHECK(fixture.authority.challenges == 1 && fixture.authority.registrations == 2 && !fixture.authority.baseline_changed);
        }
        for (auto scenario : {Scenario::excessive_members, Scenario::wrong_identity, Scenario::oversized_reply, Scenario::empty_member}) {
            Fixture fixture(*identity, scenario);
            fixture.client->begin(0);
            auto result = fixture.result();
            CHECK(!result);
            const bool capacity = scenario == Scenario::excessive_members || scenario == Scenario::oversized_reply;
            CHECK(result.error().code == (capacity ? ErrorCode::capacity : ErrorCode::identity));
        }
        for (auto scenario : {Scenario::challenge_deadline, Scenario::total_deadline}) {
            Fixture fixture(*identity, scenario);
            const auto start = Clock::now();
            fixture.client->begin(0);
            auto result = fixture.result();
            CHECK(!result && result.error().code == ErrorCode::timeout);
            // Challenge 消耗三秒后, Register 只能继续使用原先剩余的两秒, 不能再获得五秒.
            CHECK(Clock::now() - start < Milliseconds(6500));
            CHECK(fixture.authority.registrations == (scenario == Scenario::total_deadline ? 1U : 0U));
        }
        {
            Fixture fixture(*identity, Scenario::cancellation);
            fixture.client->begin(0);
            const auto deadline = Clock::now() + std::chrono::seconds(3);
            while (fixture.authority.registrations == 0 && Clock::now() < deadline) {
                CHECK(!fixture.client->poll());
                std::this_thread::sleep_for(Milliseconds(1));
            }
            CHECK(fixture.authority.registrations == 1);
            fixture.client->cancel();
            auto result = fixture.result();
            CHECK(!result && result.error().code == ErrorCode::cancelled);
        }
        std::cout << "PASS retained CAS baseline, reply limits, identity mismatch and in-flight cancellation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
