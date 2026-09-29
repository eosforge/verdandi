#pragma once
#include "types.hpp"

namespace comet {
namespace detail {
class Publishing;
}
class Client;

// Catalog 同步发布器, 只绑定 Scope; 每次提交显式 TTL, 版本完全由 SDK 管理.
class Publisher {
public:
    struct Receipt {
        std::string instance;    // 本次确认的接入 Star, 不表示复制已完成.
        std::uint64_t version{}; // SDK 分配的正内容版本, 不作为全网事务 ID.
    };

    // 同批 1..128 个唯一 UTF-8 Key, 键与正文合计至多 1 MiB.
    struct Entry {
        std::string key; // 1..1024 字节, 同一个 Key 的唯一写入者由应用保证.
        Value value;     // 共享不可变完整值, nullptr 拒绝, 空 vector 合法.
    };

    Publisher() = default;                           // 空句柄, 不创建网络或定时器.
    Publisher(Publisher&&) noexcept;                 // 移交唯一公开业务句柄.
    Publisher& operator=(Publisher&&) noexcept;      // 先关闭原对象, 再移交.
    Publisher(const Publisher&) = delete;            // 不复制业务生命周期.
    Publisher& operator=(const Publisher&) = delete; // 不复制业务生命周期.
    ~Publisher();                                    // 非阻塞取消自己的调用, 不关闭共享 Client.

    // 同步原子提交完整批次; ttl 为 1s..10min, timeout 为 (0, 1min].
    // 同一目标和精确 Key 集合复用已确认版本基线, 其他情况先查询; 不保留 Data.
    // 一个 deadline 覆盖连接、查询和最多一次明确版本冲突修复; 超时可能已提交.
    // 同对象并发调用返回 busy, 不合并请求; SDK 通知回调内同步写入返回 busy.
    // 返回后没有待重发内容和自动续租, 缓冲只保留到当前 RPC 真正完成.
    Result<Receipt> update(std::vector<Entry> entries, std::chrono::milliseconds ttl, std::chrono::milliseconds timeout = std::chrono::seconds(3));
    // 单键是批次的特例, vector 转移正文所有权, 不暴露 version 参数.
    Result<Receipt> update(std::string key, std::vector<std::uint8_t> data, std::chrono::milliseconds ttl, std::chrono::milliseconds timeout = std::chrono::seconds(3));
    void close() noexcept;                              // 幂等立即停止本地接纳, 取消当前 RPC.
    bool wait(std::chrono::milliseconds timeout) const; // 仅等待实际调用排空, 不隐式 close.

private:
    friend class Client;
    explicit Publisher(std::shared_ptr<detail::Publishing> publishing); // 仅工厂完成接纳后构造.
    std::shared_ptr<detail::Publishing> publishing_;                    // 内部调用自行保持生命周期.
};
} // namespace comet
