#pragma once
#include "astra.pb.h"
#include "catalog.hpp"
#include "ephemeris.hpp"
#include "origin.hpp"

namespace astra {
// 原生来源与类型化线协议的有限边界. 不读 Clock、不更改来源位置、不实现第二套数据存储.
class Parcel {
public:
    // 验证 UTF-8 地址并持有原文本; 内部复制仍允许 __ 范围, Comet 的访问边界另行限制.
    static std::optional<Scope> scope(const proto::astra::v1::Scope& message);
    // 编码已经验证的二元地址, 不拼接路径或重新规范化名称.
    static void scope(proto::astra::v1::Scope& message, const Scope& value);
    // 有限非负 Unix 纳秒转换, 不将溢出或零截止解成无限 TTL.
    static std::optional<Clock::Time> time(std::uint64_t value) noexcept;
    // 编码/解码原生 Catalog 完整正文或明确的正版本水位, 零字节正文保留存在性.
    static void record(proto::astra::v1::CatalogRecord& message, const Catalog::Record& value);
    static std::optional<Catalog::Record> record(const proto::astra::v1::CatalogRecord& message);
    // 编码/解码 Ephemeris 原生完整记录, 固定 Attr/TTL 和两个独立 order 原样保留.
    static void record(proto::astra::v1::EphemerisRecord& message, const Ephemeris::Record& value);
    static std::optional<Ephemeris::Record> record(const proto::astra::v1::EphemerisRecord& message);
    // 来源事件按原生 form 编码, 续租/Data-only 不重发其他正文; 无效内部组合抛逻辑错误.
    static void delta(proto::astra::v1::CatalogDelta& message, const Origin<Catalog::Record>::Event& event);
    static void delta(proto::astra::v1::EphemerisDelta& message, const Origin<Ephemeris::Record, true>::Event& event);
    // 只检查完整单包结构及硬预算, 不比较当前连续位置或替代来源身份检查.
    static bool valid(const proto::astra::v1::CatalogChanges& message) noexcept;
    static bool valid(const proto::astra::v1::EphemerisChanges& message) noexcept;

private:
    // 只检查字段, 不构造副本或读取本机时间; 单个 Buffer 最大 1 MiB.
    static bool valid(const proto::astra::v1::CatalogRecord& message) noexcept;
    static bool valid(const proto::astra::v1::EphemerisRecord& message) noexcept;
    static bool valid(const proto::astra::v1::Scope& message) noexcept;
    // 字节按原长度解码, 允许 NUL 及空值; 输入大小在调用前已经受限.
    static Catalog::Value value(const std::string& bytes);
};
} // namespace astra
