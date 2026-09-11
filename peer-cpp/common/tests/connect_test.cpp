#include "admission.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace verdandi::peer;
#define CHECK(condition)                                                                                                                                       \
    do {                                                                                                                                                       \
        if (!(condition))                                                                                                                                      \
            throw std::runtime_error("Connect check failed at line " + std::to_string(__LINE__));                                                              \
    } while (false)

// 仅在 loopback 持有一个未响应的本地端点, 不依赖外部黑洞地址或修改系统 DNS.
struct Socket {
    int fd = -1;
    std::uint16_t port{};
    explicit Socket(int type) {
        fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        CHECK(fd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close(fd);
            throw std::runtime_error("Cannot bind test socket");
        }
        socklen_t size = sizeof(address);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0 || (type == SOCK_STREAM && listen(fd, 8) != 0)) {
            close(fd);
            throw std::runtime_error("Cannot prepare test socket");
        }
        port = ntohs(address.sin_port);
    }
    ~Socket() {
        close(fd);
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};

void blocked(const std::shared_ptr<Identity>& identity, std::string address, bool cancel) {
    Config config;
    config.cluster = "alpha";
    config.advertise = *Endpoint::parse("127.0.0.1:7443");
    config.supervisor = std::move(address);
    config.connect_timeout = Milliseconds(300);
    config.handshake_timeout = Milliseconds(600);
    Admission client(config, identity, *Identity::new_id(), [] {});
    client.begin(0);
    const auto started = Clock::now();
    while (Clock::now() - started < std::chrono::seconds(2)) {
        if (cancel && Clock::now() - started >= Milliseconds(50)) {
            client.cancel();
        }
        if (auto result = client.poll()) {
            CHECK(!*result && result->error().code == (cancel ? ErrorCode::cancelled : ErrorCode::timeout));
            CHECK(!client.pending());
            return;
        }
        std::this_thread::sleep_for(Milliseconds(1));
    }
    throw std::runtime_error("Blocked connection exceeded total budget");
}

int main() {
    try {
        auto identity = Identity::load(std::filesystem::path(VERDANDI_FIXTURES) / "peer-a", *Endpoint::parse("127.0.0.1:7443"));
        CHECK(identity);
        {
            Socket server(SOCK_STREAM);
            blocked(*identity, "127.0.0.1:" + std::to_string(server.port), false);
            const auto accepted = accept4(server.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            CHECK(accepted >= 0);
            close(accepted);
        }
        {
            Socket dns(SOCK_DGRAM);
            blocked(*identity, "dns://127.0.0.1:" + std::to_string(dns.port) + "/verdandi-test.invalid:7440", false);
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
