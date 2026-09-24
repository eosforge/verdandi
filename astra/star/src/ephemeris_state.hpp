#pragma once
#include "agenda.hpp"
#include "borrowing.hpp"
#include "ephemeris.hpp"
#include "origin.hpp"
#include "reading.hpp"
#include "restore.hpp"
#include "scene.hpp"
#include <functional>
#include <map>
#include <shared_mutex>

namespace astra {
// 单 Star 自有 Ephemeris 的实际提交器. 统一定序原生状态、来源历史、内容投影和一个来源级时间轮.
// 不承担 Session/实例验证, RPC 调用方须持有 Access::Permit 到方法返回; 本对象不调用应用回调或网络.
class Ephemeris::State {
public:
    using Source = Origin<Record, true>; // 一个来源位置覆盖本 Star 的全部 Ephemeris Scope, 不按范围编号.

    // 公开投影没有 TTL/order, Renew 和同字节 Update 不复制这份页面或制造观察事件.
    struct Content {
        Value attr; // 固定属性的原生不可变引用.
        Value data; // 当前动态数据的原生不可变引用, 零字节也是合法存在值.
    };

    using Projection = Scene<Content, Source::Name>;                                 // 与来源共享名称/Attr/Data, 无双 Key 配对.
    using Time = std::move_only_function<std::optional<Clock::Reading>()>;           // 在最终提交锁内采样, 生产绑定真实 Clock.
    using Notify = void (*)(void*, const Scope&, const Projection::Event&) noexcept; // 只用于有界内部收集, 不调用外部用户.

    // 组合层错误, 仍明确区分业务拒绝与容量/历史问题; 异常分配由 RPC 映射为资源不足.
    enum class Error {
        // 输入、Scope、UUID、载荷或 order 非法.
        input,
        // 没有本地可用业务时间, 不等价于 Pulsar 暂时不可达.
        clock,
        // 自有 UUID 已过期或不存在, 不能通过 Update/Renew 复活.
        ended,
        // order 低于已确认值.
        obsolete,
        // 同 order 对应不同 Data, 或有限随机重试仍冲突.
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
        std::size_t replicas = 63;                     // 远端来源数量上限, 不是每 Scope 的数量.
        std::size_t replica_bytes = 128 * 1024 * 1024; // 本域所有远端原生来源合计逻辑预算.
    };

    // 创建确认只含已提交 UUID 和固定 TTL, 不返回调度节点或公开原生 order 元数据.
    struct Receipt {
        std::string uuid;    // 本 Star 新分配的 16 字节 UUIDv4 二进制, 成功后由调用方持有.
        std::uint32_t ttl{}; // 此注册固定毫秒 TTL, 1000..600000.
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
    // 创建新 UUID, 截止在最后一次可用时间读数上计算; 失败不泄漏记录/轮节点/来源位置.
    std::expected<Receipt, Error> create(const Scope& scope, Value attr, Value data, std::uint32_t ttl);
    // 只修改已有自有 UUID, 末次准备后重查其期限, 不隐式续租.
    std::expected<void, Error> update(const Scope& scope, std::string_view uuid, Value data, std::uint64_t order);
    // 固定 TTL 的新 order 续租, 同 order 只确认且不增加来源位置/延长截止.
    std::expected<void, Error> renew(const Scope& scope, std::string_view uuid, std::uint64_t order);
    // 显式权威结束, 已结束返回 ended; 删除与来源历史/可见变化在同边界发布.
    std::expected<void, Error> remove(const Scope& scope, std::string_view uuid);
    // 完整推进真实经过的拍数, 失败传播且不声称已经追平; 已完整提交的先前到期不会回滚.
    void tick();
    // 推进到期后捕获一个公开范围, 未出现范围返回游标零空根, 不创建目录或消耗 Scope 额度.
    std::expected<Projection::View, Error> capture(const Scope& scope);
    // 精确查找不生成整表快照, 同样先推进到期, 明确返回缺项及范围游标.
    std::expected<Projection::Point, Error> find(const Scope& scope, std::string_view uuid);
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
    // 仅供已验签的对等控制器建立来源, id 为该 Star 进程身份; 不执行认证或重新接纳已替换身份.
    std::expected<void, Error> admit(std::string_view id);
    // 仅在控制器已验证同部署较新身份后冻结旧来源, 已有数据继续原 TTL 倒计时.
    void retire(std::string_view id);
    // 远端连续已安装位置, 未登记来源返回 input; 回补位置不冒充为组位置.
    std::expected<std::uint64_t, Error> received(std::string_view id) const;
    // 私有来源基线, 最终 replace 仍核对位置/当前期限, 准备本身不安装记录.
    std::expected<Source::Draft, Error> prepare(std::string_view id, std::uint64_t position);
    using Recovery = Restore<State>; // 每次推进至多安装一个 Scope, 不跨调用持有域锁.
    // 完整候选转换为分步恢复任务, 来源确认仍等待全部 Scope 成功.
    std::expected<Recovery, Error> restore(std::string_view id, Source::Draft&& draft);
    // 同步完成分步恢复; 失败保留已安装 Scope 的覆盖证据, 不回滚已公开数据.
    std::expected<void, Error> replace(std::string_view id, Source::Draft&& draft);
    // 复制适配器构造 Data/Renew 前点查完整副本, 缺失需回补, 不获得本机写权限.
    std::expected<std::optional<Record>, Error> replica(std::string_view id, const Scope& scope, std::string_view uuid) const;
    // 连续完整来源事实; form 保留 Data/Renew 语义, 缺失返回 ended 供精确回补.
    std::expected<void, Error> apply(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view uuid, std::optional<Record> record, Source::Form form);
    // 精确回补只覆盖该目标在 position 以内的旧事实, 不推进整个来源 ACK; 关联请求由流控制器核对.
    std::expected<void, Error> repair(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view uuid, std::optional<Record> record);

private:
    friend class Reading<State>; // 共用读取同步边界, 不公开私有锁或回收类型.
    friend class Restore<State>;
    // 同一范围的原生状态/可见投影/定时器原子安装, 不提前确认完整来源.
    std::expected<void, Error> install(std::string_view id, const Source::Draft& draft, const Scope& scope);
    // 所有范围已完成后推进来源位置并回收临时覆盖证据.
    std::expected<void, Error> finish(std::string_view id, std::uint64_t position);

    // 稳定钩子与来源名称共享, 不再保存第二份 deadline/Attr/Data.
    struct Timer : Agenda::Node {
        explicit Timer(Source::Tree::Key name) : name(std::move(name)) {} // 名称由已准备的来源持有.

        Source::Tree::Key name; // 用于到期时直接定位 Scope/UUID, 不扫来源全表.
    };

    // 一个远端 Star 的独立来源/调度器, 不保留向第三方再次广播的历史.
    struct Replica {
        // 为正在追赶的精确目标或 Scope 安装保留覆盖标记, 连续位置追平后回收, 不是所有 UUID 的永久副表.
        struct Coverage {
            Source::Tree::Key name;   // 共享地址/UUID, 空 Key 代表整个 Scope; 正文到期删除后仍保持关联.
            std::uint64_t position{}; // 已安装目标或 Scope 的位置, 不能冒充本组连续位置.
        };

        Replica(std::shared_ptr<std::shared_mutex> gate, Source::Limits limits) : source(std::move(gate), static_cast<Source::Measure>(&State::measure), false, limits) {} // 来源位置初始零.

        std::mutex mutex;                                                       // 原生候选独占锁, 等待时必须释放域锁; 私有准备不持域锁.
        Source source;                                                          // 该 Star 的全部 Scope, 不与其他来源共用编号.
        std::unique_ptr<Agenda> agenda;                                         // 断流后仍独立推进原截止.
        std::unordered_map<const Source::Name*, std::unique_ptr<Timer>> timers; // 名称共享的稳定钩子.
        std::vector<Coverage> coverage;                                         // 至多 128 个精确目标加 2 * source.scopes 个 Scope 标记, 与正文同边界提交.
        bool retired{};                                                         // 可信替换后拒绝新事实, 清空后可回收组; 旧身份拒绝依据由控制器保留.
    };

    using Guard = Borrowing<Replica>; // 同来源准备互斥, 本地来源不取得远端锁.
    // 持有域锁查找, 等待来源锁时释放域锁, 返回后重新核对目录身份; 空 get 表示未知来源.
    Guard acquire(std::string_view id, std::unique_lock<std::shared_mutex>& domain) const;

    // 远端提交准备的可见所有权索引, 失败只撤销本次新增项, 不删除已安装身份.
    struct Ownership {
        State& owner;                                              // 同一域锁覆盖准备/回滚.
        std::vector<const Source::Name*> added;                    // 候选名称覆盖此借用寿命.
        bool committed{};                                          // 来源/投影共同提交后才为真.
        ~Ownership();                                              // 失败擦除新增项, 不分配.
        void add(const Source::Tree::Key& name, Replica& replica); // 先记录撤销责任, 再 emplace.
    };

    // 提交后在域锁外释放旧内容/历史及已摘链钩子, 避免批量到期回收拖住锁.
    struct Retired {
        Source::Retired source;       // 本次覆盖/删除及源端历史淘汰.
        Projection::Retired scene;    // 本次可见通知和投影历史淘汰, 无可见变化时为空.
        std::unique_ptr<Timer> timer; // 已摘链节点, 绝不能在域锁外仍挂在轮中.
    };

    // 创建失败时撤销新目录/钩子, 来源和投影的候选分别由各自 Edit 回滚.
    struct Pending {
        State& owner;                // 同一域锁覆盖准备寿命.
        const Scope& scope;          // 同步调用期间借用请求地址.
        const Source::Name* timer{}; // 暂存且尚未调度的钩子索引.
        bool created{};              // 本次创建了尚未公开的空 Scope.
        bool committed{};            // 完整提交后不再回滚.
        ~Pending();                  // 先撤销钩子, 再删除未公开范围.
    };

    // 仅共享两个原生载荷引用, 大小相加受各 1 MiB 业务上限约束.
    static std::size_t measure(const Record& record) noexcept;
    static std::size_t measure(const Content& record) noexcept;
    // 原生/来源/投影失败统一映射, 不使用错误文本推测提交结果.
    static Error error(Ephemeris::Error error) noexcept;
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
    // 副本到期不推进来源位置/自有广播, 只清理对应可见注册.
    void advance(Replica& replica, Clock::Time now, std::vector<Retired>& retired);
    // 不创建范围的内部查找, 供来源安装/本地过期判定可见状态.
    Projection* locate(const Scope& scope);
    // 来源连续事实和精确回补共用原子准备; repair=true 只生成目标覆盖, 不增加来源位置.
    std::expected<void, Error> receive(std::string_view id, std::uint64_t position, const Scope& scope, std::string_view uuid, std::optional<Record> record, Source::Form form, bool repair);
    // 同时准备来源与投影的删除, 所有可失败步骤完成后才摘链/发布/归还旧资源.
    std::expected<Retired, Error> erase(const Scope& scope, std::string_view uuid, std::chrono::steady_clock::time_point now, bool active = false);
    // Update/Renew 共用两阶段提交, renew 为 true 时不接受 data, 两种业务 order 仍独立.
    std::expected<void, Error> change(const Scope& scope, std::string_view uuid, Value data, std::uint64_t order, bool renewal);

    const std::shared_ptr<std::shared_mutex> gate_ = std::make_shared<std::shared_mutex>();     // 写入/合并提交独占, 没有到期维护责任的公开读取共享.
    const std::shared_ptr<std::shared_mutex> export_ = std::make_shared<std::shared_mutex>();   // 本机原生根/日志独立同步域; 只按 gate_ -> export_ 获取, 来源导出不取得 gate_.
    std::mutex timing_;                                                                         // 仅串行调用注入时钟及单调检查, 共享读者不竞争其他读者的正文捕获.
    Clock::Time due_{};                                                                         // 下一次任意轮可能推进的边界; 零强制复核, 无轮时为 max, 不是逐记录第二期限.
    Time time_;                                                                                 // 最后受理边界采样函数, 不允许从外部传入过时 Reading.
    const Limits limits_;                                                                       // 固定的来源/范围/下游容量约束.
    Source source_;                                                                             // 本 Star 自有 Ephemeris, 一个位置覆盖所有 Scope.
    std::map<std::string, std::shared_ptr<Replica>, std::less<>> replicas_;                     // 受数量/总字节约束的远端组.
    std::unordered_map<const Source::Name*, Replica*> owners_;                                  // 仅索引可见远端条目, 本机条目不在此表.
    std::size_t replica_bytes_{};                                                               // 所有远端当前行/目录计费, 无第三方发送历史.
    std::unique_ptr<Agenda> agenda_;                                                            // 首个有限期限前不分配来源轮, 地址不移动.
    std::unordered_map<const Source::Name*, std::unique_ptr<Timer>> timers_;                    // 稳定共享名称指针查钩子, 无 UUID 再格式化.
    std::map<std::string, std::map<std::string, Projection, std::less<>>, std::less<>> scenes_; // 二元可见目录, 空范围保留游标.
    std::size_t scopes_{};                                                                      // 已建立的投影范围数, 默认零.
    std::size_t history_{};                                                                     // 所有 Scene 历史逻辑字节, 默认零.
    std::optional<Clock::Time> observed_;                                                       // 仅用于拒绝注入时钟倒退, 不是记录第二期限.
    Notify notify_{};                                                                           // 内部提交通知, 默认不通知.
    void* context_{};                                                                           // notify_ 关联上下文, 默认空.
};
} // namespace astra
