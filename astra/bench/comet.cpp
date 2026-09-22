#include "delivery.hpp"
#include "measure.hpp"
#include <comet/client.hpp>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;

// 只等待本次进程的可观察条件, 超时使场景失败; 不通过重试写入掩盖未知结果.
void until(auto&& ready, std::chrono::seconds timeout = 30s) {
    const auto deadline = Measure::Clock::now() + timeout; // 本地有界等待, 不读取墙钟.
    while (!ready()) {
        Measure::require(Measure::Clock::now() < deadline);
        std::this_thread::sleep_for(50us);
    }
}

// 所有业务句柄和回调必须在 Client 清理后才能销毁, 异常路径同样有界关闭.
struct Cleanup {
    std::vector<comet::Client>& clients; // 生命周期覆盖本守卫的共享核心集合.

    ~Cleanup() {
        for (auto& client : clients) {
            client.close();
        }
        for (auto& client : clients) {
            if (!client.wait(10s)) {
                std::terminate();
            }
        }
    }
};

// 收到真实确认才能继续推进同一写者; 超时/拒绝返回非零, 不计入成功吞吐.
void completed(auto future) {
    Measure::require(future.wait_for(10s) == std::future_status::ready);
    const auto result = future.get(); // 本次结果拥有必要身份, 不借用后台状态.
    if (!result) {
        throw std::runtime_error("SDK operation failed, code=" + std::to_string(static_cast<int>(result.error().code)));
    }
}

// 生成独立正文, 前八字节为本次序号, 其余填固定字节; 不将共享可变内存交给 SDK.
std::vector<std::uint8_t> content(std::size_t bytes, std::uint64_t version) {
    std::vector<std::uint8_t> result(bytes, 42); // bytes 至少 16, 序号拷贝不会越界.
    std::memcpy(result.data(), &version, sizeof(version));
    return result;
}

// 场景的唯一参数载体, 由受限命令行生成, 日志由外层同时记录全部取值.
struct Options {
    std::vector<std::string> endpoints; // 至少一台已就绪 Star, 首台负责写入.
    std::string ca;                     // "-" 禁用业务 TLS, 否则为测试 CA 文件.
    std::vector<std::uint8_t> secret;   // 空关闭业务认证, 非空只从本次夹具的私有文件读取.
    bool catalog{};                     // true 测 Catalog, false 测 Ephemeris.
    std::size_t records{};              // 1..500 个活动对象, 含未频繁更新的背景记录.
    std::size_t bytes{};                // Data/正文 16..4096 字节.
    std::size_t attr{};                 // Ephemeris 固定属性 0..65536 字节.
    std::size_t watchers{};             // 1..50 个完整范围订阅, 分散至指定 Star.
    std::size_t writers{};              // 1..32 个独立写者, 每个固定一个对象且最多一条在途写入.
    unsigned seconds{};                 // 2..30 秒采样时间, 初始化和清理不计入.
    unsigned ttl{};                     // 固定租约毫秒, 1000..600000.
    unsigned rate{};                    // 零表示闭环; 正数为所有写者合计计划每秒次数, 延迟包含发送排队.
};

// 真正使用公开原生 SDK, 数据由实际 Star 进程提交/复制/推流, 不用测试专用 Stub 代替.
void run(const Options& options) {

    std::vector<comet::Client> clients;            // 首个生产者, 其后每端点一个共享订阅 Client.
    const Cleanup cleanup{clients};                // 提前建立异常清理, 业务局部析构后仍会排空核心.
    const comet::Scope scope{"measure", "stream"}; // 外层每次重建集群, 不复用其他样本的数据.
    auto open = [&](const std::string& endpoint) { // 配置仅来自本次隔离夹具, 准入关闭由 Star 明确配置.
        comet::Client::Options settings;
        settings.endpoints = {endpoint};
        settings.auth = !options.secret.empty();
        settings.key = settings.auth ? "performance" : "";
        settings.secret = options.secret;
        settings.tls = options.ca != "-";
        settings.ca = settings.tls ? options.ca : "";
        settings.readers = 64;
        auto result = comet::Client::open(std::move(settings));
        Measure::require(result.has_value());
        clients.push_back(std::move(*result));
    };
    open(options.endpoints.front());
    for (const auto& endpoint : options.endpoints) {
        open(endpoint);
    }

    // 用实际工厂创建完整背景数据, Ephemeris 保留全部 Beacon 以触发正常自动续租.
    std::vector<comet::Publisher> publishers; // Catalog 拥有者, 与 beacons 二选一.
    std::vector<comet::Beacon> beacons;       // Ephemeris 拥有者, 关闭时尽力注销.
    std::vector<std::string> keys;            // 对应业务 Key 或服务器生成的 UUID.
    for (std::size_t index = 0; index < options.records; ++index) {
        if (options.catalog) {
            keys.push_back("key-" + std::to_string(index));
            auto result = clients.front().publisher(scope, keys.back(), std::chrono::milliseconds(options.ttl));
            Measure::require(result.has_value());
            publishers.push_back(std::move(*result));
            completed(publishers.back().publish(1, content(options.bytes, 1)));
        } else {
            auto result = clients.front().beacon(scope, std::vector<std::uint8_t>(options.attr, 17), content(options.bytes, 1), std::chrono::milliseconds(options.ttl));
            Measure::require(result.has_value());
            beacons.push_back(std::move(*result));
        }
    }
    if (!options.catalog) {
        until([&] { return std::ranges::all_of(beacons, [](const auto& beacon) { const auto state = beacon.state(); return state.phase == comet::Beacon::Phase::ready && state.identity.has_value(); }); });
        for (const auto& beacon : beacons) {
            keys.push_back(beacon.state().identity->uuid);
        }
    }

    std::vector<comet::Subscriber> subscribers;                                                                             // 回调只报告完整 View, 样本包含 SDK 安装及通知成本.
    std::vector<comet::Observer> observers;                                                                                 // 每个订阅独立安装 Attr/Data, 不共享虚假的应用内副本.
    const auto delivery = std::make_shared<Delivery>(keys, options.writers, options.watchers, options.bytes, options.attr); // 回调独立拥有确认器, 异常清理不借用本函数栈.
    for (std::size_t index = 0; index < options.watchers; ++index) {
        auto& client = clients[1 + index % options.endpoints.size()]; // 轮流连接各 Star, 包括远端来源复制.
        if (options.catalog) {
            comet::Subscriber::Options settings; // 回调在 SDK 锁外执行, 不等待网络/其他业务对象.
            settings.changed = [delivery, index](comet::Subscriber::View view) { delivery->receive(index, view); };
            auto result = client.subscriber(scope, {}, std::move(settings));
            Measure::require(result.has_value());
            subscribers.push_back(std::move(*result));
        } else {
            comet::Observer::Options settings;
            settings.changed = [delivery, index](comet::Observer::View view) { delivery->receive(index, view); };
            auto result = client.observer(scope, {}, std::move(settings));
            Measure::require(result.has_value());
            observers.push_back(std::move(*result));
        }
    }
    until([&] { return std::ranges::all_of(subscribers, [&](const auto& item) { return item.watch().state() == comet::Subscriber::State::ready && item.watch().size() == options.records; }) && std::ranges::all_of(observers, [&](const auto& item) { return item.select().state() == comet::Observer::State::ready && item.select().size() == options.records; }); });

    // 热身二百毫秒只让初始推流/握手排空; 原生微基准和网络场景分别统计, 不混算吞吐.
    std::this_thread::sleep_for(200ms);
    std::mutex gate;                                                // 只在每个工作线程退出时合并样本, 不介入逐条测量.
    std::exception_ptr failure;                                     // 捕获首个工作线程失败, join 后再抛给 main.
    std::vector<std::int64_t> acknowledged;                         // 调用计划时刻到内存提交回执的纳秒延迟.
    std::vector<std::int64_t> installed;                            // 调用计划时刻到所有订阅已安装的纳秒延迟.
    const auto begin = Measure::Clock::now() + 100ms;               // 给全部工作线程准备时间, 使用公共采样起点.
    const auto end = begin + std::chrono::seconds(options.seconds); // 闭环停止发起新操作的边界.
    std::vector<std::jthread> workers;                              // 作用域内 join, 不让采样线程访问已析构句柄.
    for (std::size_t worker = 0; worker < options.writers; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                std::vector<std::int64_t> receipts, views; // 线程私有样本, 热路径无全局锁.
                receipts.reserve(10000);
                views.reserve(10000);
                std::this_thread::sleep_until(begin);
                for (std::uint64_t index = 0; options.rate ? index * options.writers + worker < static_cast<std::uint64_t>(options.rate) * options.seconds : Measure::Clock::now() < end; ++index) {
                    const auto planned = options.rate ? begin + std::chrono::nanoseconds(static_cast<std::int64_t>((index * options.writers + worker) * 1000000000ULL / options.rate)) : Measure::Clock::now(); // 固定到达模式从原计划计时, 不隐藏排队.
                    std::this_thread::sleep_until(planned);
                    Measure::require(Measure::Clock::now() < end + 30s); // 过载总预算, 不无限消化积压.
                    const auto version = index + 2;                      // 初始一, 同一写者逐次增长且永不重用.
                    auto value = content(options.bytes, version);        // 应用构建载荷成本属于端到端窗口.
                    if (options.catalog) {
                        completed(publishers[worker].publish(version, std::move(value)));
                    } else {
                        completed(beacons[worker].update(std::move(value)));
                    }
                    receipts.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Measure::Clock::now() - planned).count());

                    // 等待最慢订阅确认该条内容, 闭环吞吐是完整可见完成率, 不冒充服务器最大受理率.
                    delivery->wait(worker, version);
                    views.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Measure::Clock::now() - planned).count());
                }
                const std::lock_guard lock(gate); // 线程结束后才合并, 分配不污染该线程的延迟.
                acknowledged.insert(acknowledged.end(), receipts.begin(), receipts.end());
                installed.insert(installed.end(), views.begin(), views.end());
            } catch (...) {
                const std::lock_guard lock(gate);
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        });
    }
    workers.clear(); // jthread 析构等待本次有界工作完成, 不并行启动另一场景.
    if (failure) {
        std::rethrow_exception(failure);
    }
    const auto elapsed = std::chrono::duration<double>(Measure::Clock::now() - begin).count(); // 包含末条排空及采样循环开销.
    const auto count = installed.size();                                                       // 只有提交及所有订阅可见均成功的次数.
    Measure::require(count > 0);
    Measure::report("comet.commit", std::move(acknowledged), elapsed, count);
    Measure::report("comet.visible", std::move(installed), elapsed, count);
    for (const auto& client : clients) {
        Measure::require(client.exceptions() == 0);
    }
}
} // namespace

// 参数由项目内运行器产生; 所有数据量限制在已批准 VM 的首轮预算内.
int main(int count, char** arguments) {
    try {
        Measure::require(count == 12 || count == 13);
        Options options;
        std::string endpoints(arguments[1]); // 逗号分隔的回环地址, 不接受空端点.
        Measure::require(!endpoints.empty() && endpoints.back() != ',');
        for (std::size_t offset = 0; offset < endpoints.size();) {
            const auto next = endpoints.find(',', offset);
            options.endpoints.push_back(endpoints.substr(offset, next - offset));
            Measure::require(!options.endpoints.back().empty());
            offset = next == std::string::npos ? endpoints.size() : next + 1;
        }
        options.ca = arguments[2];
        if (count == 13) {
            std::ifstream file(arguments[12], std::ios::binary); // 仅允许夹具生成的固定 32 字节秘密, 不输出正文.
            options.secret.resize(32);
            Measure::require(static_cast<bool>(file.read(reinterpret_cast<char*>(options.secret.data()), 32)) && file.peek() == std::char_traits<char>::eof());
        }
        options.catalog = std::string_view(arguments[3]) == "catalog";
        Measure::require(options.catalog || std::string_view(arguments[3]) == "ephemeris");
        options.records = Measure::number(arguments[4], 1, 500);
        options.bytes = Measure::number(arguments[5], 16, 4096);
        options.attr = Measure::number(arguments[6], 0, 65536);
        options.watchers = Measure::number(arguments[7], 1, 50);
        options.writers = Measure::number(arguments[8], 1, 32);
        options.seconds = static_cast<unsigned>(Measure::number(arguments[9], 2, 30));
        options.ttl = static_cast<unsigned>(Measure::number(arguments[10], 1000, 600000));
        options.rate = static_cast<unsigned>(Measure::number(arguments[11], 0, 10000));
        Measure::require(options.writers <= options.records);
        run(options);
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
