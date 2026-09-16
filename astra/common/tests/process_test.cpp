// 功能: 验证进程信号恢复, 跨线程唤醒和诊断输出的边界行为.
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
static_assert(!std::is_copy_constructible_v<Signals> && !std::is_move_constructible_v<Signals>);
static_assert(!std::is_copy_constructible_v<Logger> && !std::is_move_constructible_v<Logger>);

void signal_restore() {
    constexpr std::array numbers{SIGINT, SIGTERM, SIGPIPE};
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
        struct sigaction current{};
        CHECK(sigaction(numbers[i], nullptr, &current) == 0);
        CHECK(current.sa_handler == original[i].sa_handler);
    }
}

// 只改这个测试进程的 stdout, 作用域退出先恢复原描述符, 再关闭自己创建的管道.
class OutputPipe {
public:
    OutputPipe() {
        CHECK(pipe2(ends_.data(), O_CLOEXEC) == 0);
        saved_ = dup(STDOUT_FILENO);
        CHECK(saved_ >= 0);
        CHECK(dup2(ends_[1], STDOUT_FILENO) >= 0);
        CHECK(fcntl(ends_[0], F_SETFL, O_NONBLOCK) == 0);
    }
    ~OutputPipe() {
        dup2(saved_, STDOUT_FILENO);
        close(saved_);
        close(ends_[0]);
        close(ends_[1]);
    }
    OutputPipe(const OutputPipe&) = delete;
    OutputPipe& operator=(const OutputPipe&) = delete;
    std::string read() const {
        std::array<char, 4096> bytes{};
        const auto count = ::read(ends_[0], bytes.data(), bytes.size());
        CHECK(count > 0);
        return {bytes.data(), static_cast<std::size_t>(count)};
    }

private:
    std::array<int, 2> ends_{};
    int saved_ = -1;
};

void bounded_log() {
    OutputPipe output;
    const auto flags = fcntl(STDOUT_FILENO, F_GETFL);
    CHECK((flags & O_NONBLOCK) == 0);
    {
        Logger logger(Role::planet);
        CHECK((fcntl(STDOUT_FILENO, F_GETFL) & O_NONBLOCK) != 0);
        logger.failure("connect_failed", Error::Code::identity);
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

void concurrent_wakeup() {
    Wakeup wake;
    std::jthread producer([&] {
        for (unsigned i = 0; i < 10000; ++i) {
            wake.notify();
        }
    });
    for (;;) {
        const auto observed = wake.observe();
        if (observed == 10000) {
            break;
        }
        wake.wait(observed);
    }
}

int main() {
    // 不透明 id 的控制字符不能破坏逐行 JSON 日志, Unicode 原值必须保留.
    CHECK(json_string("星体/\"\\\n\0"sv) == "\"星体/\\\"\\\\\\u000a\\u0000\"");
    signal_restore();
    bounded_log();
    concurrent_wakeup();
    std::cout << "Process signal, log backpressure and wakeup checks passed\n";
}
