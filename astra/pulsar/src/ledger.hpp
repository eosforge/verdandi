#pragma once
#include <astra/types.hpp>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace astra {
// Pulsar 的 SQLite 成员账本, 持久确认后发布不可变当前视图, 不承载业务数据.
class Ledger {
public:
    // 同一个部署摘要只存在一个当前成员. 旧启动请求另存精简绑定, 不缓存整个应答.
    using Members = std::map<std::string, Member, std::less<>>;
    // 打开专用库并独占服务锁. initialize 默认 false, 缺库失败; true 只允许显式创建新库.
    // galaxy 和 authority 绑定用途; maximum 为每角色上限, starts 为累计启动上限, 达限拒绝新登记.
    Ledger(const std::filesystem::path& path, std::string galaxy, std::string authority, std::size_t maximum, std::size_t starts, bool initialize = false);
    // 关闭文件并释放进程锁, 所有服务 handler 必须已停止.
    ~Ledger();
    // 禁止复制文件和写入责任.
    Ledger(const Ledger&) = delete;
    // 禁止覆盖持有独占服务锁和当前视图的账本.
    Ledger& operator=(const Ledger&) = delete;
    // 在写锁中校验重试或准备新成员, prepare 构造拥有数据的应答, 然后持久提交.
    // prepare 可以抛异常, 此时没有写盘或修改可见状态; 它不得重入本账本的登记方法.
    Result<void> register_member(Member candidate, std::string_view request_id, const std::function<void(const Member&, const Members&)>& prepare);
    // 用不可变快照验证当前凭证, 不等待登记的密码计算或磁盘提交锁.
    bool current(const Member& member) const;
    // 若 member 是捕获视图中的当前身份, 返回该不可变目录; 否则返回空指针.
    // 校验和读取共享同一次快照, 不将目录事实解释为节点在线状态.
    std::shared_ptr<const Members> snapshot(const Member& member) const;

private:
    // 每个请求只记部署和当时的成员代次, 已被替换的请求永远不能重新取得准入.
    struct Start {
        // 该启动请求对应的部署摘要文本, 独立拥有, 用于防止请求 ID 跨部署复用.
        std::string principal;
        // 该启动请求已提交的成员代次, 有效记录非零, 与当前部署记录核对后才允许幂等重试.
        std::uint64_t epoch{};
    };

    // 只验证新记录与旧状态的关系; 启动恢复原地建表, 线上提交另行复制小型当前成员表.
    Result<void> validate(const Member& member, std::string_view request_id, const Members& view) const;
    // 隐藏 SQLite 连接, 预备语句与进程锁, 不将第三方类型暴露到使用者头文件.
    struct Database;
    // 原子持久化当前成员与启动绑定, 完整 uint64 代次编码成八字节大端 BLOB.
    // 任何数据库异常都隔离写入, 旧内存视图保留, 必须重启确认实际提交结果.
    bool commit(const Member& member, std::string_view request, std::string_view payload);
    // 校验已知 Schema, 绑定及全部成员/启动历史, 一次性恢复内存, 不导入旧日志.
    void restore();
    // 只由登记 handler 竞争, Pulse 读取不获取此锁.
    std::mutex mutex_;
    // 发布后的成员快照不可变, atomic 成对转交新视图的生命周期.
    std::atomic<std::shared_ptr<const Members>> members_{std::make_shared<const Members>()};
    // 历史启动索引, 有硬容量上限, 只在写锁下访问.
    std::map<std::string, Start, std::less<>> starts_;
    // 唯一 Galaxy, 不允许请求切换或在错误数据库上继续登记.
    std::string galaxy_;
    // 每角色成员容量和累计启动容量.
    std::size_t maximum_, maximum_starts_;
    // 独占 SQLite 连接和服务锁, 在成员声明之外完整定义, 析构后释放全部句柄.
    std::unique_ptr<Database> database_;
    // I/O 错误后保持 false, 必须重启回放确认实际落盘结果.
    bool writable_ = true;
};
} // namespace astra
