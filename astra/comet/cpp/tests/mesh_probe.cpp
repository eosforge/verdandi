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
} // namespace

// 外层 Python 根据阶段事件强杀/重启 Star A; 探针只使用公开 Comet API, 没有内部复制或管理 Stub.
int main(int count, char** arguments) {

    try {
        if (count != 6) {
            throw std::runtime_error("Expected three endpoints, CA and secret file");
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
        static_cast<void>(completed(publisher->publish(1, one, 10s)));
        until([&] { const auto state = beacon->state(); return state.phase == comet::Beacon::Phase::ready && state.identity.has_value(); });
        const auto original = *beacon->state().identity;
        until([&] { return visible(*first_catalog, *first_services, original.uuid, 1, one, one) && visible(*second_catalog, *second_services, original.uuid, 1, one, one) && visible(*third_catalog, *third_services, original.uuid, 1, one, one); });
        std::cout << "{\"event\":\"mesh_ready\"}" << std::endl; // 外层在此强杀原权威 Star, 不先注销注册.

        until([&] { const auto state = beacon->state(); return state.phase == comet::Beacon::Phase::ready && state.identity && state.identity->instance != original.instance && state.identity->uuid != original.uuid; }, 60s);
        const auto replacement = *beacon->state().identity;
        static_cast<void>(completed(publisher->publish(2, two, 10s)));
        static_cast<void>(completed(beacon->update(two, 10s)));
        until([&] { return visible(*second_catalog, *second_services, replacement.uuid, 2, two, two) && visible(*third_catalog, *third_services, replacement.uuid, 2, two, two) && !second_services->select().find(original.uuid) && !third_services->select().find(original.uuid); });
        std::cout << "{\"event\":\"mesh_failover\"}" << std::endl; // 外层用原部署端口重启 Star A, 由 Pulsar 签发较新身份.

        until([&] {
            const auto almanac = first_almanac->load();
            return visible(*first_catalog, *first_services, replacement.uuid, 2, two, two) && first_services->select().instance() != original.instance && almanac.state() == comet::Reader::State::ready && almanac.version() && *almanac.version() == 4;
        },
              75s);
        const auto restored = second_almanac->load();
        if (restored.state() != comet::Reader::State::ready || !restored.version() || *restored.version() != 4) {
            throw std::runtime_error("Almanac authority baseline was not retained");
        }
        static_cast<void>(completed(publisher->publish(3, std::vector<std::uint8_t>{}, 10s)));
        until([&] { return visible(*first_catalog, *first_services, replacement.uuid, 3, {}, two) && visible(*second_catalog, *second_services, replacement.uuid, 3, {}, two) && visible(*third_catalog, *third_services, replacement.uuid, 3, {}, two); });
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
