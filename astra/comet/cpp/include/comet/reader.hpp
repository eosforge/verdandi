#pragma once
#include "types.hpp"
#include <functional>
#include <string_view>

namespace comet {
namespace detail {
class Projection;
class Core;
template <class Policy>
class Watching;
using Reading = Watching<Projection>;
struct Contents;
} // namespace detail
class Client;

// 持续读取一个 Almanac 范围, 私有网络状态与应用拥有的不可变 View 分离.
class Reader {
public:
    // 每次 load/回调取得时的本地状态, 不会反向修改应用已持有的旧 View.
    enum class State {
        waiting, // 尚未安装任何完整权威基线.
        ready,   // 最近批次完整安装且当前流没有已知失败.
        stale,   // 保留最后完整内容, 当前连接或恢复尚未完成.
        failed,  // 永久输入/容量/协议错误, 暂停自动恢复.
        closed   // 已停止本地业务, 旧内容仍可独立读取.
    };

    // 轻量拥有式视图, 查询不依赖 Reader/Client 仍然存活, 不返回可写容器或生成协议类型.
    class View {
    public:
        // 默认视图为 waiting, 没有完整版本且没有绑定范围.
        View() = default;
        State state() const noexcept;                          // 取得本视图时的状态, 默认 waiting.
        const Scope& scope() const noexcept;                   // 原始固定绑定, 默认两个空字段.
        const std::string& target() const noexcept;            // 空为整个 Scope, 非空为精确 Key.
        const std::string& instance() const noexcept;          // 最后完整批次的 Star, 未就绪为空.
        std::optional<std::uint64_t> version() const noexcept; // 只有完整安装后才有值, 显式零是合法基线.
        std::size_t size() const noexcept;                     // 最近完整投影的记录数, 默认零.
        const std::optional<Error>& error() const noexcept;    // 本次状态所附的受限诊断, 成功时为空.
        Value find(std::string_view key) const;                // 精确查找, 缺项返回空指针, 不发 RPC 或插入条目.

        // 同步借用稳定 Key/Value, 回调可抛异常或重入 SDK; 需要保留数据时复制 Value 所有权.
        // 此桥接只借用本次调用栈, 不分配 std::function 或把模板扩散到网络实现.
        void each(auto&& reader) const {
            auto callback = [&reader](std::string_view key, const Value& value) { std::invoke(reader, key, value); };
            visit(&callback, [](void* context, std::string_view key, const Value& value) { (*static_cast<decltype(callback)*>(context))(key, value); });
        }

    private:
        friend class detail::Projection;
        template <class Policy>
        friend class detail::Watching;
        // 非拥有类型擦除桥接, context 和 visitor 仅在本次同步调用有效.
        void visit(void* context, void (*visitor)(void*, std::string_view, const Value&)) const;
        // 完整内容只读共享, 不持有网络核心、RPC 上下文或应用回调.
        std::shared_ptr<const detail::Contents> contents_;
        // 状态包装与内容分离, 断线时不复制整个容器或改写旧状态.
        State state_ = State::waiting;
        // 本地绑定在未就绪时也能查询, 拷贝 View 只在这两个有界字段分配.
        Scope scope_;
        std::string target_;
        std::optional<Error> error_; // 未发生本次错误时为空.
    };

    struct Options {
        // 每对象可安装内容与容器的保守字节预算, 默认 64 MiB; 私有候选另允许有界替换峰值.
        std::size_t bytes = 64 * 1024 * 1024;
        // 每完整投影至多 65536 条, 超限停止恢复, 不无限请求同一快照.
        std::size_t records = 65536;
        // 在安装完成并释放 SDK 锁后调用, 同对象通知串行, 必须快速返回, 可以非阻塞 close.
        std::move_only_function<void(View)> changed;
    };

    // 空句柄不拥有业务; 工厂返回的实际句柄仅可移动, 不复制自动恢复责任.
    Reader() = default;
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader();                                          // 非阻塞 close, 不在析构等待网络或 join 线程.
    View load() const;                                  // 最近完整内容与当前状态, 不主动发请求.
    void close() noexcept;                              // 幂等停止该对象, 不关闭共享 Client 的其他对象.
    bool wait(std::chrono::milliseconds timeout) const; // 只等待本地清理, SDK 回调内禁止调用, 不隐式 close.

private:
    friend class Client;
    // 应用句柄与私有网络状态分开, 不因旧 View/future 存活而保留自动订阅.
    explicit Reader(std::shared_ptr<detail::Reading> reading);
    std::shared_ptr<detail::Reading> reading_; // 空表示默认或已移动句柄.
};
} // namespace comet
