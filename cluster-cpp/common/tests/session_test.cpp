// 功能: 使用会话测试适配器验证 Hello, 心跳, 缓冲交接与取消生命周期.
#include "check.hpp"
#include "fixture.hpp"
#include "grpc_session.hpp"

#include <iostream>
#include <stdexcept>
#include <thread>

using namespace verdandi::cluster;

// 测试专用凭据使用仓库公开私钥, 不读取开发者部署身份.
std::shared_ptr<wire::Hello> hello(const Identity& identity) {
    wire::RegistrationResponse::Member member;
    member.set_cluster_id("alpha");
    member.set_group("default");
    member.set_id("00000001000040008000000000000000");
    member.set_advertise("127.0.0.1:7443");
    member.set_principal(identity.principal(member.cluster_id(), member.advertise()).text());
    member.set_epoch(1);
    member.set_role(wire::ROLE_STAR);
    auto result = std::make_shared<wire::Hello>();
    result->set_protocol_major(6);
    result->set_max_frame_bytes(4096);
    result->set_admission(member.SerializeAsString());
    result->set_admission_signature(test::sign(result->admission()));
    return result;
}

// 仅记录传输是否在认证之后安装身份, 角色规则由 core_test 独立覆盖.
struct RecordingPolicy final : Policy {
    unsigned installations{};
    Result<void> initialize(const Member&, std::span<const Member>) override {
        return {};
    }
    Result<std::vector<SessionGeneration>> accept(const Member&, Direction, SessionGeneration, const std::optional<Member>&) override {
        ++installations;
        return std::vector<SessionGeneration>{};
    }
    void closed(SessionGeneration, std::optional<ErrorCode>, Clock::time_point) override {}
    std::optional<Member> due(Clock::time_point) override {
        return {};
    }
    void failed(const Member&, ErrorCode, Clock::time_point) override {}
    bool needs_refresh(Clock::time_point) override {
        return false;
    }
    NetworkStatus status() const override {
        return {};
    }
};

// 人工控制回调顺序, 覆盖真实网络难以稳定复现的停滞写与截止竞争.
struct ControlledSession final : RpcSession {
    using RpcSession::RpcSession;
    wire::SessionPacket* receiving{};
    wire::SessionPacket last_write;
    unsigned reads{}, writes{}, finishes{}, calls{}, forced{};
    bool ready = true;
    bool finish_immediately = true;
    void complete() {
        completed(grpc::Status::OK);
    }
    bool transport_ready() const override {
        return ready;
    }
    void deliver(wire::SessionPacket packet) {
        CHECK(receiving);
        receiving->Swap(&packet);
        receiving = nullptr;
        read_done(true);
    }
    void acknowledge() {
        write_done(true);
    }
    void acknowledge_metadata() {
        metadata_done(true);
    }
    void begin_call() override {
        ++calls;
    }
    void begin_read(wire::SessionPacket* packet) override {
        CHECK(!receiving);
        receiving = packet;
        ++reads;
    }
    void begin_write(const wire::SessionPacket* packet) override {
        last_write = *packet;
        ++writes;
    }
    void request_cancel(bool force) override {
        forced += force;
    }
    void finish_call(ErrorCode) override {
        ++finishes;
        if (finish_immediately) {
            complete();
        }
    }
};

wire::SessionPacket ping(std::uint64_t id, bool response = false) {
    wire::SessionPacket result;
    if (response) {
        result.mutable_pong()->set_request_id(id);
    } else {
        result.mutable_ping()->set_request_id(id);
    }
    return result;
}

// 真正从另一线程交付读取完成, 检查每次重新提交的缓冲区都已由控制循环消费.
// 不在测试中修改 gRPC 私有状态, 不依赖一次 sleep 恰好命中竞争窗口.
class ConcurrentReadSession final : public RpcSession {
public:
    ConcurrentReadSession(const Config& config, std::shared_ptr<const wire::Hello> greeting)
        : RpcSession(config, Direction::inbound, {8}, greeting, std::nullopt, [] {}), worker_([this, greeting](std::stop_token stop) {
              while (!stop.stop_requested()) {
                  if (auto* packet = reading_.exchange(nullptr)) {
                      if (delivered_ == 0) {
                          packet->mutable_hello()->CopyFrom(*greeting);
                      } else {
                          packet->mutable_ping()->set_request_id(delivered_ + 1);
                      }
                      read_done(true);
                      ++delivered_;
                  }
                  if (writing_.exchange(false)) {
                      write_done(true);
                  }
                  std::this_thread::yield();
              }
          }) {}
    void ready() {
        metadata_done(true);
    }
    unsigned delivered() const {
        return delivered_.load();
    }

private:
    void begin_call() override {}
    void begin_read(wire::SessionPacket* packet) override {
        CHECK(packet->ByteSizeLong() == 0);
        CHECK(reading_.exchange(packet) == nullptr);
    }
    void begin_write(const wire::SessionPacket*) override {
        CHECK(!writing_.exchange(true));
    }
    void request_cancel(bool) override {}
    void finish_call(ErrorCode) override {}
    std::atomic<wire::SessionPacket*> reading_{};
    std::atomic_bool writing_{};
    std::atomic_uint delivered_{};
    // 最后声明使 worker 在其他派生成员和 RpcSession 之前停止并 join.
    std::jthread worker_;
};

int main() {
    try {
        auto identity = Identity::load(std::filesystem::path(VERDANDI_FIXTURES) / "star-a", *Endpoint::parse("127.0.0.1:7443"));
        CHECK(identity);
        const auto greeting = hello(**identity);
        Config config;
        config.heartbeat_interval = Milliseconds(20);
        config.pong_timeout = Milliseconds(50);
        const auto now = Clock::now();
        RecordingPolicy policy;
        const auto start = [&](ControlledSession& session) {
            session.pump(policy, **identity, now);
            CHECK(session.writes == 0);
            session.acknowledge_metadata();
            wire::SessionPacket packet;
            packet.mutable_hello()->CopyFrom(*greeting);
            session.deliver(std::move(packet));
            session.pump(policy, **identity, now);
            CHECK(session.installed());
            CHECK(session.last_write.has_hello());
            session.acknowledge();
        };
        {
            ControlledSession session(config, Direction::inbound, {1}, greeting, std::nullopt, [] {});
            session.cancel(ErrorCode::timeout);
            session.cancel(ErrorCode::identity);
            CHECK(session.error() == ErrorCode::timeout);
            session.pump(policy, **identity, now);
            CHECK(session.done() && session.calls == 0 && session.reads == 0 && session.writes == 0 && session.finishes == 1);
            session.cancel();
            CHECK(session.error() == ErrorCode::timeout);
        }
        {
            ControlledSession session(config, Direction::inbound, {2}, greeting, std::nullopt, [] {});
            start(session);
            session.pump(policy, **identity, now + Milliseconds(20));
            CHECK(session.last_write.has_ping());
            const auto id = session.last_write.ping().request_id();
            session.acknowledge();
            session.deliver(ping(id + 1, true));
            session.pump(policy, **identity, now + Milliseconds(60));
            CHECK(!session.done());
            // 正确 Pong 恰好落在原截止上仍超时, 不允许陈旧响应刷新期限.
            session.deliver(ping(id, true));
            session.pump(policy, **identity, now + Milliseconds(70));
            CHECK(session.done() && session.error() == ErrorCode::timeout);
        }
        {
            ControlledSession session(config, Direction::inbound, {3}, greeting, std::nullopt, [] {});
            start(session);
            for (std::uint64_t id = 1; id <= 6; ++id) {
                session.deliver(ping(id));
                session.pump(policy, **identity, now + Milliseconds(1));
            }
            CHECK(session.error() == ErrorCode::capacity && !session.done());
            const auto writes = session.writes;
            session.acknowledge();
            session.pump(policy, **identity, now + Milliseconds(2));
            CHECK(session.done() && session.writes == writes && session.finishes == 1);
        }
        {
            ControlledSession session(config, Direction::inbound, {4}, greeting, std::nullopt, [] {});
            start(session);
            session.deliver(ping(1));
            session.pump(policy, **identity, now + Milliseconds(1));
            session.pump(policy, **identity, now + Milliseconds(51));
            CHECK(session.error() == ErrorCode::timeout);
            session.pump(policy, **identity, Clock::now() + Milliseconds(300));
            CHECK(session.forced == 1);
            session.acknowledge();
            session.pump(policy, **identity, Clock::now() + Milliseconds(301));
            CHECK(session.done());
        }
        {
            // 响应头停滞时, 后入队的 Pong 仍按自己的较短截止到期, 不被 Hello 的期限遮盖.
            ControlledSession session(config, Direction::inbound, {5}, greeting, std::nullopt, [] {});
            session.pump(policy, **identity, now);
            wire::SessionPacket packet;
            packet.mutable_hello()->CopyFrom(*greeting);
            session.deliver(std::move(packet));
            session.pump(policy, **identity, now);
            session.deliver(ping(1));
            session.pump(policy, **identity, now + Milliseconds(1));
            session.pump(policy, **identity, now + Milliseconds(51));
            CHECK(session.error() == ErrorCode::timeout && session.writes == 0);
            session.acknowledge_metadata();
            session.pump(policy, **identity, now + Milliseconds(52));
            CHECK(session.done());
        }
        {
            // 拨号尚未完成时不投递 Hello. 到期后仍启动并排空已绑定的 client call.
            ControlledSession session(config, Direction::outbound, {6}, greeting, std::nullopt, [] {});
            session.ready = false;
            session.pump(policy, **identity, now);
            CHECK(session.calls == 0 && session.writes == 0);
            session.pump(policy, **identity, Clock::now() + config.connect_timeout);
            CHECK(session.done() && session.error() == ErrorCode::timeout && session.calls == 1 && session.writes == 0);
        }
        CHECK(policy.installations == 4);
        {
            // Finish 已投递也不等于 OnDone 已发生. 残留读取或最终状态停滞仍须有取消上限.
            ControlledSession session(config, Direction::inbound, {7}, greeting, std::nullopt, [] {});
            session.finish_immediately = false;
            session.cancel();
            session.pump(policy, **identity, Clock::now());
            CHECK(session.finishes == 1 && !session.done());
            session.pump(policy, **identity, Clock::now() + Milliseconds(300));
            CHECK(session.forced == 1 && session.finishes == 1);
            session.complete();
            CHECK(session.done());
        }
        {
            ConcurrentReadSession session(config, greeting);
            session.pump(policy, **identity, now);
            session.ready();
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            while (session.delivered() < 50000 && Clock::now() < deadline) {
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
