#include "topology.hpp"
#include "upstream.hpp"
#include <verdandi/peer/config.hpp>

#include <atomic>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>

using namespace verdandi::peer;
#define CHECK(condition)                                                                                                                                       \
    do {                                                                                                                                                       \
        if (!(condition))                                                                                                                                      \
            throw std::runtime_error("Check failed at line " + std::to_string(__LINE__) + ": " #condition);                                                    \
    } while (false)

Member member(unsigned n, Role role = Role::star, std::uint64_t epoch = 1) {
    Member result;
    result.cluster = "alpha";
    result.group = "default";
    result.id.bytes[0] = static_cast<std::uint8_t>(n);
    result.id.bytes[6] = 0x40;
    result.id.bytes[8] = 0x80;
    result.principal.bytes[0] = static_cast<std::uint8_t>(n);
    result.address = *Endpoint::parse("127.0.0.1:" + std::to_string(7000 + n));
    result.epoch = MemberEpoch{epoch};
    result.role = role;
    return result;
}

void configuration() {
    const std::array<std::string_view, 3> args{"--cluster=alpha", "--listen=127.0.0.1:7443", "--super=localhost:7444"};
    auto parsed = parse_options(args, Role::star);
    CHECK(parsed);
    CHECK(parsed->max_peers == 64);
    CHECK(parsed->max_inbound == 128);
    CHECK(parsed->heartbeat_interval == Milliseconds(10000));
    CHECK(parsed->identity == "identity");
    CHECK(option_help(Role::planet).contains("Usage: planet"));
    CHECK(option_help(Role::star).contains("default=5000"));
    for (const auto bad : {"--max-peers=0", "--max-peers=4097", "--max-peers=1x", "--max-peers=18446744073709551616", "--max-peers=-1", "--cluster=beta",
                           "--worker-threads=2", "--role=star", "--identity=", "--pong-timeout-ms=0", "--shutdown-timeout-seconds=61"}) {
        std::vector<std::string_view> values(args.begin(), args.end());
        values.push_back(bad);
        CHECK(!parse_options(values, Role::star));
    }
    const std::array<std::string_view, 8> separated{"--cluster", "alpha",          "--listen",    "0.0.0.0:7443",
                                                    "--super",   "localhost:7444", "--advertise", "127.0.0.1:7443"};
    CHECK(parse_options(separated, Role::planet));
    CHECK(!parse_options(std::span(separated).first<6>(), Role::planet));
    CHECK(!parse_options({}, Role::star));
    CHECK(!Endpoint::parse("127.000.0.1:7443"));
    CHECK(!Endpoint::parse("[::ffff:127.0.0.1]:7443"));
    CHECK(!Endpoint::parse("[fe80::1%2]:7443"));
    CHECK(!Endpoint::parse("224.0.0.1:7443"));
    CHECK(!Endpoint::parse("0.0.0.0:7443"));
    CHECK(!supervisor_address("0.0.0.0:7443"));
    CHECK(!supervisor_address("224.0.0.1:7443"));
    CHECK(Endpoint::parse("0.0.0.0:0", true));
    CHECK(Endpoint::parse("[2001:0DB8:0:0:0:0:0:1]:7443")->text() == "[2001:db8::1]:7443");
    CHECK(!PeerId::parse("short"));
    CHECK(valid_uuid(member(1).id));
    CHECK(!valid_uuid(PeerId{}));
    CHECK(!valid_name("alpha/beta"));
    CHECK(!valid_name(std::string(65, 'a')));
}

void stars() {
    Config config;
    config.cluster = "alpha";
    config.max_peers = 3;
    StarTopology topology(config);
    const auto a = member(1), b = member(2);
    std::array list{a, b};
    CHECK(topology.initialize(a, list));
    CHECK(topology.status().members == 2);
    CHECK(!topology.initialize(a, list));
    auto targets = topology.due(Clock::now(), 1);
    CHECK(targets.size() == 1);
    CHECK(topology.due(Clock::now(), 1).empty());
    CHECK(topology.accept(b, Direction::outbound, {1}, targets[0]));
    CHECK(topology.accept(b, Direction::inbound, {2}, std::nullopt));
    CHECK(!topology.accept(b, Direction::inbound, {3}, std::nullopt));
    auto next = b;
    next.epoch = MemberEpoch{2};
    next.id.bytes[1] = 1;
    auto replacement = topology.accept(next, Direction::inbound, {4}, std::nullopt);
    CHECK(replacement && replacement->size() == 2);
    topology.closed({2}, {}, Clock::now());
    CHECK(topology.status().inbound == 1);
    CHECK(!topology.accept(b, Direction::outbound, {5}, targets[0]));
    CHECK(topology.status().members == 2);
    CHECK(!topology.accept(a, Direction::inbound, {6}, std::nullopt));
    const auto planet = member(3, Role::planet);
    CHECK(topology.accept(planet, Direction::inbound, {7}, std::nullopt));
    CHECK(topology.status().members == 2 && topology.status().planet_inbound == 1);
    CHECK(!topology.accept(planet, Direction::outbound, {8}, std::nullopt));
    auto conflict = next;
    conflict.group = "changed";
    CHECK(!topology.accept(conflict, Direction::outbound, {9}, std::nullopt));
}

void planets() {
    Config config;
    config.cluster = "alpha";
    config.role = Role::planet;
    PlanetUpstream upstream(config);
    const auto local = member(9, Role::planet), a = member(1);
    auto b = member(2);
    b.group = "remote";
    std::array list{a, b};
    CHECK(upstream.initialize(local, list));
    const auto now = Clock::now();
    auto first = upstream.due(now, 4);
    CHECK(first.size() == 1 && first[0].member == a);
    CHECK(upstream.due(now, 4).empty());
    upstream.failed(first[0], ErrorCode::transport, now);
    auto fallback = upstream.due(now, 4);
    CHECK(fallback.size() == 1 && fallback[0].member == b);
    auto restarted = b;
    restarted.epoch = MemberEpoch{2};
    restarted.id.bytes[1] = 1;
    CHECK(upstream.accept(restarted, Direction::outbound, {10}, fallback[0]));
    CHECK(upstream.status().upstream);
    CHECK(upstream.due(now, 4).empty());
    CHECK(!upstream.needs_refresh(now));
    upstream.closed({9}, {}, now);
    CHECK(upstream.status().upstream);
    upstream.closed({10}, ErrorCode::transport, now);
    CHECK(!upstream.status().upstream);
    CHECK(upstream.initialize(local, list));
    auto again = upstream.due(now + std::chrono::seconds(6), 1);
    CHECK(again.size() == 1);
    upstream.failed(again[0], ErrorCode::identity, now);
    CHECK(upstream.initialize(local, list));
    auto other = upstream.due(now + std::chrono::seconds(6), 1);
    CHECK(other.size() == 1 && other[0].member.principal != again[0].member.principal);
    CHECK(other[0].member.epoch.value == 2);
    CHECK(!upstream.accept(a, Direction::inbound, {11}, other[0]));
}

void atomic_lists_and_capacity() {
    Config config;
    config.cluster = "alpha";
    config.max_peers = 2;
    const auto a = member(1), b = member(2);
    StarTopology star(config);
    auto alias = b;
    alias.address = a.address;
    std::array invalid{a, alias};
    CHECK(!star.initialize(a, invalid));
    CHECK(!star.status().initialized && star.status().members == 0);
    std::array valid{a, b};
    CHECK(star.initialize(a, valid));
    CHECK(!star.accept(member(3), Direction::inbound, {1}, std::nullopt));
    // Star 和 Planet 分别计数, 一个角色满额不能污染另一个角色或原有索引.
    CHECK(star.accept(member(3, Role::planet), Direction::inbound, {2}, std::nullopt));
    CHECK(star.accept(member(4, Role::planet), Direction::inbound, {3}, std::nullopt));
    CHECK(!star.accept(member(5, Role::planet), Direction::inbound, {4}, std::nullopt));
    CHECK(star.status().members == 2 && star.status().planet_inbound == 2);
    alias = member(6, Role::planet);
    alias.address = b.address;
    CHECK(!star.accept(alias, Direction::inbound, {5}, std::nullopt));
    CHECK(star.status().members == 2 && star.status().planet_inbound == 2);

    config.role = Role::planet;
    PlanetUpstream planet(config);
    const auto local = member(20, Role::planet);
    std::vector<Member> oversized;
    for (unsigned i = 1; i <= 9; ++i) {
        oversized.push_back(member(i));
    }
    CHECK(!planet.initialize(local, oversized));
    CHECK(!planet.status().initialized && planet.status().candidates == 0);
    CHECK(planet.initialize(local, valid));
    auto unsorted = valid;
    std::swap(unsorted[0], unsorted[1]);
    CHECK(!planet.initialize(local, unsorted));
    const auto due = planet.due(Clock::now(), 1);
    CHECK(due.size() == 1 && (due[0].member == a || due[0].member == b));
}

void concurrent_snapshots() {
    Config config;
    config.cluster = "alpha";
    const auto a = member(1), b = member(2);
    StarTopology topology(config);
    const std::array list{a, b};
    CHECK(topology.initialize(a, list));
    std::atomic_bool invalid{};
    // 读取方只看快照, 写入方反复改变逻辑会话. TSan 必须实际执行并发, 不只启动插桩的单线程程序.
    const auto reader = [&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            const auto status = topology.status();
            if (!status.initialized || status.members != 2 || status.inbound > 1 || status.outbound != 0) {
                invalid = true;
            }
        }
    };
    {
        std::jthread first(reader), second(reader);
        for (std::uint64_t i = 1; i <= 200; ++i) {
            CHECK(topology.accept(b, Direction::inbound, {i}, std::nullopt));
            topology.closed({i}, {}, Clock::now());
        }
    }
    CHECK(!invalid && topology.status().inbound == 0);
}

void candidate_group_changes() {
    Config config;
    config.cluster = "alpha";
    config.role = Role::planet;
    PlanetUpstream planet(config);
    auto a = member(1), b = member(2);
    b.group = "remote";
    const std::array list{a, b};
    CHECK(planet.initialize(member(9, Role::planet), list));
    const auto now = Clock::now();
    auto first = planet.due(now, 1);
    CHECK(first.size() == 1 && first[0].member == a);
    a.epoch = {2};
    a.id.bytes[1] = 1;
    a.group = "remote";
    CHECK(planet.accept(a, Direction::outbound, {1}, first[0]));
    planet.closed({1}, ErrorCode::transport, now);
    auto second = planet.due(now, 1);
    CHECK(second.size() == 1 && second[0].member == b);
    b.epoch = {2};
    b.id.bytes[1] = 1;
    b.group = "default";
    CHECK(planet.accept(b, Direction::outbound, {2}, second[0]));
    planet.closed({2}, ErrorCode::transport, now);
    // 不依赖 Supervisor 刷新, 下一轮仍优先当前的同组 Star, 而不是原名单中排第一的 Star.
    const auto preferred = planet.due(now + std::chrono::seconds(6), 1);
    CHECK(preferred.size() == 1 && preferred[0].member == b);
}

void salted_candidate_selection() {
    Config config;
    config.cluster = "alpha";
    config.role = Role::planet;
    std::vector<Member> choices;
    for (unsigned i = 1; i <= 8; ++i) {
        choices.push_back(member(i));
    }
    std::set<Principal> selected;
    // 同一份授权名单可被不同进程使用, 同组首选必须有所分散, 不能一律使用排序后的第一项.
    for (unsigned i = 20; i <= 80; ++i) {
        PlanetUpstream planet(config);
        CHECK(planet.initialize(member(i, Role::planet), choices));
        auto target = planet.due(Clock::now(), 1);
        CHECK(target.size() == 1);
        selected.insert(target[0].member.principal);
    }
    CHECK(selected.size() > 1);
}

int main() {
    try {
        configuration();
        stars();
        planets();
        atomic_lists_and_capacity();
        concurrent_snapshots();
        candidate_group_changes();
        salted_candidate_selection();
        Config config;
        CHECK(retry_delay(std::numeric_limits<std::uint32_t>::max(), config, 123) <= config.reconnect_max);
        std::cout << "PASS configuration, identity boundaries, Star fencing and Planet failover\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
