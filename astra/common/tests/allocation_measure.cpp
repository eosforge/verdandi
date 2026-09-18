// 仅链接到显式测量构建. 统计全进程替换型 C++ new, 包含静态依赖, 不覆盖直接 malloc 或 placement new.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <unistd.h>

namespace {
constinit std::atomic<unsigned long long> calls{}, bytes{};

// 计数只覆盖成功分配的请求大小. new_handler 语义保留, 不为统计引入额外动态对象.
void* allocate(std::size_t size, std::size_t alignment = 0) {
    for (;;) {
        void* pointer = nullptr;
        if (alignment) {
            if (posix_memalign(&pointer, alignment, size ? size : 1) != 0) {
                pointer = nullptr;
            }
        } else {
            pointer = std::malloc(size ? size : 1);
        }
        if (pointer) {
            calls.fetch_add(1, std::memory_order_relaxed);
            bytes.fetch_add(size, std::memory_order_relaxed);
            return pointer;
        }
        if (auto handler = std::get_new_handler()) {
            handler();
        } else {
            throw std::bad_alloc{};
        }
    }
}

// atexit 输出统计点之前的累计请求, 不把请求字节数解释为仍存活内存或峰值 RSS.
void report() {
    char buffer[192];
    const int length = std::snprintf(buffer, sizeof(buffer), "{\"event\":\"allocation_measure\",\"calls\":%llu,\"requested_bytes\":%llu}\n",
                                     calls.load(std::memory_order_relaxed), bytes.load(std::memory_order_relaxed));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(buffer)) {
        const auto ignored = write(STDERR_FILENO, buffer, static_cast<std::size_t>(length));
        (void)ignored;
    }
}
[[maybe_unused]] const int registered = std::atexit(report);
} // namespace

// 数组和对齐重载共享同一个分配计数点; sized delete 不重复计数.
void* operator new(std::size_t size) {
    return allocate(size);
}
void* operator new[](std::size_t size) {
    return allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}
