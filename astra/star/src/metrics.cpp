#include "metrics.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <poll.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace astra {

struct Metrics::Server {
    // 每个描述符只有一个所有者, -1 表示尚未打开; 关闭不重试以免误关复用的编号.
    struct File {
        int value = -1; // Linux 文件描述符, 成功打开后非负.

        // 默认空描述符; 固定槽位原位管理, 不需要移动所有权.
        File() = default;
        // 禁止复制同一个描述符的释放责任.
        File(const File&) = delete;
        // 禁止赋值覆盖仍然打开的描述符.
        File& operator=(const File&) = delete;

        // 释放本对象唯一持有的描述符.
        ~File() {
            reset();
        }

        // 清空并关闭旧描述符, next=-1 仅释放; 在 poll 线程中也用于回收客户端槽位.
        void reset(int next = -1) noexcept {
            if (value >= 0)
                ::close(value);
            value = next;
        }
    };

    // 快照发布后不可变, 慢客户端共享正文, 不在 HTTP 热路径格式化指标.
    struct Snapshot {
        std::string response;       // 完整 HTTP 200 报文, 固定指标规模, 小于 8 KiB.
        Steady::time_point sampled; // 控制线程实际采样时间, 不以抓取时间伪装新观测.
    };

    // 每条连接只处理一个请求, 16 个槽位, 不支持 keep-alive、请求正文或管线化.
    struct Client {
        File socket;                          // 独占客户端 socket, 空槽为 -1.
        std::array<char, 4096> header{};      // 请求头硬限制 4 KiB, 不动态扩容.
        std::size_t received{};               // 已读取头部字节数, 0..4096.
        std::size_t sent{};                   // 已发响应字节数, 默认零.
        Steady::time_point deadline{};        // 接纳后固定两秒, 读写共享, 不滑动续期.
        std::shared_ptr<const Snapshot> view; // 发送期间固定快照, 完成/超时立即释放.
    };

    // 先完成监听和 eventfd, 最后启动线程; 中间任意抛出均由成员析构释放描述符.
    explicit Server(const Endpoint& endpoint) : address(endpoint) {

        const auto family = address.ipv6 ? AF_INET6 : AF_INET; // 只接受配置层的数值 IP.
        listener.reset(::socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        wake.reset(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
        if (listener.value < 0 || wake.value < 0)
            throw std::runtime_error("Cannot open metrics descriptors");
        const int enabled = 1; // 允许旧 TIME_WAIT 重启, 不允许多个活跃进程共享监听.
        if (::setsockopt(listener.value, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0)
            throw std::runtime_error("Cannot configure metrics listener");
        sockaddr_storage storage{}; // 零初始化数值监听地址, 不执行 DNS.
        socklen_t length{};         // 本次地址族所需的 sockaddr 字节数.
        if (address.ipv6) {
            auto* value = reinterpret_cast<sockaddr_in6*>(&storage); // 只在正确的地址族内解释存储.
            value->sin6_family = AF_INET6;
            value->sin6_port = htons(address.port);
            length = sizeof(sockaddr_in6);
            if (::inet_pton(AF_INET6, address.host.c_str(), &value->sin6_addr) != 1 || ::setsockopt(listener.value, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled)) != 0)
                throw std::runtime_error("Invalid metrics IPv6 listener");
        } else {
            auto* value = reinterpret_cast<sockaddr_in*>(&storage); // IPv4 不接受映射地址别名.
            value->sin_family = AF_INET;
            value->sin_port = htons(address.port);
            length = sizeof(sockaddr_in);
            if (::inet_pton(AF_INET, address.host.c_str(), &value->sin_addr) != 1)
                throw std::runtime_error("Invalid metrics IPv4 listener");
        }
        if (::bind(listener.value, reinterpret_cast<const sockaddr*>(&storage), length) != 0 || ::listen(listener.value, 16) != 0 || ::getsockname(listener.value, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
            throw std::runtime_error("Cannot bind metrics listener");
        address.port = ntohs(address.ipv6 ? reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port : reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);

        // 所有对象已经构造完成, 后台线程不借用外部 Runtime 或业务锁.
        worker = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    // 唤醒没有网络事件的 poll, 显式 join 保证客户端和快照仍然存活.
    ~Server() {
        worker.request_stop();
        const std::uint64_t value = 1; // eventfd 仅用于销毁唤醒, 不用作累计业务计数.
        static_cast<void>(::write(wake.value, &value, sizeof(value)));
        worker.join();
    }

    // 同一槽位全部状态一起释放, 不让上一请求的报文或引用泄漏到新连接.
    static void close(Client& client) noexcept {
        client.socket.reset();
        client.view.reset();
        client.received = client.sent = 0;
    }

    // 不完整请求等待下一轮, 非精确 GET /metrics 关闭. 此专用端口不提供通用 HTTP 路由.
    void read(Client& client, Steady::time_point now) noexcept {

        const auto count = ::recv(client.socket.value, client.header.data() + client.received, client.header.size() - client.received, 0); // 单轮最多一次有界读取.
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        if (count <= 0) {
            close(client);
            return;
        }
        client.received += static_cast<std::size_t>(count);
        const std::string_view request(client.header.data(), client.received); // 只借用当前槽的固定头部存储.
        const auto end = request.find("\r\n\r\n");                             // 不接受 LF 简化语法, 避免歧义和请求走私.
        if (end == std::string_view::npos) {
            if (client.received == client.header.size())
                close(client);
            return;
        }
        if (end + 4 != request.size() || (!request.starts_with("GET /metrics HTTP/1.1\r\n") && !request.starts_with("GET /metrics HTTP/1.0\r\n"))) {
            close(client);
            return;
        }

        client.view = current.load(std::memory_order_acquire);
        // 控制循环若停止推进, 不能继续把旧数据作为成功实时观测交给管理面.
        if (!client.view || now - client.view->sampled > std::chrono::seconds(10))
            close(client);
    }

    // 非阻塞发送只推进已确认的字节数, 不重建字符串, 对端关闭不产生 SIGPIPE.
    static void write(Client& client) noexcept {
        const auto& response = client.view->response; // view 由此槽持续持有到完成或超时.
        const auto count = ::send(client.socket.value, response.data() + client.sent, response.size() - client.sent, MSG_NOSIGNAL);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        if (count <= 0) {
            close(client);
            return;
        }
        client.sent += static_cast<std::size_t>(count);
        if (client.sent == response.size())
            close(client);
    }

    // 固定 18 个 poll 描述符; 每轮至多接纳 16 次并为各活跃槽执行一次 I/O, 不因洪水饿死退出.
    void run(std::stop_token stop) noexcept {
        while (!stop.stop_requested()) {
            std::array<pollfd, 18> events{}; // 前两个为监听/停止, 后 16 个一一对应客户端.
            events[0] = {listener.value, POLLIN, 0};
            events[1] = {wake.value, POLLIN, 0};
            for (std::size_t index = 0; index < clients.size(); ++index)
                events[index + 2] = {clients[index].socket.value, static_cast<short>(clients[index].view ? POLLOUT : POLLIN), 0};
            const auto count = ::poll(events.data(), events.size(), 100); // 100 ms 只控制超时清理粒度; 停止通过 eventfd 立即唤醒.
            if (stop.stop_requested() || (count < 0 && errno != EINTR))
                break;
            const auto now = Steady::now(); // 一轮共享的截止观察, 不使用可回拨的墙钟.
            for (std::size_t index = 0; index < clients.size(); ++index) {
                auto& client = clients[index];                // 单线程独占, 无跨线程客户端锁.
                const auto flags = events[index + 2].revents; // 仅处理 poll 时已存在的连接, 新接纳留到下一轮.
                if (client.socket.value < 0)
                    continue;
                if (now >= client.deadline || (flags & (POLLERR | POLLNVAL)) != 0) {
                    close(client);
                    continue;
                }
                if (client.view && (flags & POLLOUT) != 0)
                    write(client);
                else if (!client.view && (flags & POLLIN) != 0)
                    read(client, now);
                else if ((flags & POLLHUP) != 0)
                    close(client);
            }
            if (count <= 0 || (events[0].revents & POLLIN) == 0)
                continue;
            for (std::size_t accepted = 0; accepted < clients.size(); ++accepted) {
                const auto socket = ::accept4(listener.value, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC); // 没有等待线程, 满额立即关闭.
                if (socket < 0)
                    break;
                const auto slot = std::ranges::find_if(clients, [](const Client& value) { return value.socket.value < 0; });
                if (slot == clients.end()) {
                    ::close(socket);
                    continue;
                }
                slot->socket.reset(socket);
                slot->deadline = now + std::chrono::seconds(2);
            }
        }
        for (auto& client : clients)
            close(client); // 关闭不等待慢速客户端发送或读取.
    }

    Endpoint address;                                     // 构造完成后不可变的实际绑定端点.
    File listener;                                        // 私有 HTTP 监听器, 默认配置不创建.
    File wake;                                            // 停止事件描述符, 比工作线程活得更久.
    std::array<Client, 16> clients;                       // 固定连接与输入内存上限.
    std::atomic<std::shared_ptr<const Snapshot>> current; // 控制线程发布, HTTP 线程获取不可变快照.
    std::jthread worker;                                  // 最后构造, 最先停止和 join.
};

Metrics::Metrics(const Endpoint& endpoint) : server_(std::make_unique<Server>(endpoint)) {}

Metrics::~Metrics() = default;

Endpoint Metrics::endpoint() const {
    return server_->address;
}

bool Metrics::publish(const Sample& sample) noexcept {
    try {
        // 进程 ID 只允许安全头部字节. 未准入仍可显示 ready=0, 管理面不会把它关联为可信实例.
        if (sample.instance.size() > 256 || std::ranges::any_of(sample.instance, [](unsigned char byte) { return byte < 33 || byte > 126; }))
            return false;
        std::string body; // 固定八个无标签 gauge, 不包含业务键/凭据或不受限基数.
        body.reserve(1024);
        const auto append = [&](std::string_view name, auto value) { body += "# TYPE "; body += name; body += " gauge\n"; body += name; body += ' '; body += std::to_string(value); body += '\n'; }; // 一次采样格式化一次.
        append("astra_ready", sample.ready);
        append("astra_almanac_ready", sample.almanac);
        append("astra_clock_ready", sample.clock);
        append("astra_clock_synchronized", sample.synchronized);
        append("astra_clock_uncertainty_nanoseconds", sample.uncertainty);
        append("astra_members", sample.members);
        append("astra_sessions", sample.sessions);
        append("astra_recovery_bytes", sample.recovery);
        auto snapshot = std::make_shared<Server::Snapshot>(); // 完整分配成功才原子替换旧快照.
        snapshot->sampled = Steady::now();
        snapshot->response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4; charset=utf-8\r\nConnection: close\r\nCache-Control: no-store\r\nX-Astra-Instance: " + sample.instance + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        server_->current.store(std::move(snapshot), std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace astra
