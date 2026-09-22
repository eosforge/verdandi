#pragma once
#include "comet.grpc.pb.h"
#include "table.hpp"
#include <comet/observer.hpp>
#include <unordered_set>

namespace comet::detail {
// Ephemeris 记录只携带完整 Attr/Data, 不包含服务端 TTL/来源/操作顺序.
struct Registration {
    static constexpr std::size_t overhead = 2 * (sizeof(std::vector<std::uint8_t>) + 32); // 两份独立不可变载荷对象.
    Value attr;                                                                           // 固定 Attr, 后续 data 分支直接共享本引用.
    Value data;                                                                           // 当前 Data, 合法空正文也持有实际 vector.

    std::size_t bytes() const noexcept {
        return attr->size() + data->size();
    } // 私有 Table 的完整注册计费.
};

// 一个完整发布点, 没有网络句柄、回调或 Client 所有权.
struct Registrations {
    Table<Registration> data; // 不可变分页字典, 跨快照共享未变化页.
    std::string instance;     // 对应这份完整数据的实际 Star ID.
    std::uint64_t version{};  // 此 Star 的 Ephemeris 合并视图游标, 零允许合法空底稿.
};

// Ephemeris 下行安装策略. 单个网络任务顺序调用, 当前根只在 complete 全部校验通过后替换.
class Selection {
public:
    using API = Observer;                                // 本投影的公共读取接口.
    using Reply = proto::comet::v1::EphemerisWatchReply; // 私有具体下行消息.
    using Service = proto::comet::v1::Ephemeris;         // 私有生成 Watch 服务.
    static bool valid(std::string_view uuid) noexcept;   // 只接受小写 RFC UUIDv4 文本, 不在热路径格式化.
    bool repair(Error::Code code) noexcept;              // 缺少 Attr 最多允许一次新快照回退, 其他错误立即失败.
    bool resume() const noexcept;                        // 回退期间省略恢复版本, 仍保留旧完整视图供应用读取.
    // scope/target 已由工厂作外部边界检查, bytes/records 为本地安装预算.
    // reserve 调整共享 Client 的保守持有计费, 缩小/归零必须成功, 空回调供独立核心使用.
    using Reserve = std::move_only_function<bool(std::size_t) noexcept>;
    Selection(Scope scope, std::string target, std::size_t bytes, std::size_t records, Reserve reserve = {});
    ~Selection(); // 归还网络受控计费, 应用额外保有旧 View 的实际寿命不受影响.
    // 新流丢弃未完整候选, 保留最后完整位置. expected 来自登录确认, 匿名首次可以为空.
    void begin(std::string expected = {});
    // 接收真实生成消息; 未 complete 返回空, complete 返回不可变 View; 错误保留旧根并拒绝继续本流.
    Result<std::optional<Observer::View>> accept(const proto::comet::v1::EphemerisWatchReply& page);
    // 状态只包装最近完整内容, 不读写网络, 不偷偷改变已经给应用的 View.
    Observer::View view(Observer::State state, std::optional<Error> error = {}) const;
    // 放弃未完成页面, 不取消网络或改变旧视图; 分配失败/断流时同样调用.
    void discard() noexcept;

private:
    // 统一失败路径使本流失效, 不把损坏页面当作可忽略的心跳.
    Result<std::optional<Observer::View>> fail(Error::Code code);
    const Scope scope_;                               // 终身固定, 对象换范围需要重新创建.
    const std::string target_;                        // 空为全分组, 非空只接受同名 Key.
    const std::size_t bytes_;                         // Key/值逻辑容量, 不与 gRPC 消息大小混用.
    const std::size_t records_;                       // 可安装的最多记录数, 初值来自 Observer::Options.
    Reserve reserve_;                                 // 只作共享预算调整, 不回调用户或取得 Reading 的锁.
    std::shared_ptr<const Registrations> current_;    // 最后完整发布, 无网络寿命依赖.
    std::optional<Table<Registration>::Draft> draft_; // 当前批次的私有写页, 未完成不对外读取.
    std::unordered_set<std::string> seen_;            // 本批跨页去重, 完成/失败立即清空实际桶与键.
    std::size_t seen_bytes_{};                        // 去重 Key 正文字节, 与本批元数据一起计费.
    std::string expected_;                            // 当前流确认的实际 Star, 首批完成后固定.
    proto::comet::v1::Mode mode_{};                   // 当前批次模式, 未开始时为 unspecified.
    bool first_ = true;                               // reset 只允许作为一条新流的第一批.
    bool repaired_{};                                 // 本对象已经消耗唯一缺 Attr 回退, 新基线成功不重置次数.
    bool fresh_{};                                    // 下一条流必须取得完整 reset, 不继续从损坏增量位置恢复.
    bool valid_{};                                    // begin 后为 true, 协议/容量失败后必须重新 begin.
};
} // namespace comet::detail
