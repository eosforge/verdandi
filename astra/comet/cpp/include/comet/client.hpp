#pragma once
#include "beacon.hpp"
#include "observer.hpp"
#include "publisher.hpp"
#include "reader.hpp"
#include "subscriber.hpp"
#include <filesystem>

namespace comet {
// 同一活动 Star 的共享原生客户端, 内部保持独立的长期流/unary Channel, 不接入 Pulsar.
class Client {
public:
    struct Options {
        // 按顺序有界切换的 Star 业务地址, 必须 1..64 个不同 HOST:PORT, 不并行连接整个列表.
        std::vector<std::string> endpoints;
        // 默认要求公共登录, false 明确匿名, 失败不会自动降级.
        bool auth = true;
        // 默认校验服务端 TLS, false 明确明文, 不改变协议或增加自研消息签名.
        bool tls = true;
        // 固定 APIKEY, 认证开启时 1..128 字节, 不能在现有 Client 上替换所属账号.
        std::string key;
        // 完整 SECRET 初值, 认证开启时 1..4096 字节, 后续用 secret() 替换, 不回显.
        std::vector<std::uint8_t> secret;
        // 可选外部 CA PEM 文件, 优先于编译嵌入根; 两者都无时使用 gRPC 系统信任根.
        std::filesystem::path ca;
        // 订阅对象及实际 RPC 各自的上限, 默认 64, 包括取消尚未 OnDone 的旧流.
        std::size_t readers = 64;
        std::size_t beacons = 1024; // 自动写对象总上限, Publisher/Beacon 共用, 与 Watch 额度分开.
        // Client 当前内容及准备的受控总预算, 默认 256 MiB, 不强制回收应用持有的旧 View.
        std::size_t bytes = 256 * 1024 * 1024;
        // Session 首次确认及连接尝试的本地超时, 默认 3 秒, 确认后不会用它结束长期 Session.
        std::chrono::milliseconds timeout{3000};
    };

    // 只完成本地校验和异步核心接纳, 成功不代表远端服务已就绪或登录已成功.
    static Result<Client> open(Options options);
    Client() = default;                             // 空句柄, 不创建线程/网络.
    Client(const Client&) = default;                // 共享同一应用拥有者与核心, 不再次认证.
    Client(Client&&) noexcept = default;            // 转移一个拥有者引用, 移动后为空.
    Client& operator=(const Client&) = default;     // 释放旧拥有者后共享新核心.
    Client& operator=(Client&&) noexcept = default; // 标准共享指针的非阻塞移交.
    ~Client() = default;                            // 最后一个 Client 释放不关闭仍有业务对象拥有的核心.
    // 更新同一 APIKEY 的 SECRET, 只影响之后的认证, 不修改任何 Star 的凭据.
    Result<void> secret(std::vector<std::uint8_t> value);
    // 接纳固定范围的 Almanac 读取, changed 可在返回前由另一执行路径调用.
    Result<Reader> reader(Scope scope, std::string target = {}, Reader::Options options = {});
    // 固定整个 Ephemeris 分组或规范 UUID, 与 Reader 共用 Client 接入和有界调度.
    Result<Observer> observer(Scope scope, std::string target = {}, Observer::Options options = {});
    Result<Subscriber> subscriber(Scope scope, std::string target = {}, Subscriber::Options options = {});                                                           // 动态 Catalog, 每条记录有独立内容版本.
    Result<Beacon> beacon(Scope scope, std::vector<std::uint8_t> attr, std::vector<std::uint8_t> data, std::chrono::milliseconds ttl, Beacon::Options options = {}); // 仅表示本地接纳, 实际身份由状态/通知确认.
    Result<Publisher> publisher(Scope scope, std::string key, std::chrono::milliseconds ttl, Publisher::Options options = {});                                       // 不预发布空内容, 首次 publish 才建立期望.
    void close() noexcept;                                                                                                                                           // 显式关闭所有所属业务对象, 非阻塞且幂等.
    bool wait(std::chrono::milliseconds timeout) const;                                                                                                              // 等待本地网络和通知清理, 不隐式 close, SDK 回调内禁止等待.
    std::uint64_t exceptions() const noexcept;                                                                                                                       // 观察者抛出的异常累计数, 不含敏感正文; 空 Client 返回 0.

private:
    struct Owner;                                  // 应用 Client 引用与内部 RPC 引用分开计数, 避免后台任务自我保活泄漏.
    explicit Client(std::shared_ptr<Owner> owner); // 只由 open 在本地校验完整通过后构造.
    std::shared_ptr<Owner> owner_;                 // 复制 Client 共享这个应用拥有者, 不把每次内部回调算成用户.
};
} // namespace comet
