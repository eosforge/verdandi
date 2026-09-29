// 独立 SDK 的同步发布契约用例, 仅用已有 gRPC 依赖, 不链接 C++26 Star.
#include "check.hpp"
#include "comet.grpc.pb.h"
#include "publishing.hpp"
#include <atomic>
#include <comet/client.hpp>
#include <future>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <latch>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;

// 模拟版本竞争及失去确认, 不替代 Linux 中真实 Star 的原子提交验收.
class Catalog final : public proto::comet::v1::Catalog::CallbackService {
public:
    std::atomic_uint queries{};     // 查询请求总数, 初始零.
    std::atomic_uint writes{};      // 提交请求总数, 初始零.
    std::atomic_uint conflicts{};   // 需要注入的明确版本冲突次数.
    std::atomic_bool lost{};        // 下一次提交实际应用后丢失确认.
    std::atomic_bool malformed{};   // 下一查询不返回条目, 检查协议拒绝.
    std::atomic_bool unsupported{}; // 下一查询模拟尚未提供 Query 的旧服务.
    std::atomic_bool slow_query{};  // 下一查询延迟, 用于总 deadline.
    std::atomic_bool slow_write{};  // 下一提交延迟, 用于并发和取消.
    std::atomic_bool entered{};     // 延迟提交已经进入服务端, 用于显式同步.
    std::atomic_bool paused{};      // 下一提交停在确定的服务端边界, 由夹具显式放行.
    std::atomic_uint renewals{};    // 新 SDK 不得调用 Renew.

    // 固定实例及按 Key 的水位, 返回顺序与请求一致; 未知值为零.
    grpc::ServerUnaryReactor* Query(grpc::CallbackServerContext* context, const proto::comet::v1::CatalogQueryRequest* request, proto::comet::v1::CatalogQueryReply* reply) override {
        ++queries;
        if (unsupported.exchange(false))
            return finish(*context, {grpc::StatusCode::UNIMPLEMENTED, "Query unavailable"});
        if (slow_query.exchange(false))
            std::this_thread::sleep_for(200ms);
        reply->set_instance("publisher-test");
        if (!malformed.exchange(false)) {
            const std::lock_guard lock(mutex_);
            for (const auto& key : request->keys()) {
                auto* row = reply->add_entries(); // 每个请求 Key 对应一个独立水位.
                row->set_key(key);
                row->set_version(versions_[key]);
            }
        }
        return finish(*context, grpc::Status::OK);
    }

    // 一次 RPC 同步替换整个批次的水位, 故障发生在提交前或确认前的明确边界.
    grpc::ServerUnaryReactor* Publish(grpc::CallbackServerContext* context, const proto::comet::v1::PublishRequest* request, proto::comet::v1::PublishReply* reply) override {

        ++writes;
        if (paused.exchange(false)) {
            entered.store(true);
            released_.wait(); // 只用于本用例, Fixture 析构也会放行, 不依赖固定睡眠.
        }
        if (slow_write.exchange(false)) {
            entered.store(true);
            std::this_thread::sleep_for(200ms);
        }
        const std::lock_guard lock(mutex_);
        if (conflicts.load() != 0) {
            --conflicts;
            for (const auto& entry : request->entries())
                versions_[entry.key()] = request->version() + 10;
            proto::comet::v1::Failure failure; // 精确说明整批未提交, 允许 SDK 的一次修复.
            failure.set_reason(proto::comet::v1::REASON_VERSION);
            failure.set_effect(proto::comet::v1::EFFECT_UNAPPLIED);
            context->AddTrailingMetadata("comet-error-bin", failure.SerializeAsString());
            return finish(*context, {grpc::StatusCode::FAILED_PRECONDITION, "Version conflict"});
        }
        for (const auto& entry : request->entries())
            versions_[entry.key()] = request->version();
        if (lost.exchange(false))
            return finish(*context, {grpc::StatusCode::DEADLINE_EXCEEDED, "Confirmation lost"});
        reply->set_instance("publisher-test");
        reply->set_version(request->version());
        return finish(*context, grpc::Status::OK);
    }

    // 只做计数, 若 SDK 仍自动续租, 契约用例会明确失败.
    grpc::ServerUnaryReactor* Renew(grpc::CallbackServerContext* context, const proto::comet::v1::CatalogRenewRequest*, proto::comet::v1::Empty*) override {
        ++renewals;
        return finish(*context, grpc::Status::OK);
    }

    // 注入进程已保存的版本上界, 不经过 SDK 外部 version 参数.
    void version(std::string key, std::uint64_t version) {
        const std::lock_guard lock(mutex_);
        versions_[std::move(key)] = version;
    }

    void resume() {
        if (!resumed_.exchange(true))
            released_.count_down();
    } // 单次停顿幂等放行, 测试完成和夹具异常清理可重复调用.

private:
    std::latch released_{1};     // 初始不放行, 每个 Fixture 至多一个确定性停顿.
    std::atomic_bool resumed_{}; // 防止异常清理重复 count_down 越过零.

    // 默认 reactor 完全由 gRPC 持有, 不在返回后借用请求或回复.
    static grpc::ServerUnaryReactor* finish(grpc::CallbackServerContext& context, grpc::Status status) {
        auto* reactor = context.DefaultReactor(); // Server 负责真正 RPC 完成后的清理.
        reactor->Finish(std::move(status));
        return reactor;
    }

    std::mutex mutex_;                              // 仅保护夹具版本表, 不模拟客户端锁.
    std::map<std::string, std::uint64_t> versions_; // 初始化为空, 独立 Key 可同版本.
};

class Fixture {
public:
    Catalog catalog;      // 注册到独立回环端口的受控服务.
    comet::Client client; // 测试各角色共享此客户端.

    // 不访问现有部署, 不下载证书或创建第三方依赖.
    Fixture() {
        grpc::ServerBuilder builder;
        int port{}; // 由内核分配, 不与并行用例共享端口.
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&catalog);
        server_ = builder.BuildAndStart();
        CHECK(server_ && port > 0);
        comet::Client::Options options; // 仅隔离回环夹具明确关闭 TLS 与登录.
        options.auth = false;
        options.tls = false;
        endpoint_ = "127.0.0.1:" + std::to_string(port);
        options.endpoints = {endpoint_};
        auto opened = comet::Client::open(std::move(options));
        CHECK(opened);
        client = std::move(*opened);
    }

    // 先取消客户端调用并排空, 再关闭 Server; 不留下后台线程访问测试栈.
    ~Fixture() {
        catalog.resume(); // 即使用例中途失败, 服务端也不无限等待测试信号.
        if (control_) {
            control_->close();
            if (!control_->wait(3s))
                std::terminate();
        }
        client.close();
        if (!client.wait(3s))
            std::terminate();
        server_->Shutdown(std::chrono::system_clock::now() + 3s);
        server_->Wait();
    }

    // 只供调度竞态用例访问既有私有核心, 不为测试扩大 SDK 公共接口.
    std::shared_ptr<comet::detail::Core> control() {
        comet::Client::Options options; // 与公开客户端相同的隔离回环服务, 另有独立生命周期.
        options.auth = false;
        options.tls = false;
        options.endpoints = {endpoint_};
        auto prepared = comet::detail::Core::prepare(std::move(options));
        CHECK(prepared);
        control_ = *prepared;
        control_->start();
        return control_;
    }

private:
    std::string endpoint_;                         // 内核分配的隔离回环端口, 供定向用例复用同一受控服务.
    std::shared_ptr<comet::detail::Core> control_; // 仅调度竞态用例启动, 夹具负责显式关闭.
    std::unique_ptr<grpc::Server> server_;         // 全部回调结束后才销毁服务实例.
};

// 内部版本涵盖已知水位、未知提交及新对象初始化, 无任何后台发布或续租.
void versions() {

    Fixture fixture;
    auto publisher = fixture.client.publisher({"service", "main"});
    CHECK(publisher);
    const auto value = std::make_shared<const std::vector<std::uint8_t>>(1, 7); // 两键共享完整不可变正文.
    const auto first = publisher->update({{"a", value}, {"b", value}}, 1s);
    CHECK(first && first->version == 1);
    fixture.catalog.conflicts.store(1);
    const auto repaired = publisher->update("a", {8}, 1s);
    CHECK(repaired && repaired->version == 13);
    fixture.catalog.lost.store(true);
    const auto lost = publisher->update("a", {9}, 1s);
    CHECK(!lost && lost.error().effect == comet::Error::Effect::unknown);
    const auto writes = fixture.catalog.writes.load(); // 记录错误返回时已发生的实际请求数.
    std::this_thread::sleep_for(450ms);                // 有界负向窗口, 超过旧实现 TTL/3 恢复周期.
    CHECK(fixture.catalog.writes.load() == writes && fixture.catalog.renewals.load() == 0);
    auto restarted = fixture.client.publisher({"service", "main"});
    CHECK(restarted);
    const auto next = restarted->update("a", {}, 2s);
    CHECK(next && next->version == 15);
}

// 错误协议、版本溢出和重复冲突不进入无限修复, 不产生额外业务调用.
void failures() {

    Fixture fixture;
    auto publisher = fixture.client.publisher({"service", "main"});
    CHECK(publisher);
    fixture.catalog.unsupported.store(true);
    const auto unsupported = publisher->update("a", {1}, 1s); // 没有 Query 的旧服务不能被误报为可重试网络故障.
    CHECK(!unsupported && unsupported.error().code == comet::Error::Code::protocol && unsupported.error().effect == comet::Error::Effect::unapplied);
    CHECK(fixture.catalog.writes.load() == 0);
    fixture.catalog.malformed.store(true);
    const auto malformed = publisher->update("a", {1}, 1s);
    CHECK(!malformed && malformed.error().code == comet::Error::Code::protocol && fixture.catalog.writes.load() == 0);
    fixture.catalog.version("a", std::numeric_limits<std::uint64_t>::max());
    const auto exhausted = publisher->update("a", {1}, 1s);
    CHECK(!exhausted && exhausted.error().code == comet::Error::Code::limit && fixture.catalog.writes.load() == 0);
    fixture.catalog.conflicts.store(2);
    const auto conflict = publisher->update("b", {1}, 1s);
    CHECK(!conflict && conflict.error().code == comet::Error::Code::version && conflict.error().effect == comet::Error::Effect::unapplied);
    CHECK(fixture.catalog.writes.load() == 2);

    // 本地连续写入也必须检查 UINT64_MAX, 不能绕过 Query 的上界保护而回绕到零.
    auto final = fixture.client.publisher({"final", "main"});
    fixture.catalog.version("final", std::numeric_limits<std::uint64_t>::max() - 1);
    CHECK(final);
    const auto last = final->update("final", {}, 1s);
    CHECK(last && last->version == std::numeric_limits<std::uint64_t>::max());
    const auto queries = fixture.catalog.queries.load(); // 已有有效基线, 本次必须在本地拒绝.
    const auto overflow = final->update("final", {}, 1s);
    CHECK(!overflow && overflow.error().code == comet::Error::Code::limit);
    CHECK(fixture.catalog.queries.load() == queries && fixture.catalog.writes.load() == 3);
}

// 查询也消耗调用截止; 同对象并发返回 busy, close 可取消网络等待且不关闭 Client.
void cancellation() {

    Fixture fixture;
    auto publisher = fixture.client.publisher({"service", "main"});
    CHECK(publisher);
    CHECK(publisher->update("ready", {}, 1s));         // 先确认匿名连接已建立, 短 deadline 必须覆盖实际 Query.
    const auto before = fixture.catalog.writes.load(); // 只读查询超时不得增加业务提交.
    fixture.catalog.slow_query.store(true);
    const auto timeout = publisher->update("a", {1}, 1s, 30ms);
    CHECK(!timeout && timeout.error().code == comet::Error::Code::timeout && timeout.error().effect == comet::Error::Effect::unapplied);
    CHECK(fixture.catalog.writes.load() == before);
    fixture.catalog.slow_write.store(true);
    auto pending = std::async(std::launch::async, [&] { return publisher->update("a", {2}, 1s); }); // 并发仅由测试显式产生.
    const auto deadline = std::chrono::steady_clock::now() + 3s;                                    // 观察服务器已进入真实 RPC, 不靠抢时序.
    while (!fixture.catalog.entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    CHECK(fixture.catalog.entered.load());
    const auto busy = publisher->update("b", {3}, 1s);
    CHECK(!busy && busy.error().code == comet::Error::Code::busy);
    publisher->close();
    const auto cancelled = pending.get();
    CHECK(!cancelled && cancelled.error().effect == comet::Error::Effect::unknown && publisher->wait(3s));
    auto other = fixture.client.publisher({"service", "other"});
    CHECK(other); // 子对象关闭没有关闭共享客户端, 不要求原取消是远端未提交.
}

// 相同 Key 集合换序仍只初始化一次; 更换集合和未知确认均必须重新查询.
void baseline() {

    Fixture fixture;
    auto publisher = fixture.client.publisher({"baseline", "main"});
    CHECK(publisher);
    auto value = std::make_shared<const std::vector<std::uint8_t>>(1, 3);  // 固定不可变测试正文.
    const std::weak_ptr<const std::vector<std::uint8_t>> released = value; // 返回后 SDK 不保留业务正文.
    const auto first = publisher->update({{"a", value}, {"b", value}}, 1s);
    const auto second = publisher->update({{"b", value}, {"a", value}}, 2s);
    CHECK(first && first->version == 1 && second && second->version == 2);
    value.reset();
    CHECK(released.expired());
    CHECK(fixture.catalog.queries.load() == 1 && fixture.catalog.writes.load() == 2);
    const auto changed = publisher->update("c", {}, 1s); // 换集合重新初始化, 保持本对象已经分配的高水位.
    CHECK(changed && changed->version == 3);
    CHECK(fixture.catalog.queries.load() == 2);
    fixture.catalog.lost.store(true);
    CHECK(!publisher->update("c", {1}, 1s));
    CHECK(fixture.catalog.queries.load() == 2);
    const auto recovered = publisher->update("c", {2}, 1s); // 未知结果后重新查询, 不复用未确认基线.
    CHECK(recovered && recovered->version == 5);
    CHECK(fixture.catalog.queries.load() == 3);
    CHECK(!publisher->wait(std::chrono::milliseconds::min())); // 极小超时立即检查, 不做溢出的时钟换算.
    publisher->close();
    CHECK(publisher->wait(std::chrono::milliseconds::max())); // 极大超时也遵守已完成谓词, 不回绕成负截止.
}

// 人为传入控制轮之前捕获的空绑定, 实际新调用仍须使用当前有效绑定完成.
void scheduling() {

    Fixture fixture;
    const auto core = fixture.control(); // 夹具负责异常退出时关闭额外核心.
    core->start();                       // 重复启动不得让一个 Alarm 上出现重叠控制轮.
    auto publisher = core->publisher({"schedule", "main"});
    CHECK(publisher);
    const auto body = std::make_shared<const std::vector<std::uint8_t>>(1, 1);
    fixture.catalog.paused.store(true);
    auto pending = std::async(std::launch::async, [&] { return (*publisher)->update({{"key", body}}, 1s, 3s); });
    const auto deadline = std::chrono::steady_clock::now() + 3s; // 等真实 RPC 进入停顿边界, 不猜测连接就绪时刻.
    while (!fixture.catalog.entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    CHECK(fixture.catalog.entered.load());
    (*publisher)->poll({}, {}, {}, false); // 模拟旧轮次的空快照, 不代表当前 Core 没有绑定.
    fixture.catalog.resume();
    CHECK(pending.get());
    (*publisher)->close();
    CHECK((*publisher)->wait(3s));
}

// 用户回调故意占住唯一控制轮, Client::close 仍须直接取消另一线程上的同步发布.
void shutdown() {

    Fixture fixture;
    auto publisher = fixture.client.publisher({"shutdown", "main"});
    CHECK(publisher && publisher->update("key", {}, 1s));
    fixture.catalog.paused.store(true);
    auto pending = std::async(std::launch::async, [&] { return publisher->update("key", {2}, 1s); });
    const auto deadline = std::chrono::steady_clock::now() + 3s; // 先确保正在等待真实 Publish.
    while (!fixture.catalog.entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    CHECK(fixture.catalog.entered.load());

    // promise 在 Fixture 之前析构, 断言失败也使 wait 就绪; callback 不借用任何测试栈对象.
    const auto entered = std::make_shared<std::promise<void>>();
    auto notified = entered->get_future();
    std::promise<void> release;
    comet::Subscriber::Options options;
    options.changed = [entered, released = release.get_future().share()](comet::Subscriber::View) {
        entered->set_value();
        released.wait();
    };
    auto subscriber = fixture.client.subscriber({"shutdown", "main"}, {}, std::move(options));
    CHECK(subscriber && notified.wait_for(3s) == std::future_status::ready);
    fixture.client.close();
    const auto stopped = pending.wait_for(500ms); // 小于原 3s RPC 截止, 控制轮仍被 callback 占住.
    release.set_value();
    CHECK(stopped == std::future_status::ready);
    const auto result = pending.get();
    CHECK(!result && result.error().code == comet::Error::Code::closed && result.error().effect == comet::Error::Effect::unknown);
}

} // namespace

// 显式 CTest 入口, 创建/格式化源码不会自动启动本用例.
int main() {
    try {
        versions();
        baseline();
        scheduling();
        shutdown();
        failures();
        cancellation();
        std::cout << "Synchronous Catalog publisher cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
