#include "check.hpp"
#include "fixture.hpp"
#include "grpc_session.hpp"

#include <iostream>
#include <stdexcept>
#include <thread>

using namespace astra;

namespace {

// 测试专用凭据使用仓库公开私钥, 不读取开发者部署身份.
std::shared_ptr<proto::astra::v1::Hello> hello(const Identity& identity) {

    // member 拥有测试用签名身份, 字段与仓库公开夹具保持一致.
    proto::orbit::v1::Member member;
    member.set_galaxy("alpha");
    member.set_group("default");
    member.set_id("00000001000040008000000000000000");
    member.set_advertise("127.0.0.1:7443");
    member.set_principal(identity.principal(member.galaxy(), member.advertise()).text());
    member.set_epoch(1);
    member.set_role(proto::orbit::v1::ROLE_STAR);
    // result 共享不可变握手内容, 使用真实测试签名供会话层验证.
    auto result = std::make_shared<proto::astra::v1::Hello>();
    result->set_protocol_major(protocol_major);
    result->set_max_frame_bytes(4096);
    result->set_admission(member.SerializeAsString());
    result->set_admission_signature(test::sign(result->admission()));
    return result;
}

// 仅记录传输是否在认证之后安装身份, 角色规则由 core_test 独立覆盖.
struct Recorder final : Policy {
    // installations 从零记录成功进入 accept 的次数, 证明握手前不会安装业务身份.
    unsigned installations{};

    // 记录夹具不检查名单, 未使用参数仅满足 Policy 契约, 角色校验由独立用例覆盖.
    Result<void> initialize(const Member&, std::span<const Member>) override {
        return {};
    }

    // 每次接纳递增安装次数, 不模拟替换旧连接, 返回空取消列表.
    Result<std::vector<Generation>> accept(const Member&, Policy::Direction, Generation, const std::optional<Member>&) override {
        ++installations;
        return std::vector<Generation>{};
    }

    // 关闭通知为空操作, 此夹具只观察安装时序而不维护真实拓扑.
    void closed(Generation, std::optional<Status::Code>, Steady::time_point) override {}

    // 不创建实际拨号目标, 返回空以隔离会话单元测试.
    std::optional<Member> due(Steady::time_point) override {
        return {};
    }

    // 忽略拨号失败通知, 退避逻辑由 core_test 验证.
    void failed(const Member&, Status::Code, Steady::time_point) override {}

    // 夹具不发起控制面刷新, 固定返回 false.
    bool needs_refresh(Steady::time_point) override {
        return false;
    }

    // 返回空诊断快照, 当前夹具的可观察结果是 installations.
    Policy::State status() const override {
        return {};
    }
};

// 人工控制回调顺序, 覆盖真实网络难以稳定复现的停滞写与截止竞争.
struct Manual final : Session {
    using Session::Session;
    // receiving 借用唯一在途读缓冲, deliver 发布完成后立即清空.
    proto::astra::v1::SessionPacket* receiving{};
    // last_write 复制最近提交的发送内容, 只用于断言, 不模拟网络所有权.
    proto::astra::v1::SessionPacket last_write;
    // reads/writes/finishes/calls/forced 分别计数读,写,结束,启动和强制取消, 初始均为零.
    unsigned reads{}, writes{}, finishes{}, calls{}, forced{};
    // ready 初始为 true, 出站停滞用例显式改为 false 以模拟通道未就绪.
    bool ready = true;
    // finish_immediately 默认同步完成假 RPC, 设 false 可分离 Finish 与 OnDone 时序.
    bool finish_immediately = true;

    // 向基类发布最终成功完成, 只在测试显式要求结束时调用.
    void complete() {
        completed(grpc::Status::OK);
    }

    // 返回人工指定的通道就绪状态, 不执行真实网络查询.
    bool transport_ready() const override {
        return ready;
    }

    // 将拥有的 packet 交换进唯一在途缓冲并发布读完成, 未提交读取时直接断言.
    void deliver(proto::astra::v1::SessionPacket packet) {

        CHECK(receiving);
        receiving->Swap(&packet);
        receiving = nullptr;
        read_done(true);
    }

    // 人工发布成功写完成, 使基类允许复用发送缓冲.
    void acknowledge() {
        write_done(true);
    }

    // 人工发布初始元数据发送完成, 解除后续消息发送的等待.
    void acknowledge_metadata() {
        metadata_done(true);
    }

    // 启动钩子由具体夹具计数或保持为空, 均不创建真实 RPC.
    void begin_call() override {
        ++calls;
    }

    // 接收 packet 读缓冲借用, 由夹具的交付路径填充, 禁止覆盖尚在途的缓冲.
    void begin_read(proto::astra::v1::SessionPacket* packet) override {

        CHECK(!receiving);
        receiving = packet;
        ++reads;
    }

    // 记录本次写入操作, 由夹具人工或另一线程发布完成, 不在此同步回调基类.
    void begin_write(const proto::astra::v1::SessionPacket* packet) override {
        last_write = *packet;
        ++writes;
    }

    // 接收取消请求; 控制夹具统计 force, 并发夹具不拥有可取消的真实网络.
    void request_cancel(bool force) override {
        forced += force;
    }

    // 记录或忽略假传输的结束钩子, 不模拟外部 gRPC 生命周期.
    void finish_call(Status::Code) override {

        ++finishes;
        if (finish_immediately) {
            complete();
        }
    }
};

// 构造指定 id 的 Ping 或 Pong, response 默认 false, 允许调用方注入非法序号.
proto::astra::v1::SessionPacket ping(std::uint64_t id, bool response = false) {

    // result 独立拥有一个心跳分支, 返回后不借用输入参数.
    proto::astra::v1::SessionPacket result;
    if (response) {
        result.mutable_pong()->set_request_id(id);
    } else {
        result.mutable_ping()->set_request_id(id);
    }
    return result;
}

// 真正从另一线程交付读取完成, 检查每次重新提交的缓冲区都已由控制循环消费.
// 不在测试中修改 gRPC 私有状态, 不依赖一次 sleep 恰好命中竞争窗口.
class Concurrent final : public Session {
public:
    // 接管 greeting 的共享所有权并启动交付线程, config 被基类复制, 线程只通过回调交接缓冲.
    Concurrent(const Config& config, std::shared_ptr<const proto::astra::v1::Hello> greeting) : Session(config, Policy::Direction::inbound, {8}, greeting, std::nullopt, [] {}), worker_([this, greeting](std::stop_token stop) { run(stop, *greeting); }) {}

    // 在并发交付夹具中显式完成响应头, 允许发送后续消息.
    void ready() {
        metadata_done(true);
    }

    // 以原子读取返回已经交付的消息数, 用于有界等待完成条件.
    unsigned delivered() const {
        return delivered_.load();
    }

private:
    // 唯一交付线程借用 greeting, 所有权由线程闭包保留; stop 随 jthread 析构请求停止.
    void run(std::stop_token stop, const proto::astra::v1::Hello& greeting) {

        while (!stop.stop_requested()) {
            // packet 取得控制循环发布的独占缓冲, 首条填 Hello, 后续填不同序号 Ping.
            if (auto* packet = reading_.exchange(nullptr)) {
                if (delivered_ == 0) {
                    packet->mutable_hello()->CopyFrom(greeting);
                } else {
                    packet->mutable_ping()->set_request_id(delivered_ + 1);
                }
                read_done(true);
                ++delivered_;
            }

            // 每次发布后释放写槽, 保留显式让步以给控制循环机会消费完成状态.
            if (writing_.exchange(false)) {
                write_done(true);
            }
            std::this_thread::yield();
        }
    }

    // 启动钩子由具体夹具计数或保持为空, 均不创建真实 RPC.
    void begin_call() override {}

    // 接收 packet 读缓冲借用, 由夹具的交付路径填充, 禁止覆盖尚在途的缓冲.
    void begin_read(proto::astra::v1::SessionPacket* packet) override {
        CHECK(packet->ByteSizeLong() == 0);
        CHECK(reading_.exchange(packet) == nullptr);
    }

    // 记录本次写入操作, 由夹具人工或另一线程发布完成, 不在此同步回调基类.
    void begin_write(const proto::astra::v1::SessionPacket*) override {
        CHECK(!writing_.exchange(true));
    }

    // 接收取消请求; 控制夹具统计 force, 并发夹具不拥有可取消的真实网络.
    void request_cancel(bool) override {}

    // 记录或忽略假传输的结束钩子, 不模拟外部 gRPC 生命周期.
    void finish_call(Status::Code) override {}

    // reading_ 原子交接唯一读缓冲指针, 空表示当前没有新提交的读取.
    std::atomic<proto::astra::v1::SessionPacket*> reading_{};
    // writing_ 初始 false, 从提交写到工作线程确认期间为 true.
    std::atomic_bool writing_{};
    // delivered_ 从零统计跨线程读完成数, 首包为 Hello, 后续为非零序号 Ping.
    std::atomic_uint delivered_{};
    // 最后声明使 worker 在其他派生成员和 Session 之前停止并 join.
    std::jthread worker_;
};

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
int main() {

    try {
        // identity 使用公开测试材料, 加载失败立即断言, 不读取用户部署凭证.
        auto identity = Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", *Endpoint::parse("127.0.0.1:7443"));
        CHECK(identity);
        // greeting 是复用的有效签名 Hello, 覆盖本场景所有会话对象的寿命.
        const auto greeting = hello(**identity);
        // config 为会话测试设置短心跳和 Pong 截止, 推进时间由用例显式给定.
        Config config;
        config.heartbeat_interval = Milliseconds(20);
        config.pong_timeout = Milliseconds(50);
        // now 为统一单调起点, 后续显式偏移精确检查截止边界.
        const auto now = Steady::now();
        Recorder policy;
        // start 借用 policy/identity/now/greeting, 为每个独立会话完成一致的双向握手前置步骤.
        const auto start = [&](Manual& session) {
            session.pump(policy, **identity, now);
            CHECK(session.writes == 0);
            session.acknowledge_metadata();
            // packet 为待交付的 Hello, 独立拥有后移入接收缓冲.
            proto::astra::v1::SessionPacket packet;
            packet.mutable_hello()->CopyFrom(*greeting);
            session.deliver(std::move(packet));
            session.pump(policy, **identity, now);
            CHECK(session.installed());
            CHECK(session.last_write.has_hello());
            session.acknowledge();
        };
        {
            Manual session(config, Policy::Direction::inbound, {1}, greeting, std::nullopt, [] {});
            session.cancel(Status::Code::timeout);
            session.cancel(Status::Code::identity);
            CHECK(session.error() == Status::Code::timeout);
            session.pump(policy, **identity, now);
            CHECK(session.done() && session.calls == 0 && session.reads == 0 && session.writes == 0 && session.finishes == 1);
            session.cancel();
            CHECK(session.error() == Status::Code::timeout);
        }
        {
            Manual session(config, Policy::Direction::inbound, {2}, greeting, std::nullopt, [] {});
            start(session);
            session.pump(policy, **identity, now + Milliseconds(20));
            CHECK(session.last_write.has_ping());
            // id 保存本次发出的 Ping 序号, 用于分别构造错配与匹配但迟到的 Pong.
            const auto id = session.last_write.ping().request_id();
            session.acknowledge();
            session.deliver(ping(id + 1, true));
            session.pump(policy, **identity, now + Milliseconds(60));
            CHECK(!session.done());
            // 正确 Pong 恰好落在原截止上仍超时, 不允许陈旧响应刷新期限.
            session.deliver(ping(id, true));
            session.pump(policy, **identity, now + Milliseconds(70));
            CHECK(session.done() && session.error() == Status::Code::timeout);
        }
        {
            Manual session(config, Policy::Direction::inbound, {3}, greeting, std::nullopt, [] {});
            start(session);
            // 停滞写期间保留读取, 过载必须按明确错误结束, 不能靠停止读取遮蔽 Pong.
            for (std::uint64_t id = 1; id <= 6; ++id) {
                session.deliver(ping(id));
                session.pump(policy, **identity, now + Milliseconds(1));
            }
            CHECK(session.error() == Status::Code::capacity && !session.done());
            // writes 固定过载发生时的发送计数, 取消后不能继续发送积压消息.
            const auto writes = session.writes;
            session.acknowledge();
            session.pump(policy, **identity, now + Milliseconds(2));
            CHECK(session.done() && session.writes == writes && session.finishes == 1);
        }
        {
            Manual session(config, Policy::Direction::inbound, {4}, greeting, std::nullopt, [] {});
            start(session);
            session.deliver(ping(1));
            session.pump(policy, **identity, now + Milliseconds(1));
            session.pump(policy, **identity, now + Milliseconds(51));
            CHECK(session.error() == Status::Code::timeout);
            session.pump(policy, **identity, Steady::now() + Milliseconds(300));
            CHECK(session.forced == 1);
            session.acknowledge();
            session.pump(policy, **identity, Steady::now() + Milliseconds(301));
            CHECK(session.done());
        }
        {
            // 响应头停滞时, 后入队的 Pong 仍按自己的较短截止到期, 不被 Hello 的期限遮盖.
            Manual session(config, Policy::Direction::inbound, {5}, greeting, std::nullopt, [] {});
            session.pump(policy, **identity, now);
            // packet 为待交付的 Hello, 独立拥有后移入接收缓冲.
            proto::astra::v1::SessionPacket packet;
            packet.mutable_hello()->CopyFrom(*greeting);
            session.deliver(std::move(packet));
            session.pump(policy, **identity, now);
            session.deliver(ping(1));
            session.pump(policy, **identity, now + Milliseconds(1));
            session.pump(policy, **identity, now + Milliseconds(51));
            CHECK(session.error() == Status::Code::timeout && session.writes == 0);
            session.acknowledge_metadata();
            session.pump(policy, **identity, now + Milliseconds(52));
            CHECK(session.done());
        }
        {
            // 拨号尚未完成时不投递 Hello. 到期后仍启动并排空已绑定的 client call.
            Manual session(config, Policy::Direction::outbound, {6}, greeting, std::nullopt, [] {});
            session.ready = false;
            session.pump(policy, **identity, now);
            CHECK(session.calls == 0 && session.writes == 0);
            session.pump(policy, **identity, Steady::now() + config.connect_timeout);
            CHECK(session.done() && session.error() == Status::Code::timeout && session.calls == 1 && session.writes == 0);
        }
        CHECK(policy.installations == 4);
        {
            // Finish 已投递也不等于 OnDone 已发生. 残留读取或最终状态停滞仍须有取消上限.
            Manual session(config, Policy::Direction::inbound, {7}, greeting, std::nullopt, [] {});
            session.finish_immediately = false;
            session.cancel();
            session.pump(policy, **identity, Steady::now());
            CHECK(session.finishes == 1 && !session.done());
            session.pump(policy, **identity, Steady::now() + Milliseconds(300));
            CHECK(session.forced == 1 && session.finishes == 1);
            session.complete();
            CHECK(session.done());
        }
        {
            Concurrent session(config, greeting);
            session.pump(policy, **identity, now);
            session.ready();
            // deadline 为并发测试的十秒防挂起预算, 完成依据仍是实际交付次数.
            const auto deadline = Steady::now() + std::chrono::seconds(10);
            while (session.delivered() < 50000 && Steady::now() < deadline) {
                session.pump(policy, **identity, now);
                CHECK(!session.error());
                std::this_thread::yield();
            }
            CHECK(session.delivered() >= 50000);
        }
        std::cout << "PASS pre-start cancellation, stale Pong deadline, bounded queue, stalled write and concurrent receive handoff\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
