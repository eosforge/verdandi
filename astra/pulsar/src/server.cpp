// 功能: 验证 Pulsar 启动配置, 隔离对时和登记的线程/内存预算, 持有服务直到退出.
#include "server.hpp"
#include <algorithm>
#include <bitset>
#include <charconv>
#include <grpc/support/time.h>
#include <grpcpp/server_builder.h>
#include <stdexcept>

namespace astra {
std::string_view PulsarConfig::help() {
    return "Usage: pulsar --listen=IP:PORT --pulse-listen=IP:PORT --galaxy=ID [options]\n"
           "  --identity=DIR       TLS, admission.key/pub and accounts.json; default=identity\n"
           "  --state=FILE         Exclusive durable journal; default=state/pulsar.journal\n"
           "  --max-members=N      Capacity per role, 1..4096; default=64\n"
           "  --max-starts=N       Retained startup records, 1..1000000; default=65536\n"
           "  --help / --version   Do not load credentials or start services\n"
           "Listeners require concrete IPs authorized by the TLS certificate. Stop with SIGINT or SIGTERM.\n";
}

Result<PulsarConfig> PulsarConfig::parse(std::span<const std::string_view> arguments) {
    constexpr std::array names{"listen", "pulse-listen", "galaxy", "identity", "state", "max-members", "max-starts"};
    std::bitset<names.size()> seen;
    PulsarConfig result;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        if (!argument.starts_with("--")) {
            return Error::configuration("Expected a named Pulsar option");
        }
        const auto equal = argument.find('=');
        const auto name = argument.substr(2, equal == std::string_view::npos ? equal : equal - 2);
        const auto found = std::ranges::find(names, name);
        if (found == names.end()) {
            return Error::configuration("Unknown Pulsar option");
        }
        const auto index = static_cast<std::size_t>(found - names.begin());
        if (seen.test(index)) {
            return Error::configuration("Duplicate Pulsar option");
        }
        seen.set(index);
        const auto value = equal != std::string_view::npos ? argument.substr(equal + 1) : i + 1 < arguments.size() ? arguments[++i] : std::string_view{};
        if (value.empty() || value.size() > 4096 || value.starts_with("--")) {
            return Error::configuration("Missing or oversized Pulsar value");
        }
        if (index <= 1) {
            const auto endpoint = Endpoint::parse(value, true);
            if (!endpoint || endpoint->wildcard) {
                return Error::configuration("Pulsar requires a concrete numeric listener");
            }
            (index == 0 ? result.listen : result.pulse_listen) = *endpoint;
        } else if (index == 2) {
            result.galaxy = value;
        } else if (index == 3) {
            result.identity = value;
        } else if (index == 4) {
            result.state = value;
        } else {
            std::size_t count{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
            if (error != std::errc{} || end != value.data() + value.size() || count == 0 || count > (index == 5 ? 4096U : 1'000'000U)) {
                return Error::configuration("Invalid Pulsar capacity");
            }
            (index == 5 ? result.maximum : result.starts) = count;
        }
    }
    if (!seen.test(0) || !seen.test(1) || !seen.test(2) || !Member::valid_name(result.galaxy) ||
        (result.listen.port != 0 && result.listen == result.pulse_listen)) {
        return Error::configuration("Pulsar requires a Galaxy and two distinct listeners");
    }
    return result;
}

PulsarServer::PulsarServer(PulsarConfig config, PhysicalClock::Provider provider) : config_(std::move(config)) {
    auto authority = PulsarAuthority::load(config_.identity, config_.listen, config_.pulse_listen);
    if (!authority) {
        throw std::runtime_error("Cannot load Pulsar authority");
    }
    authority_ = std::move(*authority);
    ledger_ = std::make_unique<MembershipLedger>(config_.state, config_.galaxy, authority_->key_id(), config_.maximum, config_.starts);
    clock_ = std::make_unique<PhysicalClock>(std::move(provider));
    pulse_ = std::make_unique<Pulse>(*authority_, *ledger_, *clock_);
}

PulsarServer::~PulsarServer() {
    stop();
}

void PulsarServer::start() {
    if (pulse_server_ || admission_server_) {
        throw std::runtime_error("Pulsar already started");
    }
    // 两个 Server 使用不同 ResourceQuota 和监听连接. 相同证书不意味着共享 RPC 执行配额.
    const auto build = [&](grpc::Service& service, Endpoint& endpoint, bool pulse) {
        grpc::ServerBuilder builder;
        // 一个端点只代表一个权威进程, 禁止内核把同端口连接分流到不同账本/时间参考.
        builder.AddChannelArgument("grpc.so_reuseport", 0);
        builder.RegisterService(&service);
        builder.SetMaxReceiveMessageSize(pulse ? 128 : 4096);
        builder.SetMaxSendMessageSize(pulse ? 128 : 2 * 1024 * 1024);
        builder.AddChannelArgument("grpc.server_handshake_timeout_ms", 3000);
        builder.AddChannelArgument("grpc.max_connection_idle_ms", 30000);
        builder.AddChannelArgument("grpc.max_concurrent_streams", pulse ? 1 : 4);
        builder.AddChannelArgument("grpc.max_metadata_size", 4096);
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
        int port = 0;
        builder.AddListeningPort(endpoint.text(), authority_->identity().server_credentials(), &port);
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

void PulsarServer::stop() {
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

std::string PulsarServer::admission_endpoint() const {
    return config_.listen.text();
}
std::string PulsarServer::pulse_endpoint() const {
    return config_.pulse_listen.text();
}
std::optional<EpochReading> PulsarServer::time() const {
    return clock_->now();
}
} // namespace astra
