// 仅链接到显式测量构建. 统计全进程替换型 C++ new, 包含静态依赖, 不覆盖直接 malloc 或 placement new.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <unistd.h>

namespace {
// calls 统计成功 new 次数, bytes 累计请求字节, 静态初始化为零且不代表存活内存.
constinit std::atomic<unsigned long long> calls{}, bytes{};

// 计数只覆盖成功分配的请求大小. new_handler 语义保留, 不为统计引入额外动态对象.
void* allocate(std::size_t size, std::size_t alignment = 0) {

    for (;;) {
        // pointer 为当前尝试的拥有地址, 分配成功后交给调用方释放.
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

        // handler 为运行库当前的分配失败回调, 保留标准 new 的重试语义.
        if (auto handler = std::get_new_handler()) {
            handler();
        } else {
            throw std::bad_alloc{};
        }
    }
}

// atexit 输出统计点之前的累计请求, 不把请求字节数解释为仍存活内存或峰值 RSS.
void report() {

    // buffer 是固定栈缓冲, 统计输出本身不申请堆内存.
    char buffer[192];
    // length 是 snprintf 所需字节数, 仅完整放入 buffer 时才输出.
    const int length = std::snprintf(buffer, sizeof(buffer), "{\"event\":\"allocation_measure\",\"calls\":%llu,\"requested_bytes\":%llu}\n", calls.load(std::memory_order_relaxed), bytes.load(std::memory_order_relaxed));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(buffer)) {
        // ignored 接收尽力输出的结果; 退出统计不重试或改变进程结果.
        const auto ignored = write(STDERR_FILENO, buffer, static_cast<std::size_t>(length));
        (void)ignored;
    }
}

// registered 在静态初始化时注册一次退出统计, 不创建服务线程.
[[maybe_unused]] const int registered = std::atexit(report);
} // namespace

// 数组和对齐重载共享同一个分配计数点; sized delete 不重复计数.
void* operator new(std::size_t size) {
    return allocate(size);
}

// 全局替换分配入口, size 为请求字节数; 对齐重载将 alignment 传给同一分配器.
void* operator new[](std::size_t size) {
    return allocate(size);
}

// 全局替换分配入口, size 为请求字节数; 对齐重载将 alignment 传给同一分配器.
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}

// 全局替换分配入口, size 为请求字节数; 对齐重载将 alignment 传给同一分配器.
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}

// 全局替换回收入口, 空指针可安全释放; 大小和对齐参数不改变同一分配域的回收方式.
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

// 全局替换回收入口, 空指针可安全释放; 大小和对齐参数不改变同一分配域的回收方式.
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

// 全局替换回收入口, 空指针可安全释放; 大小和对齐参数不改变同一分配域的回收方式.
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

// 全局替换回收入口, 空指针可安全释放; 大小和对齐参数不改变同一分配域的回收方式.
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

// 全局替换回收入口, 空指针可安全释放; 大小和对齐参数不改变同一分配域的回收方式.
void operator delete(void* pointer, std::align_val_t) noexcept {
    std::free(pointer);
}

// 全局替换回收入口, 空指针可安全释放; 大小和对齐参数不改变同一分配域的回收方式.
void operator delete[](void* pointer, std::align_val_t) noexcept {
    std::free(pointer);
}

// 全局替换回收入口, 空指针可安全释放; 大小和对齐参数不改变同一分配域的回收方式.
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}

// 全局替换回收入口, 空指针可安全释放; 大小和对齐参数不改变同一分配域的回收方式.
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}
