#include "workload.hpp"
#include <verdandi/catalog/catalog.hpp>
#include <verdandi/registration/registration.hpp>
#include <verdandi/registration/selector.hpp>

namespace {
// 冻结旧 SDK 的公开 API 适配层, 不修改其线程、脚本或 Pub/Sub 行为.
class Adapter {
    using Registration = verdandi::registration::registration<verdandi::fields, verdandi::fields>; // 单字段完整二进制正文.
    using Selector = verdandi::registration::selector<verdandi::fields, verdandi::fields>;         // 借用投影避免 snapshot 的额外整表复制.

public:
    // 所有客户端位于本次独占 Redis 中, 每场景由外层清空本轮独占实例.
    explicit Adapter(const Workload::Options& options) : options_(options) {

        for (std::size_t index = 0; index < options.clients * 2; ++index) {
            verdandi::redis_configuration settings; // 连接池上限显式记录, 空闲连接及 Pub/Sub 数量由 SDK 决定.
            settings.addresses = {options.endpoint};
            settings.timeout = std::chrono::seconds(5);
            settings.pool.max_connections = 32;
            clients_.push_back(take(verdandi::client::open(settings)));
            if (options.catalog) {
                verdandi::catalog_configuration configuration; // 不启用本地 SQLite 检查点, 只测共同内存语义.
                configuration.zone = "Baseline";
                catalogs_.push_back(take(verdandi::catalog::client::open(clients_.back(), configuration)));
            } else {
                verdandi::registration_configuration configuration; // 基线默认零, 对照场景显式恢复旧版默认合并窗口.
                configuration.zone = "Baseline";
                configuration.selector.view_publish_interval = std::chrono::milliseconds(options.legacy);
                configuration.policy.attr_value_max_bytes = options.attr;
                configuration.policy.data_value_max_bytes = options.bytes;
                configuration.policy.record_max_bytes = 65536;
                registrations_.push_back(take(verdandi::registration::client::open(clients_.back(), configuration)));
            }
        }
        for (std::size_t record = 0; record < options.records; ++record) {
            const auto value = fields(Workload::content(options.bytes, record, 1)); // 初始正文和新版完全相同, 仅增加旧协议所需字段包装.
            if (options.catalog) {
                paths_.push_back(take(verdandi::catalog::path::create(group(record % options.groups), "key" + std::to_string(record))));
                publishers_.push_back(take(verdandi::catalog::publisher::create(catalogs_[record % options.clients])));
                static_cast<void>(take(publishers_.back().replace(paths_.back(), verdandi::catalog::kind::map, value)));
            } else {
                verdandi::registration::options settings; // 与新版相同的 TTL / 自动续租, 应用版本固定一.
                settings.type = group(record % options.groups);
                settings.ttl = std::chrono::milliseconds(options.ttl);
                settings.version = 1;
                records_.push_back(take(Registration::create(registrations_[record % options.clients], settings)));
                accepted(records_.back().publish(fields(std::vector<std::uint8_t>(options.attr, 17)), value));
            }
        }

        // 每个 watcher 代表独立 Selector/Subscriber,
        // 而不是同一对象上的多个应用回调.
        for (std::size_t watcher = 0; watcher < options.groups * options.fanout; ++watcher) {
            const auto client = options.clients + watcher % options.clients; // 消费客户端不复用生产者的连接池.
            if (options.catalog) {
                verdandi::catalog::subscription subscription; // 按 Part 对齐新版完整 Scope.
                subscription.parts = {group(watcher / options.fanout)};
                subscribers_.push_back(take(verdandi::catalog::subscriber::create(catalogs_[client], std::move(subscription))));
                entries_.emplace_back();
                for (std::size_t record = watcher / options.fanout; record < options.records; record += options.groups) {
                    entries_.back().push_back(subscribers_.back()->find(paths_[record]));
                    Workload::check(static_cast<bool>(entries_.back().back()), "Missing legacy Entry");
                }
            } else {
                selectors_.push_back(take(Selector::create(registrations_[client], {group(watcher / options.fanout)})));
            }
        }
    }

    // 各领域 Client 负责排空自身对象, 随后关闭根传输; 部分初始化失败也由成员 RAII
    // 释放.
    ~Adapter() {
        for (auto& client : catalogs_) {
            static_cast<void>(client.close());
        }
        for (auto& client : registrations_) {
            static_cast<void>(client.close());
        }
        for (auto& client : clients_) {
            static_cast<void>(client.close());
        }
    }

    // 旧版服务端产生自己的协议版本, 应用版本仍包含在共用正文中供端到端核验.
    void write(std::size_t record, std::uint64_t, std::vector<std::uint8_t> value) {
        const auto encoded = fields(value); // 旧 SDK Fields 接口的真实编码/复制成本属于该适配器.
        if (options_.catalog) {
            static_cast<void>(take(publishers_[record].replace(paths_[record], verdandi::catalog::kind::map, encoded)));
        } else {
            accepted(records_[record].update(encoded));
        }
    }

    // Selector 借用候选集, 返回空选择避免复制脱离记录. Catalog 通过公开 Entry
    // 解码.
    void scan(std::size_t watcher, auto&& receive) {
        if (options_.catalog) {
            for (const auto& entry : entries_[watcher]) {
                const auto loaded = take(entry->load<verdandi::fields>()); // 单一不可变状态,
                                                                           // 不拼接两次读取结果.
                if (loaded.value && loaded.state == verdandi::catalog::status::present) {
                    receive(bytes(*loaded.value));
                }
            }
        } else {
            const auto result = selectors_[watcher]->one([&](auto& candidates) -> verdandi::result<std::optional<verdandi::registration::choice>> {
                for (std::size_t index = 0; index < candidates.size(); ++index) {
                    const auto value = candidates.get(index); // 当前事务内借用, 不越过回调生命周期.
                    Workload::check(value.has_value(), "Invalid legacy candidate");
                    Workload::check(bytes(value->attr()).size() == options_.attr, "Legacy Attr length mismatch");
                    receive(bytes(value->data()));
                }
                return std::optional<verdandi::registration::choice>{};
            });
            if (!result) {
                const auto diagnostic = selectors_[watcher]->try_error(); // 失败时附带已有诊断, 不重试、不吞掉不可用状态.
                throw std::runtime_error(result.error().message() + (diagnostic ? " / " + diagnostic->message() : " / no queued diagnostic"));
            }
        }
    }

    // 任何后台同步/续租诊断使样本失败, 不将重连恢复代价悄悄过滤.
    void verify() {

        // 与 Comet 一致, 窗口外验证每个 Selector 的固定 Attr 正文, 不增加热路径扫描负担.
        for (const auto& selector : selectors_) {
            static_cast<void>(take(selector->one([&](auto& candidates) -> verdandi::result<std::optional<verdandi::registration::choice>> {
                for (std::size_t index = 0; index < candidates.size(); ++index) {
                    const auto value = candidates.get(index); // 借用当前事务内的固定 Attr.
                    Workload::check(value.has_value(), "Invalid final legacy candidate");
                    const auto attr = bytes(value->attr()); // 不复制 Fields 所持有的正文.
                    Workload::check(attr.size() == options_.attr && std::ranges::all_of(attr, [](auto byte) { return byte == 17; }), "Legacy final Attr mismatch");
                }
                return std::optional<verdandi::registration::choice>{};
            })));
        }

        for (auto& value : records_) {
            Workload::check(!value.try_error(), "Legacy registration diagnostic");
        }
        for (auto& value : selectors_) {
            Workload::check(!value->try_error(), "Legacy Selector diagnostic");
        }
        for (auto& value : subscribers_) {
            Workload::check(!value->try_error(), "Legacy Subscriber diagnostic");
        }
    }

private:
    const Workload::Options& options_;                                            // 借用有效配置, 不在后台改写.
    std::vector<verdandi::client> clients_;                                       // 根连接池, 最后析构.
    std::vector<verdandi::catalog::client> catalogs_;                             // Catalog 领域客户端.
    std::vector<verdandi::registration::client> registrations_;                   // 注册领域客户端.
    std::vector<verdandi::catalog::path> paths_;                                  // 每个逻辑记录固定 Path.
    std::vector<verdandi::catalog::publisher> publishers_;                        // 多个独立轻量发布句柄.
    std::vector<Registration> records_;                                           // 持有自动续租任务.
    std::vector<std::unique_ptr<Selector>> selectors_;                            // 不可移动旧版对象由唯一指针持有.
    std::vector<std::unique_ptr<verdandi::catalog::subscriber>> subscribers_;     // 独立订阅任务.
    std::vector<std::vector<std::shared_ptr<verdandi::catalog::entry>>> entries_; // 缓存公开稳定 Entry, 不是私有数据旁路.

    // Type 与 Part 共用同一组名, ASCII 首字母满足旧协议限制.
    static std::string group(std::size_t index) {
        return "Group" + std::to_string(index);
    }

    // Fields 仅包一个 body 字段, 不用 JSON/Base64 人为增加负载.
    static verdandi::fields fields(std::span<const std::uint8_t> value) {
        const auto source = std::as_bytes(value); // 标准字节视图, 复制到旧接口的拥有容器.
        return {{"body", verdandi::bytes(source.begin(), source.end())}};
    }

    // 借用本次 Fields 的正文, 调用方不得在字段对象销毁后保留 span.
    static std::span<const std::uint8_t> bytes(const verdandi::fields& value) {
        const auto found = value.find("body"); // 精确单字段, 不默默创建缺失项.
        Workload::check(found != value.end() && value.size() == 1, "Malformed legacy Fields");
        return {reinterpret_cast<const std::uint8_t*>(found->second.data()), found->second.size()};
    }

    // 错误保留 SDK 文本, 不让适配器重试掩盖请求失败.
    template <class Result>
    static typename Result::value_type take(Result result) {
        if (!result) {
            throw std::runtime_error(result.error().message());
        }
        return std::move(*result);
    }

    // void 结果和有值结果分开处理, 避免模板对 void 解引用.
    static void accepted(verdandi::result<void> result) {
        if (!result) {
            throw std::runtime_error(result.error().message());
        }
    }
};
} // namespace

// 独立旧版可执行文件, 退出码和新版遵循同一失败契约.
int main(int count, char** arguments) {
    try {
        Workload::run<Adapter>(Workload::parse(count, arguments));
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
