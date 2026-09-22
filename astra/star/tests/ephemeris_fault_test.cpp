// 独立 Ephemeris 组合事务故障进程, 替换型 new 不进入生产目标.
#include "check.hpp"
#include "ephemeris_state.hpp"
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
using astra::Clock;
using astra::Ephemeris;
using State = Ephemeris::State;

// 独立注入不同公开写路径, 真实使用来源、COW 投影、历史和侵入式时间轮.
enum class Operation {
    create, // 新范围/UUID/来源行/公开投影/时间轮/钩子完整准备.
    update, // 已冻结来源和内容根的共同路径复制.
    renew,  // 只修改来源/历史/轮, 不复制公开内容根.
    remove, // 来源结束与内容删除的共同准备.
    expire  // tick 内发生分配失败时重排, 下一次仍能完成到期.
};

// 每个分配点独立新建 State, 不让前一个失败留下的容器容量隐藏后续分配路径.
void atomicity(Operation operation) {

    bool completed{};
    unsigned failures{};
    for (std::ptrdiff_t point = 0; point < 128; ++point) {
        auto now = 1s; // 组件注入时间, 只有 expire 模式推进到期边界.
        State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
        const astra::Scope initial{"initial", "scope"};
        const astra::Scope target{"target", "scope"};
        const auto attr = std::make_shared<const Ephemeris::Buffer>(12, 1);
        const auto data = std::make_shared<const Ephemeris::Buffer>(12, 2);
        const auto replacement = std::make_shared<const Ephemeris::Buffer>(16, 3);
        const auto first = state.create(initial, attr, data, 1000);
        CHECK(first);
        const auto source = state.source();       // 强制后续来源编辑经过 COW, 冻结根不被修改.
        const auto view = state.capture(initial); // 更新/删除同样经过独立公开页 COW.
        CHECK(view);
        if (operation == Operation::expire) {
            now = 2s;
        }
        bool failed{};
        try {
            const Failure failure(point);
            switch (operation) {
            case Operation::create:
                CHECK(state.create(target, attr, data, 1000));
                break;
            case Operation::update:
                CHECK(state.update(initial, first->uuid, replacement, 1));
                break;
            case Operation::renew:
                CHECK(state.renew(initial, first->uuid, 1));
                break;
            case Operation::remove:
                CHECK(state.remove(initial, first->uuid));
                break;
            case Operation::expire:
                state.tick();
                break;
            }
            completed = true;
        } catch (const std::bad_alloc&) {
            CHECK(injected);
            failed = true;
            ++failures;
        }

        // 冻结根无论成败都固定原来的同一 Attr/Data/期限, 不读取待检查当前状态时暗中推进它.
        CHECK(source.position() == 1 && source.size() == 1 && view->version() == 1 && view->size() == 1);
        source.each([&](const astra::Scope&, const std::string&, const Ephemeris::Record& record) { CHECK(record.attr == attr && record.data == data && record.deadline == Clock::Time(2s)); });
        view->each([&](const std::string&, const State::Content& content) { CHECK(content.attr == attr && content.data == data); });
        if (operation == Operation::expire) {
            now = 3s; // 原失败钩子被重排到下一拍, 下一轮可以完成, 不永久丢失期限.
            state.tick();
            CHECK(state.source().position() == 2 && state.source().size() == 0);
            CHECK(state.capture(initial)->version() == 2 && state.capture(initial)->size() == 0);
            const auto events = state.events(0);
            CHECK(events && events->size() == 2 && events->back().form == State::Source::Form::erase);
        } else if (failed) {
            CHECK(state.source().position() == 1 && state.source().size() == 1);
            CHECK(state.capture(initial)->version() == 1);
            const auto events = state.events(0);
            CHECK(events && events->size() == 1);
            CHECK(state.find(initial, first->uuid)->record->data == data);
            CHECK(state.capture(target)->version() == 0 && state.capture(target)->size() == 0);
            // 撤销后正常新写仍能成功, 不能残留 editing 标志/悬垂查找项/错误历史字节.
            CHECK(state.update(initial, first->uuid, replacement, 1));
            CHECK(state.source().position() == 2 && state.capture(initial)->version() == 2);
        } else {
            CHECK(state.source().position() == 2);
            CHECK(state.capture(initial)->version() == (operation == Operation::update || operation == Operation::remove ? 2U : 1U));
            const auto events = state.events(0);
            CHECK(events && events->size() == 2);
            if (operation == Operation::create) {
                CHECK(state.capture(target)->version() == 1 && state.source().size() == 2);
            }
        }
        if (completed) {
            break;
        }
    }
    CHECK(completed && failures > 0); // 必须既命中过失败, 又到达完整成功, 不将未覆盖当作通过.
}
} // namespace

// 独立进程只替换普通 new; 不声称覆盖系统随机源失败、对齐分配或网络解码 OOM.
int main() {
    try {
        atomicity(Operation::create);
        atomicity(Operation::update);
        atomicity(Operation::renew);
        atomicity(Operation::remove);
        atomicity(Operation::expire);
        std::cout << "Ephemeris fault tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
