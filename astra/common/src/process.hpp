// 详细说明: 本头文件提供了系统级别的抽象封装，用于处理进程终止信号、跨线程的同步唤醒机制，
// 以及一个非阻塞的有界结构化（JSON）日志记录器。目的是为了在服务端应用中安全、高效地进行资源生命周期管理与可观测性输出。
#pragma once

#include <astra/policy.hpp>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <mutex>

namespace astra {
// 详细说明: 遍历输入的字符串，并对其中的控制字符、双引号（"）和反斜杠（\）等特殊字符进行转义，
// 生成符合 JSON 标准的字符串表达形式。此函数不对业务上的 ID 格式进行额外校验，只负责编码。
// - value (std::string_view): 待编码的只读字符串片段，默认不提供值。必须是合法的 UTF-8 字符串。
// 返回值: 编码并转义后的 JSON 字符串（包含头尾的双引号）。
std::string json_string(std::string_view value);

// 进程入口唯一持有信号适配器, 析构恢复进入前的处理器. 不能复制或交给 RPC 回调持有.
// 详细说明: 这是一个 RAII 风格的信号处理类，用于接管操作系统的终止信号（如 SIGINT, SIGTERM）
// 并忽略 SIGPIPE 信号，确保程序能够在收到外部中断时优雅地进行清理，而不是被强杀。
class Signals {
public:
    // 构造函数: 安装 SIGINT/SIGTERM 的停止请求处理器并忽略 SIGPIPE 信号。
    // 详细说明: 在构造时，会替换当前的信号处理函数。如果任意一个信号处理器安装失败，
    // 它会自动回滚已经改动的处理器并抛出 std::runtime_error，保证系统状态不被破坏。
    Signals();

    // 析构函数: 恢复构造前保存的三个信号（SIGINT, SIGTERM, SIGPIPE）的处理器。
    // 详细说明: 确保在对象销毁（通常是进程主函数退出）时，系统的信号处理方式能够恢复原状。要求进程内只存在一个此类所有者。
    ~Signals();

    // 禁止复制构造函数: 禁止复制信号所有权, 避免多个实例在析构时重复恢复进程级处理器，导致状态混乱。
    Signals(const Signals&) = delete;

    // 禁止赋值操作符: 禁止赋值转移处理器快照, 确保恢复目标始终严格对应本次初始安装的快照。
    Signals& operator=(const Signals&) = delete;

    // 详细说明: 不阻塞线程，不清除已触发的信号标志，且不在此函数中执行实际的关闭逻辑。
    // 仅用于让其他线程（如控制循环）轮询检查是否需要退出。
    // 返回值: true 表示已经收到终止信号，false 表示未收到。
    bool requested() const noexcept;

private:
    // 保存 SIGINT 信号被覆盖前的原始处理动作配置 (默认值: 空结构体 {})
    struct sigaction interrupt_{};
    // 保存 SIGTERM 信号被覆盖前的原始处理动作配置 (默认值: 空结构体 {})
    struct sigaction terminate_{};
    // 保存 SIGPIPE 信号被覆盖前的原始处理动作配置 (默认值: 空结构体 {})
    struct sigaction pipe_{};
};

// 详细说明: Runtime 与回调共享这个独立的唤醒对象，使得回调不直接借用 Runtime 的引用。
// 采用序列号 (sequence) 的方式，完美覆盖了在处理期间可能瞬间到达的多个并发通知，避免丢失唤醒事件。
class Wakeup {
public:
    // 详细说明: 调用方需要记录这个值，并在后续调用 wait() 时传入同一值。
    // 返回值: 当前的通知序列号。
    std::uint64_t observe() const noexcept;

    // 详细说明: 增加内部通知序号并唤醒正在等待的线程。该函数设计为纯同步/唤醒操作，
    // 绝不直接访问 Runtime 或者执行任何具体的网络协议操作。
    void notify();

    // 详细说明: 最多等待 10 毫秒，避免永久阻塞。结合超时机制，调用方可以同时轮询退出标志和 Channel 连接状态。
    // - observed (std::uint64_t): 期望的通知序号，必须是在本轮步进（step）之前通过 observe() 取得的值。
    void wait(std::uint64_t observed);

private:
    // 互斥锁，保护 sequence_ 的原子一致性以及配合 condition_ 使用。
    std::mutex mutex_;
    // 条件变量，用于阻塞并等待跨线程的唤醒信号。
    std::condition_variable condition_;
    // 单调递增的通知序列号，用于检测是否有新的唤醒事件发生 (默认值: 0)。
    std::atomic_uint64_t sequence_{0};
};

// 控制循环唯一持有. 只接收固定事件名和经过验证的字段, 不输出凭证或远端错误正文.
// 详细说明: 对管道 (pipe) 和 socket 标准输出启用非阻塞写入。
// 遇到背压（无法立即写入）时，选择丢弃单条诊断日志，而不建立内存积压的日志队列，以防止阻塞网络协议的推进。
class Logger {
public:
    // 构造函数: 确定角色并初始化非阻塞的诊断输出。
    // - role (Member::Role): 枚举类型，决定固定的组件名（如 star 或是 planet）。
    // 详细说明: 当发现标准输出 (stdout) 是管道或 socket 时，尽力开启 O_NONBLOCK 非阻塞写标志，并保存原有的文件状态标志。
    explicit Logger(Member::Role role);
    // 服务进程入口使用固定组件名, 例如 pulsar; 不接受未经校验的远端文本.
    explicit Logger(std::string_view component);

    // 析构函数: 恢复 stdout 的原始配置。
    // 详细说明: 若构造时成功读取并修改了 stdout 的标志，则在此处恢复。这不会直接关闭进程持有的标准输出描述符，仅仅是恢复标志。
    ~Logger();

    // 禁止复制构造: 禁止复制描述符恢复责任, 避免多个 Logger 对象的生命周期交错导致混乱地修改 stdout 状态。
    Logger(const Logger&) = delete;

    // 禁止赋值操作: 禁止赋值替换唯一日志所有者及其保存的原始 stdout 标志。
    Logger& operator=(const Logger&) = delete;

    // - event (std::string_view): 必须是固定且安全的事件名，例如 "status"。
    // - fields (std::string_view): 必须是经过校验的字段所构成的 JSON 对象文本 (默认值: "{}")。本函数不会对输入再做转义。
    // 详细说明: 仅尝试向底层写入不超过 4096 字节（PIPE_BUF）的单条诊断记录，写入失败时静默忽略，不建立重试或缓冲队列。
    void write(std::string_view event, std::string_view fields = "{}") const;

    // - status (const Policy::NetworkStatus&): 当前的网络连接快照，包含成员信息、连接数等。
    // - id (const Id&): 本地节点的唯一标识。
    // 详细说明: 将上述信息编码为公开可见的 JSON 诊断日志。该函数不负责读取任何敏感身份材料，也不会修改现有的策略。
    void status(const Policy::NetworkStatus& status, const Id& id) const;

    // - event (std::string_view): 发生错误时的关联事件名称。
    // - error (Status::Code): 错误代码枚举值。
    // 详细说明: error 只会通过内置的白名单映射函数输出对应的字符串 reason (例如 "timeout")，坚决不接收/打印不受信任的远端错误正文内容。
    void failure(std::string_view event, Status::Code error) const;

private:
    // 固定的组件名称 (通常由 Member::Role 决定，如 "star" 或 "planet")。
    std::string component_;
    // 存储标准输出 (stdout) 在修改前的文件状态描述符标志，用于析构时恢复 (默认值: -1，代表尚未获取或不是 pipe/socket)。
    int flags_ = -1;
};
} // namespace astra
