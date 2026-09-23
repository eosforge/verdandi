#pragma once
#include "agenda.hpp"
#include "borrowing.hpp"
#include "catalog.hpp"
#include "origin.hpp"
#include "reading.hpp"
#include "restore.hpp"
#include "scene.hpp"
#include <functional>
#include <map>
#include <shared_mutex>

namespace astra {
// Catalog 自有来源的实际提交器, 保留过期版本水位. 只对直接发布/续租编号, 本地清理不广播.
// 不承担 Session/实例验证, RPC 调用方须持有 Access::Permit 到方法返回; 本对象不调用应用回调或网络.
class Catalog::State {
public:
    using Source = Origin<Record>; // 一个来源位置覆盖本 Star 的全部 Catalog Scope, 不按范围编号.

    // 公开投影只有业务版本和正文, 期限变化不复制页面或制造观察事件.
    struct Content {
        std::uint64_t version{}; // 正内容版本, 不等于 Scope 视图游标或来源位置.
        Value value;             // 当前完整不可变正文, 零字节也是合法存在值.
    };

    using Projection = Scene<Content, Source::Name>;                                 // 与来源共享名称/正文, 不将时限暴露给订阅者.
    using Time = std::move_only_function<std::optional<Clock::Reading>()>;           // 在最终提交锁内采样, 生产绑定真实 Clock.
    using Notify = void (*)(void*, const Scope&, const Projection::Event&) noexcept; // 只用于有界内部收集, 不调用外部用户.

    // 组合层错误, 仍明确区分业务拒绝与容量/历史问题; 异常分配由 RPC 映射为资源不足.
    enum class Error {
        // 输入、Scope、Key、载荷或内容版本非法.
        input,
        // 没有本地可用业务时间, 不等价于 Pulsar 暂时不可达.
        clock,
        // 本机没有活动正文, Renew 不能凭水位恢复.
        ended,
        // 请求低于最高业务版本或 Renew 不等于本机版本.
        version,
        // 相同业务版本提交不同完整正文.
        conflict,
        // 来源位置、视图游标或绝对截止耗尽.
        exhausted,
        // 来源、范围或视图容量不足, 准备失败未发布业务写入.
        capacity,
        // 当前游标无法完整恢复, 应改用完整视图.
        history
    };

    // 所有范围分享有限来源容量和一份下游历史字节预算, 不给每个空范围独立分配轮.
    struct Limits {
        Source::Limits source;                         // 原生状态/发送历史上限, 默认 64 MiB/8 MiB 和 65536 行.
        Projection::Limits projection;                 // 单 Scope 内容/历史上限, 仍受下方 scopes/history 总限制.
        std::size_t scopes = 4096;                     // 曾开放的 Scope 数上限, 空范围保留游标, 不重用位置.
        std::size_t history = 8 * 1024 * 1024;         // 所有 Scope 下游历史的总逻辑字节预算.
        std::size_t replicas = 63;                     // 远端来源数量上限, 不按 Scope 放大.
        std::size_t replica_bytes = 128 * 1024 * 1024; // 全部远端原生来源合计逻辑字节上限.
    };

    // 生产构造复用 Star 公共纪元 Clock, 不创建独立时钟或线程; 初始状态为空且来源位置为零.
    explicit State(Clock& clock);
    // 可注入顺序时钟用于确定性组件用例, time 必需且不得重入本对象; 构造本身不采样.
    State(Time time, Limits limits);
    // 先停止外部直接调用和通知接收者, 再析构; 已捕获内容/来源 View 可继续读取.
    ~State();
    // 不复制来源身份空间或稳定钩子地址.
    State(const State&) = delete;
    // 不覆盖已工作的提交边界.
    State& operator=(const State&) = delete;

    // 仅安装内部不可抛错的提交通知; context 由调用方保证寿命, 更换前须按外层生命周期停止旧使用.
    void notify(Notify notify, void* context);
    // 单 Key 完整发布, 正业务版本可以跳号; 同版本同正文重试可刷新 TTL, 不制造内容通知.
    std::expected<void, Error> publish(const Scope& scope, std::string_view key, Value value, std::uint64_t version, std::uint32_t ttl);
    // 只延长本机有效正文, 不能只凭水位恢复; TTL 每次显式提供, 1s..10m.
    std::expected<void, Error> renew(const Scope& scope, std::string_view key, std::uint64_t version, std::uint32_t ttl);
    // 完整推进真实经过的拍数, 失败传播且不声称已经追平; 已完整提交的先前到期不会回滚.
    void tick();
    // 推进到期后捕获一个公开范围, 未出现范围返回游标零空根, 不创建目录或消耗 Scope 额度.
    std::expected<Projection::View, Error> capture(const Scope& scope);
    // 精确 Key 查找不生成整表快照, 同样先推进到期, 明确返回缺项及范围游标.
    std::expected<Projection::Point, Error> find(const Scope& scope, std::string_view key);
    // 读取连续可见变化, 续租不会出现在其中; 请求前同样推进到期.
    std::expected<std::vector<Projection::Event>, Error> changes(const Scope& scope, std::uint64_t since);
    // 给来源发送器提供完整自有状态/连续发送历史, 不把期限刷新误判为下游内容变化.
    Source::View source();
    std::expected<std::vector<Source::Event>, Error> events(std::uint64_t since);

    // 精确回补响应在同一来源捕获边界取得完整事实及位置, 不扫描全部 Scope.
    struct Point {
        std::uint64_t position{};     // 自有来源连续头部, 不使用公开视图游标.
        std::optional<Record> record; // 来源当前完整事实, 未知为空.
    };

    std::expected<Point, Error> resolve(const Scope& scope, std::string_view key);
    // 返回 true 表示本目标已经被完整前缀或精确回补覆盖, 并按连续规则确认位置.
    std::expected<bool, Error> covered(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key);
    // 来源发送器在同一提交边界取得有界后缀或完整基线, 编码在域锁外执行.
    std::expected<Source::Delivery, Error> deliver(std::uint64_t since, std::size_t count, std::size_t bytes);
    // 只由已验签的复制控制器接纳来源; 已可信替换的旧身份不得重新进入.
    std::expected<void, Error> admit(std::string_view id);
    // 停止接纳旧来源新事实, 已知正文仍按原期限清理, 合并水位不回退.
    void retire(std::string_view id);
    // 来源连续位置, 不含精确回补尚未覆盖的其他目标.
    std::expected<std::uint64_t, Error> received(std::string_view id) const;
    // 私有完整来源候选, 接收方最终 replace 再复查身份及位置.
    std::expected<Source::Draft, Error> prepare(std::string_view id, std::uint64_t position);
    using Recovery = Restore<State>; // 每步一个 Scope, 部分成功靠既有覆盖证据防止重放回退.
    // 创建可取消的私有任务, 本调用不安装任何 Scope; 全部完成后才确认来源位置.
    std::expected<Recovery, Error> restore(std::string_view id, Source::Draft&& draft);
    // 同步排空任务的组件入口, 失败保留此前完整 Scope, 不回滚已经公开的视图.
    std::expected<void, Error> replace(std::string_view id, Source::Draft&& draft);
    // 构造仅期限增量时点查该来源完整值, 不能借用别人的正文冒充本来源.
    std::expected<std::optional<Record>, Error> replica(std::string_view id, const Scope& scope, std::string_view key) const;
    // 严格连续位置的原生事实, Renew 缺少本来源正文返回 ended 触发精确回补.
    std::expected<void, Error> apply(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key, Record record, Source::Form form);
    // 允许返回本来源未知, 但不删除其他来源可见值或合并水位; 只覆盖目标在 R 内的事件.
    std::expected<void, Error> repair(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key, std::optional<Record> record);

private:
    friend class Reading<State>; // 共用读取同步边界, 不公开私有锁或回收类型.
    friend class Restore<State>;
    // 原子安装一个 Scope 的来源/合并/投影/期限及覆盖证据; 不推进全来源确认.
    std::expected<void, Error> install(std::string_view id, const Source::Draft& draft, const Scope& scope);
    // 所有 Scope 完成后复查来源身份和更高覆盖, 再确认完整位置.
    std::expected<void, Error> finish(std::string_view id, std::uint64_t position);

    // 稳定钩子与来源名称共享, 不再保存第二份 deadline/正文.
    struct Timer : Agenda::Node {
        explicit Timer(Source::Tree::Key name) : name(std::move(name)) {} // 名称由已准备的来源持有.

        Source::Tree::Key name; // 用于到期时直接定位 Scope/Key, 不扫来源全表.
    };

    // 远端只保存自身来源及 TTL, 不保留再次广播第三方事实的历史.
    struct Replica {
        // 正在等待连续前缀追平的精确回补或 Scope 安装, 即使正文到期也不提前丢失覆盖证据.
        struct Coverage {
            Source::Tree::Key name;   // Scope/Key 的共享所有权; 空 Key 代表整个 Scope, 不用于业务记录.
            std::uint64_t position{}; // 此目标已安装的 R, 不代表整个组 ACK.
        };

        Replica(std::shared_ptr<std::shared_mutex> gate, Source::Limits limits) : source(std::move(gate), static_cast<Source::Measure>(&State::measure), false, limits) {} // 初始来源位置零.

        std::mutex mutex;                                                       // 原生候选独占锁, 等待时必须释放域锁; 私有准备不持域锁.
        Source source;                                                          // 独立原生根, 不混入别人的高版本.
        std::unique_ptr<Agenda> agenda;                                         // 有限正文首次出现才分配.
        std::unordered_map<const Source::Name*, std::unique_ptr<Timer>> timers; // 只为活动正文保留稳定节点.
        std::vector<Coverage> coverage;                                         // 至多 128 个精确目标加 2 * source.scopes 个 Scope 标记, 连续位置追平后回收.
        bool retired{};                                                         // 可信替换后拒绝迟到任务, 正文全部到期后可回收组.
    };

    using Guard = Borrowing<Replica>; // 同来源准备互斥, 本地来源不取得远端锁.
    // 持有域锁查找, 等待来源锁时释放域锁, 返回后重新核对目录身份; 空 get 表示未知来源.
    Guard acquire(std::string_view id, std::unique_lock<std::shared_mutex>& domain) const;

    // 完整来源恢复为合并索引预分配新钩子, 未提交时撤销, 已有活动钩子不提前改动.
    struct Hooks {
        State& owner;                            // 外层域锁覆盖全部操作.
        std::vector<const Source::Name*> added;  // 本次新增但未调度的索引.
        bool committed{};                        // 完成无异常提交后为真.
        ~Hooks();                                // 失败删除全部新节点.
        void add(const Source::Tree::Key& name); // 已有节点复用, 新项先登记回滚责任.
    };

    // 提交后在域锁外释放旧内容/历史及已摘链钩子, 避免批量到期回收拖住锁.
    struct Retired {
        std::unique_ptr<Timer> deadline; // 被摘下的合并期限节点, 与来源 timer 独立释放.
        Source::Retired merged;          // 合并水位/内容旧引用, 与本机来源独立回收.
        Source::Retired source;          // 本次覆盖/删除及源端历史淘汰.
        Projection::Retired scene;       // 本次可见通知和投影历史淘汰, 无可见变化时为空.
        std::unique_ptr<Timer> timer;    // 已摘链节点, 绝不能在域锁外仍挂在轮中.
    };

    // 创建失败时撤销新目录/钩子, 来源和投影的候选分别由各自 Edit 回滚.
    struct Pending {
        State& owner;                   // 同一域锁覆盖准备寿命.
        const Scope& scope;             // 同步调用期间借用请求地址.
        const Source::Name* timer{};    // 暂存且尚未调度的钩子索引.
        const Source::Name* deadline{}; // 新的合并期限钩子, 放弃时从独立调度表撤销.
        bool created{};                 // 本次创建了尚未公开的空 Scope.
        bool committed{};               // 完整提交后不再回滚.
        ~Pending();                     // 先撤销钩子, 再删除未公开范围.
    };

    // 正文最多 1 MiB, 仅水位计零载荷字节; 元数据由来源/投影自行计费.
    static std::size_t measure(const Record& record) noexcept;
    static std::size_t measure(const Content& record) noexcept;
    // 原生/来源/投影失败统一映射, 不使用错误文本推测提交结果.
    static Error error(Catalog::Error error) noexcept;
    static Error error(Source::Error error) noexcept;
    static Error error(Projection::Error error) noexcept;
    // 最终时钟必须已建立且不倒退, reading 缺失返回明确 clock.
    std::expected<Clock::Reading, Error> reading();
    // 同步执行返回 expected<T, Error> 的内部读取, 整拍边界前共享读取, 越界才独占推进; action 不重入本 State, 不返回受锁保护对象的裸借用.
    // 准备、读取和异常展开均先释放域锁再回收旧资源; 模板仅在本实现文件实例化, 不封装写入的最终取时.
    auto execute(auto&& action);
    // 只在真实写入/副本安装时建立有界范围, 持续保留已提交游标; 只读访问使用 locate.
    std::expected<Projection*, Error> obtain(const Scope& scope);
    // 下游共用剩余额度, 当前 Scope 自己已有历史可在本次覆盖预算内重新使用.
    std::size_t allowance(const Projection& scene) const;
    // 已准备的可见提交计费/通知, 不在这里执行网络或应用回调.
    void publish(Projection& scene, std::size_t before, const Projection::Event& event) noexcept;
    // held 是调用方已经独占的来源, 由其自行推进, 不重复 try_lock; 返回尚有到期/退役责任的忙来源供读者在域锁外等待.
    // tick 回调只处理已经到期的真实记录; 未到真实 deadline 时按当前拍重新安排.
    std::shared_ptr<Replica> advance(Clock::Time now, std::vector<Retired>& retired, Replica* held = nullptr);
    // 远端到期只释放正文并保留该来源水位, 不发本机来源增量.
    void advance(Replica& replica, Clock::Time now, std::vector<Retired>& retired);
    // 连续增量和精确回补的共同原子准备边界.
    std::expected<void, Error> receive(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view key, std::optional<Record> record, Source::Form form, bool repair);
    // 不分配范围的内部查找, 完整组恢复用它判定已经公开的内容.
    Projection* locate(const Scope& scope);
    // 清理到期正文及投影, 来源保留正版本水位且不推进来源位置或追加广播事件.
    std::expected<Retired, Error> expire(const Scope& scope, std::string_view key, Clock::Time time, bool projected);
    // 两种直接写入共用准备/重读时间/提交边界, renewal 不接受正文.
    std::expected<void, Error> change(const Scope& scope, std::string_view key, Value value, std::uint64_t version, std::uint32_t ttl, bool renewal);

    const std::shared_ptr<std::shared_mutex> gate_ = std::make_shared<std::shared_mutex>();     // 写入/合并提交独占, 没有到期维护责任的公开读取共享.
    const std::shared_ptr<std::shared_mutex> export_ = std::make_shared<std::shared_mutex>();   // 本机原生根/日志独立同步域; 只按 gate_ -> export_ 获取, 来源导出不取得 gate_.
    std::mutex timing_;                                                                         // 仅串行调用注入时钟及单调检查, 共享读者不竞争其他读者的正文捕获.
    Clock::Time due_{};                                                                         // 下一次任意轮可能推进的边界; 零强制复核, 无轮时为 max, 不是逐记录第二期限.
    Time time_;                                                                                 // 最后受理边界采样函数, 不允许从外部传入过时 Reading.
    const Limits limits_;                                                                       // 固定的来源/范围/下游容量约束.
    Source source_;                                                                             // 本 Star 自有 Catalog, 一个位置覆盖所有 Scope.
    std::map<std::string, std::shared_ptr<Replica>, std::less<>> replicas_;                     // 只含已准入的有界远端来源.
    std::size_t replica_bytes_{};                                                               // 远端行/目录的当前逻辑计费, 初始零.
    Source merged_;                                                                             // 跨来源最高版本/最长有效截止, 不编号、不广播, 不能当作本机来源完整导出.
    std::unique_ptr<Agenda> outlook_;                                                           // 合并可见记录的期限轮, 与本机来源截止可以不同.
    std::unordered_map<const Source::Name*, std::unique_ptr<Timer>> deadlines_;                 // 合并期限钩子, 与公开投影共享键.
    std::unique_ptr<Agenda> agenda_;                                                            // 首个有限期限前不分配来源轮, 地址不移动.
    std::unordered_map<const Source::Name*, std::unique_ptr<Timer>> timers_;                    // 稳定共享名称指针查钩子, 不复制第二份 Key.
    std::map<std::string, std::map<std::string, Projection, std::less<>>, std::less<>> scenes_; // 二元可见目录, 空范围保留游标.
    std::size_t scopes_{};                                                                      // 已建立的投影范围数, 默认零.
    std::size_t history_{};                                                                     // 所有 Scene 历史逻辑字节, 默认零.
    std::optional<Clock::Time> observed_;                                                       // 仅用于拒绝注入时钟倒退, 不是记录第二期限.
    Notify notify_{};                                                                           // 内部提交通知, 默认不通知.
    void* context_{};                                                                           // notify_ 关联上下文, 默认空.
};
} // namespace astra
