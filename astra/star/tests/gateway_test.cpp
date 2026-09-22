#include "check.hpp"
#include "gateway.hpp"
#include "identity.hpp"
#include <grpcpp/create_channel.h>
#include <grpcpp/server_builder.h>
#include <iostream>

namespace {
using astra::Access;
using astra::Almanac;
using astra::Gateway;

// 模拟一个真实业务提交入口, 借生产 Gateway 检查初始 metadata 和最终 Permit, 不实现假 Catalog 存储.
class Probe final : public proto::comet::v1::Catalog::CallbackService {
public:
    // gateway 覆盖全部 Probe RPC 的寿命, 本例只复用准入边界.
    explicit Probe(Gateway& gateway) : gateway_(gateway) {}

    // 有效令牌才返回实例; 同一个 Channel 的其他独立 RPC 不自动继承前一请求权限.
    grpc::ServerUnaryReactor* Publish(grpc::CallbackServerContext* context, const proto::comet::v1::PublishRequest*, proto::comet::v1::PublishReply* reply) override {

        auto* reactor = context->DefaultReactor(); // 标准 unary reactor 由 gRPC 所有.
        auto permit = gateway_.enter(*context);    // 保留至本次模拟提交完成, 不反复校验 SECRET.
        if (!permit) {
            reactor->Finish(permit.error());
        } else {
            reply->set_instance(gateway_.instance());
            reactor->Finish(grpc::Status::OK);
        }
        return reactor;
    }

private:
    // 固定借用同一 Gateway, 不复制会话表.
    Gateway& gateway_;
};

// 每例真实 TLS Server 与 Channel 采用动态回环端口, 不依赖部署进程或公开业务监听.
class Fixture {
public:
    // 原生权限状态, 容量与网关相同, 在服务器之前构造, 之后释放.
    Access access;
    // 被测逻辑流服务, 默认启用登录, 初始由构造函数显式 ready.
    Gateway gateway;
    // 只验证 metadata 的独立 unary 测试服务.
    Probe probe;
    // 测试独占监听, 只在正常排空后释放.
    std::unique_ptr<grpc::Server> server;
    // 真实客户端 Channel, TLS 主机验证采用公开本地证书夹具.
    std::shared_ptr<grpc::Channel> channel;
    // 两种生成 Stub, 共用同一 Channel 但仍逐 RPC 传 session metadata.
    std::unique_ptr<proto::comet::v1::Gateway::Stub> login;
    // 真实 unary Stub, 响应仅用于确认授权状态.
    std::unique_ptr<proto::comet::v1::Catalog::Stub> catalog;

    // auth/maximum/readiness 对应待测入口边界, 不把无证书通道混入 TLS 测试.
    explicit Fixture(bool auth = true, std::size_t maximum = 4, bool readiness = true) : access(maximum), gateway(access, "star-test", auth, static_cast<std::ptrdiff_t>(maximum)), probe(gateway) {

        Access::Draft draft; // 当前账号拥有含零字节的合法 SECRET.
        CHECK(draft.set("application", {0, 1, 255}));
        CHECK(access.reset(std::move(draft), [] { return std::expected<bool, Almanac::Error>(true); }));
        const auto identity = astra::Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", *astra::Endpoint::parse("127.0.0.1:7443"));
        CHECK(identity);
        grpc::ServerBuilder builder; // 只注册公共服务和测试 probe, 没有任何 Orbit/Pulse 接口.
        builder.RegisterService(&gateway);
        builder.RegisterService(&probe);
        int port{}; // 由系统分配回环端口, 零表示尚未成功绑定.
        builder.AddListeningPort("127.0.0.1:0", (*identity)->server_credentials(), &port);
        server = builder.BuildAndStart();
        CHECK(server && port > 0);
        try {
            channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), (*identity)->channel_credentials());
            login = proto::comet::v1::Gateway::NewStub(channel);
            catalog = proto::comet::v1::Catalog::NewStub(channel);
            if (readiness) {
                gateway.ready();
            }
        } catch (...) {
            server->Shutdown();
            server->Wait();
            throw;
        }
    }

    // 先停止所有逻辑流再给 gRPC 有限关闭预算, 不依赖客户端析构碰巧结束空闲 Session.
    ~Fixture() {
        gateway.stop();
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(3));
        server->Wait();
    }

    // 独立业务请求, tokens 允许故意缺失/重复/非 32 字节, 失败细节必须保持可解析.
    grpc::Status call(std::initializer_list<std::string_view> tokens = {}) {

        grpc::ClientContext context; // 每个调用的 metadata 只属于自己, 不继承前一请求.
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
        for (const auto token : tokens) {
            context.AddMetadata("comet-session-bin", std::string(token));
        }
        proto::comet::v1::PublishRequest request; // Probe 不解释业务正文, 专门隔离会话许可测试.
        proto::comet::v1::PublishReply reply;
        const auto status = catalog->Publish(&context, request, &reply);
        if (status.ok()) {
            CHECK(reply.instance() == "star-test");
        } else {
            const auto details = context.GetServerTrailingMetadata().find("comet-error-bin");
            CHECK(details != context.GetServerTrailingMetadata().end());
            proto::comet::v1::Failure failure;
            CHECK(failure.ParseFromArray(details->second.data(), static_cast<int>(details->second.size())));
            CHECK(failure.effect() == proto::comet::v1::EFFECT_UNAPPLIED && failure.instance() == "star-test");
        }
        return status;
    }
};

// 客户端长期 RPC 的唯一拥有者, 例外展开也取消并 Finish, 不遗留服务端活动会话.
class Login {
public:
    // 稳定地址的客户端上下文, 不随 Login 移动.
    grpc::ClientContext context;
    // 唯一服务端流, 有效直到调用 finish 或析构.
    std::unique_ptr<grpc::ClientReader<proto::comet::v1::SessionReply>> stream;
    // 成功确认的原始 token/实际实例, 未读取成功时为空.
    proto::comet::v1::SessionReply reply;
    // 第一条是否确实成功收到, 失败时不能把默认消息当作认证成功.
    bool accepted{};

    // correct=false 发送错误 SECRET, 服务端应明确拒绝而不消耗活动会话容量.
    explicit Login(Fixture& fixture, bool correct = true) {
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
        proto::comet::v1::SessionRequest request; // 请求仅在 stub 发起时使用, 长期流不再次发送密码.
        request.set_key("application");
        request.set_secret(correct ? std::string("\0\1\xff", 3) : "wrong");
        stream = fixture.login->Session(&context, request);
        accepted = stream->Read(&reply);
    }

    // 最终取消仅针对本 RPC, 不能取消共用 Channel 的其他登录或 unary.
    ~Login() {
        if (stream) {
            context.TryCancel();
            static_cast<void>(stream->Finish());
        }
    }

    // 收尾真实流, 此后析构不重复 Finish; 只在已收到 EOF 或显式取消之后调用.
    grpc::Status finish() {
        const auto status = stream->Finish();
        stream.reset();
        return status;
    }
};

// 登录与 unary 拥有不同的 RPC 上下文, 只允许正确且仍有效的唯一 token.
void metadata() {

    Fixture fixture;
    Login rejected(fixture, false);
    CHECK(!rejected.accepted && rejected.finish().error_code() == grpc::StatusCode::UNAUTHENTICATED);
    Login active(fixture);
    CHECK(active.accepted && active.reply.instance() == "star-test" && active.reply.session().size() == 32);
    CHECK(fixture.call().error_code() == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(fixture.call({"short"}).error_code() == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(fixture.call({active.reply.session(), active.reply.session()}).error_code() == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(fixture.call({active.reply.session()}).ok());
    active.context.TryCancel();
    CHECK(!active.finish().ok());
}

// SECRET 轮换不等 OnDone 才禁止旧业务, 完整快照恢复后新会话不会被迟到清理误伤.
void revocation() {

    Fixture fixture;
    Login previous(fixture);
    CHECK(previous.accepted);
    CHECK(fixture.access.apply("application", Almanac::Buffer{2}, [] { return std::expected<bool, Almanac::Error>(true); }));
    CHECK(fixture.call({previous.reply.session()}).error_code() == grpc::StatusCode::UNAUTHENTICATED);
    proto::comet::v1::SessionReply extra; // Session 不得发送第二份成功确认.
    CHECK(!previous.stream->Read(&extra));
    CHECK(!previous.finish().ok());
    Access::Draft restored;
    CHECK(restored.set("application", {0, 1, 255}));
    CHECK(fixture.access.reset(std::move(restored), [] { return std::expected<bool, Almanac::Error>(true); }));
    Login current(fixture);
    CHECK(current.accepted && fixture.call({current.reply.session()}).ok());
    fixture.gateway.stop();
    CHECK(!current.stream->Read(&extra));
    CHECK(!current.finish().ok());
    fixture.gateway.ready(); // stop 是永久边界, 不因随后误调 ready 重新接纳.
    CHECK(fixture.call({current.reply.session()}).error_code() == grpc::StatusCode::UNAVAILABLE);
}

// 匿名模式不签发伪会话, 空闲真实 Session 必须占用容量, 未就绪服务不能登录或执行业务.
void modes() {

    {
        Fixture fixture(false);
        Login login(fixture);
        CHECK(!login.accepted && login.finish().error_code() == grpc::StatusCode::FAILED_PRECONDITION);
        CHECK(fixture.call().ok());
    }
    {
        Fixture fixture(true, 1);
        Login first(fixture);
        CHECK(first.accepted);
        Login second(fixture);
        CHECK(!second.accepted && second.finish().error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED);
        CHECK(fixture.call({first.reply.session()}).ok());
    }
    {
        Fixture fixture(true, 4, false);
        Login login(fixture);
        CHECK(!login.accepted && login.finish().error_code() == grpc::StatusCode::UNAVAILABLE);
        CHECK(fixture.call().error_code() == grpc::StatusCode::UNAVAILABLE);
    }
}
} // namespace

// 显式运行时执行真实 TLS/gRPC 验证, 不读取业务部署账号或启动外部服务.
int main() {
    try {
        metadata();
        revocation();
        modes();
        std::cout << "PASS business gateway TLS sessions and cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
