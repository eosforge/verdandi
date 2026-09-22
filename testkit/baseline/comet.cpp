#include "workload.hpp"
#include <comet/client.hpp>

namespace {
// 只使用公开 Comet API, 和 Redis 适配器保持相同逻辑对象及共享客户端数量.
class Adapter {
public:
    // 配置借用到析构结束; 初始化创建全部真实注册/发布对象, 不直接写 Star
    // 内部数据.
    explicit Adapter(const Workload::Options& options) : options_(options) {

        for (std::size_t index = 0; index < options.clients * 2; ++index) {
            comet::Client::Options settings; // 前半生产客户端, 后半消费客户端; 两侧无业务 TLS/认证.
            settings.endpoints = {options.endpoint};
            settings.auth = false;
            settings.tls = false;
            settings.readers = 128;
            clients_.push_back(take(comet::Client::open(std::move(settings))));
        }
        for (std::size_t record = 0; record < options.records; ++record) {
            auto& client = clients_[record % options.clients];        // 固定分配生产者, 同一对象没有并发写者.
            auto value = Workload::content(options.bytes, record, 1); // 初始内容版本固定一.
            if (options.catalog) {
                publishers_.push_back(take(client.publisher(scope(record % options.groups), "key" + std::to_string(record), std::chrono::milliseconds(options.ttl))));
                complete(publishers_.back().publish(1, std::move(value)));
            } else {
                beacons_.push_back(take(client.beacon(scope(record % options.groups), std::vector<std::uint8_t>(options.attr, 17), std::move(value), std::chrono::milliseconds(options.ttl))));
            }
        }

        // 一个消费者 Client 可拥有多个独立范围订阅, 不把 HTTP/2 流误计为独立 TCP
        // 连接.
        for (std::size_t watcher = 0; watcher < options.groups * options.fanout; ++watcher) {
            auto& client = clients_[options.clients + watcher % options.clients];
            if (options.catalog) {
                subscribers_.push_back(take(client.subscriber(scope(watcher / options.fanout))));
            } else {
                observers_.push_back(take(client.observer(scope(watcher / options.fanout))));
            }
        }
    }

    // 先请求全部核心停止再等待, 句柄及视图仍然有效; 析构不抛异常.
    ~Adapter() {
        for (auto& client : clients_) {
            client.close();
        }
        for (auto& client : clients_) {
            if (!client.wait(std::chrono::seconds(10))) {
                std::terminate();
            }
        }
    }

    // 调用返回必须意味着服务器已确认本次写入; 每个 record 的 version
    // 由共用负载递增.
    void write(std::size_t record, std::uint64_t version, std::vector<std::uint8_t> value) {
        if (options_.catalog) {
            complete(publishers_[record].publish(version, std::move(value)));
        } else {
            complete(beacons_[record].update(std::move(value)));
        }
    }

    // 只遍历公开只读 View; 不使用新 SDK 专有 changed 回调影响旧版对比口径.
    void scan(std::size_t watcher, auto&& receive) {
        if (options_.catalog) {
            const auto view = subscribers_[watcher].watch(); // 一轮读取同一完整版本.
            Workload::check(!view.error(), "Comet Subscriber error");
            view.each([&](std::string_view, const comet::Subscriber::Record& value) { receive(std::span<const std::uint8_t>(*value.value)); });
        } else {
            const auto view = observers_[watcher].select(); // 热路径核验 Attr 长度, 完整内容在正式窗口之后检查.
            Workload::check(!view.error(), "Comet Observer error");
            view.each([&](std::string_view, const comet::Observer::Record& value) {
                Workload::check(value.attr->size() == options_.attr, "Comet Attr length mismatch");
                receive(std::span<const std::uint8_t>(*value.data));
            });
        }
    }

    // 异步异常即整份样本失败, 不能将 SDK 恢复错误算作正常负载.
    void verify() {

        // 固定 Attr 不进入写入计时, 但最后必须核验内容, 不能仅凭长度宣告一致.
        for (const auto& observer : observers_) {
            observer.select().each([&](std::string_view, const comet::Observer::Record& value) { Workload::check(value.attr->size() == options_.attr && std::ranges::all_of(*value.attr, [](auto byte) { return byte == 17; }), "Comet final Attr mismatch"); });
        }

        for (const auto& client : clients_) {
            Workload::check(client.exceptions() == 0, "Comet asynchronous exception");
        }
    }

private:
    const Workload::Options& options_;           // 受验证配置, 覆盖适配器生命周期.
    std::vector<comet::Client> clients_;         // 两组独立共享网络核心.
    std::vector<comet::Publisher> publishers_;   // 每记录一个 Publisher, 与 Beacon 二选一.
    std::vector<comet::Beacon> beacons_;         // 保持自动续租, 不在负载中人为禁用.
    std::vector<comet::Subscriber> subscribers_; // 每范围拥有 fanout 个独立订阅对象.
    std::vector<comet::Observer> observers_;     // 每个对象安装自己的视图.

    // Scope 命名与旧版 Type/Part 对齐, 零基组号只用于测试数据隔离.
    static comet::Scope scope(std::size_t group) {
        return {"Baseline", "Group" + std::to_string(group)};
    }

    // 成功对象按值移出, 失败保留 SDK 错误码, 不隐式重试.
    template <class Result>
    static typename Result::value_type take(Result result) {
        if (!result) {
            throw std::runtime_error("Comet error " + std::to_string(static_cast<int>(result.error().code)));
        }
        return std::move(*result);
    }

    // future 的等待预算只约束测试; SDK 自身也有 RPC deadline.
    static void complete(auto future) {
        Workload::check(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "Comet operation timed out");
        static_cast<void>(take(future.get()));
    }
};
} // namespace

// 单独进程避免新版 BoringSSL 与旧版 OpenSSL 的链接符号相互影响.
int main(int count, char** arguments) {
    try {
        Workload::run<Adapter>(Workload::parse(count, arguments));
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
