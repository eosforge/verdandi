#pragma once
#include "ephemeris_feed.hpp"
#include "ephemeris_state.hpp"
#include "gateway.hpp"

namespace astra {
// Ephemeris 公共写接口, 复用逻辑 Session 和原生 State. 只挂独立 Comet 端口, 不接收对等来源事件.
class Ephemeris::Service final : public proto::comet::v1::Ephemeris::CallbackService {
public:
    // state/gateway 必须覆盖全部 RPC; maximum 是同步内存提交并发准备数, 1..65536, 默认 128.
    Service(State& state, Gateway& gateway, std::ptrdiff_t maximum = 128);
    // 实际 Runtime 注入共享循环唤醒器, 不由每条连接创建线程.
    Service(State& state, Gateway& gateway, std::function<void()> wake, std::ptrdiff_t maximum = 128);
    // 在 Server 排空以后析构, 没有每连接线程或后台任务需要额外停止.
    ~Service() override;
    // 启动后由同一控制线程调用, 推进到期与最多 32 条就绪 Watch; 故障结束旧流而不伪装追平.
    void pump(std::chrono::steady_clock::time_point now, std::size_t maximum = 32);
    // 父服务先停止 Gateway 写入接纳, 再 stop/pump 到 empty 并排空 Server.
    void stop() noexcept;
    // Watch 的全部 OnDone 已回收, unary 的最终清理由 Server::Wait 完成.
    bool empty() const;
    // 只读下行分发计数快照, 仅用于归因测量; 调用只取快照, 不推进发送或改变预算.
    Feed::Delivery delivery() const;
    // 复用本服务的固定状态和 Gateway, 首次 reset 与连续 apply 均按 complete 提交.
    grpc::ServerWriteReactor<proto::comet::v1::EphemerisWatchReply>* Watch(grpc::CallbackServerContext* context, const proto::comet::v1::WatchRequest* request) override;
    // 创建完整 Attr/Data, 请求可省略首次 instance, 成功回复实际实例/新 UUID/固定 TTL.
    grpc::ServerUnaryReactor* Create(grpc::CallbackServerContext* context, const proto::comet::v1::CreateRequest* request, proto::comet::v1::CreateReply* reply) override;
    // 在本机实例/UUID 内按独立正 order 更新 Data, 不延期或重发 Attr.
    grpc::ServerUnaryReactor* Update(grpc::CallbackServerContext* context, const proto::comet::v1::UpdateRequest* request, proto::comet::v1::UpdateReply* reply) override;
    // 独立正续租 order, 响应不回传客户端已有的 UUID/TTL.
    grpc::ServerUnaryReactor* Renew(grpc::CallbackServerContext* context, const proto::comet::v1::RenewRequest* request, proto::comet::v1::RenewReply* reply) override;
    // 明确权威结束, 缺项返回 ended, 不保留永久注销记录.
    grpc::ServerUnaryReactor* Remove(grpc::CallbackServerContext* context, const proto::comet::v1::RemoveRequest* request, proto::comet::v1::Empty* reply) override;

private:
    // 只覆盖有界同步准备/提交区, 不在等待长期网络时持有槽位.
    struct Slot {
        std::counting_semaphore<65536>& slots; // 原服务并发额度, 外层已成功 acquire.

        ~Slot() {
            slots.release();
        } // 正常/异常返回均归还同步槽位.
    };

    // 标准 unary 由 gRPC 持有 reactor. 所有异常返回效果未知, 不能在提交后失败时谎报 unapplied.
    // work 必须在真实提交前完成响应缓冲分配; Finish 在许可释放后调用, 不把网络等待带入 Access 锁.
    template <typename F>
    grpc::ServerUnaryReactor* execute(grpc::CallbackServerContext& context, F&& work) {

        auto* reactor = context.DefaultReactor(); // 生命周期由 gRPC 管理, 不手动 delete.
        grpc::Status status;
        try {
            if (!slots_.try_acquire()) {
                status = gateway_.error(context, grpc::StatusCode::RESOURCE_EXHAUSTED, proto::comet::v1::REASON_BUSY, "Ephemeris admission is busy");
            } else {
                const Slot slot{slots_};
                status = work(); // work 自行在最终边界取得并持有 Access::Permit.
            }
        } catch (const std::bad_alloc&) {
            status = grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Ephemeris preparation failed");
        } catch (...) {
            status = grpc::Status(grpc::StatusCode::INTERNAL, "Ephemeris processing failed");
        }
        reactor->Finish(status);
        return reactor;
    }

    // 最终登录许可与输入/实例校验, 已明确拒绝的单次尝试没有提交.
    std::expected<std::optional<Access::Permit>, grpc::Status> enter(grpc::CallbackServerContext& context, std::string_view instance, const proto::comet::v1::Scope& scope, bool initial) const;
    // 任何 Scope 字符串复制前先验证静态边界, 防止超长请求在拒绝前放大内存.
    grpc::Status address(grpc::CallbackServerContext& context, const proto::comet::v1::Scope& scope) const;
    // 原生错误映射为稳定公共 reason/effect, 不泄漏远端文本或请求载荷.
    grpc::Status error(grpc::CallbackServerContext& context, State::Error error) const;
    // TTL 静态拒绝附合法范围, 客户端不需要动态 Inspect/Limits 握手.
    grpc::Status ttl(grpc::CallbackServerContext& context) const;
    // 单 Buffer 至多 1 MiB; 校验后仅复制一次到不可变原生载荷, 不保留 Proto 消息为业务存储.
    static Value copy(const std::string& value);
    State& state_;                         // 实际自有来源和投影提交器, 不借用冻结 SDK.
    Gateway& gateway_;                     // 当前实例与长流登录身份, 不重复比较 SECRET.
    std::counting_semaphore<65536> slots_; // 同时准备的 unary 数量, 不声称是所有网络内存的硬上限.
    std::unique_ptr<Feed> feed_;           // 活动推流收集器, 析构前须清理全部网络回调.
};
} // namespace astra
