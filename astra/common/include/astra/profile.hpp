#pragma once

// 诊断开关由 CMake 统一传播. 关闭时连参数也不求值, 不引入计时器、符号、TLS 或运行期分支.
#if defined(ASTRA_PROFILE) && ASTRA_PROFILE
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <new>
#include <string_view>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace astra {
// 仅诊断构建使用. 记录墙钟跨度而不是凭函数名猜测 CPU 时间; 不采集正文、Key 或凭据.
class Profile {
    // 文件使用当前 Linux 小端 64 位 ABI, 固定宽度布局由离线解析器再次验证.
    struct Entry {
        std::uint64_t start;     // 单调起点, 纳秒; 可在同一宿主的进程间对齐.
        std::uint64_t elapsed;   // 包含已插桩子调用的墙钟耗时, 纳秒.
        std::uint64_t own;       // 扣除同步子探针区间后的墙钟耗时, 仍含未插桩工作与抢占.
        std::uint64_t cpu;       // 可选线程 CPU 纳秒, 未启用时为零.
        std::uint64_t exclusive; // 扣除同步子调用的 CPU 纳秒, 未启用时为零.
        std::uint64_t id;        // 本进程唯一跨度号, 从 1 开始; 计数事件为零.
        std::uint64_t parent;    // 同线程父跨度号, 顶层为零; 不虚构跨异步调用的父子关系.
        std::uint64_t site;      // 固定探针名称的 FNV-1a 标识, 由源码目录表解析并检查碰撞.
        std::uint64_t value;     // 计数事件的非敏感数值; 普通跨度为零.
        std::uint32_t thread;    // 内核线程号, 用于时间线和父子关系校验.
        std::uint32_t flags;     // 位 0 为启用 CPU 计时, 位 1 为采样计数事件, 位 2 为独立异步间隔.
    };

    struct Header {
        std::uint64_t magic = 0x4153545241505246ULL; // 固定格式标识, 不与既有临时探针混用.
        std::uint64_t version = 1;                   // 二进制布局版本.
        std::uint64_t width = sizeof(Entry);         // 一条记录的字节数.
        std::uint64_t capacity{};                    // 文件可容纳的记录数, 不在运行中扩容.
        std::atomic_uint64_t count{};                // 按 64 槽分块预留的水位, 包含线程未使用的零槽.
        std::atomic_uint64_t missed{};               // 满额后直接拒绝的探针数, 不包含随父作用域停采的子树.
        std::atomic_uint64_t errors{};               // 时钟异常/不合法嵌套计数, 非零不得宣称完整剖析.
        std::uint64_t sample{};                      // 顶层跨度以约 1/N 概率采一棵完整同步调用树.
        std::uint64_t cpu{};                         // 是否启用额外的线程 CPU 时钟系统调用.
        std::array<std::uint64_t, 7> reserved{};     // 固定 128 字节头, 后续扩展须提升版本.
    };

    static_assert(sizeof(Header) == 128 && sizeof(Entry) == 80);
    static_assert(std::atomic_uint64_t::is_always_lock_free);

    class Storage {
    public:
        Header* header{}; // 无目录或创建失败时为空, 诊断禁用但不改变业务执行结果.
        Entry* entries{}; // 文件映射中的固定记录区, 进程退出后才由分析器读取.

        // 读取诊断环境. 目录必须预先创建且为绝对路径; 不创建线程、不下载、不输出配置值.
        Storage() noexcept {

            const char* directory = std::getenv("ASTRA_PROFILE_DIR"); // 缺省不创建文件.
            if (!directory)
                return;
            const auto sample = number("ASTRA_PROFILE_SAMPLE", 64, 1, 65536); // 顶层采样间隔, 默认 64.
            const auto mib = number("ASTRA_PROFILE_MIB", 64, 1, 512);         // 每进程映射预算, 默认 64 MiB.
            const auto cpu = number("ASTRA_PROFILE_CPU", 0, 0, 1);            // CPU 系统调用需单独开启并测量扰动.
            if (directory[0] != '/' || sample == UINT64_MAX || mib == UINT64_MAX || cpu == UINT64_MAX) {
                failure();
                return;
            }

            char path[4096]; // 路径只用于打开文件, 不写入性能记录.
            const auto length = std::snprintf(path, sizeof(path), "%s/%ld.profile", directory, static_cast<long>(::getpid()));
            if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
                failure();
                return;
            }
            const auto bytes = static_cast<std::size_t>(mib) * 1024 * 1024;             // 上限 512 MiB, 已检查乘法范围.
            const int file = ::open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600); // 不覆盖旧证据.
            if (file < 0) {
                failure();
                return;
            }
            if (::ftruncate(file, static_cast<off_t>(bytes)) != 0 || ::posix_fallocate(file, 0, static_cast<off_t>(bytes)) != 0) {
                ::close(file);
                failure();
                return;
            }
            void* memory = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0); // 已预留磁盘, 仍有初触页/内核回写开销, 不同步刷盘.
            ::close(file);
            if (memory == MAP_FAILED) {
                failure();
                return;
            }
            header = new (memory) Header;
            header->capacity = (bytes - sizeof(Header)) / sizeof(Entry) / 64 * 64;
            header->sample = sample;
            header->cpu = cpu;
            entries = reinterpret_cast<Entry*>(static_cast<char*>(memory) + sizeof(Header));
        }

        // 解析不带符号的十进制配置. 缺省用 fallback, 空值、越界和非数字返回 UINT64_MAX.
        static std::uint64_t number(const char* name, std::uint64_t fallback, std::uint64_t minimum, std::uint64_t maximum) noexcept {

            const char* text = std::getenv(name); // 仅同步借用进程环境, 运行中不支持修改配置.
            if (!text)
                return fallback;
            if (!*text)
                return UINT64_MAX;
            std::uint64_t value{}; // 逐位累加, 在乘法前检查上界.
            for (; *text; ++text) {
                if (*text < '0' || *text > '9')
                    return UINT64_MAX;
                const auto digit = static_cast<std::uint64_t>(*text - '0');
                if (digit > maximum || value > (maximum - digit) / 10)
                    return UINT64_MAX;
                value = value * 10 + digit;
            }
            return value >= minimum ? value : UINT64_MAX;
        }

        // 初始化失败只打印固定诊断. 测试驱动必须将缺文件或坏文件判为测量失败.
        static void failure() noexcept {
            std::fputs("Astra profiling initialization failed\n", stderr);
        }

        // 每线程独占 64 槽, 只在换块时竞争一次原子计数. 块边界按 64 字节对齐, 避免写记录时伪共享.
        std::uint64_t reserve() noexcept {

            struct Block {
                std::uint64_t next{}; // 下一槽编号, 未分块时为零.
                std::uint64_t end{};  // 独占块尾, 不包含此槽.
                bool full{};          // 容量耗尽后不再分块, 避免水位无限增长.
            };

            thread_local Block block; // 与进程内唯一 Storage 配套, 不支持 fork 后继承映射继续采集.
            if (block.next == block.end && !block.full) {
                block.next = header->count.fetch_add(64, std::memory_order_relaxed);
                block.end = block.next + 64;
                block.full = block.next >= header->capacity;
            }

            if (block.full) {
                header->missed.fetch_add(1, std::memory_order_relaxed);
                return UINT64_MAX;
            }
            return block.next++;
        }

        // 在独占槽建立平凡记录的对象寿命, 无堆分配. 读取必须晚于进程退出, 不提供在线发布协议.
        void write(std::uint64_t slot, const Entry& entry) noexcept {
            ::new (static_cast<void*>(entries + slot)) Entry(entry);
        }
    };

    // 只对已启用的诊断使用进程级固定映射, 不在普通构建中存在此对象.
    static Storage& storage() noexcept {
        static Storage result;
        return result;
    }

    // 单调纳秒仅测量跨度, 不参与业务时钟或租约.
    static std::uint64_t now() noexcept {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // 所有事件共用线程号缓存, 每个线程最多一次系统调用.
    static std::uint32_t thread() noexcept {
        thread_local const auto value = static_cast<std::uint32_t>(::syscall(SYS_gettid));
        return value;
    }

    // 当前线程实际消耗的 CPU 纳秒. 失败显式记入文件头, 不把失败冒充零开销.
    static std::uint64_t cpu(Storage& output) noexcept {
        timespec time{}; // 由内核写入秒/纳秒.
        if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
            output.header->errors.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(time.tv_nsec);
    }

    // 编译期计算固定名称的标识, 不对业务数据计算散列.
    static constexpr std::uint64_t identify(std::string_view name) noexcept {
        std::uint64_t value = 14695981039346656037ULL;
        for (const char item : name) {
            value ^= static_cast<unsigned char>(item);
            value *= 1099511628211ULL;
        }
        return value;
    }

public:
    // 同线程同步作用域的 RAII 跨度. 必须按栈顺序结束, 不跨协程挂起或把对象移到另一线程.
    class Span {
        inline static thread_local Span* current_{};        // 包括未采样父作用域, 防止子调用自行重复抽样.
        inline static thread_local std::uint32_t random_{}; // 每线程独立的非密码学采样状态, 零为尚未初始化.
        Span* parent_{};                                    // 当前线程的上一作用域, 默认无父.
        Storage* output_{};                                 // 空表示未启用记录; 不拥有映射.
        std::uint64_t site_{};                              // 固定名称标识.
        std::uint64_t id_{};                                // 零为未采样, 非零为本进程唯一编号.
        std::uint64_t start_{};                             // 单调起点纳秒.
        std::uint64_t cpu_{};                               // 可选 CPU 起点纳秒.
        std::uint64_t children_{};                          // 已完成直接子跨度的墙钟总和.
        std::uint64_t consumed_{};                          // 已完成直接子跨度的 CPU 总和.
        bool open_{};                                       // 仅已进入线程栈的作用域为 true, finish 后归零.

    public:
        // site 为固定文字的编译期散列; 无凭据、地址或正文.
        explicit Span(std::uint64_t site) noexcept : site_(site) {

            auto& output = storage(); // 首次打开映射在计时起点之前完成.
            if (!output.header)
                return;
            parent_ = current_;
            // GCC 16 的内联逃逸检查不识别 finish/析构恢复 TLS 的借用边界. 仅此赋值抑制误报,
            // 不禁止移动/复制之外的生命周期检查; 嵌套、提前结束及异常展开均由独立用例覆盖.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
            current_ = this;
#pragma GCC diagnostic pop
            open_ = true;
            output_ = &output;
            if (parent_ ? parent_->id_ == 0 : !select(output.header->sample))
                return;
            const auto slot = output.reserve(); // 提前预留本线程槽位, 结束时直接写入, 无逐记录原子竞争.
            if (slot == UINT64_MAX)
                return;
            id_ = slot + 1;
            if (output.header->cpu)
                cpu_ = cpu(output);
            start_ = now();
        }

        // 异常退出同样关闭跨度. 不能复制或移动, 避免破坏 TLS 栈与重复记录.
        ~Span() {
            finish();
        }

        Span(const Span&) = delete;
        Span& operator=(const Span&) = delete;

        // 提前结束用于锁获取等局部阶段; 其后的析构无动作, 不修改被测锁或其生命周期.
        void finish() noexcept {

            if (!open_)
                return;
            if (current_ != this) {
                output_->header->errors.fetch_add(1, std::memory_order_relaxed);
                std::abort(); // 仅内部探针误用, 不允许悬垂 TLS 父链继续执行.
            }
            current_ = parent_;
            open_ = false;
            if (!id_)
                return;
            const auto end = now(); // 先结束墙钟, 记录动作本身不计入本跨度.
            const auto elapsed = end >= start_ ? end - start_ : 0;
            const auto used = output_->header->cpu ? cpu(*output_) : 0;
            const auto consumed = used >= cpu_ ? used - cpu_ : 0;
            if (end < start_ || children_ > elapsed || used < cpu_ || consumed_ > consumed)
                output_->header->errors.fetch_add(1, std::memory_order_relaxed);
            if (parent_) {
                parent_->children_ += elapsed;
                parent_->consumed_ += consumed;
            }
            output_->write(id_ - 1, {start_, elapsed, elapsed >= children_ ? elapsed - children_ : 0, consumed, consumed >= consumed_ ? consumed - consumed_ : 0, id_, parent_ ? parent_->id_ : 0, site_, 0, thread(), static_cast<std::uint32_t>(output_->header->cpu)});
        }

        // 固定名称只在编译期处理, 不在热路径格式化字符串.
        static consteval std::uint64_t label(std::string_view name) noexcept {
            return identify(name);
        }

        // 异步标记沿用入口的采样决定, 不把 TLS 父对象传递给另一线程.
        static bool sampled() noexcept {
            return current_ && current_->id_;
        }

    private:
        // 固定周期取模会与 Runtime 的等待/工作交替相位重合, 永久漏采其中一种调用.
        // xorshift32 只在顶层执行三组移位/异或, 无分配、共享原子或随机设备调用.
        // 非零状态周期为 2^32-1; 取模存在可忽略的采样偏差, 不用于安全或精确总量外推.
        static bool select(std::uint64_t interval) noexcept {

            if (interval == 1)
                return true;
            if (!random_) {
                random_ = thread() ^ 0x9e3779b9U; // 不同线程错开起点, 极端异或为零时改用非零常量.
                if (!random_)
                    random_ = 1;
            }

            random_ ^= random_ << 13;
            random_ ^= random_ >> 17;
            random_ ^= random_ << 5;
            return random_ % interval == 0;
        }

    public:
        // 计数只随当前被采样调用树记录, value 为已有的字节数/条数/命中次数, 不是总量估算.
        static void count(std::uint64_t site, std::uint64_t value) noexcept {
            if (!current_ || !current_->id_)
                return;
            auto& output = *current_->output_;
            const auto slot = output.reserve(); // 计数事件也使用线程独占槽位, 满额明确报告而不分配额外内存.
            if (slot == UINT64_MAX)
                return;
            output.write(slot, {now(), 0, 0, 0, 0, 0, current_->id_, site, value, thread(), 2});
        }
    };

    // 一次完成到一次消费的诊断时间戳. 调用者使用既有业务锁保证 mark/consume 成对, 不另加原子竞争.
    class Stamp {
        std::uint64_t start_{}; // 零为本次未采样或已消费; 不构成业务状态.

    public:
        // 随入口同步跨度的采样决定记录. 每个新事件覆盖上次已完成标记, 不累积历史或分配内存.
        void mark() noexcept {
            start_ = Span::sampled() ? now() : 0;
        }

        // 可由另一线程消费, 但必须与 mark 使用调用者已有锁/发布边界. 间隔不算作本线程 CPU 或同步子调用.
        void consume(std::uint64_t site) noexcept {

            if (!start_)
                return;
            const auto start = start_; // 先清除, 同一事件不重复记录.
            start_ = 0;
            const auto end = now();
            auto& output = storage();
            if (end < start) {
                output.header->errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto slot = output.reserve();
            if (slot == UINT64_MAX)
                return;
            output.write(slot, {start, end - start, end - start, 0, 0, 0, 0, site, 0, thread(), 4});
        }
    };
};
} // namespace astra

// 唯一变量名只在预处理阶段拼接; 关闭时所有参数不求值, 不增加业务 ABI.
#define ASTRA_PROFILE_JOIN_INNER(left, right) left##right
#define ASTRA_PROFILE_JOIN(left, right) ASTRA_PROFILE_JOIN_INNER(left, right)
#define ASTRA_PROFILE_SCOPE(name)                                         \
    const ::astra::Profile::Span ASTRA_PROFILE_JOIN(profile_, __LINE__) { \
        ::astra::Profile::Span::label(name)                               \
    }
#define ASTRA_PROFILE_BEGIN(token, name)    \
    ::astra::Profile::Span token {          \
        ::astra::Profile::Span::label(name) \
    }
#define ASTRA_PROFILE_END(token) token.finish()
#define ASTRA_PROFILE_COUNT(name, value) ::astra::Profile::Span::count(::astra::Profile::Span::label(name), static_cast<std::uint64_t>(value))
#define ASTRA_PROFILE_STAMP(token) ::astra::Profile::Stamp token
#define ASTRA_PROFILE_MARK(token) token.mark()
#define ASTRA_PROFILE_CONSUME(token, name) token.consume(::astra::Profile::Span::label(name))
#else
#define ASTRA_PROFILE_SCOPE(name) ((void)0)
#define ASTRA_PROFILE_BEGIN(token, name) ((void)0)
#define ASTRA_PROFILE_END(token) ((void)0)
#define ASTRA_PROFILE_COUNT(name, value) ((void)0)
#define ASTRA_PROFILE_STAMP(token) static_assert(true)
#define ASTRA_PROFILE_MARK(token) ((void)0)
#define ASTRA_PROFILE_CONSUME(token, name) ((void)0)
#endif
