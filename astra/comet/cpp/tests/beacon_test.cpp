#include "check.hpp"
#include "ephemeris_service.hpp"
#include "identity.hpp"
#include <atomic>
#include <comet/client.hpp>
#include <condition_variable>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <thread>

namespace {
using namespace std::chrono_literals;
using astra::Ephemeris;

// 实际原生提交之后注入失败回执, 或暂缓一个 Data 回执. 不用假字典替代服务端规则.
class Faults final : public proto::comet::v1::Ephemeris::CallbackService {
public:
    std::atomic_bool lose_create{};  // 恰好下一次 Create 在提交后返回不确定超时.
    std::atomic_bool lose_update{};  // 恰好下一次 Data 提交后返回不确定超时.
    std::atomic_bool lose_renew{};   // 恰好下一次 Renew 提交后返回不确定超时.
    std::atomic_bool hold_update{};  // 只延迟响应, 不让处理线程等待网络或测试条件.
    std::atomic_uint updates{};      // 实际到达的 Update 次数, 包括固定 order 重试.
    std::atomic_uint renewals{};     // 实际到达的 Renew 次数, 不以 SDK ready 代替成功证据.
    std::atomic_bool valid{true};    // 原生提交/身份若意外失败, 留给测试主线程明确断言.
    std::atomic_bool held{};         // Data 请求已经实际占用一个在途 RPC.
    std::atomic_bool duplicate{};    // 相同 Renew order 已观察到原截止未改变.
    Ephemeris::State& state;         // 真实来源/投影/轮.
    astra::Gateway& gateway;         // 与实际公共服务相同的登录/Ready 入口.
    std::atomic<std::int64_t>& time; // 只注入测试业务时间, 不改变 SDK 的本地租约时钟.
    Ephemeris::Service service;      // 未注入故障的操作直接转交生产 handler.

    Faults(Ephemeris::State& state, astra::Gateway& gateway, std::atomic<std::int64_t>& time) : state(state), gateway(gateway), time(time), service(state, gateway) {} // 创建不启动线程.

    // 清理始终可以完成被暂缓的默认 unary reactor, 不把指针带到 Server::Wait 之后.
    void release() {
        const std::lock_guard lock(mutex_);
        if (pending_) {
            std::exchange(pending_, nullptr)->Finish(grpc::Status::OK);
        }
    }

    grpc::ServerUnaryReactor* Create(grpc::CallbackServerContext* context, const proto::comet::v1::CreateRequest* request, proto::comet::v1::CreateReply* reply) override {
        if (!lose_create.exchange(false)) {
            return service.Create(context, request, reply);
        }
        auto permit = gateway.enter(*context);
        auto result = state.create({request->scope().sector(), request->scope().spectrum()}, bytes(request->attr()), bytes(request->data()), request->ttl_ms());
        if (!permit || !result) {
            valid.store(false);
        }
        return finish(context, grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Injected loss after Create commit")); // 不附 unapplied 细节.
    }

    grpc::ServerUnaryReactor* Update(grpc::CallbackServerContext* context, const proto::comet::v1::UpdateRequest* request, proto::comet::v1::UpdateReply* reply) override {
        ++updates;
        const bool lost = lose_update.exchange(false); // 每项故障只消费一次, 后续真实重试会成功.
        const bool delayed = hold_update.exchange(false);
        if (!lost && !delayed) {
            return service.Update(context, request, reply);
        }
        auto permit = gateway.enter(*context);
        auto result = state.update({request->scope().sector(), request->scope().spectrum()}, request->uuid(), bytes(request->data()), request->order());
        if (!permit || !result) {
            valid.store(false);
        }
        if (lost) {
            return finish(context, grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Injected loss after Data commit"));
        }
        reply->set_order(request->order());
        const std::lock_guard lock(mutex_);
        pending_ = context->DefaultReactor(); // 请求与响应由 gRPC 保持到 Finish, 本测试不在期间再次访问它们.
        held.store(true);
        return pending_;
    }

    grpc::ServerUnaryReactor* Renew(grpc::CallbackServerContext* context, const proto::comet::v1::RenewRequest* request, proto::comet::v1::RenewReply* reply) override {
        ++renewals;
        auto permit = gateway.enter(*context);
        auto result = state.renew({request->scope().sector(), request->scope().spectrum()}, request->uuid(), request->order());
        if (!permit || !result) {
            valid.store(false);
        }
        const bool lost = lose_renew.exchange(false);
        {
            const std::lock_guard lock(mutex_);
            state.source().each([&](const astra::Scope&, std::string_view uuid, const Ephemeris::Record& record) {
                if (uuid == request->uuid()) {
                    if (lost) {
                        order_ = request->order();
                        deadline_ = record.deadline;
                    } else if (order_ == request->order()) {
                        duplicate.store(record.deadline == deadline_); // 后一次业务时间已变化, 重复确认仍不能续命.
                    }
                }
            });
        }
        if (lost) {
            time.fetch_add(100'000'000); // 有限推进 100 ms, 当前注册仍有效.
            return finish(context, grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Injected loss after Renew commit"));
        }
        reply->set_order(request->order());
        return finish(context, grpc::Status::OK);
    }

    grpc::ServerUnaryReactor* Remove(grpc::CallbackServerContext* context, const proto::comet::v1::RemoveRequest* request, proto::comet::v1::Empty* reply) override {
        return service.Remove(context, request, reply);
    } // 注销仍走全部生产校验.

    grpc::ServerWriteReactor<proto::comet::v1::EphemerisWatchReply>* Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request) override {
        return service.Watch(context, request);
    } // 下行不注入假内容.

private:
    static Ephemeris::Value bytes(const std::string& value) {
        return std::make_shared<const Ephemeris::Buffer>(value.begin(), value.end());
    } // 测试中保持真实不可变正文.

    static grpc::ServerUnaryReactor* finish(grpc::CallbackServerContext* context, const grpc::Status& status) {
        auto* reactor = context->DefaultReactor();
        reactor->Finish(status);
        return reactor;
    } // 默认 reactor 由 gRPC 释放.

    std::mutex mutex_;                    // 只保护故障注入记录和暂缓的完成指针.
    grpc::ServerUnaryReactor* pending_{}; // 唯一被延迟的响应, release 后置空, 不重复 Finish.
    std::uint64_t order_{};               // 丢失过确认的 Renew order, 零为没有记录.
    astra::Clock::Time deadline_{};       // 对应第一次真实提交的截止, 用于检查幂等性.
};

class Fixture {
public:
    std::atomic<std::int64_t> time{1'000'000'000};       // 不自动前进的真实 State 时钟输入, 用例显式改变.
    astra::Access access;                                // 匿名公共配置也复用生产 Gateway.
    astra::Gateway gateway{access, "star-fault", false}; // 本测试只注入传输结果, 准入另有真实用例.
    Ephemeris::State state{[this] { return std::optional(astra::Clock::Reading{.time = astra::Clock::Time(std::chrono::nanoseconds(time.load())), .ready = true}); }, {}};
    Faults faults{state, gateway, time}; // 组合真实状态和生产处理器.
    comet::Client client;                // 最后关闭后等待实际清理再销毁服务端.
    std::string endpoint;                // 本例独占的回环监听.

    Fixture() {
        auto credentials = astra::Identity::external(std::filesystem::path(ASTRA_FIXTURES) / "star-a");
        CHECK(credentials);
        grpc::ServerBuilder builder;
        int port{}; // 独立端口, 不碰部署服务.
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

    ~Fixture() {
        stop();
    } // 同样覆盖主线程断言失败, 不遗留持有裸 reactor 的请求.

private:
    void stop() {
        faults.release();
        client.close();
        if (!client.wait(5s)) {
            std::terminate();
        }
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

    std::mutex mutex_;                     // 控制循环等待专用锁.
    std::condition_variable_any changed_;  // 停止可立即唤醒.
    std::unique_ptr<grpc::Server> server_; // 在 worker 仍能排空回调时先关闭.
    std::jthread worker_;                  // 每服务一个测试控制线程, 不是每注册线程.
};

// 只等待可观察事实, 每次至多 5 秒, 不用固定长休眠推测成功.
void eventually(auto&& condition) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

void lost() {

    Fixture fixture;
    fixture.faults.lose_create.store(true);
    auto beacon = fixture.client.beacon({"service", "main"}, {1}, {2}, 1s);
    CHECK(beacon);
    eventually([&] { return beacon->state().phase == comet::Beacon::Phase::ready; });
    CHECK(fixture.state.source().size() == 2); // 不确定创建生成新 UUID, 原孤儿由 TTL 清理, 不冒充恰好一次.
    fixture.faults.lose_update.store(true);
    auto update = beacon->update(std::vector<std::uint8_t>{3});
    CHECK(update.wait_for(5s) == std::future_status::ready);
    const auto result = update.get();
    CHECK(!result && result.error().code == comet::Error::Code::timeout && result.error().effect == comet::Error::Effect::unknown);
    eventually([&] { return fixture.faults.updates.load() >= 2; });
    std::uint64_t order{}; // 后台重试仍使用同一正 order, 不改变原 future 的不确定结论.
    const auto identity = *beacon->state().identity;
    fixture.state.source().each([&](const astra::Scope&, std::string_view uuid, const Ephemeris::Record& record) { if (uuid == identity.uuid) { order = record.update; CHECK(*record.data == Ephemeris::Buffer{3}); } });
    CHECK(order == 1);
    fixture.faults.lose_renew.store(true);
    eventually([&] { return fixture.faults.duplicate.load(); });
    CHECK(fixture.faults.valid.load());
}

void independent() {

    Fixture fixture;
    auto beacon = fixture.client.beacon({"service", "main"}, {}, {}, 1s);
    CHECK(beacon);
    eventually([&] { return beacon->state().phase == comet::Beacon::Phase::ready; });
    fixture.faults.hold_update.store(true);
    auto update = beacon->update(std::vector<std::uint8_t>{4});
    eventually([&] { return fixture.faults.held.load(); });
    const auto before = fixture.faults.renewals.load();
    eventually([&] { return fixture.faults.renewals.load() > before; });
    CHECK(update.wait_for(0ms) == std::future_status::timeout); // Data 未结束时仍有真实续租到达.
    fixture.faults.release();
    CHECK(update.wait_for(3s) == std::future_status::ready && update.get());
    CHECK(fixture.faults.valid.load());
}
} // namespace

int main() {
    try {
        lost();
        independent();
        std::cout << "Beacon RPC fault cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
