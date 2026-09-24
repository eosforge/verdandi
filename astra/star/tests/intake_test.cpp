#include "check.hpp"
#include "fixture.hpp"
#include "intake.hpp"
#include <grpcpp/server_builder.h>
#include <iostream>
#include <thread>

using namespace astra;

namespace {
// 真实 TLS 通道上的协议情景, 不替换生产接收状态或 gRPC 回调实现.
enum class Scenario {
    // 分页快照, 连续后缀, Probe 及完整安装确认.
    complete,
    // 对端只有 Star 身份, 不能成为 Almanac 发布权威.
    role,
    // 原始凭证签名损坏, 拒绝接受首份快照.
    signature,
    // 半份快照后断线, 私有准备不能泄漏进 Library.
    interrupted,
    // 已安装基线之后跳号, 保留已成功版本并关闭当前流.
    gap,
    // 相同部署的较高身份代次可以替换过时目录目标.
    replacement
};

// 固定公开测试身份, endpoint 是实际回环端点, role 区分 Star 与 Polaris, epoch 默认为 1.
Member member(const Endpoint& endpoint, Member::Role role, std::uint64_t epoch = 1) {
    return Member{"alpha", role == Member::Role::polaris ? "polaris-" + std::to_string(epoch) : "star-instance", *Principal::parse(std::string(64, role == Member::Role::polaris ? 'a' : 'b')), endpoint, {epoch}, role, "default"};
}

// 用公开签发夹具生成原始 Hello, 不从部署目录取得任何私钥.
proto::astra::v1::Hello hello(const Member& value) {

    proto::orbit::v1::Member encoded;
    encoded.set_galaxy(value.galaxy);
    encoded.set_id(value.id);
    encoded.set_principal(value.principal.bytes.data(), value.principal.bytes.size());
    encoded.set_advertise(value.address.text());
    encoded.set_epoch(value.epoch.value);
    encoded.set_role(Identity::role(value.role));
    encoded.set_group(value.group);
    proto::astra::v1::Hello result;
    result.set_protocol_major(1);
    result.set_max_frame_bytes(1024); // 故意声明较小接收量, 检查客户端清单不会沿用 256 KiB.
    result.set_admission(encoded.SerializeAsString());
    result.set_admission_signature(test::sign(result.admission()));
    return result;
}

// 只用于测试的有限同步服务, 生产 Polaris 仍为 Go. 客户端取消后阻塞读取必须结束.
class Authority final : public proto::polaris::v1::Almanac::Service {
public:
    // scene 固定本例行为, 构造不启动线程或修改 Library.
    explicit Authority(Scenario scene) : scene_(scene) {}

    // greeting 在启动客户端之前填入, 后续 handler 只读.
    proto::astra::v1::Hello greeting;
    // received 统计真实收到的清单项, verified 标记真实累计 ACK 和 Probe 均匹配.
    std::atomic_size_t received{};
    // 整条正常场景由真实消息完成后置位, 默认 false.
    std::atomic_bool verified{};
    // handler 的断言失败经固定错误返回, 同时记录标志供主测试线程观察.
    std::atomic_bool failed{};

    // context/stream 只在 handler 内借用. 抛异常不会穿过 gRPC 服务边界.
    grpc::Status Open(grpc::ServerContext*, grpc::ServerReaderWriter<proto::polaris::v1::Packet, proto::polaris::v1::Packet>* stream) override {

        try {
            proto::polaris::v1::Packet packet;
            CHECK(stream->Read(&packet) && packet.has_hello());
            packet.Clear();
            *packet.mutable_hello() = greeting;
            CHECK(stream->Write(packet));
            if (scene_ == Scenario::role || scene_ == Scenario::signature) {
                while (stream->Read(&packet)) {}
                return grpc::Status::OK;
            }

            // inventory 逐页累计, 不把收到第一条空/非空页当成整体完成.
            proto::polaris::v1::Inventory inventory;
            do {
                CHECK(stream->Read(&packet) && packet.has_inventory() && packet.ByteSizeLong() <= 1024);
                for (const auto& position : packet.inventory().positions()) {
                    *inventory.add_positions() = position;
                    ++received;
                }
            } while (!packet.inventory().complete());
            packet.Clear();
            *packet.mutable_plan() = inventory;
            auto* required = packet.mutable_plan()->add_positions();
            required->mutable_scope()->set_sector("a");
            required->mutable_scope()->set_spectrum("s");
            required->set_version(1);
            packet.mutable_plan()->set_complete(true);
            CHECK(stream->Write(packet));

            // 首页没有 complete, 断线场景到此结束, Library 必须仍没有 a/s.
            packet.Clear();
            auto* page = packet.mutable_snapshot();
            page->mutable_scope()->set_sector("a");
            page->mutable_scope()->set_spectrum("s");
            page->set_version(1);
            auto* record = page->add_entries();
            record->set_key("key");
            record->set_value("original");
            CHECK(stream->Write(packet));
            if (scene_ == Scenario::interrupted) {
                return {grpc::StatusCode::UNAVAILABLE, "Owned test interruption"};
            }
            page->clear_entries();
            page->set_complete(true);
            CHECK(stream->Write(packet));
            CHECK(stream->Read(&packet) && packet.has_acknowledged() && packet.acknowledged().version() == 1);

            // 修改必须在完整基线以后应用; gap 只改变版本, 不依赖网络随机乱序.
            packet.Clear();
            auto* updates = packet.mutable_updates();
            updates->mutable_scope()->set_sector("a");
            updates->mutable_scope()->set_spectrum("s");
            auto* patch = updates->add_patches();
            patch->set_version(scene_ == Scenario::gap ? 3 : 2);
            patch->mutable_change()->set_key("key");
            patch->mutable_change()->set_value("updated");
            CHECK(stream->Write(packet));
            if (scene_ == Scenario::gap) {
                while (stream->Read(&packet)) {}
                return grpc::Status::OK;
            }
            packet.Clear();
            packet.mutable_probe();
            CHECK(stream->Write(packet));
            // ACK 与 Probe 清单可以交错, 只断言真实版本及完整范围, 不假设发送调度先后.
            bool acknowledged{}, complete{};
            std::size_t positions{};
            while (!acknowledged || !complete) {
                CHECK(stream->Read(&packet) && packet.ByteSizeLong() <= 1024);
                if (packet.has_acknowledged()) {
                    CHECK(packet.acknowledged().version() == 2);
                    acknowledged = true;
                } else {
                    CHECK(packet.has_inventory());
                    for (const auto& position : packet.inventory().positions()) {
                        if (position.scope().sector() == "a") {
                            CHECK(position.version() == 2);
                        }
                        ++positions;
                    }
                    complete = packet.inventory().complete();
                }
            }
            CHECK(positions == static_cast<std::size_t>(inventory.positions_size()) + 1);
            verified = true;
            packet.Clear();
            packet.mutable_ready();
            CHECK(stream->Write(packet));
            while (stream->Read(&packet)) {} // 保持长流直到拥有者取消, 不用 EOF 代替 ready.
            return grpc::Status::OK;
        } catch (...) {
            failed = true;
            return {grpc::StatusCode::INTERNAL, "Owned test assertion failed"};
        }
    }

private:
    // 本例固定协议行为, 不被并发 handler 修改.
    const Scenario scene_;
};

// 每例拥有动态监听端口、客户端 reactor 和原生 Library, 析构严格排空真实 I/O.
struct Fixture {
    // 数据在 Intake 之前构造, 服务/客户端完成后最后销毁.
    Library library;
    // 此 Library 唯一配套登录索引, 由 Receiver 在内部凭据提交时更新.
    Access access;
    // 独立测试处理器, 比其注册的 gRPC Server 活得更久.
    Authority authority;
    // 实际动态端口监听器, 不依赖部署 Pulsar/Polaris.
    std::unique_ptr<grpc::Server> server;
    // 客户端唯一所有者, OnDone 前绝不销毁.
    std::unique_ptr<Intake> intake;

    // count 为预安装合法空 Scope 数, 用于验证小帧清单分页, 默认零.
    Fixture(Scenario scene, unsigned count = 0) : authority(scene) {

        for (unsigned index = 0; index < count; ++index) {
            auto draft = library.prepare({std::string(128, 's'), std::string(120, 'p') + std::to_string(index)}, 0);
            CHECK(draft && library.reset(std::move(*draft)));
        }
        const auto identity = Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", *Endpoint::parse("127.0.0.1:7443"));
        CHECK(identity);
        grpc::ServerBuilder builder;
        builder.RegisterService(&authority);
        int port{};
        builder.AddListeningPort("127.0.0.1:0", (*identity)->server_credentials(), &port);
        server = builder.BuildAndStart();
        CHECK(server && port > 0);
        try {
            const auto endpoint = *Endpoint::parse("127.0.0.1:" + std::to_string(port));
            const auto target = member(endpoint, Member::Role::polaris);
            auto remote = target;
            if (scene == Scenario::role) {
                remote.role = Member::Role::star;
            } else if (scene == Scenario::replacement) {
                remote = member(endpoint, Member::Role::polaris, 2);
            }
            authority.greeting = hello(remote);
            if (scene == Scenario::signature) {
                authority.greeting.mutable_admission_signature()->front() ^= 1;
            }
            intake = std::make_unique<Intake>(*identity, hello(member(*Endpoint::parse("127.0.0.1:7443"), Member::Role::star)), target, library, access, [] {});
        } catch (...) {
            // 构造未完成不会调用 Fixture 析构, 此时必须主动排空已开始的测试监听.
            server->Shutdown();
            server->Wait();
            throw;
        }
    }

    // 测试失败时仍取消并排空, 清理超时以非零终止, 不制造 reactor 悬空借用.
    ~Fixture() {

        if (intake) {
            intake->cancel();
            const auto deadline = Steady::now() + std::chrono::seconds(7);
            while (!intake->done() && Steady::now() < deadline) {
                intake->pump(Steady::now());
                std::this_thread::sleep_for(Milliseconds(1));
            }
            if (!intake->done()) {
                std::_Exit(3);
            }
            intake.reset();
        }
        if (server) {
            server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
            server->Wait();
        }
    }

    // 最多七秒推进真实回调, 到 ready 或最终关闭后返回; 不使用无限等待掩盖失败.
    void wait() {

        const auto deadline = Steady::now() + std::chrono::seconds(7);
        while (!intake->ready() && !intake->done() && Steady::now() < deadline) {
            intake->pump(Steady::now());
            std::this_thread::sleep_for(Milliseconds(1));
        }
        CHECK(intake->ready() || intake->done());
    }
};
} // namespace

// 通过真实 gRPC/TLS 验证接收与释放边界, 无构建、下载或外部测试服务副作用.
int main() {

    try {
        for (const auto scenario : {Scenario::complete, Scenario::replacement}) {
            Fixture fixture(scenario, 20);
            fixture.wait();
            CHECK(fixture.intake->ready() && fixture.authority.verified && !fixture.authority.failed && fixture.authority.received == 20);
            CHECK(fixture.intake->target().epoch.value == (scenario == Scenario::replacement ? 2 : 1));
            const auto point = fixture.library.find({"a", "s"})->find("key");
            CHECK(point && point->version == 2 && std::string(point->value->begin(), point->value->end()) == "updated");
        }
        for (const auto scenario : {Scenario::role, Scenario::signature, Scenario::interrupted, Scenario::gap}) {
            Fixture fixture(scenario);
            fixture.wait();
            CHECK(!fixture.intake->ready() && fixture.intake->done());
            const auto book = fixture.library.find({"a", "s"});
            CHECK(scenario == Scenario::gap ? book && book->usage().version == 1 : !book);
        }
        {
            Fixture fixture(Scenario::complete);
            fixture.intake->cancel(); // 尚未 StartCall 的已绑定 reactor 也必须正常结束.
            fixture.wait();
            CHECK(fixture.intake->done() && !fixture.intake->ready());
        }
        std::cout << "PASS TLS Almanac intake, negotiated inventory, replacement, partial reset and cancellation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
