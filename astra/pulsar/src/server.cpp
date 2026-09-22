#include "server.hpp"
#include <algorithm>
#include <bitset>
#include <charconv>
#include <grpc/support/time.h>
#include <grpcpp/server_builder.h>
#include <stdexcept>

namespace astra {
std::string_view Server::Config::help() {

    return "Usage: pulsar --listen=IP:PORT --pulse-listen=IP:PORT --galaxy=ID [options]\n" "  --identity=DIR       TLS, admission.key/pub and accounts.json; default=identity\n" "  --state=FILE         Exclusive SQLite database; default=state/pulsar.db\n" "  --init=true|false    Initialize a new database; default=false\n" "  --max-members=N      Capacity per role, 1..4096; default=64\n" "  --max-starts=N       Retained startup records, 1..1000000; default=65536\n" "  --help / --version   Do not load credentials or start services\n" "Listeners require concrete IPs authorized by the TLS certificate. Stop with SIGINT or SIGTERM.\n";
}

Result<Server::Config> Server::Config::parse(std::span<const std::string_view> arguments) {

    // names 定义此解析器支持的固定选项顺序, seen 使用相同索引识别重复.
    constexpr std::array names{"listen", "pulse-listen", "galaxy", "identity", "state", "max-members", "max-starts", "init"};
    // seen 初始全零, 记录实际出现过的选项, 默认值不能代替必填项.
    std::bitset<names.size()> seen;
    // result 从配置默认值开始, 完成所有跨字段检查后才返回.
    Server::Config result;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        // argument 借用当前 argv 项, i 为参数索引, 分隔式写法会额外消费下一项.
        const auto argument = arguments[i];
        if (!argument.starts_with("--")) {
            return Status::configuration("Expected a named Pulsar option");
        }

        // equal 为等号位置, npos 表示使用下一项作为选项值.
        const auto equal = argument.find('=');
        // name 借用去除双横杠和可选值后的选项名, 不执行大小写折叠.
        const auto name = argument.substr(2, equal == std::string_view::npos ? equal : equal - 2);
        // found 定位固定名称表, 未命中时拒绝而不忽略未知配置.
        const auto found = std::ranges::find(names, name);
        if (found == names.end()) {
            return Status::configuration("Unknown Pulsar option");
        }

        // index 对应 names 与 seen 的相同槽位, 由已成功的查找得到.
        const auto index = static_cast<std::size_t>(found - names.begin());
        if (seen.test(index)) {
            return Status::configuration("Duplicate Pulsar option");
        }
        seen.set(index);
        // value 借用等号后或下一项参数, 空值,伪选项和超过 4096 字节均拒绝.
        const auto value = equal != std::string_view::npos ? argument.substr(equal + 1) : i + 1 < arguments.size() ? arguments[++i]
                                                                                                                   : std::string_view{};
        if (value.empty() || value.size() > 4096 || value.starts_with("--")) {
            return Status::configuration("Missing or oversized Pulsar value");
        }
        if (index <= 1) {
            // endpoint 允许端口零供系统分配, 但对时和登记监听均要求明确 IP.
            const auto endpoint = Endpoint::parse(value, true);
            if (!endpoint || endpoint->wildcard) {
                return Status::configuration("Pulsar requires a concrete numeric listener");
            }
            (index == 0 ? result.listen : result.pulse_listen) = *endpoint;
        } else if (index == 2) {
            result.galaxy = value;
        } else if (index == 3) {
            result.identity = value;
        } else if (index == 4) {
            result.state = value;
        } else if (index == 7) {
            if (value != "true" && value != "false") {
                return Status::configuration("Pulsar init must be true or false");
            }
            result.initialize = value == "true";
        } else {
            // count 暂存容量输入, 不接受零值,尾随字符或超过各自上限的值.
            std::size_t count{};
            // end 表示完整消费位置, error 表示数字转换状态, 两者联合排除部分解析.
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
            if (error != std::errc{} || end != value.data() + value.size() || count == 0 || count > (index == 5 ? 4096U : 1'000'000U)) {
                return Status::configuration("Invalid Pulsar capacity");
            }
            (index == 5 ? result.maximum : result.starts) = count;
        }
    }
    if (!seen.test(0) || !seen.test(1) || !seen.test(2) || !Member::valid_name(result.galaxy) || (result.listen.port != 0 && result.listen == result.pulse_listen)) {
        return Status::configuration("Pulsar requires a Galaxy and two distinct listeners");
    }
    return result;
}

Server::Server(Server::Config config, Source::Provider provider) : config_(std::move(config)) {

    // authority 完整加载两端 TLS 授权和签发材料, 成功后才创建账本与线程.
    auto authority = Authority::load(config_.identity, config_.listen, config_.pulse_listen);
    if (!authority) {
        throw std::runtime_error("Cannot load Pulsar authority");
    }
    authority_ = std::move(*authority);
    ledger_ = std::make_unique<Ledger>(config_.state, config_.galaxy, authority_->key_id(), config_.maximum, config_.starts, config_.initialize);
    clock_ = std::make_unique<Source>(std::move(provider));
    pulse_ = std::make_unique<Pulse>(*authority_, *ledger_, *clock_);
}

Server::~Server() {
    stop();
}

void Server::start() {

    if (pulse_server_ || admission_server_) {
        throw std::runtime_error("Pulsar already started");
    }

    // 两个 Server 使用不同 ResourceQuota 和监听连接. 相同证书不意味着共享 RPC 执行配额.
    const auto build = [&](grpc::Service& service, Endpoint& endpoint, bool pulse) {
        // builder 仅构造当前 service 的独立监听器, endpoint 在成功后更新实际端口.
        grpc::ServerBuilder builder;
        // 一个端点只代表一个权威进程, 禁止内核把同端口连接分流到不同账本/时间参考.
        builder.AddChannelArgument("grpc.so_reuseport", 0);
        builder.RegisterService(&service);
        builder.SetMaxReceiveMessageSize(pulse ? 128 : 4096);
        // 四种角色每种最多 4096 条, 完整目录预算独立于只返回 Star/Polaris 的节点读取预算.
        builder.SetMaxSendMessageSize(pulse ? 128 : 8 * 1024 * 1024);
        builder.AddChannelArgument("grpc.server_handshake_timeout_ms", 3000);
        builder.AddChannelArgument("grpc.max_connection_idle_ms", 30000);
        builder.AddChannelArgument("grpc.max_concurrent_streams", pulse ? 1 : 4);
        builder.AddChannelArgument("grpc.max_metadata_size", 4096);
        // quota 为本监听器独立的资源池, 避免同步登记工作挤占对时服务配额.
        grpc::ResourceQuota quota;
        quota.Resize(pulse ? 16U * 1024 * 1024 : 32U * 1024 * 1024);
        if (!pulse) {
            // 只有登记运行同步 KDF/文件提交. Pulse 回调不创建按流阻塞的同步工作线程.
            builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::NUM_CQS, 2);
            builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MIN_POLLERS, 1);
            builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MAX_POLLERS, 4);
            quota.SetMaxThreads(16);
        }
        builder.SetResourceQuota(quota);
        // port 接收系统实际绑定端口, 零表示尚未确认监听成功.
        int port = 0;
        builder.AddListeningPort(endpoint.text(), authority_->identity().server_credentials(), &port);
        // server 接管监听资源, 只有非空且实际端口有效才交给 Server.
        auto server = builder.BuildAndStart();
        if (!server || port <= 0) {
            throw std::runtime_error("Cannot bind Pulsar TLS listener");
        }
        endpoint.port = static_cast<std::uint16_t>(port);
        return server;
    };
    try {
        pulse_server_ = build(*pulse_, config_.pulse_listen, true);
        issuer_ = std::make_unique<Issuer>(config_.galaxy, pulse_endpoint(), *authority_, *ledger_);
        admission_server_ = build(*issuer_, config_.listen, false);
    } catch (...) {
        stop();
        throw;
    }
}

void Server::stop() {

    // deadline 是两个监听器共用的单调五秒关闭期限, 第二个不重新获得预算.
    const auto deadline = gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(5, GPR_TIMESPAN));
    if (pulse_server_) {
        pulse_server_->Shutdown(deadline);
    }
    if (admission_server_) {
        admission_server_->Shutdown(deadline);
    }
    if (pulse_server_) {
        pulse_server_->Wait();
        pulse_server_.reset();
    }
    if (admission_server_) {
        admission_server_->Wait();
        admission_server_.reset();
    }
    issuer_.reset();
}

std::string Server::admission_endpoint() const {
    return config_.listen.text();
}

std::string Server::pulse_endpoint() const {
    return config_.pulse_listen.text();
}

std::optional<Clock::Reading> Server::time() const {
    return clock_->now();
}
} // namespace astra
