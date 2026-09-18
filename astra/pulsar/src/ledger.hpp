#pragma once
#include <astra/types.hpp>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace astra {
class MembershipLedger {
public:
    // 同一个部署摘要只存在一个当前成员. 旧启动请求另存精简绑定, 不缓存整个应答.
    using Members = std::map<std::string, Member, std::less<>>;
    // 打开或创建专用日志并独占锁定. 截断末尾未完成记录, 完整记录损坏则拒绝启动.
    // galaxy 和 authority 绑定日志用途; maximum 为每角色上限, starts 为累计启动上限, 达限拒绝新登记.
    MembershipLedger(const std::filesystem::path& path, std::string galaxy, std::string authority, std::size_t maximum, std::size_t starts);
    // 关闭文件并释放进程锁, 所有服务 handler 必须已停止.
    ~MembershipLedger();
    // 禁止复制文件和写入责任.
    MembershipLedger(const MembershipLedger&) = delete;
    MembershipLedger& operator=(const MembershipLedger&) = delete;
    // 在写锁中校验重试或准备新成员, prepare 构造拥有数据的应答, 然后持久提交.
    // prepare 可以抛异常, 此时没有写盘或修改可见状态; 它不得重入本账本的登记方法.
    Result<void> register_member(Member candidate, std::string_view request_id, const std::function<void(const Member&, const Members&)>& prepare);
    // 用不可变快照验证当前凭证, 不等待登记的密码计算或磁盘提交锁.
    bool current(const Member& member) const;

private:
    // 每个请求只记部署和当时的成员代次, 已被替换的请求永远不能重新取得准入.
    struct Start {
        std::string principal;
        std::uint64_t epoch{};
    };
    // 只验证新记录与旧状态的关系; 启动恢复原地建表, 线上提交另行复制小型当前成员表.
    Result<void> validate(const Member& member, std::string_view request_id, const Members& view) const;
    // 追加长度 + 链式 SHA256 + Protobuf, fdatasync 成功才返回 true; 失败后禁止继续写入.
    bool append(std::string_view payload);
    // 读取并回放已有日志, 只修复末尾物理不完整记录, 不吞掉校验或语义错误.
    void restore();
    // 只由登记 handler 竞争, Pulse 读取不获取此锁.
    std::mutex mutex_;
    // 发布后的成员快照不可变, atomic 成对转交新视图的生命周期.
    std::atomic<std::shared_ptr<const Members>> members_{std::make_shared<const Members>()};
    // 历史启动索引, 有硬容量上限, 只在写锁下访问.
    std::map<std::string, Start, std::less<>> starts_;
    // 唯一 Galaxy, 不允许请求切换或在错误日志上继续登记.
    std::string galaxy_;
    // 日志头包含格式版本, Galaxy 和签发公钥摘要, 不包含私钥.
    std::string header_;
    // 每角色成员容量和累计启动容量.
    std::size_t maximum_, maximum_starts_;
    // 进程持有的追加日志描述符, 析构关闭.
    int file_ = -1;
    // 最近完整记录的链摘要, 每次成功提交后更新.
    std::array<std::uint8_t, 32> chain_{};
    // I/O 错误后保持 false, 必须重启回放确认实际落盘结果.
    bool writable_ = true;
};
} // namespace astra
