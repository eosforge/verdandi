#include "check.hpp"
#include "identity.hpp"
#include "readout.hpp"
#include <condition_variable>
#include <grpcpp/create_channel.h>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <memory>
#include <thread>

namespace {
using astra::Almanac;
using astra::Readout;

// 每个夹具独占回环 TLS 端口, 控制线程可由测试暂停以确定性制造跨页并发提交.
class Fixture {
public:
    std::mutex control;                                    // 只保护 pump 调度, Library 提交仍可在控制暂停时独立通知.
    std::mutex sleep;                                      // 条件变量专用锁, 不在等待期间占 control.
    std::condition_variable_any changed;                   // 网络回调与新提交只唤醒一个共享控制线程.
    astra::Library library;                                // 原生有界路由, 回调借用 Readout, 后者销毁后不再写入.
    astra::Access access;                                  // 登录核心活过所有 Watch, 不需要假权限索引.
    astra::Gateway gateway;                                // 使用固定实际测试实例, 可选择匿名但始终禁止 __.
    Readout readout;                                       // 被测真实 CallbackService, 没有每订阅线程.
    std::unique_ptr<grpc::Server> server;                  // 仅属于当前夹具, 不连接用户部署端口.
    std::shared_ptr<grpc::Channel> channel;                // 使用公开测试 CA 校验服务端证书.
    std::unique_ptr<proto::comet::v1::Almanac::Stub> stub; // 每条 Watch 有独立 RPC 上下文.
    std::jthread worker;                                   // 一个服务调度线程, 先排空网络再请求停止和 join.

    // history=0 验证活动快照后缀不依赖历史保留, limits 允许单独覆盖慢流与容量边界.
    explicit Fixture(bool auth = false, std::size_t history = 1000, Readout::Limits limits = {}) : library([history] { astra::Library::Limits value; value.scope.history = history; return value; }(), [this](const astra::Scope& scope, const Almanac::Change& change) noexcept { readout.changed(scope, change); }), gateway(access, "star-test", auth), readout(library, gateway, [this] { changed.notify_all(); }, limits) {

        const auto identity = astra::Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", *astra::Endpoint::parse("127.0.0.1:7443"));
        CHECK(identity);
        grpc::ServerBuilder builder; // 公共端口只注册 Gateway 和 Almanac, 没有内部服务.
        builder.RegisterService(&gateway);
        builder.RegisterService(&readout);
        builder.SetMaxSendMessageSize(8 * 1024 * 1024);
        builder.SetMaxReceiveMessageSize(8 * 1024 * 1024);
        int port{}; // 内核分配, 不共享固定测试端口.
        builder.AddListeningPort("127.0.0.1:0", (*identity)->server_credentials(), &port);
        server = builder.BuildAndStart();
        CHECK(server && port > 0);
        try {
            channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), (*identity)->channel_credentials());
            stub = proto::comet::v1::Almanac::NewStub(channel);
            gateway.ready();
            worker = std::jthread([this](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    {
                        const std::lock_guard lock(control);
                        readout.pump(std::chrono::steady_clock::now());
                    }
                    std::unique_lock lock(sleep);
                    changed.wait_for(lock, stop, std::chrono::milliseconds(2), [] { return false; });
                }
            });
        } catch (...) {
            server->Shutdown();
            server->Wait();
            throw;
        }
    }

    // 全部客户端由局部 RAII 先清理, 例外路径仍取消 Server, 让共享控制循环完成 OnDone 回收.
    ~Fixture() {
        gateway.stop();
        readout.stop();
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(3));
        server->Wait();
        worker.request_stop();
        changed.notify_all();
        worker.join();
        readout.pump(std::chrono::steady_clock::now(), 65536);
    }

    // 构造确定的完整权威基线, count 条小值使分页位置主要由条数控制.
    void fill(std::uint64_t version = 1, unsigned count = 1) {
        auto draft = library.prepare({"routes", "main"}, version);
        CHECK(draft);
        for (unsigned index = 0; index < count; ++index) {
            CHECK(draft->set("key-" + std::to_string(index), {static_cast<std::uint8_t>(index % 251)}));
        }
        CHECK(library.reset(std::move(*draft)));
    }
};

// 独占客户端调用, 半份快照或断言失败也会取消并 Finish, 避免留下服务端资源.
class Watching {
public:
    grpc::ClientContext context;                                                     // 地址稳定且与此 RPC 同寿命, 不跨流复用.
    std::unique_ptr<grpc::ClientReader<proto::comet::v1::AlmanacWatchReply>> stream; // 析构前唯一 Finish.

    // version 带 presence; instance 默认同实例, 只有传 version 时才写入请求.
    Watching(Fixture& fixture, std::string target = {}, std::optional<std::uint64_t> version = {}, std::string instance = "star-test", std::string sector = "routes", std::string token = {}) {

        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
        if (!token.empty()) {
            context.AddMetadata("comet-session-bin", std::move(token));
        }
        proto::comet::v1::WatchRequest request; // Stub 发起后不借用这个局部对象.
        request.mutable_scope()->set_sector(std::move(sector));
        request.mutable_scope()->set_spectrum("main");
        request.set_target(std::move(target));
        if (version) {
            request.set_version(*version);
            request.set_instance(std::move(instance));
        }
        stream = fixture.stub->Watch(&context, request);
    }

    // 只取消本 RPC, 不关闭共用 Channel 或其他客户端订阅.
    ~Watching() {
        if (stream) {
            context.TryCancel();
            static_cast<void>(stream->Finish());
        }
    }

    // 获取单页, 不把 EOF 当作合法空 complete.
    proto::comet::v1::AlmanacWatchReply page() {
        proto::comet::v1::AlmanacWatchReply reply;
        CHECK(stream->Read(&reply));
        CHECK(reply.complete() == reply.has_version());
        CHECK(reply.complete() == !reply.instance().empty());
        return reply;
    }

    // 读取一批完成帧, 只供小结果断言; 首页可由调用者已读取, 本函数继续收尾.
    proto::comet::v1::AlmanacWatchReply complete() {
        for (unsigned page = 0; page < 1000; ++page) {
            auto reply = this->page();
            if (reply.complete()) {
                return reply;
            }
        }
        throw std::runtime_error("Watch did not finish a bounded batch");
    }

    // 用于预期拒绝/取消, 必须收到 EOF, 随后唯一一次 Finish 获取明确状态.
    grpc::Status rejected() {
        proto::comet::v1::AlmanacWatchReply reply;
        CHECK(!stream->Read(&reply));
        const auto status = stream->Finish();
        stream.reset();
        CHECK(!status.ok());
        return status;
    }
};

// 有界等待仅同步异步夹具状态, 超时抛错, 不在生产代码引入 polling.
void eventually(auto&& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// 空范围和精确目标缺失均有完整零/当前版本, 不能靠内容为空推断未就绪.
void baselines() {

    Fixture fixture;
    {
        Watching absent(fixture);
        const auto page = absent.complete();
        CHECK(page.version() == 0 && page.changes().empty() && page.mode() == proto::comet::v1::MODE_RESET);
    }
    fixture.fill();
    Watching exact(fixture, "missing");
    CHECK(exact.complete().changes().empty());
    CHECK(fixture.library.apply({"routes", "main"}, 2, "other", Almanac::Buffer{}));
    const auto unrelated = exact.complete();
    CHECK(unrelated.mode() == proto::comet::v1::MODE_APPLY && unrelated.version() == 2 && unrelated.changes().empty());
    CHECK(fixture.library.apply({"routes", "main"}, 3, "missing", Almanac::Buffer{}));
    const auto created = exact.complete();
    CHECK(created.version() == 3 && created.changes_size() == 1 && created.changes(0).action_case() == proto::comet::v1::AlmanacChange::kValue && created.changes(0).value().empty());
    CHECK(fixture.library.apply({"routes", "main"}, 4, "missing", std::nullopt));
    const auto erased = exact.complete();
    CHECK(erased.version() == 4 && erased.changes_size() == 1 && erased.changes(0).action_case() == proto::comet::v1::AlmanacChange::kErase);
}

// 精确目标只向命中桶收集正文; 无关订阅仍获得空 apply, 不因减少扫描而失去进度.
void fanout() {

    Fixture fixture;
    fixture.fill();
    std::vector<std::unique_ptr<Watching>> watches; // 八条无关精确目标加一条命中目标, 同范围共享一次 changed 遍历.
    for (unsigned index = 0; index < 8; ++index)
        watches.push_back(std::make_unique<Watching>(fixture, "probe-" + std::to_string(index)));
    auto hit = std::make_unique<Watching>(fixture, "wanted");
    for (const auto& watch : watches)
        CHECK(watch->complete().changes().empty());
    CHECK(hit->complete().changes().empty());
    const auto before = fixture.readout.delivery();
    CHECK(fixture.library.apply({"routes", "main"}, 2, "wanted", Almanac::Buffer{1}));
    const auto after = fixture.readout.delivery();                                 // 提交通知同步到达收集器, 快照无需等待网络 pump.
    CHECK(after.scans == before.scans + 1 && after.matched == before.matched + 1); // 精确索引只访问命中桶的一条流; 八条无关订阅由 pump 心跳推进, 不占提交锁.
    const auto matched = hit->complete();                                          // 不能只用计数证明成功, 必须收到真实业务页.
    CHECK(matched.mode() == proto::comet::v1::MODE_APPLY && matched.version() == 2 && matched.changes_size() == 1 && matched.changes(0).key() == "wanted");
    for (const auto& watch : watches) {
        const auto empty = watch->complete();
        CHECK(empty.mode() == proto::comet::v1::MODE_APPLY && empty.version() == 2 && empty.changes().empty());
    }
}

// 恢复只在同实例且历史完整时 apply; Almanac 的最低权威版本跨实例仍不可回退.
void recovery() {

    Fixture fixture;
    fixture.fill();
    CHECK(fixture.library.apply({"routes", "main"}, 2, "key-0", Almanac::Buffer{7}));
    {
        Watching resumed(fixture, {}, 1);
        const auto page = resumed.complete();
        CHECK(page.mode() == proto::comet::v1::MODE_APPLY && page.version() == 2 && page.changes_size() == 1);
    }
    {
        Watching moved(fixture, {}, 1, "another-star");
        CHECK(moved.complete().mode() == proto::comet::v1::MODE_RESET);
    }
    {
        Watching caught(fixture, {}, 2);
        const auto page = caught.complete();
        CHECK(page.mode() == proto::comet::v1::MODE_APPLY && page.changes().empty());
    }
    {
        Watching ahead(fixture, {}, 3, "another-star");
        CHECK(ahead.rejected().error_code() == grpc::StatusCode::UNAVAILABLE);
    }
    Fixture forgotten(false, 0);
    forgotten.fill();
    CHECK(forgotten.library.apply({"routes", "main"}, 2, "key-0", Almanac::Buffer{8}));
    Watching reset(forgotten, {}, 1);
    CHECK(reset.complete().mode() == proto::comet::v1::MODE_RESET);
}

// 第一页发出后暂停唯一 pump, 清空普通历史并反复覆盖同 Key, 后缀仍必须连续且只发最终 action.
void suffix() {

    Fixture fixture(false, 0);
    fixture.fill(1, 600);
    std::unique_lock pause(fixture.control); // 只冻结服务器发送推进, gRPC 回调继续报告 Write 完成.
    Watching watch(fixture);
    eventually([&] { return !fixture.readout.empty(); });
    fixture.readout.pump(std::chrono::steady_clock::now(), 1);
    const auto first = watch.page();
    CHECK(!first.complete() && first.mode() == proto::comet::v1::MODE_RESET);
    for (std::uint64_t version = 2; version <= 101; ++version) {
        CHECK(fixture.library.apply({"routes", "main"}, version, "key-0", Almanac::Buffer{static_cast<std::uint8_t>(version)}));
    }
    CHECK(fixture.library.apply({"routes", "main"}, 102, "key-1", std::nullopt));
    CHECK(fixture.library.apply({"routes", "main"}, 103, "added", Almanac::Buffer{}));
    pause.unlock();
    CHECK(watch.complete().version() == 1);
    const auto delta = watch.complete();
    CHECK(delta.mode() == proto::comet::v1::MODE_APPLY && delta.version() == 103 && delta.changes_size() == 3);
    std::map<std::string, const proto::comet::v1::AlmanacChange*> changes; // 仅在 delta 生命周期内借用.
    for (const auto& change : delta.changes()) {
        CHECK(changes.emplace(change.key(), &change).second);
    }
    CHECK(changes.at("key-0")->value() == std::string(1, static_cast<char>(101)));
    CHECK(changes.at("key-1")->action_case() == proto::comet::v1::AlmanacChange::kErase);
    CHECK(changes.at("added")->action_case() == proto::comet::v1::AlmanacChange::kValue && changes.at("added")->value().empty());
}

// 不同插入位置及同键替换共用一次查找, 保留最终动作与精确预算, 不误覆盖相邻键.
void coalescing() {

    Fixture fixture(false, 0); // 关闭历史, 只能通过活动流待发后缀保存变化.
    fixture.fill();
    Watching watch(fixture);
    CHECK(watch.complete().version() == 1);
    {
        const std::lock_guard pause(fixture.control); // 同批构造首部/中间/末尾插入和多次替换.
        std::uint64_t version = 1;                    // 每个合法写入推进一次权威版本.
        for (const auto key : {"b", "d", "a", "c", "e"}) {
            CHECK(fixture.library.apply({"routes", "main"}, ++version, key, Almanac::Buffer(10, 1)));
        }
        CHECK(fixture.library.apply({"routes", "main"}, ++version, "c", Almanac::Buffer(20, 2)));
        CHECK(fixture.library.apply({"routes", "main"}, ++version, "b", std::nullopt));
        CHECK(fixture.library.apply({"routes", "main"}, ++version, "a", Almanac::Buffer{}));
    }

    const auto delta = watch.complete(); // 五个键各保留最终动作, 删除与空正文不能混淆.
    CHECK(delta.mode() == proto::comet::v1::MODE_APPLY && delta.version() == 9 && delta.changes_size() == 5);
    CHECK(delta.changes(0).key() == "a" && delta.changes(0).has_value() && delta.changes(0).value().empty());
    CHECK(delta.changes(1).key() == "b" && delta.changes(1).has_erase());
    CHECK(delta.changes(2).key() == "c" && delta.changes(2).value() == std::string(20, '\2'));
    CHECK(delta.changes(3).key() == "d" && delta.changes(3).value() == std::string(10, '\1'));
    CHECK(delta.changes(4).key() == "e" && delta.changes(4).value() == std::string(10, '\1'));
}

// 新完整替换不能偷偷混入旧流 apply, 字节超额也不能继续发送假 complete.
void discontinuity() {

    Fixture fixture;
    fixture.fill();
    Watching watch(fixture);
    CHECK(watch.complete().version() == 1);
    fixture.fill(10);
    CHECK(watch.rejected().error_code() == grpc::StatusCode::OUT_OF_RANGE);

    {
        Fixture broken; // 模拟上游通知漏掉一个版本, 不能因精确目标不匹配而掩盖断档.
        broken.fill();
        Watching exact(broken, "missing");
        CHECK(exact.complete().version() == 1);
        {
            const std::lock_guard pause(broken.control);                       // 两个通知收集完毕再运行发送, 不依赖线程时序.
            const auto key = std::make_shared<const std::string>("unrelated"); // 与订阅无关, 仍必须检查范围连续性.
            broken.readout.changed({"routes", "main"}, {.key = key, .value = {}, .version = 2});
            broken.readout.changed({"routes", "main"}, {.key = key, .value = {}, .version = 4});
        }
        CHECK(exact.rejected().error_code() == grpc::StatusCode::OUT_OF_RANGE);
    }

    Readout::Limits limits; // 较小的合并预算只用于确定性触发资源错误.
    limits.pending = 4096;
    Fixture limited(false, 0, limits);
    limited.fill();
    Watching slow(limited);
    CHECK(slow.complete().version() == 1);
    CHECK(limited.library.apply({"routes", "main"}, 2, "large", Almanac::Buffer(4096, 1)));
    CHECK(slow.rejected().error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED);
}

// 匿名不等于能读内部数据, 非法恢复位置和超长目标在准备状态前拒绝.
void boundaries() {

    Fixture fixture;
    {
        Watching internal(fixture, {}, {}, {}, "__auth");
        CHECK(internal.rejected().error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    }
    {
        Watching invalid(fixture, {}, 0, "");
        CHECK(invalid.rejected().error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    }
    {
        Watching large(fixture, std::string(1025, 'k'));
        CHECK(large.rejected().error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    }
    Readout::Limits limits;
    limits.streams = 1;
    Fixture one(false, 1000, limits);
    {
        Watching first(one);
        CHECK(first.complete().version() == 0);
        Watching second(one);
        CHECK(second.rejected().error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED);
    }
    eventually([&] { return one.readout.empty(); });
    for (unsigned turn = 0; turn < 40; ++turn) {
        {
            Watching next(one);
            CHECK(next.complete().version() == 0);
        }
        eventually([&] { return one.readout.empty(); });
    }
}

// 空闲 Watch 也立即跟随 Session 撤销, 不依赖下一次业务写入或周期扫描发现失效.
void revocation() {

    Fixture fixture(true);
    astra::Access::Draft draft;
    CHECK(draft.set("application", {1, 2, 3}));
    CHECK(fixture.access.reset(std::move(draft), [] { return std::expected<bool, Almanac::Error>(true); }));
    const Almanac::Buffer secret{1, 2, 3};
    auto session = fixture.access.open("application", secret);
    CHECK(session);
    Watching watch(fixture, {}, {}, {}, "routes", std::string((*session)->token()));
    CHECK(watch.complete().version() == 0);
    fixture.access.close(*session);
    static_cast<void>(watch.rejected());
    eventually([&] { return fixture.readout.empty(); });
}
} // namespace

// 测试错误只打印断言, 不打印真实凭据或部署材料.
int main() {
    try {
        baselines();
        fanout();
        recovery();
        suffix();
        coalescing();
        discontinuity();
        boundaries();
        revocation();
        std::cout << "Almanac readout tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
