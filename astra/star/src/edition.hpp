#pragma once
#include "almanac.hpp"
#include "comet.pb.h"
#include <variant>

namespace astra {
// 一批已冻结的 Almanac 下行投影. 分页不再次读取可变分组, 不给每条 Watch 复制完整 Map.
// 仅由一个发送任务访问, gRPC Write 归还页面后才构造下一页; 重连位置由最终 complete 决定.
class Edition {
public:
    // 固定完整根, mode=reset; View 的 O(1) 捕获在 Almanac 内完成, 不在此重新遍历计数.
    explicit Edition(Almanac::View view);
    // 固定精确 Key 的完整点查, 缺失值产生合法空 reset; key 必须由请求入口验证.
    Edition(std::string key, Almanac::Point point);
    // 固定连续历史的目标位置, 按 Key 合并为最终 action; target 非空时只投影精确 Key.
    explicit Edition(Almanac::Replay replay, std::string_view target = {});
    // 已确认权威清单中没有该 Scope 时使用零版本空 reset, 不创造持久范围.
    Edition();
    // 只转移冻结根/历史的拥有权, 不复制分页位置或大载荷.
    Edition(Edition&&) noexcept = default;
    // 禁止隐式复制一批正在发送的状态.
    Edition(const Edition&) = delete;
    // 构造最多 256 KiB 目标页或 256 条记录; 单条大记录允许独占页, 仍须符合 8 MiB 硬上限.
    // instance 为固定当前 Star ID, 只在最终页出现; 完成后再调抛 logic_error, 异常不消费本页位置.
    proto::comet::v1::AlmanacWatchReply next(std::string_view instance);
    // 已经构造最终完整页, 不代表对应 Write 已完成或客户端已安装.
    bool complete() const noexcept;
    // 本批固定完成位置, 只在最后一页写成功后由流推进游标.
    std::uint64_t version() const noexcept;
    // 固定快照/历史载荷的保守保有字节, 不等同于新分配或进程 RSS.
    std::size_t bytes() const noexcept;

private:
    // 精确 reset 只保持一个 Key 与不可变值, 缺失时 value 为空, 不抓取整张根.
    struct Point {
        // 请求的精确文本 Key, 不通过格式化拼接路径.
        std::string key;
        // 点查时已经固定的完整内容, 空表示目标当时不存在.
        Almanac::Value value;
    };

    // 给单页加入完整操作, 达到软目标时返回 false 且不消费该项; 单条硬超限抛 length_error.
    static bool append(proto::comet::v1::AlmanacWatchReply& page, std::size_t& bytes, std::string_view key, const Almanac::Value& value);
    // 空基线、完整根、精确点查和冻结历史互斥, 不在一批中切换状态来源.
    std::variant<std::monostate, Almanac::View, Point, std::vector<Almanac::Change>> data_;
    // mode 在整批中固定, 初始为空 reset.
    proto::comet::v1::Mode mode_ = proto::comet::v1::MODE_RESET;
    // 与固定数据来自同一次捕获的权威位置, 默认零.
    std::uint64_t version_{};
    // 已成功构造的项目数, 初始零, next 抛错时保持原值.
    std::size_t offset_{};
    // 冻结内容/键/必要历史引用的保守计费, 默认零.
    std::size_t bytes_{};
    // 完成页已产生标志, 初始 false.
    bool complete_{};
};
} // namespace astra
