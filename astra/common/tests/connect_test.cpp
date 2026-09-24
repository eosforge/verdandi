#include "admission.hpp"
#include "check.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace astra;

namespace {

// 仅在 loopback 持有一个未响应的本地端点, 不依赖外部黑洞地址或修改系统 DNS.
struct Socket {
    // fd 独占测试 socket 描述符, -1 表示未创建, 仅限本进程回环端点.
    int fd = -1;
    // port 从零开始, 创建后记录系统分配的非零监听或 UDP 端口.
    std::uint16_t port{};

    // 按 type 创建非阻塞 TCP 或 UDP 回环端点, 构造失败关闭已创建的描述符.
    explicit Socket(int type) {

        fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        CHECK(fd >= 0);
        // address 使用 IPv4 回环和零端口, 由 bind 分配可用端口.
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close(fd);
            throw std::runtime_error("Cannot bind test socket");
        }

        // size 初始为地址缓冲宽度, getsockname 写回实际长度.
        socklen_t size = sizeof(address);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0 || (type == SOCK_STREAM && listen(fd, 8) != 0)) {
            close(fd);
            throw std::runtime_error("Cannot prepare test socket");
        }
        port = ntohs(address.sin_port);
    }

    // 关闭本夹具唯一拥有的 socket, 不影响其他监听器.
    ~Socket() {
        close(fd);
    }

    // 禁止复制描述符关闭责任, 防止同一 fd 被重复关闭.
    Socket(const Socket&) = delete;
    // 禁止覆盖已有描述符所有权, 夹具不提供赋值语义.
    Socket& operator=(const Socket&) = delete;
};

// 用 identity 连接不会回应的 address, cancel 指定是否提前取消, 返回时要求客户端已不再 pending.
void blocked(const std::shared_ptr<Identity>& identity, std::string address, bool cancel) {

    // config 将连接限制为 300 ms,握手总预算 600 ms, 不改变系统 DNS 配置.
    Config config;
    config.galaxy = "alpha";
    config.advertise = *Endpoint::parse("127.0.0.1:7443");
    config.pulsar = std::move(address);
    config.connect_timeout = Milliseconds(300);
    config.handshake_timeout = Milliseconds(600);
    Admission client(config, identity, [] {});
    client.begin(0);
    // started 为本次等待的单调起点, 取消在 50 ms 后触发, 总等待不超过两秒.
    const auto started = Steady::now();
    while (Steady::now() - started < std::chrono::seconds(2)) {
        if (cancel && Steady::now() - started >= Milliseconds(50)) {
            client.cancel();
        }
        if (auto result = client.poll()) {
            CHECK(!*result && result->error().code == (cancel ? Status::Code::cancelled : Status::Code::timeout));
            CHECK(!client.pending());
            return;
        }
        std::this_thread::sleep_for(Milliseconds(1));
    }
    throw std::runtime_error("Blocked connection exceeded total budget");
}

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
int main() {

    try {
        // identity 使用公开 TLS 夹具, 连接失败必须由所测停滞引起而不是缺少材料.
        auto identity = Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", *Endpoint::parse("127.0.0.1:7443"));
        CHECK(identity);
        {
            Socket server(SOCK_STREAM);
            blocked(*identity, "127.0.0.1:" + std::to_string(server.port), false);
            // accepted 确认 TCP 建连已实际到达测试监听, 随后只关闭这个新接受的描述符.
            const auto accepted = accept4(server.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            CHECK(accepted >= 0);
            close(accepted);
        }
        {
            Socket dns(SOCK_DGRAM);
            blocked(*identity, "dns://127.0.0.1:" + std::to_string(dns.port) + "/astra-test.invalid:7440", false);
            // query 接收本机 DNS 请求, 证明客户端确实查询了测试 UDP 端点.
            std::array<char, 512> query{};
            CHECK(recv(dns.fd, query.data(), query.size(), 0) > 0);
        }
        {
            Socket server(SOCK_STREAM);
            blocked(*identity, "127.0.0.1:" + std::to_string(server.port), true);
        }
        std::cout << "PASS stalled TLS, stalled local DNS and connection cancellation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
