#pragma once
#include <astra/clock.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

namespace astra {
// Catalog 原生记录与合并规则. 业务版本按 Key 独立, 来源组位置由外层提交器统一覆盖全部 Scope.
// 此层只准备候选, 不读网络、不分配版本、不修改旧记录; 索引/历史/轮/下游投影须由外层原子发布.
class Catalog {
public:
    class State;   // 自有来源、公开投影和本地 TTL 的提交边界.
    class Edition; // 已冻结公开批次的有界分页编码.
    class Feed;    // 公共 Watch 生命周期与背压.
    class Service; // 独立业务入口的 Publish/Renew/Watch.
    // 完整业务字节, 不解释内部编码; 空 Buffer 是合法的存在值.
    using Buffer = std::vector<std::uint8_t>;
    // 不可变载荷所有权, 空指针仅用于水位节点, 不代表零字节内容.
    using Value = std::shared_ptr<const Buffer>;

    // 候选失败保持旧状态, RPC 层分别映射版本冲突、缺失、计时及输入错误.
    enum class Error {
        // 非法版本/TTL/载荷或不一致的原生记录.
        input,
        // 没有可用本地业务时间, 单纯参考源失联不触发.
        clock,
        // 本机来源没有尚在期限内的完整正文, 不能仅凭 Renew 接管远端来源.
        ended,
        // 请求低于已知最高业务版本, 或 Renew 不等于本机来源版本.
        version,
        // 同一业务版本被声明为不同的完整正文.
        conflict,
        // 绝对截止相加溢出, 不改为无穷期.
        exhausted
    };

    // 一条来源事实或本地合并水位, 不包含来源/Key/Scope 字符串和复制位置.
    struct Record {
        // 已知的正内容版本, 清理载荷后仍永久保留在相应水位中.
        std::uint64_t version{};
        // 活动内容的共享引用; 为空时 deadline 也必须为空.
        Value value;
        // 与 value 成对的有限 Unix 截止, 水位无期限且不占活动时间轮节点.
        std::optional<Clock::Time> deadline;
    };

    // 合并候选不等于来源事实, 不能将其他来源的较高版本写回本机来源表.
    struct Change {
        // 新合并水位及其可见候选, 只与当前合并记录交换, 不直接广播.
        Record record;
        // true 才修改本地内容投影/游标; 只提高不可见水位或延长截止不唤醒订阅.
        bool visible{};
    };

    // 完整 Publish 可从无本机来源开始, 也可同版本重建过期正文; 更高版本允许跳号.
    // source/merged 可为空表示从未见过; reading 在最终受理保护内读取, 候选截止不借用远端来源.
    static std::expected<Record, Error> publish(const Record* source, const Record* merged, Value value, std::uint64_t version, std::uint32_t ttl, const Clock::Reading& reading) noexcept;
    // Renew 仅刷新本机来源已有的有效完整记录, 同时检查跨来源水位, 不生成新业务版本.
    static std::expected<Record, Error> renew(const Record* source, const Record* merged, std::uint64_t version, std::uint32_t ttl, const Clock::Reading& reading) noexcept;
    // 合并已验证来源事实, 即使更高事实已过期/仅水位也提高防回退下限. 等版本水位不撤销别处仍有效的正文.
    // now 是当前可用纪元时间, incoming 仍携带源端原截止, 不重新加 TTL; 冲突返回失败且不改变 current.
    static std::expected<Change, Error> merge(const Record* current, const Record& incoming, Clock::Time now) noexcept;
    // 只清理到期正文, 保留最高版本. 无载荷或尚未到期返回原值, 不生成来源事件.
    static Record expire(const Record& current, Clock::Time now) noexcept;
    // 校验原生事实形状和单正文上限, 无时钟读取; 用于来源完整快照和本机候选边界.
    static bool valid(const Record& record) noexcept;

private:
    class Encoding; // 公开分页的字段策略, 不向原生存储引入 Protobuf 类型.
    // 验证已建立的非负时间, 不要求仍与 Pulsar 同步.
    static std::expected<void, Error> ready(const Clock::Reading& reading) noexcept;
    // 统一验证两份可选旧记录, 不借机建立不存在的索引节点.
    static bool valid(const Record* source, const Record* merged) noexcept;
    // 对已知最高版本及同版本正文做精确核对, 非空零字节值仍需要比较.
    static std::expected<void, Error> check(const Record* current, std::uint64_t version, const Value& value) noexcept;
    // 只比较下游可见的存在性/正文/内容版本, 不把期限变化变为内容更新.
    static bool visible(const Record* current, const Record& candidate) noexcept;
    // 将本地时间的期限错误转换为原生失败分类.
    static Error error(Clock::Error value) noexcept;
};
} // namespace astra
