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
// 一条对时流的异步处理器, 由 OnDone 唯一释放, 不在回调中等待网络.
class Reactor final : public grpc::ServerBidiReactor<proto::pulsar::v1::Ping, proto::pulsar::v1::Pong> {
    // Alarm 只持有独立状态, 不捕获 Reactor; OnDone 清空上下文后才释放处理器.
    struct Expiry {
        // 只协调 TryCancel 与上下文寿命的交接, 不保护消息或跨网络 I/O 持有.
        std::mutex mutex;
        // gRPC 拥有上下文, 仅在非空且持锁时允许定时器调用 TryCancel.
        grpc::CallbackServerContext* context{};
    };

public:
    // 构造尚不提交操作或占用配额; 服务 handler 返回前 gRPC 不调用 OnDone.
    Reactor(grpc::CallbackServerContext* context, std::counting_semaphore<64>& streams, const Source& clock) : context_(context), streams_(streams), clock_(clock) {}

    // 完成一次凭证验证再异步读首包. 所有失败都 Finish, 配额直到 OnDone 才归还.
    void start(const Authority& authority, const Ledger& ledger) {

        try {
            if (!streams_.try_acquire()) {
                Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Pulse capacity reached"));
                return;
            }
            admitted_ = true;
            // metadata 借用当前上下文的只读请求头, 先检查唯一性再按键取得凭证.
            const auto& metadata = context_->client_metadata();
            if (metadata.count("astra-admission-bin") != 1 || metadata.count("astra-signature-bin") != 1) {
                Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Pulse credential required"));
                return;
            }

            // admission 指向唯一的准入正文, 字节仅在本次验证期间借用.
            const auto admission = metadata.find("astra-admission-bin");
            // signature 指向唯一的准入签名, 不允许重复头产生解释歧义.
            const auto signature = metadata.find("astra-signature-bin");
            // value 为验签后拥有的成员身份, 还需核对 Star 角色和当前登记代次.
            const auto value = authority.identity().verify({reinterpret_cast<const std::uint8_t*>(admission->second.data()), admission->second.size()}, {reinterpret_cast<const std::uint8_t*>(signature->second.data()), signature->second.size()});
            if (!value || value->role != Member::Role::star || !ledger.current(*value)) {
                Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Pulse credential rejected"));
                return;
            }

            // 不读取/换算客户端的墙钟 deadline. 服务端自己强制最多三秒的单调流寿命.
            expiry_target_ = std::make_shared<Expiry>();
            expiry_target_->context = context_;
            expiry_.Set(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(3, GPR_TIMESPAN)), [target = expiry_target_](bool expired) {
                // lock 将目标指针检查和 TryCancel 作为一个寿命保护区间.
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
            // t1 是接收处理起点的公共时间与质量, 本地走时可用不等于可以向其他节点提供可信新样本.
            const auto t1 = clock_.now();
            if (!t1 || !t1->synchronized) {
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
            // t2 是填充完成后的公共时间, 与 t1 同域, 两者差覆盖本次处理耗时.
            const auto t2 = clock_.now();
            if (!t2 || !t2->synchronized || t2->time < t1->time || pong_.precision_ns() == 0) {
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
                // lock 与迟到 Alarm 回调互斥, 指针清空后才允许销毁上下文借用者.
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
    // 借用服务的 64 个流配额, 只在 admitted_ 为 true 时于 OnDone 归还.
    std::counting_semaphore<64>& streams_;
    // 服务负责先停止全部 RPC 再销毁时钟, 不借用线程私有观测.
    const Source& clock_;
    // 仅 handler 写入, OnDone 读取, 防止拒绝路径归还未取得的配额.
    bool admitted_{};
    // 只有 OnWriteDone 修改, 上限八个应答.
    unsigned responses_{};
    // 消息寿命覆盖对应 StartRead/StartWrite 到完成回调, 不保留消息队列.
    proto::pulsar::v1::Ping ping_;
    // 复用应答缓冲, OnWriteDone 之前保持不变, 下一次构造前 Clear.
    proto::pulsar::v1::Pong pong_;
    // 定时器与 Reactor 的寿命解耦, 已排队的 Alarm 回调在对象销毁后只见空上下文.
    std::shared_ptr<Expiry> expiry_target_;
    // 本流三秒单调寿命定时器, 与独立目标状态共同避免迟到回调访问已释放上下文.
    grpc::Alarm expiry_;
};
} // namespace

// Pulse 构造只保存引用, 不启动线程, 实际服务由 Server 装配.
// authority/ledger/clock 为签发材料、账本与时钟引用, 生命周期由 Server 保证.
Pulse::Pulse(const Authority& authority, const Ledger& ledger, const Source& clock) : clock_(clock), authority_(authority), ledger_(ledger) {}

// Pulse::Bounce 创建四时间戳对时反应器, 每个连接独立, 断开即销毁.
// context 为回调上下文; 返回反应器所有权, 由 gRPC 框架驱动.
grpc::ServerBidiReactor<proto::pulsar::v1::Ping, proto::pulsar::v1::Pong>* Pulse::Bounce(grpc::CallbackServerContext* context) {

    // reactor 由最终 OnDone 自释放, 服务 handler 只负责构造和启动, 不再持有所有权.
    auto* reactor = new Reactor(context, streams_, clock_);
    reactor->start(authority_, ledger_);
    return reactor;
}
} // namespace astra
