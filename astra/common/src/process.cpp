// 功能: 实现 Linux 信号恢复, 无丢失通知的唤醒等待和不排队的结构化诊断输出.
// 详细说明: 这个实现文件为 process.hpp 中的各个系统级和诊断工具类提供了具体的实现逻辑。
// 主要涉及 Linux signal API 调用、条件变量配合原子的线程同步、以及底层 write I/O 系统调用。
#include "process.hpp"

#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace astra {
namespace {
// 进程全局的停止请求原子标志 (默认值: false)
// 用于记录进程是否收到了 SIGINT 或 SIGTERM 信号。
std::atomic_bool stop_requested{false};
// 编译期断言：确保 std::atomic_bool 永远是无锁实现，
// 这是为了保证在 Unix 信号处理器（signal handler）中调用它也是绝对安全（async-signal-safe）的。
static_assert(std::atomic_bool::is_always_lock_free);

// 信号可能送到任意 gRPC 线程. 编译期确认无锁的原子标志满足信号安全和跨线程可见性.
// 参数:
// - int: 收到的具体信号编号，函数内未使用，仅为满足 sigaction 签名的要求。
// 详细说明: 该函数是注册到操作系统底层的信号回调。使用 memory_order_relaxed 是因为
// 我们仅仅需要设置这个布尔值状态，且依赖其他机制（如轮询）来同步数据，不需要严格的内存屏障。
void stop_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}
} // namespace

// 将已验证 UTF-8 字符串编码为独立 JSON 字面量, 转义引号、反斜杠和控制字节, 不解释 ID 格式.
// 参数:
// - value (std::string_view): 待转义的原始字符串片段。
// 返回值: 带有双引号且内部特殊字符已转义的 JSON 字符串。
std::string json_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (char byte : value) {
        const auto c = static_cast<unsigned char>(byte);
        // 如果遇到双引号或反斜杠，进行转义处理（前面加反斜杠）
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 0x20) {
            // 对于小于 0x20 的不可见控制字符，转义为 "\u00XX" 格式的 16 进制形式
            result += "\\u00";
            result += hex[c >> 4]; // 取得高四位
            result += hex[c & 15]; // 取得低四位
        } else {
            // 普通可见字符，直接追加
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}

// Signals 类的构造函数实现
// 详细说明: 负责将进程的停止标志置为 false，并挂载特定的信号处理函数。
Signals::Signals() {
    stop_requested.store(false, std::memory_order_relaxed);
    struct sigaction action{};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask); // 清空信号掩码
    
    // 安装 SIGINT 处理器（通常是 Ctrl+C），如果失败则直接抛异常。
    if (sigaction(SIGINT, &action, &interrupt_)) {
        throw std::runtime_error("Cannot install exit signal handlers");
    }
    // 安装 SIGTERM 处理器（通常是 kill 命令）。
    // 后续处理器安装失败时恢复已安装部分, 构造失败也不能遗留半套进程信号配置.
    if (sigaction(SIGTERM, &action, &terminate_)) {
        sigaction(SIGINT, &interrupt_, nullptr); // 回滚恢复 SIGINT
        throw std::runtime_error("Cannot install exit signal handlers");
    }
    
    // 安装 SIGPIPE 处理器为忽略 (SIG_IGN)，避免往断开的连接写数据导致进程崩溃。
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, &pipe_)) {
        sigaction(SIGINT, &interrupt_, nullptr); // 回滚恢复 SIGINT
        sigaction(SIGTERM, &terminate_, nullptr); // 回滚恢复 SIGTERM
        throw std::runtime_error("Cannot install pipe signal handler");
    }
}

// Signals 类的析构函数实现
// 详细说明: 负责将信号处理器恢复到类实例化之前的状态。
Signals::~Signals() {
    sigaction(SIGINT, &interrupt_, nullptr);
    sigaction(SIGTERM, &terminate_, nullptr);
    sigaction(SIGPIPE, &pipe_, nullptr);
}

// requested 方法实现
// 返回值: 进程是否收到停止请求的布尔状态，跨线程无锁访问。
bool Signals::requested() const noexcept {
    return stop_requested.load(std::memory_order_relaxed);
}

// observe 方法实现
// 返回值: 返回当前的 sequence_ 序列号，无锁读取。
std::uint64_t Wakeup::observe() const noexcept {
    return sequence_.load(std::memory_order_relaxed);
}

// notify 方法实现
// 详细说明: 递增序列号并通知一个等待在条件变量上的线程。
void Wakeup::notify() {
    {
        // 与 wait 的检查/入睡共用一把锁, 消除检查谓词后, 真正入睡前的通知窗口.
        // 如果这里不加锁，可能发生竞态：消费者刚判断完谓词发现无需等待但尚未真正进入休眠时，
        // 生产者递增了序列号并发出 notify，导致消费者永久错过该通知。
        std::lock_guard lock(mutex_);
        sequence_.fetch_add(1, std::memory_order_relaxed);
    }
    // 唤醒一个可能正在 wait() 的线程
    condition_.notify_one();
}

// wait 方法实现
// 参数:
// - observed (std::uint64_t): 期望的旧序列号。
// 详细说明: 如果当前的 observe() 不等于 observed，说明期间有 notify 发生，立即返回不等待。
// 否则最多阻塞 10 毫秒，超时或被唤醒且序列号改变后返回。
void Wakeup::wait(std::uint64_t observed) {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, Milliseconds(10), [&] { return observe() != observed; });
}

// Logger 构造函数实现
// 参数:
// - role (Role): 角色枚举，如果是 star 则 component 为 "star"，否则为 "planet"。
Logger::Logger(Role role) : component_(role == Role::star ? "star" : "planet") {
    struct stat status{};
    // 获取标准输出的文件状态
    if (fstat(STDOUT_FILENO, &status) == 0 && (S_ISFIFO(status.st_mode) || S_ISSOCK(status.st_mode))) {
        // 如果标准输出是管道 (FIFO) 或者套接字 (Socket)
        flags_ = fcntl(STDOUT_FILENO, F_GETFL); // 读取当前文件控制标志
        if (flags_ >= 0) {
            // 将标志追加 O_NONBLOCK 非阻塞写标志，并写回
            fcntl(STDOUT_FILENO, F_SETFL, flags_ | O_NONBLOCK);
        }
    }
}

// Logger 析构函数实现
// 详细说明: 恢复标志，前提是我们在构造时成功读取到了非负的 flags_ 标志。
Logger::~Logger() {
    if (flags_ >= 0) {
        fcntl(STDOUT_FILENO, F_SETFL, flags_);
    }
}

// write 方法实现
// 参数:
// - event (std::string_view): 事件名称。
// - fields (std::string_view): 格式化的 JSON 字段内容。
// 详细说明: 将诊断信息封装成标准化 JSON，并输出到标准输出 (STDOUT)。
void Logger::write(std::string_view event, std::string_view fields) const {
    // 获取当前的 Unix 毫秒时间戳
    const auto now = std::chrono::duration_cast<Milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    // 格式化输出的 JSON 字符串行，确保以换行符结尾
    const auto line = std::format(R"({{"time_unix_ms":{},"level":"INFO","component":"{}","event":"{}","fields":{}}})"
                                  "\n",
                                  now, component_, event, fields);
    // Linux 的 PIPE_BUF 为 4096, 小于等于此值的管道写不会输出半条 JSON.
    // 超过这个大小我们直接抛弃整条日志，以防阻塞。
    if (line.size() <= 4096) {
        // 忽略可能产生的写入失败，不建立缓存重试。
        const auto written = ::write(STDOUT_FILENO, line.data(), line.size());
        static_cast<void>(written); // 压制 unused-variable 警告
    }
}

// status 方法实现
// 参数:
// - status (const NetworkStatus&): 包含了详细的网络运行指标的数据结构。
// - id (const Id&): 节点的唯一身份。
void Logger::status(const NetworkStatus& status, const Id& id) const {
    // 格式化上游活跃成员（如果有的话）
    const auto upstream = status.active_member ? std::format(R"({{"id":{},"group":"{}","address":"{}"}})", json_string(status.active_member->id),
                                                             status.active_member->group, status.active_member->address.text())
                                               : "null";

    // 格式化并写入网络状态 JSON。直接调用写方法，并带上一系列经过校验和统计的值。
    write("status", std::format(R"({{"id":{},"initialized":{},"members":{},"inbound":{},"outbound":{},"planet_inbound":{},"candidates":{},"upstream":{}}})",
                                json_string(id), status.initialized, status.members, status.inbound, status.outbound, status.planet_inbound, status.candidates,
                                upstream));
}

// failure 方法实现
// 参数:
// - event (std::string_view): 事件分类名。
// - error (Error::Code): 枚举错误码。
// 详细说明: 强制只输出由内部白名单决定的 `Error::name(error)` 的值作为 reason，隔绝来自远端的恶意或非规范错误字符串。
void Logger::failure(std::string_view event, Error::Code error) const {
    write(event, std::format(R"({{"reason":"{}"}})", Error::name(error)));
}
} // namespace astra
