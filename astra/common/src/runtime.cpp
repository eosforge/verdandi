// 该文件实现了整个 Astra 节点的核心运行时环境, 它包含监听外部会话的 gRPC 服务端处理, 并管理所有主动拨出和被动接收的会话生命周期.
#include <astra/profile.hpp>
#include <astra/runtime.hpp>

#include "admission.hpp"
#include "catalog_service.hpp"
#include "ephemeris_service.hpp"
#include "exchange.hpp"
#include "grpc_session.hpp"
#include "intake.hpp"
#include "metrics.hpp"
#include "process.hpp"
#include "pulse_client.hpp"
#include "readout.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <grpc/support/time.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/server_builder.h>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace astra {
namespace {
// Rejection 类: 没有准入资格或超额的 RPC 立即 Finish. 只有 OnDone 一个释放点, 不进入业务状态集合.
// 用于在服务端拒绝非法或超出容量限制的入站连接请求.它继承自 gRPC 的双向流式服务端反应器 (ServerBidiReactor).
class Rejection final : public grpc::ServerBidiReactor<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket> {
public:
    // 构造函数: code 为固定拒绝状态, 构造时立即 Finish; 不进入 Runtime 会话集合, 由 OnDone 唯一释放.
    // 参数 code: 触发拒绝的 gRPC 状态码.
    explicit Rejection(grpc::StatusCode code) {
        // 立即调用 Finish 向对端发送关闭状态.
        Finish(grpc::Status(code, "Star is not accepting this session"));
    }

    // OnDone: gRPC 确认拒绝 RPC 最终完成后删除自身, 此路径没有共享会话所有者.
    // 资源在 RPC 流真正销毁时自动清理.
    void OnDone() override {
        delete this;
    }
};

// Runtime 类: 实现具体的服务端接口及事件循环控制逻辑.
class Runtime final : public proto::astra::v1::StarTransport::CallbackService {
public:
    // 构造函数: 接管已校验配置, 身份和角色策略, 进程 ID 在首次准入成功时安装; 不在构造时开始监听或准入.
    // 参数 config: 节点配置参数.
    // 参数 identity: 节点安全身份对象.
    // 参数 policy: 特定于角色的策略接口实现(如拓扑要求等).
    Runtime(Config config, std::shared_ptr<Identity> identity, std::unique_ptr<Policy> policy) : config_(std::move(config)), identity_(std::move(identity)), policy_(std::move(policy)), logger_(config_.role) {}

    // OpenSession: gRPC 回调方法, 处理新的双向流连接请求.handler 只做接纳和所有权登记. 慢解析, 验签, 策略和日志全部留给控制循环.
    // 参数 context: 服务端上下文环境.
    // 返回值: 新分配的流处理器反应器对象.
    grpc::ServerBidiReactor<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket>* OpenSession(grpc::CallbackServerContext* context) override {

        ASTRA_PROFILE_SCOPE("common.runtime.OpenSession");

        try {
            // 使用锁保护接收缓冲队列, 因为该调用运行在 gRPC 的工作线程上.
            ASTRA_PROFILE_BEGIN(profile_lock_60, "common.runtime.OpenSession.wait.lock");
            std::lock_guard lock(incoming_mutex_);
            ASTRA_PROFILE_END(profile_lock_60);
            // 如果不在接纳状态或当前节点根本不是 Star(如 Planet 角色不能接受连接), 返回拒绝会话.
            if (!accepting_ || config_.role != Member::Role::star) {
                return new Rejection(grpc::StatusCode::UNAVAILABLE);
            }

            // 超过最大并发入站连接限制, 返回资源耗尽状态.
            if (inbound_ >= config_.max_inbound) {
                return new Rejection(grpc::StatusCode::RESOURCE_EXHAUSTED);
            }

            // 为新会话分配一个递增的世代号以追踪生命周期.
            auto generation = next_generation();
            // 创建并接纳合法的请求, 初始化流控处理器
            auto session = std::make_shared<Inbound>(context, config_, generation, hello_, notifier());
            // reactor 为返回给 gRPC 的借用指针, session 必须先进入 Runtime 所有权容器.
            auto* reactor = session.get();
            // 存入 incoming_ 暂存队列, 等待控制循环消费.
            incoming_.push_back(std::move(session));
            ++inbound_;
            // 唤醒主控制循环处理新连接
            wake_->notify();
            return reactor;
        } catch (...) {
            // 发生未知异常时保护进程不崩溃, 拒绝连接
            return new Rejection(grpc::StatusCode::INTERNAL);
        }
    }

    // run: 主入口, 借用入口的信号所有者, 建立 TLS 监听后运行控制循环并排空关闭; 正常停止返回 0, 准入致命失败返回 1.
    // 监听或内部异常向入口传播, 已进入运行阶段的异常先执行 shutdown, 日志不输出异常正文.
    // 参数 signals: 捕获操作系统的终止等信号.
    // 返回值: 进程退出码.
    // run 为节点控制循环入口, 按信号驱动推进会话、目录与业务, 返回进程退出码.
    // 参数 signals: 退出信号源; 返回值: 0 正常, 非零故障.
    int run(const Signals& signals) {

        grpc::EnableDefaultHealthCheckService(true); // 使用 gRPC 自带标准健康服务, 不增加 Comet Inspect 握手.
        // 先取得实际监听端口再登记准入, 避免向 Pulsar 发布尚未绑定或端口仍为 0 的端点.
        grpc::ServerBuilder builder;
        builder.SetMaxReceiveMessageSize(static_cast<int>(config_.max_frame_bytes));
        builder.SetMaxSendMessageSize(static_cast<int>(config_.max_frame_bytes));
        builder.AddChannelArgument("grpc.server_handshake_timeout_ms", static_cast<int>(config_.handshake_timeout.count()));
        // 同一部署端点必须唯一, 不允许独立进程以 SO_REUSEPORT 分摊成不同身份.
        builder.AddChannelArgument("grpc.so_reuseport", 0);
        builder.AddChannelArgument("grpc.max_connection_idle_ms", 60000);
        // quota 约束此监听器的 gRPC 资源, 不替代会话数和消息队列的应用层上限.
        grpc::ResourceQuota quota;
        quota.Resize(32 * 1024 * 1024);
        builder.SetResourceQuota(quota);
        builder.RegisterService(this); // 注册自身服务
        // port 接收实际绑定端口, 初始零表示尚未确认成功监听.
        int port = 0;
        // 绑定配置中的监听地址, 并获取实际绑定的端口号
        builder.AddListeningPort(config_.listen.text(), identity_->server_credentials(), &port);
        server_ = builder.BuildAndStart();
        if (!server_ || port <= 0) {
            throw std::runtime_error("Cannot bind TLS gRPC listener");
        }
        if (auto* health = server_->GetHealthCheckService())
            health->SetServingStatus(false); // 身份/基线未就绪, 监听存在不等于业务可用.

        // 若配置中的通告地址未指明端口, 使用实际监听分配的端口
        if (config_.advertise.port == 0) {
            config_.advertise.port = static_cast<std::uint16_t>(port);
        }
        try {
            if (config_.metrics) {
                metrics_ = std::make_unique<Metrics>(*config_.metrics);
                logger_.write("metrics_started", "{\"listen\":\"" + metrics_->endpoint().text() + "\"}");
            }
            // 创建准入管理器
            admission_ = std::make_unique<Admission>(config_, identity_, notifier());
            logger_.write("started", "{\"advertise\":\"" + config_.advertise.text() + "\"}");
            // 进入主控制循环, 直到收到停止信号或遇到致命错误
            while (!signals.requested() && !fatal_) {
                // 观察唤醒锁状态
                // observed 保存当前唤醒序号, wait 仅在序号未变化时休眠, 避免检查与等待间丢失事件.
                const auto observed = wake_->observe();
                // 运行一次单步逻辑
                step();
                // 如果没有新事件触发, 则挂起等待唤醒
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
    // next_generation: 生成自增的世代 ID.generation 分配跨 handler 和控制循环, 只需要 atomic 自身的唯一性, 不用于发布对象状态.
    // 返回值: Generation, 包装好的非 0 唯一 ID.
    Generation next_generation() {

        // current 为 CAS 的期望代次, 失败会被更新, max 保留为耗尽边界.
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

        ASTRA_PROFILE_SCOPE("common.runtime.collect");

        // lock 协调 gRPC 接纳回调与控制循环的队列和接纳状态, 不在此执行网络等待.
        ASTRA_PROFILE_BEGIN(profile_lock_180, "common.runtime.collect.wait.lock");
        std::lock_guard lock(incoming_mutex_);
        ASTRA_PROFILE_END(profile_lock_180);
        // 使用 move_iterator 高效转移数据所有权
        sessions_.insert(sessions_.end(), std::make_move_iterator(incoming_.begin()), std::make_move_iterator(incoming_.end()));
        incoming_.clear();
    }

    // reap: 仅回收已发布 done 的会话, 根据是否安装身份通知 closed/failed, now 用于角色退避.
    // 入站容量在此释放, 避免取消尚未最终完成的 RPC 过早让出槽位.
    // 参数 now: 用于更新策略模块中失败时间记录的时间点.
    void reap(Steady::time_point now) {

        ASTRA_PROFILE_SCOPE("common.runtime.reap");

        std::erase_if(sessions_, [&](const std::shared_ptr<Session>& session) {
            // 未完成的会话保留在容器中
            if (!session->done()) {
                return false;
            }

            // error 是完成会话的最终分类, 空表示未记录具体错误.
            const auto error = session->error();
            // reason 给没有明确错误的断开提供 transport 诊断, 不输出远端正文.
            const auto reason = error.value_or(Status::Code::transport);
            // 已经完成鉴权及身份绑定的会话断开
            if (session->installed()) {
                policy_->closed(session->generation(), error, now);
                logger_.failure("disconnected", reason);
            } else if (session->expected()) {
                // 尚未完成绑定, 说明是建立连接过程中失败
                policy_->failed(*session->expected(), reason, now);
                logger_.failure("connect_failed", reason);
            }

            // 释放入站容量计数槽位
            if (session->direction() == Policy::Direction::inbound) {
                // lock 协调 gRPC 接纳回调与控制循环的队列和接纳状态, 不在此执行网络等待.
                ASTRA_PROFILE_BEGIN(profile_lock_214, "common.runtime.reap.wait.lock");
                std::lock_guard lock(incoming_mutex_);
                ASTRA_PROFILE_END(profile_lock_214);
                --inbound_;
            }

            // 返回 true, 使 erase_if 删除该项
            return true;
        });
    }

    // pump_sessions: 用同一单调时间推进当前会话, 执行合法替换返回的旧代次取消, 最后回收已完成对象.
    // 参数 now: 统一使用的推进逻辑单调时钟时间.
    void pump_sessions(Steady::time_point now) {

        ASTRA_PROFILE_SCOPE("common.runtime.pump_sessions");

        for (const auto& session : sessions_) {
            // installed 保存推进前状态, 用于只在本轮首次安装身份时记录连接事件.
            const bool installed = session->installed();
            // pump: 处理会话内的数据读取, 握手校验等逻辑, 并返回需要被替代的旧会话的世代号数组
            const auto cancelled = session->pump(*policy_, *identity_, now);
            // 如果刚刚完成了身份安装(握手成功), 则记录连接成功日志
            if (!installed && session->installed()) {
                logger_.write("connected");
            }

            // 对于新连接上位而需要踢掉的老连接, 找到它们并主动发送取消指令
            for (const auto generation : cancelled) {
                // old 定位要取消的旧代次, 查找失败说明已回收, 不影响新会话.
                const auto old = std::ranges::find(sessions_, generation, [](const auto& item) { return item->generation(); });
                if (old != sessions_.end()) {
                    (*old)->cancel();
                }
            }
            bind(session, now); // 本轮新 Hello 后立即接线, 下一轮任何数据到达前完成.
        }

        // 集中清理本轮操作中宣告结束的会话
        reap(now);
    }

    // 来源身份只接受受信名单/已验签 Hello, 保持每个部署最高代次, 不因断线或 TTL 清理忘记旧实例禁入.
    Result<void> observe(const Member& member) {

        ASTRA_PROFILE_SCOPE("common.runtime.observe");

        if (member.role != Member::Role::star || member.id == id_) {
            return {};
        }
        const auto found = sources_.find(member.principal);
        if (found == sources_.end()) {
            if (sources_.size() >= config_.max_members - 1) {
                return Status::capacity("Peer identity capacity exceeded");
            }
            sources_.emplace(member.principal, member);
            return {};
        }
        const auto replacement = supersedes(member, found->second);
        if (!replacement) {
            return std::unexpected(replacement.error());
        }
        if (*replacement) {
            Member prepared = member; // 先完成字符串分配, 退役后的身份发布不得再抛分配异常.
            catalog_->retire(found->second.id);
            ephemeris_->retire(found->second.id);
            found->second = std::move(prepared);
        }
        return {};
    }

    // 为已经安装的当前 Star 会话连接业务复制, 旧会话只取消不再推进数据, Planet 保持冻结控制路径.
    void bind(const std::shared_ptr<Session>& session, Steady::time_point now) {

        ASTRA_PROFILE_SCOPE("common.runtime.bind");

        if (!catalog_ || !ephemeris_ || !session->installed() || session->bound() || session->error() || !session->peer() || session->peer()->role != Member::Role::star) {
            return;
        }
        try {
            const auto accepted = observe(*session->peer());
            if (!accepted) {
                session->cancel(accepted.error().code);
                return;
            }
            const auto catalog = catalog_->admit(session->peer()->id);
            const auto ephemeris = ephemeris_->admit(session->peer()->id);
            if (!catalog || !ephemeris) {
                session->cancel(Status::Code::capacity);
                return;
            }
            session->bind(std::make_unique<Exchange>(*catalog_, *ephemeris_, session->peer()->id, recovery_, session->capacity(), now));
        } catch (const std::bad_alloc&) {
            session->cancel(Status::Code::capacity);
        } catch (...) {
            session->cancel(Status::Code::internal);
        }
    }

    // 首轮来源恢复采用 30 s 有界等待; 超时只允许明确降级开放, 不将缺失来源或半份基线标为成功.
    void ready(Steady::time_point now, bool clock) {

        ASTRA_PROFILE_SCOPE("common.runtime.ready");

        if (business_ready_ || !clock || !almanac_ready_ || config_.role != Member::Role::star) {
            return;
        }
        if (!recovery_deadline_) {
            recovery_deadline_ = now + std::chrono::seconds(30);
        }
        std::size_t complete{}; // 只统计当前最高身份且当前有效流已完成双域初始化的来源.
        for (const auto& [principal, member] : sources_) {
            (void)principal;
            complete += std::ranges::any_of(sessions_, [&](const auto& session) { return session->peer() && session->peer()->id == member.id && session->synchronized(); });
        }
        if (complete == sources_.size() || now >= *recovery_deadline_) {
            business_ready_ = true;
            if (gateway_) {
                gateway_->ready();
            }
            if (auto* health = server_->GetHealthCheckService())
                health->SetServingStatus(true);
            if (public_) {
                if (auto* health = public_->GetHealthCheckService())
                    health->SetServingStatus(true);
            }
            logger_.write(complete == sources_.size() ? "business_ready" : "business_degraded", "{\"synchronized\":" + std::to_string(complete) + ",\"sources\":" + std::to_string(sources_.size()) + "}");
        }
    }

    // advance_admission: 准入独立推进首次登记与候选刷新, 失败保留原有重试期限和本地身份.
    // 参数 now: 当前时钟时间.
    void advance_admission(Steady::time_point now) {

        ASTRA_PROFILE_SCOPE("common.runtime.advance_admission");

        // 如果注册任务已有结果
        if (auto result = admission_->poll()) {
            if (*result) {
                // 成功注册
                auto& joined = **result;
                if (hello_ && config_.role == Member::Role::star) {
                    auto refreshed = policy_->refresh(joined.local, joined.members); // 迟到名单不能回退已验签的新身份.
                    if (!refreshed) {
                        fatal_ = true;
                        logger_.failure("directory_rejected", refreshed.error().code);
                        return;
                    }
                    for (const auto generation : *refreshed) {
                        for (const auto& session : sessions_) {
                            if (session->generation() == generation) {
                                session->cancel();
                            }
                        }
                    }
                } else {
                    if (auto initialized = policy_->initialize(joined.local, joined.members); !initialized) {
                        fatal_ = true;
                        logger_.failure("registration_rejected", initialized.error().code);
                        return;
                    }
                }

                // 首次连接成功, 设置核心凭证对象
                if (!hello_) {
                    // lock 协调 gRPC 接纳回调与控制循环的队列和接纳状态, 不在此执行网络等待.
                    ASTRA_PROFILE_BEGIN(profile_lock_368, "common.runtime.advance_admission.wait.lock");
                    std::lock_guard lock(incoming_mutex_);
                    ASTRA_PROFILE_END(profile_lock_368);
                    id_ = joined.local.id;
                    hello_ = joined.hello;
                    accepting_ = config_.role != Member::Role::star;
                    logger_.write("initialized", "{\"id\":" + json_string(id_) + "}");
                }

                // 重置重试计数器
                registration_failures_ = 0;
                if (config_.role == Member::Role::star) {
                    next_registration_ = now + Milliseconds(30000 + std::hash<std::string>{}(id_) % 5001);
                    if (!almanac_) {
                        almanac_ = std::make_unique<Library>(Library::Limits{}, [this](const Scope& scope, const Almanac::Change& change) noexcept {
                            if (readout_) {
                                readout_->changed(scope, change);
                            }
                        });
                        access_ = std::make_unique<Access>();
                        ephemeris_ = std::make_unique<Ephemeris::State>([this] { return synchronized_clock_.now(); }, Ephemeris::State::Limits{.source = {}, .projection = {}, .replicas = config_.max_members - 1});
                        catalog_ = std::make_unique<Catalog::State>([this] { return synchronized_clock_.now(); }, Catalog::State::Limits{.source = {}, .projection = {}, .replicas = config_.max_members - 1});
                        if (config_.comet) {
                            gateway_ = std::make_unique<Gateway>(*access_, id_, config_.auth);
                            readout_ = std::make_unique<Readout>(*almanac_, *gateway_, notifier());
                            ephemeris_service_ = std::make_unique<Ephemeris::Service>(*ephemeris_, *gateway_, notifier());
                            catalog_service_ = std::make_unique<Catalog::Service>(*catalog_, *gateway_, notifier());
                            open_comet();
                        }
                    }
                    for (const auto& member : joined.members) {
                        const auto found = sources_.find(member.principal);
                        if (found != sources_.end() && member.epoch < found->second.epoch) {
                            continue; // 查询可以滞后于已验签 Hello, 不回退部署身份.
                        }
                        if (const auto accepted = observe(member); !accepted) {
                            fatal_ = true;
                            logger_.failure("source_identity_rejected", accepted.error().code);
                            return;
                        }
                    }
                    if (!joined.services.empty()) {
                        auto candidate = joined.services.front(); // 已通过受信完整名单的角色/别名校验.
                        if (polaris_ && candidate.principal != polaris_->principal) {
                            polaris_conflict_ = true;
                            if (intake_) {
                                intake_->cancel(Status::Code::conflict);
                            }
                            logger_.failure("polaris_conflict", Status::Code::conflict);
                        } else if (!polaris_ || candidate.epoch >= polaris_->epoch) {
                            if (polaris_ && !supersedes(candidate, *polaris_)) {
                                polaris_conflict_ = true;
                                if (intake_) {
                                    intake_->cancel(Status::Code::identity);
                                }
                            } else {
                                if (intake_ && candidate != intake_->target()) {
                                    intake_->cancel();
                                }
                                polaris_ = std::move(candidate);
                                polaris_conflict_ = false;
                            }
                        }
                    }
                }
                // Star 对时线程只在准入成功后启动, 复用本进程凭证. 旧 Go 控制面没有 Pulse 字段时显式保持未就绪.
                if (config_.role == Member::Role::star && joined.pulse_endpoint != pulse_endpoint_) {
                    pulse_.reset();
                    pulse_endpoint_ = joined.pulse_endpoint;
                    if (!pulse_endpoint_.empty()) {
                        pulse_ = std::make_unique<Sampler>(pulse_endpoint_, identity_, hello_, synchronized_clock_);
                    }
                }
            } else {
                // 注册失败
                const auto code = result->error().code;
                if (admission_->revoked()) {
                    fatal_ = true;
                    logger_.failure("admission_revoked", code);
                    return;
                }
                // temporary 仅允许传输,超时和容量失败在首次准入时进入重试.
                const bool temporary = code == Status::Code::transport || code == Status::Code::timeout || code == Status::Code::capacity;
                if (config_.role == Member::Role::star && hello_ && !temporary) {
                    // 不可信/冲突名单暂停新的权威安装, 完整旧底稿仍保留, 下次只读查询可以恢复.
                    polaris_conflict_ = true;
                    if (intake_) {
                        intake_->cancel(code);
                    }
                }
                // 对于非临时错误且尚未初始化的, 认定为严重致命错误直接退出
                if (!temporary && !hello_) {
                    fatal_ = true;
                    logger_.failure("registration_rejected", code);
                    return;
                }

                // 是临时错误或者是已经初始化过的节点(可以进行带有退避的重试)
                registration_failures_ = std::min(registration_failures_ + 1, 100U);
                next_registration_ = now + retry_delay(registration_failures_, config_, std::hash<std::string>{}(id_.empty() ? config_.advertise.text() : id_));
                logger_.failure("registration_retry", code);
            }
        }

        // 当不在请求中并且已经到了重试或定时检查的时间点时
        if (!admission_->pending() && now >= next_registration_) {
            if (!hello_) {
                // 还没拿到入场券, 发起初始注册, 0 作为第0轮候选
                admission_->begin(0);
            } else if (config_.role == Member::Role::star) {
                admission_->begin(0); // 已登记 Star 复用准入 metadata 调用 List, 不再发送密码.
            } else if (policy_->needs_refresh(now)) {
                // 如果策略判断需要向 Pulsar 刷新候选列表
                if (candidate_round_ == std::numeric_limits<std::uint32_t>::max()) {
                    fatal_ = true;
                    return;
                }

                // 启动带有自增轮次的刷新请求
                admission_->begin(++candidate_round_);
            }
        }
    }

    // advance_almanac 在既有控制循环接收有界页, 不为每个分组创建线程或独立连接.
    // clock 表示本进程绝对时间已建立, 后续 holdover 不撤销已完成的启动资格.
    void advance_almanac(Steady::time_point now, bool clock) {

        ASTRA_PROFILE_SCOPE("common.runtime.advance_almanac");

        if (config_.role != Member::Role::star || !hello_ || !almanac_) {
            return;
        }
        if (intake_) {
            intake_->pump(now);
            if (polaris_ && intake_->target().principal == polaris_->principal && intake_->target().epoch > polaris_->epoch) {
                polaris_ = intake_->target(); // 已验签的新实例优先于下一轮可能迟到的查询.
            }
            if (intake_->ready() && !almanac_ready_) {
                almanac_ready_ = true;
                logger_.write("almanac_initialized");
            }
            if (intake_->done()) {
                logger_.failure("almanac_disconnected", intake_->error().value_or(Status::Code::transport));
                intake_.reset();
                almanac_failures_ = std::min(almanac_failures_ + 1, 100U);
                next_almanac_ = now + retry_delay(almanac_failures_, config_, std::hash<std::string>{}(id_));
            } else if (intake_->ready()) {
                almanac_failures_ = 0;
            }
        }

        if (!intake_ && polaris_ && !polaris_conflict_ && clock && now >= next_almanac_) {
            intake_ = std::make_unique<Intake>(identity_, *hello_, *polaris_, *almanac_, *access_, notifier());
        }
        if (almanac_ready_ && clock) {
            ASTRA_PROFILE_BEGIN(profile_lock_520, "common.runtime.advance_almanac.wait.lock");
            const std::lock_guard lock(incoming_mutex_); // 初始底稿完成后才接纳 Star 对等流.
            ASTRA_PROFILE_END(profile_lock_520);
            accepting_ = true;
        }
    }

    // 显式业务端口只挂公共服务. 与内部端口完全独立的 TLS/消息预算, 不注册 Orbit/Pulse/Polaris 写入.
    void open_comet() {

        ASTRA_PROFILE_SCOPE("common.runtime.open_comet");

        auto credentials = config_.tls ? Identity::external(config_.comet_identity) : Result<std::shared_ptr<grpc::ServerCredentials>>(grpc::InsecureServerCredentials());
        if (!credentials) {
            throw std::runtime_error("Cannot prepare independent Comet transport");
        }
        grpc::ServerBuilder builder;
        builder.SetMaxReceiveMessageSize(8 * 1024 * 1024);
        builder.SetMaxSendMessageSize(8 * 1024 * 1024);
        builder.AddChannelArgument("grpc.so_reuseport", 0);
        builder.AddChannelArgument("grpc.server_handshake_timeout_ms", 5000);
        builder.AddChannelArgument("grpc.http2.min_recv_ping_interval_without_data_ms", 60000);
        builder.AddChannelArgument("grpc.keepalive_time_ms", 60000);
        builder.AddChannelArgument("grpc.keepalive_timeout_ms", 20000);
        builder.AddChannelArgument("grpc.keepalive_permit_without_calls", 0);
        grpc::ResourceQuota quota; // 接收/解码和传输资源另设上限, 不等同于 Readout 的逻辑保有预算.
        quota.Resize(64 * 1024 * 1024);
        builder.SetResourceQuota(quota);
        builder.RegisterService(gateway_.get());
        builder.RegisterService(readout_.get());
        builder.RegisterService(ephemeris_service_.get());
        builder.RegisterService(catalog_service_.get());
        int port{}; // 实际绑定的端口只记录在公开日志, 不覆盖内部 advertise.
        builder.AddListeningPort(config_.comet->text(), *credentials, &port);
        public_ = builder.BuildAndStart();
        if (!public_ || port <= 0) {
            throw std::runtime_error("Cannot bind independent Comet listener");
        }
        if (auto* health = public_->GetHealthCheckService()) {
            health->SetServingStatus(false);
            for (const auto name : {"proto.comet.v1.Gateway", "proto.comet.v1.Almanac", "proto.comet.v1.Catalog", "proto.comet.v1.Ephemeris"})
                health->SetServingStatus(name, false);
        }
        config_.comet->port = static_cast<std::uint16_t>(port);
        logger_.write("comet_listening", "{\"endpoint\":" + json_string(config_.comet->text()) + "}");
    }

    // dial: 拨号预算跨轮次生效, 每轮最多建立一个流, 避免集中启动时出现连接突发.
    // 参数 now: 当前时钟.
    void dial(Steady::time_point now) {

        ASTRA_PROFILE_SCOPE("common.runtime.dial");

        if (hello_ && (config_.role != Member::Role::star || almanac_ready_) && now >= next_dial_) {
            // 计算当前尚未建立起稳定通信(即正在连接/握手中的)外呼会话的数量
            const auto pending = std::count_if(sessions_.begin(), sessions_.end(), [](const auto& session) { return session->direction() == Policy::Direction::outbound && !session->installed(); });
            // 如果还未超出并行拨号的限制
            if (static_cast<std::size_t>(pending) < config_.max_dials) {
                // 询问策略: 当前是否有需要进行外呼的对端目标
                if (auto target = policy_->due(now)) {
                    // 发起实际的出站连接创建并加入循环监控
                    sessions_.push_back(connect_session(config_, *identity_, next_generation(), hello_, std::move(*target), notifier()));
                    // 设置下一个允许外拨的时间点 (限制频率为每 250ms 最多建立一条, 避免拥塞)
                    next_dial_ = now + Milliseconds(250);
                }
            }
        }
    }

    // step: 控制循环是协议和角色状态的唯一推进者. 顺序为回收完成, 准入, 拨号, 诊断.
    void step() {

        ASTRA_PROFILE_SCOPE("common.runtime.step");

        // now 是本控制轮共用的单调时间, 用于会话截止,退避和历史保留.
        const auto now = Steady::now();
        collect();              // 取出来自 gRPC 的新连接
        pump_sessions(now);     // 处理每个会话的状态读写
        advance_admission(now); // 推动与 Pulsar 注册状态的更新
        if (fatal_) {
            return;
        }
        // time 是本轮唯一业务读数. 未初始化时不猜测有限截止, 失联后本地推进及有限期限能力保持可用.
        const auto time = synchronized_clock_.now();
        advance_almanac(now, time.has_value());
        if (readout_) {
            readout_->pump(now);
        }
        if (catalog_service_) {
            catalog_service_->pump(now);
        }
        if (ephemeris_service_) {
            ephemeris_service_->pump(now); // 来源期限和活动 Watch 由同一个既有控制循环推进.
        }
        ready(now, time.has_value()); // 完整初始来源或明确有界降级后才开放 Comet.
        dial(now);                    // 准入、时钟和初始 Almanac 完成后开始对等互联.

        // synchronized 只报告参考质量, 与本地计时是否可用分离; holdover 不使注册和续租失去时间资格.
        const bool synchronized = time && time->synchronized;
        if (metrics_ && now >= next_metrics_) {
            const auto state = policy_->status(); // 只读有界拓扑计数, 不遍历业务 Key 或 Scope.
            static_cast<void>(metrics_->publish({id_, business_ready_, almanac_ready_, time && time->ready, synchronized, time ? time->uncertainty_ns : 0, state.members, state.inbound + state.outbound, recovery_.used}));
            next_metrics_ = now + std::chrono::seconds(1); // 不为每个 HTTP 请求重采样或争用业务锁.
        }
        if (config_.role == Member::Role::star && synchronized != reported_synchronized_) {
            reported_synchronized_ = synchronized;
            logger_.write(synchronized ? "clock_synchronized" : time ? "clock_holdover"
                                                                     : "clock_unavailable");
        }

        // 按配置的时间间隔定期打印健康状态
        if (config_.status_interval.count() != 0 && now >= next_status_) {
            logger_.status(policy_->status(), id_);
            if (config_.role == Member::Role::star) {
                logger_.write("clock_status", time ? std::string("{\"ready\":") + (time->ready ? "true" : "false") + ",\"synchronized\":" + (synchronized ? "true" : "false") + ",\"nanoseconds\":" + std::to_string(time->time.time_since_epoch().count()) + ",\"uncertainty_ns\":" + std::to_string(time->uncertainty_ns) + ",\"rtt_ns\":" + std::to_string(time->rtt_ns) + "}" : "{\"ready\":false,\"synchronized\":false}");
            }
            next_status_ = now + config_.status_interval;
        }
    }

    // shutdown: 停止接纳并取消所有已拥有 RPC, 在统一截止内继续推进完成, 然后关闭服务器.
    // 截止耗尽仍无法排空会话时记录固定事件并 _Exit(1), 不强行析构仍被 gRPC 借用的对象.
    void shutdown() {

        // deadline 是本次关闭各阶段共用的截止, 后续阶段只消费剩余预算.
        const auto deadline = Steady::now() + config_.shutdown_timeout;
        if (server_) {
            if (auto* health = server_->GetHealthCheckService())
                health->SetServingStatus(false);
        }
        if (public_) {
            if (auto* health = public_->GetHealthCheckService())
                health->SetServingStatus(false);
        }
        metrics_.reset(); // 停止独立指标线程, 不让关闭中的节点继续报告先前 ready 状态.
        if (gateway_) {
            gateway_->stop();
        }
        if (readout_) {
            readout_->stop();
        }
        if (catalog_service_) {
            catalog_service_->stop();
        }
        if (ephemeris_service_) {
            ephemeris_service_->stop();
        }
        if (pulse_) {
            pulse_->stop();
        }
        {
            // lock 协调 gRPC 接纳回调与控制循环的队列和接纳状态, 不在此执行网络等待.
            std::lock_guard lock(incoming_mutex_);
            accepting_ = false; // 标记关闭, 不再接纳新的 RPC 会话
        }
        if (admission_) {
            admission_->cancel(); // 取消正在进行的准入请求
        }
        if (intake_) {
            intake_->cancel();
        }
        collect();
        // 向所有持有的活动会话发出取消信号
        for (const auto& session : sessions_) {
            session->cancel();
        }

        // 控制循环继续推进 Finish/RemoveHold 和 OnDone, 不能先 join 或销毁仍被回调借用的对象.
        // 直到队列变空, 才安全退出.
        while (!sessions_.empty() || (admission_ && admission_->pending()) || intake_ || (readout_ && !readout_->empty()) || (ephemeris_service_ && !ephemeris_service_->empty()) || (catalog_service_ && !catalog_service_->empty())) {
            // observed 保存当前唤醒序号, wait 仅在序号未变化时休眠, 避免检查与等待间丢失事件.
            const auto observed = wake_->observe();
            pump_sessions(Steady::now());
            if (readout_) {
                readout_->pump(Steady::now());
            }
            if (catalog_service_) {
                catalog_service_->pump(Steady::now());
            }
            if (ephemeris_service_) {
                ephemeris_service_->pump(Steady::now());
            }
            if (admission_) {
                static_cast<void>(admission_->poll());
            }
            if (intake_ && intake_->done()) {
                intake_.reset();
            }
            if (Steady::now() >= deadline) {
                // 超时无法完全排空, 为了防止破坏正在被 gRPC 核心引用的内存数据, 直接系统级强制退出.
                logger_.write("shutdown_timeout");
                std::_Exit(1);
            }
            wake_->wait(observed);
        }

        // 平缓关闭 gRPC 服务端监听, 在设定的截止时间内
        if (public_) {
            const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(std::max(deadline - Steady::now(), Steady::duration::zero())).count();
            public_->Shutdown(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_nanos(remaining, GPR_TIMESPAN)));
            public_->Wait();
            public_.reset();
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(std::max(deadline - Steady::now(), Steady::duration::zero())).count();
        server_->Shutdown(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_nanos(remaining, GPR_TIMESPAN)));
        server_->Wait();
        server_.reset();
        admission_.reset();
        pulse_.reset();
        logger_.write("stopped");
    }

    Config config_;                                             // 全局配置实例.
    std::shared_ptr<Identity> identity_;                        // 节点证书和凭证管理者.
    Id id_;                                                     // 节点自身的 ID, 首次向 Pulsar 注册后确定.
    std::unique_ptr<Policy> policy_;                            // 动态角色策略协调器.
    Logger logger_;                                             // 日志器实例, 打印格式化日志输出.
    std::shared_ptr<Wakeup> wake_ = std::make_shared<Wakeup>(); // 同步机制, 当有异步网络事件时触发.

    Exchange::Budget recovery_;                           // 比所有 Session 数据协作者活得更久, 跨流约束恢复额外工作区.
    std::map<Principal, Member> sources_;                 // 每部署当前可信身份, 缺项/失联不删除, 上限受 max_members 控制.
    std::optional<Steady::time_point> recovery_deadline_; // 首轮 Almanac/Clock 后开始的来源等待截止.
    bool business_ready_{};                               // 一次开放后不因后续来源离线撤销本地业务能力.
    std::unique_ptr<Metrics> metrics_;                    // 可选独立指标线程, 只共享不可变固定规模快照.
    Steady::time_point next_metrics_{};                   // 下次允许发布指标的单调时间, 初始立即采样.

    // handler 只写 incoming_/inbound_, collect 之后的 sessions_ 只归控制循环. hello_ 安装后不再修改.
    std::mutex incoming_mutex_;                            // 互斥锁, 用于跨线程保护 incoming_ 和 accepting_.
    bool accepting_{};                                     // 服务端是否还愿意接收新的入站连接, 初始值为 false.
    std::size_t inbound_{};                                // 当前处理中以及被接纳的入站连接总数计数器.
    std::vector<std::shared_ptr<Session>> incoming_;       // 缓冲层, 由 gRPC 工作线程推入新连接.
    std::vector<std::shared_ptr<Session>> sessions_;       // 主工作层, 仅由控制循环进行管理和状态推进.
    std::shared_ptr<const proto::astra::v1::Hello> hello_; // 握手用的 Hello 封包数据缓存.
    std::atomic_uint64_t next_generation_{1};              // 原子变量, 用于生成全进程唯一且递增的会话识别世代号.
    std::unique_ptr<Admission> admission_;                 // Pulsar 交互管理器实例指针.
    // 时钟先于借用它的动态状态/对时线程声明, 后于这些所有者销毁.
    Clock synchronized_clock_;
    // 自有来源和公开投影使用原生注册结构, 不再以通用 Store 作为业务占位.
    std::unique_ptr<Catalog::State> catalog_; // 自有 Catalog 内容版本/水位和有限 TTL.
    std::unique_ptr<Ephemeris::State> ephemeris_;
    // Almanac 原生数据先于接收流声明, 流彻底退出后才释放完整已安装底稿.
    std::unique_ptr<Library> almanac_;
    // 与内部凭据共同提交的业务会话索引, 比 Intake 与后续公共 RPC 活得更久.
    std::unique_ptr<Access> access_;
    // 公共服务先于 Library/Access 释放, 实际 Server 关闭后才销毁这些被借用的对象.
    std::unique_ptr<Gateway> gateway_;
    std::unique_ptr<Readout> readout_;
    // 同一个公共服务包含原生写入和活动 Watch, Server 排空后才释放.
    std::unique_ptr<Catalog::Service> catalog_service_; // 公开单 Key 写入与动态内容 Watch.
    std::unique_ptr<Ephemeris::Service> ephemeris_service_;
    // 独立业务监听, 内部 RPC 永远不挂入本 Server.
    std::unique_ptr<grpc::Server> public_;
    // 当前同步流包含退出中对象, 直到 OnDone 才归还唯一槽位.
    std::unique_ptr<Intake> intake_;
    // 同一部署最新已验证 Polaris, 不以缺项列表或暂时离线删除替换依据.
    std::optional<Member> polaris_;
    // 名单存在歧义时暂停安装, 默认 false, 不根据网络可达性擅自选主.
    bool polaris_conflict_{};
    // 首轮完整 Almanac 安装事实, 一旦成立保留, 失联不回滚或把所有业务变为未初始化.
    bool almanac_ready_{};
    // 同步失败次数和下一次重连时间, 默认零, 完整就绪后清零失败数.
    std::uint32_t almanac_failures_{};
    // 下一次允许建流的单调截止, 不重置已经安装的业务版本.
    Steady::time_point next_almanac_{};
    // 拥有独立对时线程, 初始为空, 准入取得 Pulse 地址后创建并在关闭时排空.
    std::unique_ptr<Sampler> pulse_;
    // 当前采样端点和已经报告的质量状态, 只由 Runtime 控制线程访问.
    std::string pulse_endpoint_;
    // 上次已记录的同步质量, 初始 false, 只在质量变化时输出事件; 不代表业务是否可以续租.
    bool reported_synchronized_{};

    // shutdown 完成所有 OnDone 后才释放 server 和 Admission, 不依赖成员析构顺序来取消 RPC.
    std::unique_ptr<grpc::Server> server_;   // gRPC 服务监听器句柄.
    bool fatal_{};                           // 指示是否遇到了导致程序崩溃退出级别的严重错误.
    std::uint32_t candidate_round_{};        // 当前节点重新竞选和刷新角色的逻辑轮次.
    std::uint32_t registration_failures_{};  // 与 Pulsar 通讯时连续发生错误的次数记录, 用于计算退避.
    Steady::time_point next_registration_{}; // 下一次能够请求 Pulsar 进行验证的时钟点.
    Steady::time_point next_dial_{};         // 流量控制: 允许发起下一次外呼请求的时间点.
    Steady::time_point next_status_{};       // 下一次定期打印节点状态的时钟点.
};
} // namespace

// run_node: 节点主进程调用入口.
// 参数 argc, argv: 命令行参数.
// 参数 role: 当前要运行为哪个角色(Star/Planet 等).
// 参数 factory: 产生用于管理会话关系逻辑 Policy 的工厂方法.
// 返回值: 系统的返回错误码 (0 为正常, 其他为错误).
// run_node 为 Star/Planet 共用进程入口, 解析角色专属配置后托管运行时.
// argc/argv 为命令行参数; role 为节点角色; factory 为角色策略工厂.
// 返回进程退出码, 参数错误返回 2, 运行故障返回 1.
int run_node(int argc, char** argv, Member::Role role, PolicyFactory factory) {

    try {
        // 帮助与版本查询不加载身份文件也不启动网络, 参数输入的 string_view 只借用 argv 本次调用.
        if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            std::cout << Config::help(role);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            // 简单区分输出版本信息
            std::cout << (role == Member::Role::star ? "star" : "planet") << " 0.1.0 (C++26, gRPC v1)\n";
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

        // 配置通过后先验证身份材料; 进程 ID 等待 Pulsar 签发, 重连复用准入结果.
        // 根据配置中指引的位置加载加密材料, 公私钥对以建立节点唯一 Identity
        auto identity = Identity::load(config->identity, config->advertise);
        if (!identity) {
            std::cerr << identity.error().message << '\n';
            return 1;
        }

        // 挂载信号管理器以便支持优雅关机
        Signals signals;
        // 实例化 Runtime 对象
        Runtime runtime(*config, *identity, factory(*config));
        // 将线程阻塞在运行循环, 直至完全关闭
        return runtime.run(signals);
    } catch (const std::exception&) {
        // 捕捉所有的致命 C++ 异常并给出通用退出信息
        std::cerr << "Internal service failure\n";
        return 1;
    }
}
} // namespace astra
