#include "catalog_service.hpp"
#include "check.hpp"
#include "core.hpp"
#include "ephemeris_service.hpp"
#include "identity.hpp"
#include "readout.hpp"
#include <atomic>
#include <comet/client.hpp>
#include <condition_variable>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <thread>

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
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        auto core = comet::detail::Core::prepare(fixture.options(false, false)); // 只借助私有测试入口观察所有权, 不扩大 SDK 公共 API.
        CHECK(core);
        const std::weak_ptr<comet::detail::Core> retained = *core; // 弱引用不阻止真实析构, 同时覆盖立即 close 与首轮回调竞争.
        (*core)->start();
        (*core)->close();
        CHECK((*core)->wait(3s));
        core->reset();
        eventually([&] { return retained.expired(); });
    }
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
    auto beacon = client.value.beacon({"service", "main"}, {4, 5}, {}, 1s);
    CHECK(observer && beacon);
    eventually([&] { return beacon->state().phase == comet::Beacon::Phase::ready; });
    const auto old = *beacon->state().identity;
    eventually([&] { return observer->select().find(old.uuid).has_value(); });
    auto result = beacon->update(std::vector<std::uint8_t>{7, 8});
    CHECK(result.wait_for(5s) == std::future_status::ready);
    const auto receipt = result.get();
    CHECK(receipt && receipt->identity == old && receipt->order == 1);
    eventually([&] { const auto value = observer->select().find(old.uuid); return value && *value->data == std::vector<std::uint8_t>({7, 8}); });
    eventually([&] {
        bool renewed{};
        fixture.ephemeris.source().each([&](const astra::Scope&, std::string_view uuid, const astra::Ephemeris::Record& record) { if (uuid == old.uuid) { renewed = record.renewal > 0; } });
        return renewed;
    }); // 观察真实原生 Renew order, 不能仅靠 SDK 自报 ready 证明自动续租.
    CHECK(fixture.ephemeris.remove({"service", "main"}, old.uuid));
    eventually([&] { const auto state = beacon->state(); return state.phase == comet::Beacon::Phase::ready && state.identity && state.identity->uuid != old.uuid; });
    const auto current = *beacon->state().identity;
    eventually([&] { const auto value = observer->select().find(current.uuid); return value && !observer->select().find(old.uuid) && *value->attr == std::vector<std::uint8_t>({4, 5}) && *value->data == std::vector<std::uint8_t>({7, 8}); });
    beacon->close();
    CHECK(beacon->wait(3s));
    eventually([&] { return observer->select().size() == 0; });
    CHECK(fixture.ephemeris.source().size() == 0);
    auto closed = beacon->update(std::vector<std::uint8_t>{1});
    CHECK(closed.wait_for(0ms) == std::future_status::ready && !closed.get());
}

// 未登录时合并/超时只结算单次 Update, 最新期望在之后新注册成功时仍必须生效.
void deferred() {

    Fixture fixture;
    fixture.password(43);
    Client client(fixture.options());
    auto beacon = client.value.beacon({"service", "main"}, {}, {1}, 1s);
    CHECK(beacon && !client.value.beacon({"service", "main"}, {}, {}, 999ms));
    eventually([&] { return beacon->state().phase == comet::Beacon::Phase::failed; });
    auto replaced = beacon->update(std::vector<std::uint8_t>{2}, 1s);
    auto timed = beacon->update(std::vector<std::uint8_t>{3}, 30ms);
    CHECK(replaced.wait_for(1s) == std::future_status::ready);
    const auto obsolete = replaced.get();
    CHECK(!obsolete && obsolete.error().code == comet::Error::Code::obsolete && obsolete.error().effect == comet::Error::Effect::unapplied);
    CHECK(timed.wait_for(1s) == std::future_status::ready);
    const auto timeout = timed.get();
    CHECK(!timeout && timeout.error().code == comet::Error::Code::timeout && timeout.error().effect == comet::Error::Effect::unapplied);
    CHECK(client.value.secret({43}));
    eventually([&] { return beacon->state().phase == comet::Beacon::Phase::ready; });
    const auto identity = *beacon->state().identity;
    const auto installed = fixture.ephemeris.find({"service", "main"}, identity.uuid);
    CHECK(installed && installed->record && *installed->record->data == std::vector<std::uint8_t>({3}));
    auto invalid = beacon->update(comet::Value{});
    CHECK(invalid.wait_for(0ms) == std::future_status::ready && !invalid.get());
}

// 关闭共享 Client 仍保留已确认 Session 供一次有限注销, 不把关闭门误用于内部清理.
void closing_beacon() {

    Fixture fixture;
    fixture.password(42);
    Client client(fixture.options());
    auto beacon = client.value.beacon({"service", "main"}, {}, {}, 1s);
    CHECK(beacon);
    eventually([&] { return beacon->state().phase == comet::Beacon::Phase::ready; });
    CHECK(fixture.ephemeris.source().size() == 1);
    client.value.close();
    CHECK(client.value.wait(5s) && beacon->wait(0ms));
    CHECK(beacon->state().phase == comet::Beacon::Phase::closed && fixture.ephemeris.source().size() == 0); // 测试业务时钟未经过 TTL, 消失只能来自真实注销.
}

// 切换 Star 必须使用新 UUID, 同一自动对象不携带旧身份向新 Star 强写.
void beacon_relocation() {

    Fixture first("star-first", false);
    Fixture second("star-second", false);
    auto options = first.options(false);
    options.endpoints.push_back(second.endpoint);
    Client client(std::move(options));
    auto beacon = client.value.beacon({"service", "main"}, {9}, {8}, 1s);
    CHECK(beacon);
    eventually([&] { return beacon->state().phase == comet::Beacon::Phase::ready; });
    const auto old = *beacon->state().identity;
    first.stop();
    eventually([&] { const auto state = beacon->state(); return state.phase == comet::Beacon::Phase::ready && state.identity && state.identity->instance == "star-second"; });
    const auto current = *beacon->state().identity;
    CHECK(current.uuid != old.uuid && second.ephemeris.source().size() == 1);
}

// 原生 Publisher/Subscriber 与 Reader/Observer 复用同一登录, 续租不制造内容变化.
void publications() {

    Fixture fixture;
    fixture.password(42);
    Client client(fixture.options());
    auto publisher = client.value.publisher({"dynamic", "main"}, "key", 1s);
    auto subscriber = client.value.subscriber({"dynamic", "main"});
    auto exact = client.value.subscriber({"dynamic", "main"}, "key");
    CHECK(publisher && subscriber && exact);
    eventually([&] { return subscriber->watch().state() == comet::Subscriber::State::ready; });
    CHECK(subscriber->watch().size() == 0 && fixture.catalog.source().position() == 0); // 空 Publisher 不发布空记录.
    auto initial = publisher->publish(10, std::vector<std::uint8_t>{7});
    CHECK(initial.wait_for(3s) == std::future_status::ready);
    const auto receipt = initial.get();
    CHECK(receipt && receipt->version == 10 && receipt->instance == "star-test");
    eventually([&] { const auto view = exact->watch(); return view.find("key") && view.find("key")->version == 10 && subscriber->watch().find("key").has_value(); });
    const auto old = subscriber->watch();
    eventually([&] { return fixture.catalog.source().position() >= 2; }); // 真正的自动 Renew 已到服务端.
    CHECK(fixture.catalog.capture({"dynamic", "main"})->version() == 1);
    auto conflict = publisher->publish(10, std::vector<std::uint8_t>{8});
    auto lower = publisher->publish(9, std::vector<std::uint8_t>{7});
    CHECK(conflict.wait_for(0ms) == std::future_status::ready && lower.wait_for(0ms) == std::future_status::ready);
    CHECK(conflict.get().error().code == comet::Error::Code::conflict);
    CHECK(lower.get().error().code == comet::Error::Code::version);
    auto newer = publisher->publish(100, std::vector<std::uint8_t>{});
    CHECK(newer.wait_for(3s) == std::future_status::ready && newer.get());
    eventually([&] { const auto view = subscriber->watch(); return view.find("key") && view.find("key")->version == 100; });
    CHECK(subscriber->watch().find("key")->value->empty() && old.find("key")->value->at(0) == 7);
    publisher->close();
    CHECK(publisher->wait(3s));
    CHECK(fixture.catalog.find({"dynamic", "main"}, "key")->record); // close 不伪造 Catalog Delete.
    fixture.now = 4'000'000'000;
    eventually([&] { return subscriber->watch().size() == 0 && exact->watch().size() == 0; });
    CHECK(fixture.catalog.source().size() == 1); // 正文过期但防回退水位还在.
}

// Client 显式关闭立即覆盖全部对象的公开状态, 不等控制轮在用户回调返回后再次推进 Publisher.
void publications_closed() {

    Fixture fixture("star-test", false);
    fixture.fill();
    Client client(fixture.options(false));
    auto publisher = client.value.publisher({"dynamic", "main"}, "key", 1s);
    CHECK(publisher);
    auto committed = publisher->publish(1, std::vector<std::uint8_t>{7}); // 先取得真实确认, 关闭前必须是 ready.
    CHECK(committed.wait_for(3s) == std::future_status::ready && committed.get());
    eventually([&] { return publisher->state().phase == comet::Publisher::Phase::ready; });

    std::atomic_bool observed{}; // 回调记录即时状态, 主线程在全部清理完成后检查.
    comet::Reader::Options options;
    options.changed = [&](comet::Reader::View view) {
        if (view.state() == comet::Reader::State::ready && view.version() == 2) {
            client.value.close();
            observed.store(publisher->state().phase == comet::Publisher::Phase::closed);
        }
    };
    auto reader = client.value.reader({"routes", "main"}, {}, std::move(options));
    CHECK(reader);
    eventually([&] { return reader->load().version() == 1; });
    CHECK(fixture.library.apply({"routes", "main"}, 2, "key-0", astra::Almanac::Buffer{8}));
    CHECK(client.value.wait(3s)); // 回调内关闭, 不靠 sleep 或主线程抢先读状态制造竞态.
    CHECK(observed.load() && publisher->wait(0ms));
}

// 同一对象换 Star 完整重发最新内容; Subscriber 接受新节点较低游标, 不拼接旧节点的记录.
void publications_relocation() {

    Fixture first("star-first", false);
    Fixture second("star-second", false);
    auto options = first.options(false);
    options.endpoints.push_back(second.endpoint);
    Client client(std::move(options));
    auto publisher = client.value.publisher({"dynamic", "main"}, "key", 1s);
    auto subscriber = client.value.subscriber({"dynamic", "main"});
    CHECK(publisher && subscriber);
    auto future = publisher->publish(8, std::vector<std::uint8_t>{1});
    CHECK(future.wait_for(3s) == std::future_status::ready && future.get());
    eventually([&] { return subscriber->watch().find("key").has_value(); });
    first.stop();
    eventually([&] { const auto state = publisher->state(); return state.phase == comet::Publisher::Phase::ready && state.confirmed && state.confirmed->instance == "star-second"; });
    eventually([&] { const auto view = subscriber->watch(); return view.state() == comet::Subscriber::State::ready && view.instance() == "star-second" && view.find("key").has_value(); });
    CHECK(second.catalog.find({"dynamic", "main"}, "key")->record->version == 8);
}

// 本地接纳保持版本单调, 尚未发送的调用只保留一个结果, 超时不撤销恢复期望.
void publications_pending() {

    Fixture fixture;
    fixture.password(43);
    Client client(fixture.options());
    auto publisher = client.value.publisher({"dynamic", "main"}, "key", 1s);
    CHECK(publisher);
    auto first = publisher->publish(1, std::vector<std::uint8_t>{1});
    auto latest = publisher->publish(9, std::vector<std::uint8_t>{9}, 30ms);
    CHECK(first.wait_for(1s) == std::future_status::ready && first.get().error().code == comet::Error::Code::obsolete);
    CHECK(latest.wait_for(1s) == std::future_status::ready);
    const auto timeout = latest.get();
    CHECK(!timeout && timeout.error().code == comet::Error::Code::timeout && timeout.error().effect == comet::Error::Effect::unapplied);
    CHECK(client.value.secret({43}));
    eventually([&] { return publisher->state().phase == comet::Publisher::Phase::ready; });
    CHECK(fixture.catalog.find({"dynamic", "main"}, "key")->record->version == 9);
}

// Client 的显式结果总额度包含待认证调用; 满额度仍可替换自己一个未发送的期望.
void pending_limit() {

    Fixture fixture;
    fixture.password(43); // 登录被明确拒绝, 全部调用保持未发送, 不依赖 RPC 调度竞态.
    Client client(fixture.options());
    std::vector<comet::Publisher> publishers;
    std::vector<std::future<comet::Result<comet::Publisher::Receipt>>> futures;
    publishers.reserve(257);
    futures.reserve(256);
    for (unsigned index = 0; index < 257; ++index) {
        auto publisher = client.value.publisher({"capacity", "main"}, "key-" + std::to_string(index), 1s);
        CHECK(publisher);
        publishers.push_back(std::move(*publisher));
        if (index < 256) {
            futures.push_back(publishers.back().publish(1, std::vector<std::uint8_t>{1}, 60s));
        }
    }
    auto rejected = publishers.back().publish(1, std::vector<std::uint8_t>{1});
    CHECK(rejected.wait_for(0ms) == std::future_status::ready && rejected.get().error().code == comet::Error::Code::busy);
    auto replacement = publishers.front().publish(2, std::vector<std::uint8_t>{2}, 60s);
    CHECK(futures.front().wait_for(0ms) == std::future_status::ready && futures.front().get().error().code == comet::Error::Code::obsolete);
    CHECK(replacement.wait_for(0ms) != std::future_status::ready);
    publishers.front().close(); // 完成未发调用, 额度立即归还, 不等待后台扫描.
    CHECK(replacement.wait_for(0ms) == std::future_status::ready && replacement.get().error().code == comet::Error::Code::closed);
    auto admitted = publishers.back().publish(1, std::vector<std::uint8_t>{1}, 60s);
    CHECK(admitted.wait_for(0ms) != std::future_status::ready);
    client.value.close();
    CHECK(client.value.wait(5s));
    CHECK(admitted.wait_for(0ms) == std::future_status::ready && admitted.get().error().code == comet::Error::Code::closed);
}

} // namespace

// 仅注册为显式 CTest 项, 编写和格式化不运行网络用例.
int main() {
    try {
        inputs();
        cleanup();
        shared();
        authentication();
        prepared_secret();
        observers();
        beacons();
        publications();
        publications_closed();
        publications_relocation();
        publications_pending();
        pending_limit();
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
