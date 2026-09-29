#include "check.hpp"
#include "sampling.hpp"
#include <chrono>
#include <future>
#include <iostream>

namespace {
using namespace std::chrono_literals;

// 工作者全部占用时队列必须有界, 关闭拒绝新任务但仍排空已经接纳的清理责任.
void bounded() {

    std::promise<void> exit; // 主线程在成功或断言异常时都释放工作者.
    auto ready = exit.get_future().share();
    std::atomic_uint entered{}, completed{}; // 真实开始与结束数量, 不靠固定休眠猜测队列状态.
    auto sampling = comet::detail::Sampling::open();

    struct Release {
        std::promise<void>* exit; // 比线程管理器先析构, 避免失败路径自等.

        ~Release() {
            if (exit) {
                exit->set_value();
            }
        }
    } release{&exit};

    for (unsigned index = 0; index < 2; ++index) { // 两个工作者都先进入阻塞任务.
        CHECK(sampling->submit([&] { ++entered; ready.wait(); ++completed; }));
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (entered.load() != 2) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
    for (unsigned index = 0; index < 64; ++index) { // 尚未执行的固定候选槽.
        CHECK(sampling->submit([&] { ++completed; }));
    }
    CHECK(!sampling->submit([&] { ++completed; }));
    sampling->close();
    sampling->close(); // 幂等停止不会丢弃既有任务或减少真实工作者数量.
    CHECK(!sampling->finished() && !sampling->submit([&] { ++completed; }));
    exit.set_value();
    release.exit = nullptr;
    sampling->wait();
    sampling->wait(); // 多个顺序等待者同样安全.
    CHECK(sampling->finished() && completed.load() == 66);
}
} // namespace

// 独立 SDK 在 Linux 和 Windows 都验证队列容量及真实线程回收.
int main() {
    try {
        bounded();
        std::cout << "Sampling lifecycle cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
