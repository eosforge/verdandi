// 功能: 通过真实 TLS/gRPC 服务验证准入重试, 身份校验, 取消和截止边界.
#include "admission.hpp"
#include "check.hpp"
#include "fixture.hpp"

#include <grpcpp/server_builder.h>
#include <iostream>
#include <thread>

using namespace astra;

enum class Scenario {
    // 模拟首次登记响应丢失, 检查重试保持本次启动的幂等键.
    lost_reply,
    // 返回超出角色容量的名单, 检查准入结果被拒绝.
    excessive_members,
    // 返回不属于当前进程的签名身份, 检查身份绑定校验.
    wrong_identity,
    // 保持登记 RPC 在途后由客户端取消, 检查回调排空与取消结果.
    cancellation,
    // 返回超过传输接收限制的响应, 检查消息容量边界.
    oversized_reply,
    // 自定义更小的响应上限必须作用于实际 gRPC 通道.
    reply_limit,
    // 请求超出配置上限时不能到达服务端登记处理器.
    request_limit,
    // 返回缺少有效身份字段的成员, 检查名单逐项校验.
    empty_member,
    // 阻塞登记直到截止, 检查单次 RPC 的超时收尾.
    registration_deadline,
    // 首次成功后返回另一实例, 检查刷新不能替换本地身份.
    changed_identity,
    // 使用与目标地址不匹配的证书 SAN, 检查 TLS 端点身份验证.
    wrong_san,
    // 使用不受客户端信任的签发根, 检查 TLS 信任链验证.
    untrusted_root,
    // 使用过期证书, 检查 TLS 有效期验证.
    expired_certificate
};

// 真实 TLS/gRPC 服务只操纵响应和截止, 不复制 C++ Admission 的重试状态机.
class Authority final : public proto::orbit::v1::Admission::Service {
public:
    Authority(std::shared_ptr<Identity> identity, Scenario scenario) : identity_(std::move(identity)), scenario_(scenario) {}
    std::atomic_uint registrations{};
    std::atomic_bool request_changed{};
    grpc::Status Register(grpc::ServerContext* context, const proto::orbit::v1::RegistrationRequest* request,
                          proto::orbit::v1::RegistrationResponse* response) override {
        const auto attempt = ++registrations;
        // RPC 可以由不同 worker 执行, 对记录的请求键使用短锁. 不从客户端请求构造实例 ID.
        {
            std::lock_guard lock(mutex_);
            if (request_id_.empty()) {
                request_id_ = request->request_id();
            }
            if (request->request_id().size() != 32 || request_id_ != request->request_id()) {
                request_changed = true;
            }
        }
        if (scenario_ == Scenario::cancellation || scenario_ == Scenario::registration_deadline) {
            while (!context->IsCancelled()) {
                std::this_thread::sleep_for(Milliseconds(1));
            }
            return grpc::Status(grpc::StatusCode::CANCELLED, "Test cancelled");
        }
        if (scenario_ == Scenario::lost_reply && attempt == 1) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Simulated reply loss after registration");
        }
        proto::orbit::v1::Member member;
        member.set_galaxy(request->galaxy());
        member.set_advertise(request->advertise());
        member.set_id(scenario_ == Scenario::changed_identity && attempt > 1 ? "other-instance" : "issued/session:α/42");
        member.set_role(request->role());
        member.set_group(scenario_ == Scenario::wrong_identity ? "wrong" : request->group());
        member.set_epoch(1);
        member.set_principal(identity_->principal(member.galaxy(), member.advertise()).text());
        response->set_admission(member.SerializeAsString());
        response->set_signature(test::sign(response->admission()));
        for (unsigned n = 0; n < (scenario_ == Scenario::excessive_members ? 5U : 1U); ++n) {
            response->add_members()->CopyFrom(member);
        }
        if (scenario_ == Scenario::oversized_reply) {
            response->mutable_members(0)->set_group(std::string(2 * 1024 * 1024, 'x'));
        } else if (scenario_ == Scenario::reply_limit) {
            response->mutable_members(0)->set_group(std::string(2048, 'x'));
        } else if (scenario_ == Scenario::empty_member) {
            response->mutable_members(0)->Clear();
        }
        return grpc::Status::OK;
    }

private:
    std::mutex mutex_;
    std::string request_id_;
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
            const auto fixtures = std::filesystem::path(ASTRA_FIXTURES);
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
        config.galaxy = "alpha";
        config.advertise = *Endpoint::parse("127.0.0.1:7443");
        config.supervisor = address + ":" + std::to_string(port);
        config.max_members = 4;
        if (scenario == Scenario::reply_limit) {
            config.max_admission_response_bytes = 1024;
        } else if (scenario == Scenario::request_limit) {
            config.max_admission_request_bytes = 1;
        }
        if (scenario == Scenario::wrong_san || scenario == Scenario::untrusted_root || scenario == Scenario::expired_certificate) {
            config.connect_timeout = Milliseconds(300);
        }
        client = std::make_unique<Admission>(config, identity, [] {});
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
        auto identity = Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", *Endpoint::parse("127.0.0.1:7443"));
        CHECK(identity);
        for (auto scenario : {Scenario::wrong_san, Scenario::untrusted_root, Scenario::expired_certificate}) {
            Fixture fixture(*identity, scenario);
            fixture.client->begin(0);
            CHECK(!fixture.result());
            CHECK(fixture.authority.registrations == 0);
        }
        {
            Fixture fixture(*identity, Scenario::lost_reply);
            fixture.client->begin(0);
            auto first = fixture.result();
            CHECK(!first && first.error().code == Error::Code::transport);
            fixture.client->begin(0);
            CHECK(fixture.result());
            CHECK(fixture.authority.registrations == 2 && !fixture.authority.request_changed);
        }
        {
            Fixture fixture(*identity, Scenario::changed_identity);
            fixture.client->begin(0);
            CHECK(fixture.result());
            fixture.client->begin(1);
            auto changed = fixture.result();
            CHECK(!changed && changed.error().code == Error::Code::identity);
            CHECK(!fixture.authority.request_changed);
        }
        for (auto scenario : {Scenario::excessive_members, Scenario::wrong_identity, Scenario::oversized_reply, Scenario::empty_member, Scenario::reply_limit,
                              Scenario::request_limit}) {
            Fixture fixture(*identity, scenario);
            fixture.client->begin(0);
            auto result = fixture.result();
            CHECK(!result);
            const bool capacity = scenario == Scenario::excessive_members || scenario == Scenario::oversized_reply || scenario == Scenario::reply_limit ||
                                  scenario == Scenario::request_limit;
            CHECK(result.error().code == (capacity ? Error::Code::capacity : Error::Code::identity));
            if (scenario == Scenario::request_limit) {
                CHECK(fixture.authority.registrations == 0);
            }
        }
        for (auto scenario : {Scenario::registration_deadline}) {
            Fixture fixture(*identity, scenario);
            const auto start = Clock::now();
            fixture.client->begin(0);
            auto result = fixture.result();
            CHECK(!result && result.error().code == Error::Code::timeout);
            // 单次登记继续受总期限限制, 不能因通道已连上而无限等待.
            CHECK(Clock::now() - start < Milliseconds(6500));
            CHECK(fixture.authority.registrations == 1U);
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
            CHECK(!result && result.error().code == Error::Code::cancelled);
        }
        std::cout << "PASS single RPC and retained startup request, reply limits, identity mismatch and in-flight cancellation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
