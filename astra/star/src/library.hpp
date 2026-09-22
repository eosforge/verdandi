#pragma once
#include "almanac.hpp"
#include <astra/scope.hpp>
#include <functional>
#include <map>
#include <shared_mutex>

namespace astra {
// Almanac 的 Sector/Spectrum 两级路由. 唯一 Polaris 安装者串行修改, 普通读者只短暂查索引.
// 本类不保存动态来源, 不把 Catalog/Ephemeris 重新装入统一 Store.
class Library {
public:
    // 部署资源初值, 限定当前内容/历史, 不声称等于所有用户保有旧 View 的物理内存.
    struct Limits {
        // 最多完整分组数, 默认 4096, 必须非零, 空分组保留安装版本且占一项.
        std::size_t scopes = 4096;
        // 全部 Almanac 当前 Key/Buffer 总字节, 默认 256 MiB.
        std::size_t bytes = 256 * 1024 * 1024;
        // 全部 Almanac 可保留历史总逻辑字节, 默认 64 MiB, 零允许禁用增量历史.
        std::size_t history = 64 * 1024 * 1024;
        // 每个分组的原生容量, 默认值见 Almanac::Limits.
        Almanac::Limits scope;
    };

    // 一份范围已完整安装的位置, 版本零是有效空基线.
    struct Position {
        // 拥有式两级地址, 返回后可脱离 Library 使用.
        Scope scope;
        // 同范围权威版本, 不混入本地提交计数.
        std::uint64_t version{};
    };

    // 接收者独占的一份私有快照候选. 可以跨页填充, 不可复制或修改已经安装内容.
    class Draft {
    public:
        // 逐项接管 key/value, 成功计入候选; 失败保留此前候选, 分配错误向接收层传播.
        std::expected<void, Almanac::Error> set(std::string key, Almanac::Buffer value);
        // 获取此候选固定的 Scope, 仅借用至候选销毁.
        const Scope& scope() const noexcept;
        // 获取此候选固定的权威版本, 每页都必须相同.
        std::uint64_t version() const noexcept;
        // 将唯一候选交给另一恢复任务, 被移动对象不可继续 set/安装.
        Draft(Draft&&) noexcept = default;
        // 禁止复制候选和可写页面.
        Draft(const Draft&) = delete;

    private:
        friend class Library;
        // 只由 Library 创建, owner 是对应路由, scope/version/limits 冻结本次恢复边界.
        Draft(const Library* owner, Scope scope, std::uint64_t version, std::shared_ptr<Almanac> book);
        // 路由身份只作一致性比较, 不解引用; 候选不能交给另一 Library.
        const Library* owner_;
        // 已校验的两级地址, 与 version_ 一起固定.
        Scope scope_;
        // 待安装版本, 无符号完整范围, 零只允许空内容.
        std::uint64_t version_;
        // 目标原生对象, 未插入路由的新 Scope 也由此独占保活.
        std::shared_ptr<Almanac> book_;
        // 原生 COW 页面准备, 失败或断流只丢弃此候选.
        Almanac::Draft draft_;
        // 已成功准备的 Key/Buffer 总字节, 初始零.
        std::size_t bytes_{};
    };

    // 创建空路由, 不把空进程当作已经收到 Polaris 完整范围清单.
    Library();
    // 固定部署容量, 非法上限抛 invalid_argument, 不启动线程或网络.
    // changed 在成功提交后、writer_ 内串行调用, change.key 为空表示全量替换; 不得重入 Library 或抛异常.
    // 回调只能有界入队, 不能编码、等待网络或取得 Access 锁; 活动订阅不依赖历史仍未淘汰.
    using Notify = std::move_only_function<void(const Scope&, const Almanac::Change&) noexcept>;
    explicit Library(Limits limits, Notify changed = {});
    // 返回只读原生分组的共享所有权, 不存在或非法地址返回空, 不创建临时空范围.
    std::shared_ptr<const Almanac> find(const Scope& scope) const;
    // 构造锁外候选, 低于现有完整版本拒绝, 全局准备并发由上层同步流限定为一份.
    std::expected<Draft, Almanac::Error> prepare(Scope scope, std::uint64_t version);
    // 安装完整候选并维护全局计费, true 为新提交, false 为相同版本重放.
    std::expected<bool, Almanac::Error> reset(Draft&& draft);
    // 修改已存在完整 Scope 的单 Key, 不按不存在的 Scope 偷建零版本基线.
    std::expected<bool, Almanac::Error> apply(const Scope& scope, std::uint64_t version, std::string key, std::optional<Almanac::Buffer> value);
    // 获取全部完整位置的稳定清单, 与安装串行; 不复制内容, 不借用路由节点.
    std::vector<Position> positions() const;

private:
    // 返回内部可写对象, 只在本类持有 writer_ 时修改, 普通读入口不暴露写权限.
    std::shared_ptr<Almanac> locate(const Scope& scope) const;
    // 固定容量, 不在运行时对已有分组更换规则.
    const Limits limits_;
    // 固定的提交通知接收者, 不在运行中替换; 空回调表示本路由没有网络观察者.
    Notify changed_;
    // 安装/位置捕获的短期协调锁, 不在网络或快照准备期间持有; 读者点查不持有此锁.
    mutable std::mutex writer_;
    // 只保护两级路由增添与定位, 现有分组提交不占此锁.
    mutable std::shared_mutex mutex_;
    // 两级有序地址索引, 稳定共享对象独立拥有自己的读写锁.
    std::map<std::string, std::map<std::string, std::shared_ptr<Almanac>, std::less<>>, std::less<>> books_;
    // 以下三个计数由 writer_ 保护, 初始零, 只在成功安装后更新.
    std::size_t scopes_{};
    // 全部分组当前 Key/Buffer 逻辑字节, 不含旧视图.
    std::size_t bytes_{};
    // 全部分组保留历史逻辑字节, 随各 Scope 连续前缀淘汰一起更新.
    std::size_t history_{};
};
} // namespace astra
