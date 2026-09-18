// 此目标仅链接存储模块, 替换型 new 不进入服务或其他测试进程.
#include "check.hpp"
#include "store.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <semaphore>
#include <thread>
#include <utility>

namespace {
// 当前线程在下一次失败前还允许的分配次数; -1 表示关闭注入, 不干扰其他线程.
thread_local std::ptrdiff_t remaining = -1;
// 记录是否真实触发过注入, 区分受控失败与被测代码自身的其他异常.
thread_local bool injected = false;

// 仅测试线程下一次分配暂停, 用于证明快照构建期间写入和 TTL 无需等待状态锁.
struct Pause {
    // entered 通知分配已暂停, released 允许继续, 二者初始均无许可.
    std::binary_semaphore entered{0}, released{0};
    // resumed 初始 false, 保证显式恢复与清理恢复合计只归还一次许可.
    bool resumed{};

    // 幂等解除当前分配暂停, 不分配内存, 防止异常展开留下等待线程.
    void resume() noexcept {

        if (!std::exchange(resumed, true)) {
            released.release();
        }
    }
};

// pause_next 初始为空, 仅暂停当前线程下一次分配, 取得后立即清空.
thread_local Pause* pause_next{};

struct Watch;
// 只追踪当前测试线程的分配, 不将其他线程或服务的内存误计入桶回收检查.
thread_local Watch* allocation_watch{};

// 记录作用域内最大的成功分配及其释放, 避免以进程 RSS 或分配器是否归还 OS 来判断容器行为.
struct Watch {
    // 安装观测器; 此夹具只允许单层作用域, 不在作用域外保留地址或访问对象内容.
    Watch() {
        allocation_watch = this;
    }

    // 测试退出先移除钩子, 随后 Store 析构不再访问本对象.
    ~Watch() {
        allocation_watch = nullptr;
    }

    // 观测器不可复制, 避免多个对象争夺同一线程钩子.
    Watch(const Watch&) = delete;
    // 禁止覆盖仍安装在线程分配钩子中的观测器.
    Watch& operator=(const Watch&) = delete;
    // 只在预置数据阶段选择最大分配; 关闭后继续观察这个地址的释放.
    bool collecting{true};
    // largest 仅作分配身份比较, 不读取或释放其内容; released 表示已收到对应 delete.
    void* largest{};
    // size 为当前最大被观测分配的字节数, 初始零, 仅 collecting 时更新.
    std::size_t size{};
    // released 初始 false, 只在目标地址收到释放时置 true, 不依赖分配器是否归还 OS.
    bool released{};
};

// 分配 size 字节并返回普通对齐的非空地址, 由 release 回收; 失败抛 bad_alloc.
// 普通模式保留 new_handler 行为. 注入是一次性的, 之后允许异常处理和诊断分配内存.
[[gnu::noinline]] void* allocate(std::size_t size) {

    if (auto* pause = std::exchange(pause_next, nullptr)) {
        pause->entered.release();
        pause->released.acquire();
    }
    if (remaining == 0) {
        remaining = -1;
        injected = true;
        throw std::bad_alloc{};
    }
    if (remaining > 0) {
        --remaining;
    }
    for (;;) {
        // value 是 malloc 的结果; 零长度 new 仍返回可释放的非空地址.
        if (void* value = std::malloc(size ? size : 1)) {
            if (allocation_watch && allocation_watch->collecting && size > allocation_watch->size) {
                allocation_watch->largest = value;
                allocation_watch->size = size;
                allocation_watch->released = false;
            }
            return value;
        }

        // handler 是调用方设置的标准分配失败处理器, 只在真实 malloc 失败时调用.
        if (auto handler = std::get_new_handler()) {
            handler();
        } else {
            throw std::bad_alloc{};
        }
    }
}

// 回收 allocate 返回的 value, 空指针也合法; 无返回值且不抛异常.
// 独立调用避免优化器跨 new/free 包装误判测试的分配类型.
[[gnu::noinline]] void release(void* value) noexcept {

    if (value && allocation_watch && value == allocation_watch->largest) {
        allocation_watch->released = true;
        allocation_watch->largest = nullptr;
    }
    std::free(value);
}

// 只在被测操作期间启用故障. 任何异常路径均关闭注入, 不影响断言和 Store 析构.
class Failure {
public:
    // 在当前线程允许 point 次分配后注入一次失败, 清除上一次注入标志.
    explicit Failure(std::ptrdiff_t point) {
        remaining = point;
        injected = false;
    }

    // 任意退出路径都关闭注入, 后续断言和清理可以正常分配.
    ~Failure() {
        remaining = -1;
    }

    // 禁止复制注入作用域, 确保启用和关闭各发生一次.
    Failure(const Failure&) = delete;
    // 禁止赋值覆盖正在生效的注入责任.
    Failure& operator=(const Failure&) = delete;
};
} // namespace

// 标量 new: 转发 size 到受控分配器, 保留抛出式 new 的契约.
void* operator new(std::size_t size) {
    return allocate(size);
}

// 数组 new: 与标量共用注入计数, size 已由编译器包含数组所需空间.
void* operator new[](std::size_t size) {
    return allocate(size);
}

// 标量 delete: 回收 value, 不读取已经结束生命周期的对象内容.
void operator delete(void* value) noexcept {
    release(value);
}

// 数组 delete: 回收数组分配地址, 支持空指针.
void operator delete[](void* value) noexcept {
    release(value);
}

// sized 标量 delete: 大小参数不参与 free, 与普通标量 delete 使用同一分配域.
void operator delete(void* value, std::size_t) noexcept {
    release(value);
}

// sized 数组 delete: 大小参数不参与 free, 与普通数组 delete 使用同一分配域.
void operator delete[](void* value, std::size_t) noexcept {
    release(value);
}

using namespace astra;

namespace {
// 分别覆盖 Map 插入回滚, 原值替换, 删除和多 Key 过期提交.
enum class Operation {
    // 新键需要 Map 节点及可能的桶分配.
    insert,
    // 原键不能在历史分配成功前被覆盖.
    replace,
    // 删除必须和删除增量同时提交.
    remove,
    // 过期必须整批成功或整批保持原状, 错误必须可见.
    expire,
};

// 对 operation 逐点注入并返回实际失败次数; prefix 调整 deque 分段位置.
// 每个失败点重新创建 Store, 覆盖所有分配后必须成功, 否则断言或抛错.
std::size_t sweep(Operation operation, unsigned prefix) {

    // failures 统计真实注入次数, 不把无分配的空操作误报为覆盖成功.
    // failures 从零累计实际触发注入的点数, 至少一次失败后才接受最终成功.
    std::size_t failures = 0;
    // first 是已有长 Key, 超过短字符串优化长度, 强制覆盖字符串分配路径.
    const std::string first(80, 'a');
    // second 与 first 同时到期, 用于检测只删除半个批次的问题.
    const std::string second(80, 'b');
    // added 是新的长 Key, 触发 Map 节点及桶扩容的候选路径.
    const std::string added(80, 'c');
    // point 表示本次测试要失败的第几次分配, 上限只是测试防死循环预算.
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        // store 只保留两批历史, 成功提交同时覆盖历史淘汰路径.
        Store store(2, std::chrono::minutes(10), std::chrono::milliseconds(1), Clock::Time{});
        store.put(first, {1}, Clock::Time{});
        store.put(second, {1}, Clock::Time{});
        // i 仅推进预置提交位置, 不进入注入范围.
        for (unsigned i = 0; i < prefix; ++i) {
            store.put("seed", {1});
        }

        // before 保存操作前快照, 既用于版本检查也用于实际数据对照.
        // before 保留注入前视图, 任意准备分配失败后仍须复用这一完整状态.
        const auto before = store.snapshot();
        // failed 只接受可见的 bad_alloc, 被吞掉的异常由 injected 检查发现.
        bool failed = false;
        {
            // failure 把受控失败严格限制在单次被测操作中.
            Failure failure(point);
            try {
                switch (operation) {
                case Operation::insert:
                    store.put(added, {2});
                    break;
                case Operation::replace:
                    store.put(first, {2});
                    break;
                case Operation::remove:
                    store.remove(first);
                    break;
                case Operation::expire:
                    store.tick(Clock::Time{} + std::chrono::milliseconds(1));
                    break;
                }
            } catch (const std::bad_alloc&) {
                failed = true;
            }
        }
        if (!failed) {
            CHECK(!injected);
            CHECK(failures != 0);
            CHECK(store.version() == before->version + 1);
            return failures;
        }
        CHECK(injected);
        ++failures;
        CHECK(store.version() == before->version);
        // history 检查失败操作没有留下新增量或版本空洞.
        const auto history = store.extract(before->version);
        CHECK(!history.stale && history.deltas.empty());
        CHECK(store.snapshot() == before);

        // 再成功提交一个独立标记, 强制重建快照, 防止旧缓存遮蔽版本未变但 Map 已被修改的错误.
        store.put("probe", {3});
        // after 是强制重建后的实际 Map, 不允许旧快照缓存掩盖部分写入.
        const auto after = store.snapshot();
        CHECK(after->version == before->version + 1);
        CHECK(after->data.size() == before->data.size() + 1);
        // key/value 借用旧快照, 所有原值必须在失败后的新快照中保持一致.
        for (const auto& [key, value] : before->data) {
            CHECK(after->data.contains(key));
            CHECK(*after->data.at(key).value == *value.value);
            CHECK(after->data.at(key).deadline == value.deadline);
        }

        // delta 只能包含主动写入的 probe 标记, 不得出现失败操作的任何记录.
        const auto delta = store.extract(before->version);
        CHECK(!delta.stale && delta.deltas.size() == 1 && delta.deltas.front().key == "probe");
        // 失败不能取消原有租约. 下一次补拍必须同时删除原来的两个有限条目, probe 保持存活.
        store.tick(Clock::Time{} + std::chrono::milliseconds(3));
        CHECK(store.version() == before->version + 2);
        CHECK(store.snapshot()->data.size() == before->data.size() - 1 && store.snapshot()->data.contains("probe"));
        CHECK(!store.snapshot()->data.contains(first) && !store.snapshot()->data.contains(second));
    }
    throw std::runtime_error("Allocation sweep did not reach a successful operation");
}

// 对 snapshot=true 的快照构建或 false 的增量提取逐点注入, 返回失败次数.
// 只读操作可以抛出分配异常, 但必须保留已发布快照, 当前版本及全部历史.
std::size_t sweep_read(bool snapshot) {

    // failures 必须大于零, 确保查询路径确实发生了受控分配失败.
    // failures 从零累计实际触发注入的点数, 至少一次失败后才接受最终成功.
    std::size_t failures = 0;
    // key 使用长字符串, 同时覆盖返回结果的 Key 分配和数组/Map 分配.
    const std::string key(80, 'r');
    // point 逐个移动失败位置, 每次重新创建未缓存的新版本.
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        // store 默认容量足以保留两个写入批次.
        Store store;
        store.put(key, {1});
        // previous 在查询失败前已经交给读者, 必须始终保持第一版内容.
        const auto previous = store.snapshot();
        store.put(key, {2});
        // failed 记录查询是否向调用者传播注入的异常.
        bool failed = false;
        {
            // failure 在离开此作用域后关闭, 后续检查不会误触发下一次分配失败.
            Failure failure(point);
            try {
                if (snapshot) {
                    CHECK(store.snapshot()->data.at(key).value->front() == 2);
                } else {
                    CHECK(store.extract(0).deltas.size() == 2);
                }
            } catch (const std::bad_alloc&) {
                failed = true;
            }
        }
        CHECK(store.version() == 2);
        CHECK(previous->version == 1 && previous->data.at(key).value->front() == 1);
        CHECK(store.snapshot()->data.at(key).value->front() == 2);
        CHECK(store.extract(0).deltas.size() == 2);
        if (!failed) {
            CHECK(!injected && failures != 0);
            return failures;
        }
        CHECK(injected);
        ++failures;
    }
    throw std::runtime_error("Read allocation sweep did not reach success");
}

// 多于原先 reserve(64) 的到期批次逐点失败, 同时检查失败后可续租/取消和整批重试.
std::size_t sweep_large_expiry() {

    // failures 从零累计实际触发注入的点数, 至少一次失败后才接受最终成功.
    std::size_t failures = 0;
    for (std::ptrdiff_t point = 0; point < 512; ++point) {
        // store 为当前注入点独立构造, 使用显式历史预算, 拍宽和纪元起点.
        Store store(200, std::chrono::minutes(10), std::chrono::milliseconds(1), Clock::Time{});
        for (unsigned index = 0; index < 96; ++index) {
            store.put(std::string(80, 'k') + std::to_string(index), {1}, Clock::Time{});
        }

        // failed 初始 false, 只捕获预期 bad_alloc, 注入退出后再检查状态.
        bool failed = false;
        {
            Failure failure(point);
            try {
                store.tick(Clock::Time{} + std::chrono::milliseconds(1));
            } catch (const std::bad_alloc&) {
                failed = true;
            }
        }
        if (!failed) {
            CHECK(!injected && failures != 0 && store.version() == 97 && store.snapshot()->data.empty());
            return failures;
        }
        CHECK(injected && store.version() == 96 && store.snapshot()->data.size() == 96);
        ++failures;
        // 节点可能位于旧 ready_ 或重排槽中, 续租和取消必须同时覆盖两种位置.
        const auto renewed = std::string(80, 'k') + "0";
        store.put(renewed, {2});
        store.remove(std::string(80, 'k') + "1");
        store.tick(Clock::Time{} + std::chrono::milliseconds(4));
        CHECK(store.version() == 99 && store.snapshot()->data.size() == 1);
        CHECK(store.snapshot()->data.at(renewed).value->front() == 2);
        // changes 观察重试成功后的完整批次, 检查没有遗漏,重复或部分提交.
        const auto changes = store.extract(98);
        CHECK(changes.deltas.size() == 94);
    }
    throw std::runtime_error("Large TTL allocation sweep did not reach success");
}

// 跨多个到期拍的一次补齐仍只提交一个批次, 任意分配失败后都能完整恢复.
std::size_t sweep_catchup_expiry() {

    // failures 从零累计实际触发注入的点数, 至少一次失败后才接受最终成功.
    std::size_t failures = 0;
    // origin 为确定性的业务时间起点, 所有到期和历史观测由测试显式推进.
    const auto origin = Clock::Time{};
    // target 一次跨越 3000 个毫秒拍, 用于逐点注入跨多个到期时刻的提交失败.
    const auto target = origin + std::chrono::milliseconds(3000);
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        // store 为当前注入点独立构造, 使用显式历史预算, 拍宽和纪元起点.
        Store store(20, std::chrono::minutes(10), std::chrono::milliseconds(1), origin);
        for (unsigned index = 1; index <= 3; ++index) {
            store.put(std::string(80, 'b') + std::to_string(index), {1}, origin + std::chrono::milliseconds(index * 1000 - 1));
        }

        // before 保留注入前视图, 任意准备分配失败后仍须复用这一完整状态.
        const auto before = store.snapshot();
        // failed 初始 false, 只捕获预期 bad_alloc, 注入退出后再检查状态.
        bool failed = false;
        {
            Failure failure(point);
            try {
                store.tick(target);
            } catch (const std::bad_alloc&) {
                failed = true;
            }
        }
        if (failed) {
            CHECK(injected && store.version() == 3 && store.snapshot() == before);
            CHECK(store.extract(3).deltas.empty());
            ++failures;
        } else {
            CHECK(!injected && failures != 0 && store.version() == 4 && store.snapshot()->data.empty());
        }

        // 提交分配失败时逻辑轮可能已到 target, 重排项在下一拍重试, 不再依赖剩余拍计数.
        store.tick(target + std::chrono::milliseconds(1));
        CHECK(store.version() == 4 && store.snapshot()->data.empty());
        // changes 观察重试成功后的完整批次, 检查没有遗漏,重复或部分提交.
        const auto changes = store.extract(3);
        CHECK(!changes.stale && changes.deltas.size() == 3);
        for (unsigned index = 1; index <= 3; ++index) {
            // key 与预置长键逐字节相同, 用于逐项核对删除结果恰好出现一次.
            const auto key = std::string(80, 'b') + std::to_string(index);
            // count 从零统计当前键的删除记录, 最终必须为一, 不能漏删或重复.
            std::size_t count = 0;
            for (const auto& delta : changes.deltas) {
                CHECK(delta.deleted && !delta.value && delta.version == 4);
                count += delta.key == key;
            }
            CHECK(count == 1);
        }
        if (!failed) {
            return failures;
        }
    }
    throw std::runtime_error("Catch-up TTL allocation sweep did not reach success");
}

// 没有到期条目的补拍不分配数组, 即使禁止下一次 new 也能完成.
void test_empty_tick_allocation() {

    // store 为当前注入点独立构造, 使用显式历史预算, 拍宽和纪元起点.
    Store store(1000, std::chrono::minutes(10), std::chrono::milliseconds(1), Clock::Time{});
    store.put("permanent", {1});
    {
        Failure failure(0);
        store.tick(Clock::Time{} + std::chrono::seconds(3));
        store.tick(Clock::Time{} + std::chrono::seconds(3));
        CHECK(!injected);
    }
    CHECK(store.version() == 1);
}

// 保留读者时逐点破坏写时复制分配, 部分私有路径不能污染当前或旧视图.
void test_snapshot_page_failures() {

    for (const bool erase : {false, true}) {
        // completed 记录是否已经穷尽失败点并到达完整成功, 达到测试上限而未成功即失败.
        bool completed = false;
        // failures 从零累计实际触发注入的点数, 至少一次失败后才接受最终成功.
        std::size_t failures = 0;
        for (std::ptrdiff_t point = 0; point < 32; ++point) {
            Index index;
            // original 为旧视图的共享载荷, 准备页面失败不能改变其内容或引用身份.
            const auto original = std::make_shared<const Store::Buffer>(Store::Buffer{1});
            // changed 为待提交的新载荷, 仅完整成功后的目标槽能引用它.
            const auto changed = std::make_shared<const Store::Buffer>(Store::Buffer{2});
            for (std::uint64_t slot = 0; slot < 65; ++slot) {
                index.prepare(slot);
                index.set(slot, std::make_shared<const std::string>(std::to_string(slot)), {original, {}});
            }

            // previous 持有旧根以强制写时复制, 检查部分路径准备失败仍保持原视图.
            const auto previous = index.capture();
            // failed 初始 false, 只捕获预期 bad_alloc, 注入退出后再检查状态.
            bool failed = false;
            {
                Failure failure(point);
                try {
                    index.prepare(64);
                    if (erase) {
                        index.erase(64);
                    } else {
                        index.set(64, {}, {changed, {}});
                    }
                } catch (const std::bad_alloc&) {
                    failed = true;
                }
            }
            previous.each([&](const std::string&, const Index::Record& record) { CHECK(record.value == original); });
            CHECK(previous.size() == 65);
            // current 捕获注入后的实际索引, 根据失败与删除标志核对原子结果.
            const auto current = index.capture();
            CHECK(current.size() == (failed || !erase ? 65U : 64U));
            current.each([&](const std::string& key, const Index::Record& record) { CHECK(record.value == (!failed && key == "64" ? changed : original)); });
            if (!failed) {
                CHECK(!injected && failures != 0);
                completed = true;
                break;
            }
            CHECK(injected);
            ++failures;
        }
        CHECK(completed);
    }
}

// 将构建者停在首次分配处, 在另一线程完成覆盖, 删除和 TTL. fail 追加一次快照读取中的分配失败.
// 旧实现持锁 dump 时会明确失败而非死锁挂住; 两种退出路径都不能污染状态或挂住下次快照.
void test_snapshot_writer_progress(bool fail) {

    using namespace std::chrono_literals;
    // store 为当前注入点独立构造, 使用显式历史预算, 拍宽和纪元起点.
    Store store(100, 10min, 1ms, Clock::Time{});
    store.put("keep", {1});
    store.put("remove", {2});
    store.put("expire", {3}, Clock::Time{} + 1ms);
    Pause pause;
    // captured 仅由读取线程赋值, join 后由主线程检查是否返回完整旧版本.
    std::shared_ptr<const Store::Snapshot> captured;
    // reader_error/writer_error 分别只由对应线程写入, 两个线程 join 后再读取和传播.
    std::exception_ptr reader_error, writer_error;
    // reader 在本线程下一次分配处暂停, 借用的 store/pause 持续存活到 join.
    std::jthread reader([&] {
        try {
            pause_next = &pause;
            Failure failure(fail ? 2 : -1);
            captured = store.snapshot();
        } catch (...) {
            reader_error = std::current_exception();
        }
        pause_next = nullptr;
    });

    // 任何断言/线程创建失败都先解除暂停, 再由 jthread 析构等待, 不留下测试自造的永久阻塞.
    struct Unblock {
        // pause 借用外部暂停状态, 此恢复守卫必须早于 reader 析构以避免 join 永久等待.
        Pause& pause;

        // 任何异常路径先解除分配暂停, 再允许后声明的线程所有者退出.
        ~Unblock() {
            pause.resume();
        }
    } unblock{pause};

    CHECK(pause.entered.try_acquire_for(5s));
    // written 初始无许可, 写线程无论成功或异常都发布完成, 主线程有界等待.
    std::binary_semaphore written{0};
    // writer 在读快照暂停期间完成覆盖,删除,过期和新增, 检查没有全表快照状态锁梗阻.
    std::jthread writer([&] {
        try {
            store.put("keep", {4});
            store.remove("remove");
            store.tick(Clock::Time{} + 1ms);
            store.put("added", {5});
        } catch (...) {
            writer_error = std::current_exception();
        }
        written.release();
    });
    // progressed 表示写者在读者仍暂停时已完成, 不把释放暂停后的完成误报为并发进展.
    const bool progressed = written.try_acquire_for(5s);
    pause.resume();
    writer.join();
    reader.join();
    if (writer_error) {
        std::rethrow_exception(writer_error);
    }
    CHECK(progressed);
    if (fail) {
        CHECK(reader_error && !captured);
        try {
            std::rethrow_exception(reader_error);
        } catch (const std::bad_alloc&) {
            // 只接受预期的资源错误, 其他异常继续传播给测试入口.
        }
    } else {
        if (reader_error) {
            std::rethrow_exception(reader_error);
        }
        CHECK(captured && captured->version == 3 && captured->data.size() == 3);
        CHECK(captured->data.at("keep").value->front() == 1 && captured->data.contains("remove") && captured->data.contains("expire"));
    }

    // current 为写者完成后的新版本, 不应与暂停读者捕获的旧版本混淆.
    const auto current = store.snapshot();
    CHECK(current != captured && current->version == 7 && current->data.size() == 2);
    CHECK(current->data.at("keep").value->front() == 4 && current->data.at("added").value->front() == 5);
    CHECK(store.snapshot() == current);
}

// 大表清空后应交还桶分配, 留有一个有效项时不得收缩. 小表不反复重建, 回收不改变历史/快照语义.
void test_empty_bucket_reclamation() {

    using namespace std::chrono_literals;
    // count 区分小表与超过回收阈值的大表; leave_live 保留一个永不过期值用于排除非空表收缩.
    for (const std::size_t count : {1024U, 8192U}) {
        for (const bool leave_live : {false, true}) {
            // 零历史模式下删除即回收墓碑, 使桶释放与历史保存策略分开验证.
            Store store(0, 10min, 1ms, Clock::Time{});
            Watch watch;
            for (std::size_t key = 0; key < count; ++key) {
                store.put(std::to_string(key), {1}, leave_live && key == 0 ? std::optional<Clock::Time>{} : std::optional{Clock::Time{} + 1ms});
            }
            watch.collecting = false;
            // 固定短 Key/单值/零历史使大表的最大分配为桶数组, 页, 节点和单条记录远小于此尺寸.
            const bool large = count == 8192;
            CHECK(watch.largest && !watch.released && watch.size > 4096);
            CHECK(!large || watch.size > 4096 * sizeof(void*));
            store.tick(Clock::Time{} + 1ms);
            CHECK(watch.released == (large && !leave_live));
            if (leave_live) {
                CHECK(store.snapshot()->data.at("0").value->front() == 1);
                store.remove("0");
                CHECK(!watch.released);
            }

            // 空表的回收检查不再分配; 没有到期数据的维护不推进版本或使已有空快照失效.
            const auto version = store.version();
            // empty 保留空表缓存, 后续纯维护不应改变版本或让该快照失效.
            const auto empty = store.snapshot();
            {
                Failure failure(0);
                store.tick(Clock::Time{} + 2ms);
                CHECK(!injected);
            }
            CHECK(watch.released == large && store.version() == version && store.snapshot() == empty && empty->data.empty());
            CHECK(store.extract(version).deltas.empty() && store.extract(version - 1).stale);
            // 换表之后再次注册并到期, 验证查找, 分页槽复用和时间轮依然连贯.
            store.put("reborn", {2}, Clock::Time{} + 3ms);
            CHECK(store.snapshot()->data.at("reborn").value->front() == 2);
            store.tick(Clock::Time{} + 3ms);
            CHECK(store.version() == version + 2 && store.snapshot()->data.empty() && empty->data.empty());
        }
    }

    // 有效值为空不等于节点全空: 删除批次仍保留时, 不提前回收墓碑或改变续传下界.
    // origin 为确定性的业务时间起点, 所有到期和历史观测由测试显式推进.
    const auto origin = Clock::Time{};
    Store retained(1, 10min, 1s, origin);
    Watch watch;
    for (std::size_t key = 0; key < 8192; ++key) {
        retained.put(std::to_string(key), {1}, origin + 1s);
    }
    watch.collecting = false;
    CHECK(watch.largest && !watch.released && watch.size > 4096 * sizeof(void*));
    retained.tick(origin + 1s);
    CHECK(retained.snapshot()->data.empty() && !watch.released);
    // deletion 保留整批删除增量, 回收历史与桶之后外部结果仍须完整可读.
    const auto deletion = retained.extract(8192);
    CHECK(!deletion.stale && deletion.deltas.size() == 8192 && deletion.version == 8193);
    // 只推进约 660 个整秒拍, 历史超过保留期后才允许桶回收; 外部持有的删除结果继续有效.
    {
        Failure failure(0);
        retained.tick(origin + 11min, Steady::now() + 11min);
        CHECK(!injected);
    }
    CHECK(watch.released && retained.version() == 8193 && retained.extract(8192).stale && deletion.deltas.size() == 8192);
}

// 高槽删除后恢复低槽更新的叶页复制预算, 不暴露生产树高或依靠计时判断优化是否生效.
void test_snapshot_root_contraction() {

    // original 为旧视图的共享载荷, 准备页面失败不能改变其内容或引用身份.
    const auto original = std::make_shared<const Store::Buffer>(Store::Buffer{1});
    // changed 为待提交的新载荷, 仅完整成功后的目标槽能引用它.
    const auto changed = std::make_shared<const Store::Buffer>(Store::Buffer{2});
    // low_key 保持最低槽键, 高层删除后它仍存在, 用于验证真正缩根.
    const auto low_key = std::make_shared<const std::string>("low");
    // high_key 触发各层级根扩展, 随后删除以观察低槽写入的分配预算.
    const auto high_key = std::make_shared<const std::string>("high");
    // 分别跨越叶页, 二层根和最大树高; UINT64_MAX 保留为槽耗尽标志, 不作为有效分配结果.
    for (const auto high : std::array<std::uint64_t, 4>{64, 1024, std::uint64_t{1} << 62, UINT64_MAX - 1}) {
        Index index;
        index.prepare(0);
        index.set(0, low_key, {original, {}});
        index.prepare(high);
        index.set(high, high_key, {original, {}});
        // before 保留高低两个槽的旧根, 当前缩根不能改变这个共享视图.
        const auto before = index.capture();
        index.prepare(high);
        {
            Failure failure(0);
            index.erase(high);
            CHECK(!injected);
        }

        // contracted 捕获缩根后仅有低槽的视图, 再次扩展不能反向改变它.
        const auto contracted = index.capture();
        CHECK(before.size() == 2 && contracted.size() == 1 && index.next() == 1);
        {
            // 当前目标标准库的 make_shared 仅需一次叶页分配. 多余根链会耗尽此预算并使测试失败.
            Failure failure(1);
            index.prepare(0);
            index.set(0, {}, {changed, {}});
            CHECK(!injected);
        }
        before.each([&](const std::string& key, const Index::Record& record) { CHECK((key == "low" || key == "high") && record.value == original); });
        contracted.each([&](const std::string& key, const Index::Record& record) { CHECK(key == "low" && record.value == original); });
        index.capture().each([&](const std::string& key, const Index::Record& record) { CHECK(key == "low" && record.value == changed); });
        // 收缩后仍允许再次扩展, 已捕获的低树视图不随新根变化.
        index.prepare(high);
        index.set(high, high_key, {changed, {}});
        CHECK(index.capture().size() == 2 && contracted.size() == 1 && index.next() == 1);
    }
}

} // namespace

// 返回非零表示不变量或注入覆盖失败, 不把未触发分配失败的空测试记作成功.
int main() {

    try {
        // failures 累计所有修改操作及 deque 位置上的失败数.
        // failures 从零累计实际触发注入的点数, 至少一次失败后才接受最终成功.
        std::size_t failures = 0;
        // operation 遍历所有修改入口, prefix 遍历已选定的 deque 分段位置.
        for (const auto operation : {Operation::insert, Operation::replace, Operation::remove, Operation::expire}) {
            for (const unsigned prefix : {0U, 13U, 29U, 61U}) {
                failures += sweep(operation, prefix);
            }
        }

        // read_failures 另行统计查询分配失败, 不与写入原子性覆盖混淆.
        const auto read_failures = sweep_read(true) + sweep_read(false);
        failures += sweep_large_expiry();
        failures += sweep_catchup_expiry();
        test_empty_tick_allocation();
        test_snapshot_page_failures();
        test_snapshot_writer_progress(false);
        test_snapshot_writer_progress(true);
        test_empty_bucket_reclamation();
        test_snapshot_root_contraction();
        std::cout << "PASS store atomicity under " << failures << " write and " << read_failures << " read allocation failures\n";
        return 0;
    } catch (const std::exception& error) {
        // error 保留首个失败原因, 非零退出码使 CTest 明确失败.
        std::cerr << error.what() << '\n';
        return 1;
    }
}
