// 功能: 提供推送链路测量程序, 记录吞吐, 延迟和运行状态以支持性能验证.
// 独立推流夹具: 复用已有 probe.proto 和 Rust 接收器, 不给生产 SessionPacket 增加业务字段.
#include "fixture.hpp"
#include "probe.grpc.pb.h"

#include <array>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace verdandi::cluster;
namespace probe = verdandi::probe;
using Time = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
struct ProbeConfig {
    // registries: 初始 Registry 条目数, 默认 1000, 范围 1..100000, 每条载荷固定 128 字节.
    // catalogs: 初始 Catalog 条目数, 默认 1000, 范围 1..10000.
    // bytes: Catalog 单条载荷字节数, 默认 256, 范围 64..1024.
    // fanout: 开始生产前等待的订阅会话数, 默认 1, 范围 1..64.
    // rate: 发布源总更新速率, 默认 2000 条/s, 范围 1..200000, 不按订阅者重复计数.
    // hot: 更新限制的热点键数, 默认 0 使用全量键; 非零不能超过 min(registries, catalogs).
    std::uint64_t registries = 1000, catalogs = 1000, bytes = 256, fanout = 1, rate = 2000, hot = 0;
    // 测量生产时长, 内部以秒保存, 默认 3 s; --milliseconds 输入范围 200..30000 ms.
    double seconds = 3;
    // burst: 默认 false, 约 1 ms 调度; true 改成约 50 ms 批量赶进度, 总目标速率保持 rate.
    // fresh: 默认 false 复用发送消息, true 在写完成后释放槽内消息, 用于比较分配开销.
    bool burst = false, fresh = false;
    // mode: 默认 mixed, 可选 registry/catalog/mixed; mixed 更新按 1:3 分配并穿插快照片段.
    // address: TLS 监听地址, 默认 127.0.0.1:0 使用系统端口; 由 gRPC 绑定时校验地址.
    std::string mode = "mixed", address = "127.0.0.1:0";
};
struct Queued {
    probe::Event event;
    Time::time_point created;
};
struct Stamp {
    std::uint64_t revision{};
    Time::time_point scheduled;
};
class Session;

// 发布源持有完整预置表, 512 条共享广播环和 16384 个稀疏延迟槽. 这些预算与 Rust 夹具相同.
struct Hub {
    ProbeConfig config;
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<Session*> sessions;
    std::unordered_map<std::string, std::pair<std::uint64_t, std::string>> values;
    std::array<std::shared_ptr<const Queued>, 512> ring{};
    std::array<Stamp, 16384> stamps{};
    std::uint64_t sequence = 0, revision = 0;
    std::size_t joined = 0;
    bool finished = false, stop = false;
    std::jthread producer;

    explicit Hub(ProbeConfig input) : config(std::move(input)) {
        values.reserve(config.registries + config.catalogs);
        for (const auto domain : {probe::DOMAIN_REGISTRY, probe::DOMAIN_CATALOG}) {
            const auto count = domain == probe::DOMAIN_REGISTRY ? config.registries : config.catalogs;
            const auto size = domain == probe::DOMAIN_REGISTRY ? 128 : config.bytes;
            for (std::uint64_t i = 0; i < count; ++i) {
                auto data = std::string(size, '1');
                for (unsigned byte = 0; byte < 8; ++byte) {
                    data[byte] = static_cast<char>((i >> (byte * 8)) & 255);
                }
                values.emplace(key(domain, i, false), std::pair{0, std::move(data)});
            }
        }
    }
    std::string key(probe::Domain domain, std::uint64_t index, bool update = true) const {
        const auto count = domain == probe::DOMAIN_REGISTRY ? config.registries : config.catalogs;
        index %= update && config.hot ? config.hot : count;
        auto number = std::to_string(index);
        number.insert(0, 6 - number.size(), '0');
        return (domain == probe::DOMAIN_REGISTRY ? "registry/service/instance/" : "catalog/config/") + number;
    }
    void publish(probe::Event event);
    bool apply(probe::Domain domain, std::string data, Time::time_point scheduled);
    void produce();
    void shutdown();
};

// Reactor 由 gRPC 拥有终止时机, OnDone 在 Hub 锁内摘除后释放. 发布线程只在同一锁内访问裸指针.
class Session final : public grpc::ServerBidiReactor<probe::Control, probe::Event> {
public:
    Session(Hub& hub, grpc::CallbackServerContext* context) : hub_(hub), context_(context), latency_(100001) {
        hub_.sessions.push_back(this);
        StartSendInitialMetadata();
        read();
    }
    void OnSendInitialMetadataDone(bool ok) override {
        guarded([&] {
            metadata_ = true;
            if (!ok)
                fail("response metadata failed");
            pump();
        });
    }
    void OnReadDone(bool ok) override {
        guarded([&] {
            reading_ = false;
            if (!ok) {
                fail("control stream closed");
                return;
            }
            // control 内的广播也会 pump 当前会话, 此时必须继续独占接收对象, 直到 Clear 后才能再提交读取.
            processing_read_ = true;
            control();
            input_.Clear();
            processing_read_ = false;
            read();
            pump();
        });
    }
    void OnWriteDone(bool ok) override {
        guarded([&] {
            writing_ = false;
            const bool completed = output_[head_]->has_completed();
            if (hub_.config.fresh)
                output_[head_].reset();
            head_ = (head_ + 1) % output_.size();
            --queued_;
            if (!ok)
                fail("event write failed");
            if (completed && ok)
                closing_ = true;
            pump();
            read();
        });
    }
    void OnCancel() override {
        guarded([&] { fail("session cancelled"); });
    }
    void OnDone() override {
        {
            std::lock_guard lock(hub_.mutex);
            std::erase(hub_.sessions, this);
        }
        delete this;
    }

    // 一个在途写加 16 条等待消息. fresh 每次创建消息, reuse 仅复用各槽已拥有的普通 Protobuf 对象.
    void pump() {
        if (finishing_)
            return;
        if (closing_) {
            if (!writing_ && metadata_) {
                finishing_ = true;
                Finish(status_);
            }
            return;
        }
        while (queued_ < output_.size()) {
            std::shared_ptr<const Queued> next;
            if (!controls_.empty()) {
                next = std::move(controls_.front());
                controls_.pop_front();
            } else if (joined_ && cursor_ <= hub_.sequence) {
                if (hub_.sequence - cursor_ >= hub_.ring.size()) {
                    fail("receiver lagged; resynchronization required");
                    return;
                }
                next = hub_.ring[cursor_++ % hub_.ring.size()];
            } else
                break;
            auto& slot = output_[(head_ + queued_) % output_.size()];
            if (!slot)
                slot = std::make_unique<probe::Event>();
            slot->CopyFrom(next->event);
            slot->set_queue_micros(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Time::now() - next->created).count()));
            if (slot->has_drained())
                drained_ = slot->drained();
            ++queued_;
        }
        if (queued_ && !writing_ && metadata_) {
            writing_ = true;
            // 最多合并 16 条已经就绪的更新. 最后一条及控制应答立即刷新, 不等未来消息来解开缓冲.
            grpc::WriteOptions options;
            if (queued_ > 1 && output_[head_]->has_update() && buffered_writes_ < 15) {
                options.set_buffer_hint();
                ++buffered_writes_;
            } else {
                buffered_writes_ = 0;
            }
            StartWrite(output_[head_].get(), options);
        }
        read();
    }
    void fail(std::string_view reason) {
        if (!closing_) {
            closing_ = true;
            std::cerr << "push session ended: " << reason << '\n';
            status_ = grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, std::string(reason));
            context_->TryCancel();
        }
        pump();
    }

private:
    // 异常不能越过 gRPC 回调边界; 取消会驱动 OnCancel/OnDone, 释放由它们统一完成.
    template <class Action> void guarded(Action action) noexcept {
        try {
            std::lock_guard lock(hub_.mutex);
            action();
        } catch (...) {
            context_->TryCancel();
        }
    }
    void read() {
        if (!reading_ && !processing_read_ && !closing_ && controls_.size() < 8) {
            reading_ = true;
            StartRead(&input_);
        }
    }
    std::uint64_t percentile(double fraction) const {
        const auto target = static_cast<std::uint64_t>(std::ceil(static_cast<double>(samples_) * fraction));
        if (!target)
            return 0;
        std::uint64_t count = 0;
        for (std::size_t i = 0; i < latency_.size(); ++i) {
            count += latency_[i];
            if (count >= target)
                return (i + 1) * 100;
        }
        return 10000100;
    }
    void control() {
        if (!joined_) {
            if (!input_.has_join() || !input_.join() || hub_.joined >= hub_.config.fanout) {
                fail("join required or recipient limit");
                return;
            }
            joined_ = true;
            cursor_ = hub_.sequence + 1;
            if (++hub_.joined == hub_.config.fanout)
                hub_.ready.notify_one();
            return;
        }
        probe::Event reply;
        switch (input_.command_case()) {
        case probe::Control::kPing:
            reply.set_pong(input_.ping());
            break;
        case probe::Control::kPublish:
            if (!input_.publish().request() || input_.publish().data().size() > 1024) {
                fail("invalid publish");
                return;
            }
            if (hub_.apply(probe::DOMAIN_CATALOG, input_.publish().data(), Time::now()))
                reply.set_accepted(input_.publish().request());
            else
                reply.set_rejected(input_.publish().request());
            break;
        case probe::Control::kProgress: {
            const auto& progress = input_.progress();
            if (progress.final()) {
                if (!drained_ || *drained_ != progress.revision()) {
                    fail("invalid final progress");
                    return;
                }
                auto* result = reply.mutable_completed();
                result->set_revision(progress.revision());
                result->set_sampled_updates(samples_);
                result->set_expired_samples(expired_);
                result->set_update_p50_micros(percentile(0.50));
                result->set_update_p95_micros(percentile(0.95));
                result->set_update_p99_micros(percentile(0.99));
            } else {
                if (progress.revision() <= last_progress_ || progress.revision() > hub_.revision) {
                    fail("invalid progress revision");
                    return;
                }
                last_progress_ = progress.revision();
                const auto stamp = hub_.stamps[progress.revision() % hub_.stamps.size()];
                if (stamp.revision != progress.revision())
                    ++expired_;
                else {
                    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(Time::now() - stamp.scheduled).count();
                    ++latency_[static_cast<std::size_t>(std::clamp<std::int64_t>(micros / 100, 0, 100000))];
                    ++samples_;
                }
                return;
            }
            break;
        }
        default:
            fail("invalid control");
            return;
        }
        controls_.push_back(std::make_shared<Queued>(Queued{std::move(reply), Time::now()}));
    }
    Hub& hub_;
    grpc::CallbackServerContext* context_;
    probe::Control input_;
    std::array<std::unique_ptr<probe::Event>, 17> output_;
    std::deque<std::shared_ptr<const Queued>> controls_;
    std::vector<std::uint64_t> latency_;
    std::uint64_t cursor_ = 0, last_progress_ = 0, samples_ = 0, expired_ = 0;
    std::optional<std::uint64_t> drained_;
    std::size_t head_ = 0, queued_ = 0, buffered_writes_ = 0;
    bool joined_ = false, reading_ = false, processing_read_ = false, writing_ = false, metadata_ = false, closing_ = false, finishing_ = false;
    grpc::Status status_;
};

void Hub::publish(probe::Event event) {
    ring[++sequence % ring.size()] = std::make_shared<Queued>(Queued{std::move(event), Time::now()});
    for (auto* session : sessions)
        session->pump();
}
bool Hub::apply(probe::Domain domain, std::string data, Time::time_point scheduled) {
    if (finished)
        return false;
    auto name = key(domain, ++revision);
    values.at(name) = {revision, data};
    stamps[revision % stamps.size()] = Stamp{revision, scheduled};
    probe::Event message;
    auto* update = message.mutable_update();
    update->set_revision(revision);
    update->set_key(std::move(name));
    update->set_data(std::move(data));
    update->set_domain(domain);
    publish(std::move(message));
    return true;
}
void Hub::produce() {
    try {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, 8s, [&] { return joined == config.fanout || stop; }) || stop)
            return;
        probe::Event first;
        first.set_ready(true);
        publish(std::move(first));
        const auto start = Time::now();
        const auto target = static_cast<std::uint64_t>(static_cast<double>(config.rate) * config.seconds);
        std::uint64_t count = 0, snapshots = 0;
        auto tick = start;
        while (count < target && !stop) {
            lock.unlock();
            std::this_thread::sleep_until(tick);
            lock.lock();
            tick = Time::now() + (config.burst ? 50ms : 1ms);
            const auto due =
                std::min(target, static_cast<std::uint64_t>(std::chrono::duration<double>(Time::now() - start).count() * static_cast<double>(config.rate)));
            while (count < due && !stop) {
                const auto domain = config.mode == "registry" || (config.mode == "mixed" && count % 4 == 0) ? probe::DOMAIN_REGISTRY : probe::DOMAIN_CATALOG;
                auto data = std::string(domain == probe::DOMAIN_REGISTRY ? 128 : config.bytes, 'Z');
                for (unsigned byte = 0; byte < 8; ++byte)
                    data[byte] = static_cast<char>((count >> (byte * 8)) & 255);
                const auto scheduled = start + std::chrono::duration_cast<Time::duration>(
                                                   std::chrono::duration<double>(static_cast<double>(count + 1) / static_cast<double>(config.rate)));
                apply(domain, std::move(data), scheduled);
                if (++count % 64 == 0) {
                    lock.unlock();
                    std::this_thread::yield();
                    lock.lock();
                }
            }
            if (config.mode == "mixed" &&
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Time::now() - start).count()) / 500 > snapshots) {
                ++snapshots;
                for (int i = 0; i < 4; ++i) {
                    probe::Event piece;
                    piece.set_snapshot(std::string(16384, '<'));
                    publish(std::move(piece));
                }
            }
        }
        finished = true;
        probe::Event drained;
        drained.set_drained(revision);
        publish(std::move(drained));
    } catch (const std::exception& error) {
        std::cerr << "Producer failed: " << error.what() << '\n';
        shutdown();
    }
}
void Hub::shutdown() {
    std::lock_guard lock(mutex);
    stop = true;
    ready.notify_one();
    for (auto* session : sessions)
        session->fail("server stopping");
}

class Service final : public probe::TransportProbe::CallbackService {
public:
    Service(Hub& hub, std::shared_ptr<Identity> identity) : hub_(hub), identity_(std::move(identity)) {
        wire::RegistrationResponse::Member member;
        member.set_cluster_id("alpha");
        member.set_id("probe/" + std::to_string(hub_.config.fanout));
        member.set_principal(identity_->principal("alpha", "127.0.0.1:39001").text());
        member.set_advertise("127.0.0.1:39001");
        member.set_epoch(1);
        member.set_role(wire::ROLE_STAR);
        member.set_group("default");
        wire::Hello hello;
        hello.set_protocol_major(5);
        hello.set_max_frame_bytes(32768);
        hello.set_admission(member.SerializeAsString());
        hello.set_admission_signature(test::sign(hello.admission()));
        proof_ = hello.SerializeAsString();
    }
    grpc::ServerBidiReactor<probe::Control, probe::Event>* Synchronize(grpc::CallbackServerContext* context) override {
        const auto entry = context->client_metadata().find("hello-bin");
        wire::Hello hello;
        if (entry == context->client_metadata().end() || entry->second.size() > 2048 ||
            !hello.ParseFromArray(entry->second.data(), static_cast<int>(entry->second.size())) || hello.protocol_major() != 5 ||
            hello.max_frame_bytes() != 32768 || !identity_->verify(hello.admission(), hello.admission_signature())) {
            auto* rejected = new Rejected;
            rejected->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "invalid test bearer"));
            return rejected;
        }
        context->AddInitialMetadata("hello-bin", proof_);
        std::lock_guard lock(hub_.mutex);
        return new Session(hub_, context);
    }

private:
    struct Rejected : grpc::ServerBidiReactor<probe::Control, probe::Event> {
        void OnDone() override {
            delete this;
        }
    };
    Hub& hub_;
    std::shared_ptr<Identity> identity_;
    std::string proof_;
};
std::atomic_bool interrupted{};
static_assert(std::atomic_bool::is_always_lock_free);
void interrupt(int) {
    interrupted.store(true, std::memory_order_relaxed);
}
} // namespace

int main(int argc, char** argv) {
    try {
        ProbeConfig config;
        std::map<std::string, std::string> options;
        for (int i = 1; i < argc; ++i) {
            std::string argument(argv[i]);
            const auto separator = argument.find('=');
            if (separator == std::string::npos || !options.emplace(argument.substr(0, separator), argument.substr(separator + 1)).second)
                throw std::runtime_error("Expected unique --name=value");
        }
        auto take = [&](const char* name, std::string fallback) {
            auto item = options.extract(name);
            return item.empty() ? fallback : std::move(item.mapped());
        };
        auto number = [&](const char* name, std::uint64_t fallback) {
            auto text = take(name, std::to_string(fallback));
            std::uint64_t value = 0;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
                throw std::runtime_error("Invalid numeric probe option");
            return value;
        };
        config.address = take("--address", config.address);
        config.mode = take("--data-mode", config.mode);
        config.registries = number("--registries", 1000);
        config.catalogs = number("--catalogs", 1000);
        config.bytes = number("--catalog-bytes", 256);
        config.fanout = number("--fanout", 1);
        config.rate = number("--rate", 2000);
        config.hot = number("--hot-keys", 0);
        config.seconds = static_cast<double>(number("--milliseconds", 3000)) / 1000.0;
        config.burst = number("--burst", 0) != 0;
        config.fresh = number("--fresh", 0) != 0;
        if (!options.empty() || !config.registries || config.registries > 100000 || !config.catalogs || config.catalogs > 10000 || config.bytes < 64 ||
            config.bytes > 1024 || !config.fanout || config.fanout > 64 || !config.rate || config.rate > 200000 ||
            config.hot > std::min(config.registries, config.catalogs) || config.seconds < 0.2 || config.seconds > 30 ||
            (config.mode != "mixed" && config.mode != "registry" && config.mode != "catalog"))
            throw std::runtime_error("Invalid probe limits");
        auto identity = Identity::load(std::filesystem::path(VERDANDI_FIXTURES) / "star-a", Endpoint::parse("127.0.0.1:39001").value()).value();
        Hub hub(config);
        Service service(hub, identity);
        grpc::ServerBuilder builder;
        builder.SetMaxReceiveMessageSize(32768);
        builder.SetMaxSendMessageSize(32768);
        builder.AddChannelArgument("grpc.http2.bdp_probe", 0);
        builder.AddChannelArgument("grpc.http2.stream_lookahead_bytes", 65535);
        builder.AddChannelArgument("grpc.max_concurrent_streams", 1);
        builder.AddChannelArgument("grpc.server_handshake_timeout_ms", 5000);
        builder.RegisterService(&service);
        int port = 0;
        builder.AddListeningPort(config.address, identity->server_credentials(), &port);
        auto server = builder.BuildAndStart();
        if (!server || !port)
            throw std::runtime_error("Cannot start C++ push fixture");
        std::signal(SIGTERM, interrupt);
        std::signal(SIGINT, interrupt);
        std::signal(SIGPIPE, SIG_IGN);
        hub.producer = std::jthread([&] { hub.produce(); });
        std::cout << "{\"ready\":true,\"address\":\"127.0.0.1:" << port << "\"}\n" << std::flush;
        const auto deadline = Time::now() + 60s;
        while (!interrupted.load(std::memory_order_relaxed) && Time::now() < deadline)
            std::this_thread::sleep_for(20ms);
        hub.shutdown();
        hub.producer.join();
        server->Shutdown(std::chrono::system_clock::now() + 5s);
        server->Wait();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Push fixture failed: " << error.what() << '\n';
        return 1;
    }
}
