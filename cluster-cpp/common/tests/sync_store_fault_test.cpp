// 功能: 在独立进程逐个注入分配失败, 验证状态, 历史, 快照和版本没有部分提交.
// 此目标仅链接存储模块, 替换型 new 不进入服务或其他测试进程.
#include "check.hpp"
#include "sync_store.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
thread_local std::ptrdiff_t remaining = -1;
thread_local bool injected = false;

// 普通模式保留 new_handler 行为. 注入失败是一次性的, 之后允许异常处理和诊断分配内存.
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
        if (void* value = std::malloc(size ? size : 1)) {
            return value;
        }
        if (auto handler = std::get_new_handler()) {
            handler();
        } else {
            throw std::bad_alloc{};
        }
    }
}

// 保持分配和释放适配器为独立调用, 避免优化器跨 new/free 包装误判测试的分配类型.
[[gnu::noinline]] void release(void* value) noexcept {
    std::free(value);
}

// 只在被测操作期间启用故障. 任何异常路径均关闭注入, 不影响断言和 Store 析构.
class FailureScope {
public:
    explicit FailureScope(std::ptrdiff_t point) {
        remaining = point;
        injected = false;
    }
    ~FailureScope() {
        remaining = -1;
    }
    FailureScope(const FailureScope&) = delete;
    FailureScope& operator=(const FailureScope&) = delete;
};
} // namespace

// 被测容器使用普通对齐. scalar/array 和 sized delete 共用同一对分配器.
void* operator new(std::size_t size) {
    return allocate(size);
}
void* operator new[](std::size_t size) {
    return allocate(size);
}
void operator delete(void* value) noexcept {
    release(value);
}
void operator delete[](void* value) noexcept {
    release(value);
}
void operator delete(void* value, std::size_t) noexcept {
    release(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    release(value);
}

using namespace verdandi::cluster;

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

// 每次从独立 Store 开始. prefix 使 deque 处于不同分段位置, 覆盖历史扩容分配.
std::size_t sweep(Operation operation, unsigned prefix) {
    std::size_t failures = 0;
    const std::string first(80, 'a'), second(80, 'b'), added(80, 'c');
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        SyncStore store(2);
        store.put(first, {1}, Clock::time_point{});
        store.put(second, {1}, Clock::time_point{});
        for (unsigned i = 0; i < prefix; ++i) {
            store.put("seed", {1});
        }
        const auto before = store.get_snapshot();
        bool failed = false;
        {
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
        const auto history = store.extract_since(before->global_version);
        CHECK(!history.require_snapshot && history.deltas.empty());
        CHECK(store.get_snapshot() == before);

        // 再成功提交一个独立标记, 强制重建快照, 防止旧缓存遮蔽版本未变但 Map 已被修改的错误.
        store.put("probe", {3});
        const auto after = store.get_snapshot();
        CHECK(after->global_version == before->global_version + 1);
        CHECK(after->data.size() == before->data.size() + 1);
        for (const auto& [key, payload] : before->data) {
            CHECK(after->data.contains(key));
            CHECK(*after->data.at(key) == *payload);
        }
        const auto delta = store.extract_since(before->global_version);
        CHECK(!delta.require_snapshot && delta.deltas.size() == 1 && delta.deltas.front().key == "probe");
    }
    throw std::runtime_error("Allocation sweep did not reach a successful operation");
}

// 返回非零表示不变量或注入覆盖失败, 不把未触发分配失败的空测试记作成功.
int main() {
    try {
        std::size_t failures = 0;
        for (const auto operation : {Operation::insert, Operation::replace, Operation::remove, Operation::expire}) {
            for (const unsigned prefix : {0U, 13U, 29U, 61U}) {
                failures += sweep(operation, prefix);
            }
        }
        std::cout << "PASS store atomicity under " << failures << " allocation failures\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
