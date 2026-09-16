// 功能: 协调监听, 准入, 角色策略和会话资源, 通过单一控制循环推进连接并排空关闭回调.
// 该文件实现了整个 Astra 节点的核心运行时环境，它包含监听外部会话的 gRPC 服务端处理、并管理所有主动拨出和被动接收的会话生命周期。
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
// RejectedSession 类: 没有准入资格或超额的 RPC 立即 Finish. 只有 OnDone 一个释放点, 不进入业务状态集合.
// 用于在服务端拒绝非法或超出容量限制的入站连接请求。它继承自 gRPC 的双向流式服务端反应器 (ServerBidiReactor)。
class RejectedSession final : public grpc::ServerBidiReactor<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket> {
public:
    // 构造函数: code 为固定拒绝状态, 构造时立即 Finish; 不进入 Runtime 会话集合, 由 OnDone 唯一释放.
    // 参数 code: 触发拒绝的 gRPC 状态码。
    explicit RejectedSession(grpc::StatusCode code) {
        // 立即调用 Finish 向对端发送关闭状态。
        Finish(grpc::Status(code, "Star is not accepting this session"));
    }
    // OnDone: gRPC 确认拒绝 RPC 最终完成后删除自身, 此路径没有共享会话所有者.
    // 资源在 RPC 流真正销毁时自动清理。
    void OnDone() override {
        delete this;
    }
};

// Runtime 类: 实现具体的服务端接口及事件循环控制逻辑。
class Runtime final : public proto::astra::v1::StarTransport::CallbackService {
public:
    // 构造函数: 接管已校验配置, 身份和角色策略, 进程 ID 在首次准入成功时安装; 不在构造时开始监听或准入.
    // 参数 config: 节点配置参数。
    // 参数 identity: 节点安全身份对象。
    // 参数 policy: 特定于角色的策略接口实现（如拓扑要求等）。
    Runtime(Config config, std::shared_ptr<Identity> identity, std::unique_ptr<Policy> policy)
        : config_(std::move(config)), identity_(std::move(identity)), policy_(std::move(policy)), logger_(config_.role) {}

    // OpenSession: gRPC 回调方法，处理新的双向流连接请求。handler 只做接纳和所有权登记. 慢解析, 验签, 策略和日志全部留给控制循环.
    // 参数 context: 服务端上下文环境。
    // 返回值: 新分配的流处理器反应器对象。
    grpc::ServerBidiReactor<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket>* OpenSession(grpc::CallbackServerContext* context) override {
        try {
            // 使用锁保护接收缓冲队列，因为该调用运行在 gRPC 的工作线程上。
            std::lock_guard lock(incoming_mutex_);
            // 如果不在接纳状态或当前节点根本不是 Star（如 Planet 角色不能接受连接），返回拒绝会话。
            if (!accepting_ || config_.role != Role::star) {
                return new RejectedSession(grpc::StatusCode::UNAVAILABLE);
            }
            // 超过最大并发入站连接限制，返回资源耗尽状态。
            if (inbound_ >= config_.max_inbound) {
                return new RejectedSession(grpc::StatusCode::RESOURCE_EXHAUSTED);
            }
            // 为新会话分配一个递增的世代号以追踪生命周期。
            auto generation = next_generation();
            // 创建并接纳合法的请求，初始化流控处理器
            auto session = std::make_shared<AcceptedSession>(context, config_, generation, hello_, notifier());
            auto* reactor = session.get();
            // 存入 incoming_ 暂存队列，等待控制循环消费。
            incoming_.push_back(std::move(session));
            ++inbound_;
            // 唤醒主控制循环处理新连接
            wake_->notify();
            return reactor;
        } catch (...) {
            // 发生未知异常时保护进程不崩溃，拒绝连接
            return new RejectedSession(grpc::StatusCode::INTERNAL);
        }
    }

    // run: 主入口，借用入口的信号所有者, 建立 TLS 监听后运行控制循环并排空关闭; 正常停止返回 0, 准入致命失败返回 1.
    // 监听或内部异常向入口传播, 已进入运行阶段的异常先执行 shutdown, 日志不输出异常正文.
    // 参数 signals: 捕获操作系统的终止等信号。
    // 返回值: 进程退出码。
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
        builder.RegisterService(this); // 注册自身服务
        int port = 0;
        // 绑定配置中的监听地址，并获取实际绑定的端口号
        builder.AddListeningPort(config_.listen.text(), identity_->server_credentials(), &port);
        server_ = builder.BuildAndStart();
        if (!server_ || port <= 0) {
            throw std::runtime_error("Cannot bind TLS gRPC listener");
        }
        // 若配置中的通告地址未指明端口，使用实际监听分配的端口
        if (config_.advertise.port == 0) {
            config_.advertise.port = static_cast<std::uint16_t>(port);
        }
        try {
            // 创建准入管理器
            admission_ = std::make_unique<Admission>(config_, identity_, notifier());
            logger_.write("started", "{\"advertise\":\"" + config_.advertise.text() + "\"}");
            // 进入主控制循环，直到收到停止信号或遇到致命错误
            while (!signals.requested() && !fatal_) {
                // 观察唤醒锁状态
                const auto observed = wake_->observe();
                // 运行一次单步逻辑
                step();
                // 如果没有新事件触发，则挂起等待唤醒
                wake_->wait(observed);
            }
        } catch (...) {
            // 异常退出时先平滑关闭现有资源
            shutdown();
            throw;
        }
        // 正常结束时的清理逻辑
        shutdown();
        return fatal_ ? 1 : 0;
    }

private:
    // next_generation: 生成自增的世代 ID。generation 分配跨 handler 和控制循环, 只需要 atomic 自身的唯一性, 不用于发布对象状态.
    // 返回值: SessionGeneration，包装好的非 0 唯一 ID。
    SessionGeneration next_generation() {
        auto current = next_generation_.load(std::memory_order_relaxed);
        // 不断尝试 CAS 操作来更新自增计数值
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (next_generation_.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                return {current};
            }
        }
        throw std::runtime_error("Session generation exhausted");
    }

    // notifier: 返回只持有独立 Wakeup 的回调, 不捕获 this, 允许完成发布后 Runtime 立即释放会话.
    std::function<void()> notifier() const {
        return [wake = wake_] { wake->notify(); };
    }

    // collect: 在入站登记锁内把 handler 创建的会话移入控制循环集合, 不改变在途计数或推进协议.
    void collect() {
        std::lock_guard lock(incoming_mutex_);
        // 使用 move_iterator 高效转移数据所有权
        sessions_.insert(sessions_.end(), std::make_move_iterator(incoming_.begin()), std::make_move_iterator(incoming_.end()));
        incoming_.clear();
    }

    // reap: 仅回收已发布 done 的会话, 根据是否安装身份通知 closed/failed, now 用于角色退避.
    // 入站容量在此释放, 避免取消尚未最终完成的 RPC 过早让出槽位.
    // 参数 now: 用于更新策略模块中失败时间记录的时间点。
    void reap(Clock::time_point now) {
        std::erase_if(sessions_, [&](const std::shared_ptr<RpcSession>& session) {
            // 未完成的会话保留在容器中
            if (!session->done()) {
                return false;
            }
            const auto error = session->error();
            const auto reason = error.value_or(Error::Code::transport);
            // 已经完成鉴权及身份绑定的会话断开
            if (session->installed()) {
                policy_->closed(session->generation(), error, now);
                logger_.failure("disconnected", reason);
            } else if (session->expected()) {
                // 尚未完成绑定，说明是建立连接过程中失败
                policy_->failed(*session->expected(), reason, now);
                logger_.failure("connect_failed", reason);
            }
            // 释放入站容量计数槽位
            if (session->direction() == Direction::inbound) {
                std::lock_guard lock(incoming_mutex_);
                --inbound_;
            }
            // 返回 true，使 erase_if 删除该项
            return true;
        });
    }

    // pump_sessions: 用同一单调时间推进当前会话, 执行合法替换返回的旧代次取消, 最后回收已完成对象.
    // 参数 now: 统一使用的推进逻辑单调时钟时间。
    void pump_sessions(Clock::time_point now) {
        for (const auto& session : sessions_) {
            const bool installed = session->installed();
            // pump: 处理会话内的数据读取、握手校验等逻辑，并返回需要被替代的旧会话的世代号数组
            const auto cancelled = session->pump(*policy_, *identity_, now);
            // 如果刚刚完成了身份安装（握手成功），则记录连接成功日志
            if (!installed && session->installed()) {
                logger_.write("connected");
            }
            // 对于新连接上位而需要踢掉的老连接，找到它们并主动发送取消指令
            for (const auto generation : cancelled) {
                const auto old = std::ranges::find(sessions_, generation, [](const auto& item) { return item->generation(); });
                if (old != sessions_.end()) {
                    (*old)->cancel();
                }
            }
        }
        // 集中清理本轮操作中宣告结束的会话
        reap(now);
    }

    // advance_admission: 准入独立推进首次登记与候选刷新, 失败保留原有重试期限和本地身份.
    // 参数 now: 当前时钟时间。
    void advance_admission(Clock::time_point now) {
        // 如果注册任务已有结果
        if (auto result = admission_->poll()) {
            if (*result) {
                // 成功注册
                auto& joined = **result;
                // 初始化策略对象以设置合法的节点成员信息
                if (auto initialized = policy_->initialize(joined.local, joined.members); !initialized) {
                    fatal_ = true;
                    logger_.failure("registration_rejected", initialized.error().code);
                    return;
                }
                // 首次连接成功，设置核心凭证对象
                if (!hello_) {
                    std::lock_guard lock(incoming_mutex_);
                    id_ = joined.local.id;
                    hello_ = joined.hello;
                    accepting_ = true;
                    logger_.write("initialized", "{\"id\":" + json_string(id_) + "}");
                }
                // 重置重试计数器
                registration_failures_ = 0;
            } else {
                // 注册失败
                const auto code = result->error().code;
                const bool temporary = code == Error::Code::transport || code == Error::Code::timeout || code == Error::Code::capacity;
                // 对于非临时错误且尚未初始化的，认定为严重致命错误直接退出
                if (!temporary && !hello_) {
                    fatal_ = true;
                    logger_.failure("registration_rejected", code);
                    return;
                }
                // 是临时错误或者是已经初始化过的节点（可以进行带有退避的重试）
                registration_failures_ = std::min(registration_failures_ + 1, 100U);
                next_registration_ = now + retry_delay(registration_failures_, config_, std::hash<std::string>{}(id_.empty() ? config_.advertise.text() : id_));
                logger_.failure("registration_retry", code);
            }
        }
        // 当不在请求中并且已经到了重试或定时检查的时间点时
        if (!admission_->pending() && now >= next_registration_) {
            if (!hello_) {
                // 还没拿到入场券，发起初始注册，0 作为第0轮候选
                admission_->begin(0);
            } else if (policy_->needs_refresh(now)) {
                // 如果策略判断需要向 Supervisor 刷新候选列表
                if (candidate_round_ == std::numeric_limits<std::uint32_t>::max()) {
                    fatal_ = true;
                    return;
                }
                // 启动带有自增轮次的刷新请求
                admission_->begin(++candidate_round_);
            }
        }
    }

    // dial: 拨号预算跨轮次生效, 每轮最多建立一个流, 避免集中启动时出现连接突发.
    // 参数 now: 当前时钟。
    void dial(Clock::time_point now) {
        if (hello_ && now >= next_dial_) {
            // 计算当前尚未建立起稳定通信（即正在连接/握手中的）外呼会话的数量
            const auto pending = std::count_if(sessions_.begin(), sessions_.end(),
                                               [](const auto& session) { return session->direction() == Direction::outbound && !session->installed(); });
            // 如果还未超出并行拨号的限制
            if (static_cast<std::size_t>(pending) < config_.max_dials) {
                // 询问策略：当前是否有需要进行外呼的对端目标
                if (auto target = policy_->due(now)) {
                    // 发起实际的出站连接创建并加入循环监控
                    sessions_.push_back(connect_session(config_, *identity_, next_generation(), hello_, std::move(*target), notifier()));
                    // 设置下一个允许外拨的时间点 (限制频率为每 250ms 最多建立一条，避免拥塞)
                    next_dial_ = now + Milliseconds(250);
                }
            }
        }
    }

    // step: 控制循环是协议和角色状态的唯一推进者. 顺序为回收完成, 准入, 拨号, 诊断.
    void step() {
        const auto now = Clock::now();
        collect();              // 取出来自 gRPC 的新连接
        pump_sessions(now);     // 处理每个会话的状态读写
        advance_admission(now); // 推动与 Supervisor 注册状态的更新
        if (fatal_) {
            return;
        }
        dial(now);              // 进行必要的外拨尝试
        // 按配置的时间间隔定期打印健康状态
        if (config_.status_interval.count() != 0 && now >= next_status_) {
            logger_.status(policy_->status(), id_);
            next_status_ = now + config_.status_interval;
        }
    }

    // shutdown: 停止接纳并取消所有已拥有 RPC, 在统一截止内继续推进完成, 然后关闭服务器.
    // 截止耗尽仍无法排空会话时记录固定事件并 _Exit(1), 不强行析构仍被 gRPC 借用的对象.
    void shutdown() {
        const auto deadline = Clock::now() + config_.shutdown_timeout;
        {
            std::lock_guard lock(incoming_mutex_);
            accepting_ = false; // 标记关闭，不再接纳新的 RPC 会话
        }
        if (admission_) {
            admission_->cancel(); // 取消正在进行的准入请求
        }
        collect();
        // 向所有持有的活动会话发出取消信号
        for (const auto& session : sessions_) {
            session->cancel();
        }
        // 控制循环继续推进 Finish/RemoveHold 和 OnDone, 不能先 join 或销毁仍被回调借用的对象.
        // 直到队列变空，才安全退出。
        while (!sessions_.empty() || (admission_ && admission_->pending())) {
            const auto observed = wake_->observe();
            pump_sessions(Clock::now());
            if (admission_) {
                static_cast<void>(admission_->poll());
            }
            if (Clock::now() >= deadline) {
                // 超时无法完全排空，为了防止破坏正在被 gRPC 核心引用的内存数据，直接系统级强制退出。
                logger_.write("shutdown_timeout");
                std::_Exit(1);
            }
            wake_->wait(observed);
        }
        // 平缓关闭 gRPC 服务端监听，在设定的截止时间内
        server_->Shutdown(std::chrono::system_clock::now() + std::max(deadline - Clock::now(), Clock::duration::zero()));
        server_->Wait();
        server_.reset();
        admission_.reset();
        logger_.write("stopped");
    }

    Config config_;                               // 全局配置实例。
    std::shared_ptr<Identity> identity_;          // 节点证书和凭证管理者。
    Id id_;                                       // 节点自身的 ID，首次向 Supervisor 注册后确定。
    std::unique_ptr<Policy> policy_;              // 动态角色策略协调器。
    Logger logger_;                               // 日志器实例，打印格式化日志输出。
    std::shared_ptr<Wakeup> wake_ = std::make_shared<Wakeup>(); // 同步机制，当有异步网络事件时触发。
    
    // handler 只写 incoming_/inbound_, collect 之后的 sessions_ 只归控制循环. hello_ 安装后不再修改.
    std::mutex incoming_mutex_;                   // 互斥锁，用于跨线程保护 incoming_ 和 accepting_。
    bool accepting_{};                            // 服务端是否还愿意接收新的入站连接，初始值为 false。
    std::size_t inbound_{};                       // 当前处理中以及被接纳的入站连接总数计数器。
    std::vector<std::shared_ptr<RpcSession>> incoming_; // 缓冲层，由 gRPC 工作线程推入新连接。
    std::vector<std::shared_ptr<RpcSession>> sessions_; // 主工作层，仅由控制循环进行管理和状态推进。
    std::shared_ptr<const proto::astra::v1::Hello> hello_; // 握手用的 Hello 封包数据缓存。
    std::atomic_uint64_t next_generation_{1};     // 原子变量，用于生成全进程唯一且递增的会话识别世代号。
    std::unique_ptr<Admission> admission_;        // Supervisor 交互管理器实例指针。
    
    // shutdown 完成所有 OnDone 后才释放 server 和 Admission, 不依赖成员析构顺序来取消 RPC.
    std::unique_ptr<grpc::Server> server_;        // gRPC 服务监听器句柄。
    bool fatal_{};                                // 指示是否遇到了导致程序崩溃退出级别的严重错误。
    std::uint32_t candidate_round_{};             // 当前节点重新竞选和刷新角色的逻辑轮次。
    std::uint32_t registration_failures_{};       // 与 Supervisor 通讯时连续发生错误的次数记录，用于计算退避。
    Clock::time_point next_registration_{};       // 下一次能够请求 Supervisor 进行验证的时钟点。
    Clock::time_point next_dial_{};               // 流量控制：允许发起下一次外呼请求的时间点。
    Clock::time_point next_status_{};             // 下一次定期打印节点状态的时钟点。
};
} // namespace

// run_node: 节点主进程调用入口。
// 参数 argc, argv: 命令行参数。
// 参数 role: 当前要运行为哪个角色（Star/Planet 等）。
// 参数 factory: 产生用于管理会话关系逻辑 Policy 的工厂方法。
// 返回值: 系统的返回错误码 (0 为正常，其他为错误)。
int run_node(int argc, char** argv, Role role, PolicyFactory factory) {
    try {
        // 帮助与版本查询不加载身份文件也不启动网络, 参数输入的 string_view 只借用 argv 本次调用.
        if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            std::cout << Config::help(role);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            // 简单区分输出版本信息
            std::cout << (role == Role::star ? "star" : "planet") << " 0.1.0 (C++26, gRPC v1)\n";
            return 0;
        }
        // 将 C 风格参数转换为 std::string_view 列表便于处理
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }
        // 调用统一解析函数得到具体配置
        auto config = Config::parse(arguments, role);
        if (!config) {
            std::cerr << config.error().message << '\n';
            return 2;
        }
        // 配置通过后先验证身份材料; 进程 ID 等待 Supervisor 签发, 重连复用准入结果.
        // 根据配置中指引的位置加载加密材料、公私钥对以建立节点唯一 Identity
        auto identity = Identity::load(config->identity, config->advertise);
        if (!identity) {
            std::cerr << identity.error().message << '\n';
            return 1;
        }
        // 挂载信号管理器以便支持优雅关机
        Signals signals;
        // 实例化 Runtime 对象
        Runtime runtime(*config, *identity, factory(*config));
        // 将线程阻塞在运行循环，直至完全关闭
        return runtime.run(signals);
    } catch (const std::exception&) {
        // 捕捉所有的致命 C++ 异常并给出通用退出信息
        std::cerr << "Internal service failure\n";
        return 1;
    }
}
} // namespace astra
