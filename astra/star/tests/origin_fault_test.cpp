// 独立来源事务故障进程, 替换型 new 不链接进生产 Star 或其他用例.
#include "catalog.hpp"
#include "check.hpp"
#include "origin.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>

namespace {
// 只控制本线程, -1 为关闭, 0 为下一次分配抛错, 正数为失败前允许的分配次数.
thread_local std::ptrdiff_t remaining = -1;
// 本轮是否确实经过注入点, 初始 false, 防止将其他 bad_alloc 误算为覆盖.
thread_local bool injected{};

// 分配 size 字节, 零长度也返回合法可释放地址; 保留标准 new_handler 行为.
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
        // value 是标准 malloc 返回的普通对齐存储, 不用于超对齐对象.
        if (void* value = std::malloc(size ? size : 1)) {
            return value;
        }
        // handler 由标准库持有, 无 handler 时按抛出式 new 的约定报告失败.
        if (auto handler = std::get_new_handler()) {
            handler();
        } else {
            throw std::bad_alloc{};
        }
    }
}

// 回收 allocate 返回的存储, 支持空指针, 不读取已结束寿命的对象.
[[gnu::noinline]] void release(void* value) noexcept {
    std::free(value);
}

// 注入只覆盖被测调用, 析构后所有断言与清理使用正常分配.
class Failure {
public:
    // point 是本次调用允许的成功分配数, 范围 0..127, 每次只注入一次失败.
    explicit Failure(std::ptrdiff_t point) {
        remaining = point;
        injected = false;
    }

    // 正常返回或异常展开均关闭注入, 不干扰外层检查.
    ~Failure() {
        remaining = -1;
    }

    // 注入作用域不得复制, 避免提前关闭另一个作用域的钩子.
    Failure(const Failure&) = delete;
    // 禁止覆盖有效注入作用域.
    Failure& operator=(const Failure&) = delete;
};

} // namespace

// 普通标量分配使用与标准库容器相同的故障计数, 不替换超对齐分配.
void* operator new(std::size_t size) {
    return allocate(size);
}

// 数组分配使用同一注入计数, 保留抛出式 new[] 语义.
void* operator new[](std::size_t size) {
    return allocate(size);
}

// 回收普通标量分配, value 可以为空.
void operator delete(void* value) noexcept {
    release(value);
}

// 回收普通数组分配, value 可以为空.
void operator delete[](void* value) noexcept {
    release(value);
}

// sized delete 与普通 delete 使用相同分配域, 大小由 free 自身处理.
void operator delete(void* value, std::size_t) noexcept {
    release(value);
}

// sized delete[] 同样回收原始分配地址, 不读数组元素.
void operator delete[](void* value, std::size_t) noexcept {
    release(value);
}

namespace {
using namespace std::chrono_literals;
using Source = astra::Origin<astra::Catalog::Record>; // 真实原生来源模板, 不使用假事务容器.

// 水位无正文, 有载荷时按正文实际字节保守计费.
std::size_t measure(const astra::Catalog::Record& record) noexcept {
    return record.value ? record.value->size() : 0;
}

// 每个模式覆盖不同的准备路径, 不在失败后继续发布半份候选.
enum class Operation {
    insert,  // 新 Scope、名称、哈希桶、页和历史.
    replace, // 共享页路径复制与已有条目替换.
    erase,   // 删除前页准备与历史准备.
    absent   // 缺失结束仍占一个完整来源位置.
};

// 来源、页、目录、版本和发送历史在任意单点分配失败后保持旧值.
void atomicity(Operation operation) {

    bool completed{};
    unsigned failures{};
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        const auto gate = std::make_shared<std::mutex>(); // 每次注入独立提交域与容器容量状态.
        Source source(gate, measure, true, {.history = 1});
        const auto now = std::chrono::steady_clock::now();
        const astra::Scope scope{"one", "scope"};
        const astra::Catalog::Record initial{1, std::make_shared<const astra::Catalog::Buffer>(8, 1), astra::Clock::Time(10s)};
        {
            Source::Retired retired;
            const std::lock_guard lock(*gate);
            auto edit = source.prepare(scope, "key", initial, 1, now);
            CHECK(edit);
            retired = edit->commit();
        }
        const auto frozen = [&] { const std::lock_guard lock(*gate); return source.capture(); }(); // 强制覆盖/删除经过 COW 路径.
        const astra::Scope target = operation == Operation::insert ? astra::Scope{"new", "scope"} : scope;
        const std::string key = operation == Operation::absent ? "absent" : "key";
        const std::optional<astra::Catalog::Record> value = operation == Operation::erase || operation == Operation::absent ? std::nullopt : std::optional(initial);
        bool failed{};
        {
            Source::Retired retired; // 先于锁构造, 即使准备/断言异常也在解锁后释放旧内容.
            const std::lock_guard lock(*gate);
            try {
                const Failure failure(point);
                auto edit = source.prepare(target, key, value, 2, now);
                CHECK(edit);
                retired = edit->commit();
                completed = true;
            } catch (const std::bad_alloc&) {
                CHECK(injected);
                failed = true;
                ++failures;
            }
            CHECK(source.position() == (failed ? 1U : 2U));
            const auto history = source.replay(failed ? 0 : 1);
            CHECK(history && history->size() == 1 && history->front().position == source.position());
            if (failed) {
                CHECK(source.capture().size() == 1 && source.find(scope, "key")->value == initial.value);
                CHECK(!source.find({"new", "scope"}, "key"));
                auto retry = source.prepare(target, key, value, 2, now); // 回滚后立即可重试, 不遗留 editing 或新 Scope 容量.
                CHECK(retry);
            }
        }
        frozen.each([&](const auto&, const auto&, const auto& record) { CHECK(record.value == initial.value && record.version == 1); });
        if (completed) {
            break;
        }
    }
    CHECK(completed && failures != 0);
}

// 跨 Scope 合并批次逐分配故障注入, 任何失败后原始目录/计费/水位/位置都保持不变.
void batch() {

    bool completed{};
    unsigned failures{};
    for (std::ptrdiff_t point = 0; point < 256; ++point) {
        const auto gate = std::make_shared<std::mutex>();
        Source source(gate, measure, false);
        const astra::Scope scope{"one", "main"};
        {
            Source::Retired retired;
            const std::lock_guard lock(*gate);
            auto initial = source.prepare(scope, "key", astra::Catalog::Record{1, {}, {}}, 1, std::chrono::steady_clock::now());
            CHECK(initial);
            retired = initial->commit();
        }
        const auto frozen = [&] { const std::lock_guard lock(*gate); return source.capture(); }();
        std::optional<Source::Tree> retired;
        {
            const std::lock_guard lock(*gate);
            try {
                const Failure failure(point);
                auto candidate = source.prepare();
                CHECK(candidate.set(scope, "key", {10, {}, {}}));
                CHECK(candidate.set({"new", "scope"}, "new", {20, {}, {}}));
                retired.emplace(candidate.commit());
                completed = true;
            } catch (const std::bad_alloc&) {
                CHECK(injected);
                ++failures;
                CHECK(source.position() == 1 && source.capture().size() == 1);
                CHECK(source.find(scope, "key")->version == 1 && !source.find({"new", "scope"}, "new"));
                auto retry = source.prepare();
                CHECK(retry.set({"new", "scope"}, "new", {20, {}, {}}));
            }
        }
        frozen.each([](const auto&, const auto&, const auto& record) { CHECK(record.version == 1); });
        if (completed) {
            break;
        }
    }
    CHECK(completed && failures > 0);
}

// 完整来源准备失败只影响私有候选, 已完成的候选项和旧完整根均可继续读取.
void snapshot() {

    bool completed{};
    unsigned failures{};
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        const auto gate = std::make_shared<std::mutex>();
        Source source(gate, measure, false);
        auto draft = source.prepare(100);
        CHECK(draft.set({"one", "scope"}, "key", {1, nullptr, std::nullopt}));
        bool failed{};
        try {
            const Failure failure(point);
            CHECK(draft.set({"two", "scope"}, "key", {2, nullptr, std::nullopt}));
            completed = true;
        } catch (const std::bad_alloc&) {
            CHECK(injected);
            failed = true;
            ++failures;
        }
        {
            std::optional<Source::Replaced> retired;
            const std::lock_guard lock(*gate);
            const Failure failure(0); // 已完整准备后的原子安装及根捕获不能再分配.
            auto installed = source.reset(std::move(draft));
            CHECK(installed);
            retired.emplace(std::move(*installed));
            const auto view = source.capture();
            CHECK(view.position() == 100 && view.size() == (failed ? 1U : 2U));
            CHECK(!injected);
        }
        if (completed) {
            break;
        }
    }
    CHECK(completed && failures != 0);
}
} // namespace

// 全部故障在项目独立可执行体内注入, 构建及实际运行仍需本轮授权.
int main() {

    try {
        atomicity(Operation::insert);
        atomicity(Operation::replace);
        atomicity(Operation::erase);
        atomicity(Operation::absent);
        snapshot();
        batch();
        std::cout << "source allocation cases: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
