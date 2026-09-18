#include "pulse.hpp"
#include <algorithm>
#include <astra/clock.hpp>
#include <grpc/support/time.h>
#include <grpcpp/alarm.h>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace astra {
namespace {
// Alarm 回调只借用这个独立状态, 不捕获 Reactor. OnDone 先清空指针再释放 RPC 对象.
struct ExpiryTarget {
    // 只协调 TryCancel 与上下文寿命的交接, 不保护消息或跨网络 I/O 持有.
    std::mutex mutex;
    // gRPC 拥有上下文, 仅在非空且持锁时允许由定时器调用 TryCancel.
    grpc::CallbackServerContext* context{};
};

class PulseReactor final : public grpc::ServerBidiReactor<proto::pulsar::v1::Ping, proto::pulsar::v1::Pong> {
public:
    // 构造尚不提交操作或占用配额; 服务 handler 返回前 gRPC 不调用 OnDone.
    PulseReactor(grpc::CallbackServerContext* context, std::counting_semaphore<64>& streams, const PhysicalClock& clock)
        : context_(context), streams_(streams), clock_(clock) {}

    // 完成一次凭证验证再异步读首包. 所有失败都 Finish, 配额直到 OnDone 才归还.
    void start(const PulsarAuthority& authority, const MembershipLedger& ledger) {
        try {
            if (!streams_.try_acquire()) {
                Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Pulse capacity reached"));
                return;
            }
            admitted_ = true;
            const auto& metadata = context_->client_metadata();
            if (metadata.count("astra-admission-bin") != 1 || metadata.count("astra-signature-bin") != 1) {
                Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Pulse credential required"));
                return;
            }
            const auto admission = metadata.find("astra-admission-bin");
            const auto signature = metadata.find("astra-signature-bin");
            const auto value = authority.identity().verify({reinterpret_cast<const std::uint8_t*>(admission->second.data()), admission->second.size()},
                                                           {reinterpret_cast<const std::uint8_t*>(signature->second.data()), signature->second.size()});
            if (!value || value->role != Member::Role::star || !ledger.current(*value)) {
                Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Pulse credential rejected"));
                return;
            }
            // 不读取/换算客户端的墙钟 deadline. 服务端自己强制最多三秒的单调流寿命.
            expiry_target_ = std::make_shared<ExpiryTarget>();
            expiry_target_->context = context_;
            expiry_.Set(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(3, GPR_TIMESPAN)), [target = expiry_target_](bool expired) {
                std::lock_guard lock(target->mutex);
                if (expired && target->context) {
                    target->context->TryCancel();
                }
            });
            StartRead(&ping_);
        } catch (...) {
            Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Pulse unavailable"));
        }
    }

private:
    // 每次只有一个 Read 或 Write 在途. 回调只计算标量和提交下一操作, 不等待网络.
    void OnReadDone(bool ok) override {
        if (!ok) {
            Finish(context_->IsCancelled() ? grpc::Status(grpc::StatusCode::CANCELLED, "Pulse cancelled") : grpc::Status::OK);
            return;
        }
        try {
            const auto t1 = clock_.now();
            if (!t1 || !t1->ready) {
                Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "Physical time not synchronized"));
                return;
            }
            if (ping_.t0() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid Pulse timestamp"));
                return;
            }
            // 上次写完成后才能复用, 清空未来可选字段; 清空/填充耗时由 T2-T1 覆盖.
            pong_.Clear();
            pong_.set_t0(ping_.t0());
            pong_.set_t1(static_cast<std::uint64_t>(t1->time.time_since_epoch().count()));
            pong_.set_precision_ns(clock_.precision());
            const auto t2 = clock_.now();
            if (!t2 || !t2->ready || t2->time < t1->time || pong_.precision_ns() == 0) {
                Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "Physical time not synchronized"));
                return;
            }
            pong_.set_t2(static_cast<std::uint64_t>(t2->time.time_since_epoch().count()));
            pong_.set_uncertainty_ns(std::max(t1->uncertainty_ns, t2->uncertainty_ns));
            pong_.set_synchronized(true);
            StartWrite(&pong_);
        } catch (...) {
            Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "Physical time unavailable"));
        }
    }

    // 写完成才允许复用 Pong 或开始下一次 Read; 因而 Finish 从不与写操作重叠.
    void OnWriteDone(bool ok) override {
        if (!ok) {
            Finish(grpc::Status(grpc::StatusCode::CANCELLED, "Pulse write ended"));
        } else if (++responses_ == 8) {
            Finish(grpc::Status::OK);
        } else {
            StartRead(&ping_);
        }
    }

    // 取消使唯一在途操作以 ok=false 完成, 由该回调 Finish. 不在 OnCancel 竞争重复 Finish.
    // gRPC 保证 OnDone 在其余所有 reactions 返回后执行, 此时消息和配额才能回收.
    void OnDone() override {
        if (expiry_target_) {
            {
                std::lock_guard lock(expiry_target_->mutex);
                expiry_target_->context = nullptr;
            }
            expiry_.Cancel();
        }
        if (admitted_) {
            streams_.release();
        }
        delete this;
    }

    // 上下文与配额所有者由 gRPC/服务持有至 OnDone.
    grpc::CallbackServerContext* context_;
    std::counting_semaphore<64>& streams_;
    // 服务负责先停止全部 RPC 再销毁时钟, 不借用线程私有观测.
    const PhysicalClock& clock_;
    // 仅 handler 写入, OnDone 读取, 防止拒绝路径归还未取得的配额.
    bool admitted_{};
    // 只有 OnWriteDone 修改, 上限八个应答.
    unsigned responses_{};
    // 消息寿命覆盖对应 StartRead/StartWrite 到完成回调, 不保留消息队列.
    proto::pulsar::v1::Ping ping_;
    proto::pulsar::v1::Pong pong_;
    // 定时器与 Reactor 的寿命解耦, 已排队的 Alarm 回调在对象销毁后只见空上下文.
    std::shared_ptr<ExpiryTarget> expiry_target_;
    grpc::Alarm expiry_;
};
} // namespace

Pulse::Pulse(const PulsarAuthority& authority, const MembershipLedger& ledger, const PhysicalClock& clock)
    : clock_(clock), authority_(authority), ledger_(ledger) {}

grpc::ServerBidiReactor<proto::pulsar::v1::Ping, proto::pulsar::v1::Pong>* Pulse::Bounce(grpc::CallbackServerContext* context) {
    auto* reactor = new PulseReactor(context, streams_, clock_);
    reactor->start(authority_, ledger_);
    return reactor;
}
} // namespace astra
