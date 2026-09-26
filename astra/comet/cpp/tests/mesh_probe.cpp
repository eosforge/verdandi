#include <array>
#include <charconv>
#include <chrono>
#include <comet/client.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;

// 仅消费夹具私有秘密文件, 不经命令行或日志输出 APISECRET.
std::vector<std::uint8_t> secret(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (!std::filesystem::is_regular_file(path) || size == 0 || size > 4096) {
        throw std::runtime_error("Invalid private fixture file");
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> value(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(value.data()), static_cast<std::streamsize>(size)) || file.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Cannot read fixture credential");
    }
    return value;
}

// 构造一个实际原生 Client, 生产者允许退避切换, 两个观察者分别固定 Star 以验证真正复制.
comet::Client open(std::vector<std::string> endpoints, const std::filesystem::path& ca, const std::vector<std::uint8_t>& credential) {
    comet::Client::Options options;
    options.endpoints = std::move(endpoints);
    options.ca = ca;
    options.key = "integration";
    options.secret = credential;
    auto result = comet::Client::open(std::move(options));
    if (!result) {
        throw std::runtime_error("Cannot construct native mesh client");
    }
    return std::move(*result);
}

// 等待测试自己的可观察状态, 不在 SDK 回调中阻塞, 截止超时即失败而不是无限压测.
void until(auto&& predicate, std::chrono::seconds limit = 45s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Native mesh state did not converge");
        }
        std::this_thread::sleep_for(20ms);
    }
}

// 每个拥有者独立关闭并排空, 异常路径也不能将 gRPC 回调留给已析构探针栈.
struct Cleanup {
    comet::Client& client; // 守卫之前声明的真实 Client.

    ~Cleanup() {
        client.close();
        if (!client.wait(5s)) {
            std::terminate();
        }
    }
};

// 成功与仅接纳异步任务分开, 已知失败/超时不能被夹具无条件重试掩盖.
auto completed(auto&& future) {
    if (future.wait_for(10s) != std::future_status::ready) {
        throw std::runtime_error("Native mesh operation did not finish");
    }
    auto result = future.get();
    if (!result) {
        throw std::runtime_error("Native mesh operation was not confirmed");
    }
    return std::move(*result);
}

// 验证完整版本/正文和 Observer 的固定 Attr/当前 Data, 不把 stale 旧内容当成恢复成功.
bool visible(const comet::Subscriber& subscriber, const comet::Observer& observer, std::string_view uuid, std::uint64_t version, const std::vector<std::uint8_t>& value, const std::vector<std::uint8_t>& data) {
    const auto catalog = subscriber.watch();
    const auto services = observer.select();
    const auto publication = catalog.find("key");
    const auto registration = services.find(uuid);
    return catalog.state() == comet::Subscriber::State::ready && services.state() == comet::Observer::State::ready && publication && publication->version == version && *publication->value == value && registration && *registration->attr == std::vector<std::uint8_t>{'a', 't', 't', 'r'} && *registration->data == data;
}

// 三个固定入口同时提交, 每次都验证九条来源到目标路径. seconds 是额外稳定运行秒数, 零仍执行基本验收.
void replication(const std::array<comet::Client*, 3>& clients, std::chrono::seconds seconds, std::size_t copies, std::uint64_t base) {

    // 同一 Scope 中每节点拥有 copies (1..16) 个 Key/UUID, 交错编号使相邻键属于不同来源.
    const comet::Scope scope{"mesh", "parallel"};
    const auto records = clients.size() * copies;             // 单域最多 48 条, 与 SDK 在途预算保持明确上界.
    std::vector<comet::Publisher> publishers(records);        // 各 Key 唯一业务发布者, 关闭后由 TTL 删除.
    std::vector<comet::Beacon> beacons(records);              // 固定 Attr, 后续只更改 Data.
    std::array<comet::Subscriber, 3> subscribers;             // 固定节点, 禁止通过入口退避掩盖复制失败.
    std::array<comet::Observer, 3> observers;                 // 每个节点观察全部三个注册.
    std::vector<std::string> identities(records);             // 每条记录最初确认的 UUID, 正常运行期间不得无故变化.
    std::array<std::string, 3> instances;                     // 三个不同的接纳 Star, 后续同时核对写回执和视图归属.
    const std::vector<std::uint8_t> attr{'a', 't', 't', 'r'}; // 不变属性的逐字节验证基准.
    for (std::size_t index = 0; index < records; ++index) {
        auto publisher = clients[index % clients.size()]->publisher(scope, std::to_string(index), 3000ms);
        auto beacon = clients[index % clients.size()]->beacon(scope, attr, {}, 3000ms);
        if (!publisher || !beacon)
            throw std::runtime_error("Cannot create three-source writers");
        publishers[index] = std::move(*publisher);
        beacons[index] = std::move(*beacon);
    }
    for (std::size_t index = 0; index < clients.size(); ++index) {
        auto subscriber = clients[index]->subscriber(scope);
        auto observer = clients[index]->observer(scope);
        if (!subscriber || !observer) {
            throw std::runtime_error("Cannot create three-source replication objects");
        }
        subscribers[index] = std::move(*subscriber);
        observers[index] = std::move(*observer);
    }
    until([&] {
        for (const auto& beacon : beacons) {
            const auto state = beacon.state(); // 一次一致的状态快照, 不混用不同读取的身份.
            if (state.phase != comet::Beacon::Phase::ready || !state.identity)
                return false;
        }
        return true;
    });
    for (std::size_t index = 0; index < records; ++index) {
        const auto identity = *beacons[index].state().identity; // 已确认的入口身份, UUID 允许二进制 NUL.
        identities[index] = identity.uuid;
        if (index < clients.size())
            instances[index] = identity.instance;
        else if (identity.instance != instances[index % clients.size()])
            throw std::runtime_error("Fixed client changed admission during setup");
    }
    if (instances[0] == instances[1] || instances[0] == instances[2] || instances[1] == instances[2]) {
        throw std::runtime_error("Three writers were not admitted by three distinct Stars");
    }

    // 所有异步调用先发出再等待, 每入口最多 32 个在途写, 不能用逐节点完成代替并发来源验收.
    const auto deadline = std::chrono::steady_clock::now() + seconds; // 有限单调预算, 不受系统时间回拨影响.
    std::uint64_t version = base;                                     // 跨探针保留单调内容版本, TTL 删除不等于删除来源水位.
    do {
        ++version;
        std::vector<std::vector<std::uint8_t>> values(records);                             // 2 KiB 完整正文, 同时编码来源和版本以拒绝旧值.
        std::vector<std::future<comet::Result<comet::Publisher::Receipt>>> writes(records); // 每个发布恰好完成一次.
        std::vector<std::future<comet::Result<comet::Beacon::Receipt>>> updates(records);   // 与 Catalog 同时在途.
        for (std::size_t index = 0; index < records; ++index) {
            values[index].assign(2048, static_cast<std::uint8_t>(index));
            for (unsigned byte = 0; byte < sizeof(version); ++byte)
                values[index][byte] = static_cast<std::uint8_t>(version >> (byte * 8));
            writes[index] = publishers[index].publish(version, values[index], 10s);
            updates[index] = beacons[index].update(values[index], 10s);
        }
        for (std::size_t index = 0; index < records; ++index) {
            const auto publication = completed(writes[index]);   // 实际内存提交回执, 不是任务接纳.
            const auto registration = completed(updates[index]); // 固定入口和 UUID 在正常阶段不变.
            if (publication.instance != instances[index % clients.size()] || publication.version != version || registration.identity.instance != instances[index % clients.size()] || registration.identity.uuid != identities[index]) {
                throw std::runtime_error("Three-source commit changed its owner or identity");
            }
        }
        until([&] {
            for (std::size_t target = 0; target < clients.size(); ++target) {
                const auto catalog = subscribers[target].watch(); // 当前目标完整 Catalog 视图.
                const auto services = observers[target].select(); // 当前目标完整 Ephemeris 视图.
                if (catalog.state() != comet::Subscriber::State::ready || services.state() != comet::Observer::State::ready || catalog.instance() != instances[target] || services.instance() != instances[target] || catalog.size() != records || services.size() != records)
                    return false;
                for (std::size_t source = 0; source < records; ++source) {
                    const auto publication = catalog.find(std::to_string(source)); // 精确来源 Key, 不接受仅有计数相同.
                    const auto registration = services.find(identities[source]);   // 精确 UUID 和完整 Attr/Data.
                    if (!publication || publication->version != version || *publication->value != values[source] || !registration || *registration->attr != attr || *registration->data != values[source])
                        return false;
                }
            }
            return true;
        });
        if (version == base + 1) {
            // 超过原 TTL 后仍保活且读投影版本不变, 防止把续租重新广播为业务变化.
            const auto catalog = subscribers[0].watch(); // 静默续租前的完整 Catalog 视图.
            const auto services = observers[0].select(); // 静默续租前的读投影提交号.
            std::this_thread::sleep_for(3500ms);
            if (subscribers[0].watch().version() != catalog.version() || observers[0].select().version() != services.version() || subscribers[0].watch().size() != records || observers[0].select().size() != records) {
                throw std::runtime_error("Silent renewals changed or lost the visible projection");
            }
        }
        if (version % 100 == 0)
            std::cout << "{\"event\":\"mesh_progress\",\"round\":" << version << "}" << std::endl;
        std::this_thread::sleep_for(100ms);
    } while (version < base + 2 || std::chrono::steady_clock::now() < deadline);

    // 所有写者停止后等待三节点删除收敛, 确保下一轮不能继承旧对象或悬挂租约.
    for (std::size_t index = 0; index < records; ++index) {
        publishers[index].close();
        beacons[index].close();
        if (!publishers[index].wait(5s) || !beacons[index].wait(5s))
            throw std::runtime_error("Three-source writer cleanup timed out");
    }
    until([&] {
        for (std::size_t index = 0; index < clients.size(); ++index) {
            if (subscribers[index].watch().size() != 0 || observers[index].select().size() != 0)
                return false;
        }
        return true;
    },
          15s);
    std::cout << "{\"event\":\"mesh_replicated\",\"rounds\":" << version - base << ",\"records\":" << records << "}" << std::endl;
}
} // namespace

// 外层 Python 根据阶段事件强杀/重启 Star A; 探针只使用公开 Comet API, 没有内部复制或管理 Stub.
int main(int count, char** arguments) {

    try {
        if (count != 6 && count != 8 && count != 9 && count != 10) {
            throw std::runtime_error("Expected three endpoints, CA and secret file");
        }
        std::uint64_t seconds{}; // 默认仅基础验收, 长测可附加 0..604800 秒和 failure/steady 模式.
        std::size_t copies = 1;  // 普通验收每节点一条; 长测可显式要求 1..16 条.
        std::uint64_t base{};    // 长测轮次选择不重叠的内容版本区间, 不靠新增 Scope 隐藏常驻内存增长.
        const bool steady = count >= 8 && std::string_view(arguments[7]) == "steady";
        if (count >= 8) {
            const std::string_view input(arguments[6]); // 不接受负值、尾随垃圾或溢出时长.
            const auto parsed = std::from_chars(input.data(), input.data() + input.size(), seconds);
            if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || seconds > 604800 || (!steady && std::string_view(arguments[7]) != "failure"))
                throw std::runtime_error("Invalid mesh duration or mode");
        }
        if (count >= 9) {
            const std::string_view input(arguments[8]); // 严格十进制条目数, 不接受零或超出 SDK 预算.
            const auto parsed = std::from_chars(input.data(), input.data() + input.size(), copies);
            if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || copies < 1 || copies > 16)
                throw std::runtime_error("Invalid per-Star record count");
        }
        if (count == 10) {
            const std::string_view input(arguments[9]); // 0..604800 轮, 每轮保留一千万版本; 100ms 节流下一周也不会跨区间.
            const auto parsed = std::from_chars(input.data(), input.data() + input.size(), base);
            if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || base > 604800)
                throw std::runtime_error("Invalid mesh cycle");
            base *= 10000000;
        }
        const auto credential = secret(arguments[5]);
        auto producer = open({arguments[1], arguments[2]}, arguments[4], credential);
        const Cleanup producer_cleanup{producer};
        auto first = open({arguments[1]}, arguments[4], credential);
        const Cleanup first_cleanup{first};
        auto second = open({arguments[2]}, arguments[4], credential);
        const Cleanup second_cleanup{second};
        auto third = open({arguments[3]}, arguments[4], credential); // 第三个固定观察节点, 不能退避到正在写入的节点冒充复制.
        const Cleanup third_cleanup{third};
        replication({&first, &second, &third}, std::chrono::seconds(seconds), copies, base);
        if (steady) {
            if (first.exceptions() || second.exceptions() || third.exceptions())
                throw std::runtime_error("Steady workload callback failed");
            return 0;
        }
        const comet::Scope scope{"mesh", "main"};
        auto first_catalog = first.subscriber(scope), second_catalog = second.subscriber(scope), third_catalog = third.subscriber(scope);
        auto first_services = first.observer(scope), second_services = second.observer(scope), third_services = third.observer(scope);
        auto first_almanac = first.reader({"integration", "main"}), second_almanac = second.reader({"integration", "main"});
        auto publisher = producer.publisher(scope, "key", 3000ms);
        auto beacon = producer.beacon(scope, {'a', 't', 't', 'r'}, {'o', 'n', 'e'}, 3000ms);
        if (!first_catalog || !second_catalog || !third_catalog || !first_services || !second_services || !third_services || !first_almanac || !second_almanac || !publisher || !beacon) {
            throw std::runtime_error("Cannot create native mesh objects");
        }
        const std::vector<std::uint8_t> one{'o', 'n', 'e'}, two{'t', 'w', 'o'};
        static_cast<void>(completed(publisher->publish(base + 1, one, 10s)));
        until([&] { const auto state = beacon->state(); return state.phase == comet::Beacon::Phase::ready && state.identity.has_value(); });
        const auto original = *beacon->state().identity;
        until([&] { return visible(*first_catalog, *first_services, original.uuid, base + 1, one, one) && visible(*second_catalog, *second_services, original.uuid, base + 1, one, one) && visible(*third_catalog, *third_services, original.uuid, base + 1, one, one); });
        std::cout << "{\"event\":\"mesh_ready\"}" << std::endl; // 外层在此强杀原权威 Star, 不先注销注册.

        until([&] { const auto state = beacon->state(); return state.phase == comet::Beacon::Phase::ready && state.identity && state.identity->instance != original.instance && state.identity->uuid != original.uuid; }, 60s);
        const auto replacement = *beacon->state().identity;
        static_cast<void>(completed(publisher->publish(base + 2, two, 10s)));
        static_cast<void>(completed(beacon->update(two, 10s)));
        until([&] { return visible(*second_catalog, *second_services, replacement.uuid, base + 2, two, two) && visible(*third_catalog, *third_services, replacement.uuid, base + 2, two, two) && !second_services->select().find(original.uuid) && !third_services->select().find(original.uuid); });
        std::cout << "{\"event\":\"mesh_failover\"}" << std::endl; // 外层用原部署端口重启 Star A, 由 Pulsar 签发较新身份.

        until([&] {
            const auto almanac = first_almanac->load();
            return visible(*first_catalog, *first_services, replacement.uuid, base + 2, two, two) && first_services->select().instance() != original.instance && almanac.state() == comet::Reader::State::ready && almanac.version() && *almanac.version() == 4;
        },
              75s);
        const auto restored = second_almanac->load();
        if (restored.state() != comet::Reader::State::ready || !restored.version() || *restored.version() != 4) {
            throw std::runtime_error("Almanac authority baseline was not retained");
        }
        static_cast<void>(completed(publisher->publish(base + 3, std::vector<std::uint8_t>{}, 10s)));
        until([&] { return visible(*first_catalog, *first_services, replacement.uuid, base + 3, {}, two) && visible(*second_catalog, *second_services, replacement.uuid, base + 3, {}, two) && visible(*third_catalog, *third_services, replacement.uuid, base + 3, {}, two); });
        publisher->close();
        beacon->close();
        if (!publisher->wait(5s) || !beacon->wait(5s)) {
            throw std::runtime_error("Native mesh writers did not clean up");
        }
        until([&] { return !first_catalog->watch().find("key") && !second_catalog->watch().find("key") && !first_services->select().find(replacement.uuid) && !second_services->select().find(replacement.uuid) && !third_services->select().find(replacement.uuid) && !third_catalog->watch().find("key"); }, 15s);
        if (producer.exceptions() != 0 || first.exceptions() != 0 || second.exceptions() != 0 || third.exceptions() != 0) {
            throw std::runtime_error("Native mesh callback failed");
        }
        std::cout << "{\"event\":\"mesh_recovered\"}" << std::endl;
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
