#include "catalog_edition.hpp"
#include "check.hpp"
#include "ephemeris_edition.hpp"
#include "ephemeris_service.hpp"
#include "identity.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <grpcpp/create_channel.h>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <map>
#include <thread>

namespace {
using namespace std::chrono_literals;
using astra::Clock;
using astra::Ephemeris;

// 真实 TLS/gRPC、原生来源/投影/轮, 直接为测试建立已验证 Access 身份; 长登录流另由 Gateway 用例覆盖.
class Fixture {
public:
    std::mutex control;                                      // 用例可暂停唯一 pump, 原生提交通知仍独立到达收集器.
    std::mutex sleep;                                        // 控制线程短等待专用, 不占业务锁.
    std::condition_variable_any changed;                     // 停止时立即唤醒, 不依赖硬休眠.
    std::atomic<std::int64_t> now{1'000'000'000};            // 确定性 Unix 纳秒, RPC 工作线程只读.
    std::atomic_bool ready{true};                            // 首次本地计时资格, synchronized 固定 false.
    astra::Access access;                                    // 实际准入/撤销索引, 不是假布尔鉴权.
    astra::Gateway gateway{access, "star-test"};             // 固定当前实例.
    Ephemeris::State state;                                  // 真实提交器, 寿命覆盖 Service.
    Ephemeris::Service service{state, gateway};              // 被测生产 unary 服务.
    std::shared_ptr<astra::Access::Session> session;         // 本例已验证逻辑身份, 析构撤销.
    std::unique_ptr<grpc::Server> server;                    // 独立回环监听, 本例负责排空.
    std::shared_ptr<grpc::Channel> channel;                  // 复用实际 TLS 通道.
    std::unique_ptr<proto::comet::v1::Ephemeris::Stub> stub; // 当前真实生成 Stub.
    std::jthread worker;                                     // 每个服务一个测试驱动线程, 全部 Watch 共享, 并非每连接线程.

    // 只注入时间, 所有授权/状态/顺序使用生产实现, 不需要休眠等待到期.
    explicit Fixture(bool history = true) : state([this] { return std::optional(Clock::Reading{.time = Clock::Time(std::chrono::nanoseconds(now.load())), .ready = ready.load(), .synchronized = false}); }, [history] { Ephemeris::State::Limits limits; if (!history) { limits.history = 0; limits.source.history = 0; } return limits; }()) {

        astra::Access::Draft accounts;
        CHECK(accounts.set("application", {1, 2, 3}));
        CHECK(access.reset(std::move(accounts), [] { return std::expected<bool, astra::Almanac::Error>(true); }));
        const std::array<std::uint8_t, 3> secret{1, 2, 3}; // 公开测试字节, 不读取部署凭据.
        auto opened = access.open("application", secret);
        CHECK(opened);
        session = std::move(*opened);
        const auto identity = astra::Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", *astra::Endpoint::parse("127.0.0.1:7443"));
        CHECK(identity);
        grpc::ServerBuilder builder;
        builder.SetMaxReceiveMessageSize(4 * 1024 * 1024);
        builder.RegisterService(&gateway);
        builder.RegisterService(&service);
        int port{}; // 系统分配独立端口, 不影响已有服务.
        builder.AddListeningPort("127.0.0.1:0", (*identity)->server_credentials(), &port);
        server = builder.BuildAndStart();
        CHECK(server && port > 0);
        try {
            channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), (*identity)->channel_credentials());
            stub = proto::comet::v1::Ephemeris::NewStub(channel);
            gateway.ready();
            worker = std::jthread([this](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    {
                        const std::lock_guard lock(control);
                        service.pump(std::chrono::steady_clock::now());
                    }
                    std::unique_lock lock(sleep);
                    changed.wait_for(lock, stop, 2ms, [] { return false; });
                }
            });
        } catch (...) {
            server->Shutdown();
            server->Wait();
            throw;
        }
    }

    // 撤销本例身份, 排空本例监听, 不访问外部部署.
    ~Fixture() {
        gateway.stop();
        service.stop();
        access.close(session);
        server->Shutdown(std::chrono::system_clock::now() + 3s);
        server->Wait();
        worker.request_stop();
        changed.notify_all();
        worker.join();
        service.pump(std::chrono::steady_clock::now(), 65536);
    }

    // 每 RPC 都有独立期限和 metadata, token=false 专门验证匿名拒绝.
    void prepare(grpc::ClientContext& context, bool token = true) const {
        context.set_deadline(std::chrono::system_clock::now() + 3s);
        if (token) {
            context.AddMetadata("comet-session-bin", std::string(session->token()));
        }
    }

    // 首次创建允许空 instance, 完整 Attr/Data 均来自公开测试文本.
    static proto::comet::v1::CreateRequest create(std::uint32_t ttl = 1000) {
        proto::comet::v1::CreateRequest request;
        request.mutable_scope()->set_sector("service");
        request.mutable_scope()->set_spectrum("main");
        request.set_attr("fixed");
        request.set_data("initial");
        request.set_ttl_ms(ttl);
        return request;
    }

    // 实际 metadata 必须保留稳定 reason/效果/实例, 不以错误文本决定恢复.
    static proto::comet::v1::Failure failure(const grpc::ClientContext& context, proto::comet::v1::Reason reason) {
        const auto found = context.GetServerTrailingMetadata().find("comet-error-bin");
        CHECK(found != context.GetServerTrailingMetadata().end());
        proto::comet::v1::Failure failure;
        CHECK(failure.ParseFromArray(found->second.data(), static_cast<int>(found->second.size())));
        CHECK(failure.reason() == reason && failure.effect() == proto::comet::v1::EFFECT_UNAPPLIED && failure.instance() == "star-test");
        return failure;
    }
};

// 独占一次真实 Watch, 异常退出同样 TryCancel/Finish, 不遗留 fixture 上的活动流.
class Watching {
public:
    grpc::ClientContext context;                                                       // 必须覆盖整个流, 不允许移动地址.
    std::unique_ptr<grpc::ClientReader<proto::comet::v1::EphemerisWatchReply>> stream; // 最终只 Finish 一次.

    // 游标只有同 instance 时恢复, target 空为全范围, 非空为规范 UUID.
    Watching(Fixture& fixture, std::string target = {}, std::optional<std::uint64_t> version = {}, std::string instance = "star-test") {
        context.set_deadline(std::chrono::system_clock::now() + 10s);
        context.AddMetadata("comet-session-bin", std::string(fixture.session->token()));
        proto::comet::v1::WatchRequest request;
        request.mutable_scope()->set_sector("service");
        request.mutable_scope()->set_spectrum("main");
        request.set_target(std::move(target));
        request.set_instance(std::move(instance));
        if (version) {
            request.set_version(*version);
        }
        stream = fixture.stub->Watch(&context, request);
    }

    // 先结束此流, Fixture 的共享 worker 继续排空服务端 OnDone.
    ~Watching() {
        context.TryCancel();
        static_cast<void>(stream->Finish());
    }

    // 本用例的单条内容在一页内完成, 不将未 complete 的半份批次当作成功视图.
    proto::comet::v1::EphemerisWatchReply next() {
        proto::comet::v1::EphemerisWatchReply reply;
        CHECK(stream->Read(&reply) && reply.complete() && reply.has_version() && reply.instance() == "star-test");
        return reply;
    }
};

// 零历史活动流也保留完整后缀; Create+Data 合并不能省略 Attr, 已有基线后才发 Data-only.
void streaming() {

    Fixture fixture(false);
    Watching watch(fixture);
    const auto empty = watch.next();
    CHECK(empty.mode() == proto::comet::v1::MODE_RESET && empty.changes_size() == 0 && empty.version() == 0);
    const astra::Scope scope{"service", "main"};
    const auto attr = std::make_shared<const Ephemeris::Buffer>(8, 1);
    const auto first = std::make_shared<const Ephemeris::Buffer>(8, 2);
    const auto last = std::make_shared<const Ephemeris::Buffer>(8, 3);
    std::string uuid;
    {
        const std::lock_guard pause(fixture.control); // 明确把 Create+Update 合入同一未发送批次.
        auto created = fixture.state.create(scope, attr, first, 1000);
        CHECK(created);
        uuid = created->uuid;
        CHECK(fixture.state.update(scope, uuid, last, 1));
        CHECK(!fixture.state.changes(scope, 0)); // 普通历史确实关闭, 推流不能靠事后 replay 补洞.
    }
    const auto introduced = watch.next();
    CHECK(introduced.mode() == proto::comet::v1::MODE_APPLY && introduced.version() == 2 && introduced.changes_size() == 1);
    CHECK(introduced.changes(0).uuid() == uuid && introduced.changes(0).has_record());
    CHECK(introduced.changes(0).record().attr() == std::string(8, 1) && introduced.changes(0).record().data() == std::string(8, 3));

    CHECK(fixture.state.renew(scope, uuid, 1));
    CHECK(fixture.state.update(scope, uuid, first, 2));
    const auto updated = watch.next();
    CHECK(updated.version() == 3 && updated.changes_size() == 1 && updated.changes(0).has_data());
    CHECK(updated.changes(0).data() == std::string(8, 2)); // Attr 不在此帧重复发送, Renew 也没有制造空内容通知.
    Watching switched(fixture, uuid, 1000, "old-star");
    const auto reset = switched.next();
    CHECK(reset.mode() == proto::comet::v1::MODE_RESET && reset.version() == 3 && reset.changes(0).has_record());
    CHECK(fixture.state.remove(scope, uuid));
    const auto erased = watch.next();
    CHECK(erased.version() == 4 && erased.changes_size() == 1 && erased.changes(0).has_erase());
    const auto precise = switched.next();
    CHECK(precise.version() == 4 && precise.changes(0).has_erase());
}

// 可保留历史时同实例恢复得到 apply, 跨实例重置则始终交付完整 Record.
void resume() {

    Fixture fixture;
    const astra::Scope scope{"service", "main"};
    const auto attr = std::make_shared<const Ephemeris::Buffer>(4, 1);
    const auto data = std::make_shared<const Ephemeris::Buffer>(4, 2);
    const auto created = fixture.state.create(scope, attr, data, 1000);
    CHECK(created && fixture.state.update(scope, created->uuid, std::make_shared<const Ephemeris::Buffer>(4, 3), 1));
    Watching all(fixture, {}, 0);
    const auto combined = all.next();
    CHECK(combined.mode() == proto::comet::v1::MODE_APPLY && combined.version() == 2 && combined.changes(0).has_record());
    Watching known(fixture, created->uuid, 1);
    const auto incremental = known.next();
    CHECK(incremental.mode() == proto::comet::v1::MODE_APPLY && incremental.version() == 2 && incremental.changes(0).has_data());
    fixture.now = 2'000'000'000; // worker 共享推进期限, 不要求客户端主动发另一个请求触发清理.
    CHECK(known.next().changes(0).has_erase());
}

// 实际创建/更新/续租/重复确认/注销, 检查来源位置与下游内容游标没有重新混成一个版本.
void lifecycle() {

    Fixture fixture;
    auto request = Fixture::create();
    proto::comet::v1::CreateReply created;
    grpc::ClientContext creating;
    fixture.prepare(creating);
    CHECK(fixture.stub->Create(&creating, request, &created).ok());
    CHECK(created.instance() == "star-test" && Ephemeris::valid(created.uuid()) && created.ttl_ms() == 1000);
    CHECK(fixture.state.source().position() == 1);

    proto::comet::v1::UpdateRequest update;
    update.set_instance(created.instance());
    *update.mutable_scope() = request.scope();
    update.set_uuid(created.uuid());
    update.set_order(2);
    update.set_data("latest");
    proto::comet::v1::UpdateReply updated;
    grpc::ClientContext updating;
    fixture.prepare(updating);
    CHECK(fixture.stub->Update(&updating, update, &updated).ok() && updated.order() == 2);
    CHECK(fixture.state.capture({"service", "main"})->version() == 2);
    update.set_order(1);
    grpc::ClientContext obsolete;
    fixture.prepare(obsolete);
    CHECK(fixture.stub->Update(&obsolete, update, &updated).error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    Fixture::failure(obsolete, proto::comet::v1::REASON_OBSOLETE);
    CHECK(fixture.state.source().position() == 2);

    fixture.now = 1'500'000'000;
    proto::comet::v1::RenewRequest renew;
    renew.set_instance(created.instance());
    *renew.mutable_scope() = request.scope();
    renew.set_uuid(created.uuid());
    renew.set_order(1);
    proto::comet::v1::RenewReply renewed;
    grpc::ClientContext renewing;
    fixture.prepare(renewing);
    CHECK(fixture.stub->Renew(&renewing, renew, &renewed).ok() && renewed.order() == 1);
    CHECK(fixture.state.source().position() == 3 && fixture.state.capture({"service", "main"})->version() == 2);
    fixture.now = 1'800'000'000;
    grpc::ClientContext repeating;
    fixture.prepare(repeating);
    CHECK(fixture.stub->Renew(&repeating, renew, &renewed).ok() && fixture.state.source().position() == 3);
    fixture.state.source().each([](const astra::Scope&, const std::string&, const Ephemeris::Record& record) { CHECK(record.deadline == Clock::Time(2500ms)); });

    proto::comet::v1::RemoveRequest remove;
    remove.set_instance(created.instance());
    *remove.mutable_scope() = request.scope();
    remove.set_uuid(created.uuid());
    proto::comet::v1::Empty empty;
    grpc::ClientContext removing;
    fixture.prepare(removing);
    CHECK(fixture.stub->Remove(&removing, remove, &empty).ok());
    CHECK(fixture.state.source().position() == 4 && fixture.state.capture({"service", "main"})->size() == 0);
    grpc::ClientContext ended;
    fixture.prepare(ended);
    CHECK(fixture.stub->Renew(&ended, renew, &renewed).error_code() == grpc::StatusCode::NOT_FOUND);
    Fixture::failure(ended, proto::comet::v1::REASON_ENDED);
}

// 匿名、其他实例、内部范围、TTL 和计时拒绝不产生隐藏注册, 撤销会话不能再次写入.
void rejection() {

    Fixture fixture;
    auto request = Fixture::create();
    proto::comet::v1::CreateReply reply;
    grpc::ClientContext anonymous;
    fixture.prepare(anonymous, false);
    CHECK(fixture.stub->Create(&anonymous, request, &reply).error_code() == grpc::StatusCode::UNAUTHENTICATED);
    Fixture::failure(anonymous, proto::comet::v1::REASON_SESSION);
    request.set_instance("another-star");
    grpc::ClientContext other;
    fixture.prepare(other);
    CHECK(fixture.stub->Create(&other, request, &reply).error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    Fixture::failure(other, proto::comet::v1::REASON_INSTANCE);
    request.clear_instance();
    request.mutable_scope()->set_sector("__internal");
    grpc::ClientContext internal;
    fixture.prepare(internal);
    CHECK(fixture.stub->Create(&internal, request, &reply).error_code() == grpc::StatusCode::PERMISSION_DENIED);
    Fixture::failure(internal, proto::comet::v1::REASON_DENIED);
    request = Fixture::create(999);
    grpc::ClientContext short_ttl;
    fixture.prepare(short_ttl);
    CHECK(fixture.stub->Create(&short_ttl, request, &reply).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    const auto limits = Fixture::failure(short_ttl, proto::comet::v1::REASON_INPUT);
    CHECK(limits.ttl_min_ms() == 1000 && limits.ttl_max_ms() == 600000);
    request = Fixture::create();
    fixture.ready = false;
    grpc::ClientContext unready;
    fixture.prepare(unready);
    CHECK(fixture.stub->Create(&unready, request, &reply).error_code() == grpc::StatusCode::UNAVAILABLE);
    Fixture::failure(unready, proto::comet::v1::REASON_CLOCK);
    fixture.ready = true;
    CHECK(fixture.state.source().position() == 0 && fixture.state.source().size() == 0);
    fixture.access.close(fixture.session);
    grpc::ClientContext revoked;
    fixture.prepare(revoked);
    CHECK(fixture.stub->Create(&revoked, request, &reply).error_code() == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(fixture.state.source().position() == 0);
}

// 使用真实原生历史检查增量压缩: 唯一项不得自移动丢失, 多组压缩保持最后内容及 Attr 基线.
void editions() {

    const astra::Scope scope{"edition", "main"};                                                                          // 两个域独立持有同名范围, 不共享存储或游标.
    const auto time = [] { return std::optional(astra::Clock::Reading{.time = astra::Clock::Time(1s), .ready = true}); }; // 固定可用纪元, 本用例不推进 TTL.
    const auto buffer = [](std::uint8_t value) { return std::make_shared<const Ephemeris::Buffer>(1, value); };           // 每个正文为一个独立不可变字节.

    {
        astra::Catalog::State state(time, {});
        CHECK(state.publish(scope, "a", buffer(1), 1, 1000));
        CHECK(state.publish(scope, "b", buffer(2), 1, 1000));
        CHECK(state.publish(scope, "c", buffer(3), 1, 1000));
        auto initial = state.changes(scope, 0); // 三组各一项, 压缩索引与原索引相等.
        CHECK(initial);
        astra::Catalog::Edition singles(3, std::move(*initial));
        const auto first = singles.next("star-test");
        CHECK(first.complete() && first.changes_size() == 3 && first.changes(0).key() == "a" && first.changes(1).key() == "b" && first.changes(2).key() == "c");

        CHECK(state.publish(scope, "a", buffer(7), 2, 1000));
        CHECK(state.publish(scope, "b", buffer(8), 2, 1000));
        auto history = state.changes(scope, 0); // 两组重复项后跟唯一尾项, 覆盖连续前移压缩.
        CHECK(history);
        astra::Catalog::Edition merged(5, std::move(*history));
        const auto page = merged.next("star-test");
        CHECK(page.complete() && page.version() == 5 && page.changes_size() == 3);
        CHECK(page.changes(0).key() == "a" && page.changes(0).version() == 2 && page.changes(0).value() == std::string(1, '\7'));
        CHECK(page.changes(1).key() == "b" && page.changes(1).version() == 2 && page.changes(1).value() == std::string(1, '\10'));
        CHECK(page.changes(2).key() == "c" && page.changes(2).version() == 1 && page.changes(2).value() == std::string(1, '\3'));
    }

    {
        Ephemeris::State state(time, {});
        const auto one = state.create(scope, buffer(9), buffer(1), 1000);
        const auto two = state.create(scope, buffer(9), buffer(2), 1000);
        const auto three = state.create(scope, buffer(9), buffer(3), 1000);
        CHECK(one && two && three);
        CHECK(state.update(scope, one->uuid, buffer(4), 1));
        CHECK(state.update(scope, one->uuid, buffer(5), 2));
        CHECK(state.remove(scope, two->uuid));
        CHECK(state.update(scope, three->uuid, buffer(6), 1));
        for (const auto since : {0U, 3U}) {
            auto history = state.changes(scope, since); // 零游标须保留 Attr, 已有基线则允许只推 Data.
            CHECK(history);
            Ephemeris::Edition merged(7, std::move(*history));
            const auto page = merged.next("star-test");
            CHECK(page.complete() && page.version() == 7 && page.changes_size() == 3);
            std::map<std::string, std::string> records; // 以 UUID 对照, 不假设随机 UUID 的排序顺序.
            for (const auto& change : page.changes()) {
                if (change.uuid() == two->uuid) {
                    CHECK(change.has_erase());
                } else {
                    CHECK(since == 0 ? change.has_record() : change.has_data());
                    if (since == 0) {
                        CHECK(change.record().attr() == std::string(1, '\11'));
                    }
                    CHECK(records.emplace(change.uuid(), since == 0 ? change.record().data() : change.data()).second);
                }
            }
            CHECK(records.size() == 2 && records.at(one->uuid) == std::string(1, '\5') && records.at(three->uuid) == std::string(1, '\6'));
        }
    }
}
} // namespace

// 有限真实回环测试, 不启动部署进程或长期压力, 资源由 Fixture 明确持有.
int main() {
    try {
        editions();
        lifecycle();
        rejection();
        streaming();
        resume();
        std::cout << "Ephemeris RPC tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
