// 独立进程的分配故障注入, 不将替换型 new 链接进 Star 或其他测试目标.
#include "almanac.hpp"
#include "check.hpp"

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

// 各项分别覆盖不同的可失败准备路径, 不用仅一次随机失败替代分配点枚举.
enum class Operation {
    // 新 Key 的 Map 节点、共享字符串、页复制与历史准备.
    insert,
    // 已有 Key 的新载荷与旧快照共享路径复制.
    replace,
    // 删除已有 Key 的共享页准备与历史入队.
    erase,
    // 删除不存在的 Key 仍需要权威提交历史.
    absent,
    // 全量候选已就绪后的根/历史交换, 任何准备失败都必须保留旧状态.
    reset
};
} // namespace

// 标量分配转发至受控分配器, size 已由编译器计算.
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
// 初始化一个版本 10 的完整 Scope, 返回与后续写入共享页面的旧视图.
astra::Almanac::View initialize(astra::Almanac& book, bool full) {
    // draft 含 existing 记录; full 时填满首个 64 槽叶页, 用于新 Key 扩树时的分配撤回.
    auto draft = book.prepare(10);
    CHECK(draft.set("existing", {1}));
    if (full) {
        // index 构造另外 63 个不同 Key, 正文都为 1, 旧根校验无须保存第二张期望 Map.
        for (unsigned index = 0; index < 63; ++index) {
            CHECK(draft.set("seed-" + std::to_string(index), {1}));
        }
    }
    CHECK(book.reset(std::move(draft)) == true);
    return *book.view();
}

// 对指定 operation 枚举从输入所有权到提交前历史入队的每个普通分配点.
void atomicity(Operation operation) {

    // completed 确保测试覆盖到首次无注入完成, failures 统计实际失败路径.
    bool completed{};
    unsigned failures{};
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        // 每个点使用全新 book, 避免前次准备留下的容器容量掩盖后续分配.
        astra::Almanac book;
        const auto old = initialize(book, operation == Operation::insert);
        auto draft = book.prepare(20);
        CHECK(draft.set("replacement", {2}));
        try {
            const Failure failure(point);
            switch (operation) {
            case Operation::insert:
                CHECK(book.apply(11, "new", astra::Almanac::Buffer{2}) == true);
                break;
            case Operation::replace:
                CHECK(book.apply(11, "existing", astra::Almanac::Buffer{2}) == true);
                break;
            case Operation::erase:
                CHECK(book.apply(11, "existing", std::nullopt) == true);
                break;
            case Operation::absent:
                CHECK(book.apply(11, "absent", std::nullopt) == true);
                break;
            case Operation::reset:
                CHECK(book.reset(std::move(draft)) == true);
                break;
            }
            completed = true;
        } catch (const std::bad_alloc&) {
            CHECK(injected);
            ++failures;
            CHECK(book.view()->version() == 10 && book.view()->size() == old.size());
            CHECK(*book.find("existing")->value == astra::Almanac::Buffer{1});
            CHECK(book.find("new")->value == nullptr);
            CHECK(book.find("replacement")->value == nullptr);
            CHECK(book.replay(10)->changes.empty());
        }

        // 无论成功或失败, 先前捕获的根都不能被覆盖, 包括 reset 移交原状态的路径.
        old.each([](const std::string& key, const astra::Almanac::Value& value) { CHECK((key == "existing" || key.starts_with("seed-")) && *value == astra::Almanac::Buffer{1}); });
        if (completed) {
            CHECK(book.view()->version() == (operation == Operation::reset ? 20U : 11U));
            break;
        }
    }
    // reset 可以完全不分配, 标准库 deque 的实现选择不改变正确性要求.
    CHECK(completed && (failures != 0 || operation == Operation::reset));
}

// 超预算新历史需要连同旧项一起淘汰, 任何准备失败仍必须保留原数据、版本及完整旧历史.
void eviction() {

    // limits 恰好保留 existing 与单字节值的历史, 双字节新值只超出历史预算, 不超活动容量.
    astra::Almanac::Limits limits;
    limits.backlog = sizeof(astra::Almanac::Change) + 9;
    // completed 要求最终经过无注入提交, failures 确认并非只走未失败的路径.
    bool completed{};
    unsigned failures{};
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        // 每个注入点重新建立旧视图和版本 11 的发送历史; old/saved 分别持有页与历史载荷.
        astra::Almanac book(limits);
        const auto old = initialize(book, false);
        CHECK(book.apply(11, "existing", astra::Almanac::Buffer{1}) == true);
        const auto saved = book.replay(10);
        CHECK(saved && saved->changes.size() == 1);
        try {
            const Failure failure(point);
            CHECK(book.apply(12, "existing", astra::Almanac::Buffer{2, 3}) == true);
            completed = true;
        } catch (const std::bad_alloc&) {
            CHECK(injected);
            ++failures;
            CHECK(book.view()->version() == 11 && book.view()->size() == 1);
            CHECK(*book.find("existing")->value == astra::Almanac::Buffer{1});
            // replay 重新读取真实队列, 不只检查提前固定的 saved, 防止准备失败先行移走旧历史.
            const auto replay = book.replay(10);
            CHECK(replay && replay->version == 11 && replay->changes.size() == 1);
            CHECK(replay->changes.front().version == 11 && *replay->changes.front().value == astra::Almanac::Buffer{1});
        }

        // 无论提交是否完成, 已借出的旧页和回放都仍拥有原值, 淘汰不能将其内容改写或提前释放.
        old.each([](const std::string& key, const astra::Almanac::Value& value) { CHECK(key == "existing" && *value == astra::Almanac::Buffer{1}); });
        CHECK(saved->version == 11 && *saved->changes.front().value == astra::Almanac::Buffer{1});
        if (completed) {
            CHECK(book.view()->version() == 12 && book.view()->size() == 1);
            CHECK(*book.find("existing")->value == (astra::Almanac::Buffer{2, 3}));
            CHECK(book.replay(11) == std::unexpected(astra::Almanac::Error::history));
            CHECK(book.replay(12)->changes.empty());
            // 下次小提交可以重建历史, 但不能把已丢失的版本 12 包装成连续回放.
            CHECK(book.apply(13, "existing", astra::Almanac::Buffer{3}) == true);
            CHECK(book.replay(12)->changes.size() == 1 && book.replay(12)->changes.front().version == 13);
            CHECK(book.replay(11) == std::unexpected(astra::Almanac::Error::history));
            break;
        }
    }
    CHECK(completed && failures != 0);
}

// 私有全量准备失败后仍可完整安装原候选, 不留下仅在 Map 或仅在页索引中的半条记录.
void preparation() {

    bool completed{};
    unsigned failures{};
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        astra::Almanac book;
        auto draft = book.prepare(20);
        CHECK(draft.set("existing", {1}));
        bool failed{};
        try {
            const Failure failure(point);
            CHECK(draft.set("new", {2}));
            completed = true;
        } catch (const std::bad_alloc&) {
            CHECK(injected);
            failed = true;
            ++failures;
        }
        CHECK(book.reset(std::move(draft)) == true);
        CHECK(book.view()->version() == 20);
        CHECK(book.view()->size() == (failed ? 1U : 2U));
        CHECK(*book.find("existing")->value == astra::Almanac::Buffer{1});
        CHECK(static_cast<bool>(book.find("new")->value) == !failed);
        if (completed) {
            break;
        }
    }
    CHECK(completed && failures != 0);
}

// 捕获已有不可变根只转交共享引用, 不因记录数量或首次捕获分配整张 Map.
void capture() {

    astra::Almanac book;
    auto draft = book.prepare(1);
    // index 产生跨多个叶页的紧凑 Key, 防止只用空 Scope 证明无分配.
    for (unsigned index = 0; index < 200; ++index) {
        CHECK(draft.set(std::to_string(index), {1}));
    }
    CHECK(book.reset(std::move(draft)) == true);

    // result 在强制下一次分配失败的条件下捕获, 验证失败不会提交半份状态.
    const Failure failure(0);
    const auto result = book.view();
    CHECK(result && result->version() == 1 && result->size() == 200);
    CHECK(!injected);
}
} // namespace

// 独立故障测试入口, 运行和 Sanitizer 验证均需要用户本轮授权.
int main() {
    try {
        atomicity(Operation::insert);
        atomicity(Operation::replace);
        atomicity(Operation::erase);
        atomicity(Operation::absent);
        atomicity(Operation::reset);
        eviction();
        preparation();
        capture();
        std::cout << "Almanac allocation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
