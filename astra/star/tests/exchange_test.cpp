#include "check.hpp"
#include "exchange.hpp"
#include <iostream>

namespace {
using namespace std::chrono_literals;
using astra::Catalog;
using astra::Clock;
using astra::Ephemeris;
using astra::Exchange;
using astra::Scope;

// 不透明正文只使用实际原生 Buffer, 不建立另一套假业务存储.
Catalog::Value bytes(std::string_view value) {
    return std::make_shared<const Catalog::Buffer>(value.begin(), value.end());
}

// 两个真实域共享同一可控时钟, 不启动网络/进程; 不把本夹具当作真实 TLS 回归.
struct Node {
    std::chrono::milliseconds now{1000}; // 可分别模拟两个 Star 的当前时间与源端截止差异.
    Catalog::State catalog;              // 真实来源、水位、投影提交器.
    Ephemeris::State ephemeris;          // 真实 UUID/Attr/Data/TTL 提交器.
    Exchange::Budget budget;             // 此节点所有逻辑流的恢复额度.

    Node(Catalog::State::Limits catalog_limits = {}, Ephemeris::State::Limits ephemeris_limits = {}) : catalog([this] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, catalog_limits), ephemeris([this] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, ephemeris_limits) {} // 初始时钟有效.

    // 同连接身份重新准入幂等, 建立新逻辑流不重置已经安装的连续来源位置.
    std::unique_ptr<Exchange> connect(std::string peer, astra::Steady::time_point time) {
        CHECK(catalog.admit(peer) && ephemeris.admit(peer));
        return std::make_unique<Exchange>(catalog, ephemeris, std::move(peer), budget, 8 * 1024 * 1024, time);
    }
};

// 对接真正的 SessionPacket, 每次 take 模拟 StartWrite 交接, receive 再执行完整来源恢复.
bool transfer(Exchange& from, Exchange& to, astra::Steady::time_point time) {
    const auto prepared = from.prepare(time);
    CHECK(prepared);
    if (!*prepared) {
        return false;
    }
    auto packet = from.take();
    CHECK(packet.ByteSizeLong() <= 8 * 1024 * 1024);
    CHECK(to.receive(packet, time));
    return true;
}

// 有界循环防止错误 ACK-of-ACK 或空包风暴被夹具无限吞掉.
void drain(Exchange& left, Exchange& right, astra::Steady::time_point time) {
    for (unsigned iteration = 0; iteration != 2048; ++iteration) {
        const bool first = transfer(left, right, time);
        const bool second = transfer(right, left, time);
        if (!first && !second) {
            CHECK(left.ready() && right.ready());
            return;
        }
    }
    throw std::runtime_error("Peer data did not settle within bounded transfers");
}

// 空源明确完成初始化, 自有提交双向传播, 纯续期不产生下游内容变化或副本再广播.
void bidirectional() {

    Node left, right;
    const auto time = astra::Steady::now();
    const Scope scope{"scope", "main"};
    {
        auto a = left.connect("b", time), b = right.connect("a", time);
        drain(*a, *b, time);
        CHECK(right.catalog.publish(scope, "right", bytes("r"), 1, 10000));
        CHECK(left.catalog.publish(scope, "left", bytes("l"), 1, 10000));
        const auto registration = left.ephemeris.create(scope, bytes("attr"), bytes("data"), 10000);
        CHECK(registration);
        drain(*a, *b, time);
        CHECK(left.catalog.capture(scope)->size() == 2 && right.catalog.capture(scope)->size() == 2);
        CHECK(right.ephemeris.find(scope, registration->uuid)->record);
        CHECK(left.catalog.source().position() == 1 && right.catalog.source().position() == 1 && right.ephemeris.source().position() == 0);
        const auto catalog_version = right.catalog.capture(scope)->version();
        const auto ephemeris_version = right.ephemeris.capture(scope)->version();
        left.now += 100ms;
        CHECK(left.catalog.renew(scope, "left", 1, 10000));
        CHECK(left.ephemeris.renew(scope, registration->uuid, 1));
        drain(*a, *b, time + 100ms);
        CHECK(right.catalog.capture(scope)->version() == catalog_version && right.ephemeris.capture(scope)->version() == ephemeris_version);
        CHECK(left.ephemeris.update(scope, registration->uuid, bytes("changed"), 1));
        drain(*a, *b, time + 200ms);
        CHECK(*right.ephemeris.find(scope, registration->uuid)->record->data == *bytes("changed"));
        CHECK(left.ephemeris.remove(scope, registration->uuid));
        drain(*a, *b, time + 300ms);
        CHECK(!right.ephemeris.find(scope, registration->uuid)->record);
    }
    CHECK(left.budget.used == 0 && right.budget.used == 0);
}

// 两个域在副本本地 TTL 后收到仅期限/Data, 必须通过同流回补完整行再确认, 不跳过其他 Scope.
void repair() {

    Node left, right;
    const auto time = astra::Steady::now();
    const Scope one{"one", "main"}, two{"two", "main"};
    CHECK(left.catalog.publish(one, "key", bytes("body"), 4, 1000));
    const auto registration = left.ephemeris.create(one, bytes("attr"), bytes("data"), 1000);
    CHECK(registration);
    auto a = left.connect("b", time), b = right.connect("a", time);
    drain(*a, *b, time);
    right.now = 2100ms;
    right.catalog.tick();
    right.ephemeris.tick();
    CHECK(!right.catalog.find(one, "key")->record && !right.ephemeris.find(one, registration->uuid)->record);
    left.now = 1500ms;
    CHECK(left.catalog.renew(one, "key", 4, 5000));
    CHECK(left.catalog.publish(two, "other", bytes("other"), 1, 5000));
    CHECK(left.ephemeris.update(one, registration->uuid, bytes("new"), 1));
    CHECK(left.ephemeris.renew(one, registration->uuid, 1));
    const auto other = left.ephemeris.create(two, bytes("attr"), bytes("other"), 5000);
    CHECK(other);
    drain(*a, *b, time + 1s);
    CHECK(right.catalog.find(one, "key")->record->version == 4 && right.catalog.find(two, "other")->record);
    CHECK(right.catalog.received("a") == left.catalog.source().position());
    CHECK(*right.ephemeris.find(one, registration->uuid)->record->attr == *bytes("attr"));
    CHECK(*right.ephemeris.find(one, registration->uuid)->record->data == *bytes("new"));
    CHECK(right.ephemeris.find(two, other->uuid)->record && right.ephemeris.received("a") == left.ephemeris.source().position());
}

// 零历史强制完整来源恢复, 新流从安装位置续接, 且空来源基线只清本来源 Ephemeris.
void snapshot() {

    Catalog::State::Limits catalog_limits;
    Ephemeris::State::Limits ephemeris_limits;
    catalog_limits.source.history = ephemeris_limits.source.history = 0;
    Node left(catalog_limits, ephemeris_limits), right;
    const auto time = astra::Steady::now();
    const Scope scope{"source", "main"};
    for (unsigned index = 0; index != 300; ++index) {
        CHECK(left.catalog.publish(scope, std::to_string(index), bytes("v"), index + 1, 10000));
    }
    const auto registration = left.ephemeris.create(scope, bytes("attr"), bytes("remote"), 10000);
    const auto own = right.ephemeris.create(scope, bytes("own"), bytes("own"), 10000);
    CHECK(registration && own);
    {
        auto a = left.connect("b", time), b = right.connect("a", time);
        drain(*a, *b, time);
        CHECK(right.catalog.capture(scope)->size() == 300 && right.ephemeris.capture(scope)->size() == 2);
    }
    CHECK(left.budget.used == 0 && right.budget.used == 0);
    CHECK(left.ephemeris.remove(scope, registration->uuid));
    CHECK(left.catalog.publish(scope, "new", bytes("new"), 1, 10000));
    {
        auto a = left.connect("b", time + 1s), b = right.connect("a", time + 1s);
        drain(*a, *b, time + 1s);
        CHECK(right.catalog.capture(scope)->size() == 301);
        CHECK(!right.ephemeris.find(scope, registration->uuid)->record && right.ephemeris.find(scope, own->uuid)->record);
    }
    CHECK(left.budget.used == 0 && right.budget.used == 0);
}

// 整包先验证阻止合法前项在结构错误后项前被偷偷应用; 资源拒绝不安装半份基线.
void rejection() {

    Node node;
    const auto time = astra::Steady::now();
    const Scope scope{"one", "main"};
    {
        auto receiver = node.connect("source", time);
        Exchange::Packet packet;
        auto* changes = packet.mutable_catalog_changes();
        changes->set_head(2);
        auto* first = changes->add_entries();
        first->set_position(1);
        astra::Parcel::scope(*first->mutable_scope(), scope);
        first->set_key("key");
        astra::Parcel::record(*first->mutable_record(), Catalog::Record{1, bytes("v"), Clock::Time(10s)});
        changes->add_entries()->CopyFrom(*first);
        changes->mutable_entries(1)->set_position(2);
        changes->mutable_entries(1)->clear_action();
        CHECK(!receiver->receive(packet, time));
        CHECK(node.catalog.received("source") == 0 && !node.catalog.find(scope, "key")->record);
        node.budget.maximum = 1024;
        packet.Clear();
        packet.mutable_catalog_snapshot()->set_position(1);
        packet.mutable_catalog_snapshot()->set_complete(true);
        const auto rejected = receiver->receive(packet, time);
        CHECK(!rejected && rejected.error().code == astra::Status::Code::capacity && node.catalog.received("source") == 0);
    }
    CHECK(node.budget.used == 0);
    {
        auto receiver = node.connect("source", time);
        Exchange::Packet repair; // 对端持续请求回补不能使本端未完成同步越过自己的截止.
        auto* request = repair.mutable_repair();
        request->set_domain(proto::astra::v1::DOMAIN_CATALOG);
        astra::Parcel::scope(*request->mutable_scope(), scope);
        request->set_key("absent");
        request->set_trigger(1);
        request->set_version(1);
        CHECK(node.catalog.publish(scope, "other", bytes("v"), 1, 10000));
        CHECK(receiver->receive(repair, time));
        const auto timeout = receiver->prepare(time + 31s);
        CHECK(!timeout && timeout.error().code == astra::Status::Code::timeout);
    }
    CHECK(node.budget.used == 0);
}
} // namespace

// 来源消息闭环组件用例, 不建立实际 gRPC 连接, 网络故障仍须由独立进程测试验证.
int main() {

    try {
        bidirectional();
        repair();
        snapshot();
        rejection();
        std::cout << "peer exchanges: ok\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
