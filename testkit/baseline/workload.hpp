#pragma once
#include "../../astra/bench/measure.hpp"
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <thread>

// 两种 SDK 共用的应用负载, 不接触内部协议或绕过公开 API.
class Workload {
public:
    using Clock = Measure::Clock; // 进程内单调计时, 不比较两个服务的墙钟.

    // 配置在启动时完整验证; endpoint 只能由隔离测试运行器提供.
    struct Options {
        std::string endpoint;  // Redis 为单地址, Comet 为逗号分隔的固定 Star 地址列表, 不作为故障转移名单.
        bool catalog{};        // true 为动态数据, false 为服务注册.
        bool visible{};        // true 每次等待所有目标可见; false 只等待写回执,
                               // 结束后验收最终视图.
        std::size_t records{}; // 总记录数, 8..1024, 均匀分布到 groups.
        std::size_t groups{};  // 独立范围数, 1..8.
        std::size_t fanout{};  // 每个范围的独立订阅数, 1..32.
        std::size_t clients{}; // 生产者和消费者各自的共享 Client 数, 1..8.
        std::size_t writers{}; // 并发写者数, 1..32, 每个写者至多一次在途调用.
        std::size_t bytes{};   // 完整 Data 字节数, 16..4096, 包含记录编号及版本.
        std::size_t attr{};    // 固定 Attr 字节数, 16..16384.
        unsigned seconds{};    // 正式窗口秒数, 2..30.
        unsigned ttl{};        // 自动续租使用的 TTL 毫秒数, 1000..600000.
        unsigned rate{};       // 合计计划每秒写次数; 零表示闭环最大受理负载.
        unsigned poll{};       // 两次完整观察扫描之间的最小间隔, 100..10000 微秒.
        unsigned legacy{};     // 旧 Selector 的视图合并间隔毫秒, 默认零; 对照可设置 10, 不改变 Comet.
    };

    // 基线的固定路由, 每个 Client 只连接一台 Star; 消费侧错开一个节点, 不依赖 SDK 的端点选择策略.
    class Route {
    public:
        // 启动时解析一次; 多节点要求写者、客户端和每范围订阅覆盖全部节点, 不允许空跑的陪衬 Star.
        explicit Route(const Options& options) {

            // 每个 address 借用配置后立即复制, endpoints_ 独立持有; 空项及重复地址均拒绝.
            for (const auto part : options.endpoint | std::views::split(',')) {
                const std::string address(part.begin(), part.end()); // 一个固定目标, 不解析传输层地址格式.
                check(!address.empty() && std::ranges::find(endpoints_, address) == endpoints_.end(), "Empty or duplicate Star endpoint");
                endpoints_.push_back(address);
            }
            check(!endpoints_.empty(), "Missing Star endpoint");

            // 单节点只作诊断对照; 多节点保证所有生产端有记录, 每个 Scope 的 fanout 能覆盖全部节点.
            const auto count = endpoints_.size(); // 当前基线节点数, 由独占运行器创建.
            check(count <= 8 && options.clients >= count && options.clients % count == 0 && options.writers >= count && options.writers % count == 0 && options.records >= options.clients && options.records % options.clients == 0 && options.fanout >= count, "Workload does not cover every Star");
        }

        // 零基生产 Client 轮转固定节点, 对象生命周期内不会变更自身来源 Star.
        const std::string& producer(std::size_t client) const {
            return endpoints_[client % endpoints_.size()];
        }

        // 对应消费者错开一台; fanout 覆盖所有节点, 每条记录同时验证本地与远端视图.
        const std::string& consumer(std::size_t client) const {
            return endpoints_[(client % endpoints_.size() + 1) % endpoints_.size()];
        }

    private:
        std::vector<std::string> endpoints_; // 顺序等于运行器的 star-0..star-N, 仅在构造中写入.
    };

    // 严格解析十进制无符号参数, 拒绝负号、尾部垃圾和截断溢出.
    static unsigned number(std::string_view text) {
        return static_cast<unsigned>(Measure::number(text, 0, UINT32_MAX));
    }

    // 固定位置参数便于运行器原样记录, 关系约束防止空分组和写者争用同一记录.
    static Options parse(int count, char** arguments) {

        check(count == 16 || count == 17, "Expected endpoint domain mode records groups fanout clients writers bytes attr seconds ttl rate poll v1 [legacy_view_ms]");
        Options options; // 未验证配置仅存在于启动线程.
        options.endpoint = arguments[1];
        options.catalog = std::string_view(arguments[2]) == "catalog";
        options.visible = std::string_view(arguments[3]) == "visible";
        check(options.catalog || std::string_view(arguments[2]) == "ephemeris", "Invalid domain");
        check(options.visible || std::string_view(arguments[3]) == "receipt", "Invalid mode");
        options.records = number(arguments[4]);
        options.groups = number(arguments[5]);
        options.fanout = number(arguments[6]);
        options.clients = number(arguments[7]);
        options.writers = number(arguments[8]);
        options.bytes = number(arguments[9]);
        options.attr = number(arguments[10]);
        options.seconds = number(arguments[11]);
        options.ttl = number(arguments[12]);
        options.rate = number(arguments[13]);
        options.poll = number(arguments[14]);
        options.legacy = count == 17 ? number(arguments[16]) : 0;
        check(std::string_view(arguments[15]) == "v1", "Unsupported baseline contract");

        check(!options.endpoint.empty() && options.records >= 8 && options.records <= 1024 && options.groups >= 1 && options.groups <= 8 && options.fanout >= 1 && options.fanout <= 32 && options.clients >= 1 && options.clients <= 8 && options.writers >= 1 && options.writers <= 32 && options.bytes >= 16 && options.bytes <= 4096 && options.attr >= 16 && options.attr <= 16384 && options.seconds >= 2 && options.seconds <= 30 && options.ttl >= 1000 && options.ttl <= 600000 && options.rate <= 100000 && options.poll >= 100 && options.poll <= 10000, "Baseline limit exceeded");
        check(options.records % options.groups == 0 && options.records % options.writers == 0 && options.groups * options.fanout <= 128, "Unbalanced workload");
        check(options.legacy <= 1000, "Invalid legacy view interval");
        return options;
    }

    // 公共断言保留原因, 调用失败使整个样本无效, 绝不计作成功请求.
    static void check(bool value, std::string_view reason) {
        if (!value) {
            throw std::runtime_error(std::string(reason));
        }
    }

    // 字节串包含记录编号和递增版本, 防止仅验证计数而误认其他记录的更新.
    static std::vector<std::uint8_t> content(std::size_t bytes, std::uint64_t record, std::uint64_t version) {
        std::vector<std::uint8_t> value(bytes, 42); // 前十六字节在所有受测平台使用相同主机编码.
        std::memcpy(value.data(), &record, sizeof(record));
        std::memcpy(value.data() + sizeof(record), &version, sizeof(version));
        return value;
    }

    // 使用同一负载执行器驱动两个适配器. Adapter
    // 负责初始化、完整写入、公开视图读取和清理.
    template <class Adapter>
    static void run(const Options& options) {

        std::cerr << "phase=initialize\n";                                               // 阶段证据用于区分建立基线失败和负载期间故障, 不计入延迟.
        Adapter adapter(options);                                                        // SDK 对象先构造, 所有采样线程退出之后才销毁.
        Workload work(options);                                                          // 观察矩阵由单个采样线程写, 写者只读取自己的记录行.
        std::jthread sampler([&](std::stop_token stop) { work.sample(adapter, stop); }); // 有界退出, 不让异常逃逸线程.
        work.until([&] { return work.complete(); });                                     // 每个订阅必须先观察到完整初始记录.
        std::cerr << "phase=measuring\n";
        const auto begin = Clock::now() + std::chrono::milliseconds(100); // 统一发令, 初始化时间不混入吞吐.
        const auto end = begin + std::chrono::seconds(options.seconds);   // 停止新写入, 最后一次调用允许有界排空.
        std::vector<std::jthread> writers;                                // 每个线程拥有互不重叠的记录序列.
        for (std::size_t writer = 0; writer < options.writers; ++writer) {
            writers.emplace_back([&, writer] {
                try {
                    std::vector<std::int64_t> receipts, visibility; // 线程本地样本, 测量中不争抢全局统计锁.
                    receipts.reserve(32768);
                    visibility.reserve(32768);
                    std::this_thread::sleep_until(begin);
                    for (std::uint64_t operation = 0; !work.failed_.load() && (options.rate ? operation * options.writers + writer < static_cast<std::uint64_t>(options.rate) * options.seconds : Clock::now() < end); ++operation) {
                        const auto planned = options.rate ? begin + std::chrono::nanoseconds(static_cast<std::int64_t>((operation * options.writers + writer) * 1000000000ULL / options.rate)) : Clock::now(); // 固定速率包含排队, 不制造协调遗漏.
                        std::this_thread::sleep_until(planned);
                        check(Clock::now() < end + std::chrono::seconds(30), "Writer backlog timeout");
                        const auto record = writer + static_cast<std::size_t>(operation % (options.records / options.writers)) * options.writers; // 轮转所有记录, 背景记录也进入热路径.
                        const auto version = ++work.versions_[record];                                                                            // 同一记录仅由该写者操作, 初始一.
                        adapter.write(record, version, content(options.bytes, record, version));
                        receipts.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - planned).count());
                        if (options.visible) {
                            work.until([&] { return work.visible(record, version); });
                            visibility.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - planned).count());
                        }
                    }
                    const std::lock_guard lock(work.gate_); // 退出时才合并, 不改变逐次请求延迟.
                    work.receipts_.insert(work.receipts_.end(), receipts.begin(), receipts.end());
                    work.visibility_.insert(work.visibility_.end(), visibility.begin(), visibility.end());
                } catch (...) {
                    work.fail();
                }
            });
        }
        writers.clear();                                                                  // 完成所有写者之后读取最终版本, 不与其递增产生数据竞争.
        const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count(); // 写入窗口含末次回执, 不含最终一致验收.
        work.until([&] { return work.complete(); });
        sampler.request_stop();
        sampler.join();
        work.rethrow();
        work.verify(adapter); // 停止采样后重新读取当前视图, 不将过去曾见过的记录误认作仍然存在.
        adapter.verify();     // 检查 SDK 异步诊断, 不能只看业务调用是否返回.

        check(!work.receipts_.empty(), "Empty measurement");
        const auto count = work.receipts_.size(); // 仅在所有调用及最终视图通过后输出结果.
        Measure::report("commit", std::move(work.receipts_), elapsed, count);
        if (options.visible) {
            Measure::report("visible", std::move(work.visibility_), elapsed, count);
        }
        std::cout << "{\"metric\":\"sampler\",\"scans\":" << work.scans_ << ",\"busy_seconds\":" << work.busy_ << ",\"poll_us\":" << options.poll << ",\"errors\":0,\"final_converged\":true}" << std::endl;
    }

private:
    const Options& options_;                             // 借用 run 的配置, 覆盖线程完整生命周期.
    std::unique_ptr<std::atomic<std::uint64_t>[]> seen_; // records * fanout, 零代表尚未收到初始记录.
    std::vector<std::uint64_t> versions_;                // 每个写者独占自己的元素, 初始版本一.
    std::mutex gate_;                                    // 失败和退出统计的低频锁.
    std::exception_ptr failure_;                         // 首次失败保留, 主线程统一重抛.
    std::atomic<bool> failed_{};                         // 中止其他有界工作, 不隐藏原始异常.
    std::vector<std::int64_t> receipts_, visibility_;    // 单位纳秒, 线程退出时合并.
    std::uint64_t scans_{};                              // 完整扫描次数, 包含预热与最终收敛.
    double busy_{};                                      // 扫描实际墙钟秒数, 不伪称线程 CPU 时间.

    // 构造零初始化观察矩阵; 不按吞吐动态分配确认槽位.
    explicit Workload(const Options& options) : options_(options), seen_(std::make_unique<std::atomic<std::uint64_t>[]>(options.records * options.fanout)), versions_(options.records, 1) {}

    // 同一组的每个订阅都必须观察到对应版本, 单个成功订阅不足以通过.
    bool visible(std::size_t record, std::uint64_t version) const {
        for (std::size_t watcher = 0; watcher < options_.fanout; ++watcher) {
            if (seen_[record * options_.fanout + watcher].load(std::memory_order_relaxed) < version) {
                return false;
            }
        }
        return true;
    }

    // 仅用于写者启动前和全部 join 后, 此时 versions_ 没有并发写入.
    bool complete() const {
        for (std::size_t record = 0; record < options_.records; ++record) {
            if (!visible(record, versions_[record])) {
                return false;
            }
        }
        return true;
    }

    // 有界等待不主动重发写入, 失败立即交给主线程.
    void until(auto&& predicate) {
        const auto deadline = Clock::now() + std::chrono::seconds(30); // 每个阶段最大等待预算.
        while (!predicate()) {
            rethrow();
            check(Clock::now() < deadline, "View convergence timeout");
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    // 线程 catch 路径保存首错, 其余线程观察停止标志后有界退出.
    void fail() {
        const std::lock_guard lock(gate_);
        if (!failure_) {
            failure_ = std::current_exception();
        }
        failed_.store(true);
    }

    // 只在失败时取低频锁, 正常轮询不竞争统计锁.
    void rethrow() {
        if (failed_.load()) {
            const std::lock_guard lock(gate_);
            std::rethrow_exception(failure_);
        }
    }

    // 正式窗口外逐个核验最终当前视图, 同时确认完整性、路由、最新版本和正文, 不复用 seen_ 的历史水线.
    template <class Adapter>
    void verify(Adapter& adapter) const {

        std::vector<bool> present(options_.records * options_.fanout); // 每个记录在各订阅中的最终存在位, 初始全部为 false.
        for (std::size_t watcher = 0; watcher < options_.groups * options_.fanout; ++watcher) {
            adapter.scan(watcher, [&](std::span<const std::uint8_t> value) {
                check(value.size() == options_.bytes, "Final payload length mismatch");
                std::uint64_t record{}, version{}; // 读取完整最终记录, 不依赖先前采样中的字段.
                std::memcpy(&record, value.data(), sizeof(record));
                std::memcpy(&version, value.data() + sizeof(record), sizeof(version));
                check(record < options_.records && record % options_.groups == watcher / options_.fanout, "Final routing mismatch");
                check(version == versions_[record] && std::ranges::all_of(value.subspan(16), [](auto byte) { return byte == 42; }), "Final payload mismatch");
                const auto slot = record * options_.fanout + watcher % options_.fanout; // 该订阅内不能出现重复逻辑记录.
                check(!present[slot], "Duplicate final record");
                present[slot] = true;
            });
        }

        check(std::ranges::all_of(present, [](bool value) { return value; }), "Final view lost a record");
    }

    // 统一轮询公开视图, 每轮间隔一致; 慢扫描如实进入可见延迟和 busy_seconds.
    template <class Adapter>
    void sample(Adapter& adapter, std::stop_token stop) {
        try {
            while (!stop.stop_requested() && !failed_.load()) {
                const auto begin = Clock::now(); // 每轮完整遍历的开始时刻.
                for (std::size_t watcher = 0; watcher < options_.groups * options_.fanout; ++watcher) {
                    adapter.scan(watcher, [&](std::span<const std::uint8_t> value) {
                        check(value.size() == options_.bytes, "Invalid visible payload length");
                        std::uint64_t record{}, version{}; // memcpy 避免未对齐访问, 不强转底层指针.
                        std::memcpy(&record, value.data(), sizeof(record));
                        std::memcpy(&version, value.data() + sizeof(record), sizeof(version));
                        check(record < options_.records && record % options_.groups == watcher / options_.fanout && version > 0, "Wrong subscription routing");
                        auto& cell = seen_[record * options_.fanout + watcher % options_.fanout]; // 每个槽位仅该采样线程更新.
                        const auto previous = cell.load(std::memory_order_relaxed);
                        check(version >= previous, "Visible version regressed");
                        if (version != previous) {
                            check(std::ranges::all_of(value.subspan(16), [](auto byte) { return byte == 42; }), "Visible payload corrupted");
                            cell.store(version, std::memory_order_relaxed);
                        }
                    });
                }
                ++scans_;
                busy_ += std::chrono::duration<double>(Clock::now() - begin).count();
                std::this_thread::sleep_until(begin + std::chrono::microseconds(options_.poll));
            }
        } catch (...) {
            fail();
        }
    }
};
