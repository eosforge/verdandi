// 许可证: MIT, 详见仓库根目录 LICENSE.
#include <verdandi/peer/runtime.hpp>

#include "admission.hpp"
#include "grpc_session.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace verdandi::peer {
namespace {
std::atomic_bool stop_requested{false};
static_assert(std::atomic_bool::is_always_lock_free);

// 信号可能送到任意 gRPC 线程. 使用编译期确认的无锁原子标志, 同时满足信号回调和跨线程可见性.
void stop_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

class Signals {
public:
    Signals() {
        stop_requested.store(false, std::memory_order_relaxed);
        struct sigaction action{};
        action.sa_handler = stop_signal;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, &interrupt_)) {
            throw std::runtime_error("Cannot install exit signal handlers");
        }
        if (sigaction(SIGTERM, &action, &terminate_)) {
            sigaction(SIGINT, &interrupt_, nullptr);
            throw std::runtime_error("Cannot install exit signal handlers");
        }
        action.sa_handler = SIG_IGN;
        if (sigaction(SIGPIPE, &action, &pipe_)) {
            sigaction(SIGINT, &interrupt_, nullptr);
            sigaction(SIGTERM, &terminate_, nullptr);
            throw std::runtime_error("Cannot install pipe signal handler");
        }
    }
    ~Signals() {
        sigaction(SIGINT, &interrupt_, nullptr);
        sigaction(SIGTERM, &terminate_, nullptr);
        sigaction(SIGPIPE, &pipe_, nullptr);
    }

private:
    struct sigaction interrupt_{}, terminate_{}, pipe_{};
};

// 回调持有独立唤醒对象的 shared_ptr, 最后一个 RPC 完成时不会访问已经析构的 Runtime.
struct Wakeup {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic_uint64_t sequence{0};
    void notify() {
        {
            // 与 wait 的检查/入睡共用同一把锁, 消除检查谓词后、真正入睡前的通知窗口.
            std::lock_guard lock(mutex);
            sequence.fetch_add(1, std::memory_order_relaxed);
        }
        condition.notify_one();
    }
};

// 控制循环直接写一条小于 PIPE_BUF 的 JSON 行. 管道背压时丢弃诊断, 不积累队列或阻塞协议回调.
class Logger {
public:
    explicit Logger(Role role) : component_(role == Role::star ? "peer" : "planet") {
        struct stat status{};
        if (fstat(STDOUT_FILENO, &status) == 0 && (S_ISFIFO(status.st_mode) || S_ISSOCK(status.st_mode))) {
            flags_ = fcntl(STDOUT_FILENO, F_GETFL);
            if (flags_ >= 0) {
                fcntl(STDOUT_FILENO, F_SETFL, flags_ | O_NONBLOCK);
            }
        }
    }
    ~Logger() {
        if (flags_ >= 0) {
            fcntl(STDOUT_FILENO, F_SETFL, flags_);
        }
    }
    void write(std::string_view event, std::string_view fields = "{}") const {
        const auto now = std::chrono::duration_cast<Milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto line = "{\"time_unix_ms\":" + std::to_string(now) + ",\"level\":\"INFO\",\"component\":\"" + component_ + "\",\"event\":\"" +
                          std::string(event) + "\",\"fields\":" + std::string(fields) + "}\n";
        if (line.size() <= 4096) {
            const auto written = ::write(STDOUT_FILENO, line.data(), line.size());
            static_cast<void>(written);
        }
    }
    void status(const NetworkStatus& s, const PeerId& id) const {
        const auto boolean = [](bool value) { return value ? "true" : "false"; };
        const auto upstream = s.active_member ? "{\"peer_id\":\"" + s.active_member->id.text() + "\",\"group\":\"" + s.active_member->group +
                                                    "\",\"address\":\"" + s.active_member->address.text() + "\"}"
                                              : "null";
        write("status", "{\"peer_id\":\"" + id.text() + "\",\"initialized\":" + std::string(boolean(s.initialized)) +
                            ",\"members\":" + std::to_string(s.members) + ",\"inbound\":" + std::to_string(s.inbound) +
                            ",\"outbound\":" + std::to_string(s.outbound) + ",\"planet_inbound\":" + std::to_string(s.planet_inbound) +
                            ",\"candidates\":" + std::to_string(s.candidates) + ",\"upstream\":" + upstream + "}");
    }
    void failure(std::string_view event, ErrorCode error) const {
        write(event, "{\"reason\":\"" + std::string(error_name(error)) + "\"}");
    }

private:
    std::string component_;
    int flags_ = -1;
};

// 没有准入资格或超额的 RPC 立即 Finish. 只有 OnDone 一个释放点, 不进入业务状态集合.
class RejectedSession final : public grpc::ServerBidiReactor<wire::SessionPacket, wire::SessionPacket> {
public:
    explicit RejectedSession(grpc::StatusCode code) {
        Finish(grpc::Status(code, "Peer is not accepting this session"));
    }
    void OnDone() override {
        delete this;
    }
};

class Runtime final : public wire::PeerTransport::CallbackService {
public:
    Runtime(Config config, std::shared_ptr<Identity> identity, PeerId id, std::unique_ptr<Policy> policy)
        : config_(std::move(config)), identity_(std::move(identity)), id_(id), policy_(std::move(policy)), logger_(config_.role) {}

    // handler 只做接纳和所有权登记. 慢解析、验签、策略和日志全部留给控制循环.
    grpc::ServerBidiReactor<wire::SessionPacket, wire::SessionPacket>* OpenSession(grpc::CallbackServerContext* context) override {
        try {
            std::lock_guard lock(incoming_mutex_);
            if (!accepting_ || config_.role != Role::star) {
                return new RejectedSession(grpc::StatusCode::UNAVAILABLE);
            }
            if (inbound_ >= config_.max_inbound) {
                return new RejectedSession(grpc::StatusCode::RESOURCE_EXHAUSTED);
            }
            auto generation = next_generation();
            auto session = std::make_shared<AcceptedSession>(context, config_, generation, hello_, notifier());
            auto* reactor = session.get();
            incoming_.push_back(std::move(session));
            ++inbound_;
            wake_->notify();
            return reactor;
        } catch (...) {
            return new RejectedSession(grpc::StatusCode::INTERNAL);
        }
    }

    int run() {
        grpc::ServerBuilder builder;
        builder.SetMaxReceiveMessageSize(4096);
        builder.SetMaxSendMessageSize(4096);
        builder.AddChannelArgument("grpc.server_handshake_timeout_ms", static_cast<int>(config_.handshake_timeout.count()));
        builder.AddChannelArgument("grpc.max_connection_idle_ms", 60000);
        grpc::ResourceQuota quota;
        quota.Resize(32 * 1024 * 1024);
        builder.SetResourceQuota(quota);
        builder.RegisterService(this);
        int port = 0;
        builder.AddListeningPort(config_.listen.text(), identity_->server_credentials(), &port);
        server_ = builder.BuildAndStart();
        if (!server_ || port <= 0) {
            throw std::runtime_error("Cannot bind TLS gRPC listener");
        }
        if (config_.advertise.port == 0) {
            config_.advertise.port = static_cast<std::uint16_t>(port);
        }
        try {
            admission_ = std::make_unique<Admission>(config_, identity_, id_, notifier());
            logger_.write("started", "{\"peer_id\":\"" + id_.text() + "\",\"advertise\":\"" + config_.advertise.text() + "\"}");
            while (!stop_requested.load(std::memory_order_relaxed) && !fatal_) {
                const auto observed = wake_->sequence.load(std::memory_order_relaxed);
                step();
                wait(observed);
            }
        } catch (...) {
            shutdown();
            throw;
        }
        shutdown();
        return fatal_ ? 1 : 0;
    }

private:
    // generation 分配跨 handler 和控制循环, 只需要 atomic 自身的唯一性, 不用于发布对象状态.
    SessionGeneration next_generation() {
        auto current = next_generation_.load(std::memory_order_relaxed);
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (next_generation_.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                return {current};
            }
        }
        throw std::runtime_error("Session generation exhausted");
    }

    std::function<void()> notifier() const {
        return [wake = wake_] { wake->notify(); };
    }

    void wait(std::uint64_t observed) {
        std::unique_lock lock(wake_->mutex);
        // 小上限同时负责退出标志轮询及 gRPC Channel 连接状态推进, 不创建每会话线程.
        // 基线在 step 之前取得, 防止丢掉本轮处理期间到达的完成通知.
        wake_->condition.wait_for(lock, Milliseconds(10), [&] { return wake_->sequence.load(std::memory_order_relaxed) != observed; });
    }

    void collect() {
        std::lock_guard lock(incoming_mutex_);
        sessions_.insert(sessions_.end(), std::make_move_iterator(incoming_.begin()), std::make_move_iterator(incoming_.end()));
        incoming_.clear();
    }

    void reap(Clock::time_point now) {
        std::erase_if(sessions_, [&](const std::shared_ptr<RpcSession>& session) {
            if (!session->done()) {
                return false;
            }
            if (session->installed()) {
                policy_->closed(session->generation(), session->error(), now);
                logger_.failure("disconnected", session->error().value_or(ErrorCode::transport));
            } else if (session->expected()) {
                policy_->failed(*session->expected(), session->error().value_or(ErrorCode::transport), now);
                logger_.failure("connect_failed", session->error().value_or(ErrorCode::transport));
            }
            if (session->direction() == Direction::inbound) {
                std::lock_guard lock(incoming_mutex_);
                --inbound_;
            }
            return true;
        });
    }

    void pump_sessions(Clock::time_point now) {
        for (const auto& session : sessions_) {
            const bool installed = session->installed();
            const auto cancelled = session->pump(*policy_, *identity_, now);
            if (!installed && session->installed()) {
                logger_.write("connected");
            }
            for (const auto generation : cancelled) {
                for (const auto& old : sessions_) {
                    if (old->generation() == generation) {
                        old->cancel();
                    }
                }
            }
        }
        reap(now);
    }

    void step() {
        const auto now = Clock::now();
        collect();
        pump_sessions(now);
        if (auto result = admission_->poll()) {
            if (*result) {
                auto& joined = **result;
                if (auto initialized = policy_->initialize(joined.local, joined.members); !initialized) {
                    fatal_ = true;
                    logger_.failure("registration_rejected", initialized.error().code);
                    return;
                }
                if (!hello_) {
                    std::lock_guard lock(incoming_mutex_);
                    hello_ = joined.hello;
                    accepting_ = true;
                    logger_.write("initialized");
                }
                registration_failures_ = 0;
            } else {
                const auto code = result->error().code;
                const bool temporary = code == ErrorCode::transport || code == ErrorCode::timeout || code == ErrorCode::capacity;
                if (!temporary && !hello_) {
                    fatal_ = true;
                    logger_.failure("registration_rejected", code);
                    return;
                }
                registration_failures_ = std::min(registration_failures_ + 1, 100U);
                next_registration_ = now + retry_delay(registration_failures_, config_, id_.bytes[0]);
                logger_.failure("registration_retry", code);
            }
        }
        if (!admission_->pending() && now >= next_registration_) {
            if (!hello_) {
                admission_->begin(0);
            } else if (policy_->needs_refresh(now)) {
                if (candidate_round_ == std::numeric_limits<std::uint32_t>::max()) {
                    fatal_ = true;
                    return;
                }
                admission_->begin(++candidate_round_);
            }
        }
        if (hello_ && now >= next_dial_) {
            const auto pending = std::count_if(sessions_.begin(), sessions_.end(),
                                               [](const auto& session) { return session->direction() == Direction::outbound && !session->installed(); });
            if (static_cast<std::size_t>(pending) < config_.max_dials) {
                for (auto target : policy_->due(now, 1)) {
                    sessions_.push_back(connect_session(config_, *identity_, next_generation(), hello_, std::move(target), notifier()));
                    next_dial_ = now + Milliseconds(250);
                }
            }
        }
        if (config_.status_interval.count() != 0 && now >= next_status_) {
            logger_.status(policy_->status(), id_);
            next_status_ = now + config_.status_interval;
        }
    }

    void shutdown() {
        const auto deadline = Clock::now() + config_.shutdown_timeout;
        {
            std::lock_guard lock(incoming_mutex_);
            accepting_ = false;
        }
        if (admission_) {
            admission_->cancel();
        }
        collect();
        for (const auto& session : sessions_) {
            session->cancel();
        }
        // 控制循环继续推进 Finish/RemoveHold 和 OnDone, 不能先 join 或销毁仍被回调借用的对象.
        while (!sessions_.empty() || (admission_ && admission_->pending())) {
            const auto observed = wake_->sequence.load(std::memory_order_relaxed);
            pump_sessions(Clock::now());
            if (admission_) {
                static_cast<void>(admission_->poll());
            }
            if (Clock::now() >= deadline) {
                logger_.write("shutdown_timeout");
                std::_Exit(1);
            }
            wait(observed);
        }
        server_->Shutdown(std::chrono::system_clock::now() + std::max(deadline - Clock::now(), Clock::duration::zero()));
        server_->Wait();
        server_.reset();
        admission_.reset();
        logger_.write("stopped");
    }

    Config config_;
    std::shared_ptr<Identity> identity_;
    PeerId id_;
    std::unique_ptr<Policy> policy_;
    Logger logger_;
    std::shared_ptr<Wakeup> wake_ = std::make_shared<Wakeup>();
    std::mutex incoming_mutex_;
    bool accepting_{};
    std::size_t inbound_{};
    std::vector<std::shared_ptr<RpcSession>> incoming_;
    std::vector<std::shared_ptr<RpcSession>> sessions_;
    std::shared_ptr<const wire::Hello> hello_;
    std::atomic_uint64_t next_generation_{1};
    std::unique_ptr<Admission> admission_;
    std::unique_ptr<grpc::Server> server_;
    bool fatal_{};
    std::uint32_t candidate_round_{};
    std::uint32_t registration_failures_{};
    Clock::time_point next_registration_{};
    Clock::time_point next_dial_{};
    Clock::time_point next_status_{};
};
} // namespace

int run_node(int argc, char** argv, Role role, PolicyFactory factory) {
    try {
        if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            std::cout << option_help(role);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << (role == Role::star ? "peer" : "planet") << " 0.1.0 (C++26, gRPC v4)\n";
            return 0;
        }
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }
        auto config = parse_options(arguments, role);
        if (!config) {
            std::cerr << config.error().message << '\n';
            return 2;
        }
        auto identity = Identity::load(config->identity, config->advertise);
        if (!identity) {
            std::cerr << identity.error().message << '\n';
            return 1;
        }
        auto id = Identity::new_id();
        if (!id) {
            std::cerr << id.error().message << '\n';
            return 1;
        }
        Signals signals;
        Runtime runtime(*config, *identity, *id, factory(*config));
        return runtime.run();
    } catch (const std::exception&) {
        std::cerr << "Internal service failure\n";
        return 1;
    }
}
} // namespace verdandi::peer
