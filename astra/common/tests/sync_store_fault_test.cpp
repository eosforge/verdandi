// 功能: 在独立进程逐个注入分配失败, 验证状态, 历史, 快照和版本没有部分提交.
// 此目标仅链接存储模块, 替换型 new 不进入服务或其他测试进程.
#include "check.hpp"
#include "sync_store.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
// 当前线程在下一次失败前还允许的分配次数; -1 表示关闭注入, 不干扰其他线程.
thread_local std::ptrdiff_t remaining = -1;
// 记录是否真实触发过注入, 区分受控失败与被测代码自身的其他异常.
thread_local bool injected = false;

// 分配 size 字节并返回普通对齐的非空地址, 由 release 回收; 失败抛 bad_alloc.
// 普通模式保留 new_handler 行为. 注入是一次性的, 之后允许异常处理和诊断分配内存.
[[gnu::noinline]] void* allocate(std::size_t size) {
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
    std::free(value);
}

// 只在被测操作期间启用故障. 任何异常路径均关闭注入, 不影响断言和 Store 析构.
class FailureScope {
public:
    // 在当前线程允许 point 次分配后注入一次失败, 清除上一次注入标志.
    explicit FailureScope(std::ptrdiff_t point) {
        remaining = point;
        injected = false;
    }
    // 任意退出路径都关闭注入, 后续断言和清理可以正常分配.
    ~FailureScope() {
        remaining = -1;
    }
    // 禁止复制注入作用域, 确保启用和关闭各发生一次.
    FailureScope(const FailureScope&) = delete;
    // 禁止赋值覆盖正在生效的注入责任.
    FailureScope& operator=(const FailureScope&) = delete;
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
        SyncStore store(2);
        store.put(first, {1}, Clock::time_point{});
        store.put(second, {1}, Clock::time_point{});
        // i 仅推进预置提交位置, 不进入注入范围.
        for (unsigned i = 0; i < prefix; ++i) {
            store.put("seed", {1});
        }
        // before 保存操作前快照, 既用于版本检查也用于实际数据对照.
        const auto before = store.get_snapshot();
        // failed 只接受可见的 bad_alloc, 被吞掉的异常由 injected 检查发现.
        bool failed = false;
        {
            // failure 把受控失败严格限制在单次被测操作中.
            FailureScope failure(point);
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
                    store.evict_expired(Clock::now());
                    break;
                }
            } catch (const std::bad_alloc&) {
                failed = true;
            }
        }
        if (!failed) {
            CHECK(!injected);
            CHECK(failures != 0);
            CHECK(store.global_version() == before->global_version + 1);
            return failures;
        }
        CHECK(injected);
        ++failures;
        CHECK(store.global_version() == before->global_version);
        // history 检查失败操作没有留下新增量或版本空洞.
        const auto history = store.extract_since(before->global_version);
        CHECK(!history.require_snapshot && history.deltas.empty());
        CHECK(store.get_snapshot() == before);

        // 再成功提交一个独立标记, 强制重建快照, 防止旧缓存遮蔽版本未变但 Map 已被修改的错误.
        store.put("probe", {3});
        // after 是强制重建后的实际 Map, 不允许旧快照缓存掩盖部分写入.
        const auto after = store.get_snapshot();
        CHECK(after->global_version == before->global_version + 1);
        CHECK(after->data.size() == before->data.size() + 1);
        // key/payload 借用旧快照, 所有原值必须在失败后的新快照中保持一致.
        for (const auto& [key, payload] : before->data) {
            CHECK(after->data.contains(key));
            CHECK(*after->data.at(key) == *payload);
        }
        // delta 只能包含主动写入的 probe 标记, 不得出现失败操作的任何记录.
        const auto delta = store.extract_since(before->global_version);
        CHECK(!delta.require_snapshot && delta.deltas.size() == 1 && delta.deltas.front().key == "probe");
    }
    throw std::runtime_error("Allocation sweep did not reach a successful operation");
}

// 对 snapshot=true 的快照构建或 false 的增量提取逐点注入, 返回失败次数.
// 只读操作可以抛出分配异常, 但必须保留已发布快照、当前版本及全部历史.
std::size_t sweep_read(bool snapshot) {
    // failures 必须大于零, 确保查询路径确实发生了受控分配失败.
    std::size_t failures = 0;
    // key 使用长字符串, 同时覆盖返回结果的 Key 分配和数组/Map 分配.
    const std::string key(80, 'r');
    // point 逐个移动失败位置, 每次重新创建未缓存的新版本.
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        // store 默认容量足以保留两个写入批次.
        SyncStore store;
        store.put(key, {1});
        // previous 在查询失败前已经交给读者, 必须始终保持第一版内容.
        const auto previous = store.get_snapshot();
        store.put(key, {2});
        // failed 记录查询是否向调用者传播注入的异常.
        bool failed = false;
        {
            // failure 在离开此作用域后关闭, 后续检查不会误触发下一次分配失败.
            FailureScope failure(point);
            try {
                if (snapshot) {
                    CHECK(store.get_snapshot()->data.at(key)->front() == 2);
                } else {
                    CHECK(store.extract_since(0).deltas.size() == 2);
                }
            } catch (const std::bad_alloc&) {
                failed = true;
            }
        }
        CHECK(store.global_version() == 2);
        CHECK(previous->global_version == 1 && previous->data.at(key)->front() == 1);
        CHECK(store.get_snapshot()->data.at(key)->front() == 2);
        CHECK(store.extract_since(0).deltas.size() == 2);
        if (!failed) {
            CHECK(!injected && failures != 0);
            return failures;
        }
        CHECK(injected);
        ++failures;
    }
    throw std::runtime_error("Read allocation sweep did not reach success");
}

// 返回非零表示不变量或注入覆盖失败, 不把未触发分配失败的空测试记作成功.
int main() {
    try {
        // failures 累计所有修改操作及 deque 位置上的失败数.
        std::size_t failures = 0;
        // operation 遍历所有修改入口, prefix 遍历已选定的 deque 分段位置.
        for (const auto operation : {Operation::insert, Operation::replace, Operation::remove, Operation::expire}) {
            for (const unsigned prefix : {0U, 13U, 29U, 61U}) {
                failures += sweep(operation, prefix);
            }
        }
        // read_failures 另行统计查询分配失败, 不与写入原子性覆盖混淆.
        const auto read_failures = sweep_read(true) + sweep_read(false);
        std::cout << "PASS store atomicity under " << failures << " write and " << read_failures << " read allocation failures\n";
        return 0;
    } catch (const std::exception& error) {
        // error 保留首个失败原因, 非零退出码使 CTest 明确失败.
        std::cerr << error.what() << '\n';
        return 1;
    }
}
