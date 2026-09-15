// 功能: 声明进程信号, 跨线程唤醒和有界诊断适配器, 明确资源恢复与持有边界.
#pragma once

#include <astra/policy.hpp>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <mutex>

namespace astra {
// 将已验证 UTF-8 字符串编码为独立 JSON 字面量, 转义引号、反斜杠和控制字节, 不解释 ID 格式.
std::string json_string(std::string_view value);
// 进程入口唯一持有信号适配器, 析构恢复进入前的处理器. 不能复制或交给 RPC 回调持有.
class Signals {
public:
    // 安装 SIGINT/SIGTERM 停止请求处理器并忽略 SIGPIPE; 安装失败回滚已改动处理器并抛出 runtime_error.
    Signals();
    // 恢复构造前的三个信号处理器, 要求进程内只存在一个此类所有者.
    ~Signals();
    // 禁止复制信号所有权, 避免重复恢复进程级处理器.
    Signals(const Signals&) = delete;
    // 禁止赋值转移处理器快照, 确保恢复目标始终对应本次安装.
    Signals& operator=(const Signals&) = delete;
    // 跨线程读取是否收到停止信号, 不阻塞, 不清除标志, 不在此函数中执行关闭.
    bool requested() const noexcept;

private:
    struct sigaction interrupt_{}, terminate_{}, pipe_{};
};

// Runtime 与回调共享这个独立唤醒对象, 回调不借用 Runtime. 序号覆盖处理期间到达的通知.
class Wakeup {
public:
    // 取得当前通知序号, 供控制循环在本轮处理前记录, 必须将同一值交给后续 wait.
    std::uint64_t observe() const noexcept;
    // 允许 gRPC 回调调用, 增加通知序号后唤醒等待者; 不访问 Runtime 或执行协议操作.
    void notify();
    // observed 必须在本轮 step 之前取得. 最多等待 10 ms, 同时轮询退出和 Channel 连接状态.
    void wait(std::uint64_t observed);

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic_uint64_t sequence_{0};
};

// 控制循环唯一持有. 只接收固定事件名和经过验证的字段, 不输出凭证或远端错误正文.
// 对管道和 socket 启用非阻塞写; 背压时丢弃单条诊断, 不建立日志队列或阻塞协议推进.
class Logger {
public:
    // role 决定固定组件名; stdout 为管道或 socket 时尽力启用非阻塞写并保存原标志.
    explicit Logger(Role role);
    // 若构造时成功读取 stdout 标志则恢复, 不关闭由进程拥有的标准输出描述符.
    ~Logger();
    // 禁止复制描述符恢复责任, 避免多个对象交错修改 stdout.
    Logger(const Logger&) = delete;
    // 禁止赋值替换唯一日志所有者及其原始 stdout 标志.
    Logger& operator=(const Logger&) = delete;
    // event 必须是固定安全事件名, fields 必须是已校验字段构成的 JSON 对象文本; 本函数不转义输入.
    // 仅尝试写入不超过 4096 字节的单条诊断, 忽略写失败且不建立重试队列.
    void write(std::string_view event, std::string_view fields = "{}") const;
    // 把 status 当前连接快照和本地 id 编码成公开诊断, 不读取身份材料或修改策略.
    void status(const NetworkStatus& status, const Id& id) const;
    // event 为固定安全事件名, error 只经白名单映射输出 reason, 不接收远端错误正文.
    void failure(std::string_view event, ErrorCode error) const;

private:
    std::string component_;
    int flags_ = -1;
};
} // namespace astra
