// 功能: 独立替换所有普通/对齐 C++ new, 验证时间轮正常调度, 级联, 回调和销毁期间没有堆分配.
// 只链接此测试进程, 不把分配器带入服务或其他测试. 回调自身不分配, 不测异常诊断的分配.
#include "wheel.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
// 全进程单线程分配计数, 只比较被测区间的差值, 不依赖 C++ 运行库启动行为.
std::size_t allocations{};

// 分配 size 字节, alignment 为 0 时采用普通对齐, 否则使用 POSIX 对齐分配; 保留 new_handler 语义.
[[gnu::noinline]] void* allocate(std::size_t size, std::size_t alignment = 0) {
    for (;;) {
        // address 为本次申请结果, 零长度也需要可释放的独立地址.
        void* address = nullptr;
        if (alignment == 0) {
            address = std::malloc(size == 0 ? 1 : size);
        } else if (posix_memalign(&address, alignment, size == 0 ? 1 : size) != 0) {
            address = nullptr;
        }
        if (address != nullptr) {
            ++allocations;
            return address;
        }
        if (auto handler = std::get_new_handler()) {
            handler();
        } else {
            throw std::bad_alloc{};
        }
    }
}

// 回收替换型 new 的地址. 单独函数避免优化器将 new/free 包装误诊为不匹配.
[[gnu::noinline]] void release(void* address) noexcept {
    std::free(address);
}
} // namespace

// 普通标量分配, 返回满足默认对齐的内存.
void* operator new(std::size_t size) {
    return allocate(size);
}
// 普通数组分配, 与标量共享计数.
void* operator new[](std::size_t size) {
    return allocate(size);
}
// 过对齐标量分配, 不让隐藏的对齐 new 绕过测量.
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}
// 过对齐数组分配, 与标量共享分配域.
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}
// 各 delete 变体使用同一分配域, 大小和对齐参数不改变 POSIX 内存的释放方式.
void operator delete(void* address) noexcept {
    release(address);
}
void operator delete[](void* address) noexcept {
    release(address);
}
void operator delete(void* address, std::size_t) noexcept {
    release(address);
}
void operator delete[](void* address, std::size_t) noexcept {
    release(address);
}
void operator delete(void* address, std::align_val_t) noexcept {
    release(address);
}
void operator delete[](void* address, std::align_val_t) noexcept {
    release(address);
}
void operator delete(void* address, std::size_t, std::align_val_t) noexcept {
    release(address);
}
void operator delete[](void* address, std::size_t, std::align_val_t) noexcept {
    release(address);
}

// 先验证计数器确实捕获 new, 再在栈上运行调度/取消/到期/自改期; 非零退出直接使 CTest 失败.
int main() {
    // baseline/control 用于校准计数器, 防止一个失效的测量器给出假零结果.
    const auto baseline = allocations;
    void* control = ::operator new(1);
    ::operator delete(control);
    if (allocations != baseline + 1) {
        return 1;
    }
    // before 在轮和节点构造前取值, after 在它们析构后取值, 生命周期整体包含在测量范围内.
    const auto before = allocations;
    bool valid = true;
    std::size_t expired = 0;
    {
        astra::Wheel<> wheel;
        std::array<astra::Wheel<>::Node, 1024> nodes;
        for (std::size_t id = 0; id < nodes.size(); ++id) {
            valid &= wheel.schedule(nodes[id], id + 1);
            astra::Wheel<>::cancel(nodes[id]);
            valid &= wheel.schedule(nodes[id], id + 1);
        }
        // 第一次到期时为所有节点改期 1, 验证回调路径也不隐式分配.
        std::array<bool, 1024> rescheduled{};
        for (std::size_t step = 0; step < 2048; ++step) {
            wheel.tick([&](auto* node) noexcept {
                ++expired;
                // id 从同一数组内的稳定节点地址取得, 每个节点只自改期一次.
                const auto id = static_cast<std::size_t>(node - nodes.data());
                if (!rescheduled[id]) {
                    valid &= wheel.schedule(*node, 1);
                    rescheduled[id] = true;
                }
            });
        }
        valid &= expired == 2 * nodes.size();
        for (auto& node : nodes) {
            valid &= !node.scheduled();
            valid &= wheel.schedule(node, 65'536);
        }
    }
    const auto after = allocations;
    std::printf("Wheel: %zu allocations, %zu callbacks, Node=%zu bytes, Wheel=%zu bytes\n", after - before, expired, sizeof(astra::Wheel<>::Node),
                sizeof(astra::Wheel<>));
    return valid && after == before ? 0 : 1;
}
