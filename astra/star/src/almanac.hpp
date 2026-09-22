#pragma once
#include "snapshot_index.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace astra {
// 单个 Sector/Spectrum 的 Almanac 副本. 只接受调用方已经认证的 Polaris 数据, 不负责准入或网络.
// 不使用 Store 的本地版本、TTL 或时间轮. 全量采用权威版本, 增量严格 +1, 读者不能修改底稿.
class Almanac {
    // 私有状态包含直接查找表及分页索引; 定义隐藏在实现文件, 不作为跨组件数据格式.
    struct State;

public:
    // 沿用现有字节所有权, 不引入专有 Buffer 胶水类; 空容器表示合法零字节值.
    using Buffer = std::vector<std::uint8_t>;
    // 已受理载荷只有不可变共享引用, 不与调用方保留可写别名.
    using Value = std::shared_ptr<const Buffer>;

    // 原生状态错误, 由 RPC 适配层映射为稳定原因; 分配失败仍抛 bad_alloc, 不伪装成版本冲突.
    enum class Error {
        // Key 或正文不符合静态单记录约束, 或使用已被消费的全量准备对象.
        input,
        // Scope 内容或记录数超过本对象配置的硬容量.
        capacity,
        // 快照准备出现重复 Key, 不能以最后一页覆盖前页来接受不一致快照.
        duplicate,
        // 全量回退、增量跳号、零版本补丁或版本耗尽; 不修改当前完整状态.
        version,
        // 首份完整基线尚未安装, 不能把默认空内存当作权威空 Scope.
        unready,
        // 请求的完整连续历史已不可取得, 上层需要重新捕获基线.
        history
    };

    // 每 Scope 的局部内容/历史上限, 不是进程 RSS 或跨 Scope 的恢复总预算.
    // 服务接入时仍须在外层为并发准备、旧视图、索引元数据及网络在途统一计费.
    struct Limits {
        // 当前记录数上限, 默认 65536; 零允许合法空 Scope, 拒绝任何 Set.
        std::size_t records = 65536;
        // 当前 Key 与 Buffer 的合计字节上限, 默认 64 MiB; 零只允许空 Scope.
        std::size_t bytes = 64 * 1024 * 1024;
        // 连续单 Key 历史条数上限, 默认 1000; 零表示所有旧游标均需重新取基线.
        std::size_t history = 1000;
        // 历史的 Change、Key 与 Buffer 合计计费上限, 默认 8 MiB; 零不保留历史.
        std::size_t backlog = 8 * 1024 * 1024;
    };

    // 一次已完整安装的单 Key 权威提交; 版本无缺口, 即使同值 Set 或缺失 Delete 也占一个位置.
    struct Change {
        // 拥有不可变 Key, 长度 1..1024 字节, 与查找表和索引共享原字符串.
        std::shared_ptr<const std::string> key;
        // 非空为完整 Set, 空为 Delete; 零字节 Set 仍持有非空指针.
        Value value;
        // Polaris 为该 Scope 分配的版本, 有效增量范围 1..UINT64_MAX.
        std::uint64_t version{};

        // 返回本历史项的保守逻辑字节计费, 不包含分配器和容器额外成本.
        std::size_t bytes() const noexcept;
    };

    // 不可变根与权威版本在同一个提交边界捕获, 捕获本身不遍历或复制整张 Map.
    // View 可越过 Almanac 寿命; 只保留独立读完成互斥量及页面, 不保持任何网络对象存活.
    class View {
    public:
        // 返回捕获时的权威版本; 零是已安装的合法空基线, 不是未就绪.
        std::uint64_t version() const noexcept;
        // 返回捕获时的有效记录数, 使用捕获的标量, 不访问可复用页面.
        std::size_t size() const noexcept;
        // 捕获时 Key/Buffer 的总逻辑字节, 与固定根同一边界, 用于上层共享的快照准备预算.
        std::size_t bytes() const noexcept;

        // reader 在调用期间借用 Key/Value, 可抛异常, 不在业务锁内调用.
        // 需要跨调用保存内容时复制 Value 所有权或 Key 文本, 不保留传入的引用.
        // 可以重入仍存活 Almanac 的公开操作; each 退出时仍需取得提交锁, 不保证无等待或限定等待时长.
        // 持有上层锁调用 each 时必须遵循统一锁顺序, 不能与提交路径形成反向等待.
        void each(auto&& reader) const {

            // root 保持本次遍历页面存活, fence 为正常返回与异常展开建立读完成同步.
            // root 必须先于 fence 构造, 确保 fence 先完成同步再释放本次根引用; 不以 use_count 充当屏障.
            const auto root = root_;
            const Fence fence{gate_};
            root.each([&reader](const std::string& key, const Index::Record& record) { reader(key, record.value); });
        }

        // 与 each 采用同一读完成同步, 每次最多访问 maximum 项, reader 返回 false 可以按字节预算暂停.
        // offset 为本视图已消费项数, 返回本页新增项数; 不构造整表副本或改变持久化版本.
        std::size_t page(std::size_t offset, std::size_t maximum, auto&& reader) const {

            const auto root = root_;  // 固定根覆盖本次分段访问, 与持有者解耦.
            const Fence fence{gate_}; // 成功与异常均先同步再归还临时根.
            return root.page(offset, maximum, [&reader](const std::string& key, const Index::Record& record) { return reader(key, record.value); });
        }

    private:
        friend class Almanac;

        // 一次读取结束后经过写者所用的同一互斥量, 再归还本次根引用.
        struct Fence {
            // 独立持有互斥量寿命, 不借用 Almanac/Client 内部地址.
            std::shared_ptr<std::mutex> gate;
            // 在最后一次页读取之后同步, 包括用户 reader 抛出异常的路径.
            ~Fence();
        };

        // 仅由 Almanac 在锁内构造完整视图; root/version/bytes/gate 为固定页面、权威位置、内容计费和读写同步域.
        View(Index::View root, std::uint64_t version, std::size_t bytes, std::shared_ptr<std::mutex> gate);
        // 捕获的页面树, 后续写入只复制与本视图共享的必要路径.
        Index::View root_;
        // 捕获时的权威位置, 与 root_ 一起固定.
        std::uint64_t version_{};
        // 捕获时的记录数, 避免 size() 读取内部可复用页.
        std::size_t size_{};
        // 捕获时的当前内容计费, 不随随后写入改变.
        std::size_t bytes_{};
        // 独立同步域, 对象退出后由仍存活的 View 持有.
        std::shared_ptr<std::mutex> gate_;
    };

    // 全量在私有候选中构建. 直到 reset 成功之前, 分页中断、重复 Key 或分配失败均不影响当前状态.
    // 一个 Draft 由单个恢复任务拥有, 本身不支持并发修改; 首版不在这里伪造网络 complete 标记.
    class Draft {
    public:
        // 放弃未提交的候选, 只释放自己的页面与载荷, 不访问 Almanac.
        ~Draft();
        // 恢复任务可以转交唯一候选, 被移动对象只能析构或接收赋值.
        Draft(Draft&&) noexcept;
        // 丢弃原候选后接管另一候选, 不安装到任何 Almanac.
        Draft& operator=(Draft&&) noexcept;
        // 禁止复制整份恢复准备状态.
        Draft(const Draft&) = delete;
        // 禁止通过复制赋值建立第二个候选所有者.
        Draft& operator=(const Draft&) = delete;
        // 接管 key/value, 添加一个唯一记录. 返回失败或抛异常时不改变已有候选内容.
        // key 为 1..1024 字节, value 最多 1 MiB; 版本为零的空基线不能包含数据.
        std::expected<void, Error> set(std::string key, Buffer value);

    private:
        friend class Almanac;
        // 只由 prepare 创建, version 是此次完整基线的权威位置, limits 为固定容量.
        Draft(std::uint64_t version, Limits limits);
        // 锁外构建的唯一候选, 移交成功或被移动后为空.
        std::unique_ptr<State> state_;
        // 准备过程的局部容量, reset 仍按目标 Almanac 的上限重新核对.
        Limits limits_;
    };

    // 单 Key 点查与版本在同一边界取得, 不构建整组快照.
    struct Point {
        // 当前完整 Scope 的权威版本, 包括没有目标 Key 的情况.
        std::uint64_t version{};
        // 空指针表示 Key 不存在, 非空零字节值表示存在的空记录.
        Value value;
    };

    // 有界连续历史的一次拥有式提取, 载荷与当前状态/历史共享, 不复制 Buffer.
    struct Replay {
        // 完整覆盖到的权威位置; 空 changes 仅允许请求已在此位置.
        std::uint64_t version{};
        // 按 version 递增排列的所有完整单 Key 提交, 不提前合并同 Key.
        std::vector<Change> changes;
    };

    // 一次短锁读取的逻辑计费, 不包含外部保有旧 View 的页面, 不声称等于进程 RSS.
    struct Usage {
        // 当前有效 Key/Buffer 合计字节, 初始零.
        std::size_t bytes{};
        // 当前保留增量历史的逻辑计费, 初始零.
        std::size_t history{};
        // 完整安装版本, 未就绪时为空, 显式零代表合法空基线.
        std::optional<std::uint64_t> version;
    };

    // 使用默认局部容量创建未就绪 Scope, 不把空构造视为取得 Polaris 权威基线.
    Almanac();
    // limits 由本对象复制并固定; 只创建原生状态, 不安装时钟、线程或网络回调.
    explicit Almanac(Limits limits);
    // 调用方必须先停止对本对象的直接访问; 已返回 View 和 Value 仍可独立存活.
    ~Almanac();
    // 一个 Scope 只由一个状态对象负责安装, 不复制身份与提交边界.
    Almanac(const Almanac&) = delete;
    // 禁止覆盖已被读取方使用的提交边界.
    Almanac& operator=(const Almanac&) = delete;
    // 在锁外创建指定版本的空候选, 不要求当前对象已就绪.
    Draft prepare(std::uint64_t version) const;
    // 只在网络层确认全量完整后调用. 更高基线一次替换全部记录并清空旧历史; 首份允许空版本零.
    // true 表示安装, false 表示同版本重复; 低版本拒绝. 失败不消费候选, 成功后候选为空.
    std::expected<bool, Error> reset(Draft&& draft);
    // 只允许当前完整版本 +1 的单 Key Set/Delete. value=nullopt 表示 Delete, 空 Buffer 是合法 Set.
    // 已安装范围内的非零重放返回 false, 不重做提交; 新版本成功返回 true. 失败保持状态与版本.
    // retention 为外层此 Scope 可使用的历史字节余额, 默认不额外限制, 不改变合法写入是否提交.
    // committed 可选接收本次已提交的不可变变更, 即使历史预算为零仍可向活动订阅通知; 失败不修改它.
    std::expected<bool, Error> apply(std::uint64_t version, std::string key, std::optional<Buffer> value, std::size_t retention = std::numeric_limits<std::size_t>::max(), Change* committed = nullptr);
    // 短锁捕获根、数量与版本; 未安装基线返回 unready, 不返回临时空 View.
    std::expected<View, Error> view() const;
    // 直接查找 key, 不创建不存在的条目或改变版本; 空/超长 Key 返回 input.
    std::expected<Point, Error> find(std::string_view key) const;
    // 取得 since 之后到当前版本的完整连续历史. 超前返回 version, 历史不足返回 history.
    // 结果受本对象历史条数/字节限制, 分配失败传播且不影响已经提交的数据.
    std::expected<Replay, Error> replay(std::uint64_t since) const;
    // 获取内容/历史逻辑容量和完整安装位置, 不复制根或遍历记录.
    Usage usage() const;

private:
    // key/value 的长度与 NUL 校验; 调用方须先完成 UTF-8、身份及 Sector 的 __ 入口校验.
    static bool valid(std::string_view key, const Buffer* value) noexcept;
    // 固定局部容量, 不是公共协议协商结果.
    const Limits limits_;
    // 独立分配一次的同步域, View 可延长其寿命, 不延长 Almanac 或服务寿命.
    const std::shared_ptr<std::mutex> gate_ = std::make_shared<std::mutex>();
    // 当前原生内容, 仅在 gate_ 内读取/修改; 全量提交使用指针交换, 旧状态在锁外回收.
    std::unique_ptr<State> state_;
    // 当前基线是否由 reset 完整安装, 初始 false; 版本零不代替这个状态.
    bool ready_{};
    // 连续的完整 +1 提交, reset 后为空, 只在同一提交保护内访问.
    std::deque<Change> history_;
    // history_ 的逻辑字节计费总量, 初始零, 淘汰只减对应项实际计费.
    std::size_t history_bytes_{};
};
} // namespace astra
