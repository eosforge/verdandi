# Profile.hpp 深度优化：消除隐形的全局热点

经过审查，`profile.hpp` 整体采用了极具现代 C++ 风格的“零成本”抽象：`consteval` 预先算好字符串 Hash、`thread_local` 规避原子槽位竞争、mmap 零分配写入等，性能上限非常高。

但在极度压榨性能的边缘，我发现了一个**极其致命的性能刺客**，它会在配置了 `ASTRA_PROFILE_SECONDS` 时，无情地抹杀掉您的“1/N 采样”带来的 CPU 收益。

### 痛点定位：`active()` 中的高频 `now()` 调用
看看 `Span::Span` 构造函数的入口：
```cpp
auto& output = storage();
if (!output.active(site.group)) // 第一步先检查是否激活
    return;
// ...
if (parent_ ? parent_->id_ == 0 : !select(output.header->sample)) // 最后一步才检查是否被采样！
    return;
```

再看看 `Storage::active()` 的实现：
```cpp
bool active(std::uint64_t group) noexcept {
    if (!header || !(header->groups & group) || stopped_.load(std::memory_order_relaxed))
        return false;
    // 如果设置了采集秒数，这里必然会触发！
    if (stop_ && now() >= stop_) {
        stopped_.store(true, std::memory_order_relaxed);
        return false;
    }
    return true;
}
```

**问题在于**：
`steady_clock::now()` 即使有 Linux vDSO 加持，底层仍然包含 `rdtsc` 和 `lfence`，耗时大约在 15~25 纳秒左右。
由于 `active()` 是所有探针的第一道门槛，这意味着：**即使当前请求属于“未被选中（Unsampled）”的大多数，它依然会因为检查超时，而被迫执行一次昂贵的 `now()`！**
原本一个未被采样的探针只需要进行几次内存读取和位运算（1~2 纳秒），现在却被强行拉平到了 20 纳秒级别。如果探针铺得很密，这会造成可观的纯浪费。

### 优化建议：将超时检测下沉到 `reserve()` 或采用惰性检查

超时停止并不需要精确到绝对的纳秒，稍微延后几十毫秒根本不影响灰度采样的语义。我们完全可以把 `now() >= stop_` 的判断移到真正的**采样后链路**。

最简单的方案是**将检测下沉到 `reserve()` 中**：
```cpp
bool active(std::uint64_t group) noexcept {
    // 纯内存判断，只需 1 纳秒
    return header && (header->groups & group) && !stopped_.load(std::memory_order_relaxed);
}

std::uint64_t reserve() noexcept {
    // 只有真正被选中的探针（比如 1/64），才会走到这里分配槽位
    if (stop_ && now() >= stop_) {
        stopped_.store(true, std::memory_order_relaxed);
        header->missed.fetch_add(1, std::memory_order_relaxed); // 视情况是否算作错过
        return UINT64_MAX;
    }

    struct Block { ... }
    // ... 后续分配逻辑
}
```
**收益**：
未被采样的跨度（Unsampled Spans）彻底摆脱了 `now()` 的纠缠。一旦某个**被采样**的探针触发了超时并置起 `stopped_`，后续所有探针都会在 `active()` 中以 1 纳秒的极速退出。这才是真正的零开销探针！
