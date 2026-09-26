#pragma once
#include "edition.hpp"
#include "gateway.hpp"
#include "library.hpp"
#include "progress.hpp"
#include "watch_kernel.hpp"
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace astra {
// Almanac 公共下行. gRPC 回调仅提交 I/O 结果, 既有控制循环有界 pump, 不创建每订阅线程.
// Library 的提交通知必须连接 changed, 才能在快照发送期间独立保留后缀而不赌全局历史窗口.
class Readout final : public proto::comet::v1::Almanac::CallbackService {
public:
    // 资源都是部署预算, 不固化为协议数字, 不等同于整个进程 RSS.
    struct Limits {
        // 包含接入、发送及取消等待 OnDone 的流, 默认 4096, 范围 1..65536.
        std::size_t streams = 4096;
        // 全部冻结批次和合并后缀的保守逻辑字节, 默认 256 MiB.
        std::size_t bytes = 256 * 1024 * 1024;
        // 每条流的合并后缀上限, 默认 8 MiB; 不把整个快照复制进此队列.
        std::size_t pending = 8 * 1024 * 1024;
        // 一页写入的最大本地单调等待时间, 默认 30 秒, 必须为正.
        std::chrono::milliseconds timeout{30000};
    };

    // library/gateway 活过全部 RPC, wake 只能唤醒共享控制循环; 构造不启动线程或开放端口.
    Readout(Library& library, Gateway& gateway, std::function<void()> wake);
    // 测试与部署可显式降低预算, 非法配置抛 invalid_argument.
    Readout(Library& library, Gateway& gateway, std::function<void()> wake, Limits limits);
    // 先 stop、排空 Server 并 pump 到 empty, 再释放服务; 不析构仍被 gRPC 使用的 reactor.
    ~Readout() override;
    // 验证地址和恢复位置, 登录许可与挂入事件索引定序; 禁止读取 __ 内部 Scope.
    grpc::ServerWriteReactor<proto::comet::v1::AlmanacWatchReply>* Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request) override;
    // Library 在 writer_ 内按提交顺序调用; 只合并不可变引用, 错误关闭受影响流而不撤回已提交写入.
    void changed(const Scope& scope, const Almanac::Change& change) noexcept;
    // 唯一控制线程最多推进 maximum 次, 默认 32; 每次先转交至多一条范围进度, 再处理一个就绪任务. 写超时每秒扫描, 不等待网络.
    void pump(std::chrono::steady_clock::time_point now, std::size_t maximum = 32);
    // 禁止新订阅并唤醒现有流取消; 仍须继续 pump, 直到全部 OnDone 被回收.
    void stop() noexcept;
    // 已没有本服务拥有的 RPC, 只表示 OnDone 已回收, 不代表整个 Server 停止.
    bool empty() const;

    // 只读分发计数快照, 全部初始零, 由 mutex_ 保护累加; 仅用于归因测量, 不影响预算或调度.
    struct Delivery {
        std::uint64_t scans{};   // changed 访问过的同范围流数, 不含无关精确目标.
        std::uint64_t matched{}; // 实际接纳变化进入后缀的流数, 不含重置/过滤事件.
    };

    // 返回正文收集访问与接纳次数, 不计范围进度通知及实际网络发送; 调用只取 mutex_ 快照.
    Delivery delivery() const;

private:
    // 共享拥有 reactor 的活动流, 实现中严格分开回调状态锁和合并后缀锁.
    class Stream;
    // 立即拒绝的 reactor 不占活动索引, 由自身 OnDone 删除.
    class Rejected;
    // 已持 mutex_ 时加入侵入式就绪队列, 每流至多一个位置, 不分配.
    void enqueue(Stream& stream) noexcept;
    // I/O 回调唤醒本流, 只取得 mutex_, 不访问 Library/Access.
    void signal(Stream& stream) noexcept;
    // 取得第一批完整投影; 持本流 I/O 锁, 已释放初检许可及全局队列锁; 发送前重新取得最终许可.
    std::expected<Edition, grpc::Status> initial(Stream& stream);
    // 单任务推进, 当前 reactor 的上下文直到 OnDone 取得同一 I/O 锁前始终有效.
    void advance(Stream& stream, std::chrono::steady_clock::time_point now);
    // 删除已完成索引并归还所有逻辑预算, 调用时持 mutex_, 不等待任何流.
    void retire(Stream& stream);
    // 固定原生路由, 只读取完整已安装状态.
    Library& library_;
    // 登录守卫及统一错误映射, 不重新比较 SECRET.
    Gateway& gateway_;
    // 唤醒函数不得阻塞或回调 pump, 不捕获短寿命 RPC.
    const std::function<void()> wake_;
    // 不在运行中变更计费规则.
    const Limits limits_;
    // 保护路由、就绪队列、计费及每流后缀; 接受页面后解锁再 StartWrite, 最终认证许可仍覆盖该提交.
    mutable std::mutex mutex_;

    // 地址只保存活动流的拥有者, 另加全范围/精确目标的非拥有视图与范围水位, 心跳下沉 pump.
    struct Group : Progress<Stream>::Group {
        std::set<Stream*> broadcast;                                   // 全范围流的非拥有视图, 每次提交都遍历.
        std::map<std::string, std::set<Stream*>, std::less<>> precise; // 精确目标到流的非拥有视图.
    };

    // 地址只保存有活动流的范围, 空范围在最后一条流退出后回收.
    std::map<Scope, Group> streams_;
    Progress<Stream> progress_;  // 只调度有提交的 Scope, 每次 pump 有界轮转, 不读 RPC 的 I/O 状态.
    astra::Watch<Stream> queue_; // 两条侵入式链分别管理就绪与在途, 每页发送不分配索引节点.
    // 活动流总数与全局已占用字节, 都由 mutex_ 保护.
    std::size_t count_{};
    std::size_t bytes_{};
    // changed 累计扫描与后缀接纳, 初始零, 只追加不重置; 用于核对精确目标的正文收集成本.
    std::uint64_t scans_{};
    std::uint64_t matched_{};
    // stop 可跨线程调用, 在 mutex_ 内单向设为 true.
    bool stopped_{};
    // 仅控制线程使用的低频写超时扫描截止, 不轮询所有空闲订阅的版本.
    std::chrono::steady_clock::time_point sweep_{};
};
} // namespace astra
