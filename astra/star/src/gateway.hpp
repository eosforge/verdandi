#pragma once
#include "access.hpp"
#include "comet.grpc.pb.h"
#include <atomic>
#include <semaphore>

namespace astra {
// 公共服务的逻辑登录入口. 仅注册到独立业务 Server, 不暴露节点准入、时钟或内部写入 RPC.
class Gateway final : public proto::comet::v1::Gateway::CallbackService {
public:
    // access 必须活过本服务与其全部 OnDone, instance 为固定实际 Star ID.
    // auth 默认开启, maximum 为 1..65536 条含认证中的流, 默认 4096; 构造不开放业务.
    Gateway(Access& access, std::string instance, bool auth = true, std::ptrdiff_t maximum = 4096);
    // 必须先 stop 并排空 gRPC Server, 不通过析构等待任意流或访问已释放上下文.
    ~Gateway() override = default;
    // 本地完整启动门满足后单向开放, stop 后不能重新打开; 不以 Pulsar 暂时失联关闭业务.
    void ready() noexcept;
    // 停止接纳并在锁外取消已拥有流; 实际容量直到 OnDone 才释放, 可以重复调用.
    void stop() noexcept;
    // 创建只发送一次确认的长期流; 之后只等待取消/撤销, 不使用同步 Read 或专用线程.
    grpc::ServerWriteReactor<proto::comet::v1::SessionReply>* Session(grpc::CallbackServerContext* context, const proto::comet::v1::SessionRequest* request) override;
    // 业务最终许可, auth=false 返回空守卫, 不自动开放内部 Scope; 错误同时写固定 trailing metadata.
    std::expected<std::optional<Access::Permit>, grpc::Status> enter(grpc::CallbackServerContext& context) const;
    // 当前固定实例 ID 的借用引用, 活过本 Gateway 才有效, 不从客户端请求覆盖.
    const std::string& instance() const noexcept;
    // 固定公共错误映射, 不添加远端正文、凭据或载荷, 单次尝试明确未提交.
    // version 仅在服务实际观察到该范围版本时携带, 不把未知位置写为零.
    grpc::Status error(grpc::CallbackServerContext& context, grpc::StatusCode code, proto::comet::v1::Reason reason, std::string_view message, std::optional<std::uint64_t> version = {}) const;

private:
    // 唯一拥有一条成功 Session RPC 的 reactor, 直到 OnDone 自行释放.
    class Stream;
    // 没有创建业务会话的即时拒绝流, 同样等待 OnDone 后释放.
    class Rejected;
    // 固定共享登录核心, 不在 Gateway 复制 SECRET 索引.
    Access& access_;
    // 本服务启动后不变的实例 ID, 只出现在公开确认/错误中.
    const std::string instance_;
    // 是否要求公共业务认证, 不影响内部身份或 __ 隔离.
    const bool auth_;
    // 包含正在认证、等待确认及取消排空的 Session RPC, 无空闲回收.
    std::counting_semaphore<65536> slots_;
    // 首轮数据及时间就绪之后才置 true; 关闭后只读取停止源, 不允许复活.
    std::atomic_bool ready_{};
    // 服务级取消源, 只负责已有 RPC 的关闭, 不代替 Access 的最终提交许可.
    std::stop_source stop_;
};
} // namespace astra
