#pragma once
#include "authority.hpp"
#include "ledger.hpp"
#include "physical_clock.hpp"
#include "pulsar.grpc.pb.h"
#include <semaphore>

namespace astra {
class Pulse final : public proto::pulsar::v1::Pulse::CallbackService {
public:
    // 借用签发者,账本和时钟, 不启动采样线程; 三者均须覆盖服务及全部 Reactor 的寿命.
    Pulse(const Authority& authority, const Ledger& ledger, const Source& clock);
    // 返回由 OnDone 回收的 Reactor, 网络等待不占用线程. 服务端自行限制三秒单调寿命.
    grpc::ServerBidiReactor<proto::pulsar::v1::Ping, proto::pulsar::v1::Pong>* Bounce(grpc::CallbackServerContext* context) override;

private:
    // 公共时间由独立采样线程维护, 回调只读取短锁快照, 不运行系统校准查询.
    const Source& clock_;
    // 只借用验签公钥, Pulse 不调用密码认证或签发接口.
    const Authority& authority_;
    // current() 只读原子快照, 不获取登记的写锁.
    const Ledger& ledger_;
    // 至多 64 条正在采样的流, 不让慢客户端形成无界活动集合.
    std::counting_semaphore<64> streams_{64};
};
} // namespace astra
