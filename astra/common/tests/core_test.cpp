#include "check.hpp"
#include "topology.hpp"
#include "upstream.hpp"
#include <astra/config.hpp>

#include <array>
#include <atomic>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>

using namespace astra;

namespace {

// 按 n 构造确定性的测试成员, 角色默认 Star,代次默认 1, ID/主体/端口彼此对应且不使用真实部署.
Member member(unsigned n, Member::Role role = Member::Role::star, std::uint64_t epoch = 1) {

    // result 独立拥有构造的成员字段, 返回前填满全部有效身份信息.
    Member result;
    result.galaxy = "alpha";
    result.group = "default";
    result.id = "issued/" + std::to_string(1000 + n);
    result.principal.bytes[0] = static_cast<std::uint8_t>(n);
    result.address = *Endpoint::parse("127.0.0.1:" + std::to_string(7000 + n));
    result.epoch = Member::Epoch{epoch};
    result.role = role;
    return result;
}

// 覆盖默认配置,必填项,重复项,容量和端点边界, 不加载身份或启动监听.
void configuration() {

    // arguments 为合法最小 CLI, 后续用例只追加待拒绝或待覆盖的选项.
    const std::array<std::string_view, 3> arguments{"--galaxy=alpha", "--listen=127.0.0.1:7443", "--super=localhost:7444"};
    // parsed 为最小配置解析结果, 用公开字段核对真实默认值.
    auto parsed = Config::parse(arguments, Member::Role::star);
    CHECK(parsed);
    CHECK(parsed->max_members == 64);
    CHECK(parsed->max_inbound == 128);
    CHECK(parsed->heartbeat_interval == Milliseconds(10000));
    CHECK(parsed->identity == "identity");
    CHECK(parsed->galaxy == "alpha");
    CHECK(!parsed->comet && parsed->auth && parsed->tls && parsed->comet_identity.empty());
    CHECK(parsed->max_admission_request_bytes == 4096 && parsed->max_admission_response_bytes == 2 * 1024 * 1024);
    CHECK(Config::help(Member::Role::planet).contains("Usage: planet"));
    CHECK(Config::help(Member::Role::star).contains("default=5000"));
    for (const auto bad : {"--max-members=0", "--max-members=4097", "--max-members=1x", "--max-members=18446744073709551616", "--max-members=-1", "--galaxy=beta", "--worker-threads=2", "--role=star", "--identity=", "--pong-timeout-ms=0", "--shutdown-timeout-seconds=61"}) {
        std::vector<std::string_view> values(arguments.begin(), arguments.end());
        values.push_back(bad);
        CHECK(!Config::parse(values, Member::Role::star));
    }

    // separated 使用分隔式参数和通配监听, 显式 advertise 使公开端点仍可连接.
    const std::array<std::string_view, 8> separated{"--galaxy", "alpha", "--listen", "0.0.0.0:7443", "--super", "localhost:7444", "--advertise", "127.0.0.1:7443"};
    CHECK(Config::parse(separated, Member::Role::planet));
    CHECK(!Config::parse(std::span(separated).first<6>(), Member::Role::planet));
    CHECK(!Config::parse({}, Member::Role::star));
    CHECK(!Endpoint::parse("127.000.0.1:7443"));
    CHECK(!Endpoint::parse("[::ffff:127.0.0.1]:7443"));
    CHECK(!Endpoint::parse("[fe80::1%2]:7443"));
    CHECK(!Endpoint::parse("224.0.0.1:7443"));
    CHECK(!Endpoint::parse("0.0.0.0:7443"));
    CHECK(!Config::format_pulsar("0.0.0.0:7443"));
    CHECK(!Config::format_pulsar("224.0.0.1:7443"));
    CHECK(Endpoint::parse("0.0.0.0:0", true));
    CHECK(Endpoint::parse("[2001:0DB8:0:0:0:0:0:1]:7443")->text() == "[2001:db8::1]:7443");
    CHECK(Member::valid_id("short"));
    CHECK(Member::valid_id(member(1).id));
    CHECK(!Member::valid_id(Id{}));
    CHECK(!Member::valid_name("alpha/beta"));
    CHECK(!Member::valid_name(std::string(65, 'a')));
    // 新增容量选项的边界与赋值都需验证, 防止注解解析成功却未传入 gRPC 配置.
    for (const auto invalid : {"--max-admission-request-bytes=0", "--max-admission-request-bytes=1048577", "--max-admission-response-bytes=1023", "--max-admission-response-bytes=268435457"}) {
        std::vector<std::string_view> values(arguments.begin(), arguments.end());
        values.push_back(invalid);
        CHECK(!Config::parse(values, Member::Role::star));
    }
    std::vector<std::string_view> limits(arguments.begin(), arguments.end());
    limits.insert(limits.end(), {"--max-admission-request-bytes=1", "--max-admission-response-bytes=1024"});
    // bounded 使用收发容量的最小合法值, 检查配置是否真正被赋给运行参数.
    const auto bounded = Config::parse(limits, Member::Role::star);
    CHECK(bounded && bounded->max_admission_request_bytes == 1 && bounded->max_admission_response_bytes == 1024);
    // 公共端口和证书必须显式成套配置, 不改变内部身份或借用其端口; 布尔拼写不能触发降级.
    std::vector<std::string_view> public_options(arguments.begin(), arguments.end());
    public_options.insert(public_options.end(), {"--comet=0.0.0.0:0", "--tls=false", "--auth=false"});
    const auto plaintext = Config::parse(public_options, Member::Role::star);
    CHECK(plaintext && plaintext->comet->wildcard && !plaintext->tls && !plaintext->auth && plaintext->listen.port == 7443);
    CHECK(!Config::parse(public_options, Member::Role::planet));
    public_options.push_back("--comet-identity=public");
    CHECK(!Config::parse(public_options, Member::Role::star));
    for (const auto invalid : {"--tls=TRUE", "--tls=0", "--auth=off", "--tls=false", "--auth=false", "--comet=127.0.0.1:7443", "--comet=127.0.0.1:7445", "--comet-identity=public"}) {
        std::vector<std::string_view> values(arguments.begin(), arguments.end());
        values.push_back(invalid);
        CHECK(!Config::parse(values, Member::Role::star));
    }
    public_options.assign(arguments.begin(), arguments.end());
    public_options.insert(public_options.end(), {"--comet=127.0.0.1:7445", "--comet-identity=public"});
    const auto encrypted = Config::parse(public_options, Member::Role::star);
    CHECK(encrypted && encrypted->tls && encrypted->auth && encrypted->comet_identity == "public");
    // Pulsar 主机解析移动实现文件后仍保留端口与 DNS 标签验证.
    CHECK(Config::format_pulsar("Pulsar.EXAMPLE:7444") == "Pulsar.EXAMPLE:7444");
    for (const auto invalid : {"host:0", "host:65536", "host:1x", "-host:7444", "host..local:7444", "[::]:7444"}) {
        CHECK(!Config::format_pulsar(invalid));
    }
}

// 覆盖 Star 单流裁决,重复连接拒绝,升代替换和既有 Planet 入站限制.
void stars() {

    // config 从默认值构造, 本场景仅覆盖要验证的 Galaxy,角色或容量参数.
    Config config;
    config.galaxy = "alpha";
    config.max_members = 3;
    // topology 是当前场景独立的未初始化策略, 不启动任何真实网络连接.
    Star topology(config);
    // a 为本地测试身份, 后续声明的成员用于远端和候选关系.
    const auto a = member(1), b = member(2);
    // list 拥有按 ID 排序的输入成员副本, 在 initialize 调用期间借用.
    std::array list{a, b};
    CHECK(topology.initialize(a, list));
    CHECK(topology.status().members == 2);
    CHECK(!topology.initialize(a, list));
    // targets 为被策略选中的出站目标, 后续重复 due 不应再次返回同一待拨号成员.
    auto targets = topology.due(Steady::now());
    CHECK(targets);
    CHECK(!topology.due(Steady::now()));
    CHECK(topology.accept(b, Policy::Direction::outbound, {1}, *targets));
    CHECK(!topology.accept(b, Policy::Direction::inbound, {2}, std::nullopt));
    CHECK(!topology.accept(b, Policy::Direction::inbound, {3}, std::nullopt));
    // next 从远端 b 复制后提升代次, 用于替换既有唯一会话.
    auto next = b;
    next.epoch = Member::Epoch{2};
    next.id += "/restart";
    // replacement 收集替换时需取消的旧代次, 成功后新入站会话仍有效.
    auto replacement = topology.accept(next, Policy::Direction::inbound, {4}, std::nullopt);
    CHECK(replacement && replacement->size() == 1);
    topology.closed({1}, {}, Steady::now());
    CHECK(topology.status().inbound == 1);
    CHECK(!topology.accept(b, Policy::Direction::outbound, {5}, *targets));
    CHECK(topology.status().members == 2);
    CHECK(!topology.accept(a, Policy::Direction::inbound, {6}, std::nullopt));
    // planet 为独立中继角色, Star 只允许其占用入站槽位.
    const auto planet = member(3, Member::Role::planet);
    CHECK(topology.accept(planet, Policy::Direction::inbound, {7}, std::nullopt));
    CHECK(topology.status().members == 2 && topology.status().planet_inbound == 1);
    CHECK(!topology.accept(planet, Policy::Direction::outbound, {8}, std::nullopt));
    // conflict 复制新代次成员后只改分组, 验证同代次身份不可悄悄改变.
    auto conflict = next;
    conflict.group = "changed";
    CHECK(!topology.accept(conflict, Policy::Direction::outbound, {9}, std::nullopt));
}

// 确认 pending 只产生一次拨号责任, 旧目标失败不能释放新代次的拨号标记.
void single_dial_ownership() {

    // config 从默认值构造, 本场景仅覆盖要验证的 Galaxy,角色或容量参数.
    Config config;
    config.galaxy = "alpha";
    // topology 是当前场景独立的未初始化策略, 不启动任何真实网络连接.
    Star topology(config);
    // a 为本地测试身份, 后续声明的成员用于远端和候选关系.
    const auto a = member(1), b = member(2), c = member(3);
    // now 为本场景统一的单调锚点, 后续用显式时间偏移检查退避而不休眠.
    const auto now = Steady::now();
    CHECK(!topology.due(now));
    CHECK(topology.initialize(a, std::array{a, b, c}));
    // first/second 分别持有一次出站拨号责任, 避免重复任务选中已 pending 成员.
    const auto first = topology.due(now), second = topology.due(now);
    CHECK(first && second && *first != *second);
    CHECK(!topology.due(now));
    topology.failed(*first, Status::Code::transport, now);
    CHECK(!topology.due(now));
    // retry 是退避结束后重新选中的目标, 必须保持原身份与唯一拨号责任.
    const auto retry = topology.due(now + config.reconnect_max);
    CHECK(retry == first && !topology.due(now + config.reconnect_max));

    // 旧拨号的迟到失败不能释放新实例已经取得的 pending 槽.
    // restarted 复制候选并更新进程 ID/代次, 模拟同部署新进程接管.
    auto restarted = *first;
    restarted.id += "/restart";
    ++restarted.epoch.value;
    CHECK(topology.accept(restarted, Policy::Direction::inbound, {20}, std::nullopt));
    CHECK(!topology.due(now + config.reconnect_max));
    topology.closed({20}, Status::Code::transport, now);
    // replacement 为升代后的新拨号目标, 旧目标的迟到失败不得清除其 pending.
    const auto replacement = topology.due(now + config.reconnect_max);
    CHECK(replacement && *replacement == restarted);
    topology.failed(*first, Status::Code::transport, now);
    CHECK(!topology.due(now + config.reconnect_max));
    CHECK(topology.accept(restarted, Policy::Direction::outbound, {21}, replacement));
    CHECK(topology.status().outbound == 1);
}

// 同时拨号时两端独立选中同一条逻辑流, 验证两种到达顺序及迟到完成的隔离.
void arbitration() {

    // a 的 ID 小于 b, 因此冲突时保留 a 发起的流, 不需要额外全局仲裁者.
    Config config;
    config.galaxy = "alpha";
    const auto a = member(1), b = member(2);
    const auto now = Steady::now();
    for (const bool preferred_first : {false, true}) {
        Star left(config), right(config); // 每轮独立的两端策略, 不复用已关闭的代次.
        CHECK(left.initialize(a, std::array{a, b}));
        CHECK(right.initialize(b, std::array{a, b}));
        const auto outbound = left.due(now), inbound = right.due(now); // 同时持有两个拨号责任.
        CHECK(outbound && inbound);
        if (!preferred_first) {
            CHECK(left.accept(b, Policy::Direction::inbound, {1}, std::nullopt));
            CHECK(right.accept(a, Policy::Direction::outbound, {2}, inbound));
        }
        const auto installed = left.accept(b, Policy::Direction::outbound, {3}, outbound);
        const auto accepted = right.accept(a, Policy::Direction::inbound, {4}, std::nullopt);
        CHECK(installed && accepted);
        CHECK(installed->size() == (preferred_first ? 0 : 1));
        CHECK(accepted->size() == (preferred_first ? 0 : 1));
        if (preferred_first) {
            CHECK(!left.accept(b, Policy::Direction::inbound, {1}, std::nullopt));
            CHECK(!right.accept(a, Policy::Direction::outbound, {2}, inbound));
        }

        // 被淘汰 RPC 的完成/拨号失败不能关闭新流, 也不能使健康入站触发反向补拨.
        left.closed({1}, Status::Code::transport, now);
        right.closed({2}, Status::Code::transport, now);
        right.failed(*inbound, Status::Code::transport, now);
        CHECK(left.status().outbound == 1 && left.status().inbound == 0);
        CHECK(right.status().inbound == 1 && right.status().outbound == 0);
        CHECK(!left.due(now + config.reconnect_max) && !right.due(now + config.reconnect_max));
        CHECK(!left.accept(b, Policy::Direction::outbound, {5}, outbound));
        CHECK(!right.accept(a, Policy::Direction::inbound, {6}, std::nullopt));

        // 唯一流断开后, 两端均可退避重连; 裁决只解决并发冲突, 不固定永久拨号责任.
        left.closed({3}, Status::Code::transport, now);
        right.closed({4}, Status::Code::transport, now);
        CHECK(left.due(now + config.reconnect_max) && right.due(now + config.reconnect_max));
    }
}

// 核对部署摘要的固定小写编码, 拒绝截短和非法字符, 保持跨语言字节一致.
void principal_encoding() {

    // encoded 为固定 64 字符小写摘要向量, 各字节可直接核对解析和回编码.
    constexpr std::string_view encoded = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    // parsed 是摘要解析结果, 同时检查首尾字节和完整反向编码.
    const auto parsed = Principal::parse(encoded);
    CHECK(parsed && parsed->bytes.front() == 0x01 && parsed->bytes.back() == 0xef);
    CHECK(parsed->text() == encoded);
    CHECK(!Principal::parse(encoded.substr(1)));
    for (const char invalid : {'G', 'A', '\0', '/'}) {
        // malformed 独立复制合法编码, 每轮仅替换一个字符来覆盖非法字节.
        auto malformed = std::string(encoded);
        malformed[17] = invalid;
        CHECK(!Principal::parse(malformed));
    }
}

// 保留既有 Planet 候选切换与隔离回归, 不在本次整理中增加 Planet 功能.
void planets() {

    // config 从默认值构造, 本场景仅覆盖要验证的 Galaxy,角色或容量参数.
    Config config;
    config.galaxy = "alpha";
    config.role = Member::Role::planet;
    PlanetUpstream upstream(config);
    // local 为场景内 Planet 身份, 与 a 或候选表中的所有 Star 不同.
    const auto local = member(9, Member::Role::planet), a = member(1);
    // b 为第二候选的可变副本, 设置远端分组以触发跨组后备路径.
    auto b = member(2);
    b.group = "remote";
    // list 拥有按 ID 排序的输入成员副本, 在 initialize 调用期间借用.
    std::array list{a, b};
    CHECK(upstream.initialize(local, list));
    // now 为本场景统一的单调锚点, 后续用显式时间偏移检查退避而不休眠.
    const auto now = Steady::now();
    // first 为本轮首选候选, 成功取得后会被策略标记 pending.
    auto first = upstream.due(now);
    CHECK(first && *first == a);
    CHECK(!upstream.due(now));
    upstream.failed(*first, Status::Code::transport, now);
    // fallback 为本组失败后的可用候选, 应转向跨组的 b.
    auto fallback = upstream.due(now);
    CHECK(fallback && *fallback == b);
    // restarted 复制候选并更新进程 ID/代次, 模拟同部署新进程接管.
    auto restarted = b;
    restarted.epoch = Member::Epoch{2};
    restarted.id += "/restart";
    CHECK(upstream.accept(restarted, Policy::Direction::outbound, {10}, *fallback));
    CHECK(upstream.status().active_member);
    CHECK(!upstream.due(now));
    CHECK(!upstream.needs_refresh(now));
    upstream.closed({9}, {}, now);
    CHECK(upstream.status().active_member);
    upstream.closed({10}, Status::Code::transport, now);
    CHECK(!upstream.status().active_member);
    CHECK(upstream.initialize(local, list));
    // again 为退避后的一次新尝试, 接下来的身份错误应隔离这个候选.
    auto again = upstream.due(now + std::chrono::seconds(6));
    CHECK(again);
    upstream.failed(*again, Status::Code::identity, now);
    CHECK(upstream.initialize(local, list));
    // other 为刷新后仍可用的另一候选, 不能重新启用已隔离的相同代次.
    auto other = upstream.due(now + std::chrono::seconds(6));
    CHECK(other && other->principal != again->principal);
    CHECK(other->epoch.value == 2);
    CHECK(!upstream.accept(a, Policy::Direction::inbound, {11}, *other));
}

// 验证坏名单不能部分安装, Star/Planet 容量分开计数, 三类身份别名都被拒绝.
void atomic_lists_and_capacity() {

    // config 从默认值构造, 本场景仅覆盖要验证的 Galaxy,角色或容量参数.
    Config config;
    config.galaxy = "alpha";
    config.max_members = 2;
    // a 为本地测试身份, 后续声明的成员用于远端和候选关系.
    const auto a = member(1), b = member(2);
    Star star(config);
    // alias 仅改变某项身份绑定制造冲突, 检查原拓扑与容量计数保持不变.
    auto alias = b;
    alias.address = a.address;
    // invalid 包含地址别名, 初始化必须完整拒绝并保持未初始化状态.
    std::array invalid{a, alias};
    CHECK(!star.initialize(a, invalid));
    CHECK(!star.status().initialized && star.status().members == 0);
    // valid 是失败后的合法重试名单, 用于确认对象仍可正常初始化.
    std::array valid{a, b};
    CHECK(star.initialize(a, valid));
    // 控制服务即使具有合法成员格式, 也不能借基础设施准入占用对等或 Planet 会话槽.
    for (const auto role : {Member::Role::polaris, Member::Role::astrolabe}) {
        CHECK(!star.accept(member(8, role), Policy::Direction::inbound, {99}, std::nullopt));
        CHECK(!star.accept(member(8, role), Policy::Direction::outbound, {99}, member(8, role)));
    }
    CHECK(!star.accept(member(3), Policy::Direction::inbound, {1}, std::nullopt));
    // Star 和 Planet 分别计数, 一个角色满额不能污染另一个角色或原有索引.
    CHECK(star.accept(member(3, Member::Role::planet), Policy::Direction::inbound, {2}, std::nullopt));
    CHECK(star.accept(member(4, Member::Role::planet), Policy::Direction::inbound, {3}, std::nullopt));
    CHECK(!star.accept(member(5, Member::Role::planet), Policy::Direction::inbound, {4}, std::nullopt));
    CHECK(star.status().members == 2 && star.status().planet_inbound == 2);
    alias = member(6, Member::Role::planet);
    alias.address = b.address;
    CHECK(!star.accept(alias, Policy::Direction::inbound, {5}, std::nullopt));
    CHECK(star.status().members == 2 && star.status().planet_inbound == 2);

    config.role = Member::Role::planet;
    PlanetUpstream planet(config);
    // local 为场景内 Planet 身份, 与 a 或候选表中的所有 Star 不同.
    const auto local = member(20, Member::Role::planet);
    // oversized 构造九个候选以超过 Planet 固定八项容量, 失败后候选表仍为空.
    std::vector<Member> oversized;
    for (unsigned i = 1; i <= 9; ++i) {
        oversized.push_back(member(i));
    }
    CHECK(!planet.initialize(local, oversized));
    CHECK(!planet.status().initialized && planet.status().candidates == 0);
    CHECK(planet.initialize(local, valid));
    // unsorted 复制合法名单后交换顺序, 防止排序校验被忽略.
    auto unsorted = valid;
    std::swap(unsorted[0], unsorted[1]);
    CHECK(!planet.initialize(local, unsorted));
    // 跨组排序合法时仍必须拒绝三种别名, 且失败不能部分覆盖原有候选名单.
    for (unsigned field = 0; field < 3; ++field) {
        // duplicate 每轮仅制造主体,ID 或地址中的一种别名, 保留跨组排序合法.
        auto duplicate = valid;
        duplicate[1].group = "remote";
        if (field == 0) {
            duplicate[1].principal = a.principal;
        } else if (field == 1) {
            duplicate[1].id = a.id;
        } else {
            duplicate[1].address = a.address;
        }
        CHECK(!planet.initialize(local, duplicate));
        CHECK(planet.status().candidates == 2);
    }

    // due 应仍来自此前合法名单, 证明坏刷新未覆盖原有候选.
    const auto due = planet.due(Steady::now());
    CHECK(due && (*due == a || *due == b));
}

// 本地主体和远端主体重复都必须在提交前拒绝, 且失败后的同一对象仍可正常初始化.
void initialization() {

    // config 为独立 Star 场景, Galaxy 与生成成员一致.
    Config config;
    config.galaxy = "alpha";

    // principal 分别指向本地和首个远端, 覆盖两条不同的重复主体检测路径.
    for (std::size_t principal : {0U, 1U}) {
        // members 的 ID 和地址保持严格有序且不同, 只让第三项主体重复.
        std::array members{member(1), member(2), member(3)};
        members[2].principal = members[principal].principal;
        // star 初始无身份和成员表, 拒绝输入不能留下部分可见状态.
        Star star(config);
        CHECK(!star.initialize(members[0], members));
        CHECK(!star.status().initialized && star.status().members == 0);

        // 恢复第三项主体后, 同一实例应能完整提交三个成员.
        members[2] = member(3);
        CHECK(star.initialize(members[0], members));
        CHECK(star.status().initialized && star.status().members == members.size());
    }
}

// 以并发读者观察拓扑计数, 写者反复安装和关闭会话, 快照不能出现混合状态.
void concurrent_snapshots() {

    // config 从默认值构造, 本场景仅覆盖要验证的 Galaxy,角色或容量参数.
    Config config;
    config.galaxy = "alpha";
    // a 为本地测试身份, 后续声明的成员用于远端和候选关系.
    const auto a = member(1), b = member(2);
    // topology 是当前场景独立的未初始化策略, 不启动任何真实网络连接.
    Star topology(config);
    // list 是不可变的初始成员副本, 后续策略更新不能反向修改它.
    const std::array list{a, b};
    CHECK(topology.initialize(a, list));
    // invalid 初始 false, 任一读者见到不一致计数就置 true, 工作线程退出后统一断言.
    std::atomic_bool invalid{};
    // 读取方只看快照, 写入方反复改变逻辑会话. TSan 必须实际执行并发, 不只启动插桩的单线程程序.
    const auto reader = [&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            // status 独立持有单次拓扑快照, 一次检查不拼接多次读取的不同版本.
            const auto status = topology.status();
            if (!status.initialized || status.members != 2 || status.inbound > 1 || status.outbound != 0) {
                invalid = true;
            }
        }
    };
    {
        std::jthread first(reader), second(reader);
        for (std::uint64_t i = 1; i <= 200; ++i) {
            CHECK(topology.accept(b, Policy::Direction::inbound, {i}, std::nullopt));
            topology.closed({i}, {}, Steady::now());
        }
    }
    CHECK(!invalid && topology.status().inbound == 0);
}

// 核对合法升代后的分组改变会影响后续择优, 不沿用旧候选顺序.
void candidate_group_changes() {

    // config 从默认值构造, 本场景仅覆盖要验证的 Galaxy,角色或容量参数.
    Config config;
    config.galaxy = "alpha";
    config.role = Member::Role::planet;
    PlanetUpstream planet(config);
    // a/b 为可变远端身份副本, 后续显式提升代次并改变分组, 不修改原名单.
    auto a = member(1), b = member(2);
    b.group = "remote";
    // list 是不可变的初始成员副本, 后续策略更新不能反向修改它.
    const std::array list{a, b};
    CHECK(planet.initialize(member(9, Member::Role::planet), list));
    // now 为本场景统一的单调锚点, 后续用显式时间偏移检查退避而不休眠.
    const auto now = Steady::now();
    // first 为本轮首选候选, 成功取得后会被策略标记 pending.
    auto first = planet.due(now);
    CHECK(first && *first == a);
    a.epoch = {2};
    a.id += "/restart";
    a.group = "remote";
    CHECK(planet.accept(a, Policy::Direction::outbound, {1}, *first));
    planet.closed({1}, Status::Code::transport, now);
    // second 是首选失败后的另一候选, 随后改变其分组来验证动态择优.
    auto second = planet.due(now);
    CHECK(second && *second == b);
    b.epoch = {2};
    b.id += "/restart";
    b.group = "default";
    CHECK(planet.accept(b, Policy::Direction::outbound, {2}, *second));
    planet.closed({2}, Status::Code::transport, now);
    // 不依赖 Pulsar 刷新, 下一轮仍优先当前的同组 Star, 而不是原名单中排第一的 Star.
    const auto preferred = planet.due(now + std::chrono::seconds(6));
    CHECK(preferred && *preferred == b);
}

// 同一候选名单配合不同进程身份应产生多个首选目标, 避免固定第一项聚集.
void salted_candidate_selection() {

    // config 从默认值构造, 本场景仅覆盖要验证的 Galaxy,角色或容量参数.
    Config config;
    config.galaxy = "alpha";
    config.role = Member::Role::planet;
    // choices 保存共享的八个授权候选, 每个测试 Planet 获得同一份名单.
    std::vector<Member> choices;
    for (unsigned i = 1; i <= 8; ++i) {
        choices.push_back(member(i));
    }

    // selected 记录不同进程实际选中的主体, 只检查分散存在而不承诺均匀分布.
    std::set<Principal> selected;
    // 同一份授权名单可被不同进程使用, 同组首选必须有所分散, 不能一律使用排序后的第一项.
    for (unsigned i = 20; i <= 80; ++i) {
        PlanetUpstream planet(config);
        CHECK(planet.initialize(member(i, Member::Role::planet), choices));
        // target 为当前进程的首选, 从其主体统计盐值是否影响选择.
        auto target = planet.due(Steady::now());
        CHECK(target);
        selected.insert(target->principal);
    }
    CHECK(selected.size() > 1);
}

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
int main() {

    try {
        configuration();
        stars();
        single_dial_ownership();
        arbitration();
        principal_encoding();
        planets();
        atomic_lists_and_capacity();
        initialization();
        concurrent_snapshots();
        candidate_group_changes();
        salted_candidate_selection();
        // config 从默认值构造, 本场景仅覆盖要验证的 Galaxy,角色或容量参数.
        Config config;
        CHECK(retry_delay(std::numeric_limits<std::uint32_t>::max(), config, 123) <= config.reconnect_max);
        std::cout << "PASS configuration, identity boundaries, Star fencing and Planet failover\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
