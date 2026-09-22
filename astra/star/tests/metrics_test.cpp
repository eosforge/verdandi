#include "check.hpp"
#include "metrics.hpp"
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace {
// 每次请求拥有独立回环 socket, 所有异常路径都关闭自己的连接.
class Client {
public:
    // 连接测试指标端口, 系统读写超时 3 秒; port 由实际绑定取得.
    explicit Client(std::uint16_t port) {

        descriptor_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(descriptor_ >= 0);
        sockaddr_in address{}; // 固定回环地址, 不接触测试夹具之外的服务.
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const timeval timeout{3, 0}; // 覆盖指标自身两秒总期限, 避免测试永久阻塞.
        if (::setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 || ::setsockopt(descriptor_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 || ::connect(descriptor_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(descriptor_);
            throw std::runtime_error("Cannot connect metrics fixture");
        }
    }

    // 唯一描述符在本夹具退出时关闭.
    ~Client() {
        ::close(descriptor_);
    }

    // 禁止复制释放责任.
    Client(const Client&) = delete;
    // 禁止赋值覆盖仍活动的请求.
    Client& operator=(const Client&) = delete;

    // 循环发送测试头部, MSG_NOSIGNAL 防止对端拒绝产生进程信号.
    void send(std::string_view request) {
        while (!request.empty()) {
            const auto count = ::send(descriptor_, request.data(), request.size(), MSG_NOSIGNAL);
            CHECK(count > 0);
            request.remove_prefix(static_cast<std::size_t>(count));
        }
    }

    // 读取到服务器关闭, 不以一次 recv 恰好返回整包为前提.
    std::string read() {

        std::string result; // 小型只读响应, 夹具同样设明确字节上限.
        std::array<char, 2048> buffer{};
        for (;;) {
            const auto count = ::recv(descriptor_, buffer.data(), buffer.size(), 0);
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && errno == ECONNRESET)
                break;
            CHECK(count >= 0);
            if (count == 0)
                break;
            result.append(buffer.data(), static_cast<std::size_t>(count));
            CHECK(result.size() <= 8192);
        }
        return result;
    }

private:
    int descriptor_ = -1; // 只持有本夹具创建的 TCP 连接.
};

// 覆盖真实 HTTP 边界、半头不阻塞其他抓取、读超时与停止资源归还.
void exchange() {

    astra::Metrics metrics(*astra::Endpoint::parse("127.0.0.1:0", true));
    const auto port = metrics.endpoint().port; // 系统分配实际端口, 可与其他独立测试并行.
    CHECK(port != 0);
    CHECK(metrics.publish({.instance = "star-1", .ready = true, .almanac = true, .clock = true, .members = 3, .sessions = 2, .recovery = 123}));
    Client slow(port); // 只发送不完整头部, 不能独占工作线程阻塞第二个客户端.
    slow.send("GET /metrics HTTP/1.1\r\n");
    Client normal(port);
    normal.send("GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto response = normal.read();
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(response.contains("X-Astra-Instance: star-1\r\n"));
    CHECK(response.contains("astra_ready 1\n") && response.contains("astra_recovery_bytes 123\n"));
    CHECK(slow.read().empty()); // 固定总期限到达即关闭, 不因客户端持续静默永久占槽.

    for (const auto request : {"POST /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n", "GET /metrics?key=secret HTTP/1.1\r\nHost: localhost\r\n\r\n", "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"}) {
        Client invalid(port);
        invalid.send(request);
        CHECK(invalid.read().empty());
    }
    CHECK(!metrics.publish({.instance = "bad\r\nInjected: header"})); // 无效发布保留先前有效观测.
    Client retained(port);
    retained.send("GET /metrics HTTP/1.0\r\n\r\n");
    CHECK(retained.read().contains("X-Astra-Instance: star-1\r\n"));
    CHECK(metrics.publish({.instance = "star-2"}));
    Client replaced(port);
    replaced.send("GET /metrics HTTP/1.0\r\n\r\n");
    CHECK(replaced.read().contains("astra_ready 0\n"));
}
} // namespace

// 本测试只在显式授权的 CTest 中启动私有监听; 未运行不作为验收证据.
int main() {
    try {
        exchange();
        std::cout << "PASS bounded metrics HTTP lifecycle\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
