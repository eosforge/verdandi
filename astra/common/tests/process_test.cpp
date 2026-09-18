#include "check.hpp"
#include "process.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <type_traits>
#include <unistd.h>

using namespace astra;
using namespace std::string_view_literals;

namespace {
static_assert(!std::is_copy_constructible_v<Signals> && !std::is_move_constructible_v<Signals>);
static_assert(!std::is_copy_constructible_v<Logger> && !std::is_move_constructible_v<Logger>);

// 检查 SIGINT/SIGTERM 停止标志和 SIGPIPE 忽略行为, 每个作用域退出均恢复原处理器.
void signal_restore() {

    // numbers 为当前夹具接管的三个信号, original 与其逐槽对应.
    constexpr std::array numbers{SIGINT, SIGTERM, SIGPIPE};
    // original 保存测试前处理器快照, 用于对象退出后的精确比较.
    std::array<struct sigaction, numbers.size()> original{};
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        CHECK(sigaction(numbers[i], nullptr, &original[i]) == 0);
    }
    for (const auto number : {SIGINT, SIGTERM}) {
        Signals signals;
        CHECK(!signals.requested());
        CHECK(raise(number) == 0);
        CHECK(signals.requested());
        CHECK(raise(SIGPIPE) == 0);
    }
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        // current 接收作用域退出后的实际处理器, 与 original 的相同信号槽比较.
        struct sigaction current{};
        CHECK(sigaction(numbers[i], nullptr, &current) == 0);
        CHECK(current.sa_handler == original[i].sa_handler);
    }
}

// 只改这个测试进程的 stdout, 作用域退出先恢复原描述符, 再关闭自己创建的管道.
class Pipe {
public:
    // 备份本进程 stdout 并导向独占管道, 读端非阻塞, 不触碰其他进程输出.
    Pipe() {

        CHECK(pipe2(ends_.data(), O_CLOEXEC) == 0);
        saved_ = dup(STDOUT_FILENO);
        CHECK(saved_ >= 0);
        CHECK(dup2(ends_[1], STDOUT_FILENO) >= 0);
        CHECK(fcntl(ends_[0], F_SETFL, O_NONBLOCK) == 0);
    }

    // 先恢复 stdout 再关闭备份和管道端点, 保留后续测试日志输出.
    ~Pipe() {

        dup2(saved_, STDOUT_FILENO);
        close(saved_);
        close(ends_[0]);
        close(ends_[1]);
    }

    // 禁止复制 stdout 恢复责任, 避免交错还原描述符.
    Pipe(const Pipe&) = delete;
    // 禁止赋值覆盖活动重定向, 每个实例独立负责完整的恢复过程.
    Pipe& operator=(const Pipe&) = delete;

    // 读取一条已生成的诊断记录, 当前无字节视为断言失败, 返回独立字符串.
    std::string read() const {

        // bytes 为单条日志的最大读取缓冲, 大小与生产的有界写入上限一致.
        std::array<char, 4096> bytes{};
        // count 为读到的实际字节数, 必须正数后才能转换为字符串长度.
        const auto count = ::read(ends_[0], bytes.data(), bytes.size());
        CHECK(count > 0);
        return {bytes.data(), static_cast<std::size_t>(count)};
    }

private:
    // ends_[0]/ends_[1] 分别拥有管道读端和写端, 构造成功后均有效.
    std::array<int, 2> ends_{};
    // saved_ 保存原 stdout 的复制描述符, -1 表示尚未备份.
    int saved_ = -1;
};

// 验证日志字段,非阻塞输出和管道满时不等待, 最后恢复 stdout 原标志.
void bounded_log() {

    Pipe output;
    // flags 保存 Logger 创建前的 stdout 文件状态, 销毁后应精确恢复.
    const auto flags = fcntl(STDOUT_FILENO, F_GETFL);
    CHECK((flags & O_NONBLOCK) == 0);
    {
        Logger logger(Member::Role::planet);
        CHECK((fcntl(STDOUT_FILENO, F_GETFL) & O_NONBLOCK) != 0);
        logger.failure("connect_failed", Status::Code::identity);
        // record 独立拥有实际日志行, 同时验证结构,组件,事件和固定错误词.
        const auto record = output.read();
        CHECK(record.ends_with("}\n"));
        CHECK(record.contains("\"component\":\"planet\""));
        CHECK(record.contains("\"event\":\"connect_failed\""));
        CHECK(record.contains("\"reason\":\"identity\""));
        // 写满管道后调用相同日志路径. CTest 超时会捕获任何阻塞回归, 无需依赖微秒级计时断言.
        std::array<char, 4096> padding{};
        while (::write(STDOUT_FILENO, padding.data(), padding.size()) > 0) {}
        CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
        logger.write("stopped");
    }
    CHECK(fcntl(STDOUT_FILENO, F_GETFL) == flags);
}

// 生产者发出一万次通知, 消费者交错 observe/wait, 验证不丢失序号推进.
void concurrent_wakeup() {

    Wakeup wake;
    // producer 借用 wake 发布固定次数通知, 离开作用域时先 join 再销毁 Wakeup.
    std::jthread producer([&] {
        for (unsigned i = 0; i < 10000; ++i) {
            wake.notify();
        }
    });
    for (;;) {
        // observed 是本次检查前的通知序号, wait 只在未变化时等待.
        const auto observed = wake.observe();
        if (observed == 10000) {
            break;
        }
        wake.wait(observed);
    }
}

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
int main() {

    // 不透明 id 的控制字符不能破坏逐行 JSON 日志, Unicode 原值必须保留.
    CHECK(json_string("星体/\"\\\n\0"sv) == "\"星体/\\\"\\\\\\u000a\\u0000\"");
    signal_restore();
    bounded_log();
    concurrent_wakeup();
    std::cout << "Process signal, log backpressure and wakeup checks passed\n";
}
