#include "catalog_service.hpp"
#include "check.hpp"
#include "identity.hpp"
#include <atomic>
#include <comet/client.hpp>
#include <condition_variable>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <limits>
#include <source_location>
#include <thread>

namespace {
using namespace std::chrono_literals;

// 故障发生在真实原生提交之后, Watch 的一次停滞不替换后续生产下行逻辑.
class Faults final : public proto::comet::v1::Catalog::CallbackService {
public:
    std::atomic_uint queries{};      // 实际版本查询次数, 不计 Watch.
    std::atomic_uint conflicts{};    // 剩余注入次数, Publish 前由另一写者抬高同一批版本.
    std::atomic_bool malformed{};    // 下一次查询返回缺项, 必须在提交前拒绝.
    std::atomic_bool lost{};         // 下一次 Publish 已提交但回执丢失.
    std::atomic_int stalled{};       // 0 正常, 1 首帧停滞, 2 半批停滞, 3 半批 EOF, 4 首帧前 EOF.
    std::atomic_uint writes{};       // 实际 Publish 到达次数, 不是业务版本.
    std::atomic_uint watches{};      // 实际 Watch 次数, 用于确认停止旧流后才恢复.
    std::atomic_bool valid{true};    // 注入路径的真实准入/原生提交仍须成功.
    astra::Catalog::State state;     // 固定可用时间, SDK 本地超时依然使用真实单调时间.
    astra::Catalog::Service service; // 正常 RPC 全部交给生产服务.

    // 对外 Gateway 比本夹具活得更久, state 比 service 活得更久.
    explicit Faults(astra::Gateway& gateway) : state([] { return std::optional(astra::Clock::Reading{.time = astra::Clock::Time(1s), .ready = true}); }, {}), service(state, gateway), gateway_(gateway) {}

    // 查询使用真实服务水位, 非法响应只用于验证 SDK 对应关系检查.
    grpc::ServerUnaryReactor* Query(grpc::CallbackServerContext* context, const proto::comet::v1::CatalogQueryRequest* request, proto::comet::v1::CatalogQueryReply* reply) override {
        ++queries;
        if (!malformed.exchange(false))
            return service.Query(context, request, reply);
        reply->set_instance("catalog-fault"); // 故意缺少所有 Key, 不应发送 Publish.
        auto* reactor = context->DefaultReactor();
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }

    grpc::ServerUnaryReactor* Publish(grpc::CallbackServerContext* context, const proto::comet::v1::PublishRequest* request, proto::comet::v1::PublishReply* reply) override {

        ++writes;
        if (conflicts.load() > 0) {
            --conflicts;
            std::vector<astra::Catalog::State::Entry> entries; // 模拟 Query 与 Publish 之间明确发生的版本竞争.
            for (const auto& entry : request->entries())
                entries.push_back({entry.key(), std::make_shared<const astra::Catalog::Buffer>(1, 42)});
            if (!state.publish({request->scope().sector(), request->scope().spectrum()}, entries, request->version() + 10, request->ttl_ms()))
                valid.store(false);
        }
        if (!lost.exchange(false))
            return service.Publish(context, request, reply);
        auto permit = gateway_.enter(*context);                   // 不绕过真实登录/就绪检查.
        std::expected<void, astra::Catalog::State::Error> result; // 故障在生产原生提交之后, 不绕过批次边界.
        if (request->entries().empty()) {
            auto body = std::make_shared<const astra::Catalog::Buffer>(request->value().begin(), request->value().end());
            result = state.publish({request->scope().sector(), request->scope().spectrum()}, request->key(), std::move(body), request->version(), request->ttl_ms());
        } else {
            std::vector<astra::Catalog::State::Entry> entries; // 整批一次提交, 然后丢失完整回执.
            for (const auto& entry : request->entries())
                entries.push_back({entry.key(), std::make_shared<const astra::Catalog::Buffer>(entry.value().begin(), entry.value().end())});
            result = state.publish({request->scope().sector(), request->scope().spectrum()}, entries, request->version(), request->ttl_ms());
        }
        if (!permit || !result)
            valid.store(false);
        auto* reactor = context->DefaultReactor(); // 默认 reactor 归 gRPC 持有, 注入不确定且不附 unapplied.
        reactor->Finish(grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Injected confirmation loss"));
        return reactor;
    }

    // 高频正常 Renew 仍执行实际期限和来源规则.
    grpc::ServerUnaryReactor* Renew(grpc::CallbackServerContext* context, const proto::comet::v1::CatalogRenewRequest* request, proto::comet::v1::Empty* reply) override {
        return service.Renew(context, request, reply);
    }

    grpc::ServerWriteReactor<proto::comet::v1::CatalogWatchReply>* Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request) override {
        ++watches;
        const auto mode = stalled.exchange(0); // 只注入一次, 后续恢复使用完整生产管线.
        return mode == 0 ? service.Watch(context, request) : new Frozen(mode);
    }

private:
    // 被取消时先等唯一在途 Write 完成再 Finish, OnDone 是唯一释放点.
    class Frozen final : public grpc::ServerWriteReactor<proto::comet::v1::CatalogWatchReply> {
    public:
        // mode 取上述四种注入方式; 半批绝不进入完整视图, EOF 必须排空后才重开 Watch.
        explicit Frozen(int mode) : writing_(mode == 2 || mode == 3), cancelled_(mode >= 3) {
            if (writing_) {
                page_.set_mode(proto::comet::v1::MODE_RESET);
                // 实例和完整游标只允许出现在尾页; 此处仅模拟合法半批停滞, 不注入另一种协议错误.
                auto* change = page_.add_changes(); // 页面寿命一直保持到 OnDone.
                change->set_key("partial");
                change->set_value("not-installed");
                change->set_version(1);
                StartWrite(&page_);
            } else {
                finish(); // mode=4 没有在途 Write, 直接产生 EOF; mode=1 保持首帧停滞.
            }
        }

        // 取消和 Write 回调可能并发, 一把短锁保证只 Finish 一次.
        void OnCancel() override {
            const std::lock_guard lock(mutex_);
            cancelled_ = true;
            finish();
        }

        // 失败 Write 也转入终止路径, 不等待下一次应用事件.
        void OnWriteDone(bool ok) override {
            const std::lock_guard lock(mutex_);
            writing_ = false;
            cancelled_ = cancelled_ || !ok;
            finish();
        }

        // 无外部持有者, gRPC 最终完成后删除本对象.
        void OnDone() override {
            delete this;
        }

    private:
        // 已持 mutex_, 没有在途 Write 时才结束, 不阻塞 gRPC 线程.
        void finish() {
            if (cancelled_ && !writing_ && !finished_) {
                finished_ = true;
                Finish(grpc::Status(grpc::StatusCode::CANCELLED, "Fixture stopped"));
            }
        }

        proto::comet::v1::CatalogWatchReply page_; // 只编码一次的半批, complete 缺省 false.
        std::mutex mutex_;                         // 只协调取消与写完成.
        bool writing_{};                           // 唯一 Write 是否仍在途.
        bool cancelled_{};                         // 是否已知必须停止.
        bool finished_{};                          // 确保 Finish 恰好一次.
    };

    astra::Gateway& gateway_; // 固定真实 Gateway, 不创建另一种认证模型.
};

class Fixture {
public:
    astra::Access access; // 生产访问表, 本隔离测试显式免公共登录.
    astra::Gateway gateway{access, "catalog-fault", false};
    Faults faults{gateway}; // 真实 Catalog 提交与可控回执.
    comet::Client client;   // 清理等待其所有 Reactor 结束.

    Fixture() {

        auto credentials = astra::Identity::external(std::filesystem::path(ASTRA_FIXTURES) / "star-a");
        CHECK(credentials);
        grpc::ServerBuilder builder;
        int port{}; // 每夹具独立回环端口, 没有共享部署状态.
        builder.AddListeningPort("127.0.0.1:0", *credentials, &port);
        builder.RegisterService(&gateway);
        builder.RegisterService(&faults);
        server_ = builder.BuildAndStart();
        CHECK(server_ && port > 0);
        try {
            worker_ = std::jthread([this](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    faults.service.pump(std::chrono::steady_clock::now());
                    std::unique_lock lock(mutex_);
                    changed_.wait_for(lock, stop, 2ms, [] { return false; });
                }
            });
            gateway.ready();
            comet::Client::Options options;
            options.auth = false;
            options.endpoints = {"127.0.0.1:" + std::to_string(port)};
            options.ca = std::filesystem::path(ASTRA_FIXTURES) / "star-a" / "ca.pem";
            options.timeout = 500ms;
            auto opened = comet::Client::open(std::move(options));
            CHECK(opened);
            client = std::move(*opened);
        } catch (...) {
            stop();
            throw;
        }
    }

    // 所有成功和失败路径都关闭夹具唯一所有资源.
    ~Fixture() {
        stop();
    }

private:
    // 先关闭 SDK, 服务线程仍能排空 Watch; 最后停止服务器和控制循环.
    void stop() {
        client.close();
        if (!client.wait(5s))
            std::terminate();
        gateway.stop();
        faults.service.stop();
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
        server_->Wait();
        if (worker_.joinable()) {
            worker_.request_stop();
            changed_.notify_all();
            worker_.join();
        }
        faults.service.pump(std::chrono::steady_clock::now(), 65536);
    }

    std::mutex mutex_;                     // 唤醒/停止控制循环的独立锁.
    std::condition_variable_any changed_;  // stop_token 可立即打断等待.
    std::unique_ptr<grpc::Server> server_; // 在回调完全排空后销毁.
    std::jthread worker_;                  // 每夹具一个服务控制线程, 不按 Publisher 分配.
};

// condition 为可观察事实, location 保留调用者位置; 最多等五秒, 不将固定长休眠当作成功依据.
void eventually(auto&& condition, std::source_location location = std::source_location::current()) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition()) {
        astra::test::check(std::chrono::steady_clock::now() < deadline, "Expected asynchronous state was not reached", location);
        std::this_thread::sleep_for(1ms);
    }
}

// 丢失确认仍报告 unknown, 等过旧续租周期也不能后台重发; 新对象通过 Query 恢复版本基线.
void lost() {

    Fixture fixture;
    fixture.faults.lost.store(true);
    auto publisher = fixture.client.publisher({"source", "main"});
    CHECK(publisher);
    const auto result = publisher->update("key", {7}, 1s);
    CHECK(!result && result.error().effect == comet::Error::Effect::unknown);
    std::this_thread::sleep_for(500ms); // 有界负向观察窗口, 专门捕获旧版约 TTL/3 的自动恢复.
    CHECK(fixture.faults.writes.load() == 1 && fixture.faults.queries.load() == 1 && fixture.faults.valid.load());
    CHECK(fixture.faults.state.find({"source", "main"}, "key")->record->version == 1);
    auto restarted = fixture.client.publisher({"source", "main"});
    CHECK(restarted);
    const auto next = restarted->update("key", {8}, 1s);
    CHECK(next && next->version == 2);
}

// 同步批次在分页推流中仍原子安装, 回调保留的旧视图不受新提交覆盖.
void batches() {

    Fixture fixture;
    auto publisher = fixture.client.publisher({"batch", "main"});
    auto subscriber = fixture.client.subscriber({"batch", "main"});
    CHECK(publisher && subscriber);
    eventually([&] { return subscriber->watch().state() == comet::Subscriber::State::ready; });
    const auto initial = subscriber->watch();                                           // 后续批次不得改写旧基线.
    const auto body = std::make_shared<const std::vector<std::uint8_t>>(300 * 1024, 7); // 合计 600 KiB, 强制跨页.
    const auto result = publisher->update({{"a", body}, {"b", body}}, 1s);
    CHECK(result && result->version == 1);
    eventually([&] { const auto view = subscriber->watch(); CHECK(view.size() == 0 || view.size() == 2); return view.size() == 2; });
    CHECK(initial.size() == 0 && fixture.faults.valid.load());
    const auto frozen = subscriber->watch();
    const auto next = publisher->update({{"b", body}, {"a", body}}, 2s); // 每次显式 TTL, 同一完整批次允许换序.
    CHECK(next && next->version == 2);
    eventually([&] { const auto view = subscriber->watch(); CHECK(view.find("a")->version == view.find("b")->version); return view.find("a")->version == 2; });
    CHECK(frozen.find("a")->version == 1 && frozen.find("b")->version == 1);
    const auto position = fixture.faults.state.source().position();
    std::this_thread::sleep_for(800ms); // 超过旧版 2s 租约的自动续租周期, 来源不得新增 Renew.
    CHECK(fixture.faults.state.source().position() == position);
}

// 仅明确整批未提交的版本冲突可修复一次, 第二次冲突直接返回.
void conflicts() {

    Fixture fixture;
    auto publisher = fixture.client.publisher({"repair", "main"});
    CHECK(publisher);
    fixture.faults.conflicts.store(1);
    const auto repaired = publisher->update("key", {7}, 1s);
    CHECK(repaired && repaired->version == 12 && fixture.faults.writes.load() == 2 && fixture.faults.queries.load() == 2);
    fixture.faults.conflicts.store(2);
    const auto rejected = publisher->update("key", {8}, 1s);
    CHECK(!rejected && rejected.error().code == comet::Error::Code::version && rejected.error().effect == comet::Error::Effect::unapplied);
    CHECK(fixture.faults.writes.load() == 4 && fixture.faults.queries.load() == 3 && fixture.faults.valid.load());
}

// 不完整版本响应及版本耗尽都在发送前拒绝, 不通过溢出或缺项隐式覆盖数据.
void queries() {

    Fixture fixture;
    auto publisher = fixture.client.publisher({"query", "main"});
    CHECK(publisher);
    fixture.faults.malformed.store(true);
    const auto malformed = publisher->update("key", {7}, 1s);
    CHECK(!malformed && malformed.error().code == comet::Error::Code::protocol && malformed.error().effect == comet::Error::Effect::unapplied);
    CHECK(fixture.faults.writes.load() == 0);
    CHECK(fixture.faults.state.publish({"query", "main"}, "key", std::make_shared<const astra::Catalog::Buffer>(1, 7), std::numeric_limits<std::uint64_t>::max(), 1000));
    const auto exhausted = publisher->update("key", {8}, 1s);
    CHECK(!exhausted && exhausted.error().code == comet::Error::Code::limit && fixture.faults.writes.load() == 0);
}

void stalled() {
    // 同时覆盖 reading 超时、consumed 后继续读取、首帧/半批 EOF 的 hold 归还与重连.
    for (const int mode : {1, 2, 3, 4}) {
        Fixture fixture;
        fixture.faults.stalled.store(mode);
        auto subscriber = fixture.client.subscriber({"source", "main"});
        CHECK(subscriber);
        eventually([&] { const auto view = subscriber->watch(); CHECK(!view.find("partial")); CHECK(view.state() != comet::Subscriber::State::failed); return fixture.faults.watches.load() >= 2 && view.state() == comet::Subscriber::State::ready; });
        CHECK(subscriber->watch().size() == 0); // 恢复到真实空基线, 不保留半批污染.
        subscriber->close();
        CHECK(subscriber->wait(3s));
    }
}

// 关闭发生在等待首帧或下一页时, 无需等待无进展超时, 旧流也不能在关闭后发起恢复.
void cancelled() {
    for (const int mode : {1, 2}) {
        Fixture fixture; // 每轮自有端口/Client, 不干扰其他故障用例.
        fixture.faults.stalled.store(mode);
        auto subscriber = fixture.client.subscriber({"source", "main"}); // 首个请求进入停滞夹具.
        CHECK(subscriber);
        eventually([&] { return fixture.faults.watches.load() != 0; });
        subscriber->close();
        CHECK(subscriber->wait(3s));
        CHECK(subscriber->watch().state() == comet::Subscriber::State::closed && subscriber->watch().size() == 0);
        CHECK(fixture.client.exceptions() == 0); // wait 已确认真实流结束, 不按机器调度速度断言关闭前绝不发生一次超时重连.
    }
}
} // namespace

// 真实 gRPC/TLS 故障测试, 由获准的 CTest 入口显式执行.
int main() {
    try {
        batches();
        conflicts();
        queries();
        lost();
        stalled();
        cancelled();
        std::cout << "Catalog confirmation and Watch stall cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
