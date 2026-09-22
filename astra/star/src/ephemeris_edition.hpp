#pragma once
#include "comet.pb.h"
#include "ephemeris_state.hpp"
#include <variant>

namespace astra {
// Ephemeris 专属公开分页, 首次完整 Record 含 Attr/Data, 后续已证明有 Attr 的变化才发 Data-only.
class Ephemeris::Edition {
public:
    using Reply = proto::comet::v1::EphemerisWatchReply; // 固定的下行消息, 不混入其他域.

    static bool target(std::string_view value) noexcept {
        return Ephemeris::valid(value);
    } // 非空目标必须是规范 UUID.

    // 冻结整个 Scope 的公开根, 无期限/顺序/来源细节, 不重新复制 Map.
    explicit Edition(State::Projection::View view);
    // 精确点查只保留一条内容, 缺项生成空 reset, 不先构建整张快照.
    Edition(std::string uuid, State::Projection::Point point);
    // changes 是已确认连续覆盖到 version 的变化, 可按 Key 合并; target 非空时只保留精确 UUID.
    Edition(std::uint64_t version, std::vector<State::Projection::Event> changes, std::string_view target = {});
    // 不复制一份正在发送的分页位置, 只转移唯一发送责任.
    Edition(Edition&&) noexcept = default;
    // 禁止隐式复制完整历史容器.
    Edition(const Edition&) = delete;
    // 每页目标 256 KiB/256 条, 单条完整注册允许独占大页, 硬编码上限 8 MiB.
    proto::comet::v1::EphemerisWatchReply next(std::string_view instance);
    // 已经构造最终 complete 页, 不代表对应 gRPC Write 已确认.
    bool complete() const noexcept;
    // 冻结批次的完成游标, 仅由调用方在最后 Write 成功后推进.
    std::uint64_t version() const noexcept;
    // 保守冻结引用字节, 不等于共享根的新分配或进程 RSS.
    std::size_t bytes() const noexcept;
    // 合并同 UUID 的未发送更新时保留 Attr 基线依据, 不能将 Create+Data 错合为仅 Data.
    static State::Projection::Event merge(const State::Projection::Event& previous, State::Projection::Event current) noexcept;

private:
    // 点查与请求 UUID 一起冻结, record 空为缺失; UUID 不进行二进制/字符串来回转换.
    struct Point {
        std::string uuid;                     // 一次请求目标的拥有式文本.
        std::optional<State::Content> record; // 捕获时完整公开内容, 不从可变状态补 Attr.
    };

    // 只编码本次固定记录, soft budget 不接受该项时返回 false 且不消费; hard 超限抛 length_error.
    static bool append(proto::comet::v1::EphemerisWatchReply& page, std::size_t& bytes, std::string_view uuid, const State::Content* record, bool data);
    std::variant<State::Projection::View, Point, std::vector<State::Projection::Event>> data_; // 唯一固定数据来源.
    proto::comet::v1::Mode mode_ = proto::comet::v1::MODE_RESET;                               // 全批固定, 不在页间切换.
    std::uint64_t version_{};                                                                  // 与根/完整后缀同边界固定的 Scope 游标.
    std::size_t bytes_{};                                                                      // 本批保守引用计费, 不随 next 页减少.
    std::size_t offset_{};                                                                     // 已成功构造的行数, 异常保持原值.
    bool complete_{};                                                                          // 完成页已构造后不允许再次 next.
};
} // namespace astra
