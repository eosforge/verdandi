#pragma once
#include "access.hpp"
#include "library.hpp"
#include "polaris.pb.h"
#include <astra/types.hpp>

namespace astra {
// 一条已验签 Polaris 流的业务状态, 只由接收任务串行操作, 重连必须创建新 Receiver.
// Library 独立存活, 流断开只销毁半份候选和待发确认, 已安装数据不回滚.
class Receiver {
public:
    // output/access 必须覆盖接收者寿命, 同一 Library 必须始终配套同一个 Access; 不发网络请求.
    Receiver(Library& output, Access& access);
    // 接受握手后的单帧, 失败不接受后续帧; 返回固定错误分类, 分配失败可传播至 RPC 边界.
    Result<void> receive(const proto::polaris::v1::Packet& packet);
    // 提取下一条有界控制帧, maximum 为协商后的 1024..8 MiB 上限, 默认 256 KiB; 非法参数抛异常.
    // 清单分页优先, ACK 同 Scope 合并到最新值; 无消息返回空.
    std::optional<proto::polaris::v1::Packet> next(std::size_t maximum = 256 * 1024);
    // 本流收到首轮 ready 且确认本地完整安装全部清单时为 true; 不是网络存活保证.
    bool ready() const noexcept;

private:
    // 已通过流级失败闸门后的协议分派, 只由 receive 调用.
    Result<void> process(const proto::polaris::v1::Packet& packet);
    // 严格验证单 Key 修改, 全量不接受 Delete; 内部凭据在安装前解析完整正文.
    static bool valid(const Scope& scope, const proto::comet::v1::AlmanacChange& change, bool snapshot);
    // 把原生存储错误映射为传输分类, 不打印 Key/SECRET 或底稿正文.
    static Result<void> failure(Almanac::Error error);
    // 合并首轮计划元数据, 完整后固定不再扩张, 不能低于本地已安装版本.
    Result<void> plan(const proto::polaris::v1::Inventory& page);
    // 一次只准备一个分组, 每页绑定固定 Scope/版本, complete 后原子安装并排队 ACK.
    Result<void> snapshot(const proto::polaris::v1::Snapshot& page);
    // 先验证整包结构/连续性, 再按提交逐项应用, 失败不回滚已成功前缀.
    Result<void> updates(const proto::polaris::v1::Updates& page);
    // 只记实际安装位置, 不以收到页面或开始准备代替提交.
    void acknowledge(const Scope& scope, std::uint64_t version);
    // 跨流保留的 Almanac 路由, 只借用对象, 不借用 Map 节点.
    Library& output_;
    // 与内部凭据安装共享提交边界的登录表, 跨流保留, 不在此管理业务 RPC 寿命.
    Access& access_;
    // 首轮固定范围及最低位置, 最多 4096 个, 完整收到前不安装数据.
    std::map<Scope, std::uint64_t> plan_;
    // 同 Scope 尚未发送的最新累计 ACK, 最多与本地有效范围同量级.
    std::map<Scope, std::uint64_t> acknowledgements_;
    // 初始或 Probe 的完整位置副本, 与 live 版本分开, 防止分页中出现重复/遗漏.
    std::optional<std::vector<Library::Position>> inventory_;
    // 当前清单已经取出的元素数, 新清单归零.
    std::size_t position_{};
    // 唯一未发布快照候选, 初始空, 不允许跨流延续.
    std::optional<Library::Draft> draft_;
    // 仅 __auth/comet 快照构建解析后的索引, 与 draft_ 同时安装或丢弃.
    std::optional<Access::Draft> credentials_;
    // 首轮清单完整标志, 初始 false, 完成后拒绝再次声明 plan.
    bool planned_{};
    // 初始业务安装完成标志, 初始 false, 只由真实 ready 帧触发.
    bool ready_{};
    // 首次协议/容量失败后永久关闭本接收状态, 不允许继续消费同一流的后续页.
    bool failed_{};
};
} // namespace astra
