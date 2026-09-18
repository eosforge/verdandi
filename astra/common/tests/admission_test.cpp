#include "admission.hpp"
#include "check.hpp"
#include "fixture.hpp"

#include <grpcpp/server_builder.h>
#include <iostream>
#include <thread>

using namespace astra;

namespace {

// 单个独立夹具的故障情景, 数值仅供本测试分支选择, 不用于传输编码.
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
    // 非空对时端点必须通过格式验证, 防止将无效地址交给后台采样线程.
    invalid_pulse,
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
    // 共享持有 identity 并固定 scenario, 测试服务只改变返回结果而不复制准入实现.
    Authority(std::shared_ptr<Identity> identity, Scenario scenario) : identity_(std::move(identity)), scenario_(scenario) {}

    // registrations 从零统计实际进入服务处理器的次数, 供客户端检查重试和传输拦截.
    std::atomic_uint registrations{};
    // request_changed 初始 false, 任一重试改变启动幂等键时置 true.
    std::atomic_bool request_changed{};

    // 借用本次 context/request/response, 按 scenario 制造响应或等待取消, 请求不能越过 handler 寿命.
    grpc::Status Register(grpc::ServerContext* context, const proto::orbit::v1::RegistrationRequest* request, proto::orbit::v1::RegistrationResponse* response) override {

        // attempt 从一编号实际请求, 用于只在首次丢弃应答或后续返回不同身份.
        const auto attempt = ++registrations;
        // RPC 可以由不同 worker 执行, 对记录的请求键使用短锁. 不从客户端请求构造实例 ID.
        {
            // lock 仅保护测试服务观察到的请求集合, 避免并发 RPC 写入竞争.
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

        // member 为独立应答身份, 除故障字段外保持与当前请求的部署一致.
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
        } else if (scenario_ == Scenario::invalid_pulse) {
            response->set_pulse_endpoint("not-an-endpoint");
        }
        return grpc::Status::OK;
    }

private:
    // 只保护跨 RPC 观察的 request_id_, 不在等待取消期间持锁.
    std::mutex mutex_;
    // 首次请求的 32 字节启动键, 初始为空, 后续请求只与其比较.
    std::string request_id_;
    // 共享测试身份, 确保处理器存活期间摘要计算材料仍有效.
    std::shared_ptr<Identity> identity_;
    // 本夹具固定故障情景, 构造后只读, 不在并发 RPC 间切换.
    Scenario scenario_;
};

struct Fixture {
    // authority 比服务器先构造后销毁, 覆盖全部 handler 的借用寿命.
    Authority authority;
    // server 独占本例动态端口监听, 析构先 Shutdown/Wait 后销毁处理器.
    std::unique_ptr<grpc::Server> server;
    // client 独占被测准入状态机, 析构前必须取消并消费最终完成.
    std::unique_ptr<Admission> client;

    // 为 scenario 创建本机 TLS 监听和被测客户端, identity 来自公开夹具, 端口由系统分配.
    Fixture(std::shared_ptr<Identity> identity, Scenario scenario) : authority(identity, scenario) {

        // builder 仅创建当前夹具的 TLS 服务, 不操作项目外已有监听.
        grpc::ServerBuilder builder;
        builder.RegisterService(&authority);
        // port 接收临时监听端口, 成功必须大于零, 不使用固定共享测试端口.
        int port = 0;
        // address 通常为 127.0.0.1, wrong_san 改为未被证书授权的 127.0.0.2.
        const std::string address = scenario == Scenario::wrong_san ? "127.0.0.2" : "127.0.0.1";
        // credentials 默认来自有效身份, 特定 TLS 失败用例显式替换成错误材料.
        auto credentials = identity->server_credentials();
        if (scenario == Scenario::untrusted_root || scenario == Scenario::expired_certificate) {
            // 测试服务显式绕过自己的启动检查, 用无效证书验证被测客户端仍拒绝 TLS.
            const auto fixtures = std::filesystem::path(ASTRA_FIXTURES);
            // directory 只选择仓库公开夹具目录, 根据情景提供过期或不受信任证书.
            const auto directory = scenario == Scenario::expired_certificate ? fixtures / "expired" : fixtures.parent_path().parent_path().parent_path() / "testkit/tls";
            // certificate 是所选夹具中固定的证书文件名, 不接收外部路径输入.
            const auto certificate = scenario == Scenario::expired_certificate ? "cert.pem" : "certificate.pem";
            // key 是所选夹具中对应的公开测试私钥文件名.
            const auto key = scenario == Scenario::expired_certificate ? "key.pem" : "private-key.pem";
            // options 仅用于让测试服务启动错误 TLS 材料, 生产启动检查保持不变.
            grpc::SslServerCredentialsOptions options;
            options.pem_key_cert_pairs.push_back({test::read(directory / key), test::read(directory / certificate)});
            credentials = grpc::SslServerCredentials(options);
        }
        builder.AddListeningPort(address + ":0", credentials, &port);
        server = builder.BuildAndStart();
        CHECK(server && port > 0);
        // config 为客户端连接本例临时服务, 仅按故障情景调整容量或连接截止.
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

    // 取消并排空本例 RPC 后关闭监听; 无法排空时立即非零退出, 不销毁仍被借用的请求.
    ~Fixture() {

        // 测试断言失败也先取消并排空本例拥有的 RPC, 避免析构正在被借用的请求.
        if (client) {
            client->cancel();
            // deadline 为此等待阶段的单调硬上限, 防止断言失败后清理或轮询无限挂起.
            const auto deadline = Steady::now() + std::chrono::seconds(7);
            while (client->pending() && Steady::now() < deadline) {
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

    // 最多等待七秒并推进客户端, 返回单次尝试结果, 无结果超时抛异常.
    Result<Admission::Joined> result() {

        // deadline 为此等待阶段的单调硬上限, 防止断言失败后清理或轮询无限挂起.
        const auto deadline = Steady::now() + std::chrono::seconds(7);
        while (Steady::now() < deadline) {
            if (auto value = client->poll()) {
                return std::move(*value);
            }
            std::this_thread::sleep_for(Milliseconds(1));
        }
        throw std::runtime_error("Admission test exceeded deadline");
    }
};

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
int main() {

    try {
        // identity 从公开 star-a 材料加载, 作为全部独立 TLS 情景的可信基准.
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
            // first 应表示首次应答丢失的传输失败, 后续复用同一个启动请求重试.
            auto first = fixture.result();
            CHECK(!first && first.error().code == Status::Code::transport);
            fixture.client->begin(0);
            // joined 为重试后的准入结果, 不要求旧控制面提供 Pulse 端点.
            const auto joined = fixture.result();
            CHECK(joined && joined->pulse_endpoint.empty());
            CHECK(fixture.authority.registrations == 2 && !fixture.authority.request_changed);
        }
        {
            Fixture fixture(*identity, Scenario::changed_identity);
            fixture.client->begin(0);
            CHECK(fixture.result());
            fixture.client->begin(1);
            // changed 是刷新返回另一实例的结果, 应被判为身份错误而保留原身份.
            auto changed = fixture.result();
            CHECK(!changed && changed.error().code == Status::Code::identity);
            CHECK(!fixture.authority.request_changed);
        }
        for (auto scenario : {Scenario::excessive_members, Scenario::wrong_identity, Scenario::oversized_reply, Scenario::empty_member, Scenario::reply_limit, Scenario::request_limit, Scenario::invalid_pulse}) {
            Fixture fixture(*identity, scenario);
            fixture.client->begin(0);
            // result 为当前情景的一次完整尝试结果, 随后按情景核对明确错误分类.
            auto result = fixture.result();
            CHECK(!result);
            // capacity 区分容量类故障与身份类故障, 不以任意失败代替预期分类.
            const bool capacity = scenario == Scenario::excessive_members || scenario == Scenario::oversized_reply || scenario == Scenario::reply_limit || scenario == Scenario::request_limit;
            CHECK(result.error().code == (capacity ? Status::Code::capacity : Status::Code::identity));
            if (scenario == Scenario::request_limit) {
                CHECK(fixture.authority.registrations == 0);
            }
        }
        for (auto scenario : {Scenario::registration_deadline}) {
            Fixture fixture(*identity, scenario);
            // start 捕获整个登记尝试的开始时刻, 验证连接成功不会重置握手总预算.
            const auto start = Steady::now();
            fixture.client->begin(0);
            // result 为当前情景的一次完整尝试结果, 随后按情景核对明确错误分类.
            auto result = fixture.result();
            CHECK(!result && result.error().code == Status::Code::timeout);
            // 单次登记继续受总期限限制, 不能因通道已连上而无限等待.
            CHECK(Steady::now() - start < Milliseconds(6500));
            CHECK(fixture.authority.registrations == 1U);
        }
        {
            Fixture fixture(*identity, Scenario::cancellation);
            fixture.client->begin(0);
            // deadline 为此等待阶段的单调硬上限, 防止断言失败后清理或轮询无限挂起.
            const auto deadline = Steady::now() + std::chrono::seconds(3);
            while (fixture.authority.registrations == 0 && Steady::now() < deadline) {
                CHECK(!fixture.client->poll());
                std::this_thread::sleep_for(Milliseconds(1));
            }
            CHECK(fixture.authority.registrations == 1);
            fixture.client->cancel();
            // result 为当前情景的一次完整尝试结果, 随后按情景核对明确错误分类.
            auto result = fixture.result();
            CHECK(!result && result.error().code == Status::Code::cancelled);
        }
        std::cout << "PASS single RPC and retained startup request, reply limits, identity mismatch and in-flight cancellation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
