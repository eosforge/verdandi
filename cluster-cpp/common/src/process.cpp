// 功能: 实现 Linux 信号恢复, 无丢失通知的唤醒等待和不排队的结构化诊断输出.
#include "process.hpp"

#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace verdandi::cluster {
namespace {
std::atomic_bool stop_requested{false};
static_assert(std::atomic_bool::is_always_lock_free);

// 信号可能送到任意 gRPC 线程. 编译期确认无锁的原子标志满足信号安全和跨线程可见性.
void stop_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}
} // namespace

std::string json_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (char byte : value) {
        const auto c = static_cast<unsigned char>(byte);
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 0x20) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}

Signals::Signals() {
    stop_requested.store(false, std::memory_order_relaxed);
    struct sigaction action{};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &interrupt_)) {
        throw std::runtime_error("Cannot install exit signal handlers");
    }
    // 后续处理器安装失败时恢复已安装部分, 构造失败也不能遗留半套进程信号配置.
    if (sigaction(SIGTERM, &action, &terminate_)) {
        sigaction(SIGINT, &interrupt_, nullptr);
        throw std::runtime_error("Cannot install exit signal handlers");
    }
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, &pipe_)) {
        sigaction(SIGINT, &interrupt_, nullptr);
        sigaction(SIGTERM, &terminate_, nullptr);
        throw std::runtime_error("Cannot install pipe signal handler");
    }
}

Signals::~Signals() {
    sigaction(SIGINT, &interrupt_, nullptr);
    sigaction(SIGTERM, &terminate_, nullptr);
    sigaction(SIGPIPE, &pipe_, nullptr);
}

bool Signals::requested() const noexcept {
    return stop_requested.load(std::memory_order_relaxed);
}

std::uint64_t Wakeup::observe() const noexcept {
    return sequence_.load(std::memory_order_relaxed);
}

void Wakeup::notify() {
    {
        // 与 wait 的检查/入睡共用一把锁, 消除检查谓词后, 真正入睡前的通知窗口.
        std::lock_guard lock(mutex_);
        sequence_.fetch_add(1, std::memory_order_relaxed);
    }
    condition_.notify_one();
}

void Wakeup::wait(std::uint64_t observed) {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, Milliseconds(10), [&] { return observe() != observed; });
}

Logger::Logger(Role role) : component_(role == Role::star ? "star" : "planet") {
    struct stat status{};
    if (fstat(STDOUT_FILENO, &status) == 0 && (S_ISFIFO(status.st_mode) || S_ISSOCK(status.st_mode))) {
        flags_ = fcntl(STDOUT_FILENO, F_GETFL);
        if (flags_ >= 0) {
            fcntl(STDOUT_FILENO, F_SETFL, flags_ | O_NONBLOCK);
        }
    }
}

Logger::~Logger() {
    if (flags_ >= 0) {
        fcntl(STDOUT_FILENO, F_SETFL, flags_);
    }
}

#include <format>

void Logger::write(std::string_view event, std::string_view fields) const {
    const auto now = std::chrono::duration_cast<Milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto line = std::format(R"({{"time_unix_ms":{},"level":"INFO","component":"{}","event":"{}","fields":{}}})""\n",
                                  now, component_, event, fields);
    // Linux 的 PIPE_BUF 为 4096, 小于等于此值的管道写不会输出半条 JSON.
    if (line.size() <= 4096) {
        const auto written = ::write(STDOUT_FILENO, line.data(), line.size());
        static_cast<void>(written);
    }
}

void Logger::status(const NetworkStatus& status, const Id& id) const {
    const auto upstream = status.active_member 
        ? std::format(R"({{"id":{},"group":"{}","address":"{}"}})", 
                      json_string(status.active_member->id), 
                      status.active_member->group, 
                      status.active_member->address.text())
        : "null";
        
    write("status", std::format(R"({{"id":{},"initialized":{},"members":{},"inbound":{},"outbound":{},"planet_inbound":{},"candidates":{},"upstream":{}}})",
                                json_string(id),
                                status.initialized ? "true" : "false",
                                status.members,
                                status.inbound,
                                status.outbound,
                                status.planet_inbound,
                                status.candidates,
                                upstream));
}

void Logger::failure(std::string_view event, ErrorCode error) const {
    write(event, std::format(R"({{"reason":"{}"}})", error_name(error)));
}
} // namespace verdandi::cluster
