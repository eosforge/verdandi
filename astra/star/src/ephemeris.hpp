#pragma once
#include <astra/clock.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace astra {
// Ephemeris 的原生业务规则. 索引键提供 UUID, 来源组提供身份/Scope, 记录不重复保存这些字符串.
// 方法返回尚未发布的候选, 不产生来源版本或发通知; 调用方在准备好索引/历史/投影后统一无异常提交.
class Ephemeris {
public:
    // 原生来源、TTL 和可见投影的组合实现, 定义置于 ephemeris_state.hpp, 不暴露 gRPC 类型.
    class State;
    // 公共 gRPC 适配定义独立放入 ephemeris_service.hpp, 原生头文件不包含生成协议或网络类型.
    class Service;
    // 一批已冻结的公开注册视图或增量, 完成后才能推进 Watch 游标.
    class Edition;
    // 活动下行任务与有界后缀, 由 Runtime 共享循环推进, 不创建每 Watch 线程.
    class Feed;
    // 普通载荷不解码, 空容器是合法 Attr/Data, 与空指针的非法缺项区分.
    using Buffer = std::vector<std::uint8_t>;
    // 只接受真正不可变的共享载荷, 调用方不得保留可写别名.
    using Value = std::shared_ptr<const Buffer>;

    // 原生错误由 RPC 层转换, 不携带秘密或把未提交标成成功.
    enum class Error {
        // TTL/order/载荷不符合静态契约.
        input,
        // 尚无可用本地时间, 不等价于 Pulsar 暂时失联.
        clock,
        // 当前 UUID 已经超过有限截止, 不允许通过更新或续租复活.
        ended,
        // 操作顺序低于本 UUID 对应类型的最新顺序.
        obsolete,
        // 同一 Data order 被重用为不同的正文.
        conflict,
        // 有限截止相加溢出, 不转成永久租约.
        exhausted
    };

    // 活动来源记录, Attr/Data 原生分开, 一条记录只有一个有限截止.
    struct Record {
        // 固定属性, Create 后不可修改, 合法记录始终非空指针.
        Value attr;
        // 当前完整动态正文, 合法零字节也非空指针.
        Value data;
        // 统一 Unix 纳秒截止; 大整数仍是有限期限, 不依赖客户端本地墙钟.
        Clock::Time deadline{};
        // 最新 Data 确认顺序, 新 UUID 初始零, 新请求必须为正且允许跳号.
        std::uint64_t update{};
        // 最新 Renew 确认顺序, 与 update 独立, 新 UUID 初始零.
        std::uint64_t renewal{};
        // Create 固定的毫秒 TTL, 范围 1000..600000, 后续 Renew 不接受修改.
        std::uint32_t ttl{};
    };

    // 原生候选与可见变化分开, 新 order 的同值更新仍是来源事实, 不改变 Observer 内容游标.
    struct Change {
        // 准备后的完整记录, 原记录在实际提交前保持不变.
        Record record;
        // true 为新的来源事实, false 是当前 order 的幂等确认, 不重复增长来源版本.
        bool changed{};
        // true 才需要更新内容投影/唤醒观察者, Renew 永远为 false.
        bool visible{};
    };

    // 验证规范小写 UUIDv4, 无格式化/分配; 不接受文本别名, 不将非法 UUID 当作全范围.
    static bool valid(std::string_view uuid) noexcept;
    // 仅 Create 调用可靠系统随机源并格式化一次; 失败抛 system_error, 不降级伪随机.
    static std::string uuid();
    // 验证并准备新的完整注册. attr/data 移交共享引用, reading 必须在最终提交保护内读取.
    static std::expected<Record, Error> create(Value attr, Value data, std::uint32_t ttl, const Clock::Reading& reading) noexcept;
    // 新 order 替换整个 Data, 不延期、不修改 Attr. 同 order 同内容确认, 低 order 或同号异值拒绝.
    static std::expected<Change, Error> update(const Record& current, Value data, std::uint64_t order, const Clock::Reading& reading) noexcept;
    // 新 order 才按固定 TTL 延期, 同 order 保留旧截止; 两个顺序相互独立, 过期判断优先于幂等确认.
    static std::expected<Change, Error> renew(const Record& current, std::uint64_t order, const Clock::Reading& reading) noexcept;
    // 最终受理边界确认本地计时和期限, 不等待后台物理删除, 不做来源实例/Session 校验的替代品.
    static std::expected<void, Error> active(const Record& current, const Clock::Reading& reading) noexcept;

private:
    // 验证一份有载荷的完整原生记录; 仅供已解码来源快照或内部候选使用.
    static bool valid(const Record& record) noexcept;
    // 验证非空共享载荷及 1 MiB 静态单正文上限, 不复制正文.
    static bool valid(const Value& value) noexcept;
    // 将 Clock 的期限错误映射成原生错误, 不改变 ready/synchronized 的分离规则.
    static Error error(Clock::Error value) noexcept;
};
} // namespace astra
