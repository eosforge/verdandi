// 功能: 协调监听, 准入, 角色策略和会话资源, 通过单一控制循环推进连接并排空关闭回调.
#include <astra/runtime.hpp>

#include "admission.hpp"
#include "grpc_session.hpp"
#include "process.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace astra {
namespace {
// 没有准入资格或超额的 RPC 立即 Finish. 只有 OnDone 一个释放点, 不进入业务状态集合.
class RejectedSession final : public grpc::ServerBidiReactor<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket> {
public:
    // code 为固定拒绝状态, 构造时立即 Finish; 不进入 Runtime 会话集合, 由 OnDone 唯一释放.
    explicit RejectedSession(grpc::StatusCode code) {
        Finish(grpc::Status(code, "Star is not accepting this session"));
    }
    // gRPC 确认拒绝 RPC 最终完成后删除自身, 此路径没有共享会话所有者.
    void OnDone() override {
        delete this;
    }
};

class Runtime final : public proto::astra::v1::StarTransport::CallbackService {
public:
    // 接管已校验配置, 身份和角色策略, 进程 ID 在首次准入成功时安装; 不在构造时开始监听或准入.
    Runtime(Config config, std::shared_ptr<Identity> identity, std::unique_ptr<Policy> policy)
        : config_(std::move(config)), identity_(std::move(identity)), policy_(std::move(policy)), logger_(config_.role) {}

    // handler 只做接纳和所有权登记. 慢解析, 验签, 策略和日志全部留给控制循环.
    grpc::ServerBidiReactor<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket>* OpenSession(grpc::CallbackServerContext* context) override {
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

    // 借用入口的信号所有者, 建立 TLS 监听后运行控制循环并排空关闭; 正常停止返回 0, 准入致命失败返回 1.
    // 监听或内部异常向入口传播, 已进入运行阶段的异常先执行 shutdown, 日志不输出异常正文.
    int run(const Signals& signals) {
        // 先取得实际监听端口再登记准入, 避免向 Supervisor 发布尚未绑定或端口仍为 0 的端点.
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
            admission_ = std::make_unique<Admission>(config_, identity_, notifier());
            logger_.write("started", "{\"advertise\":\"" + config_.advertise.text() + "\"}");
            while (!signals.requested() && !fatal_) {
                const auto observed = wake_->observe();
                step();
                wake_->wait(observed);
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

    // 返回只持有独立 Wakeup 的回调, 不捕获 this, 允许完成发布后 Runtime 立即释放会话.
    std::function<void()> notifier() const {
        return [wake = wake_] { wake->notify(); };
    }

    // 在入站登记锁内把 handler 创建的会话移入控制循环集合, 不改变在途计数或推进协议.
    void collect() {
        std::lock_guard lock(incoming_mutex_);
        sessions_.insert(sessions_.end(), std::make_move_iterator(incoming_.begin()), std::make_move_iterator(incoming_.end()));
        incoming_.clear();
    }

    // 仅回收已发布 done 的会话, 根据是否安装身份通知 closed/failed, now 用于角色退避.
    // 入站容量在此释放, 避免取消尚未最终完成的 RPC 过早让出槽位.
    void reap(Clock::time_point now) {
        std::erase_if(sessions_, [&](const std::shared_ptr<RpcSession>& session) {
            if (!session->done()) {
                return false;
            }
            const auto error = session->error();
            const auto reason = error.value_or(ErrorCode::transport);
            if (session->installed()) {
                policy_->closed(session->generation(), error, now);
                logger_.failure("disconnected", reason);
            } else if (session->expected()) {
                policy_->failed(*session->expected(), reason, now);
                logger_.failure("connect_failed", reason);
            }
            if (session->direction() == Direction::inbound) {
                std::lock_guard lock(incoming_mutex_);
                --inbound_;
            }
            return true;
        });
    }

    // 用同一单调时间推进当前会话, 执行合法替换返回的旧代次取消, 最后回收已完成对象.
    void pump_sessions(Clock::time_point now) {
        for (const auto& session : sessions_) {
            const bool installed = session->installed();
            const auto cancelled = session->pump(*policy_, *identity_, now);
            if (!installed && session->installed()) {
                logger_.write("connected");
            }
            for (const auto generation : cancelled) {
                const auto old = std::ranges::find(sessions_, generation, [](const auto& item) { return item->generation(); });
                if (old != sessions_.end()) {
                    (*old)->cancel();
                }
            }
        }
        reap(now);
    }

    // 准入独立推进首次登记与候选刷新, 失败保留原有重试期限和本地身份.
    void advance_admission(Clock::time_point now) {
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
                    id_ = joined.local.id;
                    hello_ = joined.hello;
                    accepting_ = true;
                    logger_.write("initialized", "{\"id\":" + json_string(id_) + "}");
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
                next_registration_ = now + retry_delay(registration_failures_, config_, std::hash<std::string>{}(id_.empty() ? config_.advertise.text() : id_));
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
    }

    // 拨号预算跨轮次生效, 每轮最多建立一个流, 避免集中启动时出现连接突发.
    void dial(Clock::time_point now) {
        if (hello_ && now >= next_dial_) {
            const auto pending = std::count_if(sessions_.begin(), sessions_.end(),
                                               [](const auto& session) { return session->direction() == Direction::outbound && !session->installed(); });
            if (static_cast<std::size_t>(pending) < config_.max_dials) {
                if (auto target = policy_->due(now)) {
                    sessions_.push_back(connect_session(config_, *identity_, next_generation(), hello_, std::move(*target), notifier()));
                    next_dial_ = now + Milliseconds(250);
                }
            }
        }
    }

    // 控制循环是协议和角色状态的唯一推进者. 顺序为回收完成, 准入, 拨号, 诊断.
    void step() {
        const auto now = Clock::now();
        collect();
        pump_sessions(now);
        advance_admission(now);
        if (fatal_) {
            return;
        }
        dial(now);
        if (config_.status_interval.count() != 0 && now >= next_status_) {
            logger_.status(policy_->status(), id_);
            next_status_ = now + config_.status_interval;
        }
    }

    // 停止接纳并取消所有已拥有 RPC, 在统一截止内继续推进完成, 然后关闭服务器.
    // 截止耗尽仍无法排空会话时记录固定事件并 _Exit(1), 不强行析构仍被 gRPC 借用的对象.
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
            const auto observed = wake_->observe();
            pump_sessions(Clock::now());
            if (admission_) {
                static_cast<void>(admission_->poll());
            }
            if (Clock::now() >= deadline) {
                logger_.write("shutdown_timeout");
                std::_Exit(1);
            }
            wake_->wait(observed);
        }
        server_->Shutdown(std::chrono::system_clock::now() + std::max(deadline - Clock::now(), Clock::duration::zero()));
        server_->Wait();
        server_.reset();
        admission_.reset();
        logger_.write("stopped");
    }

    Config config_;
    std::shared_ptr<Identity> identity_;
    Id id_;
    std::unique_ptr<Policy> policy_;
    Logger logger_;
    std::shared_ptr<Wakeup> wake_ = std::make_shared<Wakeup>();
    // handler 只写 incoming_/inbound_, collect 之后的 sessions_ 只归控制循环. hello_ 安装后不再修改.
    std::mutex incoming_mutex_;
    bool accepting_{};
    std::size_t inbound_{};
    std::vector<std::shared_ptr<RpcSession>> incoming_;
    std::vector<std::shared_ptr<RpcSession>> sessions_;
    std::shared_ptr<const proto::astra::v1::Hello> hello_;
    std::atomic_uint64_t next_generation_{1};
    std::unique_ptr<Admission> admission_;
    // shutdown 完成所有 OnDone 后才释放 server 和 Admission, 不依赖成员析构顺序来取消 RPC.
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
        // 帮助与版本查询不加载身份文件也不启动网络, 参数输入的 string_view 只借用 argv 本次调用.
        if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            std::cout << option_help(role);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << (role == Role::star ? "star" : "planet") << " 0.1.0 (C++26, gRPC v1)\n";
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
        // 配置通过后先验证身份材料; 进程 ID 等待 Supervisor 签发, 重连复用准入结果.
        auto identity = Identity::load(config->identity, config->advertise);
        if (!identity) {
            std::cerr << identity.error().message << '\n';
            return 1;
        }
        Signals signals;
        Runtime runtime(*config, *identity, factory(*config));
        return runtime.run(signals);
    } catch (const std::exception&) {
        std::cerr << "Internal service failure\n";
        return 1;
    }
}
} // namespace astra
