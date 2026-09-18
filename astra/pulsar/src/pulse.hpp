// 功能: 提供独立资源池中的有界对时采样, 不执行 KDF 或登记写锁操作.
#pragma once
#include "authority.hpp"
#include "ledger.hpp"
#include "physical_clock.hpp"
#include "pulsar.grpc.pb.h"
#include <semaphore>

namespace astra {
class Pulse final : public proto::pulsar::v1::Pulse::CallbackService {
public:
    // 借用验签, 成员表和物理参考时钟, 三者均由进程持有至 RPC 全部退出.
    Pulse(const PulsarAuthority& authority, const MembershipLedger& ledger, const PhysicalClock& clock);
    // 返回由 OnDone 回收的 Reactor, 网络等待不占用线程. 服务端自行限制三秒单调寿命.
    grpc::ServerBidiReactor<proto::pulsar::v1::Ping, proto::pulsar::v1::Pong>* Bounce(grpc::CallbackServerContext* context) override;

private:
    // 公共时间由独立采样线程维护, 回调只读取短锁快照, 不运行系统校准查询.
    const PhysicalClock& clock_;
    // 只借用验签公钥, Pulse 不调用密码认证或签发接口.
    const PulsarAuthority& authority_;
    // current() 只读原子快照, 不获取登记的写锁.
    const MembershipLedger& ledger_;
    // 至多 64 条正在采样的流, 不让慢客户端形成无界活动集合.
    std::counting_semaphore<64> streams_{64};
};
} // namespace astra
