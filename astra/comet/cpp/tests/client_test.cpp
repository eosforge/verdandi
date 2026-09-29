#include "catalog_service.hpp"
#include "check.hpp"
#include "core.hpp"
#include "ephemeris_service.hpp"
#include "identity.hpp"
#include "reading.hpp"
#include "readout.hpp"
#include <array>
#include <atomic>
#include <comet/client.hpp>
#include <condition_variable>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <ranges>
#include <thread>
#include <unordered_set>

namespace {
using namespace std::chrono_literals;

// 真实公共 TLS 端口, 与内部节点的准入身份分开; 每个用例由操作系统独立分配端口.
class Fixture {
public:
    astra::Library library;                                                                                                                                                          // 所有测试提交真实进入 Almanac, 不用内存假 Stub 替代服务行为.
    astra::Access access;                                                                                                                                                            // 默认空凭据, 用 password() 显式安装公开测试账号.
    astra::Gateway gateway;                                                                                                                                                          // 最多一条 Session, 验证多个 Reader 确实共享登录.
    astra::Readout readout;                                                                                                                                                          // 与生产相同的快照、后缀和取消处理.
    std::atomic<std::int64_t> now{1'000'000'000};                                                                                                                                    // 注册时钟独立可控, 观察者不访问 Pulsar.
    astra::Ephemeris::State ephemeris{[this] { return std::optional(astra::Clock::Reading{.time = astra::Clock::Time(std::chrono::nanoseconds(now.load())), .ready = true}); }, {}}; // 真实原生来源/TTL.
    astra::Ephemeris::Service registrations{ephemeris, gateway, [this] { changed_.notify_all(); }};                                                                                  // 与 Almanac 共用唯一登录服务.
    astra::Catalog::State catalog{[this] { return std::optional(astra::Clock::Reading{.time = astra::Clock::Time(std::chrono::nanoseconds(now.load())), .ready = true}); }, {}};     // 与注册同一可控纪元.
    astra::Catalog::Service publications{catalog, gateway, [this] { changed_.notify_all(); }};                                                                                       // 真实动态业务入口.
    std::string endpoint;                                                                                                                                                            // 当前内核选择的地址, 仅在 server 存活时监听.

    // auth 可关闭登录用于单独验证匿名连接, TLS 默认仍然开启.
    explicit Fixture(std::string instance = "star-test", bool auth = true, bool tls = true) : library({}, [this](const astra::Scope& scope, const astra::Almanac::Change& change) noexcept { readout.changed(scope, change); }), gateway(access, std::move(instance), auth, 1), readout(library, gateway, [this] { changed_.notify_all(); }) {

        grpc::ServerBuilder builder; // 公共接口不注册 Pulsar/Orbit, 客户端没有节点私钥.
        builder.RegisterService(&gateway);
        builder.RegisterService(&readout);
        builder.RegisterService(&registrations);
        builder.RegisterService(&publications);
        builder.SetMaxReceiveMessageSize(8 * 1024 * 1024);
        builder.SetMaxSendMessageSize(8 * 1024 * 1024);
        auto credentials = astra::Identity::external(std::filesystem::path(ASTRA_FIXTURES) / "star-a");
        if (!credentials) {
            throw std::runtime_error(credentials.error().message);
        }
        int port{};
        builder.AddListeningPort("127.0.0.1:0", tls ? *credentials : grpc::InsecureServerCredentials(), &port);
        server_ = builder.BuildAndStart();
        CHECK(server_ && port > 0);
        endpoint = "127.0.0.1:" + std::to_string(port);
        try {
            worker_ = std::jthread([this](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    readout.pump(std::chrono::steady_clock::now());
                    registrations.pump(std::chrono::steady_clock::now());
                    publications.pump(std::chrono::steady_clock::now());
                    std::unique_lock lock(mutex_);
                    changed_.wait_for(lock, stop, 2ms, [] { return false; });
                }
            });
            gateway.ready();
        } catch (...) {
            server_->Shutdown();
            server_->Wait();
            throw;
        }
    }

    ~Fixture() { // 成功、断言异常、重复停机使用同一清理路径, 不影响其他夹具端口.
        stop();
    }

    void stop() {
        if (server_) {
            gateway.stop();
            readout.stop();
            registrations.stop();
            publications.stop();
            server_->Shutdown(std::chrono::system_clock::now() + 2s);
            server_->Wait();
            worker_.request_stop();
            changed_.notify_all();
            worker_.join();
            readout.pump(std::chrono::steady_clock::now(), 65536);
            registrations.pump(std::chrono::steady_clock::now(), 65536);
            publications.pump(std::chrono::steady_clock::now(), 65536);
            server_.reset();
        }
    }

    // 完整基线包含 count 个 Key, 默认一条, 首条字符串值用于跨 Star 防回退断言.
    void fill(std::uint64_t version = 1, unsigned count = 1) {
        auto draft = library.prepare({"routes", "main"}, version);
        CHECK(draft);
        for (unsigned index = 0; index < count; ++index) {
            CHECK(draft->set("key-" + std::to_string(index), {static_cast<std::uint8_t>(index % 251)}));
        }
        CHECK(library.reset(std::move(*draft)));
    }

    void password(std::uint8_t value) { // 模拟 Receiver 已校验的内部凭据提交, 不直接篡改 Session.
        CHECK(access.apply("test", astra::Almanac::Buffer{value}, [] { return std::expected<bool, astra::Almanac::Error>(true); }));
    }

    comet::Client::Options options(bool auth = true, bool tls = true) const {
        comet::Client::Options options;
        options.endpoints = {endpoint};
        options.auth = auth;
        options.tls = tls;
        if (auth) {
            options.key = "test";
            options.secret = {42};
        }
        if (tls) {
            options.ca = std::filesystem::path(ASTRA_FIXTURES) / "star-a" / "ca.pem";
        }
        options.timeout = 500ms;
        return options;
    }

private:
    std::mutex mutex_;                     // 只用于控制线程有界等待, 不在等待时持有 Readout 或 Library 锁.
    std::condition_variable_any changed_;  // 新事件只唤醒共享控制循环.
    std::unique_ptr<grpc::Server> server_; // 在 worker_ 停止前完成真实 Server 排空.
    std::jthread worker_;                  // 一台服务一个控制线程, 不按 Watch 数量扩容.
};

// 保证用例异常路径先关闭 SDK 并等待本地真实完成, 再析构服务端和回调引用的局部数据.
class Client {
public:
    comet::Client value; // 公开 SDK 句柄, 测试不访问其私有 Core.

    explicit Client(comet::Client::Options options) {
        auto opened = comet::Client::open(std::move(options));
        CHECK(opened);
        value = std::move(*opened);
    }

    ~Client() {
        value.close();
        if (!value.wait(5s)) {
            std::terminate(); // 超出真实清理期限明确使测试失败, 不留下访问已析构夹具的后台回调.
        }
    }
};

// 网络调度只使用有界等待, 正确性依据可观测状态, 不依赖固定 sleep 猜测对端完成.
void eventually(auto&& condition, std::chrono::seconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

// 关闭确认不等于对象已释放; 验证最后一轮 Alarm 不再通过保留闭包把 Core 永久留在堆上.
void cleanup() {

    Fixture fixture("cleanup", false, false); // 独占匿名回环服务, 测试不连接任意固定端口.
    fixture.fill();
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        auto core = comet::detail::Core::prepare(fixture.options(false, false)); // 只借助私有测试入口观察所有权, 不扩大 SDK 公共 API.
        CHECK(core);
        const std::weak_ptr<comet::detail::Core> retained = *core; // 弱引用不阻止真实析构, 同时覆盖立即 close 与首轮回调竞争.
        auto reader = (*core)->reader({"routes", "main"}, {}, {}); // 工作空间实际捕获对象, 才能暴露跨轮保留强引用形成的拥有环.
        CHECK(reader);
        const std::weak_ptr<comet::detail::Activity> activity = *reader; // 关闭后两个对象都必须自然释放.
        (*core)->start();
        if (attempt % 2 != 0) {
            eventually([&] { return (*reader)->load().state() == comet::Reader::State::ready; });
        } // 同时覆盖未推进即关闭和已经捕获/消费数据后的关闭.
        (*core)->close();
        CHECK((*core)->wait(3s));
        reader->reset();
        core->reset();
        eventually([&] { return retained.expired() && activity.expired(); });
    }
}

// 重复定向事件合并、poll 期间再次入队、关闭后迟到事件和目录复用共享一条真实控制路径.
void queued() {

    Fixture fixture("queued", false, false); // 无登录流替局部事件触发全局刷新.
    fixture.fill();
    auto prepared = comet::detail::Core::prepare(fixture.options(false, false)); // 先接纳, 后启动, 确定性堆积重复唤醒.
    CHECK(prepared);
    const auto core = *prepared;
    std::vector<std::shared_ptr<comet::detail::Reading>> readers; // 活动对象保持到真实关闭, 测试不把裸指针留给网络.
    std::weak_ptr<comet::detail::Reading> first;                  // 回调只借弱引用, 不与 Reading 自身形成拥有环.

    struct Stop {
        std::shared_ptr<comet::detail::Core> core; // 析构先排空异步通知, readers 和 first 随后才销毁.
        bool started{};                            // 准备阶段失败也要启动一次关闭推进, 避免未 Set 的 Alarm 无法完成清理.

        ~Stop() {
            core->close();
            if (!started)
                core->start();
            if (!core->wait(5s))
                std::terminate();
        }
    } stop{core};

    for (unsigned index = 0; index < 32; ++index) {
        comet::Reader::Options options; // 首个对象在自己的通知中关闭, 强制 poll 尚未返回时重新排队.
        if (index == 0) {
            options.changed = [&first](const comet::Reader::View& view) {
                if (view.state() == comet::Reader::State::ready) {
                    if (auto reader = first.lock())
                        reader->close();
                }
            };
        }
        auto reader = core->reader({"routes", "main"}, {}, std::move(options));
        CHECK(reader);
        readers.push_back(std::move(*reader));
    }
    first = readers.front(); // 在 start 之前完成回调绑定, 不与控制线程并发修改弱引用.

    {
        std::array<std::jthread, 4> workers; // 多生产者只发就绪事件, 不改变业务状态或创建重复流.
        for (unsigned index = 0; index < workers.size(); ++index) {
            workers[index] = std::jthread([&, index] {
                for (unsigned repeat = 0; repeat < 1024; ++repeat)
                    core->wake(readers[(repeat + index) % readers.size()].get());
            });
        }
    } // 全部投递后才启动: 无论多少重复事件, 每对象仅需一个预留队列槽.
    core->start();
    stop.started = true;
    CHECK(readers.front()->wait(3s)); // 关闭来自通知内部, 后续必须消费这一轮中途产生的事件.
    eventually([&] {
        return std::ranges::all_of(readers | std::views::drop(1), [](const auto& reader) { return reader->load().state() == comet::Reader::State::ready; });
    });

    for (unsigned round = 0; round < 32; ++round) {
        const auto old = readers.back(); // 故意保留已完成对象, 模拟旧 RPC 或调用者晚到的唤醒.
        old->close();
        CHECK(old->wait(3s));
        auto fresh = core->reader({"routes", "main"}, {}, {});
        CHECK(fresh);
        readers.back() = std::move(*fresh);
        core->wake(old.get()); // 新对象接纳已清理旧目录; 旧指针仍有效但不能重入就绪队列.
        eventually([&] { return readers.back()->load().state() == comet::Reader::State::ready; });
    }
    CHECK(core->exceptions() == 0);
    const auto rounds = core->schedule(); // 定向唤醒已覆盖全部对象, 就绪轮必须实际发生且推进数不少于对象数.
    CHECK(rounds.ready > 0 && rounds.polled >= readers.size());
}

// 一条登录流承载多个对象, 分页完整后才可见, 移动/关闭不破坏仍持有的不可变视图.
void shared() {

    Fixture fixture;
    fixture.password(42);
    fixture.fill(1, 600);
    Client client(fixture.options());
    auto copy = client.value;
    auto first = copy.reader({"routes", "main"});
    auto exact = client.value.reader({"routes", "main"}, "key-599");
    CHECK(first && exact);
    eventually([&] { return first->load().state() == comet::Reader::State::ready && exact->load().state() == comet::Reader::State::ready; });
    const auto old = first->load();
    CHECK(old.size() == 600 && old.version() == 1 && exact->load().size() == 1);
    const auto original = old.find("key-0");
    CHECK(original && original->at(0) == 0);

    CHECK(fixture.library.apply({"routes", "main"}, 2, "key-0", astra::Almanac::Buffer{9}));
    eventually([&] { return first->load().version() == 2 && exact->load().version() == 2; });
    CHECK(first->load().find("key-0")->at(0) == 9);
    CHECK(old.find("key-0") == original && original->at(0) == 0);
    first->close();
    CHECK(first->wait(3s));
    CHECK(first->load().state() == comet::Reader::State::closed);
    CHECK(fixture.library.apply({"routes", "main"}, 3, "key-599", astra::Almanac::Buffer{}));
    eventually([&] { return exact->load().version() == 3; });
    CHECK(exact->load().find("key-599") && exact->load().find("key-599")->empty());
    client.value.close();
    CHECK(copy.wait(3s));
    CHECK(old.size() == 600 && old.find("key-0") == original);
}

// 无效凭据必须明确失败且不降级; 外部更新 SECRET 可以恢复同一对象, 新会话不改写旧绑定.
void authentication() {

    Fixture fixture;
    fixture.password(43);
    fixture.fill();
    Client client(fixture.options());
    auto reader = client.value.reader({"routes", "main"});
    CHECK(reader);
    eventually([&] { const auto view = reader->load(); return view.state() == comet::Reader::State::failed && view.error() && view.error()->code == comet::Error::Code::session; });
    CHECK(!reader->load().version());
    CHECK(client.value.secret({43}));
    eventually([&] { return reader->load().state() == comet::Reader::State::ready; });
    const auto before = reader->load();
    fixture.password(44);
    eventually([&] { return reader->load().state() != comet::Reader::State::ready; });
    CHECK(before.version() == 1 && before.size() == 1);
    CHECK(client.value.secret({44}));
    eventually([&] { return reader->load().state() == comet::Reader::State::ready; });
    CHECK(fixture.library.apply({"routes", "main"}, 2, "key-0", astra::Almanac::Buffer{7}));
    eventually([&] { return reader->load().version() == 2; });
}

// 首个 Star 失效后顺序切换, 目标基线落后时保留旧内容, 追平后才安装其新的实例位置.
void failover() {

    Fixture first("star-first", false);
    Fixture second("star-second", false);
    first.fill(3);
    second.fill(1);
    auto options = first.options(false);
    options.endpoints.push_back(second.endpoint);
    Client client(std::move(options));
    auto reader = client.value.reader({"routes", "main"});
    CHECK(reader);
    eventually([&] { return reader->load().state() == comet::Reader::State::ready; });
    CHECK(reader->load().version() == 3 && reader->load().instance() == "star-first");
    first.stop();
    eventually([&] { const auto view = reader->load(); return view.error() && view.error()->code == comet::Error::Code::version; });
    CHECK(reader->load().version() == 3 && reader->load().instance() == "star-first");
    second.fill(3);
    eventually([&] { const auto view = reader->load(); return view.state() == comet::Reader::State::ready && view.instance() == "star-second"; });
    CHECK(reader->load().version() == 3);
}

// 观察者异常可诊断; SDK 内等待被拒绝, 非阻塞 close 允许从观察者发起且之后不再新通知.
void callbacks() {

    Fixture fixture("star-test", false);
    fixture.fill();
    std::atomic_uint calls{}; // 同对象回调串行, 原子只用于与断言线程通信.
    std::atomic_bool rejected{};
    Client client(fixture.options(false));
    comet::Reader::Options options;
    options.changed = [&](comet::Reader::View view) {
        if (view.state() != comet::Reader::State::ready) {
            return;
        }
        calls.fetch_add(1);
        try {
            static_cast<void>(client.value.wait(0ms));
        } catch (const std::logic_error&) {
            rejected.store(true);
        }
        if (view.version() == 2) {
            client.value.close();
        }
        throw std::runtime_error("Deliberate observer exception");
    };
    auto reader = client.value.reader({"routes", "main"}, {}, std::move(options));
    CHECK(reader);
    eventually([&] { return client.value.exceptions() == 1 && rejected.load(); });
    CHECK(fixture.library.apply({"routes", "main"}, 2, "key-0", astra::Almanac::Buffer{8}));
    CHECK(client.value.wait(3s));
    CHECK(calls.load() == 2 && client.value.exceptions() == 2);
    CHECK(reader->wait(0ms) && reader->load().state() == comet::Reader::State::closed);
}

// 反复创建/移动/关闭归还应用及真实流额度; 只关闭 Client 句柄引用不撤回仍存活 Reader 的订阅.
void lifetime() {

    Fixture fixture("star-test", false, false);
    fixture.fill();
    auto options = fixture.options(false, false);
    options.readers = 1;
    Client client(std::move(options));
    for (unsigned round = 0; round < 40; ++round) {
        auto reader = client.value.reader({"routes", "main"});
        CHECK(reader);
        CHECK(!client.value.reader({"routes", "main"}));
        comet::Reader moved = std::move(*reader);
        eventually([&] { return moved.load().state() == comet::Reader::State::ready; });
        moved.close();
        CHECK(moved.wait(3s));
    }
    auto final = client.value.reader({"routes", "main"});
    CHECK(final);
    eventually([&] { return final->load().version() == 1; });
    client.value = {}; // Reader 独立应用拥有数仍保持自动订阅, 空 Client 析构不会隐式 close 它.
    CHECK(fixture.library.apply({"routes", "main"}, 2, "key-0", astra::Almanac::Buffer{6}));
    eventually([&] { return final->load().version() == 2; });
    final->close();
    CHECK(final->wait(3s));
}

// 本地输入失败不会发网络: 空地址、重复地址、混用 TLS 材料、内部范围和非法容量.
void inputs() {

    CHECK(!comet::Client::open({}));
    Fixture fixture("star-test", false);
    auto options = fixture.options(false);
    options.endpoints.push_back(fixture.endpoint);
    CHECK(!comet::Client::open(std::move(options)));
    options = fixture.options(false);
    options.tls = false;
    CHECK(!comet::Client::open(std::move(options)));
    options = fixture.options(false);
    options.ca = std::filesystem::path(ASTRA_FIXTURES) / "star-a" / "key.pem";
    CHECK(!comet::Client::open(std::move(options)));
    Client client(fixture.options(false));
    CHECK(!client.value.reader({"__auth", "keys"}));
    CHECK(!client.value.reader({"routes", "main"}, std::string(1025, 'x')));
    comet::Reader::Options small;
    small.bytes = 1;
    CHECK(!client.value.reader({"routes", "main"}, {}, std::move(small)));
    client.value.close();
    CHECK(!client.value.reader({"routes", "main"}));
    CHECK(client.value.wait(3s));
}

// 同一 Session 同时承载 Reader/Observer, Data-only 不复制 Attr, 关闭一个对象不影响另一个.
void observers() {

    Fixture fixture;
    fixture.password(42);
    fixture.fill();
    Client client(fixture.options());
    auto reader = client.value.reader({"routes", "main"});
    auto observer = client.value.observer({"service", "main"});
    CHECK(reader && observer && !client.value.observer({"service", "main"}, "invalid"));
    eventually([&] { return reader->load().state() == comet::Reader::State::ready && observer->select().state() == comet::Observer::State::ready; });
    CHECK(observer->select().size() == 0 && observer->select().version() == 0);
    const astra::Scope scope{"service", "main"};
    const auto attr = std::make_shared<const astra::Ephemeris::Buffer>(32, 0xff);
    const auto data = std::make_shared<const astra::Ephemeris::Buffer>(3, 1);
    auto created = fixture.ephemeris.create(scope, attr, data, 1000);
    CHECK(created);
    eventually([&] { return observer->select().find(created->uuid).has_value(); });
    const auto held = observer->select();
    CHECK(fixture.ephemeris.update(scope, created->uuid, std::make_shared<const astra::Ephemeris::Buffer>(), 1));
    eventually([&] { const auto value = observer->select().find(created->uuid); return value && value->data->empty(); });
    CHECK(observer->select().find(created->uuid)->attr == held.find(created->uuid)->attr);
    auto exact = client.value.observer({"service", "main"}, created->uuid);
    CHECK(exact);
    eventually([&] { return exact->select().state() == comet::Observer::State::ready; });
    CHECK(exact->select().size() == 1);
    fixture.now.store(2'000'000'000); // 服务端单独推进 TTL, SDK 不伪造到期删除.
    eventually([&] { return observer->select().size() == 0 && exact->select().size() == 0; });
    observer->close();
    CHECK(observer->wait(3s) && held.find(created->uuid)->data->size() == 3);
    CHECK(fixture.library.apply({"routes", "main"}, 2, "key-0", astra::Almanac::Buffer{7}));
    eventually([&] { return reader->load().version() == 2; });
}

// Ephemeris 的视图游标不跨 Star 比较, 切换后较低版本也必须安装新的完整成员列表.
void relocating() {

    Fixture first("star-first", false);
    Fixture second("star-second", false);
    const astra::Scope scope{"service", "main"};
    const auto value = std::make_shared<const astra::Ephemeris::Buffer>(1, 1);
    auto old = first.ephemeris.create(scope, value, value, 1000);
    auto replacement = second.ephemeris.create(scope, value, value, 1000);
    CHECK(old && replacement);
    for (std::uint64_t order = 1; order <= 8; ++order) {
        CHECK(first.ephemeris.update(scope, old->uuid, std::make_shared<const astra::Ephemeris::Buffer>(1, static_cast<std::uint8_t>(order + 1)), order));
    }
    auto options = first.options(false);
    options.endpoints.push_back(second.endpoint);
    Client client(std::move(options));
    auto observer = client.value.observer({"service", "main"});
    CHECK(observer);
    eventually([&] { return observer->select().version() == 9; });
    first.stop();
    eventually([&] { return observer->select().instance() == "star-second" && observer->select().state() == comet::Observer::State::ready; });
    CHECK(observer->select().version() == 1 && observer->select().find(replacement->uuid) && !observer->select().find(old->uuid));
}

// 改变本地下一次登录密码不主动踢掉当前已确认 Session, 服务端仍决定其有效期.
void prepared_secret() {

    Fixture fixture;
    fixture.password(42);
    fixture.fill();
    Client client(fixture.options());
    auto reader = client.value.reader({"routes", "main"});
    CHECK(reader);
    eventually([&] { return reader->load().state() == comet::Reader::State::ready; });
    CHECK(client.value.secret({43})); // 下一次认证材料还未在服务端上线.
    CHECK(fixture.library.apply({"routes", "main"}, 2, "key-0", astra::Almanac::Buffer{2}));
    eventually([&] { return reader->load().version() == 2; }); // 不能因本地新密码错误而破坏当前有效登录.
    fixture.password(43);
    CHECK(fixture.library.apply({"routes", "main"}, 3, "key-0", astra::Almanac::Buffer{3}));
    eventually([&] { return reader->load().state() == comet::Reader::State::ready && reader->load().version() == 3; });
}

// 真实 Create/Data/Renew/Remove 与 Observer 闭环, 原 UUID 结束后自动用最新 Data 重新注册.
void beacons() {

    Fixture fixture;
    fixture.password(42);
    Client client(fixture.options());
    auto observer = client.value.observer({"service", "main"});
    auto beacon = client.value.beacon({"service", "main"}, {4, 5}, {}, 1s, 250ms);
    CHECK(observer && beacon);
    eventually([&] { return beacon->state().phase == comet::Beacon::Phase::ready; });
    const auto old = *beacon->state().identity;
    eventually([&] { return observer->select().find(old.uuid).has_value(); });
    const auto receipt = beacon->update(std::vector<std::uint8_t>{7, 8});
    CHECK(receipt && receipt->identity == old && receipt->order == 1);
    eventually([&] { const auto value = observer->select().find(old.uuid); return value && *value->data == std::vector<std::uint8_t>({7, 8}); });
    eventually([&] {
        bool renewed{};
        fixture.ephemeris.source().each([&](const astra::Scope&, std::string_view uuid, const astra::Ephemeris::Record& record) { if (uuid == old.uuid) { renewed = record.renewal > 0; } });
        return renewed;
    });                                   // 观察真实原生 Renew order, 不能仅靠 SDK 自报 ready 证明自动续租.
    fixture.now.fetch_add(2'000'000'000); // 原 TTL 已过, 后续 Renew 明确 ended 后才允许同 ID 恢复.
    fixture.ephemeris.tick();
    eventually([&] {
        bool restored{};
        fixture.ephemeris.source().each([&](const astra::Scope&, std::string_view uuid, const astra::Ephemeris::Record& record) { if (uuid == old.uuid) { restored = record.generation > 1; } });
        return restored && beacon->state().phase == comet::Beacon::Phase::ready;
    });
    const auto current = *beacon->state().identity;
    CHECK(current.uuid == old.uuid);
    eventually([&] { const auto value = observer->select().find(current.uuid); return value && observer->select().size() == 1 && *value->attr == std::vector<std::uint8_t>({4, 5}) && *value->data == std::vector<std::uint8_t>({7, 8}); });
    beacon->close();
    CHECK(beacon->wait(3s));
    eventually([&] { return observer->select().size() == 0; });
    CHECK(fixture.ephemeris.source().size() == 0);
    auto closed = beacon->update(std::vector<std::uint8_t>{1});
    CHECK(!closed && closed.error().code == comet::Error::Code::closed);
}

// 首次注册失败即返回, 修复凭据后必须显式重建, 不保留待发业务内容.
void deferred() {

    Fixture fixture;
    fixture.password(43);
    Client client(fixture.options());
    auto denied = client.value.beacon({"service", "main"}, {}, {1}, 1s, 250ms);
    CHECK(!denied && fixture.ephemeris.source().size() == 0);
    CHECK(!client.value.beacon({"service", "main"}, {}, {}, 999ms, 250ms));
    CHECK(!client.value.beacon({"service", "main"}, {}, {}, 1s, 1s));
    CHECK(client.value.secret({43}));
    CHECK(fixture.ephemeris.source().size() == 0);
    auto beacon = client.value.beacon({"service", "main"}, {}, {2}, 1s, 250ms);
    CHECK(beacon && beacon->state().phase == comet::Beacon::Phase::ready);
    const auto identity = *beacon->state().identity;
    const auto installed = fixture.ephemeris.find({"service", "main"}, identity.uuid);
    CHECK(installed && installed->record && *installed->record->data == std::vector<std::uint8_t>({2}));
    const auto invalid = beacon->update(comet::Value{});
    CHECK(!invalid && invalid.error().code == comet::Error::Code::input);
}

// 关闭共享 Client 仍保留已确认 Session 供一次有限注销, 不把关闭门误用于内部清理.
void closing_beacon() {

    Fixture fixture;
    fixture.password(42);
    Client client(fixture.options());
    auto beacon = client.value.beacon({"service", "main"}, {}, {}, 1s, 250ms);
    CHECK(beacon);
    eventually([&] { return beacon->state().phase == comet::Beacon::Phase::ready; });
    CHECK(fixture.ephemeris.source().size() == 1);
    client.value.close();
    CHECK(client.value.wait(5s) && beacon->wait(0ms));
    CHECK(beacon->state().phase == comet::Beacon::Phase::closed && fixture.ephemeris.source().size() == 0); // 测试业务时钟未经过 TTL, 消失只能来自真实注销.
}

// 切换 Star 通过独立恢复能力保留逻辑 UUID, 不用 APIKEY 推断所有权.
void beacon_relocation() {

    Fixture first("star-first", false);
    Fixture second("star-second", false);
    auto options = first.options(false);
    options.endpoints.push_back(second.endpoint);
    Client client(std::move(options));
    auto beacon = client.value.beacon({"service", "main"}, {9}, {8}, 1s, 250ms);
    CHECK(beacon);
    eventually([&] { return beacon->state().phase == comet::Beacon::Phase::ready; });
    const auto old = *beacon->state().identity;
    first.stop();
    eventually([&] { const auto state = beacon->state(); return state.phase == comet::Beacon::Phase::ready && state.identity && state.identity->instance == "star-second"; });
    const auto current = *beacon->state().identity;
    CHECK(current.uuid == old.uuid && second.ephemeris.source().size() == 1);
}

// 同步发布内部管理版本, 完整值包含合法空正文, 订阅旧视图始终稳定.
void publications() {

    Fixture fixture;
    fixture.password(42);
    Client client(fixture.options());
    auto publisher = client.value.publisher({"dynamic", "main"});
    auto subscriber = client.value.subscriber({"dynamic", "main"});
    auto exact = client.value.subscriber({"dynamic", "main"}, "key");
    CHECK(publisher && subscriber && exact);
    eventually([&] { return subscriber->watch().state() == comet::Subscriber::State::ready; });
    CHECK(subscriber->watch().size() == 0 && fixture.catalog.source().position() == 0);
    const auto receipt = publisher->update("key", {7}, 1s); // 同步返回的是 Star 提交确认.
    CHECK(receipt && receipt->version == 1 && receipt->instance == "star-test");
    eventually([&] { const auto view = exact->watch(); return view.find("key") && view.find("key")->version == 1 && subscriber->watch().find("key").has_value(); });
    const auto old = subscriber->watch(); // 用户保留的不可变快照不能被后续 Data 修改.
    const auto newer = publisher->update("key", {}, 2s);
    CHECK(newer && newer->version == 2);
    eventually([&] { const auto view = subscriber->watch(); return view.find("key") && view.find("key")->version == 2; });
    CHECK(subscriber->watch().find("key")->value->empty() && old.find("key")->value->at(0) == 7);

    // 服务端业务时间到期后才删除, SDK 不按本地 TTL 删除或自动续租.
    fixture.now = 4'000'000'000;
    eventually([&] { return subscriber->watch().size() == 0 && exact->watch().size() == 0; });
    CHECK(fixture.catalog.source().size() == 1);
    auto restarted = client.value.publisher({"dynamic", "main"}); // 新对象仍查询到过期水位, 不重新从一写起.
    CHECK(restarted);
    const auto restored = restarted->update("key", {9}, 1s);
    CHECK(restored && restored->version == 3);
    publisher->close();
    CHECK(publisher->wait(3s));
}

// 一个共享客户端的独立写者不相互强加 Scope 全局版本; 活跃写者不阻塞空闲订阅.
void scheduling() {

    Fixture fixture("scheduling", false, false);
    Client client(fixture.options(false, false));
    std::vector<comet::Publisher> publishers;   // 每个对象只绑定范围, 业务保证所写 Key 唯一.
    std::vector<comet::Subscriber> subscribers; // 各 Key 独立的长期订阅.
    for (unsigned index = 0; index < 16; ++index) {
        const auto key = std::to_string(index); // 相邻 Key 允许拥有相同版本一.
        auto publisher = client.value.publisher({"dynamic", "schedule"});
        auto subscriber = client.value.subscriber({"dynamic", "schedule"}, key);
        CHECK(publisher && subscriber);
        publishers.push_back(std::move(*publisher));
        subscribers.push_back(std::move(*subscriber));
        const auto result = publishers.back().update(key, {7}, 1s);
        CHECK(result && result->version == 1);
    }
    for (std::uint64_t version = 2; version <= 64; ++version) {
        const auto result = publishers.front().update("0", {8}, 1s); // 同对象自动递增, 不向公共接口传 version.
        CHECK(result && result->version == version);
    }
    eventually([&] { return std::ranges::all_of(subscribers, [](const auto& subscriber) { return subscriber.watch().state() == comet::Subscriber::State::ready && subscriber.watch().size() == 1; }); });
    CHECK(fixture.catalog.capture({"dynamic", "schedule"})->version() == 79);
    client.value.close();
    CHECK(client.value.wait(3s));
    for (const auto& subscriber : subscribers)
        CHECK(subscriber.wait(0ms));
}

// 用户通知中关闭 Client 仍立即禁止新的写入, 不等待控制轮返回.
void publications_closed() {

    Fixture fixture("star-test", false);
    fixture.fill();
    Client client(fixture.options(false));
    auto publisher = client.value.publisher({"dynamic", "main"});
    CHECK(publisher && publisher->update("key", {7}, 1s));
    std::atomic_bool observed{}; // 控制轮结束后由主线程读取.
    comet::Reader::Options options;
    options.changed = [&](comet::Reader::View view) {
        if (view.state() == comet::Reader::State::ready && view.version() == 2) {
            client.value.close();
            const auto result = publisher->update("key", {8}, 1s); // 关闭优先于禁止回调阻塞的检查.
            observed.store(!result && result.error().code == comet::Error::Code::closed);
        }
    };
    auto reader = client.value.reader({"routes", "main"}, {}, std::move(options));
    CHECK(reader);
    eventually([&] { return reader->load().version() == 1; });
    CHECK(fixture.library.apply({"routes", "main"}, 2, "key-0", astra::Almanac::Buffer{8}));
    CHECK(client.value.wait(3s));
    CHECK(observed.load() && publisher->wait(0ms));
}

// 切换后不自动恢复旧 Catalog Data, 新的显式完整提交才创建目标缺少的 Key.
void publications_relocation() {

    Fixture first("star-first", false), second("star-second", false);
    auto options = first.options(false);
    options.endpoints.push_back(second.endpoint);
    Client client(std::move(options));
    auto publisher = client.value.publisher({"dynamic", "main"});
    auto subscriber = client.value.subscriber({"dynamic", "main"});
    CHECK(publisher && subscriber);
    const auto initial = publisher->update("key", {1}, 1s);
    CHECK(initial && initial->version == 1);
    eventually([&] { return subscriber->watch().find("key").has_value(); });
    first.stop();
    eventually([&] { const auto view = subscriber->watch(); return view.state() == comet::Subscriber::State::ready && view.instance() == "star-second"; });
    CHECK(!subscriber->watch().find("key") && second.catalog.source().position() == 0);
    const auto moved = publisher->update("key", {2}, 1s);
    CHECK(moved && moved->instance == "star-second" && moved->version > initial->version);
    eventually([&] { const auto row = subscriber->watch().find("key"); return row && row->version == moved->version; });
}

// 认证失败返回后不保留待发 Data; 修正凭据也不暗中提交上一次调用.
void publications_pending() {

    Fixture fixture;
    fixture.password(43);
    Client client(fixture.options());
    auto publisher = client.value.publisher({"dynamic", "main"});
    CHECK(publisher);
    const auto rejected = publisher->update("key", {9}, 1s, 500ms);
    CHECK(!rejected && rejected.error().effect == comet::Error::Effect::unapplied);
    CHECK(client.value.secret({43}));
    auto subscriber = client.value.subscriber({"dynamic", "main"});
    CHECK(subscriber);
    eventually([&] { return subscriber->watch().state() == comet::Subscriber::State::ready; });
    CHECK(subscriber->watch().size() == 0 && fixture.catalog.source().position() == 0);
    const auto explicit_update = publisher->update("key", {8}, 1s);
    CHECK(explicit_update && explicit_update->version == 1);
}

// 静态拒绝不触发 RPC, 不消耗版本; Key/TTL 在每次调用独立验证.
void publication_inputs() {

    Fixture fixture("inputs", false);
    Client client(fixture.options(false));
    auto publisher = client.value.publisher({"dynamic", "main"});
    CHECK(publisher);
    const auto data = std::make_shared<const std::vector<std::uint8_t>>(1, 7); // 两条条目故意引用同一不可变值.
    CHECK(publisher->update({{"a", data}, {"a", data}}, 1s).error().code == comet::Error::Code::input);
    CHECK(publisher->update({{"a", {}}}, 1s).error().code == comet::Error::Code::input);
    CHECK(publisher->update("a", {7}, 0ms).error().code == comet::Error::Code::input);
    CHECK(publisher->update("a", {7}, 1s, 0ms).error().code == comet::Error::Code::input);
    CHECK(fixture.catalog.source().position() == 0);
    const auto result = publisher->update("a", {7}, 1s);
    CHECK(result && result->version == 1);
}

// 回调挂接晚于同步也收到基线, 多键原子批完整交付, 精确 Key 区分不存在与合法空值.
void watching() {

    Fixture fixture("star-watch", false);
    std::atomic_uint maps{}, exacts{}, states{}, baselines{}; // Client 析构先等待所有捕获这些引用的通知结束.
    std::atomic_bool valid{true};
    Client client(fixture.options(false));
    auto subscriber = client.value.subscriber({"dynamic", "callbacks"});
    auto exact = client.value.subscriber({"dynamic", "callbacks"}, "a");
    auto publisher = client.value.publisher({"dynamic", "callbacks"});
    CHECK(subscriber && exact && publisher);
    eventually([&] { return subscriber->state().state() == comet::Subscriber::State::ready && exact->state().state() == comet::Subscriber::State::ready; });
    CHECK(subscriber->watch([&](comet::Subscriber::Map map) {
        if (map.size() != 0 && map.size() != 2) {
            valid = false;
        }
        ++maps;
    }));
    CHECK(exact->watch([&](std::string key, std::optional<comet::Value> value) {
        if (key != "a" || (value && !(*value)->empty())) {
            valid = false;
        }
        ++exacts;
    }));
    CHECK(subscriber->changed([&](comet::Subscriber::View) { ++states; }));
    eventually([&] { return maps.load() == 1 && exacts.load() == 1 && states.load() > 0; });
    const auto empty = std::make_shared<const std::vector<std::uint8_t>>();
    CHECK(publisher->update({{"a", empty}, {"b", empty}}, 1s));
    eventually([&] { return maps.load() >= 2 && exacts.load() >= 2; });
    const auto held = subscriber->state();
    CHECK(held.size() == 2 && held.find("a")->value->empty() && valid.load());
    subscriber->stop();
    CHECK(subscriber->wait(3s));
    CHECK(!subscriber->watch([](comet::Subscriber::Map) {}));
    CHECK(held.size() == 2);

    auto reader = client.value.reader({"routes", "callbacks"});
    CHECK(reader);
    CHECK(reader->watch([&](comet::Reader::Map map) { if (map.size() != 0) { valid = false; } ++baselines; }));
    eventually([&] { return baselines.load() == 1; });
    reader->stop();
    CHECK(reader->wait(3s) && valid.load());
}

// selector 在短锁外运行, 本地估计影响后续选择, 新权威值和 stop 都拒绝旧视图写入.
void choosing() {

    Fixture fixture("star-pool", false);
    Client client(fixture.options(false));
    auto beacon = client.value.beacon({"service", "pool"}, {1}, {2}, 1s, 250ms);
    auto observer = client.value.observer({"service", "pool"});
    CHECK(beacon && observer);
    const auto id = beacon->state().identity->uuid;
    eventually([&] { return observer->select().state() == comet::Observer::State::ready && observer->select().find(id); });
    const auto select = [&](const comet::Observer::Pool& pool) { CHECK(observer->select().find(id)); return pool.find(id); }; // selector 可以重入当前观察对象.
    auto selected = observer->one(select);
    CHECK(selected && *selected);
    auto old = **selected; // 同一权威值下独立持有的旧估计凭证.
    CHECK((*selected)->update(std::vector<std::uint8_t>{3}));
    CHECK(!old.update(std::vector<std::uint8_t>{4}));
    auto next = observer->one(select);
    CHECK(next && *next && *(*next)->record().data == std::vector<std::uint8_t>{3});
    CHECK(beacon->update(std::vector<std::uint8_t>{5}));
    eventually([&] { return *observer->select().find(id)->data == std::vector<std::uint8_t>{5}; });
    CHECK(!(*selected)->update(std::vector<std::uint8_t>{6}));
    auto fresh = observer->one(select);
    CHECK(fresh && *fresh && *(*fresh)->record().data == std::vector<std::uint8_t>{5});
    observer->stop();
    CHECK(!observer->one(select) && !(*fresh)->update(std::vector<std::uint8_t>{7}));
    CHECK(*old.record().data == std::vector<std::uint8_t>{2});
}

// 回调解除自己后, 最后一份捕获析构可以重入读取状态, 不得持 Watching 的状态锁.
void captures() {

    Fixture fixture("star-captures", false);
    std::atomic_uint released{}; // 在 Client 排空之前始终有效.
    Client client(fixture.options(false));
    auto opened = client.value.subscriber({"dynamic", "captures"});
    CHECK(opened);
    auto subscriber = std::make_shared<comet::Subscriber>(std::move(*opened));

    struct Capture {
        std::shared_ptr<comet::Subscriber> owner; // 保持可重入句柄到捕获释放后, 无栈借用.
        std::atomic_uint& released;

        ~Capture() {
            static_cast<void>(owner->state());
            ++released;
        }
    };

    CHECK(subscriber->watch([capture = std::make_shared<Capture>(subscriber, released)](comet::Subscriber::Map) {
        CHECK(capture->owner->watch(std::move_only_function<void(comet::Subscriber::Map)>{}));
    }));
    eventually([&] { return released.load() == 1; });
    CHECK(subscriber->changed([capture = std::make_shared<Capture>(subscriber, released)](comet::Subscriber::View) {
        CHECK(capture->owner->changed({}));
    }));
    eventually([&] { return released.load() == 2; });
    subscriber->stop();
    CHECK(subscriber->wait(3s) && client.value.exceptions() == 0);
}

} // namespace

// 仅注册为显式 CTest 项, 编写和格式化不运行网络用例.
int main() {
    try {
        captures();
        watching();
        choosing();
        inputs();
        cleanup();
        queued();
        shared();
        authentication();
        prepared_secret();
        observers();
        beacons();
        publications();
        scheduling();
        publications_closed();
        publications_relocation();
        publications_pending();
        publication_inputs();
        deferred();
        closing_beacon();
        beacon_relocation();
        relocating();
        failover();
        callbacks();
        lifetime();
        std::cout << "comet client tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
